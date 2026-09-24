/* Copyright (C) 2026 The XPerience Project
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */
#include "ims_call_list_bridge.h"

#include <limits.h>
#include <string.h>

namespace imscompat {
namespace {

constexpr uint32_t kCallEnd = 6;  // IMS protobuf enum, NOT QMI Voice state 9.

bool readVarint(const uint8_t* data, size_t size, size_t* pos, uint64_t* value) {
    *value = 0;
    for (unsigned i = 0; i < 10 && *pos < size; ++i) {
        const uint8_t byte = data[(*pos)++];
        if (i == 9 && (byte & 0xfe)) return false;
        *value |= static_cast<uint64_t>(byte & 0x7f) << (7 * i);
        if (!(byte & 0x80)) return true;
    }
    return false;
}

struct Field {
    uint32_t number = 0;
    uint32_t wire = 0;
    uint64_t value = 0;
    size_t offset = 0;
    size_t size = 0;
};

bool nextField(const uint8_t* data, size_t size, size_t* pos, Field* field) {
    uint64_t key = 0;
    if (!readVarint(data, size, pos, &key) || key > UINT32_MAX ||
        (key >> 3) == 0) return false;
    field->number = key >> 3;
    field->wire = key & 7;
    field->offset = *pos;
    switch (field->wire) {
        case 0:
            if (!readVarint(data, size, pos, &field->value)) return false;
            field->size = *pos - field->offset;
            return true;
        case 1:
            field->size = 8;
            break;
        case 2:
            if (!readVarint(data, size, pos, &field->value) ||
                field->value > size - *pos) return false;
            field->offset = *pos;
            field->size = static_cast<size_t>(field->value);
            break;
        case 5:
            field->size = 4;
            break;
        default:
            return false;
    }
    if (field->size > size - *pos) return false;
    if (field->wire == 5) {
        field->value = static_cast<uint32_t>(data[*pos]) |
                (static_cast<uint32_t>(data[*pos + 1]) << 8) |
                (static_cast<uint32_t>(data[*pos + 2]) << 16) |
                (static_cast<uint32_t>(data[*pos + 3]) << 24);
    }
    *pos += field->size;
    return true;
}

bool validFields(const uint8_t* data, size_t size) {
    size_t pos = 0;
    while (pos < size) {
        Field field;
        if (!nextField(data, size, &pos, &field)) return false;
    }
    return true;
}

bool parseCall(const uint8_t* data, size_t size, LegacyImsCall* call) {
    if (size > kMaximumImsCallBytes - 2) return false;
    size_t pos = 0;
    bool hasState = false, hasIndex = false, hasDetails = false;
    uint32_t seen = 0;
    call->size = 2;
    call->bytes[0] = 8;
    while (pos < size) {
        const size_t start = pos;
        Field field;
        if (!nextField(data, size, &pos, &field)) return false;
        if (field.number <= 14) {
            const uint32_t bit = 1u << field.number;
            if (seen & bit) return false;
            seen |= bit;
            const uint32_t expectedWire = field.number == 1 || field.number == 4 ||
                    field.number == 5 || field.number == 7 || field.number == 8 ? 0 :
                    field.number == 9 || field.number == 11 ||
                    field.number == 13 || field.number == 14 ? 2 : 5;
            if (field.wire != expectedWire) return false;
            if ((field.number == 4 || field.number == 5 || field.number == 7 ||
                 field.number == 8) && field.value > 1) return false;
        }
        if (field.number == 1) {
            if (hasState || field.wire != 0 || field.value > kCallEnd) return false;
            hasState = true;
            call->state = field.value;
            call->bytes[1] = call->state;
            continue;
        }
        if (field.number == 2) {
            if (hasIndex || field.wire != 5 || field.value == 0 ||
                field.value > INT_MAX) return false;
            hasIndex = true;
            call->index = field.value;
        }
        if (field.number == 13) {
            // The Clark client dereferences CallDetails unconditionally.
            if (hasDetails || field.wire != 2 ||
                !validFields(data + field.offset, field.size)) return false;
            hasDetails = true;
        }
        if (field.number == 14 &&
            !validFields(data + field.offset, field.size)) return false;
        // Preserve numbers, presentation, media and unknown extensions; do
        // not log them. Bounds above cover normalization's two extra bytes.
        memcpy(call->bytes + call->size, data + start, pos - start);
        call->size += pos - start;
    }
    return hasState && hasIndex && hasDetails;
}

bool parseList(const uint8_t* data, size_t size, LegacyImsCall* calls,
               size_t* count) {
    *count = 0;
    size_t pos = 0;
    while (pos < size) {
        Field field;
        if (!nextField(data, size, &pos, &field)) return false;
        // Both APKs declare CallList.callAttributes as field 2, NOT field 1.
        if (field.number != 2) continue;  // Protobuf extension, not a call.
        if (field.wire != 2 || *count == kMaximumImsCalls ||
            !parseCall(data + field.offset, field.size, &calls[*count])) return false;
        for (size_t i = 0; i < *count; ++i) {
            if (calls[i].index == calls[*count].index) return false;
        }
        ++*count;
    }
    return true;
}

bool appendCall(const LegacyImsCall& call, bool ended, uint8_t* payload,
                 size_t capacity, size_t* size) {
    // Length needs at most two varint bytes for our bounded call records.
    if (call.size < 2 || call.size > kMaximumImsCallBytes ||
        *size > capacity || call.size + 3 > capacity - *size) return false;
    payload[(*size)++] = 0x12;  // CallList.callAttributes, field 2 / wire 2.
    size_t length = call.size;
    do {
        uint8_t byte = length & 0x7f;
        length >>= 7;
        payload[(*size)++] = byte | (length ? 0x80 : 0);
    } while (length);
    memcpy(payload + *size, call.bytes, call.size);
    if (ended) payload[*size + 1] = kCallEnd;
    *size += call.size;
    return true;
}

}  // namespace

bool CallListBridge::expired(uint64_t nowMs) const {
    return pending_ && nowMs >= deadlineMs_;
}

bool CallListBridge::startQuery(uint64_t nowMs, uint8_t* output,
                               size_t capacity, size_t* wireSize) {
    if (!needsQuery() || nowMs > UINT64_MAX - kCallListDeadlineMs ||
        !encodeFrame(kCallListQueryToken, 1, kGetCurrentCallsId, 0,
                     nullptr, 0, output, capacity, wireSize)) return false;
    pending_ = true;
    dirty_ = false;
    deadlineMs_ = nowMs + kCallListDeadlineMs;
    return true;
}

bool CallListBridge::acceptResponse(const uint8_t* data, const DecodedFrame& frame,
                                   uint8_t* output, size_t capacity,
                                   size_t* wireSize) {
    if (!pending_ || data == nullptr || frame.metadata.type != 2 ||
        !frame.metadata.hasToken || frame.metadata.token != kCallListQueryToken ||
        frame.metadata.id != kGetCurrentCallsId ||
        (frame.metadata.hasError && frame.metadata.error != 0) ||
        frame.payloadOffset > frame.wireSize ||
        frame.payloadSize != frame.wireSize - frame.payloadOffset) return false;
    LegacyImsCall current[kMaximumImsCalls];
    size_t currentCount = 0;
    if (!parseList(data + frame.payloadOffset, frame.payloadSize,
                   current, &currentCount)) return false;
    uint8_t payload[kMaximumWireFrameSize] = {};
    size_t size = 0;
    size_t ended = 0;
    for (size_t i = 0; i < currentCount; ++i) {
        if (!appendCall(current[i], false, payload, sizeof(payload), &size)) return false;
        if (current[i].state == kCallEnd) ++ended;
    }
    for (size_t i = 0; i < count_; ++i) {
        bool present = false;
        for (size_t j = 0; j < currentCount; ++j) {
            if (calls_[i].index == current[j].index) present = true;
        }
        if (!present) {
            // A successful complete snapshot no longer contains this known
            // call. Supply END using its last details, without inventing a
            // network disconnect cause (Clark defaults to unspecified).
            if (!appendCall(calls_[i], true, payload, sizeof(payload), &size)) return false;
            ++ended;
        }
    }
    if (!encodeFrame(UINT32_MAX, 3, kCallStateChangedId, 0, payload, size,
                     output, capacity, wireSize)) return false;
    count_ = 0;
    for (size_t i = 0; i < currentCount; ++i) {
        if (current[i].state != kCallEnd) calls_[count_++] = current[i];
    }
    endedCount_ = ended;
    ++updates_;
    pending_ = false;
    return true;
}

}  // namespace imscompat
