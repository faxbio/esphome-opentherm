/* OpenTherm.cpp - OpenTherm Communication Library For Arduino, ESP8266
Copyright 2018, Ihor Melnyk */

#include "opentherm.h"

namespace esphome {
namespace opentherm {

OpenTherm::OpenTherm(GPIOPin *pin_in, GPIOPin *pin_out, bool slave):
  pin_in_(pin_in),
  pin_out_(pin_out),
  isSlave(slave),
  store_(slave)
{
}

OpenTherm::~OpenTherm() {
  this->pin_in_->detach_interrupt();
}

void OpenTherm::begin(std::function<void(uint32_t, OpenThermResponseStatus)> callback)
{
  this->pin_in_->setup();
  this->store_.pin_in = this->pin_in_->to_isr();

  this->pin_out_->setup();

  this->pin_in_->attach_interrupt(OpenThermStore::gpio_intr, &this->store_, CHANGE);
 
  activateBoiler();
  this->store_.status = OpenThermStatus::READY;
  this->process_response_callback = callback;
}

bool OpenTherm::isReady()
{
  return this->store_.status == OpenThermStatus::READY;
}

void OpenTherm::setActiveState() {
  this->pin_out_->digital_write(LOW);
}

void OpenTherm::setIdleState() {
  this->pin_out_->digital_write(HIGH);
}

void OpenTherm::activateBoiler() {
  setIdleState();
  delay(1000);
}

void OpenTherm::sendBit(bool high) {
  if (high) setActiveState(); else setIdleState();
  delayMicroseconds(500);
  if (high) setIdleState(); else setActiveState();
  delayMicroseconds(500);
}

bool OpenTherm::sendRequestAync(uint32_t request)
{
  noInterrupts();
  const bool ready = isReady();
  interrupts();

  if (!ready)
    return false;

  this->store_.status = OpenThermStatus::REQUEST_SENDING;
  this->store_.response = 0;
  responseStatus = OpenThermResponseStatus::NONE;

  sendBit(HIGH); //start bit
  for (int i = 31; i >= 0; i--) {
    sendBit(bitRead(request, i));
  }
  sendBit(HIGH); //stop bit
  setIdleState();

  this->store_.status = OpenThermStatus::RESPONSE_WAITING;
  this->store_.responseTimestamp = micros();
  return true;
}

uint32_t OpenTherm::sendRequest(uint32_t request)
{
  if (!sendRequestAync(request)) return 0;
  while (!isReady()) {
    loop();
    yield();
  }
  return this->store_.response;
}

bool OpenTherm::sendResponse(uint32_t request)
{
  this->store_.status = OpenThermStatus::REQUEST_SENDING;
  this->store_.response = 0;
  responseStatus = OpenThermResponseStatus::NONE;

  sendBit(HIGH); //start bit
  for (int i = 31; i >= 0; i--) {
    sendBit(bitRead(request, i));
  }
  sendBit(HIGH); //stop bit
  setIdleState();
  this->store_.status = OpenThermStatus::READY;
  return true;
}

OpenThermResponseStatus OpenTherm::getLastResponseStatus()
{
  return responseStatus;
}

void OpenTherm::loop()
{
  noInterrupts();
  OpenThermStatus st = this->store_.status;
  uint32_t ts = this->store_.responseTimestamp;
  interrupts();

  if (st == OpenThermStatus::READY) return;
  uint32_t newTs = micros();
  if (st != OpenThermStatus::NOT_INITIALIZED && (newTs - ts) > 1000000) {
    this->store_.status = OpenThermStatus::READY;
    responseStatus = OpenThermResponseStatus::TIMEOUT;
    if (process_response_callback) {
      process_response_callback(this->store_.response, responseStatus);
    }
  }
  else if (st == OpenThermStatus::RESPONSE_INVALID) {
    this->store_.status = OpenThermStatus::DELAY;
    responseStatus = OpenThermResponseStatus::INVALID;
    if (process_response_callback) {
      process_response_callback(this->store_.response, responseStatus);
    }
  }
  else if (st == OpenThermStatus::RESPONSE_READY) {
    this->store_.status = OpenThermStatus::DELAY;
    responseStatus = (isSlave ? isValidResponse(this->store_.response) : isValidRequest(this->store_.response)) ? OpenThermResponseStatus::SUCCESS : OpenThermResponseStatus::INVALID;
    if (process_response_callback) {
      process_response_callback(this->store_.response, responseStatus);
    }
  }
  else if (st == OpenThermStatus::DELAY) {
    if ((newTs - ts) > 100000) {
      this->store_.status = OpenThermStatus::READY;
    }
  }
}

//basic requests

uint32_t OpenTherm::setBoilerStatus(bool enableCentralHeating, bool enableHotWater, bool enableCooling, bool enableOutsideTemperatureCompensation, bool enableCentralHeating2) {
  return sendRequest(buildSetBoilerStatusRequest(enableCentralHeating, enableHotWater, enableCooling, enableOutsideTemperatureCompensation, enableCentralHeating2));
}

bool OpenTherm::setBoilerTemperature(float temperature) {
  uint32_t response = sendRequest(buildSetBoilerTemperatureRequest(temperature));
  return isValidResponse(response);
}

float OpenTherm::getBoilerTemperature() {
  uint32_t response = sendRequest(buildGetBoilerTemperatureRequest());
  return isValidResponse(response) ? getFloat(response) : 0;
}

float OpenTherm::getReturnTemperature() {
  uint32_t response = sendRequest(buildRequest(OpenThermRequestType::READ, OpenThermMessageID::MSG_TRET, 0));
  return isValidResponse(response) ? getFloat(response) : 0;
}

float OpenTherm::getModulation() {
  uint32_t response = sendRequest(buildRequest(OpenThermRequestType::READ, OpenThermMessageID::MSG_REL_MOD_LEVEL, 0));
  return isValidResponse(response) ? getFloat(response) : 0;
}

float OpenTherm::getPressure() {
  uint32_t response = sendRequest(buildRequest(OpenThermRequestType::READ, OpenThermMessageID::MSG_CH_PRESSURE, 0));
  return isValidResponse(response) ? getFloat(response) : 0;
}

uint8_t OpenTherm::getFault() {
  return ((sendRequest(buildRequest(OpenThermRequestType::READ, OpenThermMessageID::MSG_ASF_FLAGS_OEM_FAULT_CODE, 0)) >> 8) & 0xff);
}

void ICACHE_RAM_ATTR OpenThermStore::gpio_intr(OpenThermStore *arg)
{
  if (arg->status == OpenThermStatus::READY)
  {
    if (!arg->isSlave && arg->pin_in->digital_read() == HIGH) {
       arg->status = OpenThermStatus::RESPONSE_WAITING;
    }
    else {
      return;
    }
  }

  uint32_t newTs = micros();
  if (arg->status == OpenThermStatus::RESPONSE_WAITING) {
    if (arg->pin_in->digital_read() == HIGH) {
      arg->status = OpenThermStatus::RESPONSE_START_BIT;
      arg->responseTimestamp = newTs;
    }
    else {
      arg->status = OpenThermStatus::RESPONSE_INVALID;
      arg->responseTimestamp = newTs;
    }
  }
  else if (arg->status == OpenThermStatus::RESPONSE_START_BIT) {
    if ((newTs - arg->responseTimestamp < 750) && arg->pin_in->digital_read() == LOW) {
      arg->status = OpenThermStatus::RESPONSE_RECEIVING;
      arg->responseTimestamp = newTs;
      arg->responseBitIndex = 0;
    }
    else {
      arg->status = OpenThermStatus::RESPONSE_INVALID;
      arg->responseTimestamp = newTs;
    }
  }
  else if (arg->status == OpenThermStatus::RESPONSE_RECEIVING) {
    if ((newTs - arg->responseTimestamp) > 750) {
      if (arg->responseBitIndex < 32) {
        arg->response = (arg->response << 1) | !arg->pin_in->digital_read();
        arg->responseTimestamp = newTs;
        arg->responseBitIndex++;
      }
      else { //stop bit
        arg->status = OpenThermStatus::RESPONSE_READY;
        arg->responseTimestamp = newTs;
      }
    }
  }
}

bool parity(uint32_t frame) //odd parity
{
  uint8_t p = 0;
  while (frame > 0)
  {
    if (frame & 1) p++;
    frame = frame >> 1;
  }
  return (p & 1);
}

OpenThermMessageType getMessageType(uint32_t message)
{
  OpenThermMessageType msg_type = static_cast<OpenThermMessageType>((message >> 28) & 7);
  return msg_type;
}

OpenThermMessageID getDataID(uint32_t frame)
{
  return (OpenThermMessageID)((frame >> 16) & 0xFF);
}

uint32_t buildRequest(OpenThermMessageType type, OpenThermMessageID id, uint16_t data)
{
  uint32_t request = data;
  if (type == OpenThermMessageType::WRITE_DATA) {
    request |= 1ul << 28;
  }
  request |= ((uint32_t)id) << 16;
  if (parity(request)) request |= (1ul << 31);
  return request;
}

uint32_t buildResponse(OpenThermMessageType type, OpenThermMessageID id, uint16_t data)
{
  uint32_t response = data;
  response |= type << 28;
  response |= ((uint32_t)id) << 16;
  if (parity(response)) response |= (1ul << 31);
  return response;
}

bool isValidResponse(uint32_t response)
{
  if (parity(response)) return false;
  uint8_t msgType = (response << 1) >> 29;
  return msgType == READ_ACK || msgType == WRITE_ACK;
}

bool isValidRequest(uint32_t request)
{
  if (parity(request)) return false;
  uint8_t msgType = (request << 1) >> 29;
  return msgType == READ_DATA || msgType == WRITE_DATA;
}

const char *statusToString(OpenThermResponseStatus status)
{
  switch (status) {
    case NONE:  return "NONE";
    case SUCCESS: return "SUCCESS";
    case INVALID: return "INVALID";
    case TIMEOUT: return "TIMEOUT";
    default:    return "UNKNOWN";
  }
}

const char *messageTypeToString(OpenThermMessageType message_type)
{
  switch (message_type) {
    case READ_DATA:    return "READ_DATA";
    case WRITE_DATA:    return "WRITE_DATA";
    case INVALID_DATA:  return "INVALID_DATA";
    case RESERVED:    return "RESERVED";
    case READ_ACK:    return "READ_ACK";
    case WRITE_ACK:    return "WRITE_ACK";
    case DATA_INVALID:  return "DATA_INVALID";
    case UNKNOWN_DATA_ID: return "UNKNOWN_DATA_ID";
    default:        return "UNKNOWN";
  }
}

//building requests

uint32_t buildSetBoilerStatusRequest(bool enableCentralHeating, bool enableHotWater, bool enableCooling, bool enableOutsideTemperatureCompensation, bool enableCentralHeating2) {
  uint16_t data = enableCentralHeating | (enableHotWater << 1) | (enableCooling << 2) | (enableOutsideTemperatureCompensation << 3) | (enableCentralHeating2 << 4);
  data <<= 8;
  return buildRequest(OpenThermMessageType::READ_DATA, OpenThermMessageID::MSG_STATUS, data);
}

uint32_t buildSetBoilerTemperatureRequest(float temperature) {
  uint16_t data = temperatureToData(temperature);
  return buildRequest(OpenThermMessageType::WRITE_DATA, OpenThermMessageID::MSG_TSET, data);
}

uint32_t buildGetBoilerTemperatureRequest() {
  return buildRequest(OpenThermMessageType::READ_DATA, OpenThermMessageID::MSG_TBOILER, 0);
}

//parsing responses
bool isFault(uint32_t response) {
  return response & 0x1;
}

bool isCentralHeatingActive(uint32_t response) {
  return response & 0x2;
}

bool isHotWaterActive(uint32_t response) {
  return response & 0x4;
}

bool isFlameOn(uint32_t response) {
  return response & 0x8;
}

bool isCoolingActive(uint32_t response) {
  return response & 0x10;
}

bool isDiagnostic(uint32_t response) {
  return response & 0x40;
}

uint8_t getUBUInt8(const uint32_t response) {
  return (response >> 8) & 0xff;
}

uint8_t getLBUInt8(const uint32_t response) {
  return response & 0xff;
}

uint16_t getUInt16(const uint32_t response) {
  return response & 0xffff;
}

int16_t getInt16(const uint32_t response) {
  return (int16_t) (response & 0xffff);
}

float getFloat(const uint32_t response) {
  const uint16_t u88 = getUInt16(response);
  const float f = (u88 & 0x8000) ? -(0x10000L - u88) / 256.0f : u88 / 256.0f;
  return f;
}

uint16_t temperatureToData(float temperature) {
  if (temperature < 0) temperature = 0;
  if (temperature > 100) temperature = 100;
  uint16_t data = (uint16_t)(temperature * 256);
  return data;
}

}  // namespace opentherm
}  // namespace esphome