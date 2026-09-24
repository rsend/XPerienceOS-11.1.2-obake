/* Copyright (C) 2026 The XPerience Project */

#define LOG_TAG "ims_dpl_bootstrapd"

#include "ims_initial_registration.h"

#include <cutils/properties.h>
#include <dlfcn.h>
#include <log/log.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace imscompat {
namespace {

constexpr char kEnableProperty[] = "persist.ims.reg.trigger";
constexpr char kConflictingLifecycleProperty[] =
        "persist.ims.service.lifecycle";
constexpr char kEnabledValue[] = "enabled";
constexpr char kStatusProperty[] = "sys.ims.dpl.reg.status";
constexpr char kSourceCountProperty[] = "sys.ims.dpl.reg.sources";
constexpr char kDestinationCountProperty[] = "sys.ims.dpl.reg.features";
constexpr char kAttemptCountProperty[] = "sys.ims.dpl.reg.attempts";
constexpr char kSuccessCountProperty[] = "sys.ims.dpl.reg.successes";
constexpr char kFailureCountProperty[] = "sys.ims.dpl.reg.failures";
constexpr char kDeregisterCountProperty[] = "sys.ims.dpl.reg.deregisters";

constexpr char kTriggerSymbol[] =
        "_ZN9IMSDevice25IMSDevTriggerRegistrationEv";
constexpr char kDeregisterSymbol[] =
        "_ZN9IMSDevice27IMSDevTriggerDeRegistrationEv";

// Exact SU6-7.3 ABI offsets recovered from the selected unmodified blob.
constexpr size_t kRcsDeviceOffset = 188;
constexpr size_t kDeviceFeatureOffset = 144;
constexpr size_t kDeviceFeatureStride = 24;
constexpr size_t kDeviceConfigOffset = 2276;
constexpr size_t kConfigInitializedOffset = 4064;
constexpr size_t kConfigFeaturePrimaryOffset = 4072;
constexpr size_t kConfigFeatureSecondaryOffset = 4076;
constexpr size_t kConfigFeatureNumericOffset = 4080;
constexpr size_t kConfigFeatureStride = 12;
constexpr size_t kFeatureCount = 14;
constexpr size_t kMaximumFeatureString = 256;
// reconcileInitialRegistration() runs every five seconds in the bootstrap
// daemon. Bound an unavailable object/config dependency to one minute.
constexpr uint32_t kMaximumReadinessPolls = 12;

typedef void (*RegistrationOperationFunction)(void* device);

InitialRegistrationGetRcsObjectFunction gGetRcsObject = nullptr;
RegistrationOperationFunction gTriggerRegistration = nullptr;
RegistrationOperationFunction gTriggerDeregistration = nullptr;
uint32_t gSourceCount = 0;
uint32_t gDestinationCount = 0;
uint32_t gAttemptCount = 0;
uint32_t gSuccessCount = 0;
uint32_t gFailureCount = 0;
uint32_t gDeregisterCount = 0;
uint32_t gReadinessPollCount = 0;
bool gInitialized = false;
bool gAttempted = false;
bool gOwnsRegistration = false;
char gLastStatus[PROP_VALUE_MAX] = {};

void setProperty(const char* name, const char* value) {
    if (property_set(name, value) != 0) {
        ALOGE("cannot set %s=%s", name, value);
    }
}

void setStatus(const char* status) {
    if (strncmp(gLastStatus, status, sizeof(gLastStatus)) == 0) return;
    setProperty(kStatusProperty, status);
    snprintf(gLastStatus, sizeof(gLastStatus), "%s", status);
}

void setUnsignedProperty(const char* name, uint32_t value) {
    char text[16] = {};
    const int length = snprintf(text, sizeof(text), "%u", value);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(text)) {
        ALOGE("cannot format registration counter property=%s", name);
        return;
    }
    setProperty(name, text);
}

void publishCounters() {
    setUnsignedProperty(kSourceCountProperty, gSourceCount);
    setUnsignedProperty(kDestinationCountProperty, gDestinationCount);
    setUnsignedProperty(kAttemptCountProperty, gAttemptCount);
    setUnsignedProperty(kSuccessCountProperty, gSuccessCount);
    setUnsignedProperty(kFailureCountProperty, gFailureCount);
    setUnsignedProperty(kDeregisterCountProperty, gDeregisterCount);
}

bool propertyEnabled(const char* property) {
    char value[PROP_VALUE_MAX] = {};
    property_get(property, value, "disabled");
    return strcmp(value, kEnabledValue) == 0;
}

template <typename T>
T readPointer(const void* object, size_t offset) {
    T value = nullptr;
    memcpy(&value, static_cast<const uint8_t*>(object) + offset,
           sizeof(value));
    return value;
}

uint32_t readUint32(const void* object, size_t offset) {
    uint32_t value = 0;
    memcpy(&value, static_cast<const uint8_t*>(object) + offset,
           sizeof(value));
    return value;
}

bool boundedString(const char* value) {
    return value != nullptr &&
            strnlen(value, kMaximumFeatureString) < kMaximumFeatureString;
}

bool resolveObjects(void** device, void** config) {
    if (gGetRcsObject == nullptr || device == nullptr || config == nullptr) {
        return false;
    }
    void* const rcs = gGetRcsObject();
    if (rcs == nullptr) return false;
    *device = readPointer<void*>(rcs, kRcsDeviceOffset);
    if (*device == nullptr) return false;
    *config = readPointer<void*>(*device, kDeviceConfigOffset);
    return *config != nullptr;
}

// The stock trigger treats the two strings independently, so either may be
// null. Validate every pointer that it will pass to strdup without imposing
// carrier-specific assumptions about which feature fields must be paired.
bool inspectSourceTable(void* config, uint32_t* populated) {
    if (config == nullptr || populated == nullptr ||
            readUint32(config, kConfigInitializedOffset) != 1) {
        return false;
    }
    uint32_t count = 0;
    for (size_t index = 0; index < kFeatureCount; ++index) {
        const size_t slot = index * kConfigFeatureStride;
        const char* const primary = readPointer<const char*>(
                config, kConfigFeaturePrimaryOffset + slot);
        const char* const secondary = readPointer<const char*>(
                config, kConfigFeatureSecondaryOffset + slot);
        if ((primary != nullptr && !boundedString(primary)) ||
                (secondary != nullptr && !boundedString(secondary))) {
            return false;
        }
        if (primary == nullptr && secondary == nullptr) continue;
        ++count;
    }
    *populated = count;
    return count != 0;
}

bool destinationMatchesSource(void* device, void* config,
                              uint32_t* populated) {
    if (device == nullptr || config == nullptr || populated == nullptr) {
        return false;
    }
    uint32_t count = 0;
    for (size_t index = 0; index < kFeatureCount; ++index) {
        const size_t sourceSlot = index * kConfigFeatureStride;
        const size_t destinationSlot = index * kDeviceFeatureStride;
        const char* const sourcePrimary = readPointer<const char*>(
                config, kConfigFeaturePrimaryOffset + sourceSlot);
        const char* const sourceSecondary = readPointer<const char*>(
                config, kConfigFeatureSecondaryOffset + sourceSlot);
        const char* const destinationPrimary = readPointer<const char*>(
                device, kDeviceFeatureOffset + destinationSlot);
        const char* const destinationSecondary = readPointer<const char*>(
                device, kDeviceFeatureOffset + destinationSlot + 4);
        const uint32_t sourceNumeric = readUint32(
                config, kConfigFeatureNumericOffset + sourceSlot);
        const uint32_t destinationNumeric = readUint32(
                device, kDeviceFeatureOffset + destinationSlot + 8);
        const uint32_t destinationState = readUint32(
                device, kDeviceFeatureOffset + destinationSlot + 12);

        const bool primaryMatches = sourcePrimary == nullptr
                ? destinationPrimary == nullptr
                : boundedString(destinationPrimary) &&
                        strcmp(sourcePrimary, destinationPrimary) == 0;
        const bool secondaryMatches = sourceSecondary == nullptr
                ? destinationSecondary == nullptr
                : boundedString(destinationSecondary) &&
                        strcmp(sourceSecondary, destinationSecondary) == 0;
        if (!primaryMatches || !secondaryMatches ||
                sourceNumeric != destinationNumeric ||
                destinationState != 1) {
            return false;
        }
        if (sourcePrimary == nullptr && sourceSecondary == nullptr) continue;
        ++count;
    }
    *populated = count;
    return count != 0;
}

uint32_t countDestinationFeatures(void* device) {
    if (device == nullptr) return 0;
    uint32_t count = 0;
    for (size_t index = 0; index < kFeatureCount; ++index) {
        const char* const primary = readPointer<const char*>(
                device, kDeviceFeatureOffset +
                        index * kDeviceFeatureStride);
        const char* const secondary = readPointer<const char*>(
                device, kDeviceFeatureOffset +
                        index * kDeviceFeatureStride + 4);
        if (primary != nullptr || secondary != nullptr) ++count;
    }
    return count;
}

template <typename T>
bool resolveSymbol(void* library, const char* symbol, T* result) {
    dlerror();
    void* const address = dlsym(library, symbol);
    const char* const error = dlerror();
    if (address == nullptr || error != nullptr) {
        ALOGE("initial registration dependency missing symbol=%s detail=%s",
              symbol, error == nullptr ? "null" : error);
        return false;
    }
    *result = reinterpret_cast<T>(address);
    return true;
}

}  // namespace

bool initializeInitialRegistration(
        void* rcsLibrary,
        InitialRegistrationGetRcsObjectFunction getRcsObject) {
    setStatus("initializing");
    publishCounters();
    gGetRcsObject = getRcsObject;
    if (rcsLibrary == nullptr || getRcsObject == nullptr ||
            !resolveSymbol(rcsLibrary, kTriggerSymbol,
                           &gTriggerRegistration) ||
            !resolveSymbol(rcsLibrary, kDeregisterSymbol,
                           &gTriggerDeregistration)) {
        ++gFailureCount;
        publishCounters();
        setStatus("dependency_error");
        return false;
    }
    gInitialized = true;
    setStatus(isInitialRegistrationEnabled() ? "waiting_config" :
                                               "disabled");
    return true;
}

bool isInitialRegistrationEnabled() {
    return propertyEnabled(kEnableProperty);
}

void reconcileInitialRegistration() {
    if (!isInitialRegistrationEnabled()) {
        shutdownInitialRegistration();
        if (!gOwnsRegistration) setStatus("disabled");
        return;
    }
    if (!gInitialized) {
        setStatus("dependency_error");
        return;
    }
    if (propertyEnabled(kConflictingLifecycleProperty)) {
        setStatus("lifecycle_conflict");
        return;
    }
    if (gOwnsRegistration) {
        setStatus("triggered");
        return;
    }
    if (gAttempted) {
        setStatus("restart_required");
        return;
    }

    void* device = nullptr;
    void* config = nullptr;
    if (!resolveObjects(&device, &config)) {
        ++gReadinessPollCount;
        if (gReadinessPollCount >= kMaximumReadinessPolls) {
            ++gFailureCount;
            gAttempted = true;
            publishCounters();
            setStatus("readiness_timeout");
            ALOGE("initial registration object readiness timed out polls=%u",
                  gReadinessPollCount);
            return;
        }
        setStatus("waiting_objects");
        return;
    }
    if (readUint32(config, kConfigInitializedOffset) != 1) {
        ++gReadinessPollCount;
        if (gReadinessPollCount >= kMaximumReadinessPolls) {
            ++gFailureCount;
            gAttempted = true;
            publishCounters();
            setStatus("readiness_timeout");
            ALOGE("initial registration config readiness timed out polls=%u",
                  gReadinessPollCount);
            return;
        }
        setStatus("waiting_config");
        return;
    }
    uint32_t sources = 0;
    if (!inspectSourceTable(config, &sources)) {
        ++gFailureCount;
        gAttempted = true;
        publishCounters();
        setStatus("invalid_config");
        ALOGE("initial registration rejected malformed feature configuration");
        return;
    }
    gSourceCount = sources;
    gDestinationCount = countDestinationFeatures(device);
    publishCounters();
    if (gDestinationCount != 0) {
        uint32_t matched = 0;
        if (destinationMatchesSource(device, config, &matched) &&
                matched == sources) {
            gDestinationCount = matched;
            publishCounters();
            setStatus("external_active");
        } else {
            ++gFailureCount;
            gAttempted = true;
            publishCounters();
            setStatus("unexpected_state");
            ALOGE("initial registration found a non-empty mismatched feature table");
        }
        return;
    }

    gAttempted = true;
    ++gAttemptCount;
    publishCounters();
    gTriggerRegistration(device);

    uint32_t populated = 0;
    if (!destinationMatchesSource(device, config, &populated) ||
            populated != sources) {
        gDestinationCount = countDestinationFeatures(device);
        ++gFailureCount;
        publishCounters();
        setStatus("postcondition_failed");
        ALOGE("initial registration feature copy failed sources=%u features=%u",
              sources, gDestinationCount);
        // The proprietary trigger may have constructed a partial aggregate.
        // Pair the failed attempt before refusing any retry in this process.
        gTriggerDeregistration(device);
        ++gDeregisterCount;
        publishCounters();
        return;
    }

    gDestinationCount = populated;
    gOwnsRegistration = true;
    ++gSuccessCount;
    publishCounters();
    setStatus("triggered");
}

void shutdownInitialRegistration() {
    if (!gInitialized || !gOwnsRegistration) return;
    void* device = nullptr;
    void* config = nullptr;
    if (!resolveObjects(&device, &config)) {
        ++gFailureCount;
        publishCounters();
        setStatus("deregister_object_error");
        ALOGE("cannot resolve stock object for initial deregistration");
        return;
    }
    (void)config;
    gTriggerDeregistration(device);
    gOwnsRegistration = false;
    ++gDeregisterCount;
    publishCounters();
    setStatus("deregistered");
}

}  // namespace imscompat
