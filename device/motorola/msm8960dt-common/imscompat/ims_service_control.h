/* Copyright (C) 2026 The XPerience Project */

#ifndef IMS_SERVICE_CONTROL_H
#define IMS_SERVICE_CONTROL_H

#include "ims_service_state.h"

#include <stddef.h>
#include <stdint.h>

namespace imscompat {

constexpr char kServiceControlSocketPath[] =
        "/dev/socket/ims_service_state";
constexpr uint16_t kServiceControlVersion = 1;
constexpr size_t kServiceSnapshotHeaderSize = 24;
constexpr size_t kServiceSnapshotEntrySize = 20;
constexpr size_t kMaximumServiceSnapshotSize =
        kServiceSnapshotHeaderSize +
        kMaximumCanonicalServices * kServiceSnapshotEntrySize;
constexpr size_t kServiceAckSize = 12;

struct ServiceStateSnapshot {
    uint32_t sequence = 0;
    uint32_t enabledMask = 0;
    uint32_t partialMask = 0;
    CanonicalServiceEntry entries[kMaximumCanonicalServices] = {};
    size_t entryCount = 0;
};

enum class ServiceControlDecodeResult {
    kDecoded,
    kMalformed,
    kUnsupportedVersion,
    kTooManyEntries,
};

bool encodeServiceStateSnapshot(const ServiceStateTracker& services,
                                uint32_t sequence, uint8_t* output,
                                size_t capacity, size_t* encodedSize);
ServiceControlDecodeResult decodeServiceStateSnapshot(
        const uint8_t* data, size_t size, ServiceStateSnapshot* snapshot);

bool encodeServiceStateAck(uint32_t sequence, uint16_t status,
                           uint8_t* output, size_t capacity);
bool decodeServiceStateAck(const uint8_t* data, size_t size,
                           uint32_t expectedSequence, uint16_t* status);

}  // namespace imscompat

#endif  // IMS_SERVICE_CONTROL_H
