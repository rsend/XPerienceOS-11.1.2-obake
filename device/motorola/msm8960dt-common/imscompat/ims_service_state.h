/*
 * Copyright (C) 2026 The XPerience Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef IMS_SERVICE_STATE_H
#define IMS_SERVICE_STATE_H

#include "ims_frame_codec.h"

#include <stddef.h>
#include <stdint.h>

namespace imscompat {

constexpr uint32_t kSetServiceStatusRequestId = 30;
constexpr size_t kMaximumAccessUpdates = 8;
constexpr size_t kMaximumCanonicalServices = 32;

// These values are part of the CodeAurora ImsQmiIF wire ABI. Unknown values
// remain valid raw observations; they are simply not assigned a summary bit.
constexpr uint32_t kCallTypeVoice = 0;
constexpr uint32_t kCallTypeVideoTx = 1;
constexpr uint32_t kCallTypeVideoRx = 2;
constexpr uint32_t kCallTypeVideo = 3;
constexpr uint32_t kCallTypeVideoNoDirection = 4;
constexpr uint32_t kCallTypeSms = 10;
constexpr uint32_t kCallTypeUt = 11;
constexpr uint32_t kCallTypeVideoPause = 12;
constexpr uint32_t kCallTypeVideoResume = 13;
constexpr uint32_t kRadioTechLte = 14;
// ImsQmiIF uses 19 for IWLAN even though the framework-facing radio-tech
// constant used by ImsConfig is 18. Captured request-30 payloads confirm 19.
constexpr uint32_t kRadioTechIwlan = 19;
constexpr uint32_t kStatusPartiallyEnabled = 1;
constexpr uint32_t kStatusEnabled = 2;

struct AccessStatusUpdate {
    uint32_t networkMode = 0;
    uint32_t status = 0;
    uint32_t restrictionCause = 0;
    bool hasNetworkMode = false;
    bool hasStatus = false;
    bool hasRestrictionCause = false;
    bool hasRegistration = false;
};

struct ServiceStatusUpdate {
    uint32_t serviceType = 1;
    uint32_t callType = 0;
    uint32_t status = 0;
    uint32_t restrictionCause = 0;
    bool isValid = false;
    bool hasIsValid = false;
    bool hasServiceType = false;
    bool hasCallType = false;
    bool hasStatus = false;
    bool hasRestrictionCause = false;
    AccessStatusUpdate access[kMaximumAccessUpdates] = {};
    size_t accessCount = 0;
};

enum class ServiceDecodeResult {
    kDecoded,
    kNotServiceStatusRequest,
    kMalformed,
    kTooManyAccessUpdates,
};

ServiceDecodeResult decodeServiceStatusRequest(const uint8_t* wire,
                                               const DecodedFrame& frame,
                                               ServiceStatusUpdate* update);

struct CanonicalServiceEntry {
    uint32_t serviceType = 0;
    uint32_t callType = 0;
    uint32_t networkMode = 0;
    uint32_t status = 0;
    uint32_t restrictionCause = 0;
    bool valid = false;
};

enum class RecordServiceResult {
    kRecorded,
    kDuplicateToken,
    kFull,
};

enum class CompleteServiceResult {
    kApplied,
    kRejected,
    kUnknownToken,
    kStateFull,
};

// Holds request-30 observations until the matching modem/RIL response says the
// request succeeded. This prevents rejected or timed-out requests from being
// exposed as accepted service state.
class ServiceStateTracker {
  public:
    RecordServiceResult record(uint32_t token,
                               const ServiceStatusUpdate& update);
    CompleteServiceResult complete(uint32_t token, bool successful);
    bool cancel(uint32_t token);
    void clearPending();

    uint32_t enabledMask() const;
    uint32_t partialMask() const;
    size_t acceptedUpdates() const;
    size_t pendingCount() const;
    size_t entryCount() const;
    const CanonicalServiceEntry* entries() const;

  private:
    struct PendingUpdate {
        uint32_t token = 0;
        ServiceStatusUpdate update;
        bool active = false;
    };

    bool apply(const ServiceStatusUpdate& update);
    uint32_t summarize(uint32_t status) const;

    PendingUpdate pending_[kMaximumTransactions] = {};
    CanonicalServiceEntry entries_[kMaximumCanonicalServices] = {};
    size_t acceptedUpdates_ = 0;
};

}  // namespace imscompat

#endif  // IMS_SERVICE_STATE_H
