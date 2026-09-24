/*
 * Copyright (C) 2026 The XPerience Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "ims_protocol_adapter.h"

namespace imscompat {
namespace {

constexpr uint32_t kFirstCommonRequest = 1;
constexpr uint32_t kLastCommonRequest = 36;
constexpr uint32_t kClarkHold = 37;
constexpr uint32_t kClarkResume = 38;
constexpr uint32_t kClarkSendUiTtyMode = 39;
constexpr uint32_t kObakeSetTtyMode = 37;
constexpr uint32_t kClarkSetImsConfig = 44;
constexpr uint32_t kClarkGetImsConfig = 45;

}  // namespace

ClientFrameAdaptation adaptClientFrame(ProtocolProfile profile, uint8_t* data,
                                       DecodedFrame* frame) {
    ClientFrameAdaptation result;
    if (data == nullptr || frame == nullptr) return result;

    result.clientId = frame->metadata.id;
    result.upstreamId = frame->metadata.id;

    if (frame->metadata.type != 1 ||
        profile == ProtocolProfile::kPassThrough) {
        result.action = ClientFrameAction::kForward;
        return result;
    }

    const uint32_t id = frame->metadata.id;
    if (id >= kFirstCommonRequest && id <= kLastCommonRequest) {
        result.action = ClientFrameAction::kForward;
        return result;
    }

    if (id == kClarkSendUiTtyMode) {
        if (!rewriteFrameId(data, frame, kObakeSetTtyMode)) {
            return result;
        }
        result.upstreamId = kObakeSetTtyMode;
        result.action = ClientFrameAction::kForward;
        return result;
    }

    // Clark HOLD collides numerically with obake SET_TTY_MODE. Clark RESUME
    // and the generic configuration API have no stock SU6-7.3 equivalents.
    // Any other extension is unknown to this deliberately narrow profile.
    if (id == kClarkHold || id == kClarkResume ||
        id == kClarkSetImsConfig || id == kClarkGetImsConfig ||
        id > kLastCommonRequest) {
        result.action = ClientFrameAction::kRespondUnsupported;
        return result;
    }

    return result;
}

bool adaptUpstreamResponse(ProtocolProfile profile,
                           const Transaction& transaction, uint8_t* data,
                           DecodedFrame* frame) {
    if (data == nullptr || frame == nullptr) return false;
    if (profile == ProtocolProfile::kPassThrough ||
        transaction.clientId == transaction.upstreamId) {
        return true;
    }
    return rewriteFrameId(data, frame, transaction.clientId);
}

}  // namespace imscompat
