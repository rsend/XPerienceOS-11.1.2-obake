/* Copyright (C) 2026 The XPerience Project
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */
#ifndef IMS_CALL_LIST_BRIDGE_H
#define IMS_CALL_LIST_BRIDGE_H

#include "ims_frame_codec.h"

namespace imscompat {

constexpr uint32_t kGetCurrentCallsId = 6;
constexpr uint32_t kCallStateChangedId = 201;
// Reserved only in the obake_legacy session; reject client collisions.
constexpr uint32_t kCallListQueryToken = 0xfffffffeu;
constexpr uint64_t kCallListDeadlineMs = 5000;
constexpr size_t kMaximumImsCalls = 16;
constexpr size_t kMaximumImsCallBytes = 2048;

struct LegacyImsCall {
    uint32_t index = 0;
    uint32_t state = 0;
    size_t size = 0;
    // Normalized state is always the first field: 08 <state>.
    uint8_t bytes[kMaximumImsCallBytes] = {};
};

// Session-local, single-flight, event-driven stock GET_CURRENT_CALLS bridge.
// No guessed IDs, no synthetic ACTIVE/DIALING states, no autonomous retries.
class CallListBridge {
  public:
    void markDirty() { dirty_ = true; }
    bool needsQuery() const { return dirty_ && !pending_; }
    bool expired(uint64_t nowMs) const;
    bool startQuery(uint64_t nowMs, uint8_t* output, size_t capacity,
                    size_t* wireSize);
    // Call only for the reserved token. A failed/malformed response must
    // terminate the relay, never masquerade as an empty successful call list.
    bool acceptResponse(const uint8_t* data, const DecodedFrame& frame,
                        uint8_t* output, size_t capacity, size_t* wireSize);
    size_t activeCount() const { return count_; }
    size_t endedCount() const { return endedCount_; }
    size_t updates() const { return updates_; }

  private:
    bool dirty_ = false;
    bool pending_ = false;
    uint64_t deadlineMs_ = 0;
    size_t count_ = 0;
    size_t endedCount_ = 0;
    size_t updates_ = 0;
    LegacyImsCall calls_[kMaximumImsCalls];
};

}  // namespace imscompat
#endif
