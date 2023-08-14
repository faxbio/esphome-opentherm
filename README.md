# OpenTherm
Port of OpenTherm protocol to ESPHome.io firmware. This version is based on the ESPhome version of https://github.com/wichers/esphome-opentherm

## Hardware:
The following hardware is used to test and deploy this version:
https://diyless.com/product/esp8266-opentherm-gateway

## Software:
Publishes multiple sensors to Home Assistant, and a service to override the ch-setpoint in case the boiler misses MSGid9 in the opentherm implementation, and services to enable/disable DHW preheat, also known as comfort/eco mode.

### CH override service:
To use the service, send a value > 0 to the gateway using the "ch_temperature_setpoint_override"-service. It will auto enable the CH function and set the target temperature to what you've sent. setting it to 0 disables the CH function.

**Example:**
```
service: esphome.openthermgateway_ch_temperature_setpoint_override
data:
  setpoint: 32
```

### DHW Preheat (eco/comfort mode):
To enable or disable the eco/comfort mode (DHW preheat), use the "dhw_preheat_enable"-service and the "dhw_preheat_disable"-service.

**Example:**
```
service: esphome.openthermgateway_dhw_preheat_enable
data: {}
```
  
## Notes:
The ESP32 version of the YAML compiles, but is untested on real hardware.
