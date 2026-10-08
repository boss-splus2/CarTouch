#include "can_service.h"

#ifdef ARDUINO
#define CT_RX_LOCK() portENTER_CRITICAL(&_rxMux)
#define CT_RX_UNLOCK() portEXIT_CRITICAL(&_rxMux)
#else
#define CT_RX_LOCK() do {} while (0)
#define CT_RX_UNLOCK() do {} while (0)
#endif

CANService::CANService(CanInterface& can1, CanInterface& can2)
    : _can1(can1), _can2(can2) {}

CanInterface* CANService::_get(CanBusId bus) {
    if (bus == CAN_BUS_1) return &_can1;
    if (bus == CAN_BUS_2) return &_can2;
    return nullptr;
}

const CanInterface* CANService::_get(CanBusId bus) const {
    if (bus == CAN_BUS_1) return &_can1;
    if (bus == CAN_BUS_2) return &_can2;
    return nullptr;
}

bool CANService::begin() {
    const bool can1Ready = begin(CAN_BUS_1);
    const bool can2Ready = begin(CAN_BUS_2);
    return can1Ready || can2Ready;
}

bool CANService::begin(CanBusId bus) {
    CanInterface* interface = _get(bus);
    return interface && interface->begin();
}

void CANService::end() {
    end(CAN_BUS_1);
    end(CAN_BUS_2);
}

void CANService::end(CanBusId bus) {
    CanInterface* interface = _get(bus);
    if (interface) interface->end();
}

bool CANService::sendMessage(const CanMessage& msg, uint32_t timeout) {
    return _can1.sendMessage(msg, timeout);
}

bool CANService::sendMessage(CanBusId bus, const CanMessage& msg, uint32_t timeout) {
    CanInterface* interface = _get(bus);
    return interface && interface->sendMessage(msg, timeout);
}

bool CANService::receiveMessage(CanMessage& msg, uint32_t timeout) {
    return _can1.receiveMessage(msg, timeout);
}

bool CANService::receiveMessage(CanBusId bus, CanMessage& msg, uint32_t timeout) {
    CanInterface* interface = _get(bus);
    return interface && interface->receiveMessage(msg, timeout);
}

bool CANService::receiveMessageNonBlocking(CanMessage& msg) {
    return _can1.receiveMessageNonBlocking(msg);
}

bool CANService::receiveMessageNonBlocking(CanBusId bus, CanMessage& msg) {
    CanInterface* interface = _get(bus);
    return interface && interface->receiveMessageNonBlocking(msg);
}

bool CANService::_validRxTarget(CanBusId bus, CanRxConsumer consumer) const {
    return (bus == CAN_BUS_1 || bus == CAN_BUS_2) &&
           consumer < CAN_RX_CONSUMER_COUNT;
}

bool CANService::subscribeRx(CanBusId bus, CanRxConsumer consumer) {
    if (!_validRxTarget(bus, consumer)) return false;
    CT_RX_LOCK();
    RxQueue& queue = _rxQueues[(uint8_t)bus][consumer];
    queue.head = 0;
    queue.count = 0;
    queue.drops = 0;
    queue.subscribed = true;
    CT_RX_UNLOCK();
    return true;
}

void CANService::unsubscribeRx(CanBusId bus, CanRxConsumer consumer) {
    if (!_validRxTarget(bus, consumer)) return;
    CT_RX_LOCK();
    RxQueue& queue = _rxQueues[(uint8_t)bus][consumer];
    queue.head = 0;
    queue.count = 0;
    queue.subscribed = false;
    CT_RX_UNLOCK();
}

uint8_t CANService::pumpRx(uint8_t maxFramesPerBus) {
    uint8_t totalRead = 0;
    for (uint8_t busIndex = 0; busIndex < 2; ++busIndex) {
        const CanBusId bus = (CanBusId)busIndex;
        CanInterface* interface = _get(bus);
        if (!interface || !interface->isActive()) continue;

        for (uint8_t n = 0; n < maxFramesPerBus; ++n) {
            CanMessage message = {};
            if (!interface->receiveMessageNonBlocking(message)) break;

            CanRxFrame frame = {};
            frame.bus = bus;
            frame.message = message;
            frame.receivedAtMs = interface->getLastRxTime();

            CT_RX_LOCK();
            for (uint8_t consumer = 0; consumer < CAN_RX_CONSUMER_COUNT; ++consumer) {
                RxQueue& queue = _rxQueues[busIndex][consumer];
                if (!queue.subscribed) continue;
                if (queue.count >= RX_QUEUE_DEPTH) {
                    ++queue.drops;
                    continue;
                }
                const uint8_t tail =
                    (uint8_t)((queue.head + queue.count) % RX_QUEUE_DEPTH);
                queue.frames[tail] = frame;
                ++queue.count;
            }
            CT_RX_UNLOCK();
            ++totalRead;
        }
    }
    return totalRead;
}

bool CANService::receiveRx(CanBusId bus, CanRxConsumer consumer,
                           CanRxFrame& frame) {
    if (!_validRxTarget(bus, consumer)) return false;
    CT_RX_LOCK();
    RxQueue& queue = _rxQueues[(uint8_t)bus][consumer];
    if (!queue.subscribed || queue.count == 0) {
        CT_RX_UNLOCK();
        return false;
    }
    frame = queue.frames[queue.head];
    queue.head = (uint8_t)((queue.head + 1) % RX_QUEUE_DEPTH);
    --queue.count;
    CT_RX_UNLOCK();
    return true;
}

void CANService::flushRx(CanBusId bus, CanRxConsumer consumer) {
    if (!_validRxTarget(bus, consumer)) return;
    CT_RX_LOCK();
    RxQueue& queue = _rxQueues[(uint8_t)bus][consumer];
    queue.head = 0;
    queue.count = 0;
    CT_RX_UNLOCK();
}

uint32_t CANService::getRxDrops(CanBusId bus, CanRxConsumer consumer) {
    if (!_validRxTarget(bus, consumer)) return 0;
    CT_RX_LOCK();
    const uint32_t drops = _rxQueues[(uint8_t)bus][consumer].drops;
    CT_RX_UNLOCK();
    return drops;
}

void CANService::flushRxQueue() {
    _can1.flushRxQueue();
}

void CANService::flushRxQueue(CanBusId bus) {
    CanInterface* interface = _get(bus);
    if (interface) interface->flushRxQueue();
}

bool CANService::isActive() {
    return _can1.isActive();
}

bool CANService::isActive(CanBusId bus) {
    CanInterface* interface = _get(bus);
    return interface && interface->isActive();
}

CanError CANService::getLastError() {
    return _can1.getLastError();
}

CanError CANService::getLastError(CanBusId bus) {
    CanInterface* interface = _get(bus);
    return interface ? interface->getLastError() : CAN_ERROR_INIT;
}

bool CANService::recoverFromBusOff() {
    return _can1.recoverFromBusOff();
}

bool CANService::recoverFromBusOff(CanBusId bus) {
    CanInterface* interface = _get(bus);
    return interface && interface->recoverFromBusOff();
}

void CANService::getStats(uint32_t& txCount, uint32_t& rxCount,
                          uint32_t& errorCount) {
    _can1.getStats(txCount, rxCount, errorCount);
}

void CANService::getStats(CanBusId bus, uint32_t& txCount, uint32_t& rxCount,
                          uint32_t& errorCount) {
    CanInterface* interface = _get(bus);
    if (interface) interface->getStats(txCount, rxCount, errorCount);
    else txCount = rxCount = errorCount = 0;
}

bool CANService::getDiagnostics(CanDiagnostics& out) {
    return _can1.getDiagnostics(out);
}

bool CANService::getDiagnostics(CanBusId bus, CanDiagnostics& out) {
    CanInterface* interface = _get(bus);
    return interface && interface->getDiagnostics(out);
}

uint32_t CANService::getLastRxTime() const {
    return _can1.getLastRxTime();
}

uint32_t CANService::getLastRxTime(CanBusId bus) const {
    const CanInterface* interface = _get(bus);
    return interface ? interface->getLastRxTime() : 0;
}

bool CANService::reconfigureMode(bool listenOnly) {
    return _can1.reconfigureMode(listenOnly);
}

bool CANService::reconfigureMode(CanBusId bus, bool listenOnly) {
    CanInterface* interface = _get(bus);
    return interface && interface->reconfigureMode(listenOnly);
}

bool CANService::isListenOnlyActive() {
    return _can1.isListenOnlyActive();
}

bool CANService::isListenOnlyActive(CanBusId bus) {
    CanInterface* interface = _get(bus);
    return interface && interface->isListenOnlyActive();
}