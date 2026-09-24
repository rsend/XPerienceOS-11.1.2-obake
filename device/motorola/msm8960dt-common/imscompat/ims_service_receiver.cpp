/* Copyright (C) 2026 The XPerience Project */

#define LOG_TAG "ims_dpl_bootstrapd"

#include "ims_service_receiver.h"

#include "ims_service_control.h"

#include <cutils/properties.h>
#include <cutils/sockets.h>
#include <log/log.h>
#include <private/android_filesystem_config.h>

#include <errno.h>
#include <new>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace imscompat {
namespace {

constexpr char kReceiverStatusProperty[] = "sys.ims.dpl.svc.status";
constexpr char kReceiverUpdatesProperty[] = "sys.ims.dpl.svc.updates";
constexpr char kReceiverEnabledProperty[] = "sys.ims.dpl.svc.enabled";
constexpr char kReceiverPartialProperty[] = "sys.ims.dpl.svc.partial";
constexpr char kReceiverSequenceProperty[] = "sys.ims.dpl.svc.sequence";
constexpr char kReceiverSocketName[] = "ims_service_state";
constexpr int kPeerReadTimeoutMs = 500;

pthread_mutex_t gSnapshotLock = PTHREAD_MUTEX_INITIALIZER;
ServiceStateSnapshot gSnapshot;
bool gHasSnapshot = false;
uint32_t gAcceptedUpdates = 0;

bool sameSnapshot(const ServiceStateSnapshot& left,
                  const ServiceStateSnapshot& right) {
    if (left.sequence != right.sequence ||
            left.enabledMask != right.enabledMask ||
            left.partialMask != right.partialMask ||
            left.entryCount != right.entryCount) {
        return false;
    }
    for (size_t index = 0; index < left.entryCount; ++index) {
        const CanonicalServiceEntry& a = left.entries[index];
        const CanonicalServiceEntry& b = right.entries[index];
        if (a.serviceType != b.serviceType || a.callType != b.callType ||
                a.networkMode != b.networkMode || a.status != b.status ||
                a.restrictionCause != b.restrictionCause) {
            return false;
        }
    }
    return true;
}

void setProperty(const char* name, const char* value) {
    if (property_set(name, value) != 0) {
        ALOGE("cannot set %s=%s", name, value);
    }
}

void setUnsignedProperty(const char* name, uint32_t value, bool hexadecimal) {
    char text[16] = {};
    snprintf(text, sizeof(text), hexadecimal ? "0x%08x" : "%u", value);
    setProperty(name, text);
}

bool allowedPeer(int descriptor) {
    struct ucred credential = {};
    socklen_t size = sizeof(credential);
    if (getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credential, &size) !=
            0) {
        ALOGE("cannot authenticate IMS service-state peer: %s",
              strerror(errno));
        return false;
    }
    if (size != sizeof(credential) ||
            (credential.uid != AID_RADIO && credential.uid != AID_ROOT &&
             credential.uid != AID_SYSTEM)) {
        ALOGE("rejecting IMS service-state peer uid=%u pid=%d",
              credential.uid, credential.pid);
        return false;
    }
    return true;
}

void publishSnapshot(const ServiceStateSnapshot& snapshot) {
    setUnsignedProperty(kReceiverSequenceProperty, snapshot.sequence, false);
    setUnsignedProperty(kReceiverEnabledProperty, snapshot.enabledMask, true);
    setUnsignedProperty(kReceiverPartialProperty, snapshot.partialMask, true);
    setUnsignedProperty(kReceiverUpdatesProperty, gAcceptedUpdates, false);
    setProperty(kReceiverStatusProperty, "received");
}

void processPeer(int descriptor) {
    if (!allowedPeer(descriptor)) return;
    struct pollfd state = {};
    state.fd = descriptor;
    state.events = POLLIN;
    int pollResult;
    do {
        pollResult = poll(&state, 1, kPeerReadTimeoutMs);
    } while (pollResult < 0 && errno == EINTR);
    if (pollResult != 1 || (state.revents & POLLIN) == 0 ||
            (state.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        ALOGE("IMS service-state peer timed out or failed revents=0x%x",
              state.revents);
        return;
    }

    uint8_t wire[kMaximumServiceSnapshotSize] = {};
    const ssize_t count = recv(descriptor, wire, sizeof(wire), MSG_TRUNC);
    ServiceStateSnapshot candidate;
    const ServiceControlDecodeResult result =
            count > 0 && static_cast<size_t>(count) <= sizeof(wire)
                    ? decodeServiceStateSnapshot(
                              wire, static_cast<size_t>(count), &candidate)
                    : ServiceControlDecodeResult::kMalformed;
    uint16_t ackStatus = 0;
    uint32_t ackSequence = result == ServiceControlDecodeResult::kDecoded
                                   ? candidate.sequence
                                   : 0;
    if (result != ServiceControlDecodeResult::kDecoded) {
        ackStatus = 1;
        setProperty(kReceiverStatusProperty, "decode_error");
        ALOGE("rejecting IMS service-state snapshot result=%u size=%zd",
              static_cast<unsigned>(result), count);
    } else {
        pthread_mutex_lock(&gSnapshotLock);
        if (!gHasSnapshot || !sameSnapshot(candidate, gSnapshot)) {
            gSnapshot = candidate;
            gHasSnapshot = true;
            ++gAcceptedUpdates;
            publishSnapshot(gSnapshot);
        }
        pthread_mutex_unlock(&gSnapshotLock);
    }

    uint8_t ack[kServiceAckSize] = {};
    if (!encodeServiceStateAck(ackSequence, ackStatus, ack, sizeof(ack)) ||
            send(descriptor, ack, sizeof(ack), MSG_NOSIGNAL) !=
                    static_cast<ssize_t>(sizeof(ack))) {
        ALOGE("cannot acknowledge IMS service-state snapshot sequence=%u: %s",
              ackSequence, strerror(errno));
    }
}

void* receiverThread(void* argument) {
    const int listener = *static_cast<int*>(argument);
    delete static_cast<int*>(argument);
    for (;;) {
        int descriptor;
        do {
            descriptor = accept(listener, nullptr, nullptr);
        } while (descriptor < 0 && errno == EINTR);
        if (descriptor < 0) {
            ALOGE("IMS service-state accept failed: %s", strerror(errno));
            setProperty(kReceiverStatusProperty, "accept_error");
            sleep(1);
            continue;
        }
        processPeer(descriptor);
        close(descriptor);
    }
    return nullptr;
}

}  // namespace

bool startServiceStateReceiver() {
    setUnsignedProperty(kReceiverSequenceProperty, 0, false);
    setUnsignedProperty(kReceiverEnabledProperty, 0, true);
    setUnsignedProperty(kReceiverPartialProperty, 0, true);
    setUnsignedProperty(kReceiverUpdatesProperty, 0, false);
    setProperty(kReceiverStatusProperty, "starting");
    const int listener = android_get_control_socket(kReceiverSocketName);
    if (listener < 0) {
        ALOGE("cannot obtain init control socket %s: %s",
              kReceiverSocketName, strerror(errno));
        setProperty(kReceiverStatusProperty, "socket_error");
        return false;
    }
    if (listen(listener, 4) != 0) {
        ALOGE("cannot listen on IMS service-state control socket: %s",
              strerror(errno));
        setProperty(kReceiverStatusProperty, "listen_error");
        return false;
    }

    int* threadListener = new (std::nothrow) int(listener);
    if (threadListener == nullptr) {
        setProperty(kReceiverStatusProperty, "allocation_error");
        return false;
    }
    pthread_attr_t attributes;
    int createResult = pthread_attr_init(&attributes);
    const bool attributesInitialized = createResult == 0;
    if (createResult == 0) {
        createResult = pthread_attr_setdetachstate(
                &attributes, PTHREAD_CREATE_DETACHED);
    }
    pthread_t thread;
    if (createResult == 0) {
        createResult = pthread_create(&thread, &attributes, receiverThread,
                                      threadListener);
    }
    if (attributesInitialized) pthread_attr_destroy(&attributes);
    if (createResult != 0) {
        ALOGE("cannot start IMS service-state receiver thread: %s",
              strerror(createResult));
        setProperty(kReceiverStatusProperty, "thread_error");
        delete threadListener;
        return false;
    }
    setProperty(kReceiverStatusProperty, "listening");
    return true;
}

bool getServiceStateSnapshot(ServiceStateSnapshot* snapshot) {
    if (snapshot == nullptr) return false;
    pthread_mutex_lock(&gSnapshotLock);
    const bool available = gHasSnapshot;
    if (available) *snapshot = gSnapshot;
    pthread_mutex_unlock(&gSnapshotLock);
    return available;
}

}  // namespace imscompat
