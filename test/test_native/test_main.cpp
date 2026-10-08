#include <unity.h>
#include <stdint.h>
#include <string.h>
#include "ct_tx_guard.h"
#include "ct_index_parser.h"
#include "ct_time.h"
#include "ct_hex_parser.h"
#include "ct_obd_parser.h"
#include "ct_dbc_validation.h"
#include "ct_verify.h"
#include "ct_password.h"
#include "ct_ota_header.h"
#include "ct_storage_policy.h"
#include "ct_buttons.h"
#include "ct_json_validation.h"
#include "ct_origin.h"
#include "ct_battery.h"
#include "ct_obd_validity.h"
#include "ct_obd_formulas.h"
#include "ct_listen_override.h"
#include "ct_can_config.h"
#include "ct_storage_guard.h"
#include "ct_can_record.h"
#include "can_service.h"
#include "ecu_sim.h"
#include "ct_login_lock.h"
#include "ct_dbc_store.h"
#include "ct_sha256.h"
#include "ct_dbc_manifest.h"
#include "ct_http_body_limit.h"
#include "ct_sync_policy.h"
#include "ct_command_guard.h"

void setUp(void) {}
void tearDown(void) {}

class MockCanInterface : public CanInterface {
public:
    bool beginResult = true;
    bool active = false;
    bool listenOnly = true;
    bool lastReconfigure = false;
    uint8_t beginCalls = 0;
    uint8_t sendCalls = 0;
    uint8_t receiveCalls = 0;
    uint8_t reconfigureCalls = 0;
    uint32_t lastTxId = 0;
    bool scriptedRx = false;
    CanMessage rxFrames[64] = {};
    uint8_t rxFrameCount = 0;
    uint8_t rxFrameIndex = 0;

    bool begin() override {
        ++beginCalls;
        active = beginResult;
        return beginResult;
    }
    void end() override { active = false; }
    bool sendMessage(const CanMessage& msg, uint32_t) override {
        ++sendCalls;
        lastTxId = msg.id;
        return active && !listenOnly;
    }
    bool receiveMessage(CanMessage& msg, uint32_t) override {
        ++receiveCalls;
        msg.id = 0x123;
        return active;
    }
    bool receiveMessageNonBlocking(CanMessage& msg) override {
        ++receiveCalls;
        if (scriptedRx) {
            if (!active || rxFrameIndex >= rxFrameCount) return false;
            msg = rxFrames[rxFrameIndex++];
            return true;
        }
        msg.id = 0x123;
        return active;
    }
    void flushRxQueue() override {}
    bool isActive() override { return active; }
    CanError getLastError() override { return CAN_OK; }
    bool recoverFromBusOff() override { return active; }
    void getStats(uint32_t& tx, uint32_t& rx, uint32_t& errors) override {
        tx = rx = errors = 0;
    }
    bool getDiagnostics(CanDiagnostics& out) override {
        memset(&out, 0, sizeof(out));
        out.driverReady = active;
        return active;
    }
    uint32_t getLastRxTime() const override { return 0; }
    bool reconfigureMode(bool mode) override {
        ++reconfigureCalls;
        lastReconfigure = mode;
        listenOnly = mode;
        return active;
    }
    bool isListenOnlyActive() override { return active && listenOnly; }
};

void test_tx_guard_listen_only(void) {
    TEST_ASSERT_EQUAL(CT_TX_ERR_LISTEN_ONLY, ctTxGuard(true, true, 8));
    TEST_ASSERT_EQUAL(CT_TX_ERR_LISTEN_ONLY, ctTxGuard(true, true, 0));
}
void test_tx_guard_initialization_and_dlc(void) {
    TEST_ASSERT_EQUAL(CT_TX_ERR_NOT_INITIALIZED, ctTxGuard(false, false, 8));
    TEST_ASSERT_EQUAL(CT_TX_ERR_LENGTH, ctTxGuard(true, false, 9));
    TEST_ASSERT_EQUAL(CT_TX_OK, ctTxGuard(true, false, 8));
}
void test_tx_id_validity(void) {
    TEST_ASSERT_TRUE(ctTxIdValid(0x7FF, false));
    TEST_ASSERT_FALSE(ctTxIdValid(0x800, false));
    TEST_ASSERT_FALSE(ctTxIdValid(0x1234, false));
    TEST_ASSERT_TRUE(ctTxIdValid(0x1FFFFFFF, true));
    TEST_ASSERT_FALSE(ctTxIdValid(0x20000000, true));
}
void test_battery_voltage_availability(void) {
    TEST_ASSERT_FALSE(ctBatteryVoltageAvailable(0.0f));
    TEST_ASSERT_FALSE(ctBatteryVoltageAvailable(-1.0f));
    TEST_ASSERT_FALSE(ctBatteryVoltageAvailable(5.9f));
    TEST_ASSERT_TRUE(ctBatteryVoltageAvailable(6.0f));
    TEST_ASSERT_TRUE(ctBatteryVoltageAvailable(12.6f));
    TEST_ASSERT_TRUE(ctBatteryVoltageAvailable(36.0f));
    TEST_ASSERT_FALSE(ctBatteryVoltageAvailable(36.1f));
}
void test_can_frame_identity_includes_format(void) {
    TEST_ASSERT_TRUE(ctSameCanFrameId(0x123, false, 0x123, false));
    TEST_ASSERT_TRUE(ctSameCanFrameId(0x123, true, 0x123, true));
    TEST_ASSERT_FALSE(ctSameCanFrameId(0x123, false, 0x123, true));
    TEST_ASSERT_FALSE(ctSameCanFrameId(0x123, true, 0x124, true));
}
void test_can_bus_pin_conflicts_are_rejected(void) {
    TEST_ASSERT_TRUE(ctCanPinsConflictFree(9, 6, 15, 16));
    TEST_ASSERT_FALSE(ctCanPinsConflictFree(15, 6, 15, 16));
    TEST_ASSERT_FALSE(ctCanPinsConflictFree(9, 16, 15, 16));
    TEST_ASSERT_FALSE(ctCanPinsConflictFree(9, 6, 16, 16));
}
void test_mcp2515_supported_bitrates(void) {
    TEST_ASSERT_TRUE(ctMcp2515BitrateValid(100000));
    TEST_ASSERT_TRUE(ctMcp2515BitrateValid(125000));
    TEST_ASSERT_TRUE(ctMcp2515BitrateValid(250000));
    TEST_ASSERT_TRUE(ctMcp2515BitrateValid(500000));
    TEST_ASSERT_TRUE(ctMcp2515BitrateValid(1000000));
    TEST_ASSERT_FALSE(ctMcp2515BitrateValid(800000));
    TEST_ASSERT_FALSE(ctMcp2515BitrateValid(0));
}
void test_partition_layout_must_fit_detected_flash(void) {
    TEST_ASSERT_TRUE(ctPartitionFitsFlash(16u * 1024u * 1024u, 16u * 1024u * 1024u));
    TEST_ASSERT_FALSE(ctPartitionFitsFlash(8u * 1024u * 1024u, 16u * 1024u * 1024u));
    TEST_ASSERT_FALSE(ctPartitionFitsFlash(4u * 1024u * 1024u, 16u * 1024u * 1024u));
    TEST_ASSERT_FALSE(ctPartitionFitsFlash(16u * 1024u * 1024u, 0u));
    TEST_ASSERT_FALSE(ctPartitionFitsFlash(0u, 16u * 1024u * 1024u));
}
void test_filesystem_ota_requires_mounted_empty_user_storage(void) {
    TEST_ASSERT_TRUE(ctFilesystemOtaAllowed(true, false, false));
    TEST_ASSERT_FALSE(ctFilesystemOtaAllowed(false, false, false));
    TEST_ASSERT_FALSE(ctFilesystemOtaAllowed(true, true, false));
    TEST_ASSERT_FALSE(ctFilesystemOtaAllowed(true, false, true));
    TEST_ASSERT_FALSE(ctFilesystemOtaAllowed(false, true, true));
}
void test_http_body_limits_bound_forms_and_stream_imports(void) {
    const size_t formLimit = 2048;
    const size_t importLimit = 8192 * 3 + 32;
    const size_t dbcLimit = 400 * 1024 + 2048;
    TEST_ASSERT_EQUAL(formLimit,
                      ctHttpBodyLimitForPath("/api/storage", formLimit, importLimit, dbcLimit));
    TEST_ASSERT_EQUAL(importLimit,
                      ctHttpBodyLimitForPath("/api/vehicles/custom/import",
                                             formLimit, importLimit, dbcLimit));
    TEST_ASSERT_EQUAL(dbcLimit,
                      ctHttpBodyLimitForPath("/api/dbc/upload",
                                             formLimit, importLimit, dbcLimit));
    TEST_ASSERT_EQUAL(0,
                      ctHttpBodyLimitForPath("/update", formLimit, importLimit, dbcLimit));
    TEST_ASSERT_FALSE(ctHttpBodyExceedsLimit(formLimit, false, formLimit));
    TEST_ASSERT_TRUE(ctHttpBodyExceedsLimit(formLimit + 1, false, formLimit));
    TEST_ASSERT_TRUE(ctHttpBodyExceedsLimit(0, true, formLimit));
    TEST_ASSERT_FALSE(ctHttpBodyExceedsLimit(0, true, 0));
}
void test_can_service_initializes_each_bus_independently(void) {
    MockCanInterface can0;
    MockCanInterface can1;
    can0.beginResult = false;
    can1.beginResult = true; // Align with the new contract
    CANService service(can0, can1);
    TEST_ASSERT_TRUE(service.begin());
    TEST_ASSERT_EQUAL(1, can0.beginCalls);
    TEST_ASSERT_EQUAL(1, can1.beginCalls);
    TEST_ASSERT_FALSE(service.isActive(CAN_BUS_1));
    TEST_ASSERT_TRUE(service.isActive(CAN_BUS_2));
}
void test_can_service_starts_with_can1_only(void) {
    MockCanInterface can1;
    MockCanInterface can2;
    can1.beginResult = true;
    can2.beginResult = false;
    CANService service(can1, can2);
    TEST_ASSERT_TRUE(service.begin());
    TEST_ASSERT_TRUE(service.isActive(CAN_BUS_1));
    TEST_ASSERT_FALSE(service.isActive(CAN_BUS_2));
}
void test_can_service_reports_unavailable_when_both_fail(void) {
    MockCanInterface can1;
    MockCanInterface can2;
    can1.beginResult = false;
    can2.beginResult = false;
    CANService service(can1, can2);
    TEST_ASSERT_FALSE(service.begin());
    TEST_ASSERT_EQUAL(1, can1.beginCalls);
    TEST_ASSERT_EQUAL(1, can2.beginCalls);
}
void test_can_service_routes_legacy_calls_to_can1(void) {
    MockCanInterface can0;
    MockCanInterface can1;
    can0.active = can1.active = true;
    can0.listenOnly = can1.listenOnly = false;
    CANService service(can0, can1);
    CanMessage message = {};
    message.id = 0x321;
    TEST_ASSERT_TRUE(service.sendMessage(message));
    TEST_ASSERT_EQUAL(1, can0.sendCalls);
    TEST_ASSERT_EQUAL(0, can1.sendCalls);
    TEST_ASSERT_TRUE(service.sendMessage(CAN_BUS_1, message));
    TEST_ASSERT_EQUAL(2, can0.sendCalls);
    TEST_ASSERT_EQUAL(0, can1.sendCalls);
    TEST_ASSERT_TRUE(service.sendMessage(CAN_BUS_2, message));
    TEST_ASSERT_EQUAL(2, can0.sendCalls);
    TEST_ASSERT_EQUAL(1, can1.sendCalls);
}
void test_can_service_does_not_fallback_from_selected_inactive_bus(void) {
    MockCanInterface can1;
    MockCanInterface can2;
    can1.active = true;
    can1.listenOnly = false;
    can2.active = false;
    CANService service(can1, can2);
    CanMessage message = {};
    message.id = 0x321;

    TEST_ASSERT_FALSE(service.sendMessage(CAN_BUS_2, message));
    TEST_ASSERT_EQUAL(0, can1.sendCalls);
    TEST_ASSERT_EQUAL(1, can2.sendCalls);
}

void test_can_service_fans_out_frames_to_independent_subscribers(void) {
    MockCanInterface can1;
    MockCanInterface can2;
    can1.active = true;
    can1.scriptedRx = true;
    can1.rxFrameCount = 1;
    can1.rxFrames[0] = {};
    can1.rxFrames[0].id = 0x456;
    can1.rxFrames[0].length = 2;
    can1.rxFrames[0].data[0] = 0x12;
    can1.rxFrames[0].data[1] = 0x34;
    CANService service(can1, can2);
    TEST_ASSERT_TRUE(service.subscribeRx(CAN_BUS_1, CAN_RX_OBD));
    TEST_ASSERT_TRUE(service.subscribeRx(CAN_BUS_1, CAN_RX_LEARN));
    TEST_ASSERT_EQUAL(1, service.pumpRx());

    CanRxFrame obdFrame = {};
    CanRxFrame learnFrame = {};
    TEST_ASSERT_TRUE(service.receiveRx(CAN_BUS_1, CAN_RX_OBD, obdFrame));
    TEST_ASSERT_TRUE(service.receiveRx(CAN_BUS_1, CAN_RX_LEARN, learnFrame));
    TEST_ASSERT_EQUAL(0x456, obdFrame.message.id);
    TEST_ASSERT_EQUAL(0x456, learnFrame.message.id);
    TEST_ASSERT_EQUAL(0x12, obdFrame.message.data[0]);
    TEST_ASSERT_EQUAL(0x34, learnFrame.message.data[1]);
    TEST_ASSERT_FALSE(service.receiveRx(CAN_BUS_1, CAN_RX_OBD, obdFrame));
    TEST_ASSERT_FALSE(service.receiveRx(CAN_BUS_1, CAN_RX_LEARN, learnFrame));
}

void test_can_service_pumps_both_buses_independently(void) {
    MockCanInterface can1;
    MockCanInterface can2;
    can1.active = can2.active = true;
    can1.scriptedRx = can2.scriptedRx = true;
    can1.rxFrameCount = can2.rxFrameCount = 2;
    can1.rxFrames[0].id = 0x111;
    can1.rxFrames[1].id = 0x112;
    can2.rxFrames[0].id = 0x221;
    can2.rxFrames[1].id = 0x222;

    CANService service(can1, can2);
    TEST_ASSERT_TRUE(service.subscribeRx(CAN_BUS_1, CAN_RX_MONITOR));
    TEST_ASSERT_TRUE(service.subscribeRx(CAN_BUS_2, CAN_RX_MONITOR));
    TEST_ASSERT_EQUAL(4, service.pumpRx(2));

    CanRxFrame frame = {};
    TEST_ASSERT_TRUE(service.receiveRx(CAN_BUS_1, CAN_RX_MONITOR, frame));
    TEST_ASSERT_EQUAL(CAN_BUS_1, frame.bus);
    TEST_ASSERT_EQUAL_HEX32(0x111, frame.message.id);
    TEST_ASSERT_TRUE(service.receiveRx(CAN_BUS_1, CAN_RX_MONITOR, frame));
    TEST_ASSERT_EQUAL_HEX32(0x112, frame.message.id);
    TEST_ASSERT_FALSE(service.receiveRx(CAN_BUS_1, CAN_RX_MONITOR, frame));

    TEST_ASSERT_TRUE(service.receiveRx(CAN_BUS_2, CAN_RX_MONITOR, frame));
    TEST_ASSERT_EQUAL(CAN_BUS_2, frame.bus);
    TEST_ASSERT_EQUAL_HEX32(0x221, frame.message.id);
    TEST_ASSERT_TRUE(service.receiveRx(CAN_BUS_2, CAN_RX_MONITOR, frame));
    TEST_ASSERT_EQUAL_HEX32(0x222, frame.message.id);
    TEST_ASSERT_FALSE(service.receiveRx(CAN_BUS_2, CAN_RX_MONITOR, frame));
}

void test_can_service_fans_out_to_monitor_and_recorder(void) {
    MockCanInterface can1;
    MockCanInterface can2;
    can1.active = true;
    can1.scriptedRx = true;
    can1.rxFrameCount = 1;
    can1.rxFrames[0].id = 0x456;
    can1.rxFrames[0].length = 2;
    can1.rxFrames[0].data[0] = 0xAB;
    can1.rxFrames[0].data[1] = 0xCD;

    CANService service(can1, can2);
    TEST_ASSERT_TRUE(service.subscribeRx(CAN_BUS_1, CAN_RX_MONITOR));
    TEST_ASSERT_TRUE(service.subscribeRx(CAN_BUS_1, CAN_RX_RECORDER));
    TEST_ASSERT_EQUAL(1, service.pumpRx());

    CanRxFrame monitorFrame = {};
    CanRxFrame recordFrame = {};
    TEST_ASSERT_TRUE(service.receiveRx(CAN_BUS_1, CAN_RX_MONITOR, monitorFrame));
    TEST_ASSERT_TRUE(service.receiveRx(CAN_BUS_1, CAN_RX_RECORDER, recordFrame));
    TEST_ASSERT_EQUAL_HEX32(0x456, monitorFrame.message.id);
    TEST_ASSERT_EQUAL_HEX32(monitorFrame.message.id, recordFrame.message.id);
    TEST_ASSERT_EQUAL_HEX8(0xAB, recordFrame.message.data[0]);
    TEST_ASSERT_EQUAL_HEX8(0xCD, recordFrame.message.data[1]);
}

void test_can_record_format_preserves_channel_and_payload(void) {
    CanRxFrame frame = {};
    frame.bus = CAN_BUS_2;
    frame.receivedAtMs = 123;
    frame.message.id = 0x123;
    frame.message.isExtended = true;
    frame.message.length = 2;
    frame.message.data[0] = 0xA1;
    frame.message.data[1] = 0xB2;

    char line[64] = {};
    size_t written = 0;
    TEST_ASSERT_TRUE(ctFormatCanRecordLine(frame, line, sizeof(line), written));
    TEST_ASSERT_EQUAL_STRING("123,CAN2,00000123,1,0,2,A1B2\n", line);
    TEST_ASSERT_EQUAL(strlen(line), written);
}

void test_can_record_format_rejects_invalid_or_truncated_frames(void) {
    CanRxFrame frame = {};
    frame.bus = CAN_BUS_1;
    frame.message.length = 9;
    char line[64] = {};
    size_t written = 0;
    TEST_ASSERT_FALSE(ctFormatCanRecordLine(frame, line, sizeof(line), written));

    frame.message.length = 0;
    char tiny[4] = {};
    TEST_ASSERT_FALSE(ctFormatCanRecordLine(frame, tiny, sizeof(tiny), written));
}

void test_can_record_filename_allowlist(void) {
    TEST_ASSERT_TRUE(ctCanRecordFilenameValid("can0000.csv"));
    TEST_ASSERT_TRUE(ctCanRecordFilenameValid("can9999.csv"));
    TEST_ASSERT_FALSE(ctCanRecordFilenameValid("/can0000.csv"));
    TEST_ASSERT_FALSE(ctCanRecordFilenameValid("can00x0.csv"));
    TEST_ASSERT_FALSE(ctCanRecordFilenameValid("can0000.csv/../config"));
    TEST_ASSERT_FALSE(ctCanRecordFilenameValid("custom.csv"));
}

void test_can_service_bounds_consumer_queues_and_counts_drops(void) {
    MockCanInterface can1;
    MockCanInterface can2;
    can1.active = true;
    can1.scriptedRx = true;
    can1.rxFrameCount = CANService::RX_QUEUE_DEPTH + 3;
    for (uint8_t i = 0; i < can1.rxFrameCount; ++i) {
        can1.rxFrames[i].id = (uint32_t)(0x100 + i);
    }
    CANService service(can1, can2);
    TEST_ASSERT_TRUE(service.subscribeRx(CAN_BUS_1, CAN_RX_MONITOR));
    TEST_ASSERT_EQUAL(CANService::RX_QUEUE_DEPTH + 3, service.pumpRx(40));
    TEST_ASSERT_EQUAL(3, service.getRxDrops(CAN_BUS_1, CAN_RX_MONITOR));

    CanRxFrame frame = {};
    for (uint8_t i = 0; i < CANService::RX_QUEUE_DEPTH; ++i) {
        TEST_ASSERT_TRUE(service.receiveRx(CAN_BUS_1, CAN_RX_MONITOR, frame));
        TEST_ASSERT_EQUAL((uint32_t)(0x100 + i), frame.message.id);
    }
    TEST_ASSERT_FALSE(service.receiveRx(CAN_BUS_1, CAN_RX_MONITOR, frame));
}
void test_bounded_index_parser_rejects_wraparound(void) {
    uint8_t index = 0;
    TEST_ASSERT_TRUE(ctParseBoundedIndex("7", 8, index));
    TEST_ASSERT_EQUAL(7, index);
    TEST_ASSERT_FALSE(ctParseBoundedIndex("8", 8, index));
    TEST_ASSERT_FALSE(ctParseBoundedIndex("256", 8, index));
    TEST_ASSERT_FALSE(ctParseBoundedIndex("65543", 8, index));
    TEST_ASSERT_FALSE(ctParseBoundedIndex("-1", 8, index));
    TEST_ASSERT_FALSE(ctParseBoundedIndex("1x", 8, index));
    TEST_ASSERT_FALSE(ctParseBoundedIndex("", 8, index));
    TEST_ASSERT_TRUE(ctParseBoundedIndex("9", 10, index));
    TEST_ASSERT_EQUAL(9, index);
    TEST_ASSERT_FALSE(ctParseBoundedIndex("10", 10, index));
}
void test_strict_decimal_and_boolean_parsing(void) {
    uint32_t value = 0;
    bool flag = false;
    TEST_ASSERT_TRUE(ctParseUnsignedDecimal("500000", 1000000, value));
    TEST_ASSERT_EQUAL_UINT32(500000, value);
    TEST_ASSERT_FALSE(ctParseUnsignedDecimal("500000x", 1000000, value));
    TEST_ASSERT_FALSE(ctParseUnsignedDecimal("4294967296", UINT32_MAX, value));
    TEST_ASSERT_TRUE(ctParseBoolean("true", flag));
    TEST_ASSERT_TRUE(flag);
    TEST_ASSERT_TRUE(ctParseBoolean("false", flag));
    TEST_ASSERT_FALSE(flag);
    TEST_ASSERT_FALSE(ctParseBoolean("garbage", flag));
}
void test_wrap_safe_timer(void) {
    const uint32_t start = 0xFFFFFF00u;
    const uint32_t now = 0x00000100u;
    TEST_ASSERT_TRUE(ctElapsedAtLeast(now, start, 512));
    TEST_ASSERT_FALSE(ctElapsedAtLeast(now, start, 513));
    TEST_ASSERT_TRUE(ctElapsedMoreThan(now, start, 511));
    TEST_ASSERT_FALSE(ctElapsedMoreThan(now, start, 512));
}

void test_hex_standard_and_extended(void) {
    uint32_t v = 0;
    TEST_ASSERT_TRUE(ctParseHexUint32("7FF", v, 8)); TEST_ASSERT_EQUAL_HEX32(0x7FF, v);
    TEST_ASSERT_TRUE(ctParseHexUint32("0x1ABCDE", v, 8)); TEST_ASSERT_EQUAL_HEX32(0x1ABCDE, v);
    TEST_ASSERT_TRUE(ctParseHexUint32("ABCDEF01", v, 8)); TEST_ASSERT_EQUAL_HEX32(0xABCDEF01, v);
}
void test_hex_rejects_bad_input(void) {
    uint32_t v = 0; uint8_t b = 0;
    TEST_ASSERT_FALSE(ctParseHexUint32("", v, 8));
    TEST_ASSERT_FALSE(ctParseHexUint32("0x", v, 8));
    TEST_ASSERT_FALSE(ctParseHexUint32("12G4", v, 8));
    TEST_ASSERT_FALSE(ctParseHexUint32("123456789", v, 8));
    TEST_ASSERT_FALSE(ctParseHexUint32("7FFjunk", v, 8));
    TEST_ASSERT_TRUE(ctParseHexByteToken("FF", b)); TEST_ASSERT_EQUAL_HEX8(0xFF, b);
    TEST_ASSERT_FALSE(ctParseHexByteToken("100", b));
    TEST_ASSERT_FALSE(ctParseHexByteToken("GG", b));
}

void test_obd_valid_pid_response_uses_pci_length(void) {
    const uint8_t frame[] = {0x06, 0x41, 0x0C, 0x1A, 0xF8, 0xAA, 0xBB, 0xCC};
    CtObdSingleFrame p;
    TEST_ASSERT_TRUE(ctParseObdSingleFrame(frame, 8, 0x41, 0x0C, p));
    TEST_ASSERT_EQUAL(6, p.payloadLength);
    TEST_ASSERT_EQUAL(3, p.dataOffset);
}
void test_obd_reply_requires_standard_data_frame(void) {
    TEST_ASSERT_TRUE(ctIsObdReplyFrame(0x7E8, false, false));
    TEST_ASSERT_TRUE(ctIsObdReplyFrame(0x7E9, false, false));
    TEST_ASSERT_TRUE(ctIsObdReplyFrame(0x7EA, false, false));
    TEST_ASSERT_TRUE(ctIsObdReplyFrame(0x7EF, false, false));
    TEST_ASSERT_FALSE(ctIsObdReplyFrame(0x7E8, true, false));
    TEST_ASSERT_FALSE(ctIsObdReplyFrame(0x7E8, false, true));
    TEST_ASSERT_FALSE(ctIsObdReplyFrame(0x7E7, false, false));
    TEST_ASSERT_FALSE(ctIsObdReplyFrame(0x7F0, false, false));
}
void test_obd_rejects_inconsistent_dlc_and_pci(void) {
    CtObdSingleFrame p;
    const uint8_t shortFrame[] = {0x06, 0x41, 0x0C, 0x1A};
    const uint8_t multiFrame[] = {0x10, 0x06, 0x41, 0x0C, 0x1A, 0xF8, 0x00, 0x00};
    TEST_ASSERT_FALSE(ctParseObdSingleFrame(shortFrame, sizeof(shortFrame), 0x41, 0x0C, p));
    TEST_ASSERT_FALSE(ctParseObdSingleFrame(multiFrame, sizeof(multiFrame), 0x41, 0x0C, p));
}
void test_obd_rejects_wrong_service_or_pid(void) {
    CtObdSingleFrame p;
    const uint8_t frame[] = {0x06, 0x41, 0x0D, 0x40, 0, 0, 0, 0};
    TEST_ASSERT_FALSE(ctParseObdSingleFrame(frame, 8, 0x41, 0x0C, p));
    TEST_ASSERT_FALSE(ctParseObdSingleFrame(frame, 8, 0x43, 0xFF, p));
}
void test_obd_clear_dtc_requires_positive_ecu_ack(void) {
    const uint8_t positive[] = {0x01, 0x44, 0, 0, 0, 0, 0, 0};
    const uint8_t negative[] = {0x03, 0x7F, 0x04, 0x22, 0, 0, 0, 0};
    const uint8_t wrongService[] = {0x03, 0x7F, 0x03, 0x22, 0, 0, 0, 0};
    const uint8_t malformed[] = {0x02, 0x7F, 0x04, 0, 0, 0, 0, 0};
    uint8_t responseCode = 0;

    TEST_ASSERT_TRUE(ctParseObdPositiveServiceAck(positive, sizeof(positive), 0x44));
    TEST_ASSERT_FALSE(ctParseObdPositiveServiceAck(positive, sizeof(positive), 0x43));
    TEST_ASSERT_TRUE(ctParseObdNegativeResponse(negative, sizeof(negative), 0x04,
                                               responseCode));
    TEST_ASSERT_EQUAL_HEX8(0x22, responseCode);
    TEST_ASSERT_FALSE(ctParseObdNegativeResponse(wrongService, sizeof(wrongService),
                                                0x04, responseCode));
    TEST_ASSERT_FALSE(ctParseObdNegativeResponse(malformed, sizeof(malformed),
                                                0x04, responseCode));
}
void test_dtc_response_and_padding(void) {
    const uint8_t frame[] = {0x07, 0x43, 0x01, 0x23, 0x00, 0x00, 0x00, 0x00};
    CtObdSingleFrame p;
    TEST_ASSERT_TRUE(ctParseObdSingleFrame(frame, 8, 0x43, 0xFF, p));
    TEST_ASSERT_EQUAL(2, p.dataOffset);
    TEST_ASSERT_EQUAL_HEX16(0x0123, (uint16_t)((frame[2] << 8) | frame[3]));
    TEST_ASSERT_EQUAL_HEX16(0x0000, (uint16_t)((frame[4] << 8) | frame[5]));
    TEST_ASSERT_TRUE(ctDtcPayloadHasValidPairLength(1));
    TEST_ASSERT_TRUE(ctDtcPayloadHasValidPairLength(3));
    TEST_ASSERT_FALSE(ctDtcPayloadHasValidPairLength(2));

    const uint8_t noDtc[] = {0x01, 0x43};
    TEST_ASSERT_TRUE(ctParseObdSingleFrame(noDtc, sizeof(noDtc), 0x43, 0xFF, p));
    TEST_ASSERT_EQUAL(0, p.pid);
    TEST_ASSERT_EQUAL(2, p.dataOffset);
}
void test_obd_dtc_payload_parsing(void) {
    const uint8_t payload[] = {0x43, 0x01, 0x23, 0x00, 0x00, 0xA4, 0x56};
    uint16_t codes[2] = {};
    uint8_t count = 0;
    TEST_ASSERT_TRUE(ctParseObdDtcPayload(payload, sizeof(payload), codes, 2, count));
    TEST_ASSERT_EQUAL(2, count);
    TEST_ASSERT_EQUAL_HEX16(0x0123, codes[0]);
    TEST_ASSERT_EQUAL_HEX16(0xA456, codes[1]);

    const uint8_t malformed[] = {0x43, 0x01, 0x23, 0x45};
    TEST_ASSERT_FALSE(ctParseObdDtcPayload(malformed, sizeof(malformed), codes, 2, count));
    TEST_ASSERT_FALSE(ctParseObdDtcPayload(payload, sizeof(payload), codes, 0, count));
}
void test_isotp_flow_control_frame_validation(void) {
    uint8_t frame[8] = {};
    TEST_ASSERT_TRUE(ctBuildIsoTpFlowControl(frame, sizeof(frame), 0, 0, 0));
    TEST_ASSERT_EQUAL_HEX8(0x30, frame[0]);
    TEST_ASSERT_EQUAL(0, frame[1]);
    TEST_ASSERT_EQUAL(0, frame[2]);
    TEST_ASSERT_TRUE(ctBuildIsoTpFlowControl(frame, sizeof(frame), 1, 4, 0xF3));
    TEST_ASSERT_EQUAL_HEX8(0x31, frame[0]);
    TEST_ASSERT_EQUAL(4, frame[1]);
    TEST_ASSERT_EQUAL_HEX8(0xF3, frame[2]);
    TEST_ASSERT_FALSE(ctBuildIsoTpFlowControl(frame, 7, 0, 0, 0));
    TEST_ASSERT_FALSE(ctBuildIsoTpFlowControl(frame, sizeof(frame), 3, 0, 0));
    TEST_ASSERT_FALSE(ctBuildIsoTpFlowControl(frame, sizeof(frame), 0, 0, 0x80));
}
void test_isotp_multiframe_reassembly_and_sequence_validation(void) {
    const uint8_t first[] = {0x10, 0x07, 0x43, 0x01, 0x23, 0x04, 0x56, 0x07};
    const uint8_t last[] = {0x21, 0x89, 0, 0, 0, 0, 0, 0};
    uint8_t payload[8] = {};
    CtIsoTpReassembly state;
    TEST_ASSERT_TRUE(ctIsoTpBegin(first, sizeof(first), 0x7E8, false,
                                  payload, sizeof(payload), state));
    TEST_ASSERT_FALSE(ctIsoTpComplete(state));
    TEST_ASSERT_FALSE(ctIsoTpAppend(last, sizeof(last), 0x7E9, false,
                                   payload, state));
    const uint8_t wrongSequence[] = {0x22, 0x89, 0, 0, 0, 0, 0, 0};
    TEST_ASSERT_FALSE(ctIsoTpAppend(wrongSequence, sizeof(wrongSequence), 0x7E8,
                                   false, payload, state));
    TEST_ASSERT_TRUE(ctIsoTpAppend(last, sizeof(last), 0x7E8, false,
                                  payload, state));
    TEST_ASSERT_TRUE(ctIsoTpComplete(state));
    TEST_ASSERT_EQUAL(0x43, payload[0]);
    TEST_ASSERT_EQUAL(0x89, payload[6]);
    TEST_ASSERT_FALSE(ctIsoTpAppend(last, sizeof(last), 0x7E8, false,
                                   payload, state));
}

void test_isotp_rejects_invalid_first_frame(void) {
    const uint8_t singleFrame[] = {0x02, 0x43, 0, 0, 0, 0, 0, 0};
    const uint8_t oversized[] = {0x10, 0x20, 0x43, 0, 0, 0, 0, 0};
    uint8_t payload[8] = {};
    CtIsoTpReassembly state;
    TEST_ASSERT_FALSE(ctIsoTpBegin(singleFrame, sizeof(singleFrame), 0x7E8,
                                   false, payload, sizeof(payload), state));
    TEST_ASSERT_FALSE(ctIsoTpBegin(oversized, sizeof(oversized), 0x7E8,
                                   false, payload, sizeof(payload), state));
}

void test_dbc_intel_boundaries(void) {
    TEST_ASSERT_TRUE(ctDbcSignalFitsDlc(0, 1, false, 1));
    TEST_ASSERT_TRUE(ctDbcSignalFitsDlc(0, 8, false, 1));
    TEST_ASSERT_TRUE(ctDbcSignalFitsDlc(7, 2, false, 2));
    TEST_ASSERT_FALSE(ctDbcSignalFitsDlc(7, 2, false, 1));
    TEST_ASSERT_TRUE(ctDbcSignalFitsDlc(0, 64, false, 8));
}
void test_dbc_motorola_boundaries(void) {
    TEST_ASSERT_TRUE(ctDbcSignalFitsDlc(7, 8, true, 1));
    TEST_ASSERT_TRUE(ctDbcSignalFitsDlc(7, 16, true, 2));
    TEST_ASSERT_FALSE(ctDbcSignalFitsDlc(7, 16, true, 1));
    TEST_ASSERT_TRUE(ctDbcSignalFitsDlc(63, 2, true, 8));
    TEST_ASSERT_FALSE(ctDbcSignalFitsDlc(0, 64, true, 8));
}
void test_dbc_invalid_lengths_and_start(void) {
    TEST_ASSERT_FALSE(ctDbcSignalFitsDlc(0, 0, false, 8));
    TEST_ASSERT_FALSE(ctDbcSignalFitsDlc(0, 65, false, 8));
    TEST_ASSERT_FALSE(ctDbcSignalFitsDlc(64, 1, false, 8));
}

void test_verification_fingerprint_changes_with_command(void) {
    const uint8_t a[] = {0x01, 0x02};
    const uint8_t b[] = {0x01, 0x03};
    uint32_t fa = ctVerifyFingerprint(1, 1, "lock", 0x123, false, 2, a);
    TEST_ASSERT_NOT_EQUAL(0, fa);
    TEST_ASSERT_NOT_EQUAL(fa, ctVerifyFingerprint(1, 1, "lock", 0x123, false, 2, b));
    TEST_ASSERT_NOT_EQUAL(fa, ctVerifyFingerprint(2, 1, "lock", 0x123, false, 2, a));
    TEST_ASSERT_NOT_EQUAL(fa, ctVerifyFingerprint(1, 1, "unlock", 0x123, false, 2, a));
}



void test_dbc_extended_id_decode(void) {
    uint32_t id = 0;
    bool extended = false;
    TEST_ASSERT_TRUE(ctDecodeDbcCanId(0x123, id, extended));
    TEST_ASSERT_EQUAL_HEX32(0x123, id);
    TEST_ASSERT_FALSE(extended);

    TEST_ASSERT_TRUE(ctDecodeDbcCanId(0x80000123UL, id, extended));
    TEST_ASSERT_EQUAL_HEX32(0x123, id);
    TEST_ASSERT_TRUE(extended);

    TEST_ASSERT_TRUE(ctDecodeDbcCanId(0x80000000UL, id, extended));
    TEST_ASSERT_EQUAL_HEX32(0x00000000UL, id);
    TEST_ASSERT_TRUE(extended);

    // Bundled GM vendor/OpenDBC files contain raw 29-bit IDs without the
    // DBC bit-31 marker; these must still become Extended CAN frames.
    TEST_ASSERT_TRUE(ctDecodeDbcCanId(0x10630000UL, id, extended));
    TEST_ASSERT_EQUAL_HEX32(0x10630000UL, id);
    TEST_ASSERT_TRUE(extended);
}

void test_json_command_validation(void) {
    JsonDocument doc;
    JsonObject item = doc.to<JsonObject>();
    item["label"] = "lock_all";
    item["displayName"] = "Lock";
    item["canId"] = 0x123;
    item["extended"] = false;
    item["length"] = 2;
    item["data"].to<JsonArray>().add(1);
    item["data"].as<JsonArray>().add(2);
    item["source"] = 2;
    item["status"] = "verified";
    CtJsonCommandFields f;
    TEST_ASSERT_TRUE(ctValidateImportedCommand(item, f));
    TEST_ASSERT_EQUAL_HEX32(0x123, f.canId);
    TEST_ASSERT_EQUAL(2, f.length);
}
void test_json_command_rejects_invalid_types_and_ranges(void) {
    JsonDocument doc;
    JsonObject item = doc.to<JsonObject>();
    item["label"] = "x"; item["displayName"] = "x"; item["canId"] = 0x800;
    item["extended"] = false; item["length"] = 1; item["data"].to<JsonArray>().add(1);
    item["source"] = 2; item["status"] = "unverified";
    CtJsonCommandFields f;
    TEST_ASSERT_FALSE(ctValidateImportedCommand(item, f));
    item["canId"] = 0x123; item["extended"] = "false";
    TEST_ASSERT_FALSE(ctValidateImportedCommand(item, f));
    item["extended"] = false; item["source"] = 99;
    TEST_ASSERT_FALSE(ctValidateImportedCommand(item, f));
    item["source"] = 2; char longName[49]; memset(longName, 'x', 48); longName[48] = '\0'; item["displayName"] = longName;
    TEST_ASSERT_FALSE(ctValidateImportedCommand(item, f));
}

void test_origin_guard_blocks_cross_site_only(void) {
    TEST_ASSERT_TRUE(ctOriginAllowed(nullptr, "192.168.4.1"));                 // no Origin header
    TEST_ASSERT_TRUE(ctOriginAllowed("", "192.168.4.1"));
    TEST_ASSERT_TRUE(ctOriginAllowed("http://192.168.4.1", "192.168.4.1"));    // own page
    TEST_ASSERT_TRUE(ctOriginAllowed("http://192.168.4.1:8080", "192.168.4.1:8080"));
    TEST_ASSERT_TRUE(ctOriginAllowed("HTTP://CarTouch.local", "cartouch.local"));
    TEST_ASSERT_FALSE(ctOriginAllowed("http://evil.example", "192.168.4.1"));  // another site
    TEST_ASSERT_FALSE(ctOriginAllowed("https://192.168.4.1", "192.168.4.1"));  // other scheme
    TEST_ASSERT_FALSE(ctOriginAllowed("null", "192.168.4.1"));
    TEST_ASSERT_FALSE(ctOriginAllowed("http://192.168.4.1.evil.example", "192.168.4.1"));
    TEST_ASSERT_FALSE(ctOriginAllowed("http://192.168.4.1", nullptr));         // Origin without Host
}

void test_label_safe_rejects_html_and_quotes(void) {
    TEST_ASSERT_TRUE(ctLabelIsSafe("lock_all"));
    TEST_ASSERT_TRUE(ctLabelIsSafe("Window FL-up.2"));
    TEST_ASSERT_FALSE(ctLabelIsSafe(""));
    TEST_ASSERT_FALSE(ctLabelIsSafe(nullptr));
    TEST_ASSERT_FALSE(ctLabelIsSafe("x\" onmouseover=\"alert(1)"));
    TEST_ASSERT_FALSE(ctLabelIsSafe("<img src=x>"));
    TEST_ASSERT_FALSE(ctLabelIsSafe("a'b"));
    TEST_ASSERT_FALSE(ctLabelIsSafe("a&b"));
    TEST_ASSERT_FALSE(ctLabelIsSafe("a\nb"));
}

void test_json_command_rejects_unsafe_label(void) {
    JsonDocument doc;
    JsonObject item = doc.to<JsonObject>();
    item["label"] = "x\"><script>"; item["displayName"] = "Lock";
    item["canId"] = 0x123; item["extended"] = false; item["length"] = 1;
    item["data"].to<JsonArray>().add(1);
    item["source"] = 2; item["status"] = "unverified";
    CtJsonCommandFields f;
    TEST_ASSERT_FALSE(ctValidateImportedCommand(item, f));
    item["label"] = "lock_all";
    TEST_ASSERT_TRUE(ctValidateImportedCommand(item, f));
}

void test_verification_transaction_rejections(void) {
    const char* label = "lock";
    TEST_ASSERT_TRUE(ctVerifyTransactionValid(true, 1, label, 77, 1000, 1, label, 77, 1500, 120000));
    TEST_ASSERT_FALSE(ctVerifyTransactionValid(false, 1, label, 77, 1000, 1, label, 77, 1500, 120000));
    TEST_ASSERT_FALSE(ctVerifyTransactionValid(true, 1, label, 77, 1000, 2, label, 77, 1500, 120000));
    TEST_ASSERT_FALSE(ctVerifyTransactionValid(true, 1, label, 77, 1000, 1, "unlock", 77, 1500, 120000));
    TEST_ASSERT_FALSE(ctVerifyTransactionValid(true, 1, label, 77, 1000, 1, label, 78, 1500, 120000));
    TEST_ASSERT_FALSE(ctVerifyTransactionValid(true, 1, label, 77, 1000, 1, label, 77, 121001, 120000));
}
void test_vehicle_tx_guard_blocks_when_config_or_driver_listen_only() {
    // config says Listen-Only: blocked even if the driver reports Normal
    TEST_ASSERT_EQUAL(CT_TX_ERR_LISTEN_ONLY, ctVehicleTxGuard(true, true, false, 8));
    // driver really in Listen-Only: blocked even if config says Normal
    TEST_ASSERT_EQUAL(CT_TX_ERR_LISTEN_ONLY, ctVehicleTxGuard(false, true, true, 8));
    // inactive bus is reported as not initialised, never as success
    TEST_ASSERT_EQUAL(CT_TX_ERR_NOT_INITIALIZED, ctVehicleTxGuard(false, false, false, 8));
    TEST_ASSERT_EQUAL(CT_TX_ERR_LENGTH, ctVehicleTxGuard(false, true, false, 9));
    TEST_ASSERT_EQUAL(CT_TX_OK, ctVehicleTxGuard(false, true, false, 8));
}

void test_bus_route_selector_is_sanitised() {
    TEST_ASSERT_EQUAL(0, ctSanitizeBusIndex(0));
    TEST_ASSERT_EQUAL(1, ctSanitizeBusIndex(1));
    TEST_ASSERT_EQUAL(0, ctSanitizeBusIndex(2));
    TEST_ASSERT_EQUAL(0, ctSanitizeBusIndex(255));
}

void test_mcp2515_8mhz_timing_table_decodes_to_requested_bitrates() {
    // CNF1/CNF2/CNF3 for an 8 MHz crystal. Values are taken from the usual
    // autowp-mcp2515 table; scripts/check_mcp_timing.py compares them with the
    // header that is really installed by PlatformIO.
    struct Row { uint32_t bps; uint8_t c1, c2, c3; uint32_t minSp, maxSp; };
    const Row rows[] = {
        {1000000, 0x00, 0x80, 0x80, 700, 800},
        { 500000, 0x00, 0x90, 0x82, 600, 800},
        { 250000, 0x00, 0xB1, 0x85, 600, 800},
        { 125000, 0x01, 0xB1, 0x85, 600, 800},
        { 100000, 0x01, 0xB4, 0x86, 600, 800},
    };
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        uint32_t bps = 0, sp = 0; uint8_t sjw = 0;
        TEST_ASSERT_TRUE(ctMcp2515DecodeTiming(8000000UL, rows[i].c1, rows[i].c2,
                                               rows[i].c3, bps, sp, sjw));
        TEST_ASSERT_EQUAL(rows[i].bps, bps);
        TEST_ASSERT_TRUE(sp >= rows[i].minSp && sp <= rows[i].maxSp);
        TEST_ASSERT_TRUE(sjw >= 1 && sjw <= 4);
        TEST_ASSERT_TRUE(ctMcp2515BitrateValid(rows[i].bps));
    }
    TEST_ASSERT_FALSE(ctMcp2515BitrateValid(800000));   // TWAI-only rate
}

void test_can_link_state_requires_real_traffic() {
    const uint32_t T = 5000;
    // driver stopped / bus-off are never "connected"
    TEST_ASSERT_EQUAL(CT_LINK_DOWN,    ctCanLinkState(false, false, 1000, 2000, T));
    TEST_ASSERT_EQUAL(CT_LINK_BUS_OFF, ctCanLinkState(true,  true,  1000, 2000, T));
    TEST_ASSERT_EQUAL(CT_LINK_BUS_OFF, ctCanLinkState(false, true,  1000, 2000, T));
    // started but never received a frame: NOT connected
    TEST_ASSERT_EQUAL(CT_LINK_NO_TRAFFIC, ctCanLinkState(true, false, 0, 2000, T));
    // recent frame: connected; stale frame: back to no traffic
    TEST_ASSERT_EQUAL(CT_LINK_TRAFFIC,    ctCanLinkState(true, false, 1000, 6000, T));
    TEST_ASSERT_EQUAL(CT_LINK_NO_TRAFFIC, ctCanLinkState(true, false, 1000, 6001, T));
    // millis() wrap-around keeps working
    TEST_ASSERT_EQUAL(CT_LINK_TRAFFIC,    ctCanLinkState(true, false, 0xFFFFFF00u, 0x00000100u, T));
}

void test_bus_off_recovery_schedule() {
    // first attempt is immediate
    TEST_ASSERT_TRUE(ctRecoveryDue(false, 0, 100, 5000));
    // then at most one attempt per retry interval
    TEST_ASSERT_FALSE(ctRecoveryDue(true, 1000, 5999, 5000));
    TEST_ASSERT_TRUE(ctRecoveryDue(true, 1000, 6000, 5000));
    // wrap-safe
    TEST_ASSERT_FALSE(ctRecoveryDue(true, 0xFFFFFF00u, 0x00000100u, 5000));
    TEST_ASSERT_TRUE(ctRecoveryDue(true, 0xFFFFF000u, 0x00001000u, 5000));
}


static uint32_t s_fakeRndState = 12345u;
static uint32_t fakeRnd(void) { s_fakeRndState = s_fakeRndState * 1664525u + 1013904223u; return s_fakeRndState; }

void test_generated_password_shape_and_limits(void) {
    char buf[16];
    ctGeneratePassword(buf, sizeof(buf), fakeRnd);
    TEST_ASSERT_EQUAL_UINT32(CT_GEN_PASS_LEN, strlen(buf));
    for (size_t i = 0; i < strlen(buf); i++) {
        TEST_ASSERT_NULL(strchr("0O1lI", buf[i]));
    }
    char a[16], b[16];
    ctGeneratePassword(a, sizeof(a), fakeRnd);
    ctGeneratePassword(b, sizeof(b), fakeRnd);
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);
    char small[5];
    ctGeneratePassword(small, sizeof(small), fakeRnd);
    TEST_ASSERT_EQUAL_UINT32(4, strlen(small));
}

void test_ota_firmware_header_check(void) {
    const uint8_t good[] = {0xE9, 0x03};
    const uint8_t bad[]  = {0x00, 0x03};
    TEST_ASSERT_TRUE(ctOtaFirmwareHeaderOk(good, 2));
    TEST_ASSERT_FALSE(ctOtaFirmwareHeaderOk(bad, 2));
    TEST_ASSERT_FALSE(ctOtaFirmwareHeaderOk(good, 0));
    TEST_ASSERT_FALSE(ctOtaFirmwareHeaderOk(NULL, 2));
}


void test_storage_policy_auto_prefers_internal_then_sd(void) {
    CtStorageDecision d = ctResolveStorage(CT_STORE_AUTO, true, 500000, true, 9000000, 1000);
    TEST_ASSERT_EQUAL_INT(CT_LOC_INTERNAL, d.loc);
    d = ctResolveStorage(CT_STORE_AUTO, true, 1000, true, 9000000, 1000);   // internal nearly full
    TEST_ASSERT_EQUAL_INT(CT_LOC_SD, d.loc);
    TEST_ASSERT_FALSE(d.fellBack);
    d = ctResolveStorage(CT_STORE_AUTO, true, 1000, false, 0, 1000);        // no card: never drop silently
    TEST_ASSERT_EQUAL_INT(CT_LOC_NONE, d.loc);
}

void test_storage_policy_explicit_choice_falls_back_and_reports(void) {
    CtStorageDecision d = ctResolveStorage(CT_STORE_SD, true, 500000, false, 0, 1000);
    TEST_ASSERT_EQUAL_INT(CT_LOC_INTERNAL, d.loc);
    TEST_ASSERT_TRUE(d.fellBack);
    d = ctResolveStorage(CT_STORE_INTERNAL, false, 0, true, 9000000, 1000);
    TEST_ASSERT_EQUAL_INT(CT_LOC_SD, d.loc);
    TEST_ASSERT_TRUE(d.fellBack);
    d = ctResolveStorage(CT_STORE_SD, true, 500000, true, 9000000, 1000);
    TEST_ASSERT_EQUAL_INT(CT_LOC_SD, d.loc);
    TEST_ASSERT_FALSE(d.fellBack);
    d = ctResolveStorage(77, true, 500000, true, 9000000, 1000);            // invalid value behaves as AUTO
    TEST_ASSERT_EQUAL_INT(CT_LOC_INTERNAL, d.loc);
    TEST_ASSERT_FALSE(ctStorageChoiceValid(3));
}

void test_sd_cs_pin_validation(void) {
    const int used[] = { 10, 14, 15 };
    TEST_ASSERT_TRUE(ctSdCsPinAllowed(5, used, 3));
    TEST_ASSERT_FALSE(ctSdCsPinAllowed(-1, used, 3));
    TEST_ASSERT_FALSE(ctSdCsPinAllowed(0, used, 3));    // strapping
    TEST_ASSERT_FALSE(ctSdCsPinAllowed(19, used, 3));   // USB
    TEST_ASSERT_FALSE(ctSdCsPinAllowed(22, used, 3));   // nonexistent on ESP32-S3
    TEST_ASSERT_FALSE(ctSdCsPinAllowed(43, used, 3));   // UART0 / serial console
    TEST_ASSERT_FALSE(ctSdCsPinAllowed(44, used, 3));   // UART0 / serial console
    TEST_ASSERT_FALSE(ctSdCsPinAllowed(30, used, 3));   // flash/PSRAM
    TEST_ASSERT_FALSE(ctSdCsPinAllowed(14, used, 3));   // already used
    TEST_ASSERT_FALSE(ctSdCsPinAllowed(49, used, 3));
}


void test_buttons_adc_classification_and_invalid_range(void) {
    const uint16_t L[5] = {300, 700, 1100, 1500, 1900};
    TEST_ASSERT_EQUAL_INT(CT_KEY_NONE, ctAdcClassify(4095, L, 100));
    TEST_ASSERT_EQUAL_INT(CT_KEY_UP, ctAdcClassify(310, L, 100));
    TEST_ASSERT_EQUAL_INT(CT_KEY_OK, ctAdcClassify(1900, L, 100));
    TEST_ASSERT_EQUAL_INT(CT_KEY_INVALID, ctAdcClassify(2500, L, 100));
    TEST_ASSERT_EQUAL_INT(CT_KEY_INVALID, ctAdcClassify(5000, L, 100));
    const uint16_t overlap[5] = {300, 350, 1100, 1500, 1900};
    TEST_ASSERT_TRUE(ctLadderValid(L, 100));
    TEST_ASSERT_FALSE(ctLadderValid(overlap, 100));
    const int used[] = {10};
    TEST_ASSERT_TRUE(ctAdcPinAllowed(4, used, 1));
    TEST_ASSERT_FALSE(ctAdcPinAllowed(12, used, 1));   // ADC2 conflicts with Wi-Fi
}

void test_buttons_debounce_short_and_long_press(void) {
    CtKeyState s; uint32_t t = 0; CtKeyEvent e;
    e = ctKeyStep(s, CT_KEY_UP, t);                      TEST_ASSERT_EQUAL_INT(CT_EV_NONE, e.type);
    t += 10; e = ctKeyStep(s, CT_KEY_NONE, t);           // bounce
    t += 10; e = ctKeyStep(s, CT_KEY_UP, t);             TEST_ASSERT_EQUAL_INT(CT_EV_NONE, e.type);
    t += 40; e = ctKeyStep(s, CT_KEY_UP, t);             TEST_ASSERT_EQUAL_INT(CT_EV_PRESS, e.type);
    t += 50; e = ctKeyStep(s, CT_KEY_NONE, t);
    t += 40; e = ctKeyStep(s, CT_KEY_NONE, t);           TEST_ASSERT_EQUAL_INT(CT_EV_SHORT, e.type);
    t += 100; e = ctKeyStep(s, CT_KEY_OK, t);
    t += 40; e = ctKeyStep(s, CT_KEY_OK, t);             TEST_ASSERT_EQUAL_INT(CT_EV_PRESS, e.type);
    t += 900; e = ctKeyStep(s, CT_KEY_OK, t);            TEST_ASSERT_EQUAL_INT(CT_EV_LONG, e.type);
    t += 10; ctKeyStep(s, CT_KEY_NONE, t);
    t += 40; e = ctKeyStep(s, CT_KEY_NONE, t);           TEST_ASSERT_EQUAL_INT(CT_EV_NONE, e.type);  // no SHORT after LONG
}

void test_buttons_invalid_reading_creates_no_event(void) {
    CtKeyState s;
    CtKeyEvent e = ctKeyStep(s, CT_KEY_INVALID, 1000);
    TEST_ASSERT_EQUAL_INT(CT_EV_NONE, e.type);
    TEST_ASSERT_EQUAL_INT(CT_KEY_NONE, s.stable);
}

// ---- OTA image header (chip + flash size) ----
static void makeOtaHeader(uint8_t* h, uint8_t segments, uint8_t flashCode, uint16_t chip) {
    memset(h, 0, CT_OTA_HEADER_LEN);
    h[0] = 0xE9;
    h[1] = segments;
    h[3] = (uint8_t)((flashCode << 4) | 0x0F);    // size code in the high nibble
    h[12] = (uint8_t)(chip & 0xFF);
    h[13] = (uint8_t)(chip >> 8);
}

void test_ota_header_accepts_matching_image() {
    uint8_t h[CT_OTA_HEADER_LEN];
    makeOtaHeader(h, 5, 2, CT_OTA_CHIP_ESP32S3);    // built for 4 MB
    CtOtaHeaderCheck st;
    TEST_ASSERT_EQUAL(CT_OTA_HDR_OK, ctOtaHeaderFeed(st, h, sizeof(h), 4u * 1048576u));
    TEST_ASSERT_TRUE(st.done);
    CtOtaHeaderCheck st2;    // 4 MB image on a 16 MB board is allowed
    TEST_ASSERT_EQUAL(CT_OTA_HDR_OK, ctOtaHeaderFeed(st2, h, sizeof(h), 16u * 1048576u));
}

void test_ota_header_rejects_flash_chip_and_magic() {
    uint8_t h[CT_OTA_HEADER_LEN];
    CtOtaHeaderCheck st;
    makeOtaHeader(h, 5, 4, CT_OTA_CHIP_ESP32S3);    // built for 16 MB, device has 4 MB
    TEST_ASSERT_EQUAL(CT_OTA_HDR_FLASH_TOO_BIG, ctOtaHeaderFeed(st, h, sizeof(h), 4u * 1048576u));
    TEST_ASSERT_FALSE(st.done);

    CtOtaHeaderCheck st2;
    makeOtaHeader(h, 5, 2, 0x0000);    // original ESP32
    TEST_ASSERT_EQUAL(CT_OTA_HDR_WRONG_CHIP, ctOtaHeaderFeed(st2, h, sizeof(h), 4u * 1048576u));

    CtOtaHeaderCheck st3;
    makeOtaHeader(h, 0, 2, CT_OTA_CHIP_ESP32S3);
    TEST_ASSERT_EQUAL(CT_OTA_HDR_BAD_SEGMENTS, ctOtaHeaderFeed(st3, h, sizeof(h), 4u * 1048576u));
    makeOtaHeader(h, 17, 2, CT_OTA_CHIP_ESP32S3);
    CtOtaHeaderCheck st4;
    TEST_ASSERT_EQUAL(CT_OTA_HDR_BAD_SEGMENTS, ctOtaHeaderFeed(st4, h, sizeof(h), 4u * 1048576u));

    CtOtaHeaderCheck st5;    // unknown size code
    makeOtaHeader(h, 5, 9, CT_OTA_CHIP_ESP32S3);
    TEST_ASSERT_EQUAL(CT_OTA_HDR_FLASH_TOO_BIG, ctOtaHeaderFeed(st5, h, sizeof(h), 16u * 1048576u));

    CtOtaHeaderCheck st6;    // wrong first byte is refused at once, before 24 bytes arrive
    const uint8_t junk[2] = {0x00, 0x01};
    TEST_ASSERT_EQUAL(CT_OTA_HDR_BAD_MAGIC, ctOtaHeaderFeed(st6, junk, sizeof(junk), 4u * 1048576u));
}

void test_ota_header_collects_across_small_chunks() {
    uint8_t h[CT_OTA_HEADER_LEN];
    makeOtaHeader(h, 5, 2, CT_OTA_CHIP_ESP32S3);
    CtOtaHeaderCheck st;
    for (size_t i = 0; i + 1 < sizeof(h); i++) {    // one byte at a time
        TEST_ASSERT_EQUAL(CT_OTA_HDR_NEED_MORE, ctOtaHeaderFeed(st, h + i, 1, 4u * 1048576u));
    }
    TEST_ASSERT_FALSE(st.done);    // image that ends here is too short
    TEST_ASSERT_EQUAL(CT_OTA_HDR_OK, ctOtaHeaderFeed(st, h + sizeof(h) - 1, 1, 4u * 1048576u));
    TEST_ASSERT_TRUE(st.done);
    st.reset();
    TEST_ASSERT_FALSE(st.done);
    TEST_ASSERT_EQUAL(0, st.have);
    TEST_ASSERT_EQUAL(CT_OTA_HDR_NEED_MORE, ctOtaHeaderFeed(st, NULL, 0, 4u * 1048576u));
}

void test_obd_value_never_answered_is_not_valid(void) {
    TEST_ASSERT_FALSE(ctObdValueFresh(false, 0, 1000, CT_OBD_STALE_MS));
    TEST_ASSERT_FALSE(ctObdValueFresh(false, 999, 1000, CT_OBD_STALE_MS));
}

void test_obd_value_goes_stale_after_limit(void) {
    TEST_ASSERT_TRUE(ctObdValueFresh(true, 1000, 1000, CT_OBD_STALE_MS));
    TEST_ASSERT_TRUE(ctObdValueFresh(true, 1000, 1000 + CT_OBD_STALE_MS, CT_OBD_STALE_MS));
    TEST_ASSERT_FALSE(ctObdValueFresh(true, 1000, 1001 + CT_OBD_STALE_MS, CT_OBD_STALE_MS));
}

void test_obd_value_freshness_survives_millis_wraparound(void) {
    const uint32_t last = 0xFFFFFF00u;    // 256 ms before the counter wraps
    TEST_ASSERT_TRUE(ctObdValueFresh(true, last, 0x00000100u, CT_OBD_STALE_MS));
    TEST_ASSERT_FALSE(ctObdValueFresh(true, last, 0x00002000u, CT_OBD_STALE_MS));
}

void test_obd_pid_conversion_formulas(void) {
    TEST_ASSERT_EQUAL_UINT16(1726, ctObdEngineRpm(0x1A, 0xF8));
    TEST_ASSERT_EQUAL_UINT16(0, ctObdEngineRpm(0, 0));
    TEST_ASSERT_EQUAL_UINT8(0, ctObdVehicleSpeed(0));
    TEST_ASSERT_EQUAL_UINT8(255, ctObdVehicleSpeed(255));
    TEST_ASSERT_EQUAL_INT8(-40, ctObdCoolantTemp(0));
    TEST_ASSERT_EQUAL_INT8(0, ctObdCoolantTemp(40));
    TEST_ASSERT_EQUAL_INT8(60, ctObdCoolantTemp(100));
    TEST_ASSERT_EQUAL_UINT8(0, ctObdPercent(0));
    TEST_ASSERT_EQUAL_UINT8(50, ctObdPercent(128));
    TEST_ASSERT_EQUAL_UINT8(100, ctObdPercent(255));
    TEST_ASSERT_EQUAL_UINT16(0x1234, ctObdEngineRuntime(0x12, 0x34));
    TEST_ASSERT_EQUAL_FLOAT(5.0f, ctObdControlModuleVoltage(0x13, 0x88));
}

void test_obd_validity_mask_bits_are_independent(void) {
    const uint8_t mask = CT_VD_RPM | CT_VD_BATTERY;
    TEST_ASSERT_TRUE(ctVdValid(mask, CT_VD_RPM));
    TEST_ASSERT_TRUE(ctVdValid(mask, CT_VD_BATTERY));
    TEST_ASSERT_FALSE(ctVdValid(mask, CT_VD_SPEED));
    TEST_ASSERT_FALSE(ctVdValid(mask, CT_VD_COOLANT));
    TEST_ASSERT_FALSE(ctVdValid(mask, CT_VD_FUEL));
    TEST_ASSERT_FALSE(ctVdValid(0, CT_VD_RPM));
}

void test_learn_listen_only_override_is_never_persisted(void) {
    // Not learning: whatever RAM holds is saved.
    TEST_ASSERT_TRUE(ctPersistedListenOnly(true, false, false));
    TEST_ASSERT_FALSE(ctPersistedListenOnly(false, false, true));
    // Learning forced Listen-Only: the user's earlier choice is saved, not the forced true.
    TEST_ASSERT_FALSE(ctPersistedListenOnly(true, true, false));
    TEST_ASSERT_TRUE(ctPersistedListenOnly(true, true, true));
}

// ---- Simulated ECU (test/test_native/ecu_sim.h): OBD without a car ----

// PID 0x0C decoding uses the same production conversion helper as the reader.
static bool simReadRpm(SimEcuMode mode, float& rpm) {
    const uint8_t value[2] = { 0x1A, 0xF8 };    // 6904 / 4 = 1726 rpm
    SimFrame f;
    if (simEcuMode01(mode, 0x0C, value, 2, &f) == 0) return false;
    if (!ctIsObdReplyFrame(f.id, false, false)) return false;
    CtObdSingleFrame p;
    if (!ctParseObdSingleFrame(f.data, f.length, 0x41, 0x0C, p)) return false;
    if (p.dataOffset + 2 > f.length) return false;
    rpm = ctObdEngineRpm(f.data[p.dataOffset], f.data[p.dataOffset + 1]);
    return true;
}

void test_sim_ecu_normal_reply_decodes_rpm(void) {
    float rpm = 0;
    TEST_ASSERT_TRUE(simReadRpm(SIM_ECU_NORMAL, rpm));
    TEST_ASSERT_EQUAL_FLOAT(1726.0f, rpm);
}

void test_sim_ecu_bad_replies_never_produce_a_value(void) {
    float rpm = -1.0f;
    TEST_ASSERT_FALSE(simReadRpm(SIM_ECU_SILENT, rpm));
    TEST_ASSERT_FALSE(simReadRpm(SIM_ECU_WRONG_PID, rpm));
    TEST_ASSERT_FALSE(simReadRpm(SIM_ECU_TRUNCATED, rpm));
    TEST_ASSERT_FALSE(simReadRpm(SIM_ECU_NEGATIVE, rpm));
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, rpm);    // output untouched: no invented number
}

void test_sim_ecu_multiframe_dtc_reassembly(void) {
    const uint8_t dtcs[6] = { 0x01, 0x33, 0x01, 0x34, 0x02, 0x00 };    // 3rd is real
    SimFrame fr[4];
    const uint8_t n = simEcuMode03(dtcs, sizeof(dtcs), -1, fr, 4);
    TEST_ASSERT_EQUAL(2, n);

    uint8_t payload[64];
    CtIsoTpReassembly st;
    TEST_ASSERT_TRUE(ctIsoTpBegin(fr[0].data, fr[0].length, fr[0].id, false,
                                  payload, sizeof(payload), st));
    TEST_ASSERT_FALSE(ctIsoTpComplete(st));
    TEST_ASSERT_TRUE(ctIsoTpAppend(fr[1].data, fr[1].length, fr[1].id, false, payload, st));
    TEST_ASSERT_TRUE(ctIsoTpComplete(st));

    uint16_t codes[8];
    uint8_t count = 0;
    TEST_ASSERT_TRUE(ctParseObdDtcPayload(payload, st.totalLength, codes, 8, count));
    TEST_ASSERT_EQUAL(3, count);
    TEST_ASSERT_EQUAL_HEX16(0x0133, codes[0]);
    TEST_ASSERT_EQUAL_HEX16(0x0200, codes[2]);
}

void test_sim_ecu_lost_consecutive_frame_is_not_accepted(void) {
    const uint8_t dtcs[18] = { 1,1, 1,2, 1,3, 1,4, 1,5, 1,6, 1,7, 1,8, 1,9 };    // total 19: FF + 2 CF
    SimFrame fr[6];
    const uint8_t n = simEcuMode03(dtcs, sizeof(dtcs), 0, fr, 6);    // first CF lost
    TEST_ASSERT_EQUAL(2, n);    // FF + the surviving CF (sequence 2)

    uint8_t payload[64];
    CtIsoTpReassembly st;
    TEST_ASSERT_TRUE(ctIsoTpBegin(fr[0].data, fr[0].length, fr[0].id, false,
                                  payload, sizeof(payload), st));
    // The next frame that arrives has sequence 2, but 1 is expected.
    TEST_ASSERT_FALSE(ctIsoTpAppend(fr[1].data, fr[1].length, fr[1].id, false, payload, st));
    TEST_ASSERT_FALSE(ctIsoTpComplete(st));
    // A frame from another ECU id is rejected too.
    SimFrame other = fr[1];
    other.id = 0x7E9;
    other.data[0] = 0x21;
    TEST_ASSERT_FALSE(ctIsoTpAppend(other.data, other.length, other.id, false, payload, st));
}

// Mirrors the validity update in obd2_reader.cpp (OBD_POLL_DONE): only PIDs
// the ECU really answered count, and only for CT_OBD_STALE_MS afterwards.
void test_sim_ecu_poll_validity_with_partial_ecu(void) {
    static const uint8_t bits[7] = { CT_VD_RPM, CT_VD_SPEED, CT_VD_COOLANT,
                                     CT_VD_THROTTLE, CT_VD_FUEL, CT_VD_RUNTIME,
                                     CT_VD_BATTERY };
    static const uint8_t pids[7] = { 0x0C, 0x0D, 0x05, 0x11, 0x2F, 0x1F, 0x42 };
    bool answered[7] = {};
    uint32_t lastMs[7] = {};
    const uint8_t value[2] = { 0x10, 0x00 };

    uint32_t now = 1000;
    for (int round = 0; round < 3; ++round, now += 1500) {
        for (int i = 0; i < 7; ++i) {
            // This simulated car only supports RPM and speed.
            const SimEcuMode m = (i < 2) ? SIM_ECU_NORMAL : SIM_ECU_SILENT;
            SimFrame f;
            CtObdSingleFrame p;
            if (simEcuMode01(m, pids[i], value, 2, &f) &&
                ctParseObdSingleFrame(f.data, f.length, 0x41, pids[i], p)) {
                answered[i] = true;
                lastMs[i] = now;
            }
        }
    }
    uint8_t mask = 0;
    for (int i = 0; i < 7; ++i)
        if (ctObdValueFresh(answered[i], lastMs[i], now, CT_OBD_STALE_MS)) mask |= bits[i];
    TEST_ASSERT_EQUAL_HEX8(CT_VD_RPM | CT_VD_SPEED, mask);

    // Ignition off: nobody answers any more. After the stale limit, nothing is valid.
    now += CT_OBD_STALE_MS + 1;
    mask = 0;
    for (int i = 0; i < 7; ++i)
        if (ctObdValueFresh(answered[i], lastMs[i], now, CT_OBD_STALE_MS)) mask |= bits[i];
    TEST_ASSERT_EQUAL_HEX8(0, mask);
}

void test_login_lock_is_shared_and_expires(void) {
    CtLoginLock l;
    TEST_ASSERT_FALSE(ctLoginLocked(l, 1000));
    // 3 wrong guesses on one path and 2 on the other add up to the limit.
    ctLoginFailed(l, 1000); ctLoginFailed(l, 1001); ctLoginFailed(l, 1002);
    TEST_ASSERT_FALSE(ctLoginLocked(l, 1003));
    ctLoginFailed(l, 1004);
    ctLoginFailed(l, 1005);
    TEST_ASSERT_TRUE(ctLoginLocked(l, 1006));
    TEST_ASSERT_TRUE(ctLoginLocked(l, 1005 + CT_LOGIN_LOCK_MS - 1));
    TEST_ASSERT_FALSE(ctLoginLocked(l, 1005 + CT_LOGIN_LOCK_MS));    // expired and cleared
    TEST_ASSERT_EQUAL(0, l.fails);
    // A correct password resets the counter.
    ctLoginFailed(l, 5000); ctLoginFailed(l, 5001);
    ctLoginSucceeded(l);
    TEST_ASSERT_EQUAL(0, l.fails);
}

void test_login_lock_survives_millis_wraparound(void) {
    CtLoginLock l;
    const uint32_t t0 = 0xFFFFFF00u;    // lock end wraps past zero
    for (int i = 0; i < 5; ++i) ctLoginFailed(l, t0);
    TEST_ASSERT_TRUE(ctLoginLocked(l, t0 + 1000));
    TEST_ASSERT_FALSE(ctLoginLocked(l, t0 + CT_LOGIN_LOCK_MS + 1));
}

void test_dbc_name_rules(void) {
    TEST_ASSERT_TRUE(ctDbcNameValid("toyota_prius_2010_pt.dbc"));
    TEST_ASSERT_TRUE(ctDbcNameValid("My-Car.v2.DBC"));
    TEST_ASSERT_FALSE(ctDbcNameValid(nullptr));
    TEST_ASSERT_FALSE(ctDbcNameValid(".dbc"));
    TEST_ASSERT_FALSE(ctDbcNameValid(".hidden.dbc"));
    TEST_ASSERT_FALSE(ctDbcNameValid("a..b.dbc"));
    TEST_ASSERT_FALSE(ctDbcNameValid("../x.dbc"));
    TEST_ASSERT_FALSE(ctDbcNameValid("dir/x.dbc"));
    TEST_ASSERT_FALSE(ctDbcNameValid("dir\\x.dbc"));
    TEST_ASSERT_FALSE(ctDbcNameValid("car.txt"));
    TEST_ASSERT_FALSE(ctDbcNameValid("my car.dbc"));
    TEST_ASSERT_FALSE(ctDbcNameValid("a\"b.dbc"));
    TEST_ASSERT_TRUE(ctDbcNameValid("abcdefghijklmnopqrstuv.dbc"));   // 26 chars
    TEST_ASSERT_FALSE(ctDbcNameValid("abcdefghijklmnopqrstuvw.dbc")); // 27 chars
}

void test_dbc_path_fits_profile_field(void) {
    char p[32];
    TEST_ASSERT_TRUE(ctDbcBuildPath("abcdefghijklmnopqrstuv.dbc", p, sizeof(p)));
    TEST_ASSERT_EQUAL_STRING("/dbc/abcdefghijklmnopqrstuv.dbc", p);
    TEST_ASSERT_EQUAL_UINT32(31, strlen(p));
    char small[10];
    TEST_ASSERT_FALSE(ctDbcBuildPath("car.dbc", small, sizeof(small)));
    TEST_ASSERT_EQUAL_CHAR('\0', small[0]);
    TEST_ASSERT_FALSE(ctDbcBuildPath("../car.dbc", p, sizeof(p)));
    TEST_ASSERT_EQUAL_CHAR('\0', p[0]);
}

void test_dbc_size_check(void) {
    TEST_ASSERT_EQUAL(CT_DBC_EMPTY, ctDbcSizeCheck(0, 1000000, 0));
    TEST_ASSERT_EQUAL(CT_DBC_TOO_BIG, ctDbcSizeCheck(CT_DBC_MAX_BYTES + 1, 10000000, 0));
    TEST_ASSERT_EQUAL(CT_DBC_OK, ctDbcSizeCheck(CT_DBC_MAX_BYTES, 10000000, 0));
    TEST_ASSERT_EQUAL(CT_DBC_NO_SPACE, ctDbcSizeCheck(100000, 100000 + CT_DBC_RESERVE_BYTES - 1, 0));
    TEST_ASSERT_EQUAL(CT_DBC_OK, ctDbcSizeCheck(100000, 100000 + CT_DBC_RESERVE_BYTES, 0));
    // Replacing an existing file frees its own space.
    TEST_ASSERT_EQUAL(CT_DBC_NO_SPACE, ctDbcSizeCheck(100000, 50000, 0));
    TEST_ASSERT_EQUAL(CT_DBC_OK, ctDbcSizeCheck(100000, 50000, 90000));
}

void test_dbc_scan_counts_messages_and_cap(void) {
    CtDbcScan s; ctDbcScanInit(s);
    ctDbcScanLine(s, "VERSION \"\"");
    ctDbcScanLine(s, "BO_ 100 MSG_A: 8 ECU");
    ctDbcScanLine(s, " SG_ Speed : 0|16@1+ (1,0) [0|0] \"\" X");
    ctDbcScanLine(s, "\tBO_ 200 MSG_B: 8 ECU\r");
    ctDbcScanLine(s, "BO_TX_BU_ 100 : ECU;");   // not a message record
    TEST_ASSERT_EQUAL_UINT32(2, s.messages);
    TEST_ASSERT_EQUAL(CT_DBC_OK, ctDbcScanVerdict(s));

    CtDbcScan e; ctDbcScanInit(e);
    ctDbcScanLine(e, "VERSION \"\"");
    TEST_ASSERT_EQUAL(CT_DBC_NO_MESSAGES, ctDbcScanVerdict(e));

    CtDbcScan m; ctDbcScanInit(m);
    for (uint32_t i = 0; i < CT_DBC_MAX_MESSAGES; ++i) ctDbcScanLine(m, "BO_ 1 M: 8 E");
    TEST_ASSERT_EQUAL(CT_DBC_OK, ctDbcScanVerdict(m));
    ctDbcScanLine(m, "BO_ 2 M: 8 E");
    TEST_ASSERT_EQUAL(CT_DBC_TOO_MANY_MESSAGES, ctDbcScanVerdict(m));
}

void test_dbc_scan_rejects_binary(void) {
    CtDbcScan s; ctDbcScanInit(s);
    ctDbcScanLine(s, "BO_ 100 MSG_A: 8 ECU");
    ctDbcScanLine(s, "\x7f""ELF\x01\x02");
    TEST_ASSERT_EQUAL(CT_DBC_BAD_CONTENT, ctDbcScanVerdict(s));
    CtDbcScan t; ctDbcScanInit(t);
    ctDbcScanLine(t, "BO_ 1 M: 8 E\t\r");   // TAB and CR are allowed
    TEST_ASSERT_EQUAL(CT_DBC_OK, ctDbcScanVerdict(t));
}

void test_dbc_delete_decision(void) {
    TEST_ASSERT_EQUAL(CT_DBC_DEL_REFUSE_BUILTIN, ctDbcDeleteDecision(true, false, true));
    TEST_ASSERT_EQUAL(CT_DBC_DEL_NEEDS_CONFIRM, ctDbcDeleteDecision(false, true, false));
    TEST_ASSERT_EQUAL(CT_DBC_DEL_ALLOW, ctDbcDeleteDecision(false, true, true));
    TEST_ASSERT_EQUAL(CT_DBC_DEL_ALLOW, ctDbcDeleteDecision(false, false, false));
}

static void ctTestSha(const uint8_t* d, size_t n, size_t chunk, char* out) {
    CtSha256 c; ctSha256Init(c);
    for (size_t i = 0; i < n; i += chunk) ctSha256Update(c, d + i, (n - i < chunk) ? (n - i) : chunk);
    ctSha256FinishHex(c, out);
}

void test_sha256_known_vectors(void) {
    char h[65];
    ctTestSha((const uint8_t*)"", 0, 1, h);
    TEST_ASSERT_EQUAL_STRING("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", h);
    ctTestSha((const uint8_t*)"abc", 3, 3, h);
    TEST_ASSERT_EQUAL_STRING("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", h);
    const char* two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    ctTestSha((const uint8_t*)two, strlen(two), 64, h);
    TEST_ASSERT_EQUAL_STRING("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", h);
}

void test_sha256_chunking_does_not_change_result(void) {
    uint8_t data[1000];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)(i * 31u + 7u);
    char a[65], b[65], c[65];
    ctTestSha(data, sizeof(data), sizeof(data), a);
    ctTestSha(data, sizeof(data), 1, b);
    ctTestSha(data, sizeof(data), 63, c);
    TEST_ASSERT_EQUAL_STRING(a, b);
    TEST_ASSERT_EQUAL_STRING(a, c);
    TEST_ASSERT_EQUAL_STRING("5097e7d587352f5097062ae679f37bda5802d9f875aba14c8cb4d1a188ada179", a);
}

void test_sha256_boundary_lengths_and_hex_compare(void) {
    uint8_t data[130];
    memset(data, 'a', sizeof(data));
    char h55[65], h56[65], h64[65];
    ctTestSha(data, 55, 7, h55);
    ctTestSha(data, 56, 7, h56);
    ctTestSha(data, 64, 7, h64);
    TEST_ASSERT_EQUAL_STRING("9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318", h55);
    TEST_ASSERT_EQUAL_STRING("b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a", h56);
    TEST_ASSERT_EQUAL_STRING("ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb", h64);
    TEST_ASSERT_TRUE(ctSha256HexEqual(h55, h55));
    char upper[65]; strcpy(upper, h55); upper[0] = (char)(upper[0] >= 'a' ? upper[0] - 32 : upper[0]);
    TEST_ASSERT_TRUE(ctSha256HexEqual(h55, upper));
    TEST_ASSERT_FALSE(ctSha256HexEqual(h55, h56));
    TEST_ASSERT_FALSE(ctSha256HexEqual(h55, "abc"));
    TEST_ASSERT_FALSE(ctSha256HexEqual(nullptr, h55));
    TEST_ASSERT_TRUE(ctSha256HexValid(h55));
    TEST_ASSERT_FALSE(ctSha256HexValid("invalid"));
    TEST_ASSERT_FALSE(ctSha256HexValid("000000000000000000000000000000000000000000000000000000000000000g"));
}

void test_dbc_manifest_parses_real_builtin_line(void) {
    const char* line = "{\"name\":\"ESR.dbc\",\"size\":65956,\"sha256\":\"e02b01a53b444570e4a48b792a47ed4284b0a387b03769e9091a6304fbccf676\",\"messages\":80,\"tier\":\"specialized\",\"profiles\":0,\"in4mb\":false,\"source\":\"unverified\",\"license\":\"unverified\"},\n";
    CtDbcManifestEntry e;
    TEST_ASSERT_TRUE(ctDbcManifestParseLine(line, e));
    TEST_ASSERT_EQUAL_STRING("ESR.dbc", e.name);
    TEST_ASSERT_EQUAL_UINT32(65956, e.size);
    TEST_ASSERT_EQUAL_UINT32(80, e.messages);
    TEST_ASSERT_EQUAL_STRING("e02b01a53b444570e4a48b792a47ed4284b0a387b03769e9091a6304fbccf676", e.sha256);
    TEST_ASSERT_FALSE(ctDbcManifestParseLine(CT_DBC_MANIFEST_HEADER, e));
    TEST_ASSERT_FALSE(ctDbcManifestParseLine(CT_DBC_MANIFEST_FOOTER, e));
}

void test_dbc_manifest_roundtrip(void) {
    CtDbcManifestEntry a; memset(&a, 0, sizeof(a));
    strcpy(a.name, "my_car.dbc"); a.size = 12345; a.messages = 42;
    strcpy(a.sha256, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    char line[200];
    size_t n = ctDbcManifestFormatLine(a, line, sizeof(line));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_UINT32(strlen(line), n);
    CtDbcManifestEntry b;
    TEST_ASSERT_TRUE(ctDbcManifestParseLine(line, b));
    TEST_ASSERT_EQUAL_STRING(a.name, b.name);
    TEST_ASSERT_EQUAL_UINT32(a.size, b.size);
    TEST_ASSERT_EQUAL_UINT32(a.messages, b.messages);
    TEST_ASSERT_EQUAL_STRING(a.sha256, b.sha256);
    TEST_ASSERT_EQUAL_STRING("unknown", b.time);
    TEST_ASSERT_EQUAL_STRING("user", b.source);
    TEST_ASSERT_EQUAL_STRING("unverified", b.license);
    char tiny[20];
    TEST_ASSERT_EQUAL_UINT32(0, ctDbcManifestFormatLine(a, tiny, sizeof(tiny)));
    TEST_ASSERT_EQUAL_CHAR('\0', tiny[0]);
}

void test_dbc_manifest_rejects_bad_lines(void) {
    const char* h = "\"sha256\":\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"";
    char l[300]; CtDbcManifestEntry e;
    snprintf(l, sizeof(l), "{\"name\":\"../x.dbc\",\"size\":5,%s,\"messages\":1}", h);
    TEST_ASSERT_FALSE(ctDbcManifestParseLine(l, e));
    snprintf(l, sizeof(l), "{\"name\":\"a\\\"b.dbc\",\"size\":5,%s,\"messages\":1}", h);
    TEST_ASSERT_FALSE(ctDbcManifestParseLine(l, e));
    snprintf(l, sizeof(l), "{\"name\":\"x.dbc\",\"size\":0,%s,\"messages\":1}", h);
    TEST_ASSERT_FALSE(ctDbcManifestParseLine(l, e));
    snprintf(l, sizeof(l), "{\"name\":\"x.dbc\",\"size\":5,\"sha256\":\"zz7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\",\"messages\":1}");
    TEST_ASSERT_FALSE(ctDbcManifestParseLine(l, e));
    snprintf(l, sizeof(l), "{\"name\":\"x.dbc\",\"size\":5,\"sha256\":\"abc\",\"messages\":1}");
    TEST_ASSERT_FALSE(ctDbcManifestParseLine(l, e));
    snprintf(l, sizeof(l), "{\"name\":\"x.dbc\",\"size\":99999999999,%s,\"messages\":1}", h);
    TEST_ASSERT_FALSE(ctDbcManifestParseLine(l, e));
    snprintf(l, sizeof(l), "{\"name\":\"x.dbc\",\"size\":5,%s}", h);          // no messages
    TEST_ASSERT_FALSE(ctDbcManifestParseLine(l, e));
    snprintf(l, sizeof(l), "{\"name\":\"x.dbc\",\"size\":5,%s,\"messages\":1", h);   // truncated, no closing brace
    TEST_ASSERT_FALSE(ctDbcManifestParseLine(l, e));
    snprintf(l, sizeof(l), "{\"name\":\"x.dbc");                                // unterminated string
    TEST_ASSERT_FALSE(ctDbcManifestParseLine(l, e));
    TEST_ASSERT_FALSE(ctDbcManifestParseLine(nullptr, e));
}

// ---- SD and Buttons GPIO conflict detection ----
void test_sync_policy_sd_cs_vs_button_gpio_no_conflict(void) {
    const int btnPins[5] = {1, 2, 3, 4, 5};
    TEST_ASSERT_TRUE(ctSync().validateSdCsVsButtons(10, btnPins));
    TEST_ASSERT_TRUE(ctSync().validateSdCsVsButtons(-1, btnPins));
    TEST_ASSERT_TRUE(ctSync().validateSdCsVsButtons(0, nullptr));
}

void test_sync_policy_sd_cs_conflicts_with_button_gpio(void) {
    const int btnPins[5] = {1, 2, 3, 4, 5};
    TEST_ASSERT_FALSE(ctSync().validateSdCsVsButtons(1, btnPins));
    TEST_ASSERT_FALSE(ctSync().validateSdCsVsButtons(3, btnPins));
    TEST_ASSERT_FALSE(ctSync().validateSdCsVsButtons(5, btnPins));
}

void test_sync_policy_buttons_gpio_vs_sd_cs_no_conflict(void) {
    const int btnPins[5] = {1, 2, 3, 4, 5};
    TEST_ASSERT_TRUE(ctSync().validateButtonsVsGivenSdCs(btnPins, 10));
    TEST_ASSERT_TRUE(ctSync().validateButtonsVsGivenSdCs(btnPins, -1));
    TEST_ASSERT_TRUE(ctSync().validateButtonsVsGivenSdCs(nullptr, 5));
}

void test_sync_policy_buttons_gpio_conflicts_with_sd_cs(void) {
    const int btnPins[5] = {1, 2, 3, 4, 5};
    TEST_ASSERT_FALSE(ctSync().validateButtonsVsGivenSdCs(btnPins, 1));
    TEST_ASSERT_FALSE(ctSync().validateButtonsVsGivenSdCs(btnPins, 4));
    TEST_ASSERT_FALSE(ctSync().validateButtonsVsGivenSdCs(btnPins, 5));
}



void test_host_guard_allows_only_device_ips(void) {
    // AP only
    TEST_ASSERT_TRUE(ctHostAllowed("192.168.4.1", "192.168.4.1", nullptr, "CarTouch"));
    TEST_ASSERT_TRUE(ctHostAllowed("192.168.4.1:80", "192.168.4.1", "", "CarTouch"));
    TEST_ASSERT_TRUE(ctHostAllowed("192.168.4.1:8080", "192.168.4.1", "0.0.0.0", "CarTouch"));
    // STA address
    TEST_ASSERT_TRUE(ctHostAllowed("10.0.0.23", "192.168.4.1", "10.0.0.23", "CarTouch"));
    TEST_ASSERT_TRUE(ctHostAllowed("10.0.0.23:80", "192.168.4.1", "10.0.0.23", "CarTouch"));
    TEST_ASSERT_TRUE(ctHostAllowed("cartouch", "192.168.4.1", nullptr, "CarTouch"));
    TEST_ASSERT_TRUE(ctHostAllowed("CarTouch:80", "192.168.4.1", nullptr, "CarTouch"));
    // header absent (non-browser HTTP/1.0 tool)
    TEST_ASSERT_FALSE(ctHostAllowed(nullptr, "192.168.4.1", nullptr, "CarTouch"));
    TEST_ASSERT_FALSE(ctHostAllowed("", "192.168.4.1", nullptr, "CarTouch"));
    // DNS rebinding: attacker name resolved to the device IP
    TEST_ASSERT_FALSE(ctHostAllowed("evil.example", "192.168.4.1", nullptr, "CarTouch"));
    TEST_ASSERT_FALSE(ctHostAllowed("evil.example:80", "192.168.4.1", nullptr, "CarTouch"));
    TEST_ASSERT_FALSE(ctHostAllowed("192.168.4.1.evil.example", "192.168.4.1", nullptr, "CarTouch"));
    TEST_ASSERT_FALSE(ctHostAllowed("192.168.4.10", "192.168.4.1", nullptr, "CarTouch"));
    TEST_ASSERT_FALSE(ctHostAllowed("localhost", "192.168.4.1", nullptr, "CarTouch"));
    // malformed port / empty host part
    TEST_ASSERT_FALSE(ctHostAllowed("192.168.4.1:", "192.168.4.1", nullptr, "CarTouch"));
    TEST_ASSERT_FALSE(ctHostAllowed("192.168.4.1:80a", "192.168.4.1", nullptr, "CarTouch"));
    TEST_ASSERT_FALSE(ctHostAllowed(":80", "192.168.4.1", nullptr, "CarTouch"));
    // an unconnected STA ("0.0.0.0" / empty) never matches
    TEST_ASSERT_FALSE(ctHostAllowed("0.0.0.0", "192.168.4.1", "0.0.0.0", "CarTouch"));
}

void test_command_gate_applies_to_every_source(void) {
    // Active, not Listen-Only: actuators allowed
    TEST_ASSERT_EQUAL(CT_CMD_ALLOW, ctCommandGate(true, false, "lock"));
    // Listen-Only: actuators and unknown/learn-style names rejected, config allowed
    TEST_ASSERT_EQUAL(CT_CMD_REJECT_LISTEN_ONLY, ctCommandGate(true, true, "lock"));
    TEST_ASSERT_EQUAL(CT_CMD_REJECT_LISTEN_ONLY, ctCommandGate(true, true, "trunk"));
    TEST_ASSERT_EQUAL(CT_CMD_REJECT_LISTEN_ONLY, ctCommandGate(true, true, "learn_verify_send"));
    TEST_ASSERT_EQUAL(CT_CMD_REJECT_LISTEN_ONLY, ctCommandGate(true, true, "not_a_command"));
    TEST_ASSERT_EQUAL(CT_CMD_ALLOW, ctCommandGate(true, true, "listen_only"));
    TEST_ASSERT_EQUAL(CT_CMD_ALLOW, ctCommandGate(true, true, "vehicle_select_dbc:x.dbc"));
    TEST_ASSERT_EQUAL(CT_CMD_ALLOW, ctCommandGate(true, true, "toggle_theme"));
    // Asleep: actuators rejected, config allowed
    TEST_ASSERT_EQUAL(CT_CMD_REJECT_ASLEEP, ctCommandGate(false, false, "unlock"));
    TEST_ASSERT_EQUAL(CT_CMD_REJECT_ASLEEP, ctCommandGate(false, true, "unlock"));
    TEST_ASSERT_EQUAL(CT_CMD_ALLOW, ctCommandGate(false, false, "vehicle_select_custom:p1"));
    // Prefix lookalikes are not "safe"
    TEST_ASSERT_EQUAL(CT_CMD_REJECT_LISTEN_ONLY, ctCommandGate(true, true, "vehicle_selectX"));
    TEST_ASSERT_EQUAL(CT_CMD_REJECT_LISTEN_ONLY, ctCommandGate(true, true, "listen_only2"));
    // Empty / NULL
    TEST_ASSERT_EQUAL(CT_CMD_REJECT_INVALID, ctCommandGate(true, false, ""));
    TEST_ASSERT_EQUAL(CT_CMD_REJECT_INVALID, ctCommandGate(true, false, nullptr));
}


void test_command_actuator_metadata_is_explicit(void) {
    TEST_ASSERT_EQUAL(COMMAND_ACTUATOR_WINDOW, ctSuggestedActuatorClassForStandardLabel("window_fl_up"));
    TEST_ASSERT_EQUAL(COMMAND_ACTUATOR_SUNROOF, ctSuggestedActuatorClassForStandardLabel("sunroof_open"));
    TEST_ASSERT_EQUAL(COMMAND_ACTUATOR_MIRROR, ctSuggestedActuatorClassForStandardLabel("mirror_fold"));
    TEST_ASSERT_EQUAL(COMMAND_ACTUATOR_NONE, ctSuggestedActuatorClassForStandardLabel("lock_all"));
    TEST_ASSERT_EQUAL(COMMAND_ACTUATOR_UNKNOWN, ctSuggestedActuatorClassForStandardLabel("my_custom_button"));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_tx_guard_listen_only);
    RUN_TEST(test_tx_guard_initialization_and_dlc);
    RUN_TEST(test_tx_id_validity);
    RUN_TEST(test_battery_voltage_availability);
    RUN_TEST(test_can_frame_identity_includes_format);
    RUN_TEST(test_can_bus_pin_conflicts_are_rejected);
    RUN_TEST(test_mcp2515_supported_bitrates);
    RUN_TEST(test_partition_layout_must_fit_detected_flash);
    RUN_TEST(test_filesystem_ota_requires_mounted_empty_user_storage);
    RUN_TEST(test_http_body_limits_bound_forms_and_stream_imports);
    RUN_TEST(test_can_service_initializes_each_bus_independently);
    RUN_TEST(test_can_service_starts_with_can1_only);
    RUN_TEST(test_can_service_reports_unavailable_when_both_fail);
    RUN_TEST(test_can_service_routes_legacy_calls_to_can1);
    RUN_TEST(test_can_service_does_not_fallback_from_selected_inactive_bus);
    RUN_TEST(test_can_service_fans_out_frames_to_independent_subscribers);
    RUN_TEST(test_can_service_pumps_both_buses_independently);
    RUN_TEST(test_can_service_fans_out_to_monitor_and_recorder);
    RUN_TEST(test_can_record_format_preserves_channel_and_payload);
    RUN_TEST(test_can_record_format_rejects_invalid_or_truncated_frames);
    RUN_TEST(test_can_record_filename_allowlist);
    RUN_TEST(test_can_service_bounds_consumer_queues_and_counts_drops);
    RUN_TEST(test_bounded_index_parser_rejects_wraparound);
    RUN_TEST(test_strict_decimal_and_boolean_parsing);
    RUN_TEST(test_wrap_safe_timer);
    RUN_TEST(test_hex_standard_and_extended);
    RUN_TEST(test_hex_rejects_bad_input);
    RUN_TEST(test_obd_valid_pid_response_uses_pci_length);
    RUN_TEST(test_obd_reply_requires_standard_data_frame);
    RUN_TEST(test_obd_rejects_inconsistent_dlc_and_pci);
    RUN_TEST(test_obd_rejects_wrong_service_or_pid);
    RUN_TEST(test_obd_clear_dtc_requires_positive_ecu_ack);
    RUN_TEST(test_dtc_response_and_padding);
    RUN_TEST(test_obd_dtc_payload_parsing);
    RUN_TEST(test_isotp_flow_control_frame_validation);
    RUN_TEST(test_isotp_multiframe_reassembly_and_sequence_validation);
    RUN_TEST(test_isotp_rejects_invalid_first_frame);
    RUN_TEST(test_dbc_intel_boundaries);
    RUN_TEST(test_dbc_motorola_boundaries);
    RUN_TEST(test_dbc_invalid_lengths_and_start);
    RUN_TEST(test_verification_fingerprint_changes_with_command);
    RUN_TEST(test_verification_transaction_rejections);
    RUN_TEST(test_dbc_extended_id_decode);
    RUN_TEST(test_json_command_validation);
    RUN_TEST(test_json_command_rejects_invalid_types_and_ranges);
    RUN_TEST(test_json_command_rejects_unsafe_label);
    RUN_TEST(test_label_safe_rejects_html_and_quotes);
    RUN_TEST(test_origin_guard_blocks_cross_site_only);
    RUN_TEST(test_vehicle_tx_guard_blocks_when_config_or_driver_listen_only);
    RUN_TEST(test_bus_route_selector_is_sanitised);
    RUN_TEST(test_mcp2515_8mhz_timing_table_decodes_to_requested_bitrates);
    RUN_TEST(test_can_link_state_requires_real_traffic);
    RUN_TEST(test_bus_off_recovery_schedule);
    RUN_TEST(test_generated_password_shape_and_limits);
    RUN_TEST(test_ota_firmware_header_check);
    RUN_TEST(test_storage_policy_auto_prefers_internal_then_sd);
    RUN_TEST(test_storage_policy_explicit_choice_falls_back_and_reports);
    RUN_TEST(test_sd_cs_pin_validation);
    RUN_TEST(test_buttons_adc_classification_and_invalid_range);
    RUN_TEST(test_buttons_debounce_short_and_long_press);
    RUN_TEST(test_buttons_invalid_reading_creates_no_event);
    RUN_TEST(test_ota_header_accepts_matching_image);
    RUN_TEST(test_ota_header_rejects_flash_chip_and_magic);
    RUN_TEST(test_ota_header_collects_across_small_chunks);
    RUN_TEST(test_obd_value_never_answered_is_not_valid);
    RUN_TEST(test_obd_value_goes_stale_after_limit);
    RUN_TEST(test_obd_value_freshness_survives_millis_wraparound);
    RUN_TEST(test_obd_pid_conversion_formulas);
    RUN_TEST(test_obd_validity_mask_bits_are_independent);
    RUN_TEST(test_learn_listen_only_override_is_never_persisted);
    RUN_TEST(test_sim_ecu_normal_reply_decodes_rpm);
    RUN_TEST(test_sim_ecu_bad_replies_never_produce_a_value);
    RUN_TEST(test_sim_ecu_multiframe_dtc_reassembly);
    RUN_TEST(test_sim_ecu_lost_consecutive_frame_is_not_accepted);
    RUN_TEST(test_sim_ecu_poll_validity_with_partial_ecu);
    RUN_TEST(test_login_lock_is_shared_and_expires);
    RUN_TEST(test_login_lock_survives_millis_wraparound);
    RUN_TEST(test_dbc_name_rules);
    RUN_TEST(test_dbc_path_fits_profile_field);
    RUN_TEST(test_dbc_size_check);
    RUN_TEST(test_dbc_scan_counts_messages_and_cap);
    RUN_TEST(test_dbc_scan_rejects_binary);
    RUN_TEST(test_dbc_delete_decision);
    RUN_TEST(test_sha256_known_vectors);
    RUN_TEST(test_sha256_chunking_does_not_change_result);
    RUN_TEST(test_sha256_boundary_lengths_and_hex_compare);
    RUN_TEST(test_dbc_manifest_parses_real_builtin_line);
    RUN_TEST(test_dbc_manifest_roundtrip);
    RUN_TEST(test_dbc_manifest_rejects_bad_lines);
    RUN_TEST(test_sync_policy_sd_cs_vs_button_gpio_no_conflict);
    RUN_TEST(test_sync_policy_sd_cs_conflicts_with_button_gpio);
    RUN_TEST(test_sync_policy_buttons_gpio_vs_sd_cs_no_conflict);
    RUN_TEST(test_sync_policy_buttons_gpio_conflicts_with_sd_cs);
    RUN_TEST(test_host_guard_allows_only_device_ips);
    RUN_TEST(test_command_gate_applies_to_every_source);
    RUN_TEST(test_command_actuator_metadata_is_explicit);
    return UNITY_END();
}

