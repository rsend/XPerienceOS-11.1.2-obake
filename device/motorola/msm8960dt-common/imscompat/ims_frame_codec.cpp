/*
 * Copyright (C) 2026 The XPerience Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "ims_frame_codec.h"

#include <limits.h>
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
        if (*offset >= size) {
            return VarintResult::kTruncated;
        }
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
    if (size - *offset < 4) {
        return false;
    }
    const uint8_t* bytes = data + *offset;
    *value = static_cast<uint32_t>(bytes[0]) |
             (static_cast<uint32_t>(bytes[1]) << 8) |
             (static_cast<uint32_t>(bytes[2]) << 16) |
             (static_cast<uint32_t>(bytes[3]) << 24);
    *offset += 4;
    return true;
}

bool skipField(const uint8_t* data, size_t size, size_t* offset,
               uint32_t wireType) {
    uint32_t length = 0;
    switch (wireType) {
        case 0:
            return readVarint32(data, size, offset, &length) ==
                   VarintResult::kOk;
        case 1:
            if (size - *offset < 8) return false;
            *offset += 8;
            return true;
        case 2:
            if (readVarint32(data, size, offset, &length) !=
                    VarintResult::kOk ||
                length > size - *offset) {
                return false;
            }
            *offset += length;
            return true;
        case 5:
            if (size - *offset < 4) return false;
            *offset += 4;
            return true;
        default:
            // Groups and unknown wire types are ambiguous in this framing.
            return false;
    }
}

bool parseTag(const uint8_t* data, size_t size, FrameMetadata* metadata,
              size_t* idValueOffset, size_t* idValueSize) {
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

        if (field == 1) {
            if (wireType != 5 || metadata->hasToken ||
                !readFixed32(data, size, &offset, &value)) {
                return false;
            }
            metadata->token = value;
            metadata->hasToken = true;
        } else if (field >= 2 && field <= 4) {
            const size_t valueOffset = offset;
            if (wireType != 0 ||
                readVarint32(data, size, &offset, &value) !=
                        VarintResult::kOk) {
                return false;
            }
            bool* present = field == 2 ? &metadata->hasType
                                       : field == 3 ? &metadata->hasId
                                                    : &metadata->hasError;
            if (*present) {
                return false;
            }
            *present = true;
            if (field == 2) metadata->type = value;
            if (field == 3) {
                metadata->id = value;
                *idValueOffset = valueOffset;
                *idValueSize = offset - valueOffset;
            }
            if (field == 4) metadata->error = value;
        } else if (!skipField(data, size, &offset, wireType)) {
            return false;
        }
    }

    if (!metadata->hasType || !metadata->hasId) {
        return false;
    }
    // Requests and responses use transaction tokens. Unsolicited indications
    // (type 3) legitimately omit one and are logged as token=-1 by the client.
    if ((metadata->type == 1 || metadata->type == 2) &&
        !metadata->hasToken) {
        return false;
    }
    return true;
}

size_t encodeVarint32(uint32_t value, uint8_t* output) {
    size_t count = 0;
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7f);
        value >>= 7;
        if (value != 0) byte |= 0x80;
        output[count++] = byte;
    } while (value != 0);
    return count;
}

}  // namespace

uint8_t* FrameBuffer::appendData() {
    if (end_ == kMaximumWireFrameSize) {
        compact();
    }
    return bytes_ + end_;
}

size_t FrameBuffer::appendCapacity() {
    if (end_ == kMaximumWireFrameSize) {
        compact();
    }
    return kMaximumWireFrameSize - end_;
}

bool FrameBuffer::commitAppend(size_t count) {
    if (count > kMaximumWireFrameSize - end_) {
        return false;
    }
    end_ += count;
    return true;
}

const uint8_t* FrameBuffer::decodeData() const {
    return bytes_ + validatedEnd_;
}

uint8_t* FrameBuffer::mutableDecodeData() {
    return bytes_ + validatedEnd_;
}

size_t FrameBuffer::decodeSize() const {
    return end_ - validatedEnd_;
}

bool FrameBuffer::commitValidated(size_t count) {
    if (count > end_ - validatedEnd_) {
        return false;
    }
    validatedEnd_ += count;
    return true;
}

bool FrameBuffer::discardDecoded(size_t count) {
    if (count > end_ - validatedEnd_) {
        return false;
    }
    const size_t trailing = end_ - validatedEnd_ - count;
    if (trailing != 0) {
        memmove(bytes_ + validatedEnd_, bytes_ + validatedEnd_ + count,
                trailing);
    }
    end_ -= count;
    return true;
}

bool FrameBuffer::insertValidated(const uint8_t* data, size_t count) {
    compact();
    if (data == nullptr || count > kMaximumWireFrameSize - end_) return false;
    memmove(bytes_ + validatedEnd_ + count, bytes_ + validatedEnd_,
            end_ - validatedEnd_);
    memcpy(bytes_ + validatedEnd_, data, count);
    validatedEnd_ += count;
    end_ += count;
    return true;
}

bool FrameBuffer::replaceDecoded(size_t oldCount, const uint8_t* data,
                                 size_t count) {
    compact();
    if (data == nullptr || oldCount > end_ - validatedEnd_ ||
        count > kMaximumWireFrameSize - (end_ - oldCount)) return false;
    memmove(bytes_ + validatedEnd_ + count,
            bytes_ + validatedEnd_ + oldCount,
            end_ - validatedEnd_ - oldCount);
    memcpy(bytes_ + validatedEnd_, data, count);
    end_ = end_ - oldCount + count;
    return true;
}

const uint8_t* FrameBuffer::writeData() const {
    return bytes_ + begin_;
}

size_t FrameBuffer::writeSize() const {
    return validatedEnd_ - begin_;
}

bool FrameBuffer::commitWrite(size_t count) {
    if (count > validatedEnd_ - begin_) {
        return false;
    }
    begin_ += count;
    if (begin_ == end_) {
        begin_ = 0;
        validatedEnd_ = 0;
        end_ = 0;
    } else if (begin_ == validatedEnd_) {
        compact();
    }
    return true;
}

bool FrameBuffer::hasBufferedData() const {
    return begin_ != end_;
}

void FrameBuffer::compact() {
    if (begin_ == 0) {
        return;
    }
    const size_t retained = end_ - begin_;
    if (retained != 0) {
        memmove(bytes_, bytes_ + begin_, retained);
    }
    validatedEnd_ -= begin_;
    end_ = retained;
    begin_ = 0;
}

DecodeResult decodeFrame(const uint8_t* data, size_t size, DecodedFrame* frame) {
    if (data == nullptr || frame == nullptr) {
        return DecodeResult::kMalformed;
    }
    *frame = DecodedFrame{};
    if (size < kFrameLengthSize) {
        return DecodeResult::kNeedMore;
    }

    const uint32_t bodySize = (static_cast<uint32_t>(data[0]) << 24) |
                              (static_cast<uint32_t>(data[1]) << 16) |
                              (static_cast<uint32_t>(data[2]) << 8) |
                              static_cast<uint32_t>(data[3]);
    if (bodySize == 0) {
        return DecodeResult::kMalformed;
    }
    if (bodySize > kMaximumWireFrameSize - kFrameLengthSize) {
        return DecodeResult::kOversized;
    }
    const size_t wireSize = kFrameLengthSize + static_cast<size_t>(bodySize);
    if (size < wireSize) {
        return DecodeResult::kNeedMore;
    }

    const uint8_t* body = data + kFrameLengthSize;
    size_t offset = 0;
    uint32_t tagSize = 0;
    const VarintResult tagLengthResult =
            readVarint32(body, bodySize, &offset, &tagSize);
    if (tagLengthResult != VarintResult::kOk || tagSize == 0 ||
        tagSize > bodySize - offset) {
        return DecodeResult::kMalformed;
    }

    FrameMetadata metadata;
    size_t idValueOffset = 0;
    size_t idValueSize = 0;
    const size_t tagOffset = offset;
    if (!parseTag(body + tagOffset, tagSize, &metadata, &idValueOffset,
                  &idValueSize)) {
        return DecodeResult::kMalformed;
    }

    frame->wireSize = wireSize;
    frame->tagSize = tagSize;
    frame->payloadOffset = kFrameLengthSize + offset + tagSize;
    frame->payloadSize = bodySize - offset - tagSize;
    frame->idValueOffset = kFrameLengthSize + tagOffset + idValueOffset;
    frame->idValueSize = idValueSize;
    frame->metadata = metadata;
    return DecodeResult::kFrame;
}

bool rewriteFrameId(uint8_t* data, DecodedFrame* frame, uint32_t id) {
    if (data == nullptr || frame == nullptr || frame->idValueSize == 0 ||
        frame->idValueOffset > frame->wireSize ||
        frame->idValueSize > frame->wireSize - frame->idValueOffset) {
        return false;
    }
    uint8_t encoded[5] = {};
    const size_t encodedSize = encodeVarint32(id, encoded);
    if (encodedSize != frame->idValueSize) {
        return false;
    }
    memcpy(data + frame->idValueOffset, encoded, encodedSize);
    frame->metadata.id = id;
    return true;
}

bool encodeEmptyResponse(uint32_t token, uint32_t id, uint32_t error,
                         uint8_t* output, size_t capacity, size_t* wireSize) {
    return encodeFrame(token, 2, id, error, nullptr, 0,
                       output, capacity, wireSize);
}

bool encodeFrame(uint32_t token, uint32_t type, uint32_t id, uint32_t error,
                 const uint8_t* payload, size_t payloadSize,
                 uint8_t* output, size_t capacity, size_t* wireSize) {
    if (output == nullptr || wireSize == nullptr ||
        (payload == nullptr && payloadSize != 0) ||
        payloadSize > kMaximumWireFrameSize || type < 1 || type > 3) return false;

    uint8_t tag[32] = {};
    size_t tagSize = 0;
    tag[tagSize++] = 0x0d;  // MsgTag.token, fixed32.
    tag[tagSize++] = static_cast<uint8_t>(token);
    tag[tagSize++] = static_cast<uint8_t>(token >> 8);
    tag[tagSize++] = static_cast<uint8_t>(token >> 16);
    tag[tagSize++] = static_cast<uint8_t>(token >> 24);
    tag[tagSize++] = 0x10;  // MsgTag.type, varint.
    tagSize += encodeVarint32(type, tag + tagSize);
    tag[tagSize++] = 0x18;  // MsgTag.id, varint.
    tagSize += encodeVarint32(id, tag + tagSize);
    tag[tagSize++] = 0x20;  // MsgTag.error, varint.
    tagSize += encodeVarint32(error, tag + tagSize);

    uint8_t encodedTagSize[5] = {};
    const size_t tagLengthSize = encodeVarint32(
            static_cast<uint32_t>(tagSize), encodedTagSize);
    const size_t bodySize = tagLengthSize + tagSize + payloadSize;
    const size_t totalSize = kFrameLengthSize + bodySize;
    if (totalSize > capacity || totalSize > kMaximumWireFrameSize ||
        bodySize > UINT32_MAX) return false;

    output[0] = static_cast<uint8_t>(bodySize >> 24);
    output[1] = static_cast<uint8_t>(bodySize >> 16);
    output[2] = static_cast<uint8_t>(bodySize >> 8);
    output[3] = static_cast<uint8_t>(bodySize);
    memcpy(output + kFrameLengthSize, encodedTagSize, tagLengthSize);
    memcpy(output + kFrameLengthSize + tagLengthSize, tag, tagSize);
    if (payloadSize != 0) {
        memcpy(output + kFrameLengthSize + tagLengthSize + tagSize,
                payload, payloadSize);
    }
    *wireSize = totalSize;
    return true;
}

AddTransactionResult TransactionTracker::add(uint32_t token,
                                              uint32_t clientId,
                                              uint32_t upstreamId,
                                              uint64_t nowMs) {
    Transaction* freeEntry = nullptr;
    for (Transaction& entry : entries_) {
        if (entry.active && entry.token == token) {
            return AddTransactionResult::kDuplicateToken;
        }
        if (!entry.active && freeEntry == nullptr) {
            freeEntry = &entry;
        }
    }
    if (freeEntry == nullptr) {
        return AddTransactionResult::kFull;
    }
    freeEntry->token = token;
    freeEntry->clientId = clientId;
    freeEntry->upstreamId = upstreamId;
    freeEntry->deadlineMs = nowMs + kTransactionDeadlineMs;
    freeEntry->active = true;
    return AddTransactionResult::kAdded;
}

MatchTransactionResult TransactionTracker::match(uint32_t token,
                                                 uint32_t upstreamId,
                                                 Transaction* matched) {
    for (Transaction& entry : entries_) {
        if (!entry.active || entry.token != token) {
            continue;
        }
        if (matched != nullptr) {
            *matched = entry;
        }
        entry.active = false;
        return entry.upstreamId == upstreamId
                       ? MatchTransactionResult::kMatched
                       : MatchTransactionResult::kIdMismatch;
    }
    return MatchTransactionResult::kUnknownToken;
}

size_t TransactionTracker::expire(uint64_t nowMs, Transaction* expired,
                                  size_t capacity) {
    size_t count = 0;
    for (Transaction& entry : entries_) {
        if (!entry.active || entry.deadlineMs > nowMs) {
            continue;
        }
        if (expired != nullptr && count < capacity) {
            expired[count] = entry;
        }
        ++count;
        entry.active = false;
    }
    return count;
}

void TransactionTracker::clear() {
    for (Transaction& entry : entries_) {
        entry.active = false;
    }
}

size_t TransactionTracker::size() const {
    size_t count = 0;
    for (const Transaction& entry : entries_) {
        if (entry.active) ++count;
    }
    return count;
}

}  // namespace imscompat
