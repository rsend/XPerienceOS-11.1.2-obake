/* Copyright (C) 2026 The XPerience Project. Licensed under Apache-2.0. */
#define LOG_TAG "ims_lte_guard"
#include <cutils/properties.h>
#include <dlfcn.h>
#include <log/log.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "ims_lte_recovery.h"

// Experimental SU6-7.3 startup guard. No IMSS/SMS writes, network selection,
// PLMN unblock, persistent NV writes, or fabricated availability indications.
namespace {
constexpr char kStack[] = "/system/lib/radio-su6-73/";
constexpr unsigned kTimeoutMs = 2000;
constexpr uint64_t kRefreshMs = 3000;
constexpr uint64_t kNasFreshMs = 10000;
using imscompat::NasService;
using imscompat::RecoveryAction;
volatile sig_atomic_t stopping = 0;

void stopHandler(int) { stopping = 1; }
void watchdog(int) { _exit(124); }  // Init withdraws readiness, then restarts.

void publish(const char* name, const char* value) {
    char previous[PROPERTY_VALUE_MAX] = {};
    property_get(name, previous, "");
    if (strcmp(previous, value) != 0 && property_set(name, value) != 0) {
        ALOGE("failure=property_write name=%s", name);
        exit(1);
    }
}

bool equals(const char* name, const char* value) {
    char current[PROPERTY_VALUE_MAX] = {};
    property_get(name, current, "");
    return strcmp(current, value) == 0;
}

uint64_t nowMs() {
    timespec now = {};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        ALOGE("failure=monotonic_clock");
        exit(1);
    }
    return static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

void stamp(const char* name) {
    char value[32];
    snprintf(value, sizeof(value), "%llu", static_cast<unsigned long long>(nowMs()));
    publish(name, value);
}

bool enabled() {
    return equals("persist.ims.compat.mode", "obake_legacy")
            && property_get_bool("persist.ims.compat.startup", true)
            && property_get_bool("persist.ims.compat.volte", true);
}

int intent() {
    // A normal framework request supersedes the one-shot early user hint.
    const char* names[] = {"sys.ims.volte.intent", "ril.ims.early_volte"};
    for (const char* name : names) {
        if (equals(name, "1")) return 1;
        if (equals(name, "0")) return 0;
    }
    return -1;
}

uint32_t read32(const uint8_t* bytes, size_t offset) {
    uint32_t value;
    memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

void write32(uint8_t* bytes, size_t offset, uint32_t value) {
    memcpy(bytes + offset, &value, sizeof(value));
}

bool usableVoice(const uint8_t (&registration)[20], const uint8_t (&service)[72]) {
    return registration[8] == 1 && registration[9] == 1
            && service[16] == 1 && read32(service, 20) == 2;
}

NasService servingState(const uint8_t* response) {
    // NAS 1.105 serving-system aggregate is first in BOTH GET and indication.
    // C enums are uint32, despite being single bytes in its wire aggregate.
    const uint32_t reg = read32(response, 0), cs = read32(response, 4);
    const uint32_t ps = read32(response, 8), network = read32(response, 12);
    const uint32_t count = read32(response, 16);
    if (reg > 4 || cs > 2 || ps > 2 || network > 2 || count > 255)
        return NasService::kUnknown;
    if (reg == 3) return NasService::kDenied;
    if (reg == 0 || reg == 2)
        return ps == 1 ? NasService::kUnknown : NasService::kSearching;
    if (reg != 1 || count == 0) return NasService::kUnknown;
    bool lte = false;
    for (uint32_t index = 0; index < count; ++index)
        lte |= read32(response, 20 + 4 * index) == 8;
    if (lte) return ps == 1 ? NasService::kLte : NasService::kUnknown;
    return cs == 1 || ps == 1 ? NasService::kOther : NasService::kUnknown;
}

typedef void (*Indication)(void*, unsigned, void*, unsigned, void*);
typedef void (*ErrorCallback)(void*, int, void*);
struct ServiceInfo { unsigned words[5]; };
struct Api {
    void* services = nullptr;
    void* clients = nullptr;
    void* nas = nullptr;
    void* imsa = nullptr;
    int (*list)(void*, ServiceInfo*, unsigned*, unsigned*) = nullptr;
    int (*init)(const ServiceInfo*, void*, Indication, void*, void*, void**) = nullptr;
    int (*send)(void*, unsigned, const void*, unsigned, void*, unsigned, unsigned) = nullptr;
    int (*release)(void*) = nullptr;
    int (*onError)(void*, ErrorCallback, void*) = nullptr;
    int (*length)(void*, int, uint16_t, uint32_t*) = nullptr;
    int (*encode)(void*, int, uint16_t, const void*, uint32_t, void*, uint32_t, uint32_t*) = nullptr;
    int (*decode)(void*, int, uint16_t, const void*, uint32_t, void*, uint32_t) = nullptr;
};

template <typename T> bool symbol(void* library, const char* name, T* result) {
    *result = reinterpret_cast<T>(dlsym(library, name));
    Dl_info info = {};
    if (*result == nullptr || !dladdr(reinterpret_cast<void*>(*result), &info)
            || info.dli_fname == nullptr
            || strncmp(info.dli_fname, kStack, sizeof(kStack) - 1) != 0) {
        ALOGE("failure=stock_symbol_or_path symbol=%s", name);
        return false;
    }
    return true;
}

bool layout(const Api& api, void* service, int direction, uint16_t id, uint32_t size) {
    uint32_t actual = 0;
    return api.length(service, direction, id, &actual) == 0 && actual == size;
}

void makeUsageRequest(uint8_t (&request)[200], uint32_t usage) {
    memset(request, 0, sizeof(request));
    request[60] = 1;             // change_duration_valid; value at 64 = 0:
                                // POWER_CYCLE, never permanent.
    request[172] = 1;            // usage_preference_valid
    write32(request, 176, usage); // QMI: voice-centric=1, data-centric=2.
}

bool contract(const Api& api) {
    if (!imscompat::lteRecoverySelfTest()) return false;
    if (!layout(api, api.nas, 0, 0x33, 200) || !layout(api, api.nas, 1, 0x33, 8)
            || !layout(api, api.nas, 1, 0x34, 208)
            || !layout(api, api.nas, 1, 0x24, 1500)
            || !layout(api, api.nas, 2, 0x24, 1504)
            || !layout(api, api.nas, 0, 0x03, 51)
            || !layout(api, api.nas, 1, 0x03, 8)
            || !layout(api, api.imsa, 1, 0x20, 20)
            || !layout(api, api.imsa, 1, 0x21, 72)
            || !layout(api, api.imsa, 0, 0x22, 6)
            || !layout(api, api.imsa, 1, 0x22, 8)) return false;
    // Validate offsets AND wire widths with the installed legacy encoder.
    // This must emit ONLY duration + usage, not a full preference snapshot.
    for (uint32_t usage = 1; usage <= 2; ++usage) {
        alignas(8) uint8_t request[200];
        uint8_t wire[32] = {};
        makeUsageRequest(request, usage);
        uint32_t length = 0;
        uint8_t expected[] = {0x17, 1, 0, 0, 0x21, 4, 0, 0, 0, 0, 0};
        expected[7] = usage;
        if (api.encode(api.nas, 0, 0x33, request, sizeof(request), wire,
                       sizeof(wire), &length) != 0 || length != sizeof(expected)
                || memcmp(wire, expected, length) != 0) return false;
    }
    const uint8_t wire[] = {2, 4, 0, 0, 0, 0, 0, 0x1f, 4, 0, 2, 0, 0, 0};
    alignas(8) uint8_t decoded[208] = {};
    if (!(api.decode(api.nas, 1, 0x34, wire, sizeof(wire), decoded, sizeof(decoded)) == 0
            && read32(decoded, 0) == 0 && read32(decoded, 4) == 0
            && decoded[180] == 1 && read32(decoded, 184) == 2)) return false;
    const uint8_t regWire[] = {2, 4, 0, 0, 0, 0, 0, 0x10, 1, 0, 1};
    const uint8_t svcWire[] = {2, 4, 0, 0, 0, 0, 0, 0x11, 4, 0, 2, 0, 0, 0};
    alignas(8) uint8_t registration[20] = {}, service[72] = {};
    if (api.decode(api.imsa, 1, 0x20, regWire, sizeof(regWire), registration,
                   sizeof(registration)) != 0
            || api.decode(api.imsa, 1, 0x21, svcWire, sizeof(svcWire), service,
                          sizeof(service)) != 0 || !usableVoice(registration, service)) return false;
    // Policy=ON without registration, or limited/absent voice, is never ready.
    registration[9] = 0;
    if (usableVoice(registration, service)) return false;
    registration[9] = 1;
    registration[8] = 0;
    if (usableVoice(registration, service)) return false;
    registration[8] = 1;
    write32(service, 20, 1);
    if (usableVoice(registration, service)) return false;
    write32(service, 20, 2);
    service[16] = 0;
    if (usableVoice(registration, service)) return false;

    // GET result is at offset1040, NOT at offset0 as in the other messages.
    const uint8_t servingWire[] = {2, 4, 0, 0, 0, 0, 0, 1, 6, 0, 1, 2, 1, 2, 1, 8};
    alignas(8) uint8_t serving[1504] = {};
    if (api.decode(api.nas, 1, 0x24, servingWire, sizeof(servingWire), serving, 1500) != 0
            || read32(serving, 1040) != 0 || read32(serving, 1044) != 0
            || servingState(serving) != NasService::kLte) return false;
    if (api.decode(api.nas, 2, 0x24, servingWire + 7, sizeof(servingWire) - 7,
                   serving, sizeof(serving)) != 0
            || servingState(serving) != NasService::kLte) return false;
    write32(serving, 0, 2); write32(serving, 8, 2);
    if (servingState(serving) != NasService::kSearching) return false;
    write32(serving, 0, 3);
    if (servingState(serving) != NasService::kDenied) return false;
    write32(serving, 0, 1); write32(serving, 8, 1); write32(serving, 20, 5);
    if (servingState(serving) != NasService::kOther) return false;
    write32(serving, 16, 256);
    if (servingState(serving) != NasService::kUnknown) return false;
    uint8_t subscribe[51] = {}, encoded[8] = {};
    subscribe[4] = 1; subscribe[5] = 1; // Only Serving System Events (TLV0x13).
    uint32_t length = 0;
    const uint8_t expected[] = {0x13, 1, 0, 1};
    return api.encode(api.nas, 0, 0x03, subscribe, sizeof(subscribe), encoded,
                      sizeof(encoded), &length) == 0 && length == sizeof(expected)
            && memcmp(encoded, expected, length) == 0;
}

bool load(Api* api) {
    api->services = dlopen("/system/lib/radio-su6-73/libqmiservices.so", RTLD_NOW | RTLD_LOCAL);
    api->clients = dlopen("/system/lib/radio-su6-73/libqmi_cci.so", RTLD_NOW | RTLD_LOCAL);
    if (!api->services || !api->clients) {
        ALOGE("failure=stock_library_load detail=%s", dlerror());
        return false;
    }
    void* (*nas)(int32_t, int32_t, int32_t) = nullptr;
    void* (*imsa)(int32_t, int32_t, int32_t) = nullptr;
    if (!symbol(api->services, "nas_get_service_object_internal_v01", &nas)
            || !symbol(api->services, "imsa_get_service_object_internal_v01", &imsa)
            || !symbol(api->clients, "qmi_client_get_service_list", &api->list)
            || !symbol(api->clients, "qmi_client_init", &api->init)
            || !symbol(api->clients, "qmi_client_send_msg_sync", &api->send)
            || !symbol(api->clients, "qmi_client_release", &api->release)
            || !symbol(api->clients, "qmi_client_register_error_cb", &api->onError)
            || !symbol(api->services, "qmi_idl_get_message_c_struct_len", &api->length)
            || !symbol(api->services, "qmi_idl_message_encode", &api->encode)
            || !symbol(api->services, "qmi_idl_message_decode", &api->decode)) return false;
    api->nas = nas(1, 105, 6);
    api->imsa = imsa(1, 8, 6);
    if (!api->nas || !api->imsa || !contract(*api)) {
        ALOGE("failure=stock_idl_contract no_modem_write=1");
        return false;
    }
    return true;
}

bool connect(const Api& api, void* service, Indication callback, void* data, void** client) {
    *client = nullptr;
    ServiceInfo info = {};
    unsigned entries = 1, total = 0;
    if (api.list(service, &info, &entries, &total) != 0 || entries != 1 || total == 0)
        return false;
    const int result = api.init(&info, service, callback, data, nullptr, client);
    if (result == 0 && *client != nullptr) return true;
    ALOGW("dependency=qmi failure=client_init result=%d", result);
    *client = nullptr;  // A failed init never authorizes using the output handle.
    return false;
}

void release(const Api& api, void** client) {
    if (*client && api.release(*client) != 0) {
        ALOGE("dependency=qmi failure=release callback_lifetime_unknown=1");
        publish("sys.ims.voice.ready", "0");
        // Do not unload libraries or reuse callback storage on an uncertain
        // release. Init clears readiness and recovers any owned NAS setting.
        _exit(1);
    }
    *client = nullptr;
}

bool request(const Api& api, void* client, unsigned id, const void* input,
             unsigned inputSize, uint8_t* output, unsigned outputSize, unsigned resultOffset = 0) {
    if (outputSize < 8 || resultOffset > outputSize - 8) {
        ALOGE("failure=qmi_result_offset id=0x%x size=%u offset=%u", id, outputSize, resultOffset);
        return false;
    }
    memset(output, 0, outputSize);
    write32(output, resultOffset, UINT32_MAX);
    const int result = api.send(client, id, input, inputSize, output, outputSize, kTimeoutMs);
    if (result == 0 && read32(output, resultOffset) == 0 && read32(output, resultOffset + 4) == 0)
        return true;
    ALOGW("dependency=qmi failure=request id=0x%x transport=%d result=%u error=%u",
          id, result, read32(output, resultOffset), read32(output, resultOffset + 4));
    return false;
}

bool getUsage(const Api& api, void* client, uint32_t* usage) {
    alignas(8) uint8_t response[208];
    if (!request(api, client, 0x34, nullptr, 0, response, sizeof(response))) return false;
    *usage = read32(response, 184);
    if (response[180] == 1 && (*usage == 1 || *usage == 2)) return true;
    ALOGW("dependency=nas failure=missing_or_invalid_usage valid=%u value=%u",
          response[180], *usage);
    return false;
}

bool setUsage(const Api& api, void* client, uint32_t usage) {
    alignas(8) uint8_t input[200], response[8];
    makeUsageRequest(input, usage);
    return request(api, client, 0x33, input, sizeof(input), response, sizeof(response));
}

bool restoreUsage(const Api& api, void* nas) {
    if (!equals("sys.ims.lte.original", "1")) return true;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        alarm(10);
        uint32_t usage = 0;
        if (getUsage(api, nas, &usage)) {
            if (usage == 2) {
                // Read before EACH retry; never replay other NAS settings.
                setUsage(api, nas, 1);  // Readback resolves even an unknown ACK.
                if (!getUsage(api, nas, &usage)) usage = 0;
            }
            if (usage == 1) {
                publish("sys.ims.lte.original", "");
                publish("sys.ims.lte.guard", "restored");
                stamp("sys.ims.lte.restore_ms");
                return true;
            }
        }
        usleep((100000u << attempt) + (getpid() % 100) * 1000u);
    }
    publish("sys.ims.lte.guard", "restore_failed");
    ALOGE("dependency=nas failure=restore_usage attempts=3 power_cycle_may_be_required=1");
    return false;
}

struct NasState {
    const Api* api = nullptr;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    NasService service = NasService::kUnknown;
    uint64_t generation = 0;
    uint64_t losses = 0;
    uint64_t sampledMs = 0;
    bool disconnected = false;
};

// Caller holds mutex. A missing/failed response must NOT generate a loss edge.
void recordNas(NasState* state, NasService service) {
    if (service == NasService::kSearching && state->service != NasService::kSearching)
        ++state->losses;
    state->service = service;
    state->sampledMs = nowMs();
    ++state->generation;
}

void nasIndication(void*, unsigned id, void* bytes, unsigned size, void* data) {
    if (id != 0x24) return;
    NasState* state = static_cast<NasState*>(data);
    alignas(8) uint8_t decoded[1504] = {};
    const bool valid = bytes && state->api->decode(state->api->nas, 2, 0x24,
            bytes, size, decoded, sizeof(decoded)) == 0;
    if (!valid) ALOGW("dependency=nas failure=serving_indication_decode");
    pthread_mutex_lock(&state->mutex);
    recordNas(state, valid ? servingState(decoded) : NasService::kUnknown);
    pthread_mutex_unlock(&state->mutex);
}

void nasError(void*, int error, void* data) {
    ALOGW("dependency=nas failure=source_lost error=%d", error);
    NasState* state = static_cast<NasState*>(data);
    pthread_mutex_lock(&state->mutex);
    state->disconnected = true;
    recordNas(state, NasService::kUnknown);
    pthread_mutex_unlock(&state->mutex);
}

bool queryNas(const Api& api, void* nas, NasState* state) {
    pthread_mutex_lock(&state->mutex);
    const uint64_t generation = state->generation;
    pthread_mutex_unlock(&state->mutex);
    alignas(8) uint8_t response[1500];
    const bool healthy = request(api, nas, 0x24, nullptr, 0, response, sizeof(response), 1040);
    pthread_mutex_lock(&state->mutex);
    // Do not overwrite a newer indication with an in-flight GET's old state.
    if (!healthy || generation == state->generation)
        recordNas(state, healthy ? servingState(response) : NasService::kUnknown);
    pthread_mutex_unlock(&state->mutex);
    return healthy;
}

NasService nasSnapshot(NasState* state, uint64_t* losses, bool* disconnected) {
    pthread_mutex_lock(&state->mutex);
    *losses = state->losses;
    *disconnected = state->disconnected;
    const NasService result = state->disconnected || nowMs() - state->sampledMs > kNasFreshMs
            ? NasService::kUnknown : state->service;
    pthread_mutex_unlock(&state->mutex);
    return result;
}

void closeNas(const Api& api, void** nas, NasState* state) {
    release(api, nas); // Quiesce callbacks before resetting their state.
    pthread_mutex_lock(&state->mutex);
    state->disconnected = false;
    recordNas(state, NasService::kUnknown);
    pthread_mutex_unlock(&state->mutex);
}

const char* serviceName(NasService service) {
    switch (service) {
        case NasService::kSearching: return "searching";
        case NasService::kLte: return "lte_registered";
        case NasService::kOther: return "other_registered";
        case NasService::kDenied: return "registration_denied";
        default: return "unknown";
    }
}

struct VoiceState {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    uint64_t generation = 0;
    bool disconnected = false;
    bool known = false;
    bool ready = false;
    uint64_t sampledMs = 0;
};

void invalidate(VoiceState* state, bool disconnected) {
    pthread_mutex_lock(&state->mutex);
    ++state->generation;
    state->disconnected |= disconnected;
    state->known = false;
    state->ready = false;
    publish("sys.ims.voice.ready", "0");
    publish("sys.ims.voice.status", disconnected ? "source_lost" : "refreshing");
    pthread_mutex_unlock(&state->mutex);
}

void indication(void*, unsigned id, void*, unsigned, void* data) {
    // Any registration/service change invalidates the old pair immediately.
    // Only a fresh, consistent GET pair may publish a positive result.
    if (id == 0x23 || id == 0x24) invalidate(static_cast<VoiceState*>(data), false);
}

void sourceError(void*, int error, void* data) {
    ALOGW("dependency=imsa failure=source_lost error=%d", error);
    invalidate(static_cast<VoiceState*>(data), true);
}

bool refreshVoice(const Api& api, void* imsa, VoiceState* state, bool* healthy) {
    pthread_mutex_lock(&state->mutex);
    const uint64_t generation = state->generation;
    pthread_mutex_unlock(&state->mutex);
    alignas(8) uint8_t registration[20], service[72];
    *healthy = request(api, imsa, 0x20, nullptr, 0, registration, sizeof(registration))
            && request(api, imsa, 0x21, nullptr, 0, service, sizeof(service));
    if (*healthy && (registration[8] > 1 || (registration[8] && registration[9] > 1)
            || service[16] > 1 || (service[16] && read32(service, 20) > 2))) {
        ALOGW("dependency=imsa failure=malformed_registration_or_voice");
        *healthy = false;
    }
    const bool valid = *healthy && registration[8] == 1 && registration[9] <= 1
            && service[16] == 1 && read32(service, 20) <= 2;
    pthread_mutex_lock(&state->mutex);
    const bool fresh = valid && generation == state->generation && !state->disconnected;
    const bool ready = fresh && usableVoice(registration, service)
            && intent() != 0 && enabled();
    state->known = fresh;
    state->ready = ready;
    state->sampledMs = nowMs();
    publish("sys.ims.voice.ready", ready ? "1" : "0");
    publish("sys.ims.voice.status", intent() == 0 || !enabled() ? "policy_disabled"
            : !fresh ? "unknown" : ready ? "ready"
            : registration[9] != 1 ? "unregistered" : "voice_unavailable");
    stamp("sys.ims.voice.sample_ms");
    pthread_mutex_unlock(&state->mutex);
    return ready;
}

int run() {
    publish("sys.ims.voice.ready", "0");
    publish("sys.ims.voice.status", "starting");
    publish("sys.ims.lte.abi", "unverified");
    Api api;
    if (!load(&api)) return 1;
    publish("sys.ims.lte.abi", "verified");
    void* nas = nullptr;
    void* imsa = nullptr;
    VoiceState voice;
    NasState nasState;
    nasState.api = &api;
    uint64_t refreshAt = 0, discoverAt = 0, nasAt = 0, cleanupAt = 0, seenLosses = 0;
    uint64_t statusAt = 0;
    unsigned failures = 0, nasFailures = 0;
    bool cleanupPending = equals("sys.ims.lte.original", "1");
    // An interrupted write cannot earn another full attempt on the same
    // attachment. First resolve its ownership, then require a fresh cycle.
    imscompat::LteRecovery recovery(cleanupPending || equals("sys.ims.lte.exhausted", "1"));
    if (cleanupPending) publish("sys.ims.lte.guard", "restore_pending");
    publish("sys.ims.lte.attempted", ""); // Retired boot-only latch.

    while (!stopping) {
        alarm(10);  // Covers dependency hangs AND stale positive readiness.
        const uint64_t now = nowMs();
        if (intent() == 0 || !enabled()) {
            publish("sys.ims.voice.ready", "0");
            publish("sys.ims.voice.status", "policy_disabled");
        }
        uint64_t losses = 0;
        bool nasDisconnected = false;
        nasSnapshot(&nasState, &losses, &nasDisconnected);
        if (nasDisconnected) {
            closeNas(api, &nas, &nasState);
            nasAt = now + 1000;
        }
        if (now >= nasAt) {
            // Discovery/control can take longer than the normal refresh
            // cadence. Do not retain an old positive voice sample through it.
            if (!nas) invalidate(&voice, false);
            if (!nas && connect(api, api.nas, nasIndication, &nasState, &nas)) {
                uint8_t subscriptions[51] = {};
                subscriptions[4] = 1; subscriptions[5] = 1;
                alignas(8) uint8_t response[8];
                if (api.onError(nas, nasError, &nasState) != 0
                        || !request(api, nas, 0x03, subscriptions, sizeof(subscriptions),
                                    response, sizeof(response))) closeNas(api, &nas, &nasState);
            }
            if (nas && queryNas(api, nas, &nasState)) {
                nasFailures = 0;
                nasAt = nowMs() + kRefreshMs;
            } else {
                closeNas(api, &nas, &nasState);
                if (nasFailures == 0) ALOGW("dependency=nas failure=unavailable guard_source=unknown");
                if (nasFailures < 5) ++nasFailures;
                nasAt = nowMs() + (1000u << nasFailures) + getpid() % 251;
            }
        }
        if (cleanupPending && nas && nowMs() >= cleanupAt) {
            invalidate(&voice, false);
            cleanupPending = !restoreUsage(api, nas);
            cleanupAt = nowMs() + imscompat::LteRecovery::kRetryMs;
        }

        alarm(10);
        pthread_mutex_lock(&voice.mutex);
        const bool disconnected = voice.disconnected;
        pthread_mutex_unlock(&voice.mutex);
        if (disconnected) {
            release(api, &imsa);
            pthread_mutex_lock(&voice.mutex);
            voice.disconnected = false;
            pthread_mutex_unlock(&voice.mutex);
            if (discoverAt < now + 1000) discoverAt = now + 1000;
        }
        if (!imsa && now >= discoverAt) {
            if (connect(api, api.imsa, indication, &voice, &imsa)) {
                alignas(8) uint8_t response[8];
                const uint8_t subscriptions[] = {1, 1, 1, 1, 0, 0};
                if (api.onError(imsa, sourceError, &voice) != 0
                        || !request(api, imsa, 0x22, subscriptions, sizeof(subscriptions),
                                    response, sizeof(response))) {
                    invalidate(&voice, true);
                    release(api, &imsa);
                }
            }
            if (!imsa) {
                if (failures == 0) ALOGW("dependency=imsa failure=unavailable readiness=0");
                if (failures < 5) ++failures;
                discoverAt = nowMs() + (1000u << failures) + getpid() % 251;
            } else {
                refreshAt = 0;
            }
        }
        if (imsa && now >= refreshAt) {
            bool healthy = false;
            refreshVoice(api, imsa, &voice, &healthy);
            refreshAt = nowMs() + kRefreshMs;
            if (!healthy) {
                release(api, &imsa);
                if (failures < 5) ++failures;
                discoverAt = nowMs() + (1000u << failures) + getpid() % 251;
            } else {
                failures = 0;
            }
        }
        alarm(10);
        const NasService service = nasSnapshot(&nasState, &losses, &nasDisconnected);
        pthread_mutex_lock(&voice.mutex);
        const bool voiceKnown = voice.known && !voice.disconnected
                && nowMs() - voice.sampledMs <= kNasFreshMs;
        const bool ready = voiceKnown && voice.ready;
        pthread_mutex_unlock(&voice.mutex);
        const bool wanted = enabled() && intent() == 1;
        if (losses != seenLosses && service != NasService::kSearching) {
            // Preserve a real loss/reacquisition edge that occurred between
            // GET polls; failed queries never increment this counter.
            recovery.update(nowMs(), NasService::kSearching, false, false, wanted);
        }
        seenLosses = losses;
        const RecoveryAction action = recovery.update(nowMs(), service, voiceKnown, ready, wanted);
        publish("sys.ims.lte.exhausted", recovery.exhausted() ? "1" : "0");
        publish("sys.ims.lte.service", serviceName(service));
        if (now >= statusAt) {
            char used[32];
            snprintf(used, sizeof(used), "%llu", static_cast<unsigned long long>(recovery.usedMs()));
            publish("sys.ims.lte.used_ms", used);
            publish("sys.ims.lte.phase", cleanupPending ? "restore_pending" : recovery.exhausted()
                    ? "await_new_acquisition" : recovery.protecting()
                    ? (service == NasService::kSearching ? "waiting_service"
                        : service == NasService::kUnknown ? "waiting_source" : "pending_ims")
                    : "monitoring");
            statusAt = nowMs() + kRefreshMs;
        }
        if (nas && !nasDisconnected && !cleanupPending && action == RecoveryAction::kArm) {
            invalidate(&voice, false);
            uint32_t usage = 0;
            bool verified = getUsage(api, nas, &usage);
            uint64_t currentLosses = 0;
            bool currentDisconnected = false;
            const NasService current = nasSnapshot(&nasState, &currentLosses, &currentDisconnected);
            if (verified && (!enabled() || intent() != 1 || currentDisconnected
                    || (current != NasService::kSearching && current != NasService::kLte))) {
                // A user/source/RAT change during GET invalidates this ARM.
                // No setting has been written, and the next attempt reads again.
                recovery.armed(false, nowMs());
                publish("sys.ims.lte.guard", "context_changed");
                usleep(250000);
                continue;
            }
            if (verified && usage == 1) {
                // Resolve every attempt with fresh readback, even after an
                // unknown ACK; ownership still precedes the usage-only SET.
                publish("sys.ims.lte.original", "1");
                publish("sys.ims.lte.guard", "applying");
                const bool ack = setUsage(api, nas, 2);
                verified = getUsage(api, nas, &usage) && usage == 2;
                if (verified) {
                    publish("sys.ims.lte.guard", ack ? "active" : "active_ack_unknown");
                    stamp("sys.ims.lte.guard_ms");
                } else {
                    cleanupPending = !restoreUsage(api, nas);
                    cleanupAt = nowMs() + imscompat::LteRecovery::kRetryMs;
                }
            } else if (verified) {
                publish("sys.ims.lte.guard", "already_data_centric");
            }
            if (!verified) {
                ALOGW("dependency=nas failure=guard_unverified retry_ms=30000");
                if (!cleanupPending) publish("sys.ims.lte.guard", "unavailable");
            }
            recovery.armed(verified, nowMs());
        } else if (!cleanupPending && action == RecoveryAction::kRestore) {
            if (!equals("sys.ims.lte.original", "1")) {
                recovery.restored(true, nowMs()); // External data-centric state is not ours.
            } else if (nas && !nasDisconnected) {
                invalidate(&voice, false);
                if (recovery.exhausted()) ALOGW("failure=ims_acquisition_exhausted used_ms=%llu service=%s",
                        static_cast<unsigned long long>(recovery.usedMs()), serviceName(service));
                const bool restored = restoreUsage(api, nas);
                recovery.restored(restored, nowMs());
            } else {
                if (!equals("sys.ims.lte.guard", "restore_pending"))
                    ALOGW("dependency=nas failure=restore_waiting_for_source power_cycle_may_be_required=1");
                publish("sys.ims.lte.guard", "restore_pending");
            }
        }
        if (!enabled()) break;
        usleep(250000);
    }
    publish("sys.ims.voice.ready", "0");
    publish("sys.ims.voice.status", "stopped");
    if (nas) restoreUsage(api, nas);
    if (!nas && equals("sys.ims.lte.original", "1")) {
        publish("sys.ims.lte.guard", "restore_failed");
        ALOGE("dependency=nas failure=shutdown_without_restore_client power_cycle_may_be_required=1");
    }
    release(api, &imsa);
    release(api, &nas);
    pthread_mutex_destroy(&voice.mutex);
    pthread_mutex_destroy(&nasState.mutex);
    dlclose(api.clients);
    dlclose(api.services);
    return 0;
}
}  // namespace

int main() {
    signal(SIGTERM, stopHandler);
    signal(SIGINT, stopHandler);
    signal(SIGALRM, watchdog);
    for (;;) {
        // Runtime kill switch: restore in run(), then idle without QMI work.
        // Do not restart/query in a loop while disabled. An owned guard from
        // a prior crash still needs its cleanup even with the switch off.
        publish("sys.ims.voice.ready", "0");
        while (!stopping && !enabled() && !equals("sys.ims.lte.original", "1")) {
            publish("sys.ims.voice.status", "disabled");
            alarm(10);
            usleep(250000);
        }
        if (stopping) return 0;
        alarm(10);
        const int result = run();
        if (result != 0 || stopping) return result;
        if (equals("sys.ims.lte.original", "1")) {
            // Retain the failed-restore marker, but do not retry forever.
            while (!stopping) { alarm(10); usleep(250000); }
            return 1;
        }
    }
}
