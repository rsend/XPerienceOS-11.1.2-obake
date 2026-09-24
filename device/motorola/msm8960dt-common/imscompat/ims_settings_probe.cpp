/*
 * Copyright (C) 2026 The XPerience Project
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

#define LOG_TAG "ims_settings_probe"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>

#include "ims_sms_format.h"

namespace {

constexpr char kImssLibrary[] =
        "/system/lib/radio-su6-73/lib-imss.so";
constexpr char kRequiredLibraryPrefix[] =
        "/system/lib/radio-su6-73/";
constexpr char kQmiServicesLibrary[] =
        "/system/lib/radio-su6-73/libqmiservices.so";
constexpr char kQmiClientLibrary[] =
        "/system/lib/radio-su6-73/libqmi_cci.so";
// A mutating command performs GET, SET, and verification GET. Each callback
// has its own eight-second deadline, so leave enough process-level time for
// all three bounded operations while still terminating a blocked library call.
constexpr unsigned kWatchdogSeconds = 45;
constexpr unsigned kCallbackTimeoutSeconds = 8;
constexpr size_t kMaximumResponseSize = 512;
constexpr unsigned kQmiTimeoutMilliseconds = 8000;
constexpr unsigned kBootstrapQmiTimeoutMilliseconds = 2000;
constexpr unsigned kQmiServiceDiscoveryAttempts = 20;
constexpr useconds_t kQmiServiceDiscoveryDelayMicroseconds = 250000;
constexpr unsigned kGetRegManagerConfigMessageId = 0x26;
constexpr size_t kRegManagerConfigResponseSize = 280;
constexpr size_t kImsTestModeValidOffset = 277;
constexpr size_t kImsTestModeValueOffset = 278;
constexpr size_t kMaximumBootstrapResponseSize = 1024;
constexpr int32_t kImsaIdlMajorVersion = 1;
constexpr int32_t kImsaIdlMinorVersion = 8;
constexpr int32_t kImsaIdlToolVersion = 6;
constexpr unsigned kImsaGetRegistrationStatusMessageId = 0x20;
constexpr unsigned kImsaGetServiceStatusMessageId = 0x21;
constexpr unsigned kImsaRegisterIndicationsMessageId = 0x22;
constexpr unsigned kImsaRegistrationStatusIndicationId = 0x23;
constexpr unsigned kImsaServiceStatusIndicationId = 0x24;
constexpr unsigned kImsaAdditionalStatusIndicationId = 0x25;
constexpr size_t kImsaRegistrationStatusResponseSize = 20;
constexpr size_t kImsaServiceStatusResponseSize = 72;
constexpr size_t kImsaRegisterIndicationsRequestSize = 6;
constexpr size_t kImsaRegisterIndicationsResponseSize = 8;
constexpr size_t kImsaRegistrationStatusIndicationSize = 12;
constexpr size_t kImsaServiceStatusIndicationSize = 64;
constexpr size_t kImsaAdditionalStatusIndicationSize = 144;
constexpr unsigned kMinimumWatchSeconds = 5;
constexpr unsigned kMaximumWatchSeconds = 30;
constexpr unsigned kWatchdogMarginSeconds = 40;
constexpr size_t kMaximumImsaIndications = 64;
constexpr size_t kMaximumImsaRawIndicationSize = 256;
constexpr unsigned kGetRegManagerReadOnlyConfigMessageId = 0x41;
constexpr size_t kRegManagerReadOnlyConfigResponseSize = 60;

struct BootstrapQuery {
    const char* name;
    unsigned messageId;
    size_t responseSize;
};

// Recovered from the exact SU6-7.3 imsqmidaemon request functions and checked
// against its matching IMS Settings service object. These are all GET
// operations issued during imsqmidaemon's configuration bootstrap; none has
// a request payload or mutates the modem. Do not infer pairings from numeric
// adjacency: client provisioning is specifically message 0x54, not 0x48.
const BootstrapQuery kBootstrapQueries[] = {
    {"SIP_CONFIG", 0x25, 112},
    {"CLIENT_PROVISIONING_CONFIG", 0x54, 24},
    {"REG_MGR_CONFIG", 0x26, 280},
    {"SMS_CONFIG", 0x27, 284},
    {"USER_CONFIG", 0x28, 276},
    {"VOIP_CONFIG", 0x29, 60},
    {"PRESENCE_CONFIG", 0x31, 236},
    {"QIPCALL_CONFIG", 0x37, 36},
    {"MEDIA_CONFIG", 0x34, 52},
    {"SIP_READ_ONLY_CONFIG", 0x39, 560},
    {"NETWORK_READ_ONLY_CONFIG", 0x3d, 56},
    {"VOIP_READ_ONLY_CONFIG", 0x3f, 24},
    {"USER_READ_ONLY_CONFIG", 0x40, 404},
    {"REG_MGR_READ_ONLY_CONFIG", 0x41, 60},
    {"RCS_AUTO_READ_ONLY_CONFIG", 0x42, 908},
    {"RCS_IMSCORE_AUTO_READ_ONLY_CONFIG", 0x43, 40},
};

enum ExitCode {
    kSuccess = 0,
    kUsageError = 2,
    kLibraryError = 3,
    kRegistrationError = 4,
    kRequestError = 5,
    kCallbackTimeout = 6,
    kModemError = 7,
    kMalformedResponse = 8,
    kPreservationError = 9,
};

struct QipcallConfig {
    uint8_t vtCallingEnabledValid;
    uint8_t vtCallingEnabled;
    uint8_t mobileDataEnabledValid;
    uint8_t mobileDataEnabled;
    uint8_t volteEnabledValid;
    uint8_t volteEnabled;
};

struct SettingsResponseHeader {
    uint32_t qmiResponseResult;
    uint32_t qmiResponseError;
    uint8_t settingsResponseValid;
    uint8_t reserved[3];
    uint32_t settingsResponse;
};

static_assert(sizeof(QipcallConfig) == 6,
              "QIPCALL public configuration must remain six bytes");
static_assert(sizeof(SettingsResponseHeader) == 16,
              "IMS Settings public response header must remain 16 bytes");

typedef void (*ResponseCallback)(void* clientHandle, void* context,
                                 int responseId, void* response,
                                 uint32_t responseSize);
typedef void (*IndicationCallback)(void* clientHandle, void* context,
                                   int indicationId, void* indication,
                                   uint32_t indicationSize);
typedef void (*ErrorCallback)(void* clientHandle, void* context, int error);

struct ClientCallbacks {
    ResponseCallback response;
    IndicationCallback indication;
    ErrorCallback error;
};

typedef int (*RegisterFn)(const ClientCallbacks* callbacks, void* context,
                          void** clientHandle);
typedef int (*DeregisterFn)(void* clientHandle);
typedef int (*GetQipcallConfigFn)(void* clientHandle);
typedef int (*SetQipcallConfigFn)(void* clientHandle,
                                  const QipcallConfig* config);
typedef int (*QmiClientSendMsgSyncFn)(void* clientHandle,
                                     unsigned messageId,
                                     const void* request,
                                     unsigned requestSize,
                                     void* response,
                                     unsigned responseSize,
                                     unsigned timeoutMilliseconds);
typedef void (*QmiClientIndicationCallback)(void* clientHandle,
                                            unsigned messageId,
                                            void* indicationBuffer,
                                            unsigned indicationBufferSize,
                                            void* callbackData);
typedef void (*QmiClientErrorCallback)(void* clientHandle, int error,
                                       void* callbackData);
typedef int (*QmiClientMessageDecodeFn)(void* clientHandle, int messageType,
                                       unsigned messageId,
                                       const void* source,
                                       unsigned sourceSize,
                                       void* destination,
                                       unsigned destinationSize);
typedef int (*QmiClientRegisterErrorCallbackFn)(
        void* clientHandle, QmiClientErrorCallback callback,
        void* callbackData);

struct QmiServiceInfo {
    unsigned info[5];
};

typedef void* (*GetServiceObjectFn)(int32_t majorVersion,
                                    int32_t minorVersion,
                                    int32_t toolVersion);
typedef int (*QmiClientGetServiceListFn)(void* serviceObject,
                                        QmiServiceInfo* serviceInfo,
                                        unsigned* numberOfEntries,
                                        unsigned* numberOfServices);
typedef int (*QmiClientInitFn)(const QmiServiceInfo* serviceInfo,
                               void* serviceObject,
                               QmiClientIndicationCallback indicationCallback,
                               void* indicationData, void* osParameters,
                               void** clientHandle);
typedef int (*QmiClientReleaseFn)(void* clientHandle);

struct Api {
    void* library;
    RegisterFn registerClient;
    DeregisterFn deregisterClient;
    GetQipcallConfigFn getQipcallConfig;
    SetQipcallConfigFn setQipcallConfig;
    QmiClientSendMsgSyncFn qmiClientSendMsgSync;
};

struct ImsaApi {
    void* servicesLibrary;
    void* clientLibrary;
    GetServiceObjectFn getServiceObject;
    QmiClientGetServiceListFn getServiceList;
    QmiClientInitFn clientInit;
    QmiClientSendMsgSyncFn sendSync;
    QmiClientMessageDecodeFn messageDecode;
    QmiClientRegisterErrorCallbackFn registerErrorCallback;
    QmiClientReleaseFn clientRelease;
};

struct ImsaIndicationEvent {
    uint64_t elapsedMilliseconds;
    unsigned messageId;
    int decodeResult;
    size_t rawSize;
    bool rawTruncated;
    size_t decodedSize;
    uint8_t raw[kMaximumImsaRawIndicationSize];
    uint8_t decoded[kImsaAdditionalStatusIndicationSize];
};

struct ImsaWatchState {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    QmiClientMessageDecodeFn messageDecode;
    timespec start;
    bool transportError;
    int transportErrorCode;
    bool clockError;
    int clockErrorCode;
    size_t eventCount;
    size_t droppedEvents;
    ImsaIndicationEvent events[kMaximumImsaIndications];
};

struct CallbackState {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool complete;
    bool transportError;
    int callbackId;
    int error;
    uint8_t response[kMaximumResponseSize];
    size_t responseSize;
};

void watchdogHandler(int) {
    static const char message[] =
            "ERROR watchdog timeout while waiting for modem/QMI\n";
    write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}

void responseCallback(void*, void* context, int responseId, void* response,
                      uint32_t responseSize) {
    CallbackState* state = static_cast<CallbackState*>(context);
    pthread_mutex_lock(&state->mutex);
    state->callbackId = responseId;
    state->responseSize =
            std::min(static_cast<size_t>(responseSize),
                     sizeof(state->response));
    if (response != nullptr && state->responseSize != 0) {
        memcpy(state->response, response, state->responseSize);
    }
    state->complete = true;
    pthread_cond_signal(&state->condition);
    pthread_mutex_unlock(&state->mutex);
}

void indicationCallback(void*, void*, int indicationId, void*, uint32_t size) {
    fprintf(stderr,
            "NOTICE IMS_SETTINGS indication callback_id=%d size=%u\n",
            indicationId, size);
}

void errorCallback(void*, void* context, int error) {
    CallbackState* state = static_cast<CallbackState*>(context);
    pthread_mutex_lock(&state->mutex);
    state->transportError = true;
    state->error = error;
    state->complete = true;
    pthread_cond_signal(&state->condition);
    pthread_mutex_unlock(&state->mutex);
}

bool addSeconds(timespec* deadline, unsigned seconds) {
    if (clock_gettime(CLOCK_REALTIME, deadline) != 0) {
        fprintf(stderr, "ERROR clock_gettime failed: %s\n", strerror(errno));
        return false;
    }
    deadline->tv_sec += seconds;
    return true;
}

void resetCallbackState(CallbackState* state) {
    pthread_mutex_lock(&state->mutex);
    state->complete = false;
    state->transportError = false;
    state->callbackId = -1;
    state->error = 0;
    state->responseSize = 0;
    memset(state->response, 0, sizeof(state->response));
    pthread_mutex_unlock(&state->mutex);
}

int waitForCallback(CallbackState* state) {
    timespec deadline = {};
    if (!addSeconds(&deadline, kCallbackTimeoutSeconds)) {
        return kCallbackTimeout;
    }

    pthread_mutex_lock(&state->mutex);
    while (!state->complete) {
        const int result =
                pthread_cond_timedwait(&state->condition, &state->mutex,
                                       &deadline);
        if (result == ETIMEDOUT) {
            pthread_mutex_unlock(&state->mutex);
            fprintf(stderr, "ERROR modem callback timed out after %u seconds\n",
                    kCallbackTimeoutSeconds);
            return kCallbackTimeout;
        }
        if (result != 0) {
            pthread_mutex_unlock(&state->mutex);
            fprintf(stderr, "ERROR callback wait failed: %s\n",
                    strerror(result));
            return kCallbackTimeout;
        }
    }

    const bool transportError = state->transportError;
    const int error = state->error;
    const int callbackId = state->callbackId;
    const size_t responseSize = state->responseSize;
    pthread_mutex_unlock(&state->mutex);

    if (transportError) {
        fprintf(stderr, "ERROR QMI transport callback error=%d\n", error);
        return kModemError;
    }
    fprintf(stderr, "RESULT callback id=%d size=%zu\n", callbackId,
            responseSize);
    return kSuccess;
}

template <typename Function>
bool loadSymbol(void* library, const char* name, Function* output) {
    dlerror();
    void* symbol = dlsym(library, name);
    const char* error = dlerror();
    if (error != nullptr || symbol == nullptr) {
        fprintf(stderr, "ERROR missing symbol %s: %s\n", name,
                error != nullptr ? error : "not found");
        return false;
    }
    *output = reinterpret_cast<Function>(symbol);
    return true;
}

bool verifyLoadedClosure() {
    FILE* maps = fopen("/proc/self/maps", "r");
    if (maps == nullptr) {
        fprintf(stderr, "ERROR cannot inspect /proc/self/maps: %s\n",
                strerror(errno));
        return false;
    }

    const char* required[] = {
        "lib-imss.so", "libidl.so", "libqmi_cci.so",
        "libqmi_client_qmux.so", "libqmiservices.so",
    };
    bool found[sizeof(required) / sizeof(required[0])] = {};
    bool wrongPath = false;
    char line[1024] = {};
    while (fgets(line, sizeof(line), maps) != nullptr) {
        for (size_t index = 0;
             index < sizeof(required) / sizeof(required[0]); ++index) {
            if (strstr(line, required[index]) == nullptr) continue;
            found[index] = true;
            if (strstr(line, kRequiredLibraryPrefix) == nullptr) {
                fprintf(stderr, "ERROR mismatched library mapping: %s", line);
                wrongPath = true;
            }
        }
    }
    fclose(maps);

    bool missing = false;
    for (size_t index = 0;
         index < sizeof(required) / sizeof(required[0]); ++index) {
        if (!found[index]) {
            fprintf(stderr, "ERROR required library was not loaded: %s\n",
                    required[index]);
            missing = true;
        }
    }
    return !missing && !wrongPath;
}

bool loadApi(Api* api) {
    memset(api, 0, sizeof(*api));
    api->library = dlopen(kImssLibrary, RTLD_NOW | RTLD_LOCAL);
    if (api->library == nullptr) {
        fprintf(stderr, "ERROR dlopen %s failed: %s\n", kImssLibrary,
                dlerror());
        return false;
    }

    const bool loaded =
            loadSymbol(api->library, "qrcs_ims_settings_register",
                       &api->registerClient) &&
            loadSymbol(api->library, "qrcs_ims_settings_deregister",
                       &api->deregisterClient) &&
            loadSymbol(api->library, "qrcs_ims_settings_get_qipcall_config",
                       &api->getQipcallConfig) &&
            loadSymbol(api->library, "qrcs_ims_settings_set_qipcall_config",
                       &api->setQipcallConfig) &&
            loadSymbol(api->library, "qmi_client_send_msg_sync",
                       &api->qmiClientSendMsgSync);
    if (!loaded || !verifyLoadedClosure()) {
        dlclose(api->library);
        memset(api, 0, sizeof(*api));
        return false;
    }
    return true;
}

bool verifyImsaClosure() {
    FILE* maps = fopen("/proc/self/maps", "r");
    if (maps == nullptr) {
        fprintf(stderr, "ERROR cannot inspect /proc/self/maps: %s\n",
                strerror(errno));
        return false;
    }

    const char* required[] = {
        "libqmiservices.so", "libqmi_cci.so", "libqmi_client_qmux.so",
    };
    bool found[sizeof(required) / sizeof(required[0])] = {};
    bool wrongPath = false;
    char line[1024] = {};
    while (fgets(line, sizeof(line), maps) != nullptr) {
        for (size_t index = 0;
             index < sizeof(required) / sizeof(required[0]); ++index) {
            if (strstr(line, required[index]) == nullptr) continue;
            found[index] = true;
            if (strstr(line, kRequiredLibraryPrefix) == nullptr) {
                fprintf(stderr, "ERROR mismatched IMSA library mapping: %s",
                        line);
                wrongPath = true;
            }
        }
    }
    fclose(maps);

    bool missing = false;
    for (size_t index = 0;
         index < sizeof(required) / sizeof(required[0]); ++index) {
        if (!found[index]) {
            fprintf(stderr, "ERROR required IMSA library was not loaded: %s\n",
                    required[index]);
            missing = true;
        }
    }
    return !missing && !wrongPath;
}

void closeImsaApi(ImsaApi* api) {
    if (api->clientLibrary != nullptr) dlclose(api->clientLibrary);
    if (api->servicesLibrary != nullptr) dlclose(api->servicesLibrary);
    memset(api, 0, sizeof(*api));
}

bool loadImsaApi(ImsaApi* api, bool wms = false) {
    memset(api, 0, sizeof(*api));
    api->servicesLibrary = dlopen(kQmiServicesLibrary, RTLD_NOW | RTLD_LOCAL);
    if (api->servicesLibrary == nullptr) {
        fprintf(stderr, "ERROR dlopen %s failed: %s\n", kQmiServicesLibrary,
                dlerror());
        return false;
    }
    api->clientLibrary = dlopen(kQmiClientLibrary, RTLD_NOW | RTLD_LOCAL);
    if (api->clientLibrary == nullptr) {
        fprintf(stderr, "ERROR dlopen %s failed: %s\n", kQmiClientLibrary,
                dlerror());
        closeImsaApi(api);
        return false;
    }

    const bool loaded =
            loadSymbol(api->servicesLibrary,
                       wms ? "wms_get_service_object_internal_v01" :
                             "imsa_get_service_object_internal_v01",
                       &api->getServiceObject) &&
            loadSymbol(api->clientLibrary, "qmi_client_get_service_list",
                       &api->getServiceList) &&
            loadSymbol(api->clientLibrary, "qmi_client_init",
                       &api->clientInit) &&
            loadSymbol(api->clientLibrary, "qmi_client_send_msg_sync",
                       &api->sendSync) &&
            loadSymbol(api->clientLibrary, "qmi_client_message_decode",
                       &api->messageDecode) &&
            loadSymbol(api->clientLibrary, "qmi_client_register_error_cb",
                       &api->registerErrorCallback) &&
            loadSymbol(api->clientLibrary, "qmi_client_release",
                       &api->clientRelease);
    if (!loaded || !verifyImsaClosure()) {
        closeImsaApi(api);
        return false;
    }
    return true;
}

uint16_t readUint16(const uint8_t* bytes, size_t offset) {
    uint16_t value = 0;
    memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

uint32_t readUint32(const uint8_t* bytes, size_t offset) {
    uint32_t value = 0;
    memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

size_t imsaIndicationDecodedSize(unsigned messageId) {
    switch (messageId) {
        case kImsaRegistrationStatusIndicationId:
            return kImsaRegistrationStatusIndicationSize;
        case kImsaServiceStatusIndicationId:
            return kImsaServiceStatusIndicationSize;
        case kImsaAdditionalStatusIndicationId:
            return kImsaAdditionalStatusIndicationSize;
        default:
            return 0;
    }
}

uint64_t elapsedMilliseconds(const timespec& start, const timespec& end) {
    int64_t seconds = static_cast<int64_t>(end.tv_sec) - start.tv_sec;
    int64_t nanoseconds =
            static_cast<int64_t>(end.tv_nsec) - start.tv_nsec;
    if (nanoseconds < 0) {
        --seconds;
        nanoseconds += 1000000000LL;
    }
    if (seconds < 0) return 0;
    return static_cast<uint64_t>(seconds) * 1000ULL +
            static_cast<uint64_t>(nanoseconds / 1000000LL);
}

void imsaIndicationCallback(void* clientHandle, unsigned messageId,
                            void* indicationBuffer,
                            unsigned indicationBufferSize,
                            void* callbackData) {
    ImsaWatchState* state = static_cast<ImsaWatchState*>(callbackData);
    if (state == nullptr) return;

    ImsaIndicationEvent event = {};
    event.messageId = messageId;
    event.rawSize = std::min(static_cast<size_t>(indicationBufferSize),
                             sizeof(event.raw));
    event.rawTruncated = indicationBufferSize > sizeof(event.raw);
    if (indicationBuffer != nullptr && event.rawSize != 0) {
        memcpy(event.raw, indicationBuffer, event.rawSize);
    }
    event.decodedSize = imsaIndicationDecodedSize(messageId);
    event.decodeResult = -1;
    if (event.decodedSize != 0 && indicationBuffer != nullptr &&
        state->messageDecode != nullptr) {
        // QMI_IDL_INDICATION is enum value 2 in the matching QCCI ABI.
        event.decodeResult = state->messageDecode(
                clientHandle, 2, messageId, indicationBuffer,
                indicationBufferSize, event.decoded, event.decodedSize);
    }
    timespec now = {};
    const int clockResult = clock_gettime(CLOCK_MONOTONIC, &now);
    const int clockErrorCode = clockResult == 0 ? 0 : errno;
    if (clockResult == 0) {
        event.elapsedMilliseconds = elapsedMilliseconds(state->start, now);
    }

    pthread_mutex_lock(&state->mutex);
    if (clockResult != 0) {
        state->clockError = true;
        state->clockErrorCode = clockErrorCode;
    }
    if (state->eventCount < kMaximumImsaIndications) {
        state->events[state->eventCount++] = event;
    } else {
        ++state->droppedEvents;
    }
    pthread_cond_signal(&state->condition);
    pthread_mutex_unlock(&state->mutex);
}

void imsaErrorCallback(void*, int error, void* callbackData) {
    ImsaWatchState* state = static_cast<ImsaWatchState*>(callbackData);
    if (state == nullptr) return;
    pthread_mutex_lock(&state->mutex);
    state->transportError = true;
    state->transportErrorCode = error;
    pthread_cond_signal(&state->condition);
    pthread_mutex_unlock(&state->mutex);
}

void printImsaRawResponse(const char* name, const uint8_t* response,
                          size_t responseSize) {
    for (size_t offset = 0; offset < responseSize; offset += 16) {
        printf("IMSA_RAW name=%s offset=%04zx:", name, offset);
        const size_t end = std::min(offset + 16, responseSize);
        for (size_t index = offset; index < end; ++index) {
            printf(" %02x", response[index]);
        }
        printf("\n");
    }
}

bool validateValidityByte(const char* name, uint8_t value) {
    if (value <= 1) return true;
    fprintf(stderr, "ERROR malformed IMSA validity field %s=%u\n", name,
            value);
    return false;
}

int validateImsaQmiResponse(const char* operation, const uint8_t* response,
                            size_t responseSize) {
    if (responseSize < 8) {
        fprintf(stderr, "ERROR malformed IMSA %s response: size=%zu\n",
                operation, responseSize);
        return kMalformedResponse;
    }
    const uint32_t qmiResult = readUint32(response, 0);
    const uint32_t qmiError = readUint32(response, 4);
    printf("IMSA %s qmi_result=%u qmi_error=%u response_size=%zu\n",
           operation, qmiResult, qmiError, responseSize);
    if (qmiResult != 0 || qmiError != 0) {
        fprintf(stderr,
                "ERROR modem rejected IMSA %s: result=%u error=%u\n",
                operation, qmiResult, qmiError);
        return kModemError;
    }
    return kSuccess;
}

int queryImsaRegistration(const ImsaApi& api, void* clientHandle) {
    uint8_t response[kImsaRegistrationStatusResponseSize] = {};
    const int transportResult = api.sendSync(
            clientHandle, kImsaGetRegistrationStatusMessageId, nullptr, 0,
            response, sizeof(response), kQmiTimeoutMilliseconds);
    if (transportResult != 0) {
        fprintf(stderr,
                "ERROR IMSA GET_REGISTRATION_STATUS transport failure: %d\n",
                transportResult);
        return kRequestError;
    }

    printImsaRawResponse("REGISTRATION_STATUS", response, sizeof(response));
    int result = validateImsaQmiResponse("REGISTRATION_STATUS", response,
                                         sizeof(response));
    if (result != kSuccess) return result;

    const uint8_t registeredValid = response[8];
    const uint8_t registered = response[9];
    const uint8_t failureCodeValid = response[10];
    const uint16_t failureCode = readUint16(response, 12);
    const uint8_t networkValid = response[14];
    const uint16_t network = readUint16(response, 16);
    printf("IMSA REGISTRATION registered_valid=%u registered=%u "
           "failure_code_valid=%u failure_code=%u "
           "network_valid=%u network=%u\n",
           registeredValid, registered, failureCodeValid, failureCode,
           networkValid, network);
    if (!validateValidityByte("registered_valid", registeredValid) ||
        !validateValidityByte("failure_code_valid", failureCodeValid) ||
        !validateValidityByte("network_valid", networkValid)) {
        return kMalformedResponse;
    }
    return kSuccess;
}

struct ImsaServiceField {
    const char* name;
    size_t validOffset;
    size_t valueOffset;
};

const ImsaServiceField kImsaServiceFields[] = {
    {"sms_status", 8, 12},
    {"voip_status", 16, 20},
    {"vt_status", 24, 28},
    {"ut_status", 32, 36},
    {"vs_status", 40, 44},
    {"sms_rat", 48, 52},
    {"voip_rat", 56, 60},
    {"vt_rat", 64, 68},
};

int queryImsaServices(const ImsaApi& api, void* clientHandle) {
    uint8_t response[kImsaServiceStatusResponseSize] = {};
    const int transportResult = api.sendSync(
            clientHandle, kImsaGetServiceStatusMessageId, nullptr, 0,
            response, sizeof(response), kQmiTimeoutMilliseconds);
    if (transportResult != 0) {
        fprintf(stderr,
                "ERROR IMSA GET_SERVICE_STATUS transport failure: %d\n",
                transportResult);
        return kRequestError;
    }

    printImsaRawResponse("SERVICE_STATUS", response, sizeof(response));
    int result = validateImsaQmiResponse("SERVICE_STATUS", response,
                                         sizeof(response));
    if (result != kSuccess) return result;

    bool malformed = false;
    for (size_t index = 0;
         index < sizeof(kImsaServiceFields) / sizeof(kImsaServiceFields[0]);
         ++index) {
        const ImsaServiceField& field = kImsaServiceFields[index];
        const uint8_t valid = response[field.validOffset];
        const uint32_t value = readUint32(response, field.valueOffset);
        printf("IMSA SERVICE name=%s valid=%u value=%u\n", field.name,
               valid, value);
        if (!validateValidityByte(field.name, valid)) malformed = true;
    }
    return malformed ? kMalformedResponse : kSuccess;
}

void printImsaEventRaw(const char* kind, const ImsaIndicationEvent& event,
                       const uint8_t* bytes, size_t size) {
    for (size_t offset = 0; offset < size; offset += 16) {
        printf("IMSA_EVENT_%s id=0x%02x elapsed_ms=%llu offset=%04zx:",
               kind, event.messageId,
               static_cast<unsigned long long>(event.elapsedMilliseconds),
               offset);
        const size_t end = std::min(offset + 16, size);
        for (size_t index = offset; index < end; ++index) {
            printf(" %02x", bytes[index]);
        }
        printf("\n");
    }
}

bool printImsaRegistrationIndication(const ImsaIndicationEvent& event) {
    if (event.decodeResult != 0 ||
        event.decodedSize != kImsaRegistrationStatusIndicationSize) {
        return false;
    }
    const uint8_t* value = event.decoded;
    const uint8_t failureCodeValid = value[0];
    const uint16_t failureCode = readUint16(value, 2);
    const uint8_t registeredValid = value[4];
    const uint8_t registered = value[5];
    const uint8_t networkValid = value[6];
    const uint16_t network = readUint16(value, 8);
    printf("IMSA_EVENT REGISTRATION elapsed_ms=%llu "
           "registered_valid=%u registered=%u "
           "failure_code_valid=%u failure_code=%u "
           "network_valid=%u network=%u\n",
           static_cast<unsigned long long>(event.elapsedMilliseconds),
           registeredValid, registered, failureCodeValid, failureCode,
           networkValid, network);
    return validateValidityByte("event_registered_valid", registeredValid) &&
            validateValidityByte("event_failure_code_valid",
                                 failureCodeValid) &&
            validateValidityByte("event_network_valid", networkValid);
}

bool printImsaServiceIndication(const ImsaIndicationEvent& event) {
    if (event.decodeResult != 0 ||
        event.decodedSize != kImsaServiceStatusIndicationSize) {
        return false;
    }
    bool validEvent = true;
    for (size_t index = 0;
         index < sizeof(kImsaServiceFields) / sizeof(kImsaServiceFields[0]);
         ++index) {
        const ImsaServiceField& field = kImsaServiceFields[index];
        const size_t validOffset = field.validOffset - 8;
        const size_t valueOffset = field.valueOffset - 8;
        const uint8_t valid = event.decoded[validOffset];
        const uint32_t value = readUint32(event.decoded, valueOffset);
        printf("IMSA_EVENT SERVICE elapsed_ms=%llu name=%s valid=%u "
               "value=%u\n",
               static_cast<unsigned long long>(event.elapsedMilliseconds),
               field.name, valid, value);
        if (!validateValidityByte(field.name, valid)) validEvent = false;
    }
    return validEvent;
}

int printImsaWatchEvents(const ImsaWatchState& state) {
    size_t decodeFailures = 0;
    bool malformed = false;
    for (size_t index = 0; index < state.eventCount; ++index) {
        const ImsaIndicationEvent& event = state.events[index];
        printf("IMSA_EVENT id=0x%02x elapsed_ms=%llu raw_size=%zu "
               "raw_truncated=%u decoded_size=%zu decode_result=%d\n",
               event.messageId,
               static_cast<unsigned long long>(event.elapsedMilliseconds),
               event.rawSize, event.rawTruncated ? 1u : 0u,
               event.decodedSize, event.decodeResult);
        printImsaEventRaw("RAW", event, event.raw, event.rawSize);
        if (event.rawTruncated) malformed = true;
        if (event.decodedSize == 0 || event.decodeResult != 0) {
            ++decodeFailures;
            malformed = true;
            continue;
        }
        printImsaEventRaw("DECODED", event, event.decoded,
                          event.decodedSize);
        if (event.messageId == kImsaRegistrationStatusIndicationId) {
            if (!printImsaRegistrationIndication(event)) malformed = true;
        } else if (event.messageId == kImsaServiceStatusIndicationId) {
            if (!printImsaServiceIndication(event)) malformed = true;
        }
    }
    printf("WATCH_SUMMARY indications=%zu dropped=%zu decode_failures=%zu "
           "transport_error=%u transport_error_code=%d "
           "clock_error=%u clock_error_code=%d\n",
           state.eventCount, state.droppedEvents, decodeFailures,
           state.transportError ? 1u : 0u, state.transportErrorCode,
           state.clockError ? 1u : 0u, state.clockErrorCode);
    if (state.transportError || state.clockError ||
        state.droppedEvents != 0) {
        return kRequestError;
    }
    return malformed ? kMalformedResponse : kSuccess;
}

int openImsaClient(ImsaApi* api,
                   QmiClientIndicationCallback indicationCallback,
                   void* callbackData, void** clientHandle, bool wms = false) {
    if (api == nullptr || clientHandle == nullptr) return kUsageError;
    *clientHandle = nullptr;
    if (!loadImsaApi(api, wms)) return kLibraryError;

    void* serviceObject = api->getServiceObject(
            kImsaIdlMajorVersion, wms ? 20 : kImsaIdlMinorVersion,
            kImsaIdlToolVersion);
    if (serviceObject == nullptr) {
        fprintf(stderr,
                "ERROR %s service object rejected IDL version %d.%d/tool %d\n",
                wms ? "WMS" : "IMSA",
                kImsaIdlMajorVersion, wms ? 20 : kImsaIdlMinorVersion,
                kImsaIdlToolVersion);
        closeImsaApi(api);
        return kLibraryError;
    }

    QmiServiceInfo serviceInfo[4] = {};
    unsigned numberOfEntries = 0;
    unsigned numberOfServices = 0;
    int discoveryResult = -1;
    for (unsigned attempt = 0; attempt < kQmiServiceDiscoveryAttempts;
         ++attempt) {
        numberOfEntries = sizeof(serviceInfo) / sizeof(serviceInfo[0]);
        numberOfServices = 0;
        memset(serviceInfo, 0, sizeof(serviceInfo));
        discoveryResult = api->getServiceList(
                serviceObject, serviceInfo, &numberOfEntries,
                &numberOfServices);
        if (discoveryResult == 0 && numberOfEntries != 0) break;
        usleep(kQmiServiceDiscoveryDelayMicroseconds);
    }
    if (discoveryResult != 0 || numberOfEntries == 0) {
        fprintf(stderr,
                "ERROR %s service unavailable after bounded discovery: "
                "result=%d services=%u\n",
                wms ? "WMS" : "IMSA", discoveryResult, numberOfServices);
        closeImsaApi(api);
        return kRegistrationError;
    }

    if (callbackData != nullptr &&
        indicationCallback == imsaIndicationCallback) {
        ImsaWatchState* watchState =
                static_cast<ImsaWatchState*>(callbackData);
        pthread_mutex_lock(&watchState->mutex);
        watchState->messageDecode = api->messageDecode;
        pthread_mutex_unlock(&watchState->mutex);
    }

    const int initResult = api->clientInit(
            &serviceInfo[0], serviceObject, indicationCallback, callbackData,
            nullptr, clientHandle);
    if (initResult != 0 || *clientHandle == nullptr) {
        fprintf(stderr, "ERROR %s client initialization failed: result=%d\n",
                wms ? "WMS" : "IMSA", initResult);
        closeImsaApi(api);
        return kRegistrationError;
    }
    fprintf(stderr, "RESULT %s client connected handle=%p services=%u\n",
            wms ? "WMS" : "IMSA", *clientHandle, numberOfServices);
    if (callbackData != nullptr) {
        const int errorCallbackResult = api->registerErrorCallback(
                *clientHandle, imsaErrorCallback, callbackData);
        if (errorCallbackResult != 0) {
            fprintf(stderr,
                    "ERROR IMSA error callback registration failed: %d\n",
                    errorCallbackResult);
            api->clientRelease(*clientHandle);
            *clientHandle = nullptr;
            closeImsaApi(api);
            return kRegistrationError;
        }
    }
    return kSuccess;
}

int getImsaState() {
    ImsaApi api = {};
    void* clientHandle = nullptr;
    int result = openImsaClient(&api, nullptr, nullptr, &clientHandle);
    if (result != kSuccess) return result;

    const int registrationResult = queryImsaRegistration(api, clientHandle);
    const int serviceResult = queryImsaServices(api, clientHandle);
    const int releaseResult = api.clientRelease(clientHandle);
    if (releaseResult != 0) {
        fprintf(stderr, "ERROR IMSA client release failed: %d\n",
                releaseResult);
    }
    closeImsaApi(&api);

    if (registrationResult != kSuccess) return registrationResult;
    if (serviceResult != kSuccess) return serviceResult;
    return releaseResult == 0 ? kSuccess : kRegistrationError;
}

int registerImsaIndications(const ImsaApi& api, void* clientHandle) {
    // Exact IDL 1.8 request layout: three optional uint8 validity/value
    // pairs for registration, service, and additional-status indications.
    const uint8_t request[kImsaRegisterIndicationsRequestSize] = {
        1, 1, 1, 1, 1, 1,
    };
    uint8_t response[kImsaRegisterIndicationsResponseSize] = {};
    const int transportResult = api.sendSync(
            clientHandle, kImsaRegisterIndicationsMessageId, request,
            sizeof(request), response, sizeof(response),
            kQmiTimeoutMilliseconds);
    if (transportResult != 0) {
        fprintf(stderr,
                "ERROR IMSA REGISTER_INDICATIONS transport failure: %d\n",
                transportResult);
        return kRequestError;
    }
    const uint32_t qmiResult = readUint32(response, 0);
    const uint32_t qmiError = readUint32(response, 4);
    printf("IMSA REGISTER_INDICATIONS qmi_result=%u qmi_error=%u "
           "response_size=%zu\n",
           qmiResult, qmiError, sizeof(response));
    if (qmiResult != 0 || qmiError != 0) {
        fprintf(stderr,
                "ERROR modem rejected IMSA indication registration: "
                "result=%u error=%u\n",
                qmiResult, qmiError);
        return kModemError;
    }
    return kSuccess;
}

int waitForImsaIndications(ImsaWatchState* state, unsigned seconds) {
    timespec deadline = {};
    if (!addSeconds(&deadline, seconds)) return kCallbackTimeout;

    pthread_mutex_lock(&state->mutex);
    while (!state->transportError) {
        const int result = pthread_cond_timedwait(
                &state->condition, &state->mutex, &deadline);
        if (result == ETIMEDOUT) break;
        if (result != 0) {
            pthread_mutex_unlock(&state->mutex);
            fprintf(stderr, "ERROR IMSA watch wait failed: %s\n",
                    strerror(result));
            return kCallbackTimeout;
        }
    }
    const bool transportError = state->transportError;
    const int transportErrorCode = state->transportErrorCode;
    pthread_mutex_unlock(&state->mutex);
    if (transportError) {
        fprintf(stderr, "ERROR IMSA service transport failure while watching: "
                "%d\n", transportErrorCode);
        return kRequestError;
    }
    return kSuccess;
}

int watchImsaDecision(unsigned seconds) {
    ImsaWatchState state = {};
    pthread_mutex_init(&state.mutex, nullptr);
    pthread_cond_init(&state.condition, nullptr);
    if (clock_gettime(CLOCK_MONOTONIC, &state.start) != 0) {
        fprintf(stderr, "ERROR IMSA watch monotonic clock failed: %s\n",
                strerror(errno));
        pthread_cond_destroy(&state.condition);
        pthread_mutex_destroy(&state.mutex);
        return kRequestError;
    }

    ImsaApi api = {};
    state.messageDecode = nullptr;
    void* clientHandle = nullptr;
    int result = openImsaClient(&api, imsaIndicationCallback, &state,
                                &clientHandle);
    if (result != kSuccess) {
        pthread_cond_destroy(&state.condition);
        pthread_mutex_destroy(&state.mutex);
        return result;
    }
    result = registerImsaIndications(api, clientHandle);
    if (result == kSuccess) {
        printf("WATCH_PHASE PRE\n");
        result = queryImsaRegistration(api, clientHandle);
    }
    if (result == kSuccess) {
        result = queryImsaServices(api, clientHandle);
    }
    if (result == kSuccess) {
        printf("WATCH_READY duration_seconds=%u max_events=%zu\n", seconds,
               kMaximumImsaIndications);
        result = waitForImsaIndications(&state, seconds);
    }
    if (result == kSuccess) {
        printf("WATCH_PHASE POST\n");
        result = queryImsaRegistration(api, clientHandle);
    }
    if (result == kSuccess) {
        result = queryImsaServices(api, clientHandle);
    }

    const int releaseResult = api.clientRelease(clientHandle);
    if (releaseResult != 0) {
        fprintf(stderr, "ERROR IMSA client release failed: %d\n",
                releaseResult);
        if (result == kSuccess) result = kRegistrationError;
    }
    closeImsaApi(&api);

    const int eventResult = printImsaWatchEvents(state);
    if (result == kSuccess && eventResult != kSuccess) result = eventResult;
    pthread_cond_destroy(&state.condition);
    pthread_mutex_destroy(&state.mutex);
    return result;
}

bool decodeQipcallResponse(const CallbackState& state, QipcallConfig* config) {
    // The SU6-7.3 public GET response has a 16-byte response header followed
    // by the six-byte qrcs_ims_settings_qipcall_config structure.
    constexpr size_t kConfigOffset = 16;
    if (state.responseSize < kConfigOffset + sizeof(*config)) {
        fprintf(stderr,
                "ERROR malformed GET response: got %zu bytes, need at least %zu\n",
                state.responseSize, kConfigOffset + sizeof(*config));
        return false;
    }
    memcpy(config, state.response + kConfigOffset, sizeof(*config));
    return true;
}

bool validateSettingsResponse(const CallbackState& state) {
    if (state.responseSize < sizeof(SettingsResponseHeader)) {
        fprintf(stderr,
                "ERROR malformed settings response: got %zu bytes, need %zu\n",
                state.responseSize, sizeof(SettingsResponseHeader));
        return false;
    }

    SettingsResponseHeader header = {};
    memcpy(&header, state.response, sizeof(header));
    fprintf(stderr,
            "RESULT modem qmi_result=%u qmi_error=%u settings_valid=%u "
            "settings_response=%u\n",
            header.qmiResponseResult, header.qmiResponseError,
            header.settingsResponseValid, header.settingsResponse);

    // QMI result 0 and error 0 mean success. When the optional IMS Settings
    // response is present, enum value 1 is IMS_SETTINGS_RSP_NO_ERR.
    if (header.qmiResponseResult != 0 || header.qmiResponseError != 0 ||
        (header.settingsResponseValid != 0 &&
         header.settingsResponse != 1)) {
        fprintf(stderr, "ERROR modem rejected the IMS Settings operation\n");
        return false;
    }
    return true;
}

void printConfig(const QipcallConfig& config) {
    printf("QIPCALL mobile_data_enabled_valid=%u\n",
           config.mobileDataEnabledValid);
    printf("QIPCALL mobile_data_enabled=%u\n", config.mobileDataEnabled);
    printf("QIPCALL vt_calling_enabled_valid=%u\n",
           config.vtCallingEnabledValid);
    printf("QIPCALL vt_calling_enabled=%u\n", config.vtCallingEnabled);
    printf("QIPCALL volte_enabled_valid=%u\n", config.volteEnabledValid);
    printf("QIPCALL volte_enabled=%u\n", config.volteEnabled);
    fflush(stdout);
}

int getRegistrationManagerConfig(const Api& api, void* clientHandle) {
    uint8_t response[kRegManagerConfigResponseSize] = {};
    const int transportResult = api.qmiClientSendMsgSync(
            clientHandle, kGetRegManagerConfigMessageId, nullptr, 0,
            response, sizeof(response), kQmiTimeoutMilliseconds);
    if (transportResult != 0) {
        fprintf(stderr,
                "ERROR GET_REG_MGR_CONFIG transport failure: %d\n",
                transportResult);
        return kRequestError;
    }

    uint32_t qmiResult = 0;
    uint32_t qmiError = 0;
    memcpy(&qmiResult, response, sizeof(qmiResult));
    memcpy(&qmiError, response + sizeof(qmiResult), sizeof(qmiError));
    printf("REG_MGR qmi_result=%u qmi_error=%u response_size=%zu\n",
           qmiResult, qmiError, sizeof(response));
    printf("REG_MGR ims_test_mode_valid=%u ims_test_mode_enabled=%u\n",
           response[kImsTestModeValidOffset],
           response[kImsTestModeValueOffset]);
    for (size_t offset = 0; offset < sizeof(response); offset += 16) {
        printf("REG_MGR_RAW %04zx:", offset);
        const size_t end = std::min(offset + 16, sizeof(response));
        for (size_t index = offset; index < end; ++index) {
            printf(" %02x", response[index]);
        }
        printf("\n");
    }
    fflush(stdout);

    if (qmiResult != 0 || qmiError != 0) {
        fprintf(stderr,
                "ERROR modem rejected GET_REG_MGR_CONFIG: result=%u "
                "error=%u\n",
                qmiResult, qmiError);
        return kModemError;
    }
    if (response[kImsTestModeValidOffset] > 1 ||
        response[kImsTestModeValueOffset] > 1) {
        fprintf(stderr,
                "ERROR malformed IMS test-mode fields: valid=%u value=%u\n",
                response[kImsTestModeValidOffset],
                response[kImsTestModeValueOffset]);
        return kMalformedResponse;
    }
    return kSuccess;
}

int getRegistrationManagerReadOnlyConfig(const Api& api,
                                         void* clientHandle) {
    uint8_t response[kRegManagerReadOnlyConfigResponseSize] = {};
    const int transportResult = api.qmiClientSendMsgSync(
            clientHandle, kGetRegManagerReadOnlyConfigMessageId, nullptr, 0,
            response, sizeof(response), kQmiTimeoutMilliseconds);
    if (transportResult != 0) {
        fprintf(stderr,
                "ERROR GET_REG_MGR_READ_ONLY_CONFIG transport failure: %d\n",
                transportResult);
        return kRequestError;
    }

    const uint32_t qmiResult = readUint32(response, 0);
    const uint32_t qmiError = readUint32(response, 4);
    printf("REG_MGR_RO qmi_result=%u qmi_error=%u response_size=%zu\n",
           qmiResult, qmiError, sizeof(response));
    for (size_t offset = 0; offset < sizeof(response); offset += 16) {
        printf("REG_MGR_RO_RAW %04zx:", offset);
        const size_t end = std::min(offset + 16, sizeof(response));
        for (size_t index = offset; index < end; ++index) {
            printf(" %02x", response[index]);
        }
        printf("\n");
    }
    if (qmiResult != 0 || qmiError != 0) {
        fprintf(stderr,
                "ERROR modem rejected GET_REG_MGR_READ_ONLY_CONFIG: "
                "result=%u error=%u\n",
                qmiResult, qmiError);
        return kModemError;
    }
    return kSuccess;
}

void printRawResponse(const char* name, const uint8_t* response,
                      size_t responseSize) {
    for (size_t offset = 0; offset < responseSize; offset += 16) {
        printf("BOOTSTRAP_RAW name=%s offset=%04zx:", name, offset);
        const size_t end = std::min(offset + 16, responseSize);
        for (size_t index = offset; index < end; ++index) {
            printf(" %02x", response[index]);
        }
        printf("\n");
    }
}

int auditBootstrap(const Api& api, void* clientHandle,
                   const char* selectedName) {
    uint8_t response[kMaximumBootstrapResponseSize] = {};
    for (size_t index = 0;
         index < sizeof(kBootstrapQueries) / sizeof(kBootstrapQueries[0]);
         ++index) {
        const BootstrapQuery& query = kBootstrapQueries[index];
        if (strcmp(query.name, selectedName) != 0) continue;
        if (query.responseSize > sizeof(response)) {
            fprintf(stderr,
                    "ERROR BOOTSTRAP name=%s response_size=%zu exceeds "
                    "buffer=%zu\n",
                    query.name, query.responseSize, sizeof(response));
            return kMalformedResponse;
        }

        memset(response, 0, sizeof(response));
        const int transportResult = api.qmiClientSendMsgSync(
                clientHandle, query.messageId, nullptr, 0, response,
                query.responseSize, kBootstrapQmiTimeoutMilliseconds);
        if (transportResult != 0) {
            printf("BOOTSTRAP name=%s id=0x%02x size=%zu transport=%d "
                   "status=TRANSPORT_ERROR\n",
                   query.name, query.messageId, query.responseSize,
                   transportResult);
            return kRequestError;
        }

        if (query.responseSize < 8) {
            printf("BOOTSTRAP name=%s id=0x%02x size=%zu transport=0 "
                   "status=MALFORMED\n",
                   query.name, query.messageId, query.responseSize);
            return kMalformedResponse;
        }

        uint32_t qmiResult = 0;
        uint32_t qmiError = 0;
        memcpy(&qmiResult, response, sizeof(qmiResult));
        memcpy(&qmiError, response + sizeof(qmiResult), sizeof(qmiError));
        const bool success = qmiResult == 0 && qmiError == 0;
        printf("BOOTSTRAP name=%s id=0x%02x size=%zu transport=0 "
               "qmi_result=%u qmi_error=%u status=%s\n",
               query.name, query.messageId, query.responseSize, qmiResult,
               qmiError, success ? "OK" : "MODEM_ERROR");
        printRawResponse(query.name, response, query.responseSize);
        fflush(stdout);
        return success ? kSuccess : kModemError;
    }

    fprintf(stderr, "ERROR unknown bootstrap query: %s\n", selectedName);
    fprintf(stderr, "VALID_BOOTSTRAP_QUERIES");
    for (size_t index = 0;
         index < sizeof(kBootstrapQueries) / sizeof(kBootstrapQueries[0]);
         ++index) {
        fprintf(stderr, " %s", kBootstrapQueries[index].name);
    }
    fprintf(stderr, "\n");
    return kUsageError;
}

int getConfig(const Api& api, void* clientHandle, CallbackState* state,
              QipcallConfig* config) {
    resetCallbackState(state);
    const int requestResult = api.getQipcallConfig(clientHandle);
    if (requestResult != 0) {
        fprintf(stderr, "ERROR GET_QIPCALL_CONFIG rejected immediately: %d\n",
                requestResult);
        return kRequestError;
    }
    int result = waitForCallback(state);
    if (result != kSuccess) return result;
    if (state->callbackId != 0) {
        fprintf(stderr, "ERROR unexpected GET_QIPCALL_CONFIG callback=%d\n", state->callbackId);
        return kMalformedResponse;
    }
    if (!validateSettingsResponse(*state)) return kModemError;
    if (!decodeQipcallResponse(*state, config)) return kMalformedResponse;
    printConfig(*config);
    return kSuccess;
}

int setVolte(const Api& api, void* clientHandle, CallbackState* state,
             bool enabled) {
    QipcallConfig update = {};
    update.volteEnabledValid = 1;
    update.volteEnabled = enabled ? 1 : 0;

    resetCallbackState(state);
    const int requestResult = api.setQipcallConfig(clientHandle, &update);
    if (requestResult != 0) {
        fprintf(stderr, "ERROR SET_QIPCALL_CONFIG rejected immediately: %d\n",
                requestResult);
        return kRequestError;
    }
    const int result = waitForCallback(state);
    if (result != kSuccess) return result;
    if (state->callbackId != 1) {
        fprintf(stderr, "ERROR unexpected SET_QIPCALL_CONFIG callback=%d\n", state->callbackId);
        return kMalformedResponse;
    }
    if (!validateSettingsResponse(*state)) return kModemError;
    printf("SET volte_enabled=%u callback_received=1\n",
           enabled ? 1u : 0u);
    fflush(stdout);
    return kSuccess;
}

int setCellularIms(const Api& api, void* clientHandle, CallbackState* state,
                   bool mobileDataEnabled, bool volteEnabled) {
    // This is one QMI transaction. VT remains untouched because its validity
    // flag is deliberately zero.
    QipcallConfig update = {};
    update.mobileDataEnabledValid = 1;
    update.mobileDataEnabled = mobileDataEnabled ? 1 : 0;
    update.volteEnabledValid = 1;
    update.volteEnabled = volteEnabled ? 1 : 0;

    resetCallbackState(state);
    const int requestResult = api.setQipcallConfig(clientHandle, &update);
    if (requestResult != 0) {
        fprintf(stderr, "ERROR SET_QIPCALL_CONFIG rejected immediately: %d\n",
                requestResult);
        return kRequestError;
    }
    const int result = waitForCallback(state);
    if (result != kSuccess) return result;
    if (!validateSettingsResponse(*state)) return kModemError;
    printf("SET mobile_data_enabled=%u volte_enabled=%u callback_received=1\n",
           mobileDataEnabled ? 1u : 0u, volteEnabled ? 1u : 0u);
    fflush(stdout);
    return kSuccess;
}

// The SMS experiment uses one raw operation per process. Do not combine its
// GET/SET/GET on the legacy raw client: that reuse already failed in captures.
struct SmsCodec {
    void* library;
    void* service;
    int (*length)(void*, int, uint16_t, uint32_t*);
    int (*encode)(void*, int, uint16_t, const void*, uint32_t,
                  void*, uint32_t, uint32_t*);
    int (*decode)(void*, int, uint16_t, const void*, uint32_t,
                  void*, uint32_t);
};

bool smsCodecLength(const SmsCodec& codec, int direction, uint16_t message,
                    uint32_t expected) {
    uint32_t actual = 0;
    const int result = codec.length(codec.service, direction, message, &actual);
    if (result == 0 && actual == expected) return true;
    fprintf(stderr, "ERROR SMS IDL layout message=0x%x direction=%d "
            "result=%d expected=%u actual=%u\n", message, direction,
            result, expected, actual);
    return false;
}

bool loadSmsCodec(SmsCodec* codec) {
    *codec = {};
    codec->library = dlopen(kQmiServicesLibrary, RTLD_NOW | RTLD_LOCAL);
    if (codec->library == nullptr) {
        fprintf(stderr, "ERROR SMS codec dlopen failed: %s\n", dlerror());
        return false;
    }
    GetServiceObjectFn getService = nullptr;
    const bool loaded =
            loadSymbol(codec->library, "imss_get_service_object_internal_v01", &getService) &&
            loadSymbol(codec->library, "qmi_idl_get_message_c_struct_len", &codec->length) &&
            loadSymbol(codec->library, "qmi_idl_message_encode", &codec->encode) &&
            loadSymbol(codec->library, "qmi_idl_message_decode", &codec->decode);
    if (loaded) codec->service = getService(1, 22, 6);
    Dl_info codecInfo = {};
    const bool correctCodec = loaded &&
            dladdr(reinterpret_cast<void*>(codec->encode), &codecInfo) != 0 &&
            codecInfo.dli_fname != nullptr &&
            strncmp(codecInfo.dli_fname, kRequiredLibraryPrefix,
                    sizeof(kRequiredLibraryPrefix) - 1) == 0;
    if (!correctCodec || codec->service == nullptr ||
            !smsCodecLength(*codec, 0, ims_sms::kSetMessage, sizeof(ims_sms::SetRequest)) ||
            !smsCodecLength(*codec, 1, ims_sms::kSetMessage, sizeof(ims_sms::ResponseHeader)) ||
            !smsCodecLength(*codec, 1, ims_sms::kGetMessage, sizeof(ims_sms::GetResponse))) {
        fprintf(stderr, "ERROR SMS codec is not the supported SU6-7.3 ABI/closure\n");
        dlclose(codec->library);
        *codec = {};
        return false;
    }
    return true;
}

bool smsCodecTests(const SmsCodec& codec) {
    using namespace ims_sms;
    for (int32_t format = k3gpp2; format <= k3gpp; ++format) {
        SetRequest request = {};
        if (!makeFormatOnly(format, &request)) return false;
        uint8_t wire[32] = {};
        uint32_t wireSize = 0;
        const uint8_t expected[] = {0x10, 1, 0, static_cast<uint8_t>(format)};
        if (codec.encode(codec.service, 0, kSetMessage, &request, sizeof(request),
                         wire, sizeof(wire), &wireSize) != 0 ||
                wireSize != sizeof(expected) || memcmp(wire, expected, sizeof(expected)) != 0) {
            fprintf(stderr, "ERROR SMS format-only encode fixture format=%d\n", format);
            return false;
        }
        SetRequest decoded = {};
        if (codec.decode(codec.service, 0, kSetMessage, wire, wireSize,
                         &decoded, sizeof(decoded)) != 0 || decoded.formatValid != 1 ||
                decoded.format != format || decoded.overIpValid || decoded.phoneContextValid) {
            fprintf(stderr, "ERROR SMS format-only round-trip fixture format=%d\n", format);
            return false;
        }
        if (codec.encode(codec.service, 0, kSetMessage, &request, sizeof(request),
                         wire, 3, &wireSize) == 0 ||
                codec.decode(codec.service, 0, kSetMessage, expected, 3,
                             &decoded, sizeof(decoded)) == 0) {
            fprintf(stderr, "ERROR SMS short-buffer/truncated-TLV fixture\n");
            return false;
        }
    }
    SetRequest invalid = {};
    if (makeFormatOnly(-1, &invalid) || makeFormatOnly(2, &invalid) ||
            makeFormatOnly(k3gpp, nullptr)) return false;

    // Mandatory QMI success, optional settings NO_ERR, format 3GPP2,
    // over-IP enabled, context "abc". These are synthetic, not modem data.
    const uint8_t good[] = {2,4,0,0,0,0,0, 0x10,1,0,1, 0x11,1,0,0,
                           0x12,1,0,1, 0x13,3,0,'a','b','c'};
    GetResponse response = {};
    if (codec.decode(codec.service, 1, kGetMessage, good, sizeof(good),
                     &response, sizeof(response)) != 0 ||
            validateGet(response) != kValid || response.format != k3gpp2 ||
            response.overIpValid != 1 || response.overIp != 1 ||
            response.phoneContextValid != 1 || strcmp(response.phoneContext, "abc") != 0) {
        fprintf(stderr, "ERROR SMS GET decode fixture\n");
        return false;
    }
    GetResponse bad = response;
    bad.formatValid = 0;
    if (validateGet(bad) != kMalformed) return false;
    bad = response; bad.formatValid = 2;
    if (validateGet(bad) != kMalformed) return false;
    bad = response; bad.format = 2;
    if (validateGet(bad) != kMalformed) return false;
    bad = response; bad.overIp = 2;
    if (validateGet(bad) != kMalformed) return false;
    bad = response; bad.overIpValid = 2;
    if (validateGet(bad) != kMalformed) return false;
    bad = response; bad.phoneContextValid = 2;
    if (validateGet(bad) != kMalformed) return false;
    bad = response; memset(bad.phoneContext, 'x', sizeof(bad.phoneContext));
    if (validateGet(bad) != kMalformed) return false;
    bad = response; bad.header.settingsValid = 2;
    if (validateGet(bad) != kMalformed) return false;
    bad = response; bad.header.settingsError = 2;
    if (validateGet(bad) != kModemFailure) return false;
    bad = response; bad.header.result = 1; bad.header.error = 3;
    if (validateGet(bad) != kModemFailure) return false;
    bad = response; bad.header.error = 3;
    if (validateGet(bad) != kModemFailure) return false;
    if (codec.decode(codec.service, 1, kGetMessage, good, sizeof(good) - 1,
                     &bad, sizeof(bad)) == 0) return false;
    ResponseHeader setResponse = {};
    if (codec.decode(codec.service, 1, kSetMessage, good, 11,
                     &setResponse, sizeof(setResponse)) != 0 ||
            validateHeader(setResponse) != kValid) return false;
    const uint8_t rejected[] = {2,4,0,1,0,3,0};
    if (codec.decode(codec.service, 1, kSetMessage, rejected, sizeof(rejected),
                     &setResponse, sizeof(setResponse)) != 0 ||
            validateHeader(setResponse) != kModemFailure) return false;
    const uint8_t settingsError[] = {2,4,0,0,0,0,0, 0x10,1,0,2};
    if (codec.decode(codec.service, 1, kSetMessage, settingsError, sizeof(settingsError),
                     &setResponse, sizeof(setResponse)) != 0 ||
            validateHeader(setResponse) != kModemFailure) return false;
    return true;
}

int checkSmsCodec() {
    // No lib-imss registration, QCCI client, or QMI transaction in this gate.
    SmsCodec codec = {};
    if (!loadSmsCodec(&codec)) return kLibraryError;
    const bool passed = smsCodecTests(codec);
    dlclose(codec.library);
    if (!passed) {
        fprintf(stderr, "ERROR SMS_SELFTEST failed; no modem operation allowed\n");
        return kMalformedResponse;
    }
    printf("SMS_SELFTEST status=PASS modem_requests=0 set_tlv_3gpp=10010001\n");
    return kSuccess;
}

int smsValidationResult(ims_sms::Validation result, const char* operation,
                        const ims_sms::ResponseHeader& header) {
    printf("SMS_RESULT operation=%s qmi_result=%u qmi_error=%u "
           "settings_valid=%u settings_error=%d\n", operation, header.result,
           header.error, header.settingsValid, header.settingsError);
    if (result == ims_sms::kValid) return kSuccess;
    fprintf(stderr, "ERROR SMS operation=%s failure=%s\n", operation,
            result == ims_sms::kMalformed ? "malformed_response" : "modem_rejection");
    return result == ims_sms::kMalformed ? kMalformedResponse : kModemError;
}

int getSmsConfig(const Api& api, void* clientHandle) {
    ims_sms::GetResponse response = {};
    response.header.result = UINT32_MAX;
    const int transport = api.qmiClientSendMsgSync(
            clientHandle, ims_sms::kGetMessage, nullptr, 0, &response,
            sizeof(response), kQmiTimeoutMilliseconds);
    if (transport != 0) {
        fprintf(stderr, "ERROR SMS GET transport=%d\n", transport);
        return kRequestError;
    }
    const int result = smsValidationResult(ims_sms::validateGet(response), "GET", response.header);
    if (result != kSuccess) return result;
    printf("SMS_CONFIG format_valid=%u format=%d format_name=%s "
           "over_ip_valid=%u over_ip=%u phone_context_valid=%u phone_context_hex=",
           response.formatValid, response.format,
           response.format == ims_sms::k3gpp ? "3gpp" : "3gpp2",
           response.overIpValid, response.overIp, response.phoneContextValid);
    if (response.phoneContextValid) {
        for (size_t i = 0; i < sizeof(response.phoneContext) && response.phoneContext[i]; ++i) {
            printf("%02x", static_cast<unsigned char>(response.phoneContext[i]));
        }
    }
    printf("\n");
    return kSuccess;
}

int setSmsFormat(const Api& api, void* clientHandle, int32_t format) {
    ims_sms::SetRequest request = {};
    if (!ims_sms::makeFormatOnly(format, &request)) return kUsageError;
    ims_sms::ResponseHeader response = {};
    response.result = UINT32_MAX;
    printf("SMS_SET_ATTEMPT format=%d persistent=1 automatic_retry=0\n", format);
    const int transport = api.qmiClientSendMsgSync(
            clientHandle, ims_sms::kSetMessage, &request, sizeof(request),
            &response, sizeof(response), kQmiTimeoutMilliseconds);
    if (transport != 0) {
        fprintf(stderr, "ERROR SMS SET transport=%d outcome=unknown; "
                "perform fresh GET before any retry or rollback\n", transport);
        return kRequestError;
    }
    const int result = smsValidationResult(ims_sms::validateHeader(response), "SET", response);
    if (result == kSuccess) {
        printf("SMS_SET_ACK format=%d verified=0 next=fresh_get_sms_and_wms\n", format);
    }
    return result;
}

int getWmsField(unsigned message) {
    // Service 5 (WMS), NOT IMS Settings message IDs. One query per process.
    ImsaApi api = {};
    void* client = nullptr;
    int result = openImsaClient(&api, nullptr, nullptr, &client, true);
    if (result != kSuccess) return result;
    uint8_t response[20] = {};
    const unsigned size = message == 0x48 ? 20 : 16;
    int (*getLength)(void*, int, uint16_t, uint32_t*) = nullptr;
    uint32_t decodedSize = 0;
    const bool layoutValid = loadSymbol(api.servicesLibrary,
            "qmi_idl_get_message_c_struct_len", &getLength) &&
            getLength(api.getServiceObject(1, 20, 6), 1, message, &decodedSize) == 0 &&
            decodedSize == size;
    const int transport = layoutValid ? api.sendSync(client, message, nullptr, 0,
            response, size, kQmiTimeoutMilliseconds) : -1;
    if (!layoutValid) {
        fprintf(stderr, "ERROR WMS layout mismatch message=0x%x size=%u expected=%u\n",
                message, decodedSize, size);
    }
    if (transport != 0) {
        fprintf(stderr, "ERROR WMS GET message=0x%x transport=%d\n", message, transport);
        result = kRequestError;
    } else {
        const uint32_t qmiResult = readUint32(response, 0);
        const uint32_t qmiError = readUint32(response, 4);
        printf("WMS_RESULT message=0x%x qmi_result=%u qmi_error=%u\n",
               message, qmiResult, qmiError);
        if (qmiResult || qmiError) {
            result = kModemError;
        } else if (response[8] > 1 ||
                   (message != 0x4a && response[10] > 1) ||
                   (message != 0x4a && response[8] && response[9] > 1)) {
            fprintf(stderr, "ERROR WMS malformed validity/boolean fields\n");
            result = kMalformedResponse;
        } else if (message == 0x48) {
            printf("WMS_TRANSPORT registered_valid=%u registered=%u info_valid=%u "
                   "transport_type=%u capability=%u\n", response[8], response[9],
                   response[10], readUint32(response, 12), readUint32(response, 16));
        } else if (message == 0x4a) {
            printf("WMS_REGISTRATION status_valid=%u status=%u\n",
                   response[8], readUint32(response, 12));
        } else {
            // Keep the mask numeric until its exact legacy semantics are verified.
            printf("WMS_READY supported_valid=%u supported=%u status_valid=%u status=%u\n",
                   response[8], response[9], response[10], readUint32(response, 12));
        }
    }
    const int release = api.clientRelease(client);
    if (release != 0) {
        fprintf(stderr, "ERROR WMS release=%d\n", release);
        if (result == kSuccess) result = kRegistrationError;
    }
    closeImsaApi(&api);
    return result;
}

bool isBooleanArgument(const char* value) {
    return value != nullptr &&
            (strcmp(value, "0") == 0 || strcmp(value, "1") == 0);
}

// Production bridge worker: idempotent and restricted to the VoLTE-valid/value
// pair. Each invocation gets its own client/process lifetime, so a timed-out
// callback cannot be mistaken for the next request's response.
int ensureVolte(const Api& api, void* handle, CallbackState* state, bool enabled) {
    QipcallConfig before = {};
    int result = getConfig(api, handle, state, &before);
    if (result != kSuccess) return result;
    if (before.volteEnabledValid != 1 || before.volteEnabled > 1
            || before.mobileDataEnabledValid > 1 || before.vtCallingEnabledValid > 1
            || (before.mobileDataEnabledValid && before.mobileDataEnabled > 1)
            || (before.vtCallingEnabledValid && before.vtCallingEnabled > 1)) {
        fprintf(stderr, "ERROR policy GET returned invalid QIPCALL booleans\n");
        return kMalformedResponse;
    }
    if (before.volteEnabled == static_cast<uint8_t>(enabled)) return kSuccess;
    result = setVolte(api, handle, state, enabled);
    if (result != kSuccess) return result;
    QipcallConfig after = {};
    result = getConfig(api, handle, state, &after);
    if (result != kSuccess) return result;
    if (after.mobileDataEnabledValid != before.mobileDataEnabledValid
            || (before.mobileDataEnabledValid && after.mobileDataEnabled != before.mobileDataEnabled)
            || after.vtCallingEnabledValid != before.vtCallingEnabledValid
            || (before.vtCallingEnabledValid && after.vtCallingEnabled != before.vtCallingEnabled)) {
        fprintf(stderr, "ERROR policy changed an unrelated QIPCALL field; no automatic retry\n");
        return kPreservationError;
    }
    if (after.volteEnabledValid != 1 || after.volteEnabled != static_cast<uint8_t>(enabled)) {
        fprintf(stderr, "ERROR policy VoLTE readback mismatch\n");
        return kModemError;
    }
    return kSuccess;
}

bool parseWatchSeconds(const char* value, unsigned* seconds) {
    if (value == nullptr || seconds == nullptr || value[0] == '\0') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' ||
        parsed < kMinimumWatchSeconds || parsed > kMaximumWatchSeconds) {
        return false;
    }
    *seconds = static_cast<unsigned>(parsed);
    return true;
}

void usage(const char* program) {
    fprintf(stderr,
            "SMS diagnostics and guarded manual format trial:\n"
            "  %s sms-format-selftest  # codec only; no modem requests\n"
            "  %s get-sms\n"
            "  %s get-wms-transport\n"
            "  %s get-wms-registration\n"
            "  %s get-wms-ready\n"
            "  %s set-sms-format 0|1 --ack-persistent-format-only\n"
            "SMS format: 0=3GPP2, 1=3GPP. Save a fresh GET and verify SIM/idle/"
            "WMS state BEFORE SET. SET ACK is not readback or delivery proof.\n",
            program, program, program, program, program, program);
    fprintf(stderr,
            "Usage:\n"
            "  %s get\n"
            "  %s ensure-volte 0|1\n"
            "  %s get-regmgr\n"
            "  %s get-imsa\n"
            "  %s snapshot\n"
            "  %s watch-decision <%u-%u seconds>\n"
            "  %s audit-bootstrap <query-name>\n"
            "  %s set-volte 0|1\n"
            "  %s restore-volte 0|1\n"
            "  %s set-cellular-ims <mobile-data 0|1> <volte 0|1>\n"
            "  %s restore-cellular-ims <mobile-data 0|1> <volte 0|1>\n"
            "\n"
            "Run with:\n"
            "  LD_LIBRARY_PATH=/system/lib/radio-su6-73:/system/lib:"
            "/system/vendor/lib %s ...\n",
            program, program, program, program, program, program,
            kMinimumWatchSeconds, kMaximumWatchSeconds, program, program,
            program, program, program, program);
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    signal(SIGALRM, watchdogHandler);

    const bool doSmsSelftest = argc == 2 && strcmp(argv[1], "sms-format-selftest") == 0;
    const bool doGetSms = argc == 2 && strcmp(argv[1], "get-sms") == 0;
    const bool doSetSms = argc == 4 && strcmp(argv[1], "set-sms-format") == 0 &&
            isBooleanArgument(argv[2]) &&
            strcmp(argv[3], "--ack-persistent-format-only") == 0;
    unsigned wmsMessage = 0;
    if (argc == 2 && strcmp(argv[1], "get-wms-transport") == 0) wmsMessage = 0x48;
    if (argc == 2 && strcmp(argv[1], "get-wms-registration") == 0) wmsMessage = 0x4a;
    if (argc == 2 && strcmp(argv[1], "get-wms-ready") == 0) wmsMessage = 0x5c;
    const bool doGet = argc == 2 && strcmp(argv[1], "get") == 0;
    const bool doEnsureVolte = argc == 3 && strcmp(argv[1], "ensure-volte") == 0
            && isBooleanArgument(argv[2]);
    const bool doGetRegManager =
            argc == 2 && strcmp(argv[1], "get-regmgr") == 0;
    const bool doGetImsa = argc == 2 && strcmp(argv[1], "get-imsa") == 0;
    const bool doSnapshot = argc == 2 && strcmp(argv[1], "snapshot") == 0;
    unsigned watchSeconds = 0;
    const bool doWatchDecision = argc == 3 &&
            strcmp(argv[1], "watch-decision") == 0 &&
            parseWatchSeconds(argv[2], &watchSeconds);
    const bool doAuditBootstrap =
            argc == 3 && strcmp(argv[1], "audit-bootstrap") == 0;
    const bool doSetVolte = argc == 3 &&
            (strcmp(argv[1], "set-volte") == 0 ||
             strcmp(argv[1], "restore-volte") == 0) &&
            isBooleanArgument(argv[2]);
    const bool doSetCellularIms = argc == 4 &&
            (strcmp(argv[1], "set-cellular-ims") == 0 ||
             strcmp(argv[1], "restore-cellular-ims") == 0) &&
            isBooleanArgument(argv[2]) && isBooleanArgument(argv[3]);
    if (!doGet && !doEnsureVolte && !doGetRegManager && !doGetImsa && !doSnapshot &&
        !doWatchDecision &&
        !doAuditBootstrap && !doSetVolte && !doSetCellularIms &&
        !doSmsSelftest && !doGetSms && !doSetSms && !wmsMessage) {
        usage(argv[0]);
        return kUsageError;
    }

    alarm(doWatchDecision ? watchSeconds + kWatchdogMarginSeconds :
                            kWatchdogSeconds);

    if (doSmsSelftest || doGetSms || doSetSms) {
        const int check = checkSmsCodec();
        if (check != kSuccess || doSmsSelftest) {
            alarm(0);
            return check;
        }
    }
    if (wmsMessage != 0) {
        const int result = getWmsField(wmsMessage);
        alarm(0);
        return result;
    }
    if (doGetImsa) {
        const int result = getImsaState();
        alarm(0);
        return result;
    }

    Api api = {};
    if (!loadApi(&api)) return kLibraryError;

    CallbackState state = {};
    pthread_mutex_init(&state.mutex, nullptr);
    pthread_cond_init(&state.condition, nullptr);
    state.callbackId = -1;

    const ClientCallbacks callbacks = {
        responseCallback,
        indicationCallback,
        errorCallback,
    };
    void* clientHandle = nullptr;
    const int registrationResult =
            api.registerClient(&callbacks, &state, &clientHandle);
    if (registrationResult != 0 || clientHandle == nullptr) {
        fprintf(stderr,
                "ERROR IMS Settings registration failed: result=%d handle=%p\n",
                registrationResult, clientHandle);
        dlclose(api.library);
        return kRegistrationError;
    }
    fprintf(stderr, "RESULT IMS Settings registration succeeded handle=%p\n",
            clientHandle);

    int result = kSuccess;
    QipcallConfig original = {};
    if (doGetSms) {
        result = getSmsConfig(api, clientHandle);
    } else if (doSetSms) {
        result = setSmsFormat(api, clientHandle, strcmp(argv[2], "1") == 0 ? 1 : 0);
    } else if (doGet) {
        result = getConfig(api, clientHandle, &state, &original);
    } else if (doGetRegManager) {
        result = getRegistrationManagerConfig(api, clientHandle);
    } else if (doSnapshot) {
        const int regManagerResult =
                getRegistrationManagerConfig(api, clientHandle);
        const int imsaResult = getImsaState();
        result = regManagerResult != kSuccess ? regManagerResult : imsaResult;
    } else if (doWatchDecision) {
        printf("WATCH_PHASE PRE_REG_MGR\n");
        result = getRegistrationManagerConfig(api, clientHandle);
        if (result == kSuccess) {
            result = getRegistrationManagerReadOnlyConfig(api, clientHandle);
        }
        if (result == kSuccess) result = watchImsaDecision(watchSeconds);
        if (result == kSuccess) {
            printf("WATCH_PHASE POST_REG_MGR\n");
            result = getRegistrationManagerConfig(api, clientHandle);
        }
        if (result == kSuccess) {
            result = getRegistrationManagerReadOnlyConfig(api, clientHandle);
        }
    } else if (doAuditBootstrap) {
        result = auditBootstrap(api, clientHandle, argv[2]);
    } else if (doEnsureVolte) {
        result = ensureVolte(api, clientHandle, &state, strcmp(argv[2], "1") == 0);
    } else if (doSetVolte) {
        // Always capture and print the original value before mutation so the
        // exact restore command is known even if the later request fails.
        result = getConfig(api, clientHandle, &state, &original);
        if (result == kSuccess) {
            printf("RESTORE_COMMAND %s restore-volte %u\n", argv[0],
                   original.volteEnabled ? 1u : 0u);
            result = setVolte(api, clientHandle, &state,
                              strcmp(argv[2], "1") == 0);
        }
        if (result == kSuccess) {
            QipcallConfig readback = {};
            result = getConfig(api, clientHandle, &state, &readback);
            if (result == kSuccess &&
                (readback.volteEnabledValid != 1 ||
                 readback.volteEnabled !=
                         static_cast<uint8_t>(strcmp(argv[2], "1") == 0))) {
                fprintf(stderr,
                        "ERROR VoLTE readback mismatch: valid=%u value=%u\n",
                        readback.volteEnabledValid, readback.volteEnabled);
                result = kModemError;
            }
        }
    } else {
        // Both stored fields must be readable before a two-field mutation.
        // Otherwise an exact rollback cannot be promised, so fail closed.
        result = getConfig(api, clientHandle, &state, &original);
        if (result == kSuccess &&
            (original.mobileDataEnabledValid != 1 ||
             original.volteEnabledValid != 1)) {
            fprintf(stderr,
                    "ERROR cannot guarantee cellular-IMS rollback: "
                    "mobile_data_valid=%u volte_valid=%u\n",
                    original.mobileDataEnabledValid,
                    original.volteEnabledValid);
            result = kMalformedResponse;
        }
        if (result == kSuccess) {
            printf("RESTORE_COMMAND %s restore-cellular-ims %u %u\n",
                   argv[0], original.mobileDataEnabled ? 1u : 0u,
                   original.volteEnabled ? 1u : 0u);
            result = setCellularIms(api, clientHandle, &state,
                                    strcmp(argv[2], "1") == 0,
                                    strcmp(argv[3], "1") == 0);
        }
        if (result == kSuccess) {
            QipcallConfig readback = {};
            result = getConfig(api, clientHandle, &state, &readback);
            const uint8_t requestedMobileData =
                    static_cast<uint8_t>(strcmp(argv[2], "1") == 0);
            const uint8_t requestedVolte =
                    static_cast<uint8_t>(strcmp(argv[3], "1") == 0);
            if (result == kSuccess &&
                (readback.mobileDataEnabledValid != 1 ||
                 readback.mobileDataEnabled != requestedMobileData ||
                 readback.vtCallingEnabledValid !=
                         original.vtCallingEnabledValid ||
                 readback.vtCallingEnabled != original.vtCallingEnabled ||
                 readback.volteEnabledValid != 1 ||
                 readback.volteEnabled != requestedVolte)) {
                fprintf(stderr,
                        "ERROR cellular-IMS readback mismatch: "
                        "mobile_data_valid=%u mobile_data=%u "
                        "vt_valid=%u vt=%u volte_valid=%u volte=%u\n",
                        readback.mobileDataEnabledValid,
                        readback.mobileDataEnabled,
                        readback.vtCallingEnabledValid,
                        readback.vtCallingEnabled,
                        readback.volteEnabledValid,
                        readback.volteEnabled);
                result = kModemError;
            }
        }
    }

    const int deregistrationResult = api.deregisterClient(clientHandle);
    if (deregistrationResult != 0) {
        fprintf(stderr, "ERROR IMS Settings deregistration failed: %d\n",
                deregistrationResult);
        if (result == kSuccess) result = kRegistrationError;
    }
    pthread_cond_destroy(&state.condition);
    pthread_mutex_destroy(&state.mutex);
    dlclose(api.library);
    alarm(0);
    return result;
}
