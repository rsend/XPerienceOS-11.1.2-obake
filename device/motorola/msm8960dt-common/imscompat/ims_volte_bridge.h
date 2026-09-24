/* Copyright (C) 2026 The XPerience Project. Licensed under Apache-2.0. */
#ifndef IMS_VOLTE_BRIDGE_H
#define IMS_VOLTE_BRIDGE_H

#include "ims_service_state.h"

namespace imscompat {

enum class VolteRequestKind { kUnrelated, kTranslate, kUnsupported };
VolteRequestKind classifyVolteRequest(const ServiceStatusUpdate& update, bool* enabled);

struct VolteCompletion {
    uint32_t token = 0;
    bool enabled = false;
    bool successful = false;
};

// Bounded, ordered, per-connection two-phase completion: legacy request30 ACK,
// then independent QIPCALL read/set/readback. No Android success before both.
class VoltePolicyBridge {
  public:
    static constexpr size_t kCapacity = 8;
    static constexpr uint64_t kDeadlineMs = 60000;
    bool record(uint32_t token, bool enabled, uint64_t nowMs);
    bool contains(uint32_t token) const;
    bool upstreamResponse(uint32_t token, bool successful);
    bool nextJob(uint64_t nowMs, uint32_t* token, bool* enabled);
    bool finishJob(uint32_t token, bool successful, bool retryable, uint64_t nowMs);
    bool takeCompletion(VolteCompletion* result);
    bool expired(uint64_t nowMs) const;
    bool hasPending() const;

  private:
    enum class Stage { kFree, kUpstream, kQueued, kRunning, kDone };
    struct Entry {
        uint32_t token = 0;
        bool enabled = false;
        bool successful = false;
        unsigned attempts = 0;
        uint64_t sequence = 0;
        uint64_t deadlineMs = 0;
        uint64_t retryAtMs = 0;
        Stage stage = Stage::kFree;
    };
    Entry* oldest();
    Entry entries_[kCapacity] = {};
    uint64_t sequence_ = 0;
};

}  // namespace imscompat
#endif
