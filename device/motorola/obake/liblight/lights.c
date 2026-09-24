/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


// #define LOG_NDEBUG 0
#define LOG_TAG "lights"

#include <cutils/log.h>

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>

#include <sys/ioctl.h>
#include <sys/types.h>

#include <hardware/lights.h>

/******************************************************************************/

static pthread_once_t g_init = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_lcd_brightness = 0;
static int g_button_on = 0;
static struct light_state_t g_notification;
static struct light_state_t g_battery;
static struct light_state_t g_attention;

char const*const LCD_FILE
        = "/sys/class/backlight/lcd-backlight/brightness";

char const*const BUTTON_FILE
        = "/sys/class/leds/button-backlight/brightness";

char const*const RED_LED_FILE
        = "/sys/class/leds/red/brightness";

char const*const GREEN_LED_FILE
        = "/sys/class/leds/green/brightness";

char const*const BLUE_LED_FILE
        = "/sys/class/leds/blue/brightness";

char const*const RGB_BLINK_FILE
        = "/sys/class/leds/red/blink";

/**
 * device methods
 */

void init_globals(void)
{
    // init the mutex
    pthread_mutex_init(&g_lock, NULL);
    g_lcd_brightness = -1;
    g_button_on = -1;
}

static int
write_value(char const* path, char const* value)
{
    int fd;
    int saved_errno;
    size_t length = strlen(value);
    ssize_t written;

    fd = open(path, O_WRONLY);
    if (fd < 0) {
        saved_errno = errno;
        ALOGE("Failed to open %s: %s", path, strerror(saved_errno));
        return -saved_errno;
    }

    written = write(fd, value, length);
    if (written < 0) {
        saved_errno = errno;
        ALOGE("Failed to write %s: %s", path, strerror(saved_errno));
        close(fd);
        return -saved_errno;
    }
    if ((size_t)written != length) {
        ALOGE("Short write to %s: %zd of %zu bytes", path, written, length);
        close(fd);
        return -EIO;
    }
    if (close(fd) < 0) {
        saved_errno = errno;
        ALOGE("Failed to close %s after write: %s", path, strerror(saved_errno));
        return -saved_errno;
    }

    return 0;
}

static int
write_int(char const* path, int value)
{
    char buffer[20];

    snprintf(buffer, sizeof(buffer), "%d\n", value);
    return write_value(path, buffer);
}

static int
write_blink(int on_ms, int off_ms)
{
    char buffer[64];

    snprintf(buffer, sizeof(buffer), "%d %d 0 0\n", on_ms, off_ms);
    return write_value(RGB_BLINK_FILE, buffer);
}

static int
keep_first_error(int result, int candidate)
{
    return result != 0 ? result : candidate;
}

static int
is_lit(struct light_state_t const* state)
{
    return state->color & 0x00ffffff;
}

static int
rgb_to_brightness(struct light_state_t const* state)
{
    int color = state->color & 0x00ffffff;
    return ((77*((color>>16)&0x00ff))
            + (150*((color>>8)&0x00ff)) + (29*(color&0x00ff))) >> 8;
}

static int
set_light_backlight(struct light_device_t* dev,
        struct light_state_t const* state)
{
    int err = 0;
    int brightness = rgb_to_brightness(state);
    (void)dev;

    pthread_mutex_lock(&g_lock);
    if (g_lcd_brightness < 0 ||
        (g_lcd_brightness != brightness))
    {
            // Hack - maximum is only 127
            err = write_int(LCD_FILE, brightness >> 1);
            if (!err && g_button_on > 0)
                    err = write_int(BUTTON_FILE, brightness);
    }

    g_lcd_brightness = brightness;
    pthread_mutex_unlock(&g_lock);
    return err;
}

static int
set_light_buttons(struct light_device_t* dev,
        struct light_state_t const* state)
{
    int err = 0;
    int on = rgb_to_brightness(state) > 0;
    (void)dev;


    pthread_mutex_lock(&g_lock);

    if (g_button_on < 0 ||
        (!g_button_on && on > 0) ||
        (g_button_on > 0 && !on))
    {
        err = write_int(BUTTON_FILE, on ? g_lcd_brightness : 0);
    }

    g_button_on = on;
    pthread_mutex_unlock(&g_lock);
    return err;
}

static int
set_rgb_light_locked(struct light_state_t const* state)
{
    unsigned int color = state->color;
    int red = (color >> 16) & 0xff;
    int green = (color >> 8) & 0xff;
    int blue = color & 0xff;
    int on_ms = 0;
    int off_ms = 0;
    int err = 0;

    if (state->flashMode == LIGHT_FLASH_TIMED
            && state->flashOnMS > 0 && state->flashOffMS > 0) {
        on_ms = state->flashOnMS;
        off_ms = state->flashOffMS;
    }

    /* The PMIC exposes one blink engine shared by all three RGB channels. */
    err = keep_first_error(err, write_blink(0, 0));
    err = keep_first_error(err, write_int(RED_LED_FILE, red));
    err = keep_first_error(err, write_int(GREEN_LED_FILE, green));
    err = keep_first_error(err, write_int(BLUE_LED_FILE, blue));

    /* Leave a steady color on if any channel write failed. */
    if (err == 0 && (red || green || blue) && on_ms > 0 && off_ms > 0)
        err = write_blink(on_ms, off_ms);

    return err;
}

static int
update_rgb_light_locked(void)
{
    if (is_lit(&g_attention))
        return set_rgb_light_locked(&g_attention);
    if (is_lit(&g_battery))
        return set_rgb_light_locked(&g_battery);
    return set_rgb_light_locked(&g_notification);
}

static int
set_light_battery(struct light_device_t* dev,
        struct light_state_t const* state)
{
    int err;
    (void)dev;

    pthread_mutex_lock(&g_lock);
    g_battery = *state;
    err = update_rgb_light_locked();
    pthread_mutex_unlock(&g_lock);
    return err;
}

static int
set_light_notifications(struct light_device_t* dev,
        struct light_state_t const* state)
{
    int err;
    (void)dev;

    pthread_mutex_lock(&g_lock);
    g_notification = *state;
    err = update_rgb_light_locked();
    pthread_mutex_unlock(&g_lock);
    return err;
}

static int
set_light_attention(struct light_device_t* dev,
        struct light_state_t const* state)
{
    int err;
    (void)dev;

    pthread_mutex_lock(&g_lock);
    g_attention = *state;
    err = update_rgb_light_locked();
    pthread_mutex_unlock(&g_lock);
    return err;
}

/** Close the lights device */
static int
close_lights(struct light_device_t *dev)
{
    if (dev) {
        free(dev);
    }
    return 0;
}


/******************************************************************************/

/**
 * module methods
 */

/** Open a new instance of a lights device using name */
static int open_lights(const struct hw_module_t* module, char const* name,
        struct hw_device_t** device)
{
    int (*set_light)(struct light_device_t* dev,
            struct light_state_t const* state);

    if (0 == strcmp(LIGHT_ID_BACKLIGHT, name))
        set_light = set_light_backlight;
    else if (0 == strcmp(LIGHT_ID_BUTTONS, name))
        set_light = set_light_buttons;
    else if (0 == strcmp(LIGHT_ID_BATTERY, name))
        set_light = set_light_battery;
    else if (0 == strcmp(LIGHT_ID_NOTIFICATIONS, name))
        set_light = set_light_notifications;
    else if (0 == strcmp(LIGHT_ID_ATTENTION, name))
        set_light = set_light_attention;
    else
        return -EINVAL;

    pthread_once(&g_init, init_globals);

    struct light_device_t *dev = malloc(sizeof(struct light_device_t));
    if (dev == NULL)
        return -ENOMEM;

    memset(dev, 0, sizeof(*dev));

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (struct hw_module_t*)module;
    dev->common.close = (int (*)(struct hw_device_t*))close_lights;
    dev->set_light = set_light;

    *device = (struct hw_device_t*)dev;
    return 0;
}

static struct hw_module_methods_t lights_module_methods = {
    .open =  open_lights,
};

/*
 * The lights Module
 */
struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .version_major = 1,
    .version_minor = 0,
    .id = LIGHTS_HARDWARE_MODULE_ID,
    .name = "Obake Lights Module",
    .author = "razrqcom-dev-team, Google, Inc.",
    .methods = &lights_module_methods,
};
