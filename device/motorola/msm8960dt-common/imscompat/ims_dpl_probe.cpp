/* Bounded launcher for the exact stock obake RCS/DPL bootstrap ABI. */

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <new>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(IMS_DPL_SERVICE)
#include "ims_initial_registration.h"
#include "ims_long_timer_bridge.h"
#include "ims_service_receiver.h"
#include "ims_service_lifecycle.h"
#endif

#if defined(IMS_DPL_SERVICE)
#include <cutils/properties.h>
#include <log/log.h>
#endif

namespace {

constexpr char kRcsLibrary[] = "lib-imsrcs.so";
#if !defined(IMS_DPL_SERVICE)
constexpr unsigned kMinimumDurationSeconds = 5;
constexpr unsigned kMaximumDurationSeconds = 60;
#endif
constexpr unsigned kWatchdogMarginSeconds = 20;
constexpr size_t kMaximumActiveTimers = 64;

constexpr int kSuccess = 0;
constexpr int kUsageError = 2;
constexpr int kLoadError = 3;
constexpr int kStartError = 4;
constexpr int kClockError = 5;

#if defined(IMS_DPL_SERVICE)
constexpr char kServiceStatusProperty[] = "sys.ims.dpl.status";
constexpr char kConfigBootstrapProperty[] = "persist.ims.config.bootstrap";
constexpr char kConfigMonitorRepairProperty[] = "persist.ims.config.repair";
// This path belongs to modem operation modes 6/7 (the SKT/KT HTTPS
// autoconfiguration workflow), not carrier-neutral operation mode 0. Require
// a semantic opt-in value so an old persisted value of "1" from the earlier
// experiment cannot silently reactivate it after a dirty system flash.
constexpr char kCarrierConfigOptInValue[] = "mode6_or_7";
constexpr char kConfigStatusProperty[] = "sys.ims.config.status";
constexpr char kConfigRepairStatusProperty[] = "sys.ims.config.repair";
constexpr char kConfigRepairPhaseProperty[] = "sys.ims.config.repair.phase";
constexpr char kConfigRequestCountProperty[] = "sys.ims.config.requests";
constexpr char kConfigSuccessCountProperty[] = "sys.ims.config.successes";
constexpr char kConfigFailureCountProperty[] = "sys.ims.config.failures";
constexpr char kConfigOpenCallsProperty[] = "sys.ims.config.open.calls";
constexpr char kConfigOpenResultProperty[] = "sys.ims.config.open.result";
constexpr char kConfigRegCallsProperty[] = "sys.ims.config.reg.calls";
constexpr char kConfigRegResultProperty[] = "sys.ims.config.reg.result";
constexpr char kConfigSendCallsProperty[] = "sys.ims.config.send.calls";
constexpr char kConfigSendResultProperty[] = "sys.ims.config.send.result";
constexpr char kConfigSendErrnoProperty[] = "sys.ims.config.send.errno";
constexpr char kConfigSendResolvedProperty[] = "sys.ims.config.send.resolved";
constexpr char kConfigSendKindProperty[] = "sys.ims.config.send.kind";
constexpr char kConfigSendLengthProperty[] = "sys.ims.config.send.len";
constexpr char kConfigSendStatusBeforeProperty[] = "sys.ims.config.send.st.pre";
constexpr char kConfigSendStatusAfterProperty[] = "sys.ims.config.send.st.post";
constexpr char kConfigSendOwnerProperty[] = "sys.ims.config.send.owner";
constexpr char kConfigSendConnectionProperty[] = "sys.ims.config.send.conn";
constexpr char kConfigSendConnectionStateProperty[] =
        "sys.ims.config.send.conn.state";
constexpr char kConfigSendSocketDeltaProperty[] =
        "sys.ims.config.send.sock.delta";
constexpr char kConfigMonitorProperty[] = "sys.ims.config.monitor";
constexpr char kConfigProfileProperty[] = "sys.ims.config.profile";
constexpr char kConfigSubscribeStateProperty[] = "sys.ims.config.sub.state";
constexpr unsigned kServiceStartupTimeoutSeconds = 60;
constexpr unsigned kServiceHealthIntervalSeconds = 5;
constexpr unsigned kServiceFailureBackoffSeconds = 60;
constexpr unsigned kConfigCreateTimeoutSeconds = 30;
constexpr unsigned kConfigResponseTimeoutSeconds = 60;
constexpr unsigned kConfigManagerWaitAttempts = 500;
constexpr useconds_t kConfigManagerWaitDelayUs = 10 * 1000;
constexpr char kPolicyRulePriority[] = "13900";

void setServiceStatus(const char* status) {
    if (property_set(kServiceStatusProperty, status) != 0) {
        ALOGE("cannot set %s=%s", kServiceStatusProperty, status);
    }
}

void setConfigStatus(const char* status) {
    if (property_set(kConfigStatusProperty, status) != 0) {
        ALOGE("cannot set %s=%s", kConfigStatusProperty, status);
    }
}

void setConfigRepairStatus(const char* status) {
    if (property_set(kConfigRepairStatusProperty, status) != 0) {
        ALOGE("cannot set %s=%s", kConfigRepairStatusProperty, status);
    }
}

void setConfigRepairPhase(const char* phase) {
    if (property_set(kConfigRepairPhaseProperty, phase) != 0) {
        ALOGE("cannot set %s=%s", kConfigRepairPhaseProperty, phase);
    }
}

void setCounterProperty(const char* property, sig_atomic_t value) {
    char text[16];
    const int length = snprintf(text, sizeof(text), "%d",
                                static_cast<int>(value));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(text) ||
        property_set(property, text) != 0) {
        ALOGE("cannot publish counter property=%s value=%d", property,
              static_cast<int>(value));
    }
}

bool isConfigBootstrapEnabled() {
    char value[PROP_VALUE_MAX];
    property_get(kConfigBootstrapProperty, value, "disabled");
    return strcmp(value, kCarrierConfigOptInValue) == 0;
}

bool isConfigMonitorRepairEnabled() {
    char value[PROP_VALUE_MAX];
    property_get(kConfigMonitorRepairProperty, value, "disabled");
    return strcmp(value, kCarrierConfigOptInValue) == 0;
}

int cleanupPolicyRule() {
    execl("/system/bin/ip", "ip", "-6", "rule", "del", "pref",
          kPolicyRulePriority, static_cast<char*>(nullptr));
    ALOGE("cannot execute policy-rule cleanup: %s", strerror(errno));
    return EXIT_FAILURE;
}
#endif

typedef void (*StatusCallbackFn)(unsigned status);
typedef void (*StartTimerFn)(unsigned intervalMilliseconds, void* logicalHandle,
                             void** nativeHandle);
typedef void (*StopTimerFn)(void* nativeHandle);
typedef int (*StartRcsServiceFn)(const void* callbacks);
typedef int (*StopRcsServiceFn)();
typedef int (*IsRcsServiceStartedFn)();
typedef void (*TimerExpiredFn)(void* logicalHandle);
typedef void* (*GetRcsInitObjectFn)();
typedef int (*RcsInitIsServiceStartedFn)(void* object);
typedef void* (*GetRcsObjectFn)();
typedef void (*DplTraceResetFn)();
typedef int (*DplTraceGetIntFn)();

#if defined(IMS_DPL_SERVICE)
typedef void (*ConfigRequestSentFn)(void* context);
typedef void (*ConfigSuccessFn)(void* context, void* message);
typedef void (*ConfigFailureFn)(void* context, void* message);

struct ConfigCallbacks {
    ConfigRequestSentFn requestSent;
    ConfigSuccessFn success;
    ConfigFailureFn failure;
    void* reserved0;
    void* reserved1;
};

typedef void* (*CreateConfigServiceFn)(const ConfigCallbacks* callbacks,
                                       void* context);
typedef int (*RemoveConfigListenerFn)(void* serviceHandle,
                                      const ConfigCallbacks* callbacks);
typedef void (*DestroyConfigServiceFn)(void* serviceHandle);
typedef void (*ConfigIpcHandlerConstructorFn)(void* object,
                                              void* configManager);
typedef void (*ConfigIpcHandlerDestructorFn)(void* object);

#if defined(__ANDROID__)
static_assert(sizeof(ConfigCallbacks) == 20,
              "stock QRCS_CONFIG_LISTENER ABI must remain five ARM words");
#endif

struct ConfigCallbackContext {
    uint32_t magic;
};

constexpr uint32_t kConfigCallbackMagic = 0x51434647u;
// Exact SU6-7.3 ARM object offsets recovered from lib-imsrcs.so. SU6-7.3
// omits the IPC-handler construction performed by the comparison stock blob.
// Reproduce its object layout without modifying either proprietary library.
constexpr size_t kRcsConfigManagerOffset = 192;
constexpr size_t kConfigHttpsMonitorOffset = 92;
constexpr size_t kConfigInitializedOffset = 4064;
constexpr size_t kHttpsConnectionProfileOffset = 40;
constexpr size_t kHttpsSubscriptionStateOffset = 56;
constexpr size_t kConfigIpcHandlerParentOffset = 60;
constexpr size_t kConfigIpcHandlerAllocationSize = 64;
#if defined(__ANDROID__)
static_assert(kHttpsSubscriptionStateOffset + sizeof(int) <=
                      kConfigIpcHandlerAllocationSize,
              "recovered config IPC handler cannot hold monitor state");
static_assert(kConfigIpcHandlerParentOffset + sizeof(void*) <=
                      kConfigIpcHandlerAllocationSize,
              "recovered config IPC handler cannot hold manager pointer");
#endif
ConfigCallbackContext gConfigCallbackContext = {kConfigCallbackMagic};
volatile sig_atomic_t gConfigRequestCount = 0;
volatile sig_atomic_t gConfigSuccessCount = 0;
volatile sig_atomic_t gConfigFailureCount = 0;
volatile sig_atomic_t gConfigInvalidContextCount = 0;
volatile sig_atomic_t gConfigLastEvent = 0;

bool validConfigContext(void* context) {
    return context == &gConfigCallbackContext &&
            gConfigCallbackContext.magic == kConfigCallbackMagic;
}

void configRequestSent(void* context) {
    if (!validConfigContext(context)) {
        __sync_add_and_fetch(&gConfigInvalidContextCount, 1);
        return;
    }
    __sync_add_and_fetch(&gConfigRequestCount, 1);
    __sync_lock_test_and_set(&gConfigLastEvent, 1);
}

void configSuccess(void* context, void* message) {
    (void)message;
    if (!validConfigContext(context)) {
        __sync_add_and_fetch(&gConfigInvalidContextCount, 1);
        return;
    }
    __sync_add_and_fetch(&gConfigSuccessCount, 1);
    __sync_lock_test_and_set(&gConfigLastEvent, 2);
}

void configFailure(void* context, void* message) {
    (void)message;
    if (!validConfigContext(context)) {
        __sync_add_and_fetch(&gConfigInvalidContextCount, 1);
        return;
    }
    __sync_add_and_fetch(&gConfigFailureCount, 1);
    __sync_lock_test_and_set(&gConfigLastEvent, 3);
}

ConfigCallbacks gConfigCallbacks = {
        configRequestSent, configSuccess, configFailure, nullptr, nullptr};

void publishConfigCounters() {
    setCounterProperty(kConfigRequestCountProperty, gConfigRequestCount);
    setCounterProperty(kConfigSuccessCountProperty, gConfigSuccessCount);
    setCounterProperty(kConfigFailureCountProperty, gConfigFailureCount);
}

struct ConfigMonitorSnapshot {
    bool monitorPresent;
    bool profilePresent;
    int subscriptionState;
};

ConfigMonitorSnapshot inspectConfigMonitor(void* rcsObject) {
    ConfigMonitorSnapshot result = {false, false, -1};
    if (rcsObject == nullptr) return result;
    void* const configManager = *reinterpret_cast<void**>(
            static_cast<unsigned char*>(rcsObject) +
            kRcsConfigManagerOffset);
    if (configManager == nullptr) return result;
    void* const monitor = *reinterpret_cast<void**>(
            static_cast<unsigned char*>(configManager) +
            kConfigHttpsMonitorOffset);
    if (monitor == nullptr) return result;
    result.monitorPresent = true;
    result.profilePresent = *reinterpret_cast<void**>(
            static_cast<unsigned char*>(monitor) +
            kHttpsConnectionProfileOffset) != nullptr;
    result.subscriptionState = *reinterpret_cast<int*>(
            static_cast<unsigned char*>(monitor) +
            kHttpsSubscriptionStateOffset);
    return result;
}

bool repairConfigHandler(void* configManager,
                         ConfigIpcHandlerConstructorFn constructor,
                         ConfigIpcHandlerDestructorFn destructor) {
    if (configManager == nullptr || constructor == nullptr ||
        destructor == nullptr) {
        setConfigStatus("monitor_repair_dependency_error");
        return false;
    }
    void** const monitorField = reinterpret_cast<void**>(
            static_cast<unsigned char*>(configManager) +
            kConfigHttpsMonitorOffset);
    void* const existingMonitor =
            __sync_val_compare_and_swap(monitorField, nullptr, nullptr);
    if (existingMonitor != nullptr) {
        setConfigRepairStatus("not_needed");
        return true;
    }

    void* const handler = ::operator new(kConfigIpcHandlerAllocationSize,
                                         std::nothrow);
    if (handler == nullptr) {
        setConfigStatus("monitor_allocation_failed");
        setConfigRepairStatus("allocation_failed");
        return false;
    }
    memset(handler, 0, kConfigIpcHandlerAllocationSize);
    constructor(handler, configManager);
    void* const profile = *reinterpret_cast<void**>(
            static_cast<unsigned char*>(handler) +
            kHttpsConnectionProfileOffset);
    void* const parent = *reinterpret_cast<void**>(
            static_cast<unsigned char*>(handler) +
            kConfigIpcHandlerParentOffset);
    if (profile == nullptr || parent != configManager) {
        destructor(handler);
        ::operator delete(handler);
        setConfigStatus(profile == nullptr ? "monitor_profile_failed" :
                                             "handler_parent_failed");
        setConfigRepairStatus(profile == nullptr ? "profile_failed" :
                                                  "parent_failed");
        return false;
    }
    if (!__sync_bool_compare_and_swap(monitorField, nullptr, handler)) {
        destructor(handler);
        ::operator delete(handler);
        if (__sync_val_compare_and_swap(monitorField, nullptr, nullptr) !=
            nullptr) {
            setConfigRepairStatus("not_needed");
            return true;
        }
        setConfigStatus("handler_install_failed");
        setConfigRepairStatus("install_failed");
        return false;
    }
    setConfigRepairStatus("installed");
    return true;
}

struct ConfigRepairTask {
    GetRcsObjectFn getRcsObject;
    ConfigIpcHandlerConstructorFn constructor;
    ConfigIpcHandlerDestructorFn destructor;
    volatile int stopRequested;
    bool succeeded;
};

void* configRepairThread(void* argument) {
    ConfigRepairTask* const task =
            static_cast<ConfigRepairTask*>(argument);
    setConfigStatus("waiting_config_initialize");
    setConfigRepairPhase("during_manager_initialize");
    for (unsigned attempt = 0; attempt < kConfigManagerWaitAttempts;
         ++attempt) {
        if (__sync_fetch_and_add(&task->stopRequested, 0) != 0) {
            setConfigRepairStatus("start_aborted");
            return nullptr;
        }
        void* const rcsObject = task->getRcsObject();
        if (rcsObject != nullptr) {
            void** const configManagerField = reinterpret_cast<void**>(
                    static_cast<unsigned char*>(rcsObject) +
                    kRcsConfigManagerOffset);
            void* const configManager = __sync_val_compare_and_swap(
                    configManagerField, nullptr, nullptr);
            if (configManager != nullptr) {
                uint32_t* const initializedField =
                        reinterpret_cast<uint32_t*>(
                                static_cast<unsigned char*>(configManager) +
                                kConfigInitializedOffset);
                const uint32_t initialized = __sync_val_compare_and_swap(
                        initializedField, 0u, 0u);
                if (initialized == 1) {
                    // Initialize() has performed its final null store at +0x5c.
                    // This is the first safe external insertion point.
                    setConfigStatus("repairing_config_handler");
                    setConfigRepairPhase("after_manager_initialize");
                    task->succeeded = repairConfigHandler(
                            configManager, task->constructor,
                            task->destructor);
                    return nullptr;
                }
            }
        }
        usleep(kConfigManagerWaitDelayUs);
    }
    ALOGE("stock config manager did not initialize within %u ms",
          (kConfigManagerWaitAttempts * kConfigManagerWaitDelayUs) / 1000);
    setConfigStatus("config_manager_timeout");
    setConfigRepairStatus("manager_timeout");
    return nullptr;
}

void destroyConfigService(void** handle,
                          RemoveConfigListenerFn removeListener,
                          DestroyConfigServiceFn destroyService) {
    if (handle == nullptr || *handle == nullptr) return;
    if (removeListener != nullptr) {
        const int result = removeListener(*handle, &gConfigCallbacks);
        if (result != 0) {
            ALOGE("config listener removal failed result=%d handle=%p",
                  result, *handle);
        }
    }
    if (destroyService != nullptr) destroyService(*handle);
    *handle = nullptr;
}
#endif

struct TimerCallbacks {
    StartTimerFn start;
    StopTimerFn stop;
};

struct RcsCallbacks {
    StatusCallbackFn status;
    TimerCallbacks* timers;
};

struct TimerTask {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t condition;
    TimerTask* next;
    void* logicalHandle;
    unsigned intervalMilliseconds;
    bool cancelled;
    bool selfDestroy;
};

pthread_mutex_t gTimerListLock = PTHREAD_MUTEX_INITIALIZER;
TimerTask* gTimerList = nullptr;
size_t gActiveTimerCount = 0;
TimerExpiredFn gTimerExpired = nullptr;

volatile sig_atomic_t gStatusEventCount = 0;
volatile sig_atomic_t gLastStatus = -1;
volatile sig_atomic_t gTimerStartCount = 0;
volatile sig_atomic_t gTimerFireCount = 0;
volatile sig_atomic_t gTimerStopCount = 0;
volatile sig_atomic_t gTimerFailureCount = 0;

void watchdogHandler(int) {
    static const char message[] =
            "ERROR stage=watchdog class=timeout action=forced_exit\n";
    write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

void statusCallback(unsigned status) {
    gLastStatus = static_cast<sig_atomic_t>(status);
    if (gStatusEventCount < 0x7fff) ++gStatusEventCount;
}

void destroyTimerTask(TimerTask* task) {
    if (task == nullptr) return;
    pthread_cond_destroy(&task->condition);
    pthread_mutex_destroy(&task->lock);
    free(task);
}

void removeTimerTaskLocked(TimerTask* task) {
    TimerTask** cursor = &gTimerList;
    while (*cursor != nullptr) {
        if (*cursor == task) {
            *cursor = task->next;
            task->next = nullptr;
            if (gActiveTimerCount > 0) --gActiveTimerCount;
            return;
        }
        cursor = &(*cursor)->next;
    }
}

void* timerThread(void* argument) {
    TimerTask* task = static_cast<TimerTask*>(argument);
    timespec deadline;
    bool shouldFire = false;

    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        ++gTimerFailureCount;
    } else {
        const uint64_t nanoseconds =
                static_cast<uint64_t>(deadline.tv_nsec) +
                static_cast<uint64_t>(task->intervalMilliseconds % 1000) *
                        1000000ULL;
        deadline.tv_sec += task->intervalMilliseconds / 1000 +
                           static_cast<time_t>(nanoseconds / 1000000000ULL);
        deadline.tv_nsec = static_cast<long>(nanoseconds % 1000000000ULL);

        pthread_mutex_lock(&task->lock);
        int waitResult = 0;
        while (!task->cancelled && waitResult != ETIMEDOUT) {
            waitResult = pthread_cond_timedwait(&task->condition, &task->lock,
                                                &deadline);
            if (waitResult != 0 && waitResult != ETIMEDOUT) {
                ++gTimerFailureCount;
                task->cancelled = true;
            }
        }
        shouldFire = !task->cancelled && waitResult == ETIMEDOUT;
        pthread_mutex_unlock(&task->lock);
    }

    if (shouldFire && gTimerExpired != nullptr) {
        ++gTimerFireCount;
        gTimerExpired(task->logicalHandle);
    }

    pthread_mutex_lock(&task->lock);
    const bool selfDestroy = task->selfDestroy;
    pthread_mutex_unlock(&task->lock);
    if (selfDestroy) destroyTimerTask(task);
    return nullptr;
}

void stopTimer(void* nativeHandle) {
    if (nativeHandle == nullptr) return;

    pthread_mutex_lock(&gTimerListLock);
    TimerTask* task = gTimerList;
    while (task != nullptr && task != nativeHandle &&
           task->logicalHandle != nativeHandle) {
        task = task->next;
    }
    if (task != nullptr) removeTimerTaskLocked(task);
    pthread_mutex_unlock(&gTimerListLock);

    if (task == nullptr) {
        ++gTimerFailureCount;
        fprintf(stderr,
                "ERROR stage=timer-stop class=unknown_handle handle=%p\n",
                nativeHandle);
        return;
    }

    pthread_mutex_lock(&task->lock);
    task->cancelled = true;
    pthread_cond_signal(&task->condition);
    const bool calledFromTimer = pthread_equal(pthread_self(), task->thread);
    if (calledFromTimer) task->selfDestroy = true;
    pthread_mutex_unlock(&task->lock);

    ++gTimerStopCount;
    if (!calledFromTimer) {
        const int joinResult = pthread_join(task->thread, nullptr);
        if (joinResult != 0) {
            ++gTimerFailureCount;
            fprintf(stderr,
                    "ERROR stage=timer-stop class=join_failure code=%d\n",
                    joinResult);
        }
        destroyTimerTask(task);
    }
}

void startTimer(unsigned intervalMilliseconds, void* logicalHandle,
                void** nativeHandle) {
    if (nativeHandle != nullptr) *nativeHandle = nullptr;
    if (logicalHandle == nullptr || gTimerExpired == nullptr) {
        ++gTimerFailureCount;
        fprintf(stderr,
                "ERROR stage=timer-start class=invalid_callback_state "
                "logical_handle=%p\n",
                logicalHandle);
        return;
    }

    pthread_mutex_lock(&gTimerListLock);
    const bool atLimit = gActiveTimerCount >= kMaximumActiveTimers;
    pthread_mutex_unlock(&gTimerListLock);
    if (atLimit) {
        ++gTimerFailureCount;
        fprintf(stderr,
                "ERROR stage=timer-start class=resource_limit limit=%zu\n",
                kMaximumActiveTimers);
        return;
    }

    TimerTask* task = static_cast<TimerTask*>(calloc(1, sizeof(TimerTask)));
    if (task == nullptr) {
        ++gTimerFailureCount;
        fprintf(stderr, "ERROR stage=timer-start class=allocation_failure\n");
        return;
    }
    task->logicalHandle = logicalHandle;
    task->intervalMilliseconds = intervalMilliseconds;

    const int mutexResult = pthread_mutex_init(&task->lock, nullptr);
    if (mutexResult != 0) {
        ++gTimerFailureCount;
        fprintf(stderr,
                "ERROR stage=timer-start class=mutex_init_failure code=%d\n",
                mutexResult);
        free(task);
        return;
    }
    const int conditionResult = pthread_cond_init(&task->condition, nullptr);
    if (conditionResult != 0) {
        ++gTimerFailureCount;
        fprintf(stderr,
                "ERROR stage=timer-start class=condition_init_failure "
                "code=%d\n",
                conditionResult);
        pthread_mutex_destroy(&task->lock);
        free(task);
        return;
    }

    pthread_mutex_lock(&gTimerListLock);
    task->next = gTimerList;
    gTimerList = task;
    ++gActiveTimerCount;
    pthread_mutex_unlock(&gTimerListLock);

    const int createResult =
            pthread_create(&task->thread, nullptr, timerThread, task);
    if (createResult != 0) {
        pthread_mutex_lock(&gTimerListLock);
        removeTimerTaskLocked(task);
        pthread_mutex_unlock(&gTimerListLock);
        ++gTimerFailureCount;
        fprintf(stderr,
                "ERROR stage=timer-start class=thread_create_failure "
                "code=%d\n",
                createResult);
        destroyTimerTask(task);
        return;
    }

    if (nativeHandle != nullptr) *nativeHandle = task;
    ++gTimerStartCount;
}

void cleanupTimers() {
    while (true) {
        pthread_mutex_lock(&gTimerListLock);
        TimerTask* task = gTimerList;
        if (task != nullptr) removeTimerTaskLocked(task);
        pthread_mutex_unlock(&gTimerListLock);
        if (task == nullptr) return;

        pthread_mutex_lock(&task->lock);
        task->cancelled = true;
        pthread_cond_signal(&task->condition);
        pthread_mutex_unlock(&task->lock);
        const int joinResult = pthread_join(task->thread, nullptr);
        if (joinResult != 0) {
            ++gTimerFailureCount;
            fprintf(stderr,
                    "ERROR stage=timer-cleanup class=join_failure code=%d\n",
                    joinResult);
        }
        destroyTimerTask(task);
    }
}

#if !defined(IMS_DPL_SERVICE)
bool parseDuration(const char* value, unsigned* duration) {
    if (value == nullptr || duration == nullptr || value[0] == '\0') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' ||
        parsed < kMinimumDurationSeconds ||
        parsed > kMaximumDurationSeconds) {
        return false;
    }
    *duration = static_cast<unsigned>(parsed);
    return true;
}
#endif

template <typename T>
bool loadSymbol(void* library, const char* name, T* destination) {
    dlerror();
    void* symbol = dlsym(library, name);
    const char* error = dlerror();
    if (error != nullptr || symbol == nullptr) {
        fprintf(stderr,
                "ERROR stage=symbol class=missing_dependency symbol=%s "
                "detail=%s\n",
                name, error == nullptr ? "null" : error);
        return false;
    }
    *destination = reinterpret_cast<T>(symbol);
    return true;
}

int waitForDuration(unsigned duration) {
    timespec remaining = {static_cast<time_t>(duration), 0};
    while (nanosleep(&remaining, &remaining) != 0) {
        if (errno == EINTR) continue;
        fprintf(stderr,
                "ERROR stage=observe class=clock_failure errno=%d detail=%s\n",
                errno, strerror(errno));
        return kClockError;
    }
    return kSuccess;
}

#if !defined(IMS_DPL_SERVICE)
void usage(const char* executable) {
    fprintf(stderr, "Usage: %s run <%u-%u seconds>\n", executable,
            kMinimumDurationSeconds, kMaximumDurationSeconds);
}
#endif

}  // namespace

int main(int argc, char** argv) {
    unsigned duration = 0;
#if defined(IMS_DPL_SERVICE)
    if (argc == 2 && strcmp(argv[1], "cleanup") == 0) {
        return cleanupPolicyRule();
    }
    if (argc != 2 || strcmp(argv[1], "serve") != 0) {
        fprintf(stderr, "Usage: %s <serve|cleanup>\n", argv[0]);
        return kUsageError;
    }
    duration = kServiceStartupTimeoutSeconds;
    setServiceStatus("starting");
    if (!imscompat::startServiceStateReceiver()) {
        // Step 3 defines this as an explicit degraded mode: keep the proven
        // DPL baseline alive, but never claim service-lifecycle readiness.
        ALOGE("IMS service-state receiver unavailable; continuing DPL-only");
    }
#else
    if (argc != 3 || strcmp(argv[1], "run") != 0 ||
        !parseDuration(argv[2], &duration)) {
        usage(argv[0]);
        return kUsageError;
    }
#endif

    signal(SIGALRM, watchdogHandler);
    alarm(duration + kWatchdogMarginSeconds);

    void* library = dlopen(kRcsLibrary, RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        fprintf(stderr,
                "ERROR stage=dlopen class=dependency_failure library=%s "
                "detail=%s\n",
                kRcsLibrary, dlerror());
#if defined(IMS_DPL_SERVICE)
        setServiceStatus("dependency_error");
#endif
        return kLoadError;
    }

    StartRcsServiceFn startService = nullptr;
    StopRcsServiceFn stopService = nullptr;
    IsRcsServiceStartedFn isStarted = nullptr;
    GetRcsInitObjectFn getRcsInitObject = nullptr;
    RcsInitIsServiceStartedFn rcsInitIsStarted = nullptr;
    GetRcsObjectFn getRcsObject = nullptr;
    DplTraceResetFn traceReset = nullptr;
    DplTraceGetIntFn traceGetCallCount = nullptr;
    DplTraceGetIntFn traceGetLastResult = nullptr;
    DplTraceGetIntFn traceGetResolveFailureCount = nullptr;
    DplTraceGetIntFn traceGetSendCallCount = nullptr;
    DplTraceGetIntFn traceGetSendFailureCount = nullptr;
    DplTraceGetIntFn traceGetReceiveCallCount = nullptr;
    DplTraceGetIntFn traceGetReceiveFailureCount = nullptr;
    DplTraceGetIntFn traceGetScopeFixAttemptCount = nullptr;
    DplTraceGetIntFn traceGetScopeFixAppliedCount = nullptr;
    DplTraceGetIntFn traceGetScopeFixFailureCount = nullptr;
    DplTraceGetIntFn traceGetPolicyRuleAppliedCount = nullptr;
    DplTraceGetIntFn traceGetPolicyRuleFailureCount = nullptr;
    DplTraceGetIntFn traceGetNetOpenCallCount = nullptr;
    DplTraceGetIntFn traceGetNetOpenLastResult = nullptr;
    DplTraceGetIntFn traceGetNetRegCallCount = nullptr;
    DplTraceGetIntFn traceGetNetRegLastResult = nullptr;
    DplTraceGetIntFn traceGetNetSendDataCallCount = nullptr;
    DplTraceGetIntFn traceGetNetSendDataLastResult = nullptr;
    DplTraceGetIntFn traceGetNetSendDataLastErrno = nullptr;
    DplTraceGetIntFn traceGetNetSendDataLastResolved = nullptr;
    DplTraceGetIntFn traceGetNetSendDataLastMessageKind = nullptr;
    DplTraceGetIntFn traceGetNetSendDataLastLength = nullptr;
    DplTraceGetIntFn traceGetNetSendDataLastStatusBefore = nullptr;
    DplTraceGetIntFn traceGetNetSendDataLastStatusAfter = nullptr;
    DplTraceGetIntFn traceGetNetSendDataLastProfileOwner = nullptr;
    DplTraceGetIntFn traceGetNetSendDataLastConnectionPresent = nullptr;
    DplTraceGetIntFn traceGetNetSendDataLastConnectionState = nullptr;
    DplTraceGetIntFn traceGetNetSendDataLastSocketSendDelta = nullptr;
#if defined(IMS_DPL_SERVICE)
    CreateConfigServiceFn createConfigService = nullptr;
    RemoveConfigListenerFn removeConfigListener = nullptr;
    DestroyConfigServiceFn destroyConfigServiceFn = nullptr;
    ConfigIpcHandlerConstructorFn constructConfigIpcHandler = nullptr;
    ConfigIpcHandlerDestructorFn destructConfigIpcHandler = nullptr;
    void* configServiceHandle = nullptr;
    bool configTransportFailed = false;
    const bool configRequested = isConfigBootstrapEnabled();
    const bool configRepairRequested = isConfigMonitorRepairEnabled();
    bool configSymbolsLoaded = !configRequested;
    bool configHandlerReady = !configRequested;
    pthread_t configRepairThreadHandle;
    bool configRepairThreadStarted = false;
    ConfigRepairTask configRepairTask = {
            getRcsObject, nullptr, nullptr, 0, false};
#endif
    const bool symbolsLoaded =
            loadSymbol(library, "QRcsStartRCSService", &startService) &&
            loadSymbol(library, "QRcsStopRCSService", &stopService) &&
            loadSymbol(library, "QRcsIsRCSServiceStarted", &isStarted) &&
            loadSymbol(library, "QRcsAMTimerExpired", &gTimerExpired) &&
            loadSymbol(library, "_ZN7RcsInit16GetRcsInitObjectEv",
                       &getRcsInitObject) &&
            loadSymbol(library, "_ZN7RcsInit24RcsInit_IsServiceStartedEv",
                       &rcsInitIsStarted) &&
            loadSymbol(library, "_ZN6IMSRcs12GetRcsObjectEv",
                       &getRcsObject) &&
            loadSymbol(RTLD_DEFAULT, "imsDplTraceReset", &traceReset) &&
            loadSymbol(RTLD_DEFAULT, "imsDplTraceGetCallCount",
                       &traceGetCallCount) &&
            loadSymbol(RTLD_DEFAULT, "imsDplTraceGetLastResult",
                       &traceGetLastResult) &&
            loadSymbol(RTLD_DEFAULT, "imsDplTraceGetResolveFailureCount",
                       &traceGetResolveFailureCount) &&
            loadSymbol(RTLD_DEFAULT, "imsDplTraceGetSendCallCount",
                       &traceGetSendCallCount) &&
            loadSymbol(RTLD_DEFAULT, "imsDplTraceGetSendFailureCount",
                       &traceGetSendFailureCount) &&
            loadSymbol(RTLD_DEFAULT, "imsDplTraceGetReceiveCallCount",
                       &traceGetReceiveCallCount) &&
            loadSymbol(RTLD_DEFAULT, "imsDplTraceGetReceiveFailureCount",
                       &traceGetReceiveFailureCount) &&
            loadSymbol(RTLD_DEFAULT, "imsDplTraceGetScopeFixAttemptCount",
                       &traceGetScopeFixAttemptCount) &&
            loadSymbol(RTLD_DEFAULT, "imsDplTraceGetScopeFixAppliedCount",
                       &traceGetScopeFixAppliedCount) &&
            loadSymbol(RTLD_DEFAULT, "imsDplTraceGetScopeFixFailureCount",
                       &traceGetScopeFixFailureCount) &&
            loadSymbol(RTLD_DEFAULT, "imsDplTraceGetPolicyRuleAppliedCount",
                       &traceGetPolicyRuleAppliedCount) &&
            loadSymbol(RTLD_DEFAULT, "imsDplTraceGetPolicyRuleFailureCount",
                       &traceGetPolicyRuleFailureCount) &&
            loadSymbol(RTLD_DEFAULT, "imsDplTraceGetNetOpenCallCount",
                       &traceGetNetOpenCallCount) &&
            loadSymbol(RTLD_DEFAULT, "imsDplTraceGetNetOpenLastResult",
                       &traceGetNetOpenLastResult) &&
            loadSymbol(RTLD_DEFAULT, "imsDplTraceGetNetRegCallCount",
                       &traceGetNetRegCallCount) &&
            loadSymbol(RTLD_DEFAULT, "imsDplTraceGetNetRegLastResult",
                       &traceGetNetRegLastResult) &&
            loadSymbol(RTLD_DEFAULT, "imsDplTraceGetNetSendDataCallCount",
                       &traceGetNetSendDataCallCount) &&
            loadSymbol(RTLD_DEFAULT, "imsDplTraceGetNetSendDataLastResult",
                       &traceGetNetSendDataLastResult) &&
            loadSymbol(RTLD_DEFAULT, "imsDplTraceGetNetSendDataLastErrno",
                       &traceGetNetSendDataLastErrno) &&
            loadSymbol(RTLD_DEFAULT, "imsDplTraceGetNetSendDataLastResolved",
                       &traceGetNetSendDataLastResolved) &&
            loadSymbol(RTLD_DEFAULT,
                       "imsDplTraceGetNetSendDataLastMessageKind",
                       &traceGetNetSendDataLastMessageKind) &&
            loadSymbol(RTLD_DEFAULT, "imsDplTraceGetNetSendDataLastLength",
                       &traceGetNetSendDataLastLength) &&
            loadSymbol(RTLD_DEFAULT,
                       "imsDplTraceGetNetSendDataLastStatusBefore",
                       &traceGetNetSendDataLastStatusBefore) &&
            loadSymbol(RTLD_DEFAULT,
                       "imsDplTraceGetNetSendDataLastStatusAfter",
                       &traceGetNetSendDataLastStatusAfter) &&
            loadSymbol(RTLD_DEFAULT,
                       "imsDplTraceGetNetSendDataLastProfileOwner",
                       &traceGetNetSendDataLastProfileOwner) &&
            loadSymbol(RTLD_DEFAULT,
                       "imsDplTraceGetNetSendDataLastConnectionPresent",
                       &traceGetNetSendDataLastConnectionPresent) &&
            loadSymbol(RTLD_DEFAULT,
                       "imsDplTraceGetNetSendDataLastConnectionState",
                       &traceGetNetSendDataLastConnectionState) &&
            loadSymbol(RTLD_DEFAULT,
                       "imsDplTraceGetNetSendDataLastSocketSendDelta",
                       &traceGetNetSendDataLastSocketSendDelta);
    if (!symbolsLoaded) {
#if defined(IMS_DPL_SERVICE)
        setServiceStatus("dependency_error");
#endif
        dlclose(library);
        return kLoadError;
    }

#if defined(IMS_DPL_SERVICE)
    if (configRequested) {
        configSymbolsLoaded =
                loadSymbol(library, "QRCSCreateConfigService",
                           &createConfigService) &&
                loadSymbol(library, "QConfigService_RemoveListener",
                           &removeConfigListener) &&
                loadSymbol(library, "QRCSDestroyConfigService",
                           &destroyConfigServiceFn) &&
                loadSymbol(library,
                           "_ZN22IMSConfigMgrIPCHandlerC1EP12IMSConfigMgr",
                           &constructConfigIpcHandler) &&
                loadSymbol(library, "_ZN22IMSConfigMgrIPCHandlerD1Ev",
                           &destructConfigIpcHandler);
        if (!configSymbolsLoaded) {
            setConfigStatus("dependency_error");
            setConfigRepairStatus("dependency_error");
            setConfigRepairPhase("before_rcs_start");
        }
    }
#endif

    TimerCallbacks timerCallbacks = {startTimer, stopTimer};
    RcsCallbacks callbacks = {statusCallback, &timerCallbacks};
    traceReset();

#if defined(IMS_DPL_SERVICE)
    if (configRequested && configSymbolsLoaded && configRepairRequested) {
        configRepairTask.getRcsObject = getRcsObject;
        configRepairTask.constructor = constructConfigIpcHandler;
        configRepairTask.destructor = destructConfigIpcHandler;
        setConfigRepairPhase("before_rcs_start");
        const int createResult = pthread_create(
                &configRepairThreadHandle, nullptr, configRepairThread,
                &configRepairTask);
        if (createResult != 0) {
            ALOGE("config repair watcher creation failed code=%d",
                  createResult);
            setConfigStatus("repair_thread_failed");
            setConfigRepairStatus("thread_failed");
            configTransportFailed = true;
        } else {
            configRepairThreadStarted = true;
        }
    } else if (configRequested && !configRepairRequested) {
        setConfigRepairStatus("disabled");
        setConfigRepairPhase("disabled");
    }
#endif

    const int startResult = startService(&callbacks);
    const int afterStart = isStarted();
#if defined(IMS_DPL_SERVICE)
    if (configRepairThreadStarted) {
        if (startResult != 1) {
            __sync_lock_test_and_set(&configRepairTask.stopRequested, 1);
        }
        const int joinResult = pthread_join(configRepairThreadHandle, nullptr);
        if (joinResult != 0) {
            ALOGE("config repair watcher join failed code=%d", joinResult);
            setConfigStatus("repair_thread_join_failed");
            setConfigRepairStatus("thread_join_failed");
            configTransportFailed = true;
        } else {
            configHandlerReady = configRepairTask.succeeded;
            if (!configHandlerReady) configTransportFailed = true;
        }
    }
#endif
    if (startResult != 1) {
        fprintf(stderr,
                "ERROR stage=start class=bootstrap_rejected result=%d "
                "is_started=%d\n",
                startResult, afterStart);
#if defined(IMS_DPL_SERVICE)
        setServiceStatus("start_rejected");
#endif
        cleanupTimers();
        dlclose(library);
        return kStartError;
    }

#if defined(IMS_DPL_SERVICE)
    // lib-imsdpl's stock timer initializer looks only at the fixed
    // /vendor/lib/lib-imsrcs.so path. This port intentionally loads the
    // coherent radio stack from /system/lib/radio-su6-73, so repair the two
    // long-timer callbacks from the selected handle before registration or
    // config work can schedule a delay longer than one second.
    if (!imscompat::initializeLongTimerBridge(library)) {
        ALOGE("DPL long-timer bridge initialization failed");
        setServiceStatus("timer_bridge_failed");
        (void)stopService();
        cleanupTimers();
        dlclose(library);
        return kStartError;
    }
#endif

#if defined(IMS_DPL_SERVICE)
    setServiceStatus("initializing");
    bool becameActive = false;
    for (unsigned elapsed = 0;
         elapsed < kServiceStartupTimeoutSeconds; ++elapsed) {
        void* const internalObject = getRcsInitObject();
        const int publicState = isStarted();
        const int internalState = internalObject == nullptr
                ? -1 : rcsInitIsStarted(internalObject);
        const int sendCalls = traceGetSendCallCount();
        const int sendFailures = traceGetSendFailureCount();
        const int receiveCalls = traceGetReceiveCallCount();
        const int receiveFailures = traceGetReceiveFailureCount();
        const int scopeApplied = traceGetScopeFixAppliedCount();
        const int scopeFailures = traceGetScopeFixFailureCount();
        const int policyApplied = traceGetPolicyRuleAppliedCount();
        const int policyFailures = traceGetPolicyRuleFailureCount();
        const bool transportReady = sendCalls > 0 && receiveCalls > 0 &&
                sendFailures == 0 && receiveFailures == 0 &&
                scopeApplied > 0 && scopeFailures == 0 &&
                policyApplied > 0 && policyFailures == 0;
        if (publicState == 1 && internalState == 1 &&
            gStatusEventCount > 0 && gLastStatus == 1 && transportReady) {
            becameActive = true;
            break;
        }
        if (gStatusEventCount > 0 && gLastStatus != 1) {
            ALOGE("DPL reported initialization failure status=%d",
                  static_cast<int>(gLastStatus));
            setServiceStatus("callback_failed");
            sleep(kServiceFailureBackoffSeconds);
            return kStartError;
        }
        if (waitForDuration(1) != kSuccess) {
            setServiceStatus("clock_error");
            sleep(kServiceFailureBackoffSeconds);
            return kClockError;
        }
    }
    if (!becameActive) {
        void* const internalObject = getRcsInitObject();
        ALOGE("DPL initialization timed out after %u seconds: public=%d "
              "internal=%d callback_events=%d callback_status=%d sends=%d "
              "send_failures=%d receives=%d receive_failures=%d "
              "scope_applied=%d scope_failures=%d policy_applied=%d "
              "policy_failures=%d",
              kServiceStartupTimeoutSeconds, isStarted(),
              internalObject == nullptr
                      ? -1 : rcsInitIsStarted(internalObject),
              static_cast<int>(gStatusEventCount),
              static_cast<int>(gLastStatus), traceGetSendCallCount(),
              traceGetSendFailureCount(), traceGetReceiveCallCount(),
              traceGetReceiveFailureCount(), traceGetScopeFixAppliedCount(),
              traceGetScopeFixFailureCount(),
              traceGetPolicyRuleAppliedCount(),
              traceGetPolicyRuleFailureCount());
        setServiceStatus("startup_timeout");
        sleep(kServiceFailureBackoffSeconds);
        return kStartError;
    }

    if (!configRequested) {
        setConfigStatus("disabled");
        setConfigRepairStatus("not_run");
        setConfigRepairPhase("not_run");
        publishConfigCounters();
        alarm(0);
    } else {
        if (!configSymbolsLoaded) {
            setConfigStatus("dependency_error");
            setConfigRepairStatus("dependency_error");
            publishConfigCounters();
            alarm(0);
        } else {
            // Stock performs this after its status-1 callback. Do it here,
            // outside vendor callback context, after strict DPL readiness.
            const int sendCallsBefore = traceGetNetSendDataCallCount();
            bool monitorReady = true;
            if (configRepairRequested) {
                monitorReady = configHandlerReady;
                if (!monitorReady) {
                    ALOGE("stock-matched config IPC handler construction did "
                          "not succeed; no fallback is enabled");
                }
            } else {
                const ConfigMonitorSnapshot before =
                        inspectConfigMonitor(getRcsObject());
                monitorReady = before.monitorPresent && before.profilePresent;
                if (!monitorReady) {
                    ALOGE("stock HTTPS monitor repair is disabled and the "
                          "monitor is unavailable");
                    setConfigStatus("monitor_unavailable");
                }
            }
            if (monitorReady) {
                setConfigStatus("creating");
                alarm(kConfigCreateTimeoutSeconds);
                configServiceHandle = createConfigService(
                        &gConfigCallbacks, &gConfigCallbackContext);
                alarm(0);
            } else {
                configTransportFailed = true;
            }
            const int openCalls = traceGetNetOpenCallCount();
            const int regCalls = traceGetNetRegCallCount();
            const int sendCalls =
                    traceGetNetSendDataCallCount() - sendCallsBefore;
            const int openResult = traceGetNetOpenLastResult();
            const int regResult = traceGetNetRegLastResult();
            const int sendResult = traceGetNetSendDataLastResult();
            setCounterProperty(kConfigOpenCallsProperty, openCalls);
            setCounterProperty(kConfigOpenResultProperty, openResult);
            setCounterProperty(kConfigRegCallsProperty, regCalls);
            setCounterProperty(kConfigRegResultProperty, regResult);
            setCounterProperty(kConfigSendCallsProperty, sendCalls);
            setCounterProperty(kConfigSendResultProperty, sendResult);
            setCounterProperty(kConfigSendErrnoProperty,
                               traceGetNetSendDataLastErrno());
            setCounterProperty(kConfigSendResolvedProperty,
                               traceGetNetSendDataLastResolved());
            setCounterProperty(kConfigSendKindProperty,
                               traceGetNetSendDataLastMessageKind());
            setCounterProperty(kConfigSendLengthProperty,
                               traceGetNetSendDataLastLength());
            setCounterProperty(kConfigSendStatusBeforeProperty,
                               traceGetNetSendDataLastStatusBefore());
            setCounterProperty(kConfigSendStatusAfterProperty,
                               traceGetNetSendDataLastStatusAfter());
            setCounterProperty(kConfigSendOwnerProperty,
                               traceGetNetSendDataLastProfileOwner());
            setCounterProperty(kConfigSendConnectionProperty,
                               traceGetNetSendDataLastConnectionPresent());
            setCounterProperty(kConfigSendConnectionStateProperty,
                               traceGetNetSendDataLastConnectionState());
            setCounterProperty(kConfigSendSocketDeltaProperty,
                               traceGetNetSendDataLastSocketSendDelta());
            const ConfigMonitorSnapshot monitor =
                    inspectConfigMonitor(getRcsObject());
            setCounterProperty(kConfigMonitorProperty,
                               monitor.monitorPresent ? 1 : 0);
            setCounterProperty(kConfigProfileProperty,
                               monitor.profilePresent ? 1 : 0);
            setCounterProperty(kConfigSubscribeStateProperty,
                               monitor.subscriptionState);
            if (!monitorReady) {
                configTransportFailed = true;
            } else if (configServiceHandle == nullptr) {
                ALOGE("stock config-service creation returned a null handle");
                setConfigStatus("create_failed");
                configTransportFailed = true;
            } else if (!monitor.monitorPresent) {
                ALOGE("stock HTTPS monitor is unavailable");
                setConfigStatus("monitor_unavailable");
                configTransportFailed = true;
            } else if (!monitor.profilePresent) {
                ALOGE("stock HTTPS monitor has no DPL connection profile");
                setConfigStatus("profile_unavailable");
                configTransportFailed = true;
            } else if (sendCalls == 0) {
                ALOGE("stock config service did not send HTTPS subscription "
                      "state=%d startup_open_calls=%d last_open_result=%d "
                      "startup_reg_calls=%d last_reg_result=%d",
                      monitor.subscriptionState, openCalls, openResult,
                      regCalls, regResult);
                setConfigStatus("subscribe_not_sent");
                configTransportFailed = true;
            } else if (sendResult != 0) {
                ALOGE("stock HTTPS subscription send failed result=%d",
                      sendResult);
                setConfigStatus("subscribe_send_failed");
                configTransportFailed = true;
            } else {
                setConfigStatus("subscription_sent");
            }
            publishConfigCounters();
        }
    }

    setServiceStatus("active");
    if (!imscompat::initializeServiceLifecycle(getRcsObject)) {
        ALOGE("IMS service lifecycle unavailable; continuing DPL-only");
    }
    if (!imscompat::initializeInitialRegistration(library, getRcsObject) &&
            imscompat::isInitialRegistrationEnabled()) {
        ALOGE("stock initial registration unavailable; continuing DPL-only");
    }
    unsigned configResponseWaitSeconds = 0;
    sig_atomic_t publishedConfigEvent = 0;
    bool configTimeoutReported = false;
    for (;;) {
        if (imscompat::isInitialRegistrationEnabled()) {
            // Never allow the old direct-MMTEL experiment and the stock
            // initial-registration producer to race each other.
            imscompat::shutdownServiceLifecycle();
            imscompat::reconcileInitialRegistration();
        } else {
            imscompat::shutdownInitialRegistration();
            imscompat::reconcileServiceLifecycle();
        }
        if (waitForDuration(kServiceHealthIntervalSeconds) != kSuccess) {
            imscompat::shutdownInitialRegistration();
            imscompat::shutdownServiceLifecycle();
            destroyConfigService(&configServiceHandle, removeConfigListener,
                                 destroyConfigServiceFn);
            setServiceStatus("clock_error");
            sleep(kServiceFailureBackoffSeconds);
            return kClockError;
        }

        if (configServiceHandle != nullptr) {
            configResponseWaitSeconds += kServiceHealthIntervalSeconds;
            const sig_atomic_t event = gConfigLastEvent;
            if (event != publishedConfigEvent) {
                publishedConfigEvent = event;
                publishConfigCounters();
                if (event == 1) {
                    setConfigStatus("request_sent");
                } else if (event == 2) {
                    setConfigStatus("response_success");
                } else if (event == 3) {
                    ALOGE("stock config subscription reported failure "
                          "requests=%d successes=%d failures=%d",
                          static_cast<int>(gConfigRequestCount),
                          static_cast<int>(gConfigSuccessCount),
                          static_cast<int>(gConfigFailureCount));
                    setConfigStatus("response_failure");
                }
            }
            if (gConfigInvalidContextCount != 0) {
                ALOGE("stock config callback used an invalid context count=%d",
                      static_cast<int>(gConfigInvalidContextCount));
                setConfigStatus("callback_error");
            } else if (gConfigSuccessCount == 0 &&
                       gConfigFailureCount == 0 &&
                       !configTransportFailed &&
                       configResponseWaitSeconds >=
                               kConfigResponseTimeoutSeconds &&
                       !configTimeoutReported) {
                configTimeoutReported = true;
                ALOGE("stock config response timed out after %u seconds "
                      "requests=%d",
                      configResponseWaitSeconds,
                      static_cast<int>(gConfigRequestCount));
                setConfigStatus("response_timeout");
            }
        }

        void* const internalObject = getRcsInitObject();
        const int publicState = isStarted();
        const int internalState = internalObject == nullptr
                ? -1 : rcsInitIsStarted(internalObject);
        if (publicState != 1 || internalState != 1) {
            ALOGE("DPL lost readiness public=%d internal=%d", publicState,
                  internalState);
            imscompat::shutdownInitialRegistration();
            imscompat::shutdownServiceLifecycle();
            destroyConfigService(&configServiceHandle, removeConfigListener,
                                 destroyConfigServiceFn);
            setServiceStatus("lost_readiness");
            sleep(kServiceFailureBackoffSeconds);
            return kStartError;
        }
    }
#endif

    printf("DPL_READY duration_seconds=%u\n", duration);
    fflush(stdout);
    int result = waitForDuration(duration);

    const int publicDuring = isStarted();
    void* const internalObject = getRcsInitObject();
    const int internalDuring = internalObject == nullptr
            ? -1 : rcsInitIsStarted(internalObject);
    const int initializeCalls = traceGetCallCount();
    const int initializeResult = traceGetLastResult();
    const int initializeResolveFailures = traceGetResolveFailureCount();
    const int socketSendCalls = traceGetSendCallCount();
    const int socketSendFailures = traceGetSendFailureCount();
    const int socketReceiveCalls = traceGetReceiveCallCount();
    const int socketReceiveFailures = traceGetReceiveFailureCount();
    const int scopeFixAttempts = traceGetScopeFixAttemptCount();
    const int scopeFixApplied = traceGetScopeFixAppliedCount();
    const int scopeFixFailures = traceGetScopeFixFailureCount();
    const int policyRuleApplied = traceGetPolicyRuleAppliedCount();
    const int policyRuleFailures = traceGetPolicyRuleFailureCount();
    printf("DPL_OBSERVE wait_result=%d public_is_started=%d "
           "internal_object=%p internal_is_started=%d status_events=%d "
           "last_status=%d timers_started=%d timers_fired=%d "
           "timers_stopped=%d timer_failures=%d qp_dpl_initialize_calls=%d "
           "qp_dpl_initialize_result=%d qp_dpl_resolve_failures=%d\n",
           result, publicDuring, internalObject, internalDuring,
           static_cast<int>(gStatusEventCount),
           static_cast<int>(gLastStatus), static_cast<int>(gTimerStartCount),
           static_cast<int>(gTimerFireCount),
           static_cast<int>(gTimerStopCount),
           static_cast<int>(gTimerFailureCount), initializeCalls,
           initializeResult, initializeResolveFailures);
    printf("DPL_SOCKET_SUMMARY send_calls=%d send_failures=%d "
           "receive_calls=%d receive_failures=%d trace_record_limit=%d "
           "scope_fix_attempts=%d scope_fix_applied=%d "
           "scope_fix_failures=%d policy_rule_applied=%d "
           "policy_rule_failures=%d\n",
           socketSendCalls, socketSendFailures, socketReceiveCalls,
           socketReceiveFailures, 64, scopeFixAttempts, scopeFixApplied,
           scopeFixFailures, policyRuleApplied, policyRuleFailures);
    printf("DPL_STOP_BEGIN expected_stock_process_exit=255\n");
    fflush(stdout);

    const int stopResult = stopService();
    cleanupTimers();
    const int after = isStarted();
    printf("DPL_STOP_RETURNED public_is_started_before_stop=%d stop_result=%d "
           "is_started_after_stop=%d status_events=%d last_status=%d "
           "timers_started=%d timers_fired=%d timers_stopped=%d "
           "timer_failures=%d\n",
           publicDuring, stopResult, after, static_cast<int>(gStatusEventCount),
           static_cast<int>(gLastStatus), static_cast<int>(gTimerStartCount),
           static_cast<int>(gTimerFireCount),
           static_cast<int>(gTimerStopCount),
           static_cast<int>(gTimerFailureCount));
    fflush(stdout);

    gTimerExpired = nullptr;
    dlclose(library);
    alarm(0);

    if (result != kSuccess) return result;
    if (stopResult != 1 || after != 0 || gTimerFailureCount != 0) {
        fprintf(stderr,
                "ERROR stage=stop class=cleanup_failure stop_result=%d "
                "is_started=%d timer_failures=%d\n",
                stopResult, after, static_cast<int>(gTimerFailureCount));
        return kStartError;
    }
    return kSuccess;
}
