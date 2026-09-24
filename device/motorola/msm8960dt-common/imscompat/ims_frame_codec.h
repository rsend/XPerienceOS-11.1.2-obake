/*
 * Copyright (C) 2026 The XPerience Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef IMS_FRAME_CODEC_H
#define IMS_FRAME_CODEC_H

#include <stddef.h>
#include <stdint.h>

namespace imscompat {

constexpr size_t kFrameLengthSize = 4;
constexpr size_t kMaximumWireFrameSize = 64 * 1024;
constexpr uint64_t kTransactionDeadlineMs = 60 * 1000;
constexpr size_t kMaximumTransactions = 64;

enum class DecodeResult {
    kFrame,
    kNeedMore,
    kMalformed,
    kOversized,
};

struct FrameMetadata {
    uint32_t token = 0;
    uint32_t type = 0;
    uint32_t id = 0;
    uint32_t error = 0;
    bool hasToken = false;
    bool hasType = false;
    bool hasId = false;
    bool hasError = false;
};

struct DecodedFrame {
    size_t wireSize = 0;
    size_t tagSize = 0;
    size_t payloadOffset = 0;
    size_t payloadSize = 0;
    size_t idValueOffset = 0;
    size_t idValueSize = 0;
    FrameMetadata metadata;
};

// Fixed-capacity stream storage. Bytes cannot enter the write window until a
// complete frame has been validated, and partial writes never discard the
// incomplete frame that follows them.
class FrameBuffer {
  public:
    uint8_t* appendData();
    size_t appendCapacity();
    bool commitAppend(size_t count);

    const uint8_t* decodeData() const;
    uint8_t* mutableDecodeData();
    size_t decodeSize() const;
    bool commitValidated(size_t count);
    bool discardDecoded(size_t count);
    // Insert a complete generated frame after queued output, before any
    // incomplete input. One output queue prevents interleaved partial writes.
    bool insertValidated(const uint8_t* data, size_t count);
    bool replaceDecoded(size_t oldCount, const uint8_t* data, size_t count);

    const uint8_t* writeData() const;
    size_t writeSize() const;
    bool commitWrite(size_t count);

    bool hasBufferedData() const;

  private:
    void compact();

    uint8_t bytes_[kMaximumWireFrameSize] = {};
    size_t begin_ = 0;
    size_t validatedEnd_ = 0;
    size_t end_ = 0;
};

// Decodes the outer big-endian length and protobuf MsgTag. The payload remains
// opaque to this codec, but its validated offset and size are exposed to
// bounded message-specific decoders.
DecodeResult decodeFrame(const uint8_t* data, size_t size, DecodedFrame* frame);

// Rewrites only MsgTag.id. To keep framing deterministic, the replacement
// must have the same protobuf-varint width as the original value.
bool rewriteFrameId(uint8_t* data, DecodedFrame* frame, uint32_t id);

// Builds a response containing only MsgTag and no message-specific payload.
// This is used for explicit local errors when the legacy endpoint has no safe
// equivalent operation.
bool encodeEmptyResponse(uint32_t token, uint32_t id, uint32_t error,
                         uint8_t* output, size_t capacity, size_t* wireSize);
bool encodeFrame(uint32_t token, uint32_t type, uint32_t id, uint32_t error,
                 const uint8_t* payload, size_t payloadSize,
                 uint8_t* output, size_t capacity, size_t* wireSize);

enum class AddTransactionResult {
    kAdded,
    kDuplicateToken,
    kFull,
};

enum class MatchTransactionResult {
    kMatched,
    kIdMismatch,
    kUnknownToken,
};

struct Transaction {
    uint32_t token = 0;
    uint32_t clientId = 0;
    uint32_t upstreamId = 0;
    uint64_t deadlineMs = 0;
    bool active = false;
};

class TransactionTracker {
  public:
    AddTransactionResult add(uint32_t token, uint32_t clientId,
                             uint32_t upstreamId, uint64_t nowMs);
    MatchTransactionResult match(uint32_t token, uint32_t upstreamId,
                                 Transaction* matched);
    size_t expire(uint64_t nowMs, Transaction* expired, size_t capacity);
    void clear();
    size_t size() const;

  private:
    Transaction entries_[kMaximumTransactions] = {};
};

}  // namespace imscompat

#endif  // IMS_FRAME_CODEC_H
