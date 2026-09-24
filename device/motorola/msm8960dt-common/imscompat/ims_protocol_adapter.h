/*
 * Copyright (C) 2026 The XPerience Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef IMS_PROTOCOL_ADAPTER_H
#define IMS_PROTOCOL_ADAPTER_H

#include "ims_frame_codec.h"

#include <stdint.h>

namespace imscompat {

enum class ProtocolProfile {
    kPassThrough,
    kObakeLegacy,
};

enum class ClientFrameAction {
    kForward,
    kRespondUnsupported,
    kError,
};

struct ClientFrameAdaptation {
    ClientFrameAction action = ClientFrameAction::kError;
    uint32_t clientId = 0;
    uint32_t upstreamId = 0;
};

// Applies only the explicit differences proven from the Clark and SU6-7.3
// stock protocol definitions. Pass-through mode never changes a frame.
ClientFrameAdaptation adaptClientFrame(ProtocolProfile profile, uint8_t* data,
                                       DecodedFrame* frame);

// Maps a translated response back to the ID used by the client request.
bool adaptUpstreamResponse(ProtocolProfile profile,
                           const Transaction& transaction, uint8_t* data,
                           DecodedFrame* frame);

}  // namespace imscompat

#endif  // IMS_PROTOCOL_ADAPTER_H
