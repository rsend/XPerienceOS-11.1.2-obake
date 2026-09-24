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

#define LOG_TAG "ims_compatd"

#include "ims_frame_codec.h"
#include "ims_protocol_adapter.h"
#include "ims_call_list_bridge.h"
#include "ims_volte_bridge.h"
#include "ims_volte_worker.h"
#include "ims_service_state.h"
#include "ims_service_publisher.h"

#include <cutils/sockets.h>
#include <cutils/properties.h>
#include <log/log.h>
#include <private/android_filesystem_config.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>

namespace {

constexpr char kClientSocketPath[] = "/dev/socket/qmux_radio/rild_ims0";
constexpr char kUpstreamSocketPath[] = "/dev/socket/qmux_radio/rild_ims";
constexpr char kStatusProperty[] = "sys.ims.compat.status";
constexpr char kModeProperty[] = "persist.ims.compat.mode";
constexpr char kCallBridgeProperty[] = "persist.ims.compat.call_list";
constexpr char kCallStatusProperty[] = "sys.ims.compat.calls";
constexpr char kVolteBridgeProperty[] = "persist.ims.compat.volte";
constexpr char kVolteStatusProperty[] = "sys.ims.volte.status";
constexpr char kServiceStatusProperty[] = "sys.ims.svc.status";
constexpr char kServiceEnabledProperty[] = "sys.ims.svc.enabled";
constexpr char kServicePartialProperty[] = "sys.ims.svc.partial";
constexpr char kServiceUpdatesProperty[] = "sys.ims.svc.updates";
constexpr unsigned kConnectAttempts = 10;
constexpr useconds_t kInitialRetryDelayUs = 100 * 1000;
constexpr useconds_t kMaximumRetryDelayUs = 1600 * 1000;

// Early user intent shares the normal worker: never create a second IMSS
// writer or reintroduce the rolled-back global settings lock. These flags
// identify ownership independently of the framework's transaction tokens.
bool earlyPolicyAllowed = false;
bool earlyPolicyRunning = false;
bool frameworkPolicySeen = false;

void setStatus(const char* status) {
    if (property_set(kStatusProperty, status) < 0) {
        ALOGW("cannot set %s=%s", kStatusProperty, status);
    }
}

void setServiceProperty(const char* name, const char* value) {
    if (property_set(name, value) < 0) {
        ALOGW("cannot set %s=%s", name, value);
    }
}

void publishServiceState(const imscompat::ServiceStateTracker& services,
                         const char* status) {
    char value[PROPERTY_VALUE_MAX] = {};
    setServiceProperty(kServiceStatusProperty, status);
    snprintf(value, sizeof(value), "0x%08x", services.enabledMask());
    setServiceProperty(kServiceEnabledProperty, value);
    snprintf(value, sizeof(value), "0x%08x", services.partialMask());
    setServiceProperty(kServicePartialProperty, value);
    snprintf(value, sizeof(value), "%zu", services.acceptedUpdates());
    setServiceProperty(kServiceUpdatesProperty, value);
}

bool setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    const int descriptorFlags = fcntl(fd, F_GETFD, 0);
    if (flags < 0 || descriptorFlags < 0
            || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0
            || fcntl(fd, F_SETFD, descriptorFlags | FD_CLOEXEC) < 0) {
        ALOGE("cannot make fd %d nonblocking: %s", fd, strerror(errno));
        setStatus("io_error");
        return false;
    }
    return true;
}

bool monotonicMilliseconds(uint64_t* milliseconds) {
    timespec value = {};
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        ALOGE("cannot read monotonic clock: %s", strerror(errno));
        setStatus("clock_error");
        return false;
    }
    *milliseconds = static_cast<uint64_t>(value.tv_sec) * 1000u +
                    static_cast<uint64_t>(value.tv_nsec) / 1000000u;
    return true;
}

bool loadProtocolProfile(imscompat::ProtocolProfile* profile) {
    char value[PROPERTY_VALUE_MAX] = {};
    property_get(kModeProperty, value, "passthrough");
    if (strcmp(value, "passthrough") == 0) {
        *profile = imscompat::ProtocolProfile::kPassThrough;
        return true;
    }
    if (strcmp(value, "obake_legacy") == 0) {
        *profile = imscompat::ProtocolProfile::kObakeLegacy;
        return true;
    }
    ALOGE("unsupported %s=%s", kModeProperty, value);
    setStatus("invalid_mode");
    return false;
}

bool expireTransactions(imscompat::TransactionTracker* tracker,
                        imscompat::ServiceStateTracker* services) {
    uint64_t nowMs = 0;
    if (!monotonicMilliseconds(&nowMs)) {
        return false;
    }
    imscompat::Transaction expired[imscompat::kMaximumTransactions];
    const size_t count = tracker->expire(nowMs, expired,
                                         imscompat::kMaximumTransactions);
    for (size_t index = 0;
         index < std::min(count, imscompat::kMaximumTransactions); ++index) {
        services->cancel(expired[index].token);
        ALOGW("IMS transaction timeout token=%u client_id=%u upstream_id=%u",
              expired[index].token, expired[index].clientId,
              expired[index].upstreamId);
    }
    return true;
}

bool queueValidatedFrame(imscompat::FrameBuffer* buffer, const uint8_t* data,
                         size_t size) {
    return buffer->insertValidated(data, size);
}

bool volteBridgeEnabled() {
    char value[PROPERTY_VALUE_MAX] = {};
    property_get(kVolteBridgeProperty, value, "1");
    return strcmp(value, "1") == 0;
}

bool pumpVolteBridge(imscompat::VoltePolicyBridge* bridge,
                     imscompat::VolteWorker* worker, uint64_t nowMs,
                     imscompat::FrameBuffer* output,
                     imscompat::ServiceStateTracker* services,
                     imscompat::ServiceStatePublisher* publisher) {
    imscompat::VolteWorkerResult workerResult;
    bool workerFinished = worker->poll(nowMs, &workerResult);
    if (workerFinished && earlyPolicyRunning) {
        earlyPolicyRunning = false;
        workerFinished = false;
        setServiceProperty("sys.ims.volte.early", workerResult.successful
                ? "configured" : "failed");
        char completed[32];
        snprintf(completed, sizeof(completed), "%llu", static_cast<unsigned long long>(nowMs));
        setServiceProperty("sys.ims.volte.early_done_ms", completed);
    }
    char early[PROPERTY_VALUE_MAX] = {};
    property_get("sys.ims.volte.early", early, "");
    if (earlyPolicyAllowed && !frameworkPolicySeen && early[0] == '\0'
            && !worker->busy() && volteBridgeEnabled()
            && property_get_bool("persist.ims.compat.startup", true)) {
        char desired[PROPERTY_VALUE_MAX] = {};
        property_get("ril.ims.early_volte", desired, "");
        if (strcmp(desired, "1") == 0 || strcmp(desired, "0") == 0) {
            // One attempt per boot. The normal bridge retains its bounded
            // retries and takes over even if this early attempt fails.
            setServiceProperty("sys.ims.volte.early", "applying");
            char started[32];
            snprintf(started, sizeof(started), "%llu", static_cast<unsigned long long>(nowMs));
            setServiceProperty("sys.ims.volte.early_start_ms", started);
            earlyPolicyRunning = worker->start(0, desired[0] == '1', nowMs, 8000);
            if (!earlyPolicyRunning) setServiceProperty("sys.ims.volte.early", "failed");
        }
    }
    if (bridge == nullptr) return true;
    if (!volteBridgeEnabled() && bridge->hasPending()) {
        ALOGW("VoLTE policy disabled with pending requests; closing stale session");
        setServiceProperty(kVolteStatusProperty, "disabled_pending");
        return false;
    }
    if (!volteBridgeEnabled()) {
        setServiceProperty(kVolteStatusProperty, "disabled");
        return true;
    }
    if (bridge->expired(nowMs)) {
        ALOGE("VoLTE policy transaction deadline exceeded; modem outcome unknown");
        setServiceProperty(kVolteStatusProperty, "timeout");
        setStatus("volte_policy_error");
        return false;
    }
    if (workerFinished && !bridge->finishJob(workerResult.token, workerResult.successful,
                                             workerResult.retryable, nowMs)) {
        ALOGE("unmatched VoLTE worker result token=%u", workerResult.token);
        setServiceProperty(kVolteStatusProperty, "tracking_error");
        return false;
    }
    imscompat::VolteCompletion completion;
    while (bridge->takeCompletion(&completion)) {
        const imscompat::CompleteServiceResult applied =
                services->complete(completion.token, completion.successful);
        if (applied != imscompat::CompleteServiceResult::kApplied
                && applied != imscompat::CompleteServiceResult::kRejected) {
            ALOGE("VoLTE canonical state completion failed token=%u", completion.token);
            setServiceProperty(kVolteStatusProperty, "tracking_error");
            return false;
        }
        uint8_t response[32];
        size_t responseSize = 0;
        // ImsQmiIF E_GENERIC_FAILURE=2; never forward the premature legacy success.
        if (!imscompat::encodeEmptyResponse(completion.token, imscompat::kSetServiceStatusRequestId,
                                             completion.successful ? 0 : 2,
                                             response, sizeof(response), &responseSize)
                || !queueValidatedFrame(output, response, responseSize)) {
            ALOGE("cannot queue VoLTE policy completion token=%u", completion.token);
            setStatus("backpressure_error");
            return false;
        }
        // This is policy readback, NOT operational voice readiness.
        setServiceProperty(kVolteStatusProperty, completion.successful ? "configured" : "failed");
        setServiceProperty("sys.ims.volte.applied", completion.successful
                ? (completion.enabled ? "1" : "0") : "unknown");
        publishServiceState(*services, completion.successful ? "accepted" : "rejected");
        if (completion.successful) {
            publisher->markDirty();
            publisher->tick(*services, nowMs);
        } else {
            ALOGW("VoLTE policy failed token=%u desired=%d; returning Android error",
                  completion.token, completion.enabled);
        }
    }
    uint32_t token = 0;
    bool enabled = false;
    if (!worker->busy() && bridge->nextJob(nowMs, &token, &enabled)) {
        setServiceProperty(kVolteStatusProperty, "applying");
        setServiceProperty("sys.ims.volte.desired", enabled ? "1" : "0");
        setServiceProperty("sys.ims.volte.applied", "unknown");
        if (!worker->start(token, enabled, nowMs)) {
            bridge->finishJob(token, false, true, nowMs);
        }
    }
    return true;
}

bool validateFrames(imscompat::FrameBuffer* buffer, bool clientToUpstream,
                    imscompat::TransactionTracker* tracker,
                    imscompat::ProtocolProfile profile,
                    imscompat::FrameBuffer* peerOutput,
                    imscompat::CallListBridge* calls,
                    imscompat::VoltePolicyBridge* volte,
                    imscompat::ServiceStateTracker* services,
                    imscompat::ServiceStatePublisher* publisher) {
    while (buffer->decodeSize() != 0) {
        imscompat::DecodedFrame frame;
        const imscompat::DecodeResult result = imscompat::decodeFrame(
                buffer->decodeData(), buffer->decodeSize(), &frame);
        if (result == imscompat::DecodeResult::kNeedMore) {
            return true;
        }
        if (result != imscompat::DecodeResult::kFrame) {
            ALOGE("rejecting %s IMS frame: %s",
                  clientToUpstream ? "client-to-upstream"
                                   : "upstream-to-client",
                  result == imscompat::DecodeResult::kOversized
                          ? "oversized"
                          : "malformed");
            setStatus("protocol_error");
            return false;
        }

        const uint32_t observedId = frame.metadata.id;
        const imscompat::FrameMetadata& tag = frame.metadata;

        if (clientToUpstream && tag.type == 1) {
            if (volte != nullptr && volte->contains(tag.token)) {
                ALOGE("IMS client reused pending VoLTE token=%u", tag.token);
                setStatus("transaction_error");
                return false;
            }
            if (calls != nullptr && tag.token == imscompat::kCallListQueryToken) {
                ALOGE("IMS client used reserved call-list token=%u id=%u",
                      tag.token, tag.id);
                setStatus("transaction_error");
                return false;
            }
            imscompat::ServiceStatusUpdate serviceUpdate;
            bool hasServiceUpdate = false;
            bool bridgeVolte = false;
            bool desiredVolte = false;
            if (observedId == imscompat::kSetServiceStatusRequestId) {
                const imscompat::ServiceDecodeResult serviceResult =
                        imscompat::decodeServiceStatusRequest(
                                buffer->decodeData(), frame, &serviceUpdate);
                if (serviceResult !=
                        imscompat::ServiceDecodeResult::kDecoded) {
                    ALOGE("rejecting malformed IMS service-status request "
                          "token=%u result=%u", tag.token,
                          static_cast<unsigned>(serviceResult));
                    setServiceProperty(kServiceStatusProperty,
                                       "decode_error");
                    setStatus("protocol_error");
                    return false;
                }
                hasServiceUpdate = true;
                if (volte != nullptr && volteBridgeEnabled()) {
                    const imscompat::VolteRequestKind kind =
                            imscompat::classifyVolteRequest(serviceUpdate, &desiredVolte);
                    if (kind == imscompat::VolteRequestKind::kUnsupported) {
                        uint8_t response[32];
                        size_t responseSize = 0;
                        if (!imscompat::encodeEmptyResponse(tag.token, observedId, 6,
                                                             response, sizeof(response), &responseSize)
                                || !queueValidatedFrame(peerOutput, response, responseSize)
                                || !buffer->discardDecoded(frame.wireSize)) return false;
                        ALOGW("unsupported/ambiguous VoLTE policy token=%u", tag.token);
                        setServiceProperty(kVolteStatusProperty, "unsupported");
                        continue;
                    }
                    bridgeVolte = kind == imscompat::VolteRequestKind::kTranslate;
                }
            }
            const imscompat::ClientFrameAdaptation adaptation =
                    imscompat::adaptClientFrame(
                            profile, buffer->mutableDecodeData(), &frame);
            if (adaptation.action ==
                    imscompat::ClientFrameAction::kError) {
                ALOGE("cannot adapt IMS request token=%u id=%u",
                      tag.token, observedId);
                setStatus("translation_error");
                return false;
            }
            if (adaptation.action ==
                    imscompat::ClientFrameAction::kRespondUnsupported) {
                uint8_t response[32] = {};
                size_t responseSize = 0;
                if (!imscompat::encodeEmptyResponse(tag.token, observedId, 6,
                                                    response,
                                                    sizeof(response),
                                                    &responseSize) ||
                    !queueValidatedFrame(peerOutput, response,
                                         responseSize) ||
                    !buffer->discardDecoded(frame.wireSize)) {
                    ALOGE("cannot queue local IMS unsupported response "
                          "token=%u id=%u", tag.token, observedId);
                    setStatus("backpressure_error");
                    return false;
                }
                ALOGW("IMS request unsupported by obake profile token=%u "
                      "id=%u", tag.token, observedId);
                continue;
            }

            uint64_t nowMs = 0;
            if (!monotonicMilliseconds(&nowMs)) {
                return false;
            }
            const imscompat::AddTransactionResult addResult =
                    tracker->add(tag.token, adaptation.clientId,
                                 adaptation.upstreamId, nowMs);
            if (addResult != imscompat::AddTransactionResult::kAdded) {
                ALOGE("rejecting ambiguous IMS request token=%u id=%u: %s",
                      tag.token, observedId,
                      addResult ==
                                      imscompat::AddTransactionResult::
                                              kDuplicateToken
                              ? "duplicate token"
                              : "transaction table full");
                setStatus("transaction_error");
                return false;
            }
            if (hasServiceUpdate) {
                const imscompat::RecordServiceResult recordResult =
                        services->record(tag.token, serviceUpdate);
                if (recordResult !=
                        imscompat::RecordServiceResult::kRecorded) {
                    // Remove the transaction too; otherwise a later response
                    // could appear tracked while its service update was lost.
                    tracker->match(tag.token, adaptation.upstreamId, nullptr);
                    ALOGE("cannot retain IMS service-status request token=%u: "
                          "%s", tag.token,
                          recordResult ==
                                          imscompat::RecordServiceResult::
                                                  kDuplicateToken
                                  ? "duplicate token"
                                  : "pending table full");
                    setServiceProperty(kServiceStatusProperty,
                                       "tracking_error");
                    setStatus("transaction_error");
                    return false;
                }
                setServiceProperty(kServiceStatusProperty, "pending");
                if (bridgeVolte) {
                    frameworkPolicySeen = true;
                    setServiceProperty("sys.ims.volte.intent", desiredVolte ? "1" : "0");
                    if (!volte->record(tag.token, desiredVolte, nowMs)) {
                        ALOGE("VoLTE policy queue full/duplicate token=%u", tag.token);
                        setServiceProperty(kVolteStatusProperty, "queue_error");
                        setStatus("backpressure_error");
                        return false;
                    }
                    setServiceProperty(kVolteStatusProperty, "pending");
                }
            }
        } else if (!clientToUpstream && calls != nullptr && tag.type == 3 &&
                   tag.id == imscompat::kCallStateChangedId) {
            // SU6 sends a bare change notification; Clark expects CallList.
            // Never turn missing data into an empty list/disconnect event.
            if (frame.payloadSize != 0 || (tag.hasError && tag.error != 0)) {
                ALOGE("unexpected legacy call-state notification payload=%zu error=%u",
                      frame.payloadSize, tag.error);
                setServiceProperty(kCallStatusProperty, "protocol_error");
                setStatus("call_sync_error");
                return false;
            }
            calls->markDirty();
            if (!buffer->discardDecoded(frame.wireSize)) {
                ALOGE("cannot consume legacy call-state notification");
                setStatus("internal_error");
                return false;
            }
            continue;
        } else if (!clientToUpstream && calls != nullptr && tag.type == 2 &&
                   tag.token == imscompat::kCallListQueryToken) {
            uint64_t nowMs = 0;
            if (!monotonicMilliseconds(&nowMs)) return false;
            if (calls->expired(nowMs)) {
                ALOGE("late legacy call-list response token=%u", tag.token);
                setServiceProperty(kCallStatusProperty, "timeout");
                setStatus("call_sync_error");
                return false;
            }
            uint8_t notification[imscompat::kMaximumWireFrameSize];
            size_t notificationSize = 0;
            if (!calls->acceptResponse(buffer->decodeData(), frame,
                                        notification, sizeof(notification),
                                        &notificationSize)) {
                ALOGE("legacy call-list response rejected token=%u id=%u error=%u payload=%zu",
                      tag.token, tag.id, tag.error, frame.payloadSize);
                setServiceProperty(kCallStatusProperty, "response_error");
                setStatus("call_sync_error");
                return false;
            }
            if (!buffer->replaceDecoded(frame.wireSize, notification,
                                         notificationSize) ||
                !buffer->commitValidated(notificationSize)) {
                ALOGE("cannot queue translated IMS call list bytes=%zu", notificationSize);
                setServiceProperty(kCallStatusProperty, "queue_error");
                setStatus("backpressure_error");
                return false;
            }
            char value[PROPERTY_VALUE_MAX] = {};
            snprintf(value, sizeof(value), "%zu", calls->activeCount());
            setServiceProperty("sys.ims.compat.call_active", value);
            snprintf(value, sizeof(value), "%zu", calls->endedCount());
            setServiceProperty("sys.ims.compat.call_ended", value);
            snprintf(value, sizeof(value), "%zu", calls->updates());
            setServiceProperty("sys.ims.compat.call_updates", value);
            setServiceProperty(kCallStatusProperty, "synced");
            continue;  // Internal response must never reach Clark's token table.
        } else if (!clientToUpstream && tag.type == 2) {
            imscompat::Transaction request;
            const imscompat::MatchTransactionResult matchResult =
                    tracker->match(tag.token, tag.id, &request);
            if (matchResult ==
                    imscompat::MatchTransactionResult::kIdMismatch) {
                services->cancel(tag.token);
                if (profile == imscompat::ProtocolProfile::kObakeLegacy) {
                    ALOGE("IMS response ID mismatch token=%u expected=%u "
                          "response=%u", tag.token, request.upstreamId,
                          observedId);
                    setStatus("transaction_error");
                    return false;
                }
                ALOGW("IMS response ID mismatch token=%u request=%u "
                      "response=%u", tag.token, request.clientId,
                      observedId);
            } else if (matchResult ==
                       imscompat::MatchTransactionResult::kUnknownToken) {
                if (volte != nullptr && volte->contains(tag.token)) {
                    ALOGE("duplicate native response for held VoLTE token=%u", tag.token);
                    setServiceProperty(kVolteStatusProperty, "tracking_error");
                    return false;
                }
                ALOGW("IMS response has unknown token=%u id=%u", tag.token,
                      observedId);
            } else if (!imscompat::adaptUpstreamResponse(
                               profile, request,
                               buffer->mutableDecodeData(), &frame)) {
                ALOGE("cannot map IMS response token=%u upstream_id=%u "
                      "client_id=%u", tag.token, request.upstreamId,
                      request.clientId);
                setStatus("translation_error");
                return false;
            } else {
                if (request.clientId ==
                        imscompat::kSetServiceStatusRequestId) {
                    const bool successful = !tag.hasError || tag.error == 0;
                    if (volte != nullptr && volte->contains(tag.token) && !successful) {
                        ALOGW("legacy VoLTE service request rejected token=%u error=%u",
                              tag.token, tag.error);
                        setServiceProperty(kVolteStatusProperty, "upstream_rejected");
                    }
                    if (volte != nullptr && volte->upstreamResponse(tag.token, successful)) {
                        // Keep the service update pending until QIPCALL is independently
                        // verified. This legacy empty ACK alone did not enable voice.
                        if (frame.payloadSize != 0 || !buffer->discardDecoded(frame.wireSize)) {
                            ALOGE("unexpected legacy service ACK payload token=%u", tag.token);
                            setServiceProperty(kVolteStatusProperty, "protocol_error");
                            return false;
                        }
                        setServiceProperty(kVolteStatusProperty, "pending");
                        continue;
                    }
                    const imscompat::CompleteServiceResult serviceResult =
                            services->complete(tag.token, successful);
                    if (serviceResult ==
                            imscompat::CompleteServiceResult::kStateFull) {
                        ALOGE("canonical IMS service table full token=%u",
                              tag.token);
                        setServiceProperty(kServiceStatusProperty,
                                           "state_full");
                        setStatus("internal_error");
                        return false;
                    }
                    if (serviceResult ==
                            imscompat::CompleteServiceResult::kUnknownToken) {
                        ALOGE("missing pending IMS service update token=%u",
                              tag.token);
                        setServiceProperty(kServiceStatusProperty,
                                           "tracking_error");
                        setStatus("transaction_error");
                        return false;
                    }
                    publishServiceState(
                            *services,
                            serviceResult ==
                                            imscompat::CompleteServiceResult::
                                                    kApplied
                                    ? "accepted"
                                    : "rejected");
                    if (serviceResult ==
                            imscompat::CompleteServiceResult::kApplied) {
                        publisher->markDirty();
                        uint64_t nowMs = 0;
                        if (!monotonicMilliseconds(&nowMs)) return false;
                        publisher->tick(*services, nowMs);
                    }
                }
            }
        }

        // Other complete frames retain their opaque payload. The call-list
        // bridge above is the only message-specific payload transformation.
        if (!buffer->commitValidated(frame.wireSize)) {
            ALOGE("internal IMS frame-buffer validation overflow");
            setStatus("internal_error");
            return false;
        }
    }
    return true;
}

bool isAllowedClient(int fd) {
    struct ucred credential = {};
    socklen_t size = sizeof(credential);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credential, &size) < 0) {
        ALOGE("cannot read IMS client credentials: %s", strerror(errno));
        return false;
    }

    if (credential.uid != AID_ROOT && credential.uid != AID_SYSTEM &&
            credential.uid != AID_RADIO) {
        ALOGW("rejecting IMS client uid=%u pid=%d", credential.uid,
              credential.pid);
        return false;
    }
    return true;
}

int connectUpstream() {
    useconds_t delay = kInitialRetryDelayUs;
    int lastError = 0;

    for (unsigned attempt = 1; attempt <= kConnectAttempts; ++attempt) {
        const int fd = socket_local_client(kUpstreamSocketPath,
                                           ANDROID_SOCKET_NAMESPACE_FILESYSTEM,
                                           SOCK_STREAM);
        if (fd >= 0) {
            return fd;
        }

        lastError = errno;
        if (attempt != kConnectAttempts) {
            // The bounded process-local jitter prevents synchronized reconnect
            // loops when rild and the IMS client restart together.
            const useconds_t jitter =
                    static_cast<useconds_t>((getpid() * 1103515245u + attempt * 12345u) %
                                            (delay / 4 + 1));
            usleep(delay + jitter);
            delay = std::min(delay * 2, kMaximumRetryDelayUs);
        }
    }

    errno = lastError;
    ALOGW("upstream %s unavailable after %u attempts: %s", kUpstreamSocketPath,
          kConnectAttempts, strerror(lastError));
    return -1;
}

int createListener() {
    static_assert(sizeof(kClientSocketPath) <=
                          sizeof(static_cast<sockaddr_un*>(nullptr)->sun_path),
                  "IMS client socket path is too long");

    struct stat existing = {};
    if (lstat(kClientSocketPath, &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode) && !S_ISLNK(existing.st_mode)) {
            ALOGE("refusing to replace non-socket path %s", kClientSocketPath);
            return -1;
        }
        if (unlink(kClientSocketPath) < 0) {
            ALOGE("cannot remove stale %s: %s", kClientSocketPath,
                  strerror(errno));
            return -1;
        }
    } else if (errno != ENOENT) {
        ALOGE("cannot inspect %s: %s", kClientSocketPath, strerror(errno));
        return -1;
    }

    const int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) {
        ALOGE("cannot create client socket: %s", strerror(errno));
        return -1;
    }

    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, kClientSocketPath);

    const mode_t oldMask = umask(0077);
    const int bindResult = bind(listener, reinterpret_cast<sockaddr*>(&address),
                                sizeof(address));
    umask(oldMask);
    if (bindResult < 0) {
        ALOGE("cannot bind %s: %s", kClientSocketPath, strerror(errno));
        close(listener);
        return -1;
    }

    // ims_compatd runs as radio and qmux_radio is setgid radio, so the new
    // socket has the same radio:radio ownership as stock rild_ims.
    if (chmod(kClientSocketPath, 0660) < 0) {
        ALOGE("cannot set mode on %s: %s", kClientSocketPath,
              strerror(errno));
        unlink(kClientSocketPath);
        close(listener);
        return -1;
    }
    if (listen(listener, 1) < 0) {
        ALOGE("cannot listen on %s: %s", kClientSocketPath, strerror(errno));
        unlink(kClientSocketPath);
        close(listener);
        return -1;
    }
    return listener;
}

bool readInto(int fd, imscompat::FrameBuffer* buffer, bool* inputOpen,
              const char* direction,
              bool clientToUpstream,
              imscompat::TransactionTracker* tracker,
              imscompat::ProtocolProfile profile,
              imscompat::FrameBuffer* peerOutput,
              imscompat::CallListBridge* calls,
              imscompat::VoltePolicyBridge* volte,
              imscompat::ServiceStateTracker* services,
              imscompat::ServiceStatePublisher* publisher) {
    const size_t capacity = buffer->appendCapacity();
    const ssize_t count = read(fd, buffer->appendData(), capacity);
    if (count > 0) {
        if (!buffer->commitAppend(static_cast<size_t>(count))) {
            ALOGE("internal IMS frame-buffer append overflow");
            setStatus("internal_error");
            return false;
        }
        return validateFrames(buffer, clientToUpstream, tracker,
                              profile, peerOutput, calls, volte, services, publisher);
    }
    if (count == 0) {
        if (buffer->decodeSize() != 0) {
            ALOGE("%s closed with a truncated IMS frame (%zu bytes buffered)",
                  direction, buffer->decodeSize());
            setStatus("protocol_error");
            return false;
        }
        *inputOpen = false;
        return true;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return true;
    }

    ALOGW("%s read failed: %s", direction, strerror(errno));
    setStatus("io_error");
    return false;
}

bool writeFrom(int fd, imscompat::FrameBuffer* buffer, const char* direction,
               bool* transferred) {
    const ssize_t count = write(fd, buffer->writeData(), buffer->writeSize());
    if (count > 0) {
        *transferred = true;
        if (!buffer->commitWrite(static_cast<size_t>(count))) {
            ALOGE("internal IMS frame-buffer write overflow");
            setStatus("internal_error");
            return false;
        }
        return true;
    }
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return true;
    }

    ALOGW("%s write failed: %s", direction,
          count == 0 ? "zero-byte write" : strerror(errno));
    setStatus("io_error");
    return false;
}

bool relayConnection(int client, int upstream,
                     imscompat::ProtocolProfile profile,
                     imscompat::VolteWorker* volteWorker,
                     imscompat::ServiceStateTracker* services,
                     imscompat::ServiceStatePublisher* publisher) {
    if (!setNonBlocking(client) || !setNonBlocking(upstream)) {
        return false;
    }

    imscompat::FrameBuffer clientToUpstream;
    imscompat::FrameBuffer upstreamToClient;
    imscompat::CallListBridge callList;
    imscompat::VoltePolicyBridge volteBridge;
    char volteMode[PROPERTY_VALUE_MAX] = {};
    property_get(kVolteBridgeProperty, volteMode, "1");
    if (strcmp(volteMode, "0") != 0 && strcmp(volteMode, "1") != 0) {
        ALOGE("invalid %s=%s", kVolteBridgeProperty, volteMode);
        setServiceProperty(kVolteStatusProperty, "invalid_mode");
        return false;
    }
    imscompat::VoltePolicyBridge* volte =
            profile == imscompat::ProtocolProfile::kObakeLegacy ? &volteBridge : nullptr;
    setServiceProperty(kVolteStatusProperty, volte != nullptr && volteBridgeEnabled()
            ? "waiting" : "disabled");
    setServiceProperty("sys.ims.volte.applied", "unknown");
    setServiceProperty("sys.ims.volte.desired", "unknown");
    char callMode[PROPERTY_VALUE_MAX] = {};
    property_get(kCallBridgeProperty, callMode, "1");
    if (strcmp(callMode, "0") != 0 && strcmp(callMode, "1") != 0) {
        ALOGE("invalid %s=%s", kCallBridgeProperty, callMode);
        setStatus("invalid_mode");
        return false;
    }
    imscompat::CallListBridge* calls =
            profile == imscompat::ProtocolProfile::kObakeLegacy &&
            strcmp(callMode, "1") == 0 ? &callList : nullptr;
    setServiceProperty("sys.ims.compat.call_bridge", "call_list_bridge_v1");
    setServiceProperty(kCallStatusProperty, calls ? "idle" : "disabled");
    setServiceProperty("sys.ims.compat.call_active", "0");
    setServiceProperty("sys.ims.compat.call_ended", "0");
    setServiceProperty("sys.ims.compat.call_updates", "0");
    // Initial resync is read-only and occurs once per connected session.
    if (calls != nullptr) calls->markDirty();
    bool clientInputOpen = true;
    bool upstreamInputOpen = true;
    bool sentToUpstream = false;
    bool sentToClient = false;
    bool trafficVerified = false;
    imscompat::TransactionTracker transactions;

    while (clientInputOpen || upstreamInputOpen ||
           clientToUpstream.hasBufferedData() ||
           upstreamToClient.hasBufferedData()) {
        if (!expireTransactions(&transactions, services)) {
            return false;
        }
        uint64_t nowMs = 0;
        if (!monotonicMilliseconds(&nowMs)) return false;
        if (!pumpVolteBridge(volte, volteWorker, nowMs, &upstreamToClient, services, publisher)) {
            return false;
        }
        if (calls != nullptr && calls->expired(nowMs)) {
            ALOGE("legacy GET_CURRENT_CALLS timeout token=%u deadline_ms=%llu",
                  imscompat::kCallListQueryToken,
                  static_cast<unsigned long long>(imscompat::kCallListDeadlineMs));
            setServiceProperty(kCallStatusProperty, "timeout");
            setStatus("call_sync_error");
            return false;
        }
        if (calls != nullptr && calls->needsQuery()) {
            uint8_t query[32];
            size_t querySize = 0;
            if (!calls->startQuery(nowMs, query, sizeof(query), &querySize) ||
                !queueValidatedFrame(&clientToUpstream, query, querySize)) {
                ALOGE("cannot queue legacy GET_CURRENT_CALLS token=%u",
                      imscompat::kCallListQueryToken);
                setServiceProperty(kCallStatusProperty, "queue_error");
                setStatus("backpressure_error");
                return false;
            }
            setServiceProperty(kCallStatusProperty, "pending");
        }
        publisher->tick(*services, nowMs);

        struct pollfd fds[2] = {};
        fds[0].fd = client;
        fds[0].events = (clientInputOpen &&
                         clientToUpstream.appendCapacity() > 0)
                                ? POLLIN
                                : 0;
        if (upstreamToClient.writeSize() != 0) {
            fds[0].events |= POLLOUT;
        }
        fds[1].fd = upstream;
        fds[1].events = (upstreamInputOpen &&
                         upstreamToClient.appendCapacity() > 0)
                                ? POLLIN
                                : 0;
        if (clientToUpstream.writeSize() != 0) {
            fds[1].events |= POLLOUT;
        }

        // A bounded timeout lets transaction deadlines expire even if both
        // peers remain connected but silent.
        const int result = poll(fds, 2, 1000);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            ALOGW("relay poll failed: %s", strerror(errno));
            setStatus("io_error");
            return false;
        }
        if (result == 0) {
            continue;
        }

        if ((fds[0].revents & POLLIN) &&
                !readInto(client, &clientToUpstream, &clientInputOpen,
                          "client-to-upstream", true, &transactions,
                          profile, &upstreamToClient, calls, volte, services, publisher)) {
            return false;
        }
        if ((fds[1].revents & POLLOUT) &&
                !writeFrom(upstream, &clientToUpstream, "client-to-upstream",
                           &sentToUpstream)) {
            return false;
        }
        if ((fds[1].revents & POLLIN) &&
                !readInto(upstream, &upstreamToClient, &upstreamInputOpen,
                          "upstream-to-client", false, &transactions,
                          profile, &clientToUpstream, calls, volte, services, publisher)) {
            return false;
        }
        if (fds[0].revents & POLLOUT) {
            // Generated and forwarded frames share one ordered output queue,
            // including when a preceding frame has been partially written.
            if (!writeFrom(client, &upstreamToClient, "upstream-to-client", &sentToClient)) {
                return false;
            }
        }

        if (!trafficVerified && sentToUpstream && sentToClient) {
            setStatus("active");
            trafficVerified = true;
        }

        const short fatalEvents = POLLERR | POLLNVAL;
        if ((fds[0].revents & fatalEvents) || (fds[1].revents & fatalEvents)) {
            ALOGW("relay socket error: client=0x%x upstream=0x%x",
                  fds[0].revents, fds[1].revents);
            setStatus("io_error");
            return false;
        }
        // POLLHUP can be reported while unread bytes remain. Only a zero-byte
        // read is allowed to close an input direction, so drain before EOF.
        if ((fds[0].revents & POLLHUP) && !(fds[0].revents & POLLIN) &&
                clientInputOpen && clientToUpstream.appendCapacity() > 0 &&
                !readInto(client, &clientToUpstream, &clientInputOpen,
                          "client-to-upstream", true, &transactions,
                          profile, &upstreamToClient, calls, volte, services, publisher)) {
            return false;
        }
        if ((fds[1].revents & POLLHUP) && !(fds[1].revents & POLLIN) &&
                upstreamInputOpen && upstreamToClient.appendCapacity() > 0 &&
                !readInto(upstream, &upstreamToClient, &upstreamInputOpen,
                          "upstream-to-client", false, &transactions,
                          profile, &clientToUpstream, calls, volte, services, publisher)) {
            return false;
        }

        // This daemon handles one client session at a time. Once either peer
        // closes its input, keeping the opposite half open can never produce a
        // useful complete exchange: the response has no consumer, or the
        // request has no upstream. In particular, legacy rild_ims keeps its
        // socket open after a client exits. End the session here so main() can
        // close both descriptors and accept a restarted IMS client.
        if (!clientInputOpen || !upstreamInputOpen) {
            return true;
        }
    }

    return true;
}

}  // namespace

int main() {
    signal(SIGPIPE, SIG_IGN);
    setStatus("starting");

    imscompat::ProtocolProfile profile;
    if (!loadProtocolProfile(&profile)) {
        return EXIT_FAILURE;
    }

    const int listener = createListener();
    if (listener < 0) {
        setStatus("failed_listen");
        return EXIT_FAILURE;
    }
    setStatus("listening");
    imscompat::ServiceStateTracker services;
    imscompat::ServiceStatePublisher publisher;
    imscompat::VolteWorker volteWorker;
    publishServiceState(services, "waiting");
    earlyPolicyAllowed = profile == imscompat::ProtocolProfile::kObakeLegacy;
    char existingIntent[PROPERTY_VALUE_MAX] = {};
    property_get("sys.ims.volte.intent", existingIntent, "");
    frameworkPolicySeen = strcmp(existingIntent, "0") == 0 || strcmp(existingIntent, "1") == 0;

    for (;;) {
        uint64_t nowMs = 0;
        if (!monotonicMilliseconds(&nowMs)) return EXIT_FAILURE;
        pumpVolteBridge(nullptr, &volteWorker, nowMs, nullptr, &services, &publisher);
        // Also pump before a framework client connects. Call/hangup relay
        // remains nonblocking while the isolated QIPCALL child does its work.
        pollfd ready = {listener, POLLIN, 0};
        const int polled = poll(&ready, 1, 250);
        if (polled == 0 || (polled < 0 && errno == EINTR)) continue;
        if (polled < 0 || !(ready.revents & POLLIN)) {
            ALOGE("IMS listener poll failed revents=%d errno=%d", ready.revents, errno);
            return EXIT_FAILURE;
        }
        const int client = accept(listener, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            ALOGE("accept on %s failed: %s", kClientSocketPath,
                  strerror(errno));
            setStatus("failed_accept");
            return EXIT_FAILURE;
        }

        if (!isAllowedClient(client)) {
            setStatus("client_rejected");
            close(client);
            continue;
        }

        setStatus("connecting");
        const int upstream = connectUpstream();
        if (upstream < 0) {
            setStatus("upstream_unavailable");
            close(client);
            continue;
        }

        setStatus("connected");
        const bool cleanDisconnect = relayConnection(client, upstream,
                                                     profile, &volteWorker, &services,
                                                     &publisher);
        // Never retain a previous client's pending policy across reconnection/SIM changes.
        volteWorker.cancel();
        if (earlyPolicyRunning) {
            earlyPolicyRunning = false;
            setServiceProperty("sys.ims.volte.early", "cancelled");
        }
        if (cleanDisconnect) setServiceProperty(kVolteStatusProperty, "disconnected");
        services.clearPending();
        close(upstream);
        close(client);
        if (cleanDisconnect) {
            setStatus("listening");
        }
    }
}
