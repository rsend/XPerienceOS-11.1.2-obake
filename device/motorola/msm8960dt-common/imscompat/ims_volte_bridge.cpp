/* Copyright (C) 2026 The XPerience Project. Licensed under Apache-2.0. */
#include "ims_volte_bridge.h"

namespace imscompat {

VolteRequestKind classifyVolteRequest(const ServiceStatusUpdate& update, bool* enabled) {
    if (!update.hasCallType || update.callType != kCallTypeVoice) {
        return VolteRequestKind::kUnrelated;
    }
    if (!update.hasIsValid || update.serviceType != 1 || enabled == nullptr
            || update.accessCount > kMaximumAccessUpdates) {
        return VolteRequestKind::kUnsupported;
    }
    if (!update.isValid && update.accessCount == 0) {
        *enabled = false;  // Explicit global voice invalidation includes LTE.
        return VolteRequestKind::kTranslate;
    }
    if (update.accessCount == 0) return VolteRequestKind::kUnsupported;
    bool hasLte = false;
    for (size_t i = 0; i < update.accessCount; ++i) {
        if (update.access[i].hasNetworkMode && update.access[i].networkMode == kRadioTechLte) {
            hasLte = true;
        }
    }
    if (!hasLte) return VolteRequestKind::kUnrelated;
    // The installed client sends one explicit access request per feature.
    // Do not guess how to split mixed-RAT or partially-enabled policies.
    if (!update.isValid || update.accessCount != 1 || !update.access[0].hasStatus
            || (update.access[0].status != 0 && update.access[0].status != kStatusEnabled)
            || (update.hasStatus && update.status != update.access[0].status)
            || (update.hasRestrictionCause && update.restrictionCause != 0)
            || (update.access[0].hasRestrictionCause && update.access[0].restrictionCause != 0)) {
        return VolteRequestKind::kUnsupported;
    }
    *enabled = update.access[0].status == kStatusEnabled;
    return VolteRequestKind::kTranslate;
}

bool VoltePolicyBridge::contains(uint32_t token) const {
    for (const Entry& entry : entries_) {
        if (entry.stage != Stage::kFree && entry.token == token) return true;
    }
    return false;
}

bool VoltePolicyBridge::record(uint32_t token, bool enabled, uint64_t nowMs) {
    if (contains(token)) return false;
    for (Entry& entry : entries_) {
        if (entry.stage != Stage::kFree) continue;
        entry = Entry{};
        entry.token = token;
        entry.enabled = enabled;
        entry.sequence = ++sequence_;
        entry.deadlineMs = nowMs + kDeadlineMs;
        entry.stage = Stage::kUpstream;
        return true;
    }
    return false;
}

bool VoltePolicyBridge::upstreamResponse(uint32_t token, bool successful) {
    for (Entry& entry : entries_) {
        if (entry.stage != Stage::kUpstream || entry.token != token) continue;
        // Upstream rejection is forwarded by the relay without a modem write.
        entry.stage = successful ? Stage::kQueued : Stage::kFree;
        return successful;
    }
    return false;
}

VoltePolicyBridge::Entry* VoltePolicyBridge::oldest() {
    Entry* result = nullptr;
    for (Entry& entry : entries_) {
        if (entry.stage != Stage::kFree && (result == nullptr || entry.sequence < result->sequence)) {
            result = &entry;
        }
    }
    return result;
}

bool VoltePolicyBridge::nextJob(uint64_t nowMs, uint32_t* token, bool* enabled) {
    Entry* entry = oldest();
    if (entry == nullptr || entry->stage != Stage::kQueued || entry->retryAtMs > nowMs
            || nowMs >= entry->deadlineMs || token == nullptr || enabled == nullptr) return false;
    entry->stage = Stage::kRunning;
    ++entry->attempts;
    *token = entry->token;
    *enabled = entry->enabled;
    return true;
}

bool VoltePolicyBridge::finishJob(uint32_t token, bool successful, bool retryable,
                                 uint64_t nowMs) {
    for (Entry& entry : entries_) {
        if (entry.stage != Stage::kRunning || entry.token != token) continue;
        if (!successful && retryable && entry.attempts < 3 && nowMs < entry.deadlineMs) {
            const uint64_t backoff = 500u << (entry.attempts - 1);
            const uint64_t jitter = (entry.token * 1103515245u + entry.attempts * 12345u) % 251;
            entry.retryAtMs = nowMs + backoff + jitter;
            entry.stage = Stage::kQueued;
        } else {
            entry.successful = successful;
            entry.stage = Stage::kDone;
        }
        return true;
    }
    return false;
}

bool VoltePolicyBridge::takeCompletion(VolteCompletion* result) {
    Entry* entry = oldest();
    if (entry == nullptr || entry->stage != Stage::kDone || result == nullptr) return false;
    result->token = entry->token;
    result->enabled = entry->enabled;
    result->successful = entry->successful;
    entry->stage = Stage::kFree;
    return true;
}

bool VoltePolicyBridge::expired(uint64_t nowMs) const {
    for (const Entry& entry : entries_) {
        if (entry.stage != Stage::kFree && nowMs >= entry.deadlineMs) return true;
    }
    return false;
}

bool VoltePolicyBridge::hasPending() const {
    for (const Entry& entry : entries_) if (entry.stage != Stage::kFree) return true;
    return false;
}

}  // namespace imscompat
