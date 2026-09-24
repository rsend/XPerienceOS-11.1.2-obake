/*
 * Copyright (C) 2026 The XPerience Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "ims_service_state.h"

#include <string.h>

namespace imscompat {
namespace {

enum class VarintResult {
    kOk,
    kTruncated,
    kMalformed,
};

VarintResult readVarint32(const uint8_t* data, size_t size, size_t* offset,
                          uint32_t* value) {
    uint32_t result = 0;
    for (unsigned index = 0; index < 5; ++index) {
        if (*offset >= size) return VarintResult::kTruncated;
        const uint8_t byte = data[(*offset)++];
        if (index == 4 && (byte & 0xf0) != 0) {
            return VarintResult::kMalformed;
        }
        result |= static_cast<uint32_t>(byte & 0x7f) << (index * 7);
        if ((byte & 0x80) == 0) {
            *value = result;
            return VarintResult::kOk;
        }
    }
    return VarintResult::kMalformed;
}

bool readFixed32(const uint8_t* data, size_t size, size_t* offset,
                 uint32_t* value) {
    if (*offset > size || size - *offset < 4) return false;
    const uint8_t* bytes = data + *offset;
    *value = static_cast<uint32_t>(bytes[0]) |
             (static_cast<uint32_t>(bytes[1]) << 8) |
             (static_cast<uint32_t>(bytes[2]) << 16) |
             (static_cast<uint32_t>(bytes[3]) << 24);
    *offset += 4;
    return true;
}

bool readLengthDelimited(const uint8_t* data, size_t size, size_t* offset,
                         const uint8_t** value, size_t* valueSize) {
    uint32_t length = 0;
    if (readVarint32(data, size, offset, &length) != VarintResult::kOk ||
            *offset > size || length > size - *offset) {
        return false;
    }
    *value = data + *offset;
    *valueSize = length;
    *offset += length;
    return true;
}

bool skipField(const uint8_t* data, size_t size, size_t* offset,
               uint32_t wireType) {
    uint32_t unused = 0;
    const uint8_t* bytes = nullptr;
    size_t byteCount = 0;
    switch (wireType) {
        case 0:
            return readVarint32(data, size, offset, &unused) ==
                   VarintResult::kOk;
        case 1:
            if (*offset > size || size - *offset < 8) return false;
            *offset += 8;
            return true;
        case 2:
            return readLengthDelimited(data, size, offset, &bytes,
                                       &byteCount);
        case 5:
            return readFixed32(data, size, offset, &unused);
        default:
            return false;
    }
}

bool parseAccessStatus(const uint8_t* data, size_t size,
                       AccessStatusUpdate* status) {
    size_t offset = 0;
    while (offset < size) {
        uint32_t key = 0;
        if (readVarint32(data, size, &offset, &key) != VarintResult::kOk ||
                key == 0) {
            return false;
        }
        const uint32_t field = key >> 3;
        const uint32_t wireType = key & 7;
        uint32_t value = 0;
        const uint8_t* nested = nullptr;
        size_t nestedSize = 0;
        if (field == 1) {
            if (wireType != 0 || status->hasNetworkMode ||
                    readVarint32(data, size, &offset, &value) !=
                            VarintResult::kOk) {
                return false;
            }
            status->networkMode = value;
            status->hasNetworkMode = true;
        } else if (field == 2) {
            if (wireType != 0 || status->hasStatus ||
                    readVarint32(data, size, &offset, &value) !=
                            VarintResult::kOk) {
                return false;
            }
            status->status = value;
            status->hasStatus = true;
        } else if (field == 3) {
            if (wireType != 5 || status->hasRestrictionCause ||
                    !readFixed32(data, size, &offset, &value)) {
                return false;
            }
            status->restrictionCause = value;
            status->hasRestrictionCause = true;
        } else if (field == 4) {
            if (wireType != 2 || status->hasRegistration ||
                    !readLengthDelimited(data, size, &offset, &nested,
                                         &nestedSize)) {
                return false;
            }
            // Registration is retained as presence only in Step 2. Its bytes
            // are bounded by the enclosing message and are not interpreted.
            status->hasRegistration = true;
        } else if (!skipField(data, size, &offset, wireType)) {
            return false;
        }
    }
    return status->hasNetworkMode && status->hasStatus;
}

bool parseInfo(const uint8_t* data, size_t size, ServiceStatusUpdate* update,
               bool* tooManyAccessUpdates) {
    size_t offset = 0;
    while (offset < size) {
        uint32_t key = 0;
        if (readVarint32(data, size, &offset, &key) != VarintResult::kOk ||
                key == 0) {
            return false;
        }
        const uint32_t field = key >> 3;
        const uint32_t wireType = key & 7;
        uint32_t value = 0;
        const uint8_t* nested = nullptr;
        size_t nestedSize = 0;
        if (field >= 1 && field <= 4) {
            if (wireType != 0 ||
                    readVarint32(data, size, &offset, &value) !=
                            VarintResult::kOk) {
                return false;
            }
            if (field == 1) {
                if (update->hasIsValid || value > 1) return false;
                update->isValid = value != 0;
                update->hasIsValid = true;
            } else if (field == 2) {
                if (update->hasServiceType) return false;
                update->serviceType = value;
                update->hasServiceType = true;
            } else if (field == 3) {
                if (update->hasCallType) return false;
                update->callType = value;
                update->hasCallType = true;
            } else {
                if (update->hasStatus) return false;
                update->status = value;
                update->hasStatus = true;
            }
        } else if (field == 5) {
            if (wireType != 2 ||
                    !readLengthDelimited(data, size, &offset, &nested,
                                         &nestedSize)) {
                return false;
            }
        } else if (field == 6) {
            if (wireType != 5 || update->hasRestrictionCause ||
                    !readFixed32(data, size, &offset, &value)) {
                return false;
            }
            update->restrictionCause = value;
            update->hasRestrictionCause = true;
        } else if (field == 7) {
            if (wireType != 2 ||
                    !readLengthDelimited(data, size, &offset, &nested,
                                         &nestedSize)) {
                return false;
            }
            if (update->accessCount == kMaximumAccessUpdates) {
                *tooManyAccessUpdates = true;
                return false;
            }
            if (!parseAccessStatus(nested, nestedSize,
                                   &update->access[update->accessCount])) {
                return false;
            }
            ++update->accessCount;
        } else if (!skipField(data, size, &offset, wireType)) {
            return false;
        }
    }
    // The generated Qualcomm message defaults serviceType to VOIP (1), but a
    // valid request must explicitly identify validity and call type. Access
    // entries are optional so a valid disable operation can clear a service.
    return update->hasIsValid && update->hasCallType;
}

uint32_t capabilityIndex(uint32_t callType, uint32_t networkMode) {
    uint32_t serviceOffset = 0;
    if (callType == kCallTypeVoice) {
        serviceOffset = 0;
    } else if (callType == kCallTypeVideoTx || callType == kCallTypeVideoRx ||
               callType == kCallTypeVideo ||
               callType == kCallTypeVideoNoDirection ||
               callType == kCallTypeVideoPause ||
               callType == kCallTypeVideoResume) {
        serviceOffset = 1;
    } else if (callType == kCallTypeSms) {
        serviceOffset = 2;
    } else if (callType == kCallTypeUt) {
        serviceOffset = 3;
    } else {
        return 32;
    }

    if (networkMode == kRadioTechLte) return serviceOffset;
    if (networkMode == kRadioTechIwlan) return 4 + serviceOffset;
    return 32;
}

}  // namespace

ServiceDecodeResult decodeServiceStatusRequest(const uint8_t* wire,
                                               const DecodedFrame& frame,
                                               ServiceStatusUpdate* update) {
    if (wire == nullptr || update == nullptr || frame.metadata.type != 1 ||
            frame.metadata.id != kSetServiceStatusRequestId) {
        return ServiceDecodeResult::kNotServiceStatusRequest;
    }
    if (frame.payloadOffset > frame.wireSize ||
            frame.payloadSize > frame.wireSize - frame.payloadOffset) {
        return ServiceDecodeResult::kMalformed;
    }
    *update = ServiceStatusUpdate{};
    bool tooManyAccessUpdates = false;
    if (!parseInfo(wire + frame.payloadOffset, frame.payloadSize, update,
                   &tooManyAccessUpdates)) {
        return tooManyAccessUpdates
                       ? ServiceDecodeResult::kTooManyAccessUpdates
                       : ServiceDecodeResult::kMalformed;
    }
    return ServiceDecodeResult::kDecoded;
}

RecordServiceResult ServiceStateTracker::record(
        uint32_t token, const ServiceStatusUpdate& update) {
    PendingUpdate* freeEntry = nullptr;
    for (PendingUpdate& entry : pending_) {
        if (entry.active && entry.token == token) {
            return RecordServiceResult::kDuplicateToken;
        }
        if (!entry.active && freeEntry == nullptr) freeEntry = &entry;
    }
    if (freeEntry == nullptr) return RecordServiceResult::kFull;
    freeEntry->token = token;
    freeEntry->update = update;
    freeEntry->active = true;
    return RecordServiceResult::kRecorded;
}

CompleteServiceResult ServiceStateTracker::complete(uint32_t token,
                                                    bool successful) {
    for (PendingUpdate& entry : pending_) {
        if (!entry.active || entry.token != token) continue;
        const ServiceStatusUpdate update = entry.update;
        entry.active = false;
        if (!successful) return CompleteServiceResult::kRejected;
        if (!apply(update)) return CompleteServiceResult::kStateFull;
        ++acceptedUpdates_;
        return CompleteServiceResult::kApplied;
    }
    return CompleteServiceResult::kUnknownToken;
}

bool ServiceStateTracker::cancel(uint32_t token) {
    for (PendingUpdate& entry : pending_) {
        if (entry.active && entry.token == token) {
            entry.active = false;
            return true;
        }
    }
    return false;
}

void ServiceStateTracker::clearPending() {
    for (PendingUpdate& entry : pending_) entry.active = false;
}

bool ServiceStateTracker::apply(const ServiceStatusUpdate& update) {
    CanonicalServiceEntry candidate[kMaximumCanonicalServices] = {};
    memcpy(candidate, entries_, sizeof(candidate));

    if (!update.isValid) {
        for (CanonicalServiceEntry& entry : candidate) {
            if (entry.valid && entry.callType == update.callType) {
                entry.valid = false;
            }
        }
        memcpy(entries_, candidate, sizeof(entries_));
        return true;
    }

    for (size_t accessIndex = 0; accessIndex < update.accessCount;
         ++accessIndex) {
        const AccessStatusUpdate& access = update.access[accessIndex];
        CanonicalServiceEntry* destination = nullptr;
        for (CanonicalServiceEntry& entry : candidate) {
            if (entry.valid && entry.callType == update.callType &&
                    entry.networkMode == access.networkMode) {
                destination = &entry;
                break;
            }
            if (!entry.valid && destination == nullptr) destination = &entry;
        }
        if (destination == nullptr) return false;
        destination->serviceType = update.serviceType;
        destination->callType = update.callType;
        destination->networkMode = access.networkMode;
        destination->status = access.status;
        destination->restrictionCause = access.hasRestrictionCause
                                                ? access.restrictionCause
                                                : update.restrictionCause;
        destination->valid = true;
    }
    memcpy(entries_, candidate, sizeof(entries_));
    return true;
}

uint32_t ServiceStateTracker::summarize(uint32_t status) const {
    uint32_t mask = 0;
    for (const CanonicalServiceEntry& entry : entries_) {
        if (!entry.valid || entry.status != status ||
                entry.restrictionCause != 0) {
            continue;
        }
        const uint32_t index = capabilityIndex(entry.callType,
                                               entry.networkMode);
        if (index < 32) mask |= static_cast<uint32_t>(1) << index;
    }
    return mask;
}

uint32_t ServiceStateTracker::enabledMask() const {
    return summarize(kStatusEnabled);
}

uint32_t ServiceStateTracker::partialMask() const {
    return summarize(kStatusPartiallyEnabled);
}

size_t ServiceStateTracker::acceptedUpdates() const {
    return acceptedUpdates_;
}

size_t ServiceStateTracker::pendingCount() const {
    size_t count = 0;
    for (const PendingUpdate& entry : pending_) {
        if (entry.active) ++count;
    }
    return count;
}

size_t ServiceStateTracker::entryCount() const {
    size_t count = 0;
    for (const CanonicalServiceEntry& entry : entries_) {
        if (entry.valid) ++count;
    }
    return count;
}

const CanonicalServiceEntry* ServiceStateTracker::entries() const {
    return entries_;
}

}  // namespace imscompat
