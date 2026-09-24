/* Copyright (C) 2026 The XPerience Project */

#define LOG_TAG "ims_compatd"

#include "ims_service_publisher.h"

#include "ims_service_control.h"

#include <cutils/properties.h>
#include <log/log.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>

namespace imscompat {
namespace {

constexpr char kChannelStatusProperty[] = "sys.ims.svc.channel";
constexpr char kChannelSequenceProperty[] = "sys.ims.svc.sequence";
constexpr char kReceiverStatusProperty[] = "sys.ims.dpl.svc.status";
constexpr char kReceiverSequenceProperty[] = "sys.ims.dpl.svc.sequence";
constexpr int kIoTimeoutMs = 200;
constexpr uint64_t kInitialRetryMs = 1000;
constexpr uint64_t kMaximumRetryMs = 30000;
constexpr uint64_t kReceiverCheckIntervalMs = 5000;

void setProperty(const char* name, const char* value) {
    if (property_set(name, value) != 0) {
        ALOGW("cannot set %s=%s", name, value);
    }
}

bool setNonBlocking(int descriptor) {
    const int flags = fcntl(descriptor, F_GETFL, 0);
    return flags >= 0 &&
           fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool waitFor(int descriptor, short events) {
    struct pollfd descriptorState = {};
    descriptorState.fd = descriptor;
    descriptorState.events = events;
    int result;
    do {
        result = poll(&descriptorState, 1, kIoTimeoutMs);
    } while (result < 0 && errno == EINTR);
    // A one-shot peer may send its complete ACK and close before poll(2)
    // returns. Linux then reports POLLIN|POLLHUP; the queued record must still
    // be consumed. POLLERR and POLLNVAL remain fatal, while connectBounded()
    // verifies a writable socket with SO_ERROR after this check.
    return result == 1 && (descriptorState.revents & events) != 0 &&
           (descriptorState.revents & (POLLERR | POLLNVAL)) == 0;
}

bool connectBounded(int descriptor, const struct sockaddr_un& address) {
    if (connect(descriptor, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == 0) {
        return true;
    }
    if (errno != EINPROGRESS || !waitFor(descriptor, POLLOUT)) return false;
    int socketError = 0;
    socklen_t socketErrorSize = sizeof(socketError);
    return getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &socketError,
                      &socketErrorSize) == 0 && socketError == 0;
}

bool shouldLogFailure(uint32_t failures) {
    return failures == 1 || (failures & (failures - 1)) == 0;
}

}  // namespace

void ServiceStatePublisher::markDirty() {
    ++sequence_;
    if (sequence_ == 0) ++sequence_;
    failures_ = 0;
    nextAttemptMs_ = 0;
    dirty_ = true;
    setProperty(kChannelStatusProperty, "pending");
}

void ServiceStatePublisher::tick(const ServiceStateTracker& services,
                                 uint64_t nowMs) {
    if (!dirty_ && sequence_ != 0 && nowMs >= nextReceiverCheckMs_) {
        nextReceiverCheckMs_ = nowMs + kReceiverCheckIntervalMs;
        char status[PROPERTY_VALUE_MAX] = {};
        char sequence[PROPERTY_VALUE_MAX] = {};
        property_get(kReceiverStatusProperty, status, "unavailable");
        property_get(kReceiverSequenceProperty, sequence, "0");
        errno = 0;
        char* end = nullptr;
        const unsigned long received = strtoul(sequence, &end, 10);
        if (strcmp(status, "received") != 0 || errno != 0 ||
                end == sequence || *end != '\0' || received != sequence_) {
            dirty_ = true;
            failures_ = 0;
            nextAttemptMs_ = 0;
            setProperty(kChannelStatusProperty, "pending");
        }
    }
    if (!dirty_ || nowMs < nextAttemptMs_) return;
    if (sendSnapshot(services)) {
        dirty_ = false;
        failures_ = 0;
        nextAttemptMs_ = 0;
        nextReceiverCheckMs_ = nowMs + kReceiverCheckIntervalMs;
        char value[16] = {};
        snprintf(value, sizeof(value), "%u", sequence_);
        setProperty(kChannelSequenceProperty, value);
        setProperty(kChannelStatusProperty, "delivered");
        return;
    }
    scheduleRetry(nowMs);
}

bool ServiceStatePublisher::dirty() const {
    return dirty_;
}

bool ServiceStatePublisher::sendSnapshot(
        const ServiceStateTracker& services) {
    uint8_t request[kMaximumServiceSnapshotSize] = {};
    size_t requestSize = 0;
    if (!encodeServiceStateSnapshot(services, sequence_, request,
                                    sizeof(request), &requestSize)) {
        setProperty(kChannelStatusProperty, "encode_error");
        ALOGE("cannot encode IMS service-state snapshot sequence=%u",
              sequence_);
        errno = EMSGSIZE;
        return false;
    }

    const int descriptor = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (descriptor < 0 || !setNonBlocking(descriptor)) {
        if (descriptor >= 0) close(descriptor);
        return false;
    }
    struct sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    static_assert(sizeof(kServiceControlSocketPath) <= sizeof(address.sun_path),
                  "IMS service-state control path is too long");
    memcpy(address.sun_path, kServiceControlSocketPath,
           sizeof(kServiceControlSocketPath));

    bool success = connectBounded(descriptor, address);
    if (success) {
        success = waitFor(descriptor, POLLOUT);
        if (!success) errno = ETIMEDOUT;
    }
    if (success) {
        const ssize_t count = send(descriptor, request, requestSize,
                                   MSG_NOSIGNAL);
        success = count == static_cast<ssize_t>(requestSize);
    }
    uint8_t ack[kServiceAckSize] = {};
    if (success) {
        success = waitFor(descriptor, POLLIN);
        if (!success) errno = ETIMEDOUT;
    }
    if (success) {
        const ssize_t count = recv(descriptor, ack, sizeof(ack), 0);
        uint16_t status = 0xffff;
        success = count == static_cast<ssize_t>(sizeof(ack)) &&
                  decodeServiceStateAck(ack, sizeof(ack), sequence_, &status) &&
                  status == 0;
        if (!success) errno = EPROTO;
    }
    close(descriptor);
    return success;
}

void ServiceStatePublisher::scheduleRetry(uint64_t nowMs) {
    const int failureError = errno;
    ++failures_;
    const uint32_t shift = std::min(failures_ - 1, 5u);
    const uint64_t delay = std::min(kInitialRetryMs << shift,
                                    kMaximumRetryMs);
    const uint64_t jitter =
            (static_cast<uint64_t>(getpid()) * 1103515245u + sequence_) % 251u;
    nextAttemptMs_ = nowMs + delay + jitter;
    setProperty(kChannelStatusProperty, "unavailable");
    if (shouldLogFailure(failures_)) {
        ALOGW("IMS service-state delivery failed sequence=%u attempt=%u "
              "retry_ms=%llu errno=%d detail=%s", sequence_, failures_,
              static_cast<unsigned long long>(delay + jitter), failureError,
              strerror(failureError));
    }
}

}  // namespace imscompat
