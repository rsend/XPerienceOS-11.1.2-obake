/* Copyright (C) 2026 The XPerience Project
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef IMS_SMS_FORMAT_H
#define IMS_SMS_FORMAT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Exact SU6-7.3 IMS Settings IDL 1.22/tool 6. Not the newer SMS-PSI layout.
namespace ims_sms {

constexpr unsigned kSetMessage = 0x22;
constexpr unsigned kGetMessage = 0x27;
constexpr int32_t k3gpp2 = 0;
constexpr int32_t k3gpp = 1;

struct ResponseHeader {
    uint32_t result;
    uint32_t error;
    uint8_t settingsValid;
    int32_t settingsError;
};

struct SetRequest {
    uint8_t formatValid;
    int32_t format;
    uint8_t overIpValid;
    uint8_t overIp;
    uint8_t phoneContextValid;
    char phoneContext[256];
};

struct GetResponse {
    ResponseHeader header;
    uint8_t formatValid;
    int32_t format;
    uint8_t overIpValid;
    uint8_t overIp;
    uint8_t phoneContextValid;
    char phoneContext[256];
};

static_assert(sizeof(ResponseHeader) == 16, "SMS response header ABI");
static_assert(offsetof(ResponseHeader, settingsValid) == 8, "SMS error-valid ABI");
static_assert(offsetof(ResponseHeader, settingsError) == 12, "SMS error ABI");
static_assert(sizeof(SetRequest) == 268, "SMS SET ABI");
static_assert(offsetof(SetRequest, formatValid) == 0, "SMS SET format-valid ABI");
static_assert(offsetof(SetRequest, format) == 4, "SMS SET format ABI");
static_assert(offsetof(SetRequest, overIpValid) == 8, "SMS SET over-IP-valid ABI");
static_assert(offsetof(SetRequest, overIp) == 9, "SMS SET over-IP ABI");
static_assert(offsetof(SetRequest, phoneContextValid) == 10, "SMS SET context-valid ABI");
static_assert(offsetof(SetRequest, phoneContext) == 11, "SMS SET context ABI");
static_assert(sizeof(GetResponse) == 284, "SMS GET ABI");
static_assert(offsetof(GetResponse, formatValid) == 16, "SMS GET format-valid ABI");
static_assert(offsetof(GetResponse, format) == 20, "SMS GET format ABI");
static_assert(offsetof(GetResponse, overIpValid) == 24, "SMS GET over-IP-valid ABI");
static_assert(offsetof(GetResponse, overIp) == 25, "SMS GET over-IP ABI");
static_assert(offsetof(GetResponse, phoneContextValid) == 26, "SMS GET context-valid ABI");
static_assert(offsetof(GetResponse, phoneContext) == 27, "SMS GET context ABI");

enum Validation { kValid, kMalformed, kModemFailure };

inline bool validFormat(int32_t value) {
    return value == k3gpp2 || value == k3gpp;
}

inline bool makeFormatOnly(int32_t format, SetRequest* request) {
    if (request == nullptr) return false;
    memset(request, 0, sizeof(*request));
    if (!validFormat(format)) return false;
    request->formatValid = 1;
    request->format = format;
    return true;
}

inline Validation validateHeader(const ResponseHeader& response) {
    if (response.settingsValid > 1) return kMalformed;
    if (response.result != 0 || response.error != 0 ||
            (response.settingsValid && response.settingsError != 1)) {
        return kModemFailure;
    }
    return kValid;
}

inline Validation validateGet(const GetResponse& response) {
    const Validation header = validateHeader(response.header);
    if (header != kValid) return header;
    // A missing format is not a usable baseline or a successful verification.
    if (response.formatValid != 1 || !validFormat(response.format) ||
            response.overIpValid > 1 || response.phoneContextValid > 1 ||
            (response.overIpValid && response.overIp > 1) ||
            (response.phoneContextValid &&
             memchr(response.phoneContext, '\0', sizeof(response.phoneContext)) == nullptr)) {
        return kMalformed;
    }
    return kValid;
}

}  // namespace ims_sms
#endif
