/* IMS DPL transport compatibility and readiness counters. */

#include <dlfcn.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>


extern "C" int qpDplInitialize(void* parameter);
extern "C" int qpDplNetOpen(void* profile);
extern "C" int qpDplNetRegIPCPort(unsigned port, void* callback,
                                   void* profile, void* context,
                                   unsigned messageType);
extern "C" int qpDplNetSendData(void* profile, const void* data,
                                 unsigned short length);
extern "C" void* qpDcmCreateProfile(void* config, void* callback,
                                     void* profileId);
extern "C" int qpDcmGetServingSystem(void* profile, void* system);
extern "C" int qpDcmEstablishPDPConnection(void* profile);
extern "C" int qpDcmDeleteProfile(void* profile);

namespace {

typedef int (*QpDplInitializeFn)(void* parameter);
typedef int (*QpDplNetOpenFn)(void* profile);
typedef int (*QpDplNetRegIPCPortFn)(unsigned port, void* callback,
                                    void* profile, void* context,
                                    unsigned messageType);
typedef int (*QpDplNetSendDataFn)(void* profile, const void* data,
                                  unsigned short length);
typedef int (*QpDplNetGetStatusFn)(void* profile);
typedef int (*QpDplNetIsProfileOwnerFn)(void* profile);
typedef void (*DcmCallbackFn)(int message, void* profile, void* data);
typedef void* (*DcmCreateFn)(void* config, void* callback, void* profileId);
typedef int (*DcmGetSystemFn)(void* profile, void* system);
typedef int (*DcmEstablishFn)(void* profile);
typedef int (*DcmDeleteFn)(void* profile);
typedef ssize_t (*SendToFn)(int, const void*, size_t, int,
                            const struct sockaddr*, socklen_t);
#if defined(__ANDROID__)
typedef const struct sockaddr RecvFromAddress;
#else
typedef struct sockaddr RecvFromAddress;
#endif
typedef ssize_t (*RecvFromFn)(int, void*, size_t, int, RecvFromAddress*,
                              socklen_t*);

constexpr char kStockDplLibrary[] =
        "/system/lib/radio-su6-73/lib-imsdpl.so";

volatile int gCallCount = 0;
volatile int gLastResult = -9999;
volatile int gResolveFailureCount = 0;
volatile int gSendCallCount = 0;
volatile int gSendFailureCount = 0;
volatile int gReceiveCallCount = 0;
volatile int gReceiveFailureCount = 0;
volatile int gScopeFixAttemptCount = 0;
volatile int gScopeFixAppliedCount = 0;
volatile int gScopeFixFailureCount = 0;
volatile int gPolicyRuleAppliedCount = 0;
volatile int gPolicyRuleFailureCount = 0;
volatile int gNetOpenCallCount = 0;
volatile int gNetOpenLastResult = -9999;
volatile int gNetRegCallCount = 0;
volatile int gNetRegLastResult = -9999;
volatile int gNetSendDataCallCount = 0;
volatile int gNetSendDataLastResult = -9999;
volatile int gNetSendDataLastErrno = 0;
volatile int gNetSendDataLastResolved = 0;
volatile int gNetSendDataLastMessageKind = 0;
volatile int gNetSendDataLastLength = 0;
volatile int gNetSendDataLastStatusBefore = -9999;
volatile int gNetSendDataLastStatusAfter = -9999;
volatile int gNetSendDataLastProfileOwner = -1;
volatile int gNetSendDataLastConnectionPresent = 0;
volatile int gNetSendDataLastConnectionState = -1;
volatile int gNetSendDataLastSocketSendDelta = 0;
volatile int gDcmCreateCallCount = 0;
volatile int gDcmCreateFailureCount = 0;
volatile int gDcmGetCallCount = 0;
volatile int gDcmGetFailureCount = 0;
volatile int gDcmEstablishCallCount = 0;
volatile int gDcmEstablishFailureCount = 0;
volatile int gDcmCallbackCount = 0;
volatile int gDcmCallbackLookupFailureCount = 0;
volatile int gDcmLastMessage = -1;
pthread_mutex_t gPolicyRuleLock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t gDcmCallbackLock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t gDcmCreateLock = PTHREAD_MUTEX_INITIALIZER;
unsigned gPolicyRuleInterfaceIndex = 0;
pthread_once_t gResolveOnce = PTHREAD_ONCE_INIT;
pthread_once_t gSocketResolveOnce = PTHREAD_ONCE_INIT;
pthread_once_t gDplApiResolveOnce = PTHREAD_ONCE_INIT;
void* gStockDplHandle = nullptr;
QpDplInitializeFn gRealInitialize = nullptr;
QpDplNetOpenFn gRealNetOpen = nullptr;
QpDplNetRegIPCPortFn gRealNetRegIPCPort = nullptr;
QpDplNetSendDataFn gRealNetSendData = nullptr;
QpDplNetGetStatusFn gRealNetGetStatus = nullptr;
QpDplNetIsProfileOwnerFn gRealNetIsProfileOwner = nullptr;
DcmCreateFn gRealDcmCreate = nullptr;
DcmGetSystemFn gRealDcmGetSystem = nullptr;
DcmEstablishFn gRealDcmEstablish = nullptr;
DcmDeleteFn gRealDcmDelete = nullptr;
SendToFn gRealSendTo = nullptr;
RecvFromFn gRealRecvFrom = nullptr;
char gResolveError[256] = "not_resolved";
char gSocketResolveError[256] = "not_resolved";

constexpr unsigned kRouteTableOffset = 1000;
constexpr char kPolicyRulePriority[] = "13900";
constexpr unsigned kChildWaitAttempts = 100;
constexpr useconds_t kChildWaitDelayUs = 10 * 1000;
constexpr char kHttpsSubscriptionMessage[] = "Subscribe https status";
constexpr int kDcmCallbackSlots = 8;

struct DcmCallbackSlot {
    void* profile;
    DcmCallbackFn callback;
};

DcmCallbackSlot gDcmCallbacks[kDcmCallbackSlots] = {};
DcmCallbackFn gPendingDcmCallback = nullptr;

struct NetSendSnapshot {
    int status;
    int owner;
    int connectionPresent;
    int connectionState;
};

NetSendSnapshot inspectNetSendProfile(void* profile) {
    NetSendSnapshot snapshot = {-9999, -1, 0, -1};
    if (profile == nullptr) return snapshot;
    if (gRealNetGetStatus != nullptr) {
        snapshot.status = gRealNetGetStatus(profile);
    }
    if (gRealNetIsProfileOwner != nullptr) {
        snapshot.owner = gRealNetIsProfileOwner(profile);
    }
    void* const connection = *reinterpret_cast<void**>(profile);
    snapshot.connectionPresent = connection == nullptr ? 0 : 1;
    if (connection != nullptr) {
        snapshot.connectionState = *reinterpret_cast<int*>(
                static_cast<unsigned char*>(connection) + 8);
    }
    return snapshot;
}

bool isValidInterfaceName(const char* name) {
    if (name == nullptr || name[0] == '\0') return false;
    size_t length = 0;
    for (; name[length] != '\0' && length < IFNAMSIZ; ++length) {
        const char value = name[length];
        const bool valid = (value >= 'a' && value <= 'z') ||
                (value >= 'A' && value <= 'Z') ||
                (value >= '0' && value <= '9') || value == '_' ||
                value == '-' || value == '.';
        if (!valid) return false;
    }
    return length > 0 && length < IFNAMSIZ;
}

int waitForChild(pid_t child) {
    for (unsigned attempt = 0; attempt < kChildWaitAttempts; ++attempt) {
        int status = 0;
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        if (result < 0 && errno != EINTR) return -1;
        usleep(kChildWaitDelayUs);
    }
    if (kill(child, SIGKILL) != 0 && errno != ESRCH) {
        fprintf(stderr,
                "DPL_POLICY_RULE class=kill_failure pid=%d errno=%d\n",
                child, errno);
    }
    while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
    return -1;
}

int runIpRule(bool add, const char* interfaceName, unsigned table) {
    char tableValue[16];
    const int tableLength = snprintf(tableValue, sizeof(tableValue), "%u",
                                     table);
    if (tableLength <= 0 ||
        static_cast<size_t>(tableLength) >= sizeof(tableValue)) {
        return -1;
    }

    const pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        if (add) {
            execl("/system/bin/ip", "ip", "-6", "rule", "add", "pref",
                  kPolicyRulePriority, "oif", interfaceName, "lookup",
                  tableValue, static_cast<char*>(nullptr));
        } else {
            execl("/system/bin/ip", "ip", "-6", "rule", "del", "pref",
                  kPolicyRulePriority, static_cast<char*>(nullptr));
        }
        _exit(127);
    }
    return waitForChild(child);
}

bool ensurePolicyRule(const char* interfaceName, unsigned interfaceIndex) {
    if (!isValidInterfaceName(interfaceName) || interfaceIndex == 0 ||
        interfaceIndex > 0xffffu) {
        return false;
    }

    pthread_mutex_lock(&gPolicyRuleLock);
    if (gPolicyRuleInterfaceIndex == interfaceIndex) {
        pthread_mutex_unlock(&gPolicyRuleLock);
        return true;
    }

    const unsigned table = kRouteTableOffset + interfaceIndex;
    // Priority 13900 is reserved by this device integration. Deletion is
    // intentionally best-effort so a clean boot does not look like failure.
    runIpRule(false, interfaceName, table);
    const int addResult = runIpRule(true, interfaceName, table);
    if (addResult == 0) {
        gPolicyRuleInterfaceIndex = interfaceIndex;
        __sync_add_and_fetch(&gPolicyRuleAppliedCount, 1);
    } else {
        __sync_add_and_fetch(&gPolicyRuleFailureCount, 1);
        fprintf(stderr,
                "DPL_POLICY_RULE class=add_failure interface=%s index=%u "
                "table=%u priority=%s exit=%d\n",
                interfaceName, interfaceIndex, table, kPolicyRulePriority,
                addResult);
    }
    fflush(stderr);
    pthread_mutex_unlock(&gPolicyRuleLock);
    return addResult == 0;
}

unsigned findBoundIpv6Interface(int descriptor, char* interfaceName,
                                size_t interfaceNameLength) {
    if (interfaceName == nullptr || interfaceNameLength == 0) return 0;
    interfaceName[0] = '\0';
    struct sockaddr_storage localStorage;
    memset(&localStorage, 0, sizeof(localStorage));
    socklen_t localLength = sizeof(localStorage);
    if (getsockname(descriptor,
                    reinterpret_cast<struct sockaddr*>(&localStorage),
                    &localLength) != 0 || localStorage.ss_family != AF_INET6 ||
        localLength < static_cast<socklen_t>(sizeof(struct sockaddr_in6))) {
        return 0;
    }

    const struct sockaddr_in6* localAddress =
            reinterpret_cast<const struct sockaddr_in6*>(&localStorage);
    struct ifaddrs* addresses = nullptr;
    if (getifaddrs(&addresses) != 0 || addresses == nullptr) return 0;

    unsigned interfaceIndex = 0;
    for (const struct ifaddrs* current = addresses; current != nullptr;
         current = current->ifa_next) {
        if (current->ifa_addr == nullptr || current->ifa_name == nullptr ||
            current->ifa_addr->sa_family != AF_INET6) {
            continue;
        }
        const struct sockaddr_in6* candidate =
                reinterpret_cast<const struct sockaddr_in6*>(
                        current->ifa_addr);
        if (memcmp(&candidate->sin6_addr, &localAddress->sin6_addr,
                   sizeof(candidate->sin6_addr)) == 0) {
            interfaceIndex = if_nametoindex(current->ifa_name);
            if (interfaceIndex != 0) {
                const size_t nameLength = strlen(current->ifa_name);
                if (nameLength == 0 || nameLength >= interfaceNameLength) {
                    interfaceIndex = 0;
                    break;
                }
                memcpy(interfaceName, current->ifa_name, nameLength + 1);
                break;
            }
        }
    }
    freeifaddrs(addresses);
    return interfaceIndex;
}

void resolveSocketFunctions() {
    dlerror();
    gRealSendTo = reinterpret_cast<SendToFn>(dlsym(RTLD_NEXT, "sendto"));
    const char* sendError = dlerror();
    if (sendError != nullptr || gRealSendTo == nullptr) {
        snprintf(gSocketResolveError, sizeof(gSocketResolveError),
                 "sendto: %s", sendError == nullptr ? "null" : sendError);
        gRealSendTo = nullptr;
        return;
    }

    dlerror();
    gRealRecvFrom =
            reinterpret_cast<RecvFromFn>(dlsym(RTLD_NEXT, "recvfrom"));
    const char* receiveError = dlerror();
    if (receiveError != nullptr || gRealRecvFrom == nullptr) {
        snprintf(gSocketResolveError, sizeof(gSocketResolveError),
                 "recvfrom: %s",
                 receiveError == nullptr ? "null" : receiveError);
        gRealRecvFrom = nullptr;
        return;
    }

    snprintf(gSocketResolveError, sizeof(gSocketResolveError), "none");
}

void resolveStockInitialize() {
    dlerror();
    gStockDplHandle = dlopen(kStockDplLibrary, RTLD_NOW | RTLD_LOCAL);
    const char* error = dlerror();
    if (gStockDplHandle == nullptr || error != nullptr) {
        snprintf(gResolveError, sizeof(gResolveError),
                 "dlopen(%s): %s", kStockDplLibrary,
                 error == nullptr ? "null_handle" : error);
        return;
    }

    dlerror();
    void* const symbol = dlsym(gStockDplHandle, "qpDplInitialize");
    error = dlerror();
    gRealInitialize = reinterpret_cast<QpDplInitializeFn>(symbol);
    if (error != nullptr || gRealInitialize == nullptr ||
        gRealInitialize == qpDplInitialize) {
        gRealInitialize = nullptr;
        snprintf(gResolveError, sizeof(gResolveError),
                 "dlsym(%s): %s", kStockDplLibrary,
                 error == nullptr ? "invalid_symbol" : error);
        return;
    }

    snprintf(gResolveError, sizeof(gResolveError), "none");
}

template <typename Function>
Function resolveStockDplFunction(const char* name, const void* wrapper) {
    const int onceResult = pthread_once(&gResolveOnce, resolveStockInitialize);
    if (onceResult != 0 || gStockDplHandle == nullptr) return nullptr;
    dlerror();
    void* const symbol = dlsym(gStockDplHandle, name);
    const char* const error = dlerror();
    if (error != nullptr || symbol == nullptr || symbol == wrapper) {
        fprintf(stderr,
                "DPL_API class=resolve_failure function=%s detail=%s\n",
                name, error == nullptr ? "invalid_symbol" : error);
        fflush(stderr);
        return nullptr;
    }
    return reinterpret_cast<Function>(symbol);
}

DcmCallbackFn findDcmCallback(void* profile) {
    DcmCallbackFn callback = nullptr;
    pthread_mutex_lock(&gDcmCallbackLock);
    for (int index = 0; index < kDcmCallbackSlots; ++index) {
        if (gDcmCallbacks[index].profile == profile) {
            callback = gDcmCallbacks[index].callback;
            break;
        }
    }
    if (callback == nullptr) callback = gPendingDcmCallback;
    pthread_mutex_unlock(&gDcmCallbackLock);
    return callback;
}

bool saveDcmCallback(void* profile, DcmCallbackFn callback) {
    if (profile == nullptr || callback == nullptr) return false;
    bool saved = false;
    pthread_mutex_lock(&gDcmCallbackLock);
    int freeIndex = -1;
    for (int index = 0; index < kDcmCallbackSlots; ++index) {
        if (gDcmCallbacks[index].profile == profile) {
            gDcmCallbacks[index].callback = callback;
            saved = true;
            break;
        }
        if (freeIndex < 0 && gDcmCallbacks[index].profile == nullptr) {
            freeIndex = index;
        }
    }
    if (!saved && freeIndex >= 0) {
        gDcmCallbacks[freeIndex].profile = profile;
        gDcmCallbacks[freeIndex].callback = callback;
        saved = true;
    }
    pthread_mutex_unlock(&gDcmCallbackLock);
    return saved;
}

void eraseDcmCallback(void* profile) {
    pthread_mutex_lock(&gDcmCallbackLock);
    for (int index = 0; index < kDcmCallbackSlots; ++index) {
        if (gDcmCallbacks[index].profile == profile) {
            gDcmCallbacks[index].profile = nullptr;
            gDcmCallbacks[index].callback = nullptr;
            break;
        }
    }
    pthread_mutex_unlock(&gDcmCallbackLock);
}

void traceDcmCallback(int message, void* profile, void* data) {
    const int savedErrno = errno;
    const int call = __sync_add_and_fetch(&gDcmCallbackCount, 1);
    __sync_lock_test_and_set(&gDcmLastMessage, message);
    DcmCallbackFn callback = findDcmCallback(profile);
    if (callback == nullptr) {
        __sync_add_and_fetch(&gDcmCallbackLookupFailureCount, 1);
        fprintf(stderr,
                "DPL_DCM stage=callback class=lookup_failure call=%d "
                "message=%d profile=%p data=%p\n",
                call, message, profile, data);
        fflush(stderr);
        errno = savedErrno;
        return;
    }
    errno = savedErrno;
    callback(message, profile, data);
}

void resolveStockDplApis() {
    gRealNetOpen = resolveStockDplFunction<QpDplNetOpenFn>(
            "qpDplNetOpen", reinterpret_cast<const void*>(qpDplNetOpen));
    gRealNetRegIPCPort = resolveStockDplFunction<QpDplNetRegIPCPortFn>(
            "qpDplNetRegIPCPort",
            reinterpret_cast<const void*>(qpDplNetRegIPCPort));
    gRealNetSendData = resolveStockDplFunction<QpDplNetSendDataFn>(
            "qpDplNetSendData",
            reinterpret_cast<const void*>(qpDplNetSendData));
    gRealNetGetStatus = resolveStockDplFunction<QpDplNetGetStatusFn>(
            "qpDplNetGetStatus", nullptr);
    gRealNetIsProfileOwner =
            resolveStockDplFunction<QpDplNetIsProfileOwnerFn>(
                    "qpDplNetIsDplConnProfileOwner", nullptr);
    gRealDcmCreate = resolveStockDplFunction<DcmCreateFn>(
            "qpDcmCreateProfile",
            reinterpret_cast<const void*>(qpDcmCreateProfile));
    gRealDcmGetSystem = resolveStockDplFunction<DcmGetSystemFn>(
            "qpDcmGetServingSystem",
            reinterpret_cast<const void*>(qpDcmGetServingSystem));
    gRealDcmEstablish = resolveStockDplFunction<DcmEstablishFn>(
            "qpDcmEstablishPDPConnection",
            reinterpret_cast<const void*>(qpDcmEstablishPDPConnection));
    gRealDcmDelete = resolveStockDplFunction<DcmDeleteFn>(
            "qpDcmDeleteProfile",
            reinterpret_cast<const void*>(qpDcmDeleteProfile));
}

}  // namespace

extern "C" void* qpDcmCreateProfile(void* config, void* callback,
                                     void* profileId) {
    pthread_once(&gDplApiResolveOnce, resolveStockDplApis);
    const int call = __sync_add_and_fetch(&gDcmCreateCallCount, 1);
    if (gRealDcmCreate == nullptr) {
        __sync_add_and_fetch(&gDcmCreateFailureCount, 1);
        fprintf(stderr,
                "DPL_DCM stage=create class=resolve_failure call=%d "
                "config=%p callback=%p profile_id=%p\n",
                call, config, callback, profileId);
        fflush(stderr);
        return nullptr;
    }
    DcmCallbackFn original = reinterpret_cast<DcmCallbackFn>(callback);
    pthread_mutex_lock(&gDcmCreateLock);
    pthread_mutex_lock(&gDcmCallbackLock);
    gPendingDcmCallback = original;
    pthread_mutex_unlock(&gDcmCallbackLock);
    void* const profile = gRealDcmCreate(
            config, original == nullptr
                    ? nullptr : reinterpret_cast<void*>(traceDcmCallback),
            profileId);
    const int savedErrno = errno;
    pthread_mutex_lock(&gDcmCallbackLock);
    gPendingDcmCallback = nullptr;
    pthread_mutex_unlock(&gDcmCallbackLock);
    pthread_mutex_unlock(&gDcmCreateLock);
    if (profile == nullptr || !saveDcmCallback(profile, original)) {
        __sync_add_and_fetch(&gDcmCreateFailureCount, 1);
        const char* const failure = profile == nullptr
                ? "stock_failure" : "callback_table_full";
        fprintf(stderr,
                "DPL_DCM stage=create class=%s call=%d config=%p "
                "callback=%p profile_id=%p profile=%p\n",
                failure, call, config, callback, profileId, profile);
        fflush(stderr);
        if (profile != nullptr && gRealDcmDelete != nullptr) {
            gRealDcmDelete(profile);
        }
        errno = savedErrno;
        return nullptr;
    }
    errno = savedErrno;
    return profile;
}

extern "C" int qpDcmGetServingSystem(void* profile, void* system) {
    pthread_once(&gDplApiResolveOnce, resolveStockDplApis);
    __sync_add_and_fetch(&gDcmGetCallCount, 1);
    const int result = gRealDcmGetSystem == nullptr
            ? -1 : gRealDcmGetSystem(profile, system);
    const int savedErrno = errno;
    if (result != 0) __sync_add_and_fetch(&gDcmGetFailureCount, 1);
    if (result != 0) {
        fprintf(stderr, "DPL_DCM stage=serving result=%d\n", result);
    }
    errno = savedErrno;
    return result;
}

extern "C" int qpDcmEstablishPDPConnection(void* profile) {
    pthread_once(&gDplApiResolveOnce, resolveStockDplApis);
    __sync_add_and_fetch(&gDcmEstablishCallCount, 1);
    const int result = gRealDcmEstablish == nullptr
            ? -1 : gRealDcmEstablish(profile);
    const int savedErrno = errno;
    if (result != 0) __sync_add_and_fetch(&gDcmEstablishFailureCount, 1);
    if (result != 0) {
        fprintf(stderr, "DPL_DCM stage=establish result=%d errno=%d\n",
                result, savedErrno);
    }
    errno = savedErrno;
    return result;
}

extern "C" int qpDcmDeleteProfile(void* profile) {
    pthread_once(&gDplApiResolveOnce, resolveStockDplApis);
    const int result = gRealDcmDelete == nullptr
            ? -1 : gRealDcmDelete(profile);
    const int savedErrno = errno;
    eraseDcmCallback(profile);
    if (result != 0) {
        fprintf(stderr,
                "DPL_DCM stage=delete class=%s profile=%p result=%d\n",
                gRealDcmDelete == nullptr ? "resolve_failure" : "failure",
                profile, result);
        fflush(stderr);
    }
    errno = savedErrno;
    return result;
}

extern "C" int qpDplInitialize(void* parameter) {
    const int onceResult = pthread_once(&gResolveOnce, resolveStockInitialize);
    if (onceResult != 0 || gRealInitialize == nullptr) {
        __sync_add_and_fetch(&gResolveFailureCount, 1);
        __sync_add_and_fetch(&gCallCount, 1);
        __sync_lock_test_and_set(&gLastResult, -1);
        fprintf(stderr,
                "DPL_INTERPOSE class=resolve_failure parameter=%p "
                "pthread_once_result=%d detail=%s\n",
                parameter, onceResult, gResolveError);
        fflush(stderr);
        return -1;
    }

    const int result = gRealInitialize(parameter);
    __sync_lock_test_and_set(&gLastResult, result);
    __sync_add_and_fetch(&gCallCount, 1);
    if (result != 0) {
        fprintf(stderr, "DPL_INTERPOSE class=failure result=%d\n", result);
    }
    return result;
}

extern "C" int qpDplNetOpen(void* profile) {
    pthread_once(&gDplApiResolveOnce, resolveStockDplApis);
    const int result = gRealNetOpen == nullptr ? -1 : gRealNetOpen(profile);
    __sync_lock_test_and_set(&gNetOpenLastResult, result);
    __sync_add_and_fetch(&gNetOpenCallCount, 1);
    return result;
}

extern "C" int qpDplNetRegIPCPort(unsigned port, void* callback,
                                   void* profile, void* context,
                                   unsigned messageType) {
    pthread_once(&gDplApiResolveOnce, resolveStockDplApis);
    const int result = gRealNetRegIPCPort == nullptr
            ? -1 : gRealNetRegIPCPort(port, callback, profile, context,
                                      messageType);
    __sync_lock_test_and_set(&gNetRegLastResult, result);
    __sync_add_and_fetch(&gNetRegCallCount, 1);
    return result;
}

extern "C" int qpDplNetSendData(void* profile, const void* data,
                                 unsigned short length) {
    pthread_once(&gDplApiResolveOnce, resolveStockDplApis);
    const bool isHttpsSubscription = data != nullptr &&
            length == sizeof(kHttpsSubscriptionMessage) - 1 &&
            memcmp(data, kHttpsSubscriptionMessage, length) == 0;
    const NetSendSnapshot before = inspectNetSendProfile(profile);
    const int socketSendsBefore = __sync_add_and_fetch(&gSendCallCount, 0);
    const int result = gRealNetSendData == nullptr
            ? -1 : gRealNetSendData(profile, data, length);
    const int savedErrno = errno;
    const int socketSendDelta =
            __sync_add_and_fetch(&gSendCallCount, 0) - socketSendsBefore;
    const NetSendSnapshot after = inspectNetSendProfile(profile);
    __sync_lock_test_and_set(&gNetSendDataLastResult, result);
    __sync_lock_test_and_set(&gNetSendDataLastErrno, savedErrno);
    __sync_lock_test_and_set(&gNetSendDataLastResolved,
                             gRealNetSendData == nullptr ? 0 : 1);
    __sync_lock_test_and_set(&gNetSendDataLastMessageKind,
                             isHttpsSubscription ? 1 : 0);
    __sync_lock_test_and_set(&gNetSendDataLastLength,
                             static_cast<int>(length));
    __sync_lock_test_and_set(&gNetSendDataLastStatusBefore, before.status);
    __sync_lock_test_and_set(&gNetSendDataLastStatusAfter, after.status);
    __sync_lock_test_and_set(&gNetSendDataLastProfileOwner, before.owner);
    __sync_lock_test_and_set(&gNetSendDataLastConnectionPresent,
                             before.connectionPresent);
    __sync_lock_test_and_set(&gNetSendDataLastConnectionState,
                             before.connectionState);
    __sync_lock_test_and_set(&gNetSendDataLastSocketSendDelta,
                             socketSendDelta);
    __sync_add_and_fetch(&gNetSendDataCallCount, 1);
    if (result != 0) {
        fprintf(stderr,
                "DPL_NET_SEND class=%s resolved=%d length=%u result=%d "
                "errno=%d status_before=%d status_after=%d owner=%d "
                "connection_present=%d connection_state=%d "
                "socket_send_delta=%d\n",
                isHttpsSubscription ? "https_subscription" : "other",
                gRealNetSendData == nullptr ? 0 : 1,
                static_cast<unsigned>(length), result, savedErrno,
                before.status, after.status, before.owner,
                before.connectionPresent, before.connectionState,
                socketSendDelta);
        fflush(stderr);
    }
    errno = savedErrno;
    return result;
}

extern "C" ssize_t sendto(int descriptor, const void* buffer, size_t length,
                           int flags, const struct sockaddr* destination,
                           socklen_t destinationLength) {
    const int onceResult = pthread_once(&gSocketResolveOnce,
                                       resolveSocketFunctions);
    if (onceResult != 0 || gRealSendTo == nullptr) {
        __sync_add_and_fetch(&gSendFailureCount, 1);
        fprintf(stderr,
                "DPL_SOCKET direction=send class=resolve_failure fd=%d "
                "pthread_once_result=%d detail=%s\n",
                descriptor, onceResult, gSocketResolveError);
        fflush(stderr);
        errno = ENOSYS;
        return -1;
    }
    struct sockaddr_in6 correctedDestination;
    const struct sockaddr* effectiveDestination = destination;
    socklen_t effectiveDestinationLength = destinationLength;
    if (destination != nullptr && destination->sa_family == AF_INET6 &&
        destinationLength >=
                static_cast<socklen_t>(sizeof(struct sockaddr_in6))) {
        const struct sockaddr_in6* ipv6Destination =
                reinterpret_cast<const struct sockaddr_in6*>(destination);
        if (IN6_IS_ADDR_LINKLOCAL(&ipv6Destination->sin6_addr) &&
            ipv6Destination->sin6_scope_id == 0) {
            __sync_add_and_fetch(&gScopeFixAttemptCount, 1);
            char interfaceName[IFNAMSIZ];
            const unsigned interfaceIndex =
                    findBoundIpv6Interface(descriptor, interfaceName,
                                           sizeof(interfaceName));
            if (interfaceIndex != 0 &&
                ensurePolicyRule(interfaceName, interfaceIndex)) {
                memcpy(&correctedDestination, ipv6Destination,
                       sizeof(correctedDestination));
                correctedDestination.sin6_scope_id = interfaceIndex;
                effectiveDestination = reinterpret_cast<const struct sockaddr*>(
                        &correctedDestination);
                effectiveDestinationLength = sizeof(correctedDestination);
                __sync_add_and_fetch(&gScopeFixAppliedCount, 1);
            } else {
                __sync_add_and_fetch(&gScopeFixFailureCount, 1);
                fprintf(stderr,
                        "DPL_SCOPE_FIX class=not_applied fd=%d "
                        "reason=%s\n",
                        descriptor, interfaceIndex == 0
                                ? "bound_ipv6_interface_not_found"
                                : "policy_rule_unavailable");
            }
            fflush(stderr);
        }
    }
    const ssize_t result = gRealSendTo(
            descriptor, buffer, length, flags, effectiveDestination,
            effectiveDestinationLength);
    const int savedErrno = errno;
    __sync_add_and_fetch(&gSendCallCount, 1);
    if (result < 0) __sync_add_and_fetch(&gSendFailureCount, 1);
    errno = savedErrno;
    return result;
}

// Old Bionic's Fortify header defines an extern-inline C++ function named
// recvfrom. Use a distinct source identifier while exporting the required ELF
// symbol so the interposer does not redefine that inline function.
extern "C" ssize_t imsDplTraceRecvFrom(
        int descriptor, void* buffer, size_t length, int flags,
        RecvFromAddress* source, socklen_t* sourceLength) __asm__("recvfrom");

extern "C" ssize_t imsDplTraceRecvFrom(
        int descriptor, void* buffer, size_t length, int flags,
        RecvFromAddress* source, socklen_t* sourceLength) {
    const int onceResult = pthread_once(&gSocketResolveOnce,
                                       resolveSocketFunctions);
    if (onceResult != 0 || gRealRecvFrom == nullptr) {
        __sync_add_and_fetch(&gReceiveFailureCount, 1);
        fprintf(stderr,
                "DPL_SOCKET direction=receive class=resolve_failure fd=%d "
                "pthread_once_result=%d detail=%s\n",
                descriptor, onceResult, gSocketResolveError);
        fflush(stderr);
        errno = ENOSYS;
        return -1;
    }
    const ssize_t result = gRealRecvFrom(descriptor, buffer, length, flags,
                                        source, sourceLength);
    const int savedErrno = errno;
    __sync_add_and_fetch(&gReceiveCallCount, 1);
    if (result < 0) __sync_add_and_fetch(&gReceiveFailureCount, 1);
    errno = savedErrno;
    return result;
}

extern "C" void imsDplTraceReset() {
    __sync_lock_test_and_set(&gCallCount, 0);
    __sync_lock_test_and_set(&gLastResult, -9999);
    __sync_lock_test_and_set(&gResolveFailureCount, 0);
    __sync_lock_test_and_set(&gSendCallCount, 0);
    __sync_lock_test_and_set(&gSendFailureCount, 0);
    __sync_lock_test_and_set(&gReceiveCallCount, 0);
    __sync_lock_test_and_set(&gReceiveFailureCount, 0);
    __sync_lock_test_and_set(&gScopeFixAttemptCount, 0);
    __sync_lock_test_and_set(&gScopeFixAppliedCount, 0);
    __sync_lock_test_and_set(&gScopeFixFailureCount, 0);
    __sync_lock_test_and_set(&gPolicyRuleAppliedCount, 0);
    __sync_lock_test_and_set(&gPolicyRuleFailureCount, 0);
    __sync_lock_test_and_set(&gNetOpenCallCount, 0);
    __sync_lock_test_and_set(&gNetOpenLastResult, -9999);
    __sync_lock_test_and_set(&gNetRegCallCount, 0);
    __sync_lock_test_and_set(&gNetRegLastResult, -9999);
    __sync_lock_test_and_set(&gNetSendDataCallCount, 0);
    __sync_lock_test_and_set(&gNetSendDataLastResult, -9999);
    __sync_lock_test_and_set(&gNetSendDataLastErrno, 0);
    __sync_lock_test_and_set(&gNetSendDataLastResolved, 0);
    __sync_lock_test_and_set(&gNetSendDataLastMessageKind, 0);
    __sync_lock_test_and_set(&gNetSendDataLastLength, 0);
    __sync_lock_test_and_set(&gNetSendDataLastStatusBefore, -9999);
    __sync_lock_test_and_set(&gNetSendDataLastStatusAfter, -9999);
    __sync_lock_test_and_set(&gNetSendDataLastProfileOwner, -1);
    __sync_lock_test_and_set(&gNetSendDataLastConnectionPresent, 0);
    __sync_lock_test_and_set(&gNetSendDataLastConnectionState, -1);
    __sync_lock_test_and_set(&gNetSendDataLastSocketSendDelta, 0);
    __sync_lock_test_and_set(&gDcmCreateCallCount, 0);
    __sync_lock_test_and_set(&gDcmCreateFailureCount, 0);
    __sync_lock_test_and_set(&gDcmGetCallCount, 0);
    __sync_lock_test_and_set(&gDcmGetFailureCount, 0);
    __sync_lock_test_and_set(&gDcmEstablishCallCount, 0);
    __sync_lock_test_and_set(&gDcmEstablishFailureCount, 0);
    __sync_lock_test_and_set(&gDcmCallbackCount, 0);
    __sync_lock_test_and_set(&gDcmCallbackLookupFailureCount, 0);
    __sync_lock_test_and_set(&gDcmLastMessage, -1);
}

extern "C" int imsDplTraceGetCallCount() {
    return __sync_add_and_fetch(&gCallCount, 0);
}

extern "C" int imsDplTraceGetLastResult() {
    return __sync_add_and_fetch(&gLastResult, 0);
}

extern "C" int imsDplTraceGetResolveFailureCount() {
    return __sync_add_and_fetch(&gResolveFailureCount, 0);
}

extern "C" int imsDplTraceGetSendCallCount() {
    return __sync_add_and_fetch(&gSendCallCount, 0);
}

extern "C" int imsDplTraceGetSendFailureCount() {
    return __sync_add_and_fetch(&gSendFailureCount, 0);
}

extern "C" int imsDplTraceGetReceiveCallCount() {
    return __sync_add_and_fetch(&gReceiveCallCount, 0);
}

extern "C" int imsDplTraceGetReceiveFailureCount() {
    return __sync_add_and_fetch(&gReceiveFailureCount, 0);
}

extern "C" int imsDplTraceGetScopeFixAttemptCount() {
    return __sync_add_and_fetch(&gScopeFixAttemptCount, 0);
}

extern "C" int imsDplTraceGetScopeFixAppliedCount() {
    return __sync_add_and_fetch(&gScopeFixAppliedCount, 0);
}

extern "C" int imsDplTraceGetScopeFixFailureCount() {
    return __sync_add_and_fetch(&gScopeFixFailureCount, 0);
}

extern "C" int imsDplTraceGetPolicyRuleAppliedCount() {
    return __sync_add_and_fetch(&gPolicyRuleAppliedCount, 0);
}

extern "C" int imsDplTraceGetPolicyRuleFailureCount() {
    return __sync_add_and_fetch(&gPolicyRuleFailureCount, 0);
}

extern "C" int imsDplTraceGetNetOpenCallCount() {
    return __sync_add_and_fetch(&gNetOpenCallCount, 0);
}

extern "C" int imsDplTraceGetNetOpenLastResult() {
    return __sync_add_and_fetch(&gNetOpenLastResult, 0);
}

extern "C" int imsDplTraceGetNetRegCallCount() {
    return __sync_add_and_fetch(&gNetRegCallCount, 0);
}

extern "C" int imsDplTraceGetNetRegLastResult() {
    return __sync_add_and_fetch(&gNetRegLastResult, 0);
}

extern "C" int imsDplTraceGetNetSendDataCallCount() {
    return __sync_add_and_fetch(&gNetSendDataCallCount, 0);
}

extern "C" int imsDplTraceGetNetSendDataLastResult() {
    return __sync_add_and_fetch(&gNetSendDataLastResult, 0);
}

extern "C" int imsDplTraceGetNetSendDataLastErrno() {
    return __sync_add_and_fetch(&gNetSendDataLastErrno, 0);
}

extern "C" int imsDplTraceGetNetSendDataLastResolved() {
    return __sync_add_and_fetch(&gNetSendDataLastResolved, 0);
}

extern "C" int imsDplTraceGetNetSendDataLastMessageKind() {
    return __sync_add_and_fetch(&gNetSendDataLastMessageKind, 0);
}

extern "C" int imsDplTraceGetNetSendDataLastLength() {
    return __sync_add_and_fetch(&gNetSendDataLastLength, 0);
}

extern "C" int imsDplTraceGetNetSendDataLastStatusBefore() {
    return __sync_add_and_fetch(&gNetSendDataLastStatusBefore, 0);
}

extern "C" int imsDplTraceGetNetSendDataLastStatusAfter() {
    return __sync_add_and_fetch(&gNetSendDataLastStatusAfter, 0);
}

extern "C" int imsDplTraceGetNetSendDataLastProfileOwner() {
    return __sync_add_and_fetch(&gNetSendDataLastProfileOwner, 0);
}

extern "C" int imsDplTraceGetNetSendDataLastConnectionPresent() {
    return __sync_add_and_fetch(&gNetSendDataLastConnectionPresent, 0);
}

extern "C" int imsDplTraceGetNetSendDataLastConnectionState() {
    return __sync_add_and_fetch(&gNetSendDataLastConnectionState, 0);
}

extern "C" int imsDplTraceGetNetSendDataLastSocketSendDelta() {
    return __sync_add_and_fetch(&gNetSendDataLastSocketSendDelta, 0);
}
