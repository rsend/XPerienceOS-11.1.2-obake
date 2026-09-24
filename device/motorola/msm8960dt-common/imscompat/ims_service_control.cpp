/* Copyright (C) 2026 The XPerience Project */

#include "ims_service_control.h"

#include <string.h>

namespace imscompat {
namespace {

constexpr uint8_t kSnapshotMagic[4] = {'I', 'S', 'V', 'C'};
constexpr uint8_t kAckMagic[4] = {'I', 'A', 'C', 'K'};

void put16(uint8_t* output, uint16_t value) {
    output[0] = static_cast<uint8_t>(value >> 8);
    output[1] = static_cast<uint8_t>(value);
}

void put32(uint8_t* output, uint32_t value) {
    output[0] = static_cast<uint8_t>(value >> 24);
    output[1] = static_cast<uint8_t>(value >> 16);
    output[2] = static_cast<uint8_t>(value >> 8);
    output[3] = static_cast<uint8_t>(value);
}

uint16_t get16(const uint8_t* input) {
    return static_cast<uint16_t>(
            (static_cast<uint16_t>(input[0]) << 8) | input[1]);
}

uint32_t get32(const uint8_t* input) {
    return (static_cast<uint32_t>(input[0]) << 24) |
           (static_cast<uint32_t>(input[1]) << 16) |
           (static_cast<uint32_t>(input[2]) << 8) |
           static_cast<uint32_t>(input[3]);
}

}  // namespace

bool encodeServiceStateSnapshot(const ServiceStateTracker& services,
                                uint32_t sequence, uint8_t* output,
                                size_t capacity, size_t* encodedSize) {
    if (output == nullptr || encodedSize == nullptr) return false;
    const size_t entryCount = services.entryCount();
    if (entryCount > kMaximumCanonicalServices) return false;
    const size_t size = kServiceSnapshotHeaderSize +
                        entryCount * kServiceSnapshotEntrySize;
    if (size > capacity || size > UINT16_MAX) return false;

    memcpy(output, kSnapshotMagic, sizeof(kSnapshotMagic));
    put16(output + 4, kServiceControlVersion);
    put16(output + 6, static_cast<uint16_t>(size));
    put32(output + 8, sequence);
    put32(output + 12, services.enabledMask());
    put32(output + 16, services.partialMask());
    put16(output + 20, static_cast<uint16_t>(entryCount));
    put16(output + 22, 0);

    size_t outputOffset = kServiceSnapshotHeaderSize;
    const CanonicalServiceEntry* entries = services.entries();
    for (size_t index = 0; index < kMaximumCanonicalServices; ++index) {
        if (!entries[index].valid) continue;
        put32(output + outputOffset, entries[index].serviceType);
        put32(output + outputOffset + 4, entries[index].callType);
        put32(output + outputOffset + 8, entries[index].networkMode);
        put32(output + outputOffset + 12, entries[index].status);
        put32(output + outputOffset + 16,
              entries[index].restrictionCause);
        outputOffset += kServiceSnapshotEntrySize;
    }
    if (outputOffset != size) return false;
    *encodedSize = size;
    return true;
}

ServiceControlDecodeResult decodeServiceStateSnapshot(
        const uint8_t* data, size_t size, ServiceStateSnapshot* snapshot) {
    if (data == nullptr || snapshot == nullptr ||
            size < kServiceSnapshotHeaderSize ||
            memcmp(data, kSnapshotMagic, sizeof(kSnapshotMagic)) != 0 ||
            get16(data + 6) != size || get16(data + 22) != 0) {
        return ServiceControlDecodeResult::kMalformed;
    }
    if (get16(data + 4) != kServiceControlVersion) {
        return ServiceControlDecodeResult::kUnsupportedVersion;
    }
    const size_t entryCount = get16(data + 20);
    if (entryCount > kMaximumCanonicalServices) {
        return ServiceControlDecodeResult::kTooManyEntries;
    }
    if (size != kServiceSnapshotHeaderSize +
                        entryCount * kServiceSnapshotEntrySize) {
        return ServiceControlDecodeResult::kMalformed;
    }

    *snapshot = ServiceStateSnapshot{};
    snapshot->sequence = get32(data + 8);
    snapshot->enabledMask = get32(data + 12);
    snapshot->partialMask = get32(data + 16);
    snapshot->entryCount = entryCount;
    size_t offset = kServiceSnapshotHeaderSize;
    for (size_t index = 0; index < entryCount; ++index) {
        CanonicalServiceEntry& entry = snapshot->entries[index];
        entry.serviceType = get32(data + offset);
        entry.callType = get32(data + offset + 4);
        entry.networkMode = get32(data + offset + 8);
        entry.status = get32(data + offset + 12);
        entry.restrictionCause = get32(data + offset + 16);
        entry.valid = true;
        offset += kServiceSnapshotEntrySize;
    }
    return ServiceControlDecodeResult::kDecoded;
}

bool encodeServiceStateAck(uint32_t sequence, uint16_t status,
                           uint8_t* output, size_t capacity) {
    if (output == nullptr || capacity < kServiceAckSize) return false;
    memcpy(output, kAckMagic, sizeof(kAckMagic));
    put16(output + 4, kServiceControlVersion);
    put16(output + 6, status);
    put32(output + 8, sequence);
    return true;
}

bool decodeServiceStateAck(const uint8_t* data, size_t size,
                           uint32_t expectedSequence, uint16_t* status) {
    if (data == nullptr || status == nullptr || size != kServiceAckSize ||
            memcmp(data, kAckMagic, sizeof(kAckMagic)) != 0 ||
            get16(data + 4) != kServiceControlVersion ||
            get32(data + 8) != expectedSequence) {
        return false;
    }
    *status = get16(data + 6);
    return true;
}

}  // namespace imscompat
