/*
 * Compatibility adapter for Motorola's legacy libmot_sensorlistener.so.
 *
 * The proprietary library allocates a 36-byte, pre-Lollipop SensorManager
 * object and calls the old no-argument API.  A Nougat SensorManager cannot be
 * constructed in that storage because its String16 member starts at offset
 * 36.  Keep the legacy object as an opaque wrapper around Nougat's real
 * SensorManager instead.
 */

#define LOG_TAG "LegacySensorManagerShim"

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>

#include <utils/Mutex.h>
#include <gui/Sensor.h>
#include <gui/SensorEventQueue.h>
#include <utils/Log.h>
#include <utils/Singleton.h>
#include <utils/String16.h>
#include <utils/String8.h>

namespace android {

namespace {

using GetInstanceForPackageFn = void* (*)(const String16& package_name);
using GetDefaultSensorFn = const Sensor* (*)(void* manager, int type);
using CreateEventQueueFn = sp<SensorEventQueue> (*)(
        void* manager, String8 package_name, int mode);

struct ModernSensorManagerApi {
    void* handle;
    GetInstanceForPackageFn get_instance_for_package;
    GetDefaultSensorFn get_default_sensor;
    CreateEventQueueFn create_event_queue;
};

ModernSensorManagerApi g_api = {};
pthread_once_t g_api_once = PTHREAD_ONCE_INIT;

void* resolveLibguiSymbol(void* handle, const char* symbol) {
    dlerror();
    void* address = dlsym(handle, symbol);
    const char* error = dlerror();
    if (error != nullptr) {
        ALOGE("dependency=libgui.so failure_class=symbol_resolution "
              "symbol=%s error=%s", symbol, error);
        return nullptr;
    }
    return address;
}

void initializeModernApi() {
    g_api.handle = dlopen("libgui.so", RTLD_NOW | RTLD_LOCAL);
    if (g_api.handle == nullptr) {
        const char* error = dlerror();
        ALOGE("dependency=libgui.so failure_class=dlopen error=%s",
              error != nullptr ? error : "unknown");
        return;
    }

    g_api.get_instance_for_package =
            reinterpret_cast<GetInstanceForPackageFn>(resolveLibguiSymbol(
                    g_api.handle,
                    "_ZN7android13SensorManager21getInstanceForPackageERKNS_8String16E"));
    g_api.get_default_sensor =
            reinterpret_cast<GetDefaultSensorFn>(resolveLibguiSymbol(
                    g_api.handle,
                    "_ZN7android13SensorManager16getDefaultSensorEi"));
    g_api.create_event_queue =
            reinterpret_cast<CreateEventQueueFn>(resolveLibguiSymbol(
                    g_api.handle,
                    "_ZN7android13SensorManager16createEventQueueENS_7String8Ei"));
}

bool modernApiAvailable() {
    pthread_once(&g_api_once, initializeModernApi);
    return g_api.get_instance_for_package != nullptr &&
            g_api.get_default_sensor != nullptr &&
            g_api.create_event_queue != nullptr;
}

}  // namespace

class SensorManager : public Singleton<SensorManager> {
public:
    SensorManager();

    const Sensor* getDefaultSensor(int type);
    sp<SensorEventQueue> createEventQueue();

private:
    void* modern_manager_;
    uint8_t reserved_[32];
};

static_assert(sizeof(SensorManager) == 36,
              "Legacy Motorola SensorManager ABI must remain 36 bytes");

ANDROID_SINGLETON_STATIC_INSTANCE(SensorManager)

SensorManager::SensorManager() : modern_manager_(nullptr), reserved_{} {
    if (!modernApiAvailable()) {
        return;
    }

    modern_manager_ = g_api.get_instance_for_package(String16());
    if (modern_manager_ == nullptr) {
        ALOGE("dependency=sensorservice failure_class=initialization "
              "operation=getInstanceForPackage");
    }
}

const Sensor* SensorManager::getDefaultSensor(int type) {
    if (modern_manager_ == nullptr || !modernApiAvailable()) {
        return nullptr;
    }
    return g_api.get_default_sensor(modern_manager_, type);
}

sp<SensorEventQueue> SensorManager::createEventQueue() {
    if (modern_manager_ == nullptr || !modernApiAvailable()) {
        return nullptr;
    }
    return g_api.create_event_queue(modern_manager_, String8(), 0);
}

}  // namespace android
