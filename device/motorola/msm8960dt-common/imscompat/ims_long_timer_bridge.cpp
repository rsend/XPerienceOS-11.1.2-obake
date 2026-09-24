/* Copyright (C) 2026 The XPerience Project */

#define LOG_TAG "ims_dpl_bootstrapd"

#include "ims_long_timer_bridge.h"

#include <cutils/properties.h>
#include <dlfcn.h>
#include <log/log.h>

#include <string.h>

namespace imscompat {
namespace {

constexpr char kEnableProperty[] = "persist.ims.dpl.timer_bridge";
constexpr char kEnabledValue[] = "enabled";
constexpr char kStatusProperty[] = "sys.ims.dpl.timer_bridge.status";
constexpr char kDplLibrary[] = "lib-imsdpl.so";
constexpr char kStartSlotSymbol[] = "m_RCSStartTimer";
constexpr char kStopSlotSymbol[] = "m_RCSStopTimer";
constexpr char kStartFunctionSymbol[] = "RcsStartTimer";
constexpr char kStopFunctionSymbol[] = "RcsStopTimer";

void setStatus(const char* value) {
    if (property_set(kStatusProperty, value) != 0) {
        ALOGE("cannot set %s=%s", kStatusProperty, value);
    }
}

void* resolve(void* library, const char* symbol) {
    dlerror();
    void* const value = dlsym(library, symbol);
    const char* const error = dlerror();
    if (error != nullptr || value == nullptr) {
        ALOGE("long-timer dependency missing symbol=%s detail=%s", symbol,
              error == nullptr ? "null result" : error);
        return nullptr;
    }
    return value;
}

void* readSlot(void* slot) {
    void* value = nullptr;
    memcpy(&value, slot, sizeof(value));
    return value;
}

void writeSlot(void* slot, void* value) {
    memcpy(slot, &value, sizeof(value));
}

}  // namespace

bool isLongTimerBridgeEnabled() {
    char value[PROP_VALUE_MAX] = {};
    property_get(kEnableProperty, value, "disabled");
    return strcmp(value, kEnabledValue) == 0;
}

bool initializeLongTimerBridge(void* rcsLibrary) {
    if (!isLongTimerBridgeEnabled()) {
        setStatus("disabled");
        return true;
    }
    if (rcsLibrary == nullptr) {
        setStatus("dependency_error");
        ALOGE("long-timer bridge received a null RCS library handle");
        return false;
    }

    void* const dplLibrary = dlopen(kDplLibrary, RTLD_NOW | RTLD_LOCAL);
    if (dplLibrary == nullptr) {
        setStatus("dependency_error");
        ALOGE("long-timer bridge cannot open %s detail=%s", kDplLibrary,
              dlerror());
        return false;
    }

    void* const startSlot = resolve(dplLibrary, kStartSlotSymbol);
    void* const stopSlot = resolve(dplLibrary, kStopSlotSymbol);
    void* const startFunction = resolve(rcsLibrary, kStartFunctionSymbol);
    void* const stopFunction = resolve(rcsLibrary, kStopFunctionSymbol);
    if (startSlot == nullptr || stopSlot == nullptr ||
            startFunction == nullptr || stopFunction == nullptr) {
        setStatus("dependency_error");
        dlclose(dplLibrary);
        return false;
    }

    const void* const currentStart = readSlot(startSlot);
    const void* const currentStop = readSlot(stopSlot);
    if ((currentStart != nullptr && currentStart != startFunction) ||
            (currentStop != nullptr && currentStop != stopFunction)) {
        setStatus("unexpected_state");
        ALOGE("long-timer callback slots contain foreign values start=%p "
              "stop=%p expected_start=%p expected_stop=%p",
              currentStart, currentStop, startFunction, stopFunction);
        dlclose(dplLibrary);
        return false;
    }

    // A visible start callback must always have its matching stop callback.
    // Word-sized aligned stores are atomic on this ARM target. Publish stop
    // first and use a full barrier before making start callable.
    writeSlot(stopSlot, stopFunction);
    __sync_synchronize();
    writeSlot(startSlot, startFunction);
    __sync_synchronize();

    const bool valid = readSlot(startSlot) == startFunction &&
            readSlot(stopSlot) == stopFunction;
    dlclose(dplLibrary);
    if (!valid) {
        setStatus("postcondition_failed");
        ALOGE("long-timer callback slot postcondition failed");
        return false;
    }

    setStatus("active");
    return true;
}

}  // namespace imscompat
