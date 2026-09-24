#include "ims_frame_codec.h"
#include "ims_protocol_adapter.h"
#include "ims_service_state.h"
#include "ims_service_control.h"
#include "ims_call_list_bridge.h"
#include "ims_volte_bridge.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

using imscompat::AddTransactionResult;
using imscompat::DecodeResult;
using imscompat::DecodedFrame;
using imscompat::FrameBuffer;
using imscompat::MatchTransactionResult;
using imscompat::Transaction;
using imscompat::TransactionTracker;

namespace {

unsigned failures = 0;

#define EXPECT_TRUE(condition)                                                   \
    do {                                                                         \
        if (!(condition)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                           \
        }                                                                         \
    } while (0)

const uint8_t kQueryServiceStatus[] = {
        0, 0, 0, 12, 11, 13, 2, 0, 0, 0, 16, 1, 24, 29, 32, 0};
const uint8_t kSetServiceStatus[] = {
        0, 0, 0, 27, 11, 13, 3, 0, 0, 0, 16, 1, 24, 30, 32, 0,
        8, 1, 24, 0, 58, 9, 8, 14, 16, 2, 29, 0, 0, 0, 0};
const uint8_t kRegistrationResponse[] = {
        0, 0, 0, 10, 9, 13, 17, 0, 0, 0, 16, 2, 24, 1};
const uint8_t kUnsolicitedNoToken[] = {
        0, 0, 0, 6, 5, 16, 3, 24, 0xd5, 1};
const uint8_t kClarkTtyRequest[] = {
        0, 0, 0, 14, 11, 13, 14, 0, 0, 0, 16, 1, 24, 39, 32, 0, 8, 0};
const uint8_t kClarkWfcModeRequest[] = {
        0, 0, 0, 23, 11, 13, 7, 0, 0, 0, 16, 1, 24, 44, 32, 0,
        8, 29, 16, 0, 29, 1, 0, 0, 0, 40, 0};

void testGoldenFrames() {
    DecodedFrame frame;
    EXPECT_TRUE(imscompat::decodeFrame(kQueryServiceStatus,
                                       sizeof(kQueryServiceStatus), &frame) ==
                DecodeResult::kFrame);
    EXPECT_TRUE(frame.wireSize == sizeof(kQueryServiceStatus));
    EXPECT_TRUE(frame.payloadSize == 0);
    EXPECT_TRUE(frame.metadata.token == 2);
    EXPECT_TRUE(frame.metadata.type == 1);
    EXPECT_TRUE(frame.metadata.id == 29);
    EXPECT_TRUE(frame.metadata.error == 0);

    uint8_t unchanged[sizeof(kSetServiceStatus)];
    memcpy(unchanged, kSetServiceStatus, sizeof(unchanged));
    EXPECT_TRUE(imscompat::decodeFrame(unchanged, sizeof(unchanged), &frame) ==
                DecodeResult::kFrame);
    EXPECT_TRUE(frame.wireSize == sizeof(kSetServiceStatus));
    EXPECT_TRUE(frame.payloadSize == 15);
    EXPECT_TRUE(frame.payloadOffset == 16);
    EXPECT_TRUE(frame.metadata.token == 3);
    EXPECT_TRUE(frame.metadata.id == 30);
    EXPECT_TRUE(memcmp(unchanged, kSetServiceStatus, sizeof(unchanged)) == 0);

    EXPECT_TRUE(imscompat::decodeFrame(kRegistrationResponse,
                                       sizeof(kRegistrationResponse), &frame) ==
                DecodeResult::kFrame);
    EXPECT_TRUE(frame.metadata.token == 17);
    EXPECT_TRUE(frame.metadata.type == 2);
    EXPECT_TRUE(frame.metadata.id == 1);

    EXPECT_TRUE(imscompat::decodeFrame(kUnsolicitedNoToken,
                                       sizeof(kUnsolicitedNoToken), &frame) ==
                DecodeResult::kFrame);
    EXPECT_TRUE(!frame.metadata.hasToken);
    EXPECT_TRUE(frame.metadata.type == 3);
    EXPECT_TRUE(frame.metadata.id == 213);
}

void testServiceStatusDecode() {
    DecodedFrame frame;
    EXPECT_TRUE(imscompat::decodeFrame(kSetServiceStatus,
                                       sizeof(kSetServiceStatus), &frame) ==
                DecodeResult::kFrame);
    imscompat::ServiceStatusUpdate update;
    EXPECT_TRUE(imscompat::decodeServiceStatusRequest(
                        kSetServiceStatus, frame, &update) ==
                imscompat::ServiceDecodeResult::kDecoded);
    EXPECT_TRUE(update.hasIsValid && update.isValid);
    EXPECT_TRUE(update.hasCallType &&
                update.callType == imscompat::kCallTypeVoice);
    EXPECT_TRUE(!update.hasServiceType && update.serviceType == 1);
    EXPECT_TRUE(update.accessCount == 1);
    EXPECT_TRUE(update.access[0].networkMode == imscompat::kRadioTechLte);
    EXPECT_TRUE(update.access[0].status == imscompat::kStatusEnabled);
    EXPECT_TRUE(update.access[0].hasRestrictionCause);
    EXPECT_TRUE(update.access[0].restrictionCause == 0);

    EXPECT_TRUE(imscompat::decodeFrame(kQueryServiceStatus,
                                       sizeof(kQueryServiceStatus), &frame) ==
                DecodeResult::kFrame);
    EXPECT_TRUE(imscompat::decodeServiceStatusRequest(
                        kQueryServiceStatus, frame, &update) ==
                imscompat::ServiceDecodeResult::kNotServiceStatusRequest);

    uint8_t malformed[sizeof(kSetServiceStatus)];
    memcpy(malformed, kSetServiceStatus, sizeof(malformed));
    // Change the nested LTE status field from varint to fixed32 without
    // changing the enclosing length. The strict decoder must reject it.
    malformed[24] = 0x15;
    EXPECT_TRUE(imscompat::decodeFrame(malformed, sizeof(malformed), &frame) ==
                DecodeResult::kFrame);
    EXPECT_TRUE(imscompat::decodeServiceStatusRequest(
                        malformed, frame, &update) ==
                imscompat::ServiceDecodeResult::kMalformed);
}

void testAcknowledgedServiceState() {
    DecodedFrame frame;
    EXPECT_TRUE(imscompat::decodeFrame(kSetServiceStatus,
                                       sizeof(kSetServiceStatus), &frame) ==
                DecodeResult::kFrame);
    imscompat::ServiceStatusUpdate update;
    EXPECT_TRUE(imscompat::decodeServiceStatusRequest(
                        kSetServiceStatus, frame, &update) ==
                imscompat::ServiceDecodeResult::kDecoded);

    imscompat::ServiceStateTracker services;
    EXPECT_TRUE(services.record(3, update) ==
                imscompat::RecordServiceResult::kRecorded);
    EXPECT_TRUE(services.record(3, update) ==
                imscompat::RecordServiceResult::kDuplicateToken);
    EXPECT_TRUE(services.pendingCount() == 1);
    EXPECT_TRUE(services.enabledMask() == 0);
    EXPECT_TRUE(services.complete(3, false) ==
                imscompat::CompleteServiceResult::kRejected);
    EXPECT_TRUE(services.pendingCount() == 0);
    EXPECT_TRUE(services.acceptedUpdates() == 0);
    EXPECT_TRUE(services.enabledMask() == 0);

    EXPECT_TRUE(services.record(4, update) ==
                imscompat::RecordServiceResult::kRecorded);
    EXPECT_TRUE(services.complete(4, true) ==
                imscompat::CompleteServiceResult::kApplied);
    EXPECT_TRUE(services.acceptedUpdates() == 1);
    EXPECT_TRUE(services.entryCount() == 1);
    EXPECT_TRUE(services.enabledMask() == 1u);
    EXPECT_TRUE(services.partialMask() == 0);

    imscompat::ServiceStatusUpdate unknown = update;
    unknown.callType = 99;
    unknown.access[0].networkMode = 77;
    EXPECT_TRUE(services.record(5, unknown) ==
                imscompat::RecordServiceResult::kRecorded);
    EXPECT_TRUE(services.complete(5, true) ==
                imscompat::CompleteServiceResult::kApplied);
    EXPECT_TRUE(services.entryCount() == 2);
    EXPECT_TRUE(services.enabledMask() == 1u);

    imscompat::ServiceStatusUpdate disabled = update;
    disabled.isValid = false;
    disabled.accessCount = 0;
    EXPECT_TRUE(services.record(6, disabled) ==
                imscompat::RecordServiceResult::kRecorded);
    EXPECT_TRUE(services.complete(6, true) ==
                imscompat::CompleteServiceResult::kApplied);
    EXPECT_TRUE(services.enabledMask() == 0);

    EXPECT_TRUE(services.record(7, update) ==
                imscompat::RecordServiceResult::kRecorded);
    EXPECT_TRUE(services.cancel(7));
    EXPECT_TRUE(!services.cancel(7));
    EXPECT_TRUE(services.complete(7, true) ==
                imscompat::CompleteServiceResult::kUnknownToken);
}

void testServiceControlProtocol() {
    DecodedFrame frame;
    EXPECT_TRUE(imscompat::decodeFrame(kSetServiceStatus,
                                       sizeof(kSetServiceStatus), &frame) ==
                DecodeResult::kFrame);
    imscompat::ServiceStatusUpdate update;
    EXPECT_TRUE(imscompat::decodeServiceStatusRequest(
                        kSetServiceStatus, frame, &update) ==
                imscompat::ServiceDecodeResult::kDecoded);
    imscompat::ServiceStateTracker services;
    EXPECT_TRUE(services.record(20, update) ==
                imscompat::RecordServiceResult::kRecorded);
    EXPECT_TRUE(services.complete(20, true) ==
                imscompat::CompleteServiceResult::kApplied);

    uint8_t wire[imscompat::kMaximumServiceSnapshotSize] = {};
    size_t wireSize = 0;
    EXPECT_TRUE(imscompat::encodeServiceStateSnapshot(
            services, 42, wire, sizeof(wire), &wireSize));
    EXPECT_TRUE(wireSize == imscompat::kServiceSnapshotHeaderSize +
                            imscompat::kServiceSnapshotEntrySize);
    imscompat::ServiceStateSnapshot snapshot;
    EXPECT_TRUE(imscompat::decodeServiceStateSnapshot(
                        wire, wireSize, &snapshot) ==
                imscompat::ServiceControlDecodeResult::kDecoded);
    EXPECT_TRUE(snapshot.sequence == 42);
    EXPECT_TRUE(snapshot.enabledMask == 1u);
    EXPECT_TRUE(snapshot.partialMask == 0);
    EXPECT_TRUE(snapshot.entryCount == 1);
    EXPECT_TRUE(snapshot.entries[0].callType ==
                imscompat::kCallTypeVoice);
    EXPECT_TRUE(snapshot.entries[0].networkMode ==
                imscompat::kRadioTechLte);
    EXPECT_TRUE(snapshot.entries[0].status ==
                imscompat::kStatusEnabled);

    uint8_t corrupt[imscompat::kMaximumServiceSnapshotSize] = {};
    memcpy(corrupt, wire, wireSize);
    corrupt[7] ^= 1;
    EXPECT_TRUE(imscompat::decodeServiceStateSnapshot(
                        corrupt, wireSize, &snapshot) ==
                imscompat::ServiceControlDecodeResult::kMalformed);
    memcpy(corrupt, wire, wireSize);
    corrupt[5] = 2;
    EXPECT_TRUE(imscompat::decodeServiceStateSnapshot(
                        corrupt, wireSize, &snapshot) ==
                imscompat::ServiceControlDecodeResult::kUnsupportedVersion);
    EXPECT_TRUE(imscompat::decodeServiceStateSnapshot(
                        wire, wireSize - 1, &snapshot) ==
                imscompat::ServiceControlDecodeResult::kMalformed);

    uint8_t ack[imscompat::kServiceAckSize] = {};
    EXPECT_TRUE(imscompat::encodeServiceStateAck(42, 0, ack, sizeof(ack)));
    uint16_t status = 99;
    EXPECT_TRUE(imscompat::decodeServiceStateAck(
            ack, sizeof(ack), 42, &status));
    EXPECT_TRUE(status == 0);
    EXPECT_TRUE(!imscompat::decodeServiceStateAck(
            ack, sizeof(ack), 43, &status));
    EXPECT_TRUE(!imscompat::encodeServiceStateAck(42, 0, ack,
                                                  sizeof(ack) - 1));
}

void testFragmentedAndCoalescedReads() {
    DecodedFrame frame;
    for (size_t size = 0; size < sizeof(kQueryServiceStatus); ++size) {
        EXPECT_TRUE(imscompat::decodeFrame(kQueryServiceStatus, size, &frame) ==
                    DecodeResult::kNeedMore);
    }

    uint8_t joined[sizeof(kQueryServiceStatus) + sizeof(kSetServiceStatus)];
    memcpy(joined, kQueryServiceStatus, sizeof(kQueryServiceStatus));
    memcpy(joined + sizeof(kQueryServiceStatus), kSetServiceStatus,
           sizeof(kSetServiceStatus));
    EXPECT_TRUE(imscompat::decodeFrame(joined, sizeof(joined), &frame) ==
                DecodeResult::kFrame);
    EXPECT_TRUE(frame.wireSize == sizeof(kQueryServiceStatus));
    EXPECT_TRUE(imscompat::decodeFrame(joined + frame.wireSize,
                                       sizeof(joined) - frame.wireSize,
                                       &frame) == DecodeResult::kFrame);
    EXPECT_TRUE(frame.wireSize == sizeof(kSetServiceStatus));
}

void testMalformedFrames() {
    DecodedFrame frame;
    const uint8_t zeroLength[] = {0, 0, 0, 0};
    EXPECT_TRUE(imscompat::decodeFrame(zeroLength, sizeof(zeroLength), &frame) ==
                DecodeResult::kMalformed);

    const uint8_t oversized[] = {0, 1, 0, 0};
    EXPECT_TRUE(imscompat::decodeFrame(oversized, sizeof(oversized), &frame) ==
                DecodeResult::kOversized);

    const uint8_t truncatedTag[] = {0, 0, 0, 2, 4, 13};
    EXPECT_TRUE(imscompat::decodeFrame(truncatedTag, sizeof(truncatedTag),
                                       &frame) == DecodeResult::kMalformed);

    const uint8_t malformedTagLength[] = {
            0, 0, 0, 5, 0x80, 0x80, 0x80, 0x80, 0x80};
    EXPECT_TRUE(imscompat::decodeFrame(malformedTagLength,
                                       sizeof(malformedTagLength), &frame) ==
                DecodeResult::kMalformed);

    const uint8_t duplicateToken[] = {
            0, 0, 0, 16, 15, 13, 1, 0, 0, 0, 13, 2, 0, 0, 0,
            16, 1, 24, 29, 32, 0};
    EXPECT_TRUE(imscompat::decodeFrame(duplicateToken, sizeof(duplicateToken),
                                       &frame) == DecodeResult::kMalformed);
}

void testPartialWritesAndTrailingFragment() {
    FrameBuffer buffer;
    const size_t firstFragment = sizeof(kQueryServiceStatus) - 3;
    memcpy(buffer.appendData(), kQueryServiceStatus, firstFragment);
    EXPECT_TRUE(buffer.commitAppend(firstFragment));
    EXPECT_TRUE(buffer.decodeSize() == firstFragment);
    EXPECT_TRUE(buffer.writeSize() == 0);

    memcpy(buffer.appendData(), kQueryServiceStatus + firstFragment, 3);
    EXPECT_TRUE(buffer.commitAppend(3));
    DecodedFrame frame;
    EXPECT_TRUE(imscompat::decodeFrame(buffer.decodeData(), buffer.decodeSize(),
                                       &frame) == DecodeResult::kFrame);
    EXPECT_TRUE(buffer.commitValidated(frame.wireSize));
    EXPECT_TRUE(buffer.writeSize() == sizeof(kQueryServiceStatus));

    // Append an incomplete second frame before partially writing the first.
    memcpy(buffer.appendData(), kSetServiceStatus, 7);
    EXPECT_TRUE(buffer.commitAppend(7));
    EXPECT_TRUE(buffer.commitWrite(5));
    EXPECT_TRUE(buffer.writeSize() == sizeof(kQueryServiceStatus) - 5);
    EXPECT_TRUE(memcmp(buffer.writeData(), kQueryServiceStatus + 5,
                       buffer.writeSize()) == 0);
    EXPECT_TRUE(buffer.commitWrite(buffer.writeSize()));
    EXPECT_TRUE(buffer.writeSize() == 0);
    EXPECT_TRUE(buffer.decodeSize() == 7);
    EXPECT_TRUE(memcmp(buffer.decodeData(), kSetServiceStatus, 7) == 0);

    memcpy(buffer.appendData(), kSetServiceStatus + 7,
           sizeof(kSetServiceStatus) - 7);
    EXPECT_TRUE(buffer.commitAppend(sizeof(kSetServiceStatus) - 7));
    EXPECT_TRUE(imscompat::decodeFrame(buffer.decodeData(), buffer.decodeSize(),
                                       &frame) == DecodeResult::kFrame);
    EXPECT_TRUE(buffer.commitValidated(frame.wireSize));
    EXPECT_TRUE(buffer.commitWrite(buffer.writeSize()));
    EXPECT_TRUE(!buffer.hasBufferedData());

    EXPECT_TRUE(!buffer.commitValidated(1));
    EXPECT_TRUE(!buffer.commitWrite(1));
}

void testDiscardDecodedFrame() {
    FrameBuffer buffer;
    const size_t total = sizeof(kQueryServiceStatus) +
                         sizeof(kClarkWfcModeRequest) +
                         sizeof(kClarkTtyRequest);
    EXPECT_TRUE(buffer.appendCapacity() >= total);
    memcpy(buffer.appendData(), kQueryServiceStatus,
           sizeof(kQueryServiceStatus));
    memcpy(buffer.appendData() + sizeof(kQueryServiceStatus),
           kClarkWfcModeRequest, sizeof(kClarkWfcModeRequest));
    memcpy(buffer.appendData() + sizeof(kQueryServiceStatus) +
                   sizeof(kClarkWfcModeRequest),
           kClarkTtyRequest, sizeof(kClarkTtyRequest));
    EXPECT_TRUE(buffer.commitAppend(total));

    DecodedFrame frame;
    EXPECT_TRUE(imscompat::decodeFrame(buffer.decodeData(), buffer.decodeSize(),
                                       &frame) == DecodeResult::kFrame);
    EXPECT_TRUE(buffer.commitValidated(frame.wireSize));
    EXPECT_TRUE(imscompat::decodeFrame(buffer.decodeData(), buffer.decodeSize(),
                                       &frame) == DecodeResult::kFrame);
    EXPECT_TRUE(frame.metadata.id == 44);
    EXPECT_TRUE(buffer.discardDecoded(frame.wireSize));
    EXPECT_TRUE(buffer.writeSize() == sizeof(kQueryServiceStatus));
    EXPECT_TRUE(imscompat::decodeFrame(buffer.decodeData(), buffer.decodeSize(),
                                       &frame) == DecodeResult::kFrame);
    EXPECT_TRUE(frame.metadata.id == 39);
    EXPECT_TRUE(!buffer.discardDecoded(buffer.decodeSize() + 1));
}

void testTransactions() {
    TransactionTracker tracker;
    EXPECT_TRUE(tracker.add(7, 30, 30, 1000) ==
                AddTransactionResult::kAdded);
    EXPECT_TRUE(tracker.add(7, 30, 30, 1001) ==
                AddTransactionResult::kDuplicateToken);
    EXPECT_TRUE(tracker.size() == 1);

    Transaction matched;
    EXPECT_TRUE(tracker.match(7, 30, &matched) ==
                MatchTransactionResult::kMatched);
    EXPECT_TRUE(matched.token == 7 && matched.clientId == 30 &&
                matched.upstreamId == 30);
    EXPECT_TRUE(tracker.size() == 0);
    EXPECT_TRUE(tracker.match(7, 30, nullptr) ==
                MatchTransactionResult::kUnknownToken);

    EXPECT_TRUE(tracker.add(8, 39, 37, 2000) ==
                AddTransactionResult::kAdded);
    EXPECT_TRUE(tracker.match(8, 37, &matched) ==
                MatchTransactionResult::kMatched);
    EXPECT_TRUE(matched.clientId == 39 && matched.upstreamId == 37);

    EXPECT_TRUE(tracker.add(9, 44, 44, 3000) ==
                AddTransactionResult::kAdded);
    Transaction expired[2];
    EXPECT_TRUE(tracker.expire(3000 + imscompat::kTransactionDeadlineMs - 1,
                               expired, 2) == 0);
    EXPECT_TRUE(tracker.expire(3000 + imscompat::kTransactionDeadlineMs,
                               expired, 2) == 1);
    EXPECT_TRUE(expired[0].token == 9 && expired[0].clientId == 44 &&
                expired[0].upstreamId == 44);

    EXPECT_TRUE(tracker.add(10, 1, 1, 4000) ==
                AddTransactionResult::kAdded);
    tracker.clear();
    EXPECT_TRUE(tracker.size() == 0);
}

void testTransactionCapacity() {
    TransactionTracker tracker;
    for (size_t index = 0; index < imscompat::kMaximumTransactions; ++index) {
        EXPECT_TRUE(tracker.add(static_cast<uint32_t>(index), 30, 30, 0) ==
                    AddTransactionResult::kAdded);
    }
    EXPECT_TRUE(tracker.add(999, 30, 30, 0) == AddTransactionResult::kFull);
}

void testProtocolAdaptation() {
    uint8_t tty[sizeof(kClarkTtyRequest)];
    memcpy(tty, kClarkTtyRequest, sizeof(tty));
    DecodedFrame frame;
    EXPECT_TRUE(imscompat::decodeFrame(tty, sizeof(tty), &frame) ==
                DecodeResult::kFrame);
    const imscompat::ClientFrameAdaptation ttyAdaptation =
            imscompat::adaptClientFrame(
                    imscompat::ProtocolProfile::kObakeLegacy, tty, &frame);
    EXPECT_TRUE(ttyAdaptation.action ==
                imscompat::ClientFrameAction::kForward);
    EXPECT_TRUE(ttyAdaptation.clientId == 39);
    EXPECT_TRUE(ttyAdaptation.upstreamId == 37);
    EXPECT_TRUE(frame.metadata.id == 37);
    EXPECT_TRUE(tty[13] == 37);
    EXPECT_TRUE(memcmp(tty, kClarkTtyRequest, 13) == 0);
    EXPECT_TRUE(memcmp(tty + 14, kClarkTtyRequest + 14,
                       sizeof(tty) - 14) == 0);
    EXPECT_TRUE(!imscompat::rewriteFrameId(tty, &frame, 128));
    EXPECT_TRUE(frame.metadata.id == 37);

    Transaction translated = {};
    translated.token = 14;
    translated.clientId = 39;
    translated.upstreamId = 37;
    uint8_t response[32] = {};
    size_t responseSize = 0;
    EXPECT_TRUE(imscompat::encodeEmptyResponse(14, 37, 0, response,
                                               sizeof(response),
                                               &responseSize));
    EXPECT_TRUE(imscompat::decodeFrame(response, responseSize, &frame) ==
                DecodeResult::kFrame);
    EXPECT_TRUE(imscompat::adaptUpstreamResponse(
            imscompat::ProtocolProfile::kObakeLegacy, translated, response,
            &frame));
    EXPECT_TRUE(frame.metadata.id == 39);

    uint8_t passthrough[sizeof(kClarkTtyRequest)];
    memcpy(passthrough, kClarkTtyRequest, sizeof(passthrough));
    EXPECT_TRUE(imscompat::decodeFrame(passthrough, sizeof(passthrough),
                                       &frame) == DecodeResult::kFrame);
    const imscompat::ClientFrameAdaptation passAdaptation =
            imscompat::adaptClientFrame(
                    imscompat::ProtocolProfile::kPassThrough, passthrough,
                    &frame);
    EXPECT_TRUE(passAdaptation.action ==
                imscompat::ClientFrameAction::kForward);
    EXPECT_TRUE(passAdaptation.clientId == 39 &&
                passAdaptation.upstreamId == 39);
    EXPECT_TRUE(memcmp(passthrough, kClarkTtyRequest,
                       sizeof(passthrough)) == 0);

    uint8_t config[sizeof(kClarkWfcModeRequest)];
    memcpy(config, kClarkWfcModeRequest, sizeof(config));
    EXPECT_TRUE(imscompat::decodeFrame(config, sizeof(config), &frame) ==
                DecodeResult::kFrame);
    const imscompat::ClientFrameAdaptation configAdaptation =
            imscompat::adaptClientFrame(
                    imscompat::ProtocolProfile::kObakeLegacy, config, &frame);
    EXPECT_TRUE(configAdaptation.action ==
                imscompat::ClientFrameAction::kRespondUnsupported);
    EXPECT_TRUE(memcmp(config, kClarkWfcModeRequest, sizeof(config)) == 0);

    EXPECT_TRUE(imscompat::encodeEmptyResponse(7, 44, 6, response,
                                               sizeof(response),
                                               &responseSize));
    EXPECT_TRUE(imscompat::decodeFrame(response, responseSize, &frame) ==
                DecodeResult::kFrame);
    EXPECT_TRUE(frame.metadata.token == 7);
    EXPECT_TRUE(frame.metadata.type == 2);
    EXPECT_TRUE(frame.metadata.id == 44);
    EXPECT_TRUE(frame.metadata.error == 6);
    EXPECT_TRUE(frame.payloadSize == 0);

    const uint8_t unsupportedIds[] = {37, 38, 44, 45, 46};
    for (uint8_t id : unsupportedIds) {
        uint8_t request[sizeof(kClarkTtyRequest)];
        memcpy(request, kClarkTtyRequest, sizeof(request));
        request[13] = id;
        EXPECT_TRUE(imscompat::decodeFrame(request, sizeof(request), &frame) ==
                    DecodeResult::kFrame);
        EXPECT_TRUE(imscompat::adaptClientFrame(
                            imscompat::ProtocolProfile::kObakeLegacy, request,
                            &frame)
                            .action ==
                    imscompat::ClientFrameAction::kRespondUnsupported);
    }

    uint8_t common[sizeof(kClarkTtyRequest)];
    memcpy(common, kClarkTtyRequest, sizeof(common));
    common[13] = 36;
    EXPECT_TRUE(imscompat::decodeFrame(common, sizeof(common), &frame) ==
                DecodeResult::kFrame);
    EXPECT_TRUE(imscompat::adaptClientFrame(
                        imscompat::ProtocolProfile::kObakeLegacy, common,
                        &frame)
                        .action == imscompat::ClientFrameAction::kForward);
}

// Synthetic, non-personal fixtures following the stock/Clark protobuf schema.
std::vector<uint8_t> callRecord(uint8_t state, uint8_t index) {
    // CallList.callAttributes is field TWO in both decompiled APKs.
    return {0x12, 13, 8, state, 0x15, index, 0, 0, 0,
            0x6a, 4, 8, 0, 16, 2};
}

bool feedCalls(imscompat::CallListBridge* bridge,
               const std::vector<uint8_t>& payload, uint8_t* output,
               size_t capacity, size_t* outputSize, uint32_t error = 0,
               uint32_t token = imscompat::kCallListQueryToken,
               uint32_t id = imscompat::kGetCurrentCallsId) {
    uint8_t wire[4096];
    size_t size = 0;
    DecodedFrame frame;
    if (!imscompat::encodeFrame(token, 2, id, error, payload.data(), payload.size(),
                                wire, sizeof(wire), &size) ||
        imscompat::decodeFrame(wire, size, &frame) != DecodeResult::kFrame) return false;
    return bridge->acceptResponse(wire, frame, output, capacity, outputSize);
}

void beginCallQuery(imscompat::CallListBridge* bridge, uint64_t nowMs) {
    uint8_t wire[32];
    size_t size = 0;
    bridge->markDirty();
    EXPECT_TRUE(bridge->startQuery(nowMs, wire, sizeof(wire), &size));
    DecodedFrame frame;
    EXPECT_TRUE(imscompat::decodeFrame(wire, size, &frame) == DecodeResult::kFrame);
    EXPECT_TRUE(frame.metadata.type == 1);
    EXPECT_TRUE(frame.metadata.id == 6);
    EXPECT_TRUE(frame.metadata.token == imscompat::kCallListQueryToken);
    EXPECT_TRUE(frame.payloadSize == 0);
}

void testCallListLifecycle() {
    imscompat::CallListBridge bridge;
    uint8_t output[4096];
    size_t size = 0;
    DecodedFrame frame;
    EXPECT_TRUE(!bridge.needsQuery());
    beginCallQuery(&bridge, 100);
    EXPECT_TRUE(!bridge.needsQuery());
    bridge.markDirty();
    bridge.markDirty();  // Multiple notifications coalesce behind one query.
    EXPECT_TRUE(!bridge.needsQuery());
    EXPECT_TRUE(feedCalls(&bridge, callRecord(2, 1), output, sizeof(output), &size));
    EXPECT_TRUE(bridge.needsQuery());
    EXPECT_TRUE(bridge.activeCount() == 1 && bridge.endedCount() == 0);
    EXPECT_TRUE(imscompat::decodeFrame(output, size, &frame) == DecodeResult::kFrame);
    EXPECT_TRUE(frame.metadata.type == 3 && frame.metadata.id == 201);
    EXPECT_TRUE(frame.metadata.token == UINT32_MAX);
    EXPECT_TRUE(frame.payloadSize == callRecord(2, 1).size());
    EXPECT_TRUE(memcmp(output + frame.payloadOffset, callRecord(2, 1).data(),
                       frame.payloadSize) == 0);  // Actual ID/state/details preserved.
    beginCallQuery(&bridge, 200);
    EXPECT_TRUE(feedCalls(&bridge, callRecord(0, 1), output, sizeof(output), &size));
    EXPECT_TRUE(!bridge.needsQuery());
    beginCallQuery(&bridge, 300);
    EXPECT_TRUE(feedCalls(&bridge, {}, output, sizeof(output), &size));
    EXPECT_TRUE(bridge.activeCount() == 0 && bridge.endedCount() == 1);
    EXPECT_TRUE(imscompat::decodeFrame(output, size, &frame) == DecodeResult::kFrame);
    EXPECT_TRUE(frame.payloadSize == callRecord(6, 1).size());
    EXPECT_TRUE(memcmp(output + frame.payloadOffset, callRecord(6, 1).data(),
                       frame.payloadSize) == 0);  // Explicit END for missing known call.
    beginCallQuery(&bridge, 400);
    EXPECT_TRUE(feedCalls(&bridge, {}, output, sizeof(output), &size));
    EXPECT_TRUE(bridge.endedCount() == 0 && bridge.updates() == 4);
    EXPECT_TRUE(imscompat::decodeFrame(output, size, &frame) == DecodeResult::kFrame);
    EXPECT_TRUE(frame.payloadSize == 0);  // No repeated END or invented calls.
    imscompat::CallListBridge reconnected;
    EXPECT_TRUE(reconnected.activeCount() == 0 && reconnected.updates() == 0);
}

void testCallListFailuresAndMultipleCalls() {
    imscompat::CallListBridge bridge;
    uint8_t output[4096];
    size_t size = 0;
    EXPECT_TRUE(!feedCalls(&bridge, {}, output, sizeof(output), &size));
    beginCallQuery(&bridge, 100);
    EXPECT_TRUE(!bridge.expired(5099) && bridge.expired(5100));
    EXPECT_TRUE(!feedCalls(&bridge, {}, output, sizeof(output), &size, 2));
    EXPECT_TRUE(!feedCalls(&bridge, {}, output, sizeof(output), &size, 0, 123));
    EXPECT_TRUE(!feedCalls(&bridge, {}, output, sizeof(output), &size, 0,
                           imscompat::kCallListQueryToken, 29));
    EXPECT_TRUE(bridge.updates() == 0);  // Errors are not authoritative emptiness.
    EXPECT_TRUE(!feedCalls(&bridge, callRecord(9, 1), output, sizeof(output), &size));
    EXPECT_TRUE(!feedCalls(&bridge, callRecord(0, 0), output, sizeof(output), &size));
    auto wrongIndexWire = callRecord(2, 1);
    wrongIndexWire[4] = 0x10;
    EXPECT_TRUE(!feedCalls(&bridge, wrongIndexWire, output, sizeof(output), &size));
    auto duplicateState = callRecord(2, 1);
    duplicateState[1] += 2;
    duplicateState.push_back(8);
    duplicateState.push_back(2);
    EXPECT_TRUE(!feedCalls(&bridge, duplicateState, output, sizeof(output), &size));
    EXPECT_TRUE(!feedCalls(&bridge, {0x12, 7, 8, 2, 0x15, 1, 0, 0, 0},
                           output, sizeof(output), &size));  // Missing CallDetails.
    EXPECT_TRUE(!feedCalls(&bridge, {0x12, 127, 8, 2}, output, sizeof(output), &size));
    EXPECT_TRUE(!feedCalls(&bridge, {0}, output, sizeof(output), &size));
    std::vector<uint8_t> both = callRecord(2, 1);
    std::vector<uint8_t> second = callRecord(4, 2);
    both.insert(both.end(), second.begin(), second.end());
    EXPECT_TRUE(!feedCalls(&bridge, both, output, 4, &size));
    EXPECT_TRUE(bridge.activeCount() == 0 && bridge.updates() == 0);
    EXPECT_TRUE(feedCalls(&bridge, both, output, sizeof(output), &size));
    EXPECT_TRUE(bridge.activeCount() == 2);
    beginCallQuery(&bridge, 200);
    EXPECT_TRUE(!feedCalls(&bridge, {}, output, sizeof(output), &size, 2));
    EXPECT_TRUE(bridge.activeCount() == 2 && bridge.endedCount() == 0);
    EXPECT_TRUE(feedCalls(&bridge, second, output, sizeof(output), &size));
    EXPECT_TRUE(bridge.activeCount() == 1 && bridge.endedCount() == 1);
    DecodedFrame frame;
    EXPECT_TRUE(imscompat::decodeFrame(output, size, &frame) == DecodeResult::kFrame);
    EXPECT_TRUE(frame.payloadSize == both.size());
    EXPECT_TRUE(memcmp(output + frame.payloadOffset + second.size(),
                       callRecord(6, 1).data(), second.size()) == 0);
    beginCallQuery(&bridge, 300);
    EXPECT_TRUE(feedCalls(&bridge, callRecord(6, 2), output, sizeof(output), &size));
    EXPECT_TRUE(bridge.activeCount() == 0 && bridge.endedCount() == 1);
    beginCallQuery(&bridge, 400);
    const auto duplicate = callRecord(2, 1);
    both = duplicate;
    both.insert(both.end(), duplicate.begin(), duplicate.end());
    EXPECT_TRUE(!feedCalls(&bridge, both, output, sizeof(output), &size));
    std::vector<uint8_t> tooMany;
    for (size_t i = 0; i <= imscompat::kMaximumImsCalls; ++i) {
        const auto record = callRecord(0, i + 1);
        tooMany.insert(tooMany.end(), record.begin(), record.end());
    }
    EXPECT_TRUE(!feedCalls(&bridge, tooMany, output, sizeof(output), &size));
    EXPECT_TRUE(bridge.activeCount() == 0);
    auto extended = callRecord(2, 3);
    extended[1] += 2;
    extended.push_back(0x78);  // Unknown Call field 15, preserved verbatim.
    extended.push_back(7);
    EXPECT_TRUE(feedCalls(&bridge, extended, output, sizeof(output), &size));
    EXPECT_TRUE(imscompat::decodeFrame(output, size, &frame) == DecodeResult::kFrame);
    EXPECT_TRUE(frame.payloadSize == extended.size());
    EXPECT_TRUE(memcmp(output + frame.payloadOffset, extended.data(), extended.size()) == 0);
    beginCallQuery(&bridge, 500);
    std::vector<uint8_t> oversizedCall(2050, 0);
    oversizedCall[0] = 0x12;
    oversizedCall[1] = 0xff;
    oversizedCall[2] = 0x0f;  // 2047-byte call, beyond the received-call limit.
    EXPECT_TRUE(!feedCalls(&bridge, oversizedCall, output, sizeof(output), &size));
    EXPECT_TRUE(bridge.activeCount() == 1 && bridge.endedCount() == 0);
}

void testGeneratedFrameQueueOrdering() {
    FrameBuffer buffer;
    EXPECT_TRUE(buffer.insertValidated(kQueryServiceStatus, sizeof(kQueryServiceStatus)));
    EXPECT_TRUE(buffer.commitWrite(5));  // A frame has already begun on the wire.
    memcpy(buffer.appendData(), kSetServiceStatus, 7);
    EXPECT_TRUE(buffer.commitAppend(7));  // Followed by an incomplete inbound frame.
    EXPECT_TRUE(buffer.insertValidated(kRegistrationResponse, sizeof(kRegistrationResponse)));
    EXPECT_TRUE(buffer.writeSize() == sizeof(kQueryServiceStatus) - 5 + sizeof(kRegistrationResponse));
    EXPECT_TRUE(memcmp(buffer.writeData(), kQueryServiceStatus + 5,
                       sizeof(kQueryServiceStatus) - 5) == 0);
    EXPECT_TRUE(memcmp(buffer.writeData() + sizeof(kQueryServiceStatus) - 5,
                       kRegistrationResponse, sizeof(kRegistrationResponse)) == 0);
    EXPECT_TRUE(buffer.decodeSize() == 7);
    EXPECT_TRUE(memcmp(buffer.decodeData(), kSetServiceStatus, 7) == 0);
    memcpy(buffer.appendData(), kSetServiceStatus + 7, sizeof(kSetServiceStatus) - 7);
    EXPECT_TRUE(buffer.commitAppend(sizeof(kSetServiceStatus) - 7));
    EXPECT_TRUE(buffer.replaceDecoded(sizeof(kSetServiceStatus),
                                      kUnsolicitedNoToken, sizeof(kUnsolicitedNoToken)));
    EXPECT_TRUE(buffer.commitValidated(sizeof(kUnsolicitedNoToken)));
    EXPECT_TRUE(buffer.commitWrite(buffer.writeSize()));
    EXPECT_TRUE(!buffer.hasBufferedData());
    memcpy(buffer.appendData(), kUnsolicitedNoToken, sizeof(kUnsolicitedNoToken));
    EXPECT_TRUE(buffer.commitAppend(sizeof(kUnsolicitedNoToken)));
    memcpy(buffer.appendData(), kQueryServiceStatus, 6);
    EXPECT_TRUE(buffer.commitAppend(6));
    EXPECT_TRUE(buffer.replaceDecoded(sizeof(kUnsolicitedNoToken),
                                      kSetServiceStatus, sizeof(kSetServiceStatus)));
    EXPECT_TRUE(buffer.commitValidated(sizeof(kSetServiceStatus)));
    EXPECT_TRUE(buffer.decodeSize() == 6);
    EXPECT_TRUE(memcmp(buffer.decodeData(), kQueryServiceStatus, 6) == 0);
    EXPECT_TRUE(buffer.commitWrite(buffer.writeSize()));
    EXPECT_TRUE(buffer.discardDecoded(6));
    EXPECT_TRUE(!buffer.hasBufferedData());
    uint8_t large[imscompat::kMaximumWireFrameSize] = {};
    EXPECT_TRUE(buffer.insertValidated(large, sizeof(large)));
    EXPECT_TRUE(!buffer.insertValidated(kQueryServiceStatus, sizeof(kQueryServiceStatus)));
    EXPECT_TRUE(buffer.writeSize() == sizeof(large));
}

void testVoltePolicyClassification() {
    DecodedFrame frame;
    imscompat::ServiceStatusUpdate update;
    EXPECT_TRUE(imscompat::decodeFrame(kSetServiceStatus, sizeof(kSetServiceStatus), &frame)
                == DecodeResult::kFrame);
    EXPECT_TRUE(imscompat::decodeServiceStatusRequest(kSetServiceStatus, frame, &update)
                == imscompat::ServiceDecodeResult::kDecoded);
    bool enabled = false;
    EXPECT_TRUE(imscompat::classifyVolteRequest(update, &enabled)
                == imscompat::VolteRequestKind::kTranslate && enabled);
    update.access[0].status = 0;
    EXPECT_TRUE(imscompat::classifyVolteRequest(update, &enabled)
                == imscompat::VolteRequestKind::kTranslate && !enabled);
    update.access[0].status = 1;
    EXPECT_TRUE(imscompat::classifyVolteRequest(update, &enabled)
                == imscompat::VolteRequestKind::kUnsupported);
    update.access[0].status = 2;
    update.access[0].networkMode = imscompat::kRadioTechIwlan;
    EXPECT_TRUE(imscompat::classifyVolteRequest(update, &enabled)
                == imscompat::VolteRequestKind::kUnrelated);
    update.access[0].networkMode = imscompat::kRadioTechLte;
    update.callType = imscompat::kCallTypeVideo;
    EXPECT_TRUE(imscompat::classifyVolteRequest(update, &enabled)
                == imscompat::VolteRequestKind::kUnrelated);
    update.callType = imscompat::kCallTypeVoice;
    update.accessCount = 2;
    EXPECT_TRUE(imscompat::classifyVolteRequest(update, &enabled)
                == imscompat::VolteRequestKind::kUnsupported);
    update.isValid = false;
    update.accessCount = 0;
    EXPECT_TRUE(imscompat::classifyVolteRequest(update, &enabled)
                == imscompat::VolteRequestKind::kTranslate && !enabled);
}

void testVoltePolicyTwoPhaseAndOrdering() {
    imscompat::VoltePolicyBridge bridge;
    imscompat::VolteCompletion completion;
    uint32_t token = 0;
    bool enabled = false;
    EXPECT_TRUE(bridge.record(1, true, 0));
    EXPECT_TRUE(bridge.record(2, false, 0));
    EXPECT_TRUE(!bridge.record(1, false, 0));
    EXPECT_TRUE(!bridge.nextJob(0, &token, &enabled));
    EXPECT_TRUE(bridge.upstreamResponse(2, true));
    EXPECT_TRUE(!bridge.nextJob(0, &token, &enabled)); // Preserve request arrival order.
    EXPECT_TRUE(bridge.upstreamResponse(1, true));
    EXPECT_TRUE(!bridge.takeCompletion(&completion)); // Native ACK is not success yet.
    EXPECT_TRUE(bridge.nextJob(0, &token, &enabled) && token == 1 && enabled);
    EXPECT_TRUE(!bridge.nextJob(0, &token, &enabled));
    EXPECT_TRUE(!bridge.finishJob(999, true, false, 1));
    EXPECT_TRUE(bridge.finishJob(1, true, false, 1));
    EXPECT_TRUE(bridge.takeCompletion(&completion) && completion.token == 1
                && completion.successful && completion.enabled);
    EXPECT_TRUE(bridge.nextJob(2, &token, &enabled) && token == 2 && !enabled);
    EXPECT_TRUE(bridge.finishJob(2, false, false, 3));
    EXPECT_TRUE(bridge.takeCompletion(&completion) && !completion.successful);
    EXPECT_TRUE(!bridge.hasPending());
    EXPECT_TRUE(bridge.record(3, true, 4));
    EXPECT_TRUE(!bridge.upstreamResponse(3, false));
    EXPECT_TRUE(!bridge.contains(3));
    EXPECT_TRUE(!bridge.nextJob(5, &token, &enabled));
}

void testVoltePolicyLimitsAndRetries() {
    imscompat::VoltePolicyBridge bridge;
    uint32_t token;
    bool enabled;
    imscompat::VolteCompletion completion;
    EXPECT_TRUE(bridge.record(1, true, 0));
    EXPECT_TRUE(bridge.upstreamResponse(1, true));
    EXPECT_TRUE(bridge.nextJob(0, &token, &enabled));
    EXPECT_TRUE(bridge.finishJob(1, false, true, 0));
    EXPECT_TRUE(!bridge.nextJob(1, &token, &enabled));
    EXPECT_TRUE(bridge.nextJob(2000, &token, &enabled));
    EXPECT_TRUE(bridge.finishJob(1, false, true, 2000));
    EXPECT_TRUE(bridge.nextJob(5000, &token, &enabled));
    EXPECT_TRUE(bridge.finishJob(1, false, true, 5000));
    EXPECT_TRUE(bridge.takeCompletion(&completion) && !completion.successful);
    for (size_t i = 0; i < imscompat::VoltePolicyBridge::kCapacity; ++i) {
        EXPECT_TRUE(bridge.record(static_cast<uint32_t>(i), true, 0));
    }
    EXPECT_TRUE(!bridge.record(99, true, 0));
    EXPECT_TRUE(!bridge.expired(59999));
    EXPECT_TRUE(bridge.expired(60000));
    imscompat::VoltePolicyBridge newSession;
    EXPECT_TRUE(!newSession.contains(1)); // No policy inherited from an old connection/SIM.
}

}  // namespace

int main() {
    testGoldenFrames();
    testFragmentedAndCoalescedReads();
    testMalformedFrames();
    testPartialWritesAndTrailingFragment();
    testDiscardDecodedFrame();
    testTransactions();
    testTransactionCapacity();
    testProtocolAdaptation();
    testServiceStatusDecode();
    testAcknowledgedServiceState();
    testServiceControlProtocol();
    testCallListLifecycle();
    testCallListFailuresAndMultipleCalls();
    testGeneratedFrameQueueOrdering();
    testVoltePolicyClassification();
    testVoltePolicyTwoPhaseAndOrdering();
    testVoltePolicyLimitsAndRetries();
    if (failures != 0) {
        fprintf(stderr, "RESULT: FAIL (%u assertions)\n", failures);
        return 1;
    }
    puts("RESULT: PASS — IMS framing, call-list/VoLTE bridges, service state and control protocol");
    return 0;
}
