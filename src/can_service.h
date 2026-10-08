#ifndef CAN_SERVICE_H
#define CAN_SERVICE_H

#include "can_interface.h"

#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#endif

enum CanBusId : uint8_t {
    CAN_BUS_1 = 0,
    CAN_BUS_2 = 1,
    // Kept for source compatibility; unnumbered legacy paths use CAN1/TWAI.
    CAN_BUS_0 = CAN_BUS_1
};

enum CanRxConsumer : uint8_t {
    CAN_RX_OBD = 0,
    CAN_RX_LEARN,
    CAN_RX_MONITOR,
    CAN_RX_WAKE,
    CAN_RX_RECORDER,
    CAN_RX_CONSUMER_COUNT
};

struct CanRxFrame {
    CanBusId bus;
    CanMessage message;
    uint32_t receivedAtMs;
};

class CANService : public CanInterface {
public:
    static const uint8_t RX_QUEUE_DEPTH = 32;

    CANService(CanInterface& can1, CanInterface& can2);

    bool begin() override;
    bool begin(CanBusId bus);
    void end() override;
    void end(CanBusId bus);
    bool sendMessage(const CanMessage& msg,
                     uint32_t timeout = CAN_INTERFACE_TIMEOUT_MS) override;
    bool sendMessage(CanBusId bus, const CanMessage& msg,
                     uint32_t timeout = CAN_INTERFACE_TIMEOUT_MS);
    bool receiveMessage(CanMessage& msg,
                        uint32_t timeout = CAN_INTERFACE_TIMEOUT_MS) override;
    bool receiveMessage(CanBusId bus, CanMessage& msg,
                        uint32_t timeout = CAN_INTERFACE_TIMEOUT_MS);
    bool receiveMessageNonBlocking(CanMessage& msg) override;
    bool receiveMessageNonBlocking(CanBusId bus, CanMessage& msg);
    bool subscribeRx(CanBusId bus, CanRxConsumer consumer);
    void unsubscribeRx(CanBusId bus, CanRxConsumer consumer);
    uint8_t pumpRx(uint8_t maxFramesPerBus = 16);
    bool receiveRx(CanBusId bus, CanRxConsumer consumer, CanRxFrame& frame);
    void flushRx(CanBusId bus, CanRxConsumer consumer);
    uint32_t getRxDrops(CanBusId bus, CanRxConsumer consumer);
    void flushRxQueue() override;
    void flushRxQueue(CanBusId bus);
    bool isActive() override;
    bool isActive(CanBusId bus);
    CanError getLastError() override;
    CanError getLastError(CanBusId bus);
    bool recoverFromBusOff() override;
    bool recoverFromBusOff(CanBusId bus);
    void getStats(uint32_t& txCount, uint32_t& rxCount, uint32_t& errorCount) override;
    void getStats(CanBusId bus, uint32_t& txCount, uint32_t& rxCount,
                  uint32_t& errorCount);
    bool getDiagnostics(CanDiagnostics& out) override;
    bool getDiagnostics(CanBusId bus, CanDiagnostics& out);
    uint32_t getLastRxTime() const override;
    uint32_t getLastRxTime(CanBusId bus) const;
    bool reconfigureMode(bool listenOnly) override;
    bool reconfigureMode(CanBusId bus, bool listenOnly);
    bool isListenOnlyActive() override;
    bool isListenOnlyActive(CanBusId bus);

private:
    struct RxQueue {
        CanRxFrame frames[RX_QUEUE_DEPTH];
        uint8_t head;
        uint8_t count;
        bool subscribed;
        uint32_t drops;
    };

    CanInterface& _can1;
    CanInterface& _can2;
    RxQueue _rxQueues[2][CAN_RX_CONSUMER_COUNT] = {};
#ifdef ARDUINO
    mutable portMUX_TYPE _rxMux = portMUX_INITIALIZER_UNLOCKED;
#endif
    CanInterface* _get(CanBusId bus);
    const CanInterface* _get(CanBusId bus) const;
    bool _validRxTarget(CanBusId bus, CanRxConsumer consumer) const;
};

#endif