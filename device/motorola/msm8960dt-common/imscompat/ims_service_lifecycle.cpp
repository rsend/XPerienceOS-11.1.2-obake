/* Copyright (C) 2026 The XPerience Project */

#define LOG_TAG "ims_dpl_bootstrapd"

#include "ims_service_lifecycle.h"

#include "ims_service_receiver.h"

#include <cutils/properties.h>
#include <dlfcn.h>
#include <log/log.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace imscompat {
namespace {

constexpr char kMonitorLibrary[] =
        "/system/lib/radio-su6-73/lib-imsqimf.so";
constexpr char kAddSymbol[] =
        "_ZN22RegisterServiceMonitor10AddServiceEPct";
constexpr char kRemoveSymbol[] =
        "_ZN22RegisterServiceMonitor13RemoveServiceEPct";
constexpr char kCheckRegistrationSymbol[] =
        "_ZN22RegisterServiceMonitor23CheckRegistrationStatusEv";
constexpr char kSubscribeRegistrationSymbol[] =
        "_ZN22RegisterServiceMonitor27SubscribeRegistrationStatusEm";
constexpr char kUnsubscribeRegistrationSymbol[] =
        "_ZN22RegisterServiceMonitor29UnsubscribeRegistrationStatusEv";
constexpr char kEnableProperty[] = "persist.ims.service.lifecycle";
constexpr char kStatusProperty[] = "sys.ims.dpl.lifecycle.status";
constexpr char kTagProperty[] = "sys.ims.dpl.lifecycle.tag";
constexpr char kAddCountProperty[] = "sys.ims.dpl.lifecycle.adds";
constexpr char kRemoveCountProperty[] = "sys.ims.dpl.lifecycle.removes";
constexpr char kFailureCountProperty[] = "sys.ims.dpl.lifecycle.failures";
constexpr char kSequenceProperty[] = "sys.ims.dpl.lifecycle.sequence";
constexpr char kPowerQueryCountProperty[] =
        "sys.ims.dpl.lifecycle.pqueries";
constexpr char kPowerFailureCountProperty[] =
        "sys.ims.dpl.lifecycle.pfailures";
constexpr char kEnabledValue[] = "enabled";

static_assert(sizeof(kEnableProperty) <= PROPERTY_KEY_MAX, "property key too long");
static_assert(sizeof(kStatusProperty) <= PROPERTY_KEY_MAX, "property key too long");
static_assert(sizeof(kTagProperty) <= PROPERTY_KEY_MAX, "property key too long");
static_assert(sizeof(kAddCountProperty) <= PROPERTY_KEY_MAX, "property key too long");
static_assert(sizeof(kRemoveCountProperty) <= PROPERTY_KEY_MAX, "property key too long");
static_assert(sizeof(kFailureCountProperty) <= PROPERTY_KEY_MAX, "property key too long");
static_assert(sizeof(kSequenceProperty) <= PROPERTY_KEY_MAX, "property key too long");
static_assert(sizeof(kPowerQueryCountProperty) <= PROPERTY_KEY_MAX,
        "property key too long");
static_assert(sizeof(kPowerFailureCountProperty) <= PROPERTY_KEY_MAX,
        "property key too long");

// SU6-7.3 ABI offsets, recovered from the selected unmodified stock blob.
constexpr size_t kRcsDeviceOffset = 188;
constexpr size_t kEmbeddedMonitorOffset = 4;
constexpr size_t kMonitorRegistrationRequestOffset = 44;
constexpr size_t kDeviceOperatingModeOffset = 108;
constexpr size_t kDeviceConfigOffset = 2276;
constexpr size_t kDevicePdpConnectedOffset = 2320;
constexpr size_t kConfigInitializedOffset = 4064;
constexpr size_t kConfigFeaturePrimaryOffset = 4072;
constexpr size_t kConfigFeatureSecondaryOffset = 4076;
constexpr size_t kConfigFeatureStride = 12;
constexpr size_t kConfigFeatureCount = 14;

constexpr char kIcsiKey[] = "+g.3gpp.icsi-ref";
constexpr char kMmtelUrn[] =
        "urn:urn-7:3gpp-service.ims.icsi.mmtel";
constexpr uint32_t kLteVoiceMask = 1u << 0;
constexpr uint32_t kIwlanVoiceMask = 1u << 4;
constexpr uint32_t kSupportedMask = kLteVoiceMask | kIwlanVoiceMask;
constexpr size_t kMaximumConfigString = 256;
constexpr size_t kMaximumServiceString = 512;
constexpr unsigned kMaximumRetryDelaySeconds = 30;
constexpr uint32_t kOnlineOperatingMode = 5;
constexpr uint32_t kMaximumBearerSends = 6;
constexpr uint32_t kRegistrationRequestIdle = 0;
constexpr uint32_t kRegistrationRequestQueryPending = 1;
constexpr uint32_t kRegistrationRequestSubscribePending = 2;
constexpr uint32_t kRegistrationRequestSubscribed = 3;
constexpr unsigned kPowerQueryTimeoutSeconds = 5;

typedef int (*AddServiceFunction)(void* monitor, char* service,
                                  unsigned short length);
typedef int (*RemoveServiceFunction)(void* monitor, char* service,
                                     unsigned short length);
typedef int (*MonitorActionFunction)(void* monitor);
typedef int (*SubscribeRegistrationFunction)(void* monitor,
                                             unsigned long timeout);

enum class PowerRecoveryState {
    kNotStarted,
    kQueryPending,
    kResubscribePending,
    kComplete,
    kFailed,
};

GetRcsObjectFunction gGetRcsObject = nullptr;
AddServiceFunction gAddService = nullptr;
RemoveServiceFunction gRemoveService = nullptr;
MonitorActionFunction gCheckRegistrationStatus = nullptr;
MonitorActionFunction gUnsubscribeRegistrationStatus = nullptr;
SubscribeRegistrationFunction gSubscribeRegistrationStatus = nullptr;
void* gMonitorLibrary = nullptr;
char gActiveService[kMaximumServiceString] = {};
uint32_t gAddCount = 0;
uint32_t gRemoveCount = 0;
uint32_t gFailureCount = 0;
uint32_t gLastSequence = 0;
uint32_t gLastOperatingMode = UINT32_MAX;
uint32_t gBearerSendCount = 0;
uint32_t gPowerQueryCount = 0;
uint32_t gPowerFailureCount = 0;
unsigned gRetryDelaySeconds = 1;
time_t gNextAttempt = 0;
time_t gPowerQueryDeadline = 0;
PowerRecoveryState gPowerRecoveryState = PowerRecoveryState::kNotStarted;
bool gInitialized = false;

void setProperty(const char* name, const char* value) {
    if (property_set(name, value) != 0) {
        ALOGE("cannot set %s=%s", name, value);
    }
}

void setUnsignedProperty(const char* name, uint32_t value) {
    char text[16] = {};
    const int length = snprintf(text, sizeof(text), "%u", value);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(text)) {
        ALOGE("cannot format lifecycle counter property=%s", name);
        return;
    }
    setProperty(name, text);
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
            strnlen(value, kMaximumConfigString) < kMaximumConfigString;
}

void publishCounters() {
    setUnsignedProperty(kAddCountProperty, gAddCount);
    setUnsignedProperty(kRemoveCountProperty, gRemoveCount);
    setUnsignedProperty(kFailureCountProperty, gFailureCount);
    setUnsignedProperty(kSequenceProperty, gLastSequence);
    setUnsignedProperty(kPowerQueryCountProperty, gPowerQueryCount);
    setUnsignedProperty(kPowerFailureCountProperty, gPowerFailureCount);
}

bool lifecycleEnabled() {
    char value[PROP_VALUE_MAX] = {};
    property_get(kEnableProperty, value, "disabled");
    return strcmp(value, kEnabledValue) == 0;
}

void scheduleRetry() {
    const time_t now = time(nullptr);
    gNextAttempt = now == static_cast<time_t>(-1)
            ? 0 : now + gRetryDelaySeconds;
    if (gRetryDelaySeconds < kMaximumRetryDelaySeconds) {
        const unsigned doubled = gRetryDelaySeconds * 2;
        gRetryDelaySeconds = doubled > kMaximumRetryDelaySeconds
                ? kMaximumRetryDelaySeconds : doubled;
    }
}

void resetRetry() {
    gRetryDelaySeconds = 1;
    gNextAttempt = 0;
}

bool retryDue() {
    if (gNextAttempt == 0) return true;
    const time_t now = time(nullptr);
    return now == static_cast<time_t>(-1) || now >= gNextAttempt;
}

void schedulePowerPoll() {
    const time_t now = time(nullptr);
    gNextAttempt = now == static_cast<time_t>(-1) ? 0 : now + 1;
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

bool constructVoiceService(void* config, char* service, size_t capacity) {
    if (config == nullptr || service == nullptr || capacity == 0 ||
            readUint32(config, kConfigInitializedOffset) != 1) {
        return false;
    }
    for (size_t index = 0; index < kConfigFeatureCount; ++index) {
        const size_t slot = index * kConfigFeatureStride;
        const char* const primary = readPointer<const char*>(
                config, kConfigFeaturePrimaryOffset + slot);
        const char* const secondary = readPointer<const char*>(
                config, kConfigFeatureSecondaryOffset + slot);
        if (!boundedString(primary) || !boundedString(secondary) ||
                strcmp(primary, kIcsiKey) != 0 ||
                strcmp(secondary, kMmtelUrn) != 0) {
            continue;
        }
        const int length = snprintf(service, capacity, "%s=\"%s\"",
                                    primary, secondary);
        return length > 0 && static_cast<size_t>(length) < capacity &&
                static_cast<size_t>(length) <=
                        static_cast<size_t>(USHRT_MAX);
    }
    return false;
}

uint32_t registrationRequestState(void* monitor) {
    return readUint32(monitor, kMonitorRegistrationRequestOffset);
}

bool beginRegistrationPowerQuery(void* monitor) {
    if (registrationRequestState(monitor) !=
            kRegistrationRequestSubscribed) {
        setProperty(kStatusProperty, "power_subscription_not_ready");
        schedulePowerPoll();
        return false;
    }
    if (gUnsubscribeRegistrationStatus(monitor) != 1 ||
            registrationRequestState(monitor) !=
                    kRegistrationRequestIdle) {
        ++gPowerFailureCount;
        publishCounters();
        setProperty(kStatusProperty, "power_unsubscribe_failed");
        ALOGE("cannot prepare late registration-status power query");
        gPowerRecoveryState = PowerRecoveryState::kFailed;
        return false;
    }
    if (gCheckRegistrationStatus(monitor) != 1 ||
            registrationRequestState(monitor) !=
                    kRegistrationRequestQueryPending) {
        ++gPowerFailureCount;
        publishCounters();
        setProperty(kStatusProperty, "power_query_send_failed");
        ALOGE("cannot send late registration-status power query");
        if (registrationRequestState(monitor) == kRegistrationRequestIdle &&
                gSubscribeRegistrationStatus(monitor, 0) == 1) {
            gPowerRecoveryState = PowerRecoveryState::kResubscribePending;
            schedulePowerPoll();
        } else {
            gPowerRecoveryState = PowerRecoveryState::kFailed;
        }
        return false;
    }
    ++gPowerQueryCount;
    publishCounters();
    const time_t now = time(nullptr);
    gPowerQueryDeadline = now == static_cast<time_t>(-1)
            ? 0 : now + kPowerQueryTimeoutSeconds;
    gPowerRecoveryState = PowerRecoveryState::kQueryPending;
    setProperty(kStatusProperty, "power_query_pending");
    schedulePowerPoll();
    return false;
}

bool beginRegistrationResubscribe(void* monitor, const char* status) {
    if (registrationRequestState(monitor) != kRegistrationRequestIdle ||
            gSubscribeRegistrationStatus(monitor, 0) != 1) {
        ++gPowerFailureCount;
        publishCounters();
        setProperty(kStatusProperty, "power_resubscribe_failed");
        ALOGE("cannot restore registration-status subscription");
        gPowerRecoveryState = PowerRecoveryState::kFailed;
        return false;
    }
    gPowerRecoveryState = PowerRecoveryState::kResubscribePending;
    setProperty(kStatusProperty, status);
    schedulePowerPoll();
    return false;
}

bool advancePowerRecovery(void* device, uint32_t operatingMode) {
    void* const monitor = static_cast<uint8_t*>(device) +
            kEmbeddedMonitorOffset;
    const uint32_t requestState = registrationRequestState(monitor);

    if (gPowerRecoveryState == PowerRecoveryState::kComplete) {
        if (operatingMode == kOnlineOperatingMode) return true;
        setProperty(kStatusProperty, "waiting_online");
        return false;
    }
    if (gPowerRecoveryState == PowerRecoveryState::kFailed) {
        setProperty(kStatusProperty, "power_recovery_failed");
        return false;
    }
    if (gPowerRecoveryState == PowerRecoveryState::kNotStarted) {
        if (operatingMode == kOnlineOperatingMode) {
            gPowerRecoveryState = PowerRecoveryState::kComplete;
            return true;
        }
        return beginRegistrationPowerQuery(monitor);
    }
    if (gPowerRecoveryState == PowerRecoveryState::kQueryPending) {
        if (requestState == kRegistrationRequestQueryPending) {
            const time_t now = time(nullptr);
            if (gPowerQueryDeadline == 0 ||
                    now == static_cast<time_t>(-1) ||
                    now < gPowerQueryDeadline) {
                setProperty(kStatusProperty, "power_query_pending");
                schedulePowerPoll();
                return false;
            }
            ++gPowerFailureCount;
            publishCounters();
            setProperty(kStatusProperty, "power_query_timeout");
            ALOGE("registration-status power query timed out state=%u",
                  requestState);
            gPowerRecoveryState = PowerRecoveryState::kFailed;
            return false;
        }
        if (requestState != kRegistrationRequestIdle) {
            ++gPowerFailureCount;
            publishCounters();
            setProperty(kStatusProperty, "power_query_state_error");
            ALOGE("registration-status query completed in state=%u",
                  requestState);
            gPowerRecoveryState = PowerRecoveryState::kFailed;
            return false;
        }
        return beginRegistrationResubscribe(
                monitor, operatingMode == kOnlineOperatingMode
                        ? "power_resubscribing"
                        : "power_query_offline");
    }
    if (gPowerRecoveryState == PowerRecoveryState::kResubscribePending) {
        if (requestState == kRegistrationRequestSubscribePending) {
            setProperty(kStatusProperty, "power_resubscribing");
            schedulePowerPoll();
            return false;
        }
        if (requestState != kRegistrationRequestSubscribed) {
            ++gPowerFailureCount;
            publishCounters();
            setProperty(kStatusProperty, "power_resubscribe_state_error");
            ALOGE("registration-status resubscribe ended in state=%u",
                  requestState);
            gPowerRecoveryState = PowerRecoveryState::kFailed;
            return false;
        }
        if (operatingMode != kOnlineOperatingMode) {
            setProperty(kStatusProperty, "power_query_offline");
            gPowerRecoveryState = PowerRecoveryState::kFailed;
            return false;
        }
        gPowerRecoveryState = PowerRecoveryState::kComplete;
        resetRetry();
        return true;
    }
    return false;
}

bool restorePowerSubscription(void* device) {
    if (gPowerRecoveryState == PowerRecoveryState::kNotStarted ||
            gPowerRecoveryState == PowerRecoveryState::kComplete) {
        return true;
    }
    void* const monitor = static_cast<uint8_t*>(device) +
            kEmbeddedMonitorOffset;
    const uint32_t requestState = registrationRequestState(monitor);
    if (requestState == kRegistrationRequestSubscribed) {
        gPowerRecoveryState = PowerRecoveryState::kNotStarted;
        gPowerQueryDeadline = 0;
        return true;
    }
    if (requestState == kRegistrationRequestIdle) {
        return beginRegistrationResubscribe(
                monitor, "disable_power_resubscribing");
    }
    if (requestState == kRegistrationRequestQueryPending ||
            requestState == kRegistrationRequestSubscribePending) {
        setProperty(kStatusProperty, "disable_waiting_power_request");
        schedulePowerPoll();
        return false;
    }
    ++gPowerFailureCount;
    publishCounters();
    setProperty(kStatusProperty, "disable_power_state_error");
    ALOGE("cannot restore registration subscription from state=%u",
          requestState);
    return false;
}

bool removeActiveService(const char* reasonStatus) {
    if (gActiveService[0] == '\0') return true;
    void* device = nullptr;
    void* config = nullptr;
    if (!resolveObjects(&device, &config)) {
        (void)config;
        ++gFailureCount;
        publishCounters();
        setProperty(kStatusProperty, "remove_object_error");
        ALOGE("cannot resolve stock objects for paired RemoveService");
        scheduleRetry();
        return false;
    }
    const size_t length = strlen(gActiveService);
    void* const monitor = static_cast<uint8_t*>(device) +
            kEmbeddedMonitorOffset;
    if (gRemoveService(monitor, gActiveService,
                       static_cast<unsigned short>(length)) != 1) {
        ++gFailureCount;
        publishCounters();
        setProperty(kStatusProperty, "remove_failed");
        ALOGE("stock RemoveService failed length=%zu", length);
        scheduleRetry();
        return false;
    }
    ++gRemoveCount;
    gActiveService[0] = '\0';
    gBearerSendCount = 0;
    setProperty(kTagProperty, "");
    setProperty(kStatusProperty, reasonStatus);
    publishCounters();
    resetRetry();
    return true;
}

}  // namespace

bool initializeServiceLifecycle(GetRcsObjectFunction getRcsObject) {
    setProperty(kStatusProperty, "initializing");
    setProperty(kTagProperty, "");
    publishCounters();
    if (getRcsObject == nullptr) {
        setProperty(kStatusProperty, "dependency_error");
        return false;
    }
    gMonitorLibrary = dlopen(kMonitorLibrary, RTLD_NOW | RTLD_LOCAL);
    if (gMonitorLibrary == nullptr) {
        ALOGE("cannot load %s: %s", kMonitorLibrary, dlerror());
        setProperty(kStatusProperty, "dependency_error");
        return false;
    }
    dlerror();
    gAddService = reinterpret_cast<AddServiceFunction>(
            dlsym(gMonitorLibrary, kAddSymbol));
    const char* addError = dlerror();
    dlerror();
    gRemoveService = reinterpret_cast<RemoveServiceFunction>(
            dlsym(gMonitorLibrary, kRemoveSymbol));
    const char* removeError = dlerror();
    dlerror();
    gCheckRegistrationStatus = reinterpret_cast<MonitorActionFunction>(
            dlsym(gMonitorLibrary, kCheckRegistrationSymbol));
    const char* checkError = dlerror();
    dlerror();
    gSubscribeRegistrationStatus =
            reinterpret_cast<SubscribeRegistrationFunction>(
                    dlsym(gMonitorLibrary, kSubscribeRegistrationSymbol));
    const char* subscribeError = dlerror();
    dlerror();
    gUnsubscribeRegistrationStatus =
            reinterpret_cast<MonitorActionFunction>(
                    dlsym(gMonitorLibrary, kUnsubscribeRegistrationSymbol));
    const char* unsubscribeError = dlerror();
    if (gAddService == nullptr || addError != nullptr ||
            gRemoveService == nullptr || removeError != nullptr ||
            gCheckRegistrationStatus == nullptr || checkError != nullptr ||
            gSubscribeRegistrationStatus == nullptr ||
                    subscribeError != nullptr ||
            gUnsubscribeRegistrationStatus == nullptr ||
                    unsubscribeError != nullptr) {
        ALOGE("cannot resolve stock lifecycle ABI add=%d remove=%d check=%d "
              "subscribe=%d unsubscribe=%d library=%s",
              gAddService != nullptr && addError == nullptr,
              gRemoveService != nullptr && removeError == nullptr,
              gCheckRegistrationStatus != nullptr && checkError == nullptr,
              gSubscribeRegistrationStatus != nullptr &&
                      subscribeError == nullptr,
              gUnsubscribeRegistrationStatus != nullptr &&
                      unsubscribeError == nullptr,
              kMonitorLibrary);
        setProperty(kStatusProperty, "dependency_error");
        dlclose(gMonitorLibrary);
        gMonitorLibrary = nullptr;
        gAddService = nullptr;
        gRemoveService = nullptr;
        gCheckRegistrationStatus = nullptr;
        gSubscribeRegistrationStatus = nullptr;
        gUnsubscribeRegistrationStatus = nullptr;
        return false;
    }
    gGetRcsObject = getRcsObject;
    gInitialized = true;
    setProperty(kStatusProperty,
                lifecycleEnabled() ? "waiting_snapshot" : "disabled");
    return true;
}

void reconcileServiceLifecycle() {
    if (!gInitialized || !retryDue()) return;

    ServiceStateSnapshot snapshot;
    if (!getServiceStateSnapshot(&snapshot)) {
        if (lifecycleEnabled()) setProperty(kStatusProperty, "waiting_snapshot");
        return;
    }
    gLastSequence = snapshot.sequence;
    publishCounters();

    if (!lifecycleEnabled()) {
        if (gPowerRecoveryState != PowerRecoveryState::kNotStarted &&
                gPowerRecoveryState != PowerRecoveryState::kComplete) {
            void* device = nullptr;
            void* config = nullptr;
            if (!resolveObjects(&device, &config)) {
                (void)config;
                ++gPowerFailureCount;
                publishCounters();
                setProperty(kStatusProperty, "disable_power_object_error");
                schedulePowerPoll();
                return;
            }
            if (!restorePowerSubscription(device)) return;
        }
        if (!removeActiveService("disabled") && gActiveService[0] != '\0') {
            return;
        }
        setProperty(kStatusProperty, "disabled");
        return;
    }

    const uint32_t unsupported = snapshot.enabledMask & ~kSupportedMask;
    const bool voiceEnabled = (snapshot.enabledMask & kSupportedMask) != 0;
    if (snapshot.partialMask != 0 || unsupported != 0) {
        if (!removeActiveService("unsupported_state")) return;
        setProperty(kStatusProperty, "unsupported_state");
        return;
    }
    if (!voiceEnabled) {
        if (!removeActiveService("inactive")) return;
        setProperty(kStatusProperty, "inactive");
        return;
    }
    void* device = nullptr;
    void* config = nullptr;
    char service[kMaximumServiceString] = {};
    if (!resolveObjects(&device, &config)) {
        ++gFailureCount;
        publishCounters();
        setProperty(kStatusProperty, "object_error");
        ALOGE("cannot resolve stock objects for AddService");
        scheduleRetry();
        return;
    }
    const uint32_t operatingMode = readUint32(
            device, kDeviceOperatingModeOffset);
    if (operatingMode != gLastOperatingMode) {
        // Registration-manager PowerDown invalidates one-shot service state.
        // Do not preserve a false local success into the next online epoch.
        gActiveService[0] = '\0';
        gBearerSendCount = 0;
        setProperty(kTagProperty, "");
        resetRetry();
        gLastOperatingMode = operatingMode;
    }
    if (!advancePowerRecovery(device, operatingMode)) return;
    if (readUint32(device, kDevicePdpConnectedOffset) == 1) {
        setProperty(kStatusProperty, "bearer_ready");
        resetRetry();
        return;
    }
    if (gBearerSendCount >= kMaximumBearerSends) {
        setProperty(kStatusProperty, "bearer_timeout");
        return;
    }
    if (!constructVoiceService(config, service, sizeof(service))) {
        ++gFailureCount;
        publishCounters();
        setProperty(kStatusProperty, "feature_unavailable");
        ALOGE("configured MMTEL feature is unavailable or malformed");
        scheduleRetry();
        return;
    }
    const size_t length = strlen(service);
    void* const monitor = static_cast<uint8_t*>(device) +
            kEmbeddedMonitorOffset;
    // MMTEL is not accepted by IMSDevice::IMSDevAddService(), whose stock
    // filter is RCS-only. The voice producer uses RegisterServiceMonitor, but
    // only after its PowerUp indication. Repeat at a bounded rate until the
    // resulting DCM callback confirms the IMS PDP bearer.
    if (gAddService(monitor, service,
                    static_cast<unsigned short>(length)) != 1) {
        ++gFailureCount;
        publishCounters();
        setProperty(kStatusProperty, "add_failed");
        ALOGE("stock AddService failed length=%zu", length);
        scheduleRetry();
        return;
    }
    memcpy(gActiveService, service, length + 1);
    ++gBearerSendCount;
    ++gAddCount;
    setProperty(kTagProperty, gActiveService);
    setProperty(kStatusProperty, "awaiting_bearer");
    publishCounters();
    scheduleRetry();
}

void shutdownServiceLifecycle() {
    if (!gInitialized) return;
    if (retryDue()) (void)removeActiveService("stopped");
}

}  // namespace imscompat
