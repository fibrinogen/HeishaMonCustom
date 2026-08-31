#include "commands.h"
#include "decode.h"
#include <LittleFS.h>

//removed checksum from default query, is calculated in send_command
byte initialQuery[] = {0x31, 0x05, 0x10, 0x01, 0x00, 0x00, 0x00};
byte panasonicQuery[] = {0x71, 0x6c, 0x01, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
byte optionalPCBQuery[] = {0xF1, 0x11, 0x01, 0x50, 0x00, 0x00, 0x40, 0xFF, 0xFF, 0xE5, 0xFF, 0xFF, 0x00, 0xFF, 0xEB, 0xFF, 0xFF, 0x00, 0x00};
byte panasonicSendQuery[] PROGMEM = {0xf1, 0x6c, 0x01, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

#ifdef ESP32
extern QueueHandle_t pcbQueue;
#endif

const char* mqtt_topic_values PROGMEM = "main";
const char* mqtt_topic_xvalues PROGMEM = "extra";
const char* mqtt_topic_commands PROGMEM = "commands";
const char* mqtt_topic_pcbvalues PROGMEM = "optional";
const char* mqtt_topic_1wire PROGMEM = "1wire";
const char* mqtt_topic_s0 PROGMEM = "s0";
const char* mqtt_logtopic PROGMEM = "log";

const char* mqtt_willtopic PROGMEM = "LWT";
const char* mqtt_iptopic PROGMEM = "ip";

const char* mqtt_send_raw_value_topic PROGMEM = "SendRawValue";

static unsigned int temp2hex(float temp) {
  int hextemp = 0;
  if (temp > 120) {
    hextemp = 0;
  } else if (temp < -78) {
    hextemp = 255;
  } else {
    byte Uref = 255;
    int constant = 3695;
    int R25 = 6340;
    byte T25 = 25;
    int Rf = 6480;
    float K = 273.15;
    float RT = R25 * exp(constant * (1 / (temp + K) - 1 / (T25 + K)));
    hextemp = Uref * (RT / (Rf + RT));
  }
  return hextemp;
}


unsigned int set_heatpump_state(char *msg, unsigned char *cmd, char *log_msg) {
  byte heatpump_state = 1;
  String set_heatpump_state_string(msg);

  if ( set_heatpump_state_string.toInt() == 1 ) {
    heatpump_state = 2;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set heatpump state to %d"), heatpump_state - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[4] = heatpump_state;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_pump(char *msg, unsigned char *cmd, char *log_msg) {

  String set_pump_string(msg);

  byte pump_state = 16;
  if ( set_pump_string.toInt() == 1 ) {
    pump_state = 32;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set pump state to %d"), (pump_state / 16) - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[4] = pump_state;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_max_pump_duty(char *msg, unsigned char *cmd, char *log_msg) {

  String set_pumpduty_string(msg);

  byte pumpduty = set_pumpduty_string.toInt() + 1;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set max pump duty to %d"), pumpduty - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[45] = pumpduty;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_quiet_mode(char *msg, unsigned char *cmd, char *log_msg) {

  String set_quiet_mode_string(msg);

  byte quiet_mode = (set_quiet_mode_string.toInt() + 1) * 8;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set Quiet mode to %d"), quiet_mode / 8 - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[7] = quiet_mode;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_z1_heat_request_temperature(char *msg, unsigned char *cmd, char *log_msg) {

  String set_temperature_string(msg);

  byte request_temp = set_temperature_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set z1 heat request temperature to %d"), request_temp - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[38] = request_temp;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_z1_cool_request_temperature(char *msg, unsigned char *cmd, char *log_msg) {

  String set_temperature_string(msg);

  byte request_temp = set_temperature_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set z1 cool request temperature to %d"), request_temp - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[39] = request_temp;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_z2_heat_request_temperature(char *msg, unsigned char *cmd, char *log_msg) {

  String set_temperature_string(msg);

  byte request_temp = set_temperature_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set z2 heat request temperature to %d"), request_temp - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[40] = request_temp;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_z2_cool_request_temperature(char *msg, unsigned char *cmd, char *log_msg) {

  String set_temperature_string(msg);

  byte request_temp = set_temperature_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set z2 cool request temperature to %d"), request_temp - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[41] = request_temp;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_bivalent_start_temp(char *msg, unsigned char *cmd, char *log_msg) {

  String set_temperature_string(msg);

  byte request_temp = set_temperature_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set bivalent start temperature to %d"), request_temp - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[65] = request_temp;
  }

  return sizeof(panasonicSendQuery);
}
unsigned int set_bivalent_ap_start_temp(char *msg, unsigned char *cmd, char *log_msg) {

  String set_temperature_string(msg);

  byte request_temp = set_temperature_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set bivalent ap start temperature to %d"), request_temp - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[66] = request_temp;
  }

  return sizeof(panasonicSendQuery);
}
unsigned int set_bivalent_ap_stop_temp(char *msg, unsigned char *cmd, char *log_msg) {

  String set_temperature_string(msg);

  byte request_temp = set_temperature_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set bivalent stop ap temperature to %d"), request_temp - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[68] = request_temp;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_force_DHW(char *msg, unsigned char *cmd, char *log_msg) {

  String set_force_DHW_string(msg);

  byte force_DHW_mode = 64; //hex 0x40
  if ( set_force_DHW_string.toInt() == 1 ) {
    force_DHW_mode = 128; //hex 0x80
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set force DHW mode to %d"), (force_DHW_mode / 64) - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[4] = force_DHW_mode;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_force_defrost(char *msg, unsigned char *cmd, char *log_msg) {

  String set_force_defrost_string(msg);

  byte force_defrost_mode = 0;
  if ( set_force_defrost_string.toInt() == 1 ) {
    force_defrost_mode = 2; //hex 0x02
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set force defrost mode to %d"), force_defrost_mode / 2);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[8] = force_defrost_mode;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_force_sterilization(char *msg, unsigned char *cmd, char *log_msg) {

  String set_force_sterilization_string(msg);

  byte force_sterilization_mode = 0;
  if ( set_force_sterilization_string.toInt() == 1 ) {
    force_sterilization_mode = 4; //hex 0x04
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set force sterilization mode to %d"), force_sterilization_mode / 4);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[8] = force_sterilization_mode;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_force_heater(char *msg, unsigned char *cmd, char *log_msg) {

  String set_force_heater_string(msg);

  byte force_heater_mode = 4; //hex 0x04
  if ( set_force_heater_string.toInt() == 1 ) {
    force_heater_mode = 8; //hex 0x08
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set force heater mode to %d"), (force_heater_mode / 4) - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[5] = force_heater_mode;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_holiday_mode(char *msg, unsigned char *cmd, char *log_msg) {

  String set_holiday_string(msg);

  byte set_holiday = 16; //hex 0x10
  if ( set_holiday_string.toInt() == 1 ) {
    set_holiday = 32; //hex 0x20
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set holiday mode to %d"), (set_holiday / 16) - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[5] = set_holiday;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_powerful_mode(char *msg, unsigned char *cmd, char *log_msg) {

  String set_powerful_string(msg);

  byte set_powerful = (set_powerful_string.toInt() + 1) & 0b111;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set powerful mode to %d"), set_powerful - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[7] = set_powerful;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_DHW_temp(char *msg, unsigned char *cmd, char *log_msg) {

  String set_DHW_temp_string(msg);

  byte set_DHW_temp = set_DHW_temp_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set DHW temperature to %d"), set_DHW_temp - 128);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[42] = set_DHW_temp;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_operation_mode(char *msg, unsigned char *cmd, char *log_msg) {

  String set_mode_string(msg);

  byte set_mode;
  switch (set_mode_string.toInt()) {
    case 0: set_mode = 18; break;
    case 1: set_mode = 19; break;
    case 2: set_mode = 24; break;
    case 3: set_mode = 33; break;
    case 4: set_mode = 34; break;
    case 5: set_mode = 35; break;
    case 6: set_mode = 40; break;
    default: set_mode = 0; break;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set heat pump mode to %d"), set_mode);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[6] = set_mode;
  }

  return sizeof(panasonicSendQuery);
}


unsigned int set_bivalent_control(char *msg, unsigned char *cmd, char *log_msg) {

  byte set_bcontrol = 1;
  String set_bivalent_control_string(msg);

  if ( set_bivalent_control_string.toInt() == 1 ) {
    set_bcontrol = 2;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set bivalent control to %d"), set_bcontrol - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[26] = set_bcontrol;
  }

  return sizeof(panasonicSendQuery);
}


unsigned int set_bivalent_mode(char *msg, unsigned char *cmd, char *log_msg) {

  byte set_bmode = 4; // alternative mode
  String set_bivalent_mode_string(msg);

  if ( set_bivalent_mode_string.toInt() == 1 ) { //parallel mode
    set_bmode = 8;
  }
  if ( set_bivalent_mode_string.toInt() == 2 ) { //advanced parallel mode
    set_bmode = 12;
  }
  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set bivalent mode to %d"), (set_bmode / 4) - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[26] = set_bmode;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_curves(char *msg, unsigned char *cmd, char *log_msg) {
  memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));

  JsonDocument jsonDoc;
  DeserializationError error = deserializeJson(jsonDoc, msg);
  if (!error) {
    char tmpmsg[256] = { 0 };
    JsonVariant jsonValue;
    snprintf(tmpmsg, 255, "SetCurves JSON received ok");
    memcpy(log_msg, tmpmsg, sizeof(tmpmsg));
    //set correct bytes according to the values in json and if not exists keep default 0x00 value which keeps current setting for this byte
    jsonValue = jsonDoc["zone1"]["heat"]["target"]["high"]; if (!jsonValue.isNull()) cmd[75] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone1"]["heat"]["target"]["low"]; if (!jsonValue.isNull()) cmd[76] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone1"]["heat"]["outside"]["low"]; if (!jsonValue.isNull()) cmd[77] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone1"]["heat"]["outside"]["high"]; if (!jsonValue.isNull()) cmd[78] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone2"]["heat"]["target"]["high"]; if (!jsonValue.isNull()) cmd[79] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone2"]["heat"]["target"]["low"]; if (!jsonValue.isNull()) cmd[80] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone2"]["heat"]["outside"]["low"]; if (!jsonValue.isNull()) cmd[81] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone2"]["heat"]["outside"]["high"]; if (!jsonValue.isNull()) cmd[82] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone1"]["cool"]["target"]["high"]; if (!jsonValue.isNull()) cmd[86] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone1"]["cool"]["target"]["low"]; if (!jsonValue.isNull()) cmd[87] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone1"]["cool"]["outside"]["low"]; if (!jsonValue.isNull()) cmd[88] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone1"]["cool"]["outside"]["high"]; if (!jsonValue.isNull()) cmd[89] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone2"]["cool"]["target"]["high"]; if (!jsonValue.isNull()) cmd[90] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone2"]["cool"]["target"]["low"]; if (!jsonValue.isNull()) cmd[91] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone2"]["cool"]["outside"]["low"]; if (!jsonValue.isNull()) cmd[92] = jsonValue.as<int>() + 128;
    jsonValue = jsonDoc["zone2"]["cool"]["outside"]["high"]; if (!jsonValue.isNull()) cmd[93] = jsonValue.as<int>() + 128;
  } else {
    char tmpmsg[256] = { 0 };
    snprintf(tmpmsg, 255, "SetCurves JSON decode failed!");
    memcpy(log_msg, tmpmsg, sizeof(tmpmsg));
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_zones(char *msg, unsigned char *cmd, char *log_msg) {

  String set_mode_string(msg);

  byte set_mode;
  switch (set_mode_string.toInt()) {
    case 0: set_mode = 64; break;
    case 1: set_mode = 128; break;
    case 2: set_mode = 192; break;
    default: set_mode = 0; break;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set zones active state to %d"), set_mode);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[6] = set_mode;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_floor_heat_delta(char *msg, unsigned char *cmd, char *log_msg) {

  String set_temperature_string(msg);

  byte request_temp = set_temperature_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set floor heat delta to %d"), request_temp - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[84] = request_temp;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_floor_cool_delta(char *msg, unsigned char *cmd, char *log_msg) {

  String set_temperature_string(msg);

  byte request_temp = set_temperature_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set floor cool delta to %d"), request_temp - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[94] = request_temp;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_dhw_heat_delta(char *msg, unsigned char *cmd, char *log_msg) {

  String set_temperature_string(msg);

  byte request_temp = set_temperature_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set DHW heat delta to %d"), request_temp - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[99] = request_temp;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_reset(char *msg, unsigned char *cmd, char *log_msg) {
  byte resetRequest = 0;
  String set_reset_string(msg);

  if ( set_reset_string.toInt() == 1 ) {
    resetRequest = 1;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set reset to %d"), resetRequest);
    memcpy(log_msg, tmp, sizeof(tmp));
  }


  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[8] = resetRequest;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_heater_delay_time(char *msg, unsigned char *cmd, char *log_msg) {

  String stringValue(msg);

  byte byteValue = stringValue.toInt() + 1;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set heater delay time to %d"), byteValue - 1 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[104] = byteValue;
  }

  return sizeof(panasonicSendQuery);
}
unsigned int set_heater_start_delta(char *msg, unsigned char *cmd, char *log_msg) {

  String stringValue(msg);

  byte byteValue = stringValue.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set heater start delta to %d"), byteValue - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[105] = byteValue;
  }

  return sizeof(panasonicSendQuery);
}
unsigned int set_heater_stop_delta(char *msg, unsigned char *cmd, char *log_msg) {

  String stringValue(msg);

  byte byteValue = stringValue.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set heater stop delta to %d"), byteValue - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[106] = byteValue;
  }

  return sizeof(panasonicSendQuery);
}
unsigned int set_main_schedule(char *msg, unsigned char *cmd, char *log_msg) {

  String stringValue(msg);

  byte byteValue = 64; //hex 0x40

  if ( stringValue.toInt() == 1 ) {
    byteValue = 128; //hex 0x80
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set main schedule to %d"), (byteValue / 64) - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[5] = byteValue;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_alt_external_sensor(char *msg, unsigned char *cmd, char *log_msg) {

  String set_alt_string(msg);

  byte set_alt = 16;
  if ( set_alt_string.toInt() == 1 ) {
    set_alt = 32;
  }

    {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set alternative external sensor to %d"), ((set_alt / 16) - 1) );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[20] = set_alt;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_external_pad_heater(char *msg, unsigned char *cmd, char *log_msg) {

  String set_pad_string(msg);

  byte set_pad = 16;
  if ( set_pad_string.toInt() == 1 ) {
    set_pad = 32;
  }
  if ( set_pad_string.toInt() == 2 ) {
    set_pad = 48;
  }
    {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set external pad heater to %d"), ((set_pad / 16) - 1) );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[25] = set_pad;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_buffer_delta(char *msg, unsigned char *cmd, char *log_msg) {

  String set_temperature_string(msg);

  byte request_temp = set_temperature_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set buffer tank delta to %d"), request_temp - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[59] = request_temp;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_buffer(char *msg, unsigned char *cmd, char *log_msg) {

  String set_buffer_string(msg);

  byte set_buffer = 4;
  if ( set_buffer_string.toInt() == 1 ) {
    set_buffer = 8;
  }

    {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set buffer enabled to %d"), ((set_buffer / 4) - 1) );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[24] = set_buffer;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_heatingoffoutdoortemp(char *msg, unsigned char *cmd, char *log_msg) {

  String set_heatingoffoutdoortemp_string(msg);

  byte request_temp = set_heatingoffoutdoortemp_string.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set heating off outdoor temp %d"), request_temp - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[83] = request_temp;
  }

  return sizeof(panasonicSendQuery);
  
}

unsigned int set_external_control(char *msg, unsigned char *cmd, char *log_msg){
  const byte off_state=1;
  const byte address=23;
  byte value = off_state;
  if ( String(msg).toInt() == 1 ) {
    value = off_state * 2;
  }
    {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set external control enabled to %d"), ((value / off_state) - 1) );
    memcpy(log_msg, tmp, sizeof(tmp));
  }
  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[address] = value;
  }
  return sizeof(panasonicSendQuery);
}

unsigned int set_external_heat_cool_control(char *msg, unsigned char *cmd, char *log_msg){
  const byte off_state=4;
  const byte address=23;
  byte value = off_state;
  if ( String(msg).toInt() == 1 ) {
    value = off_state * 2;
  }
    {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set external cool/heat control enabled to %d"), ((value / off_state) - 1) );
    memcpy(log_msg, tmp, sizeof(tmp));
  }
  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[address] = value;
  }
  return sizeof(panasonicSendQuery);
}

unsigned int set_external_error(char *msg, unsigned char *cmd, char *log_msg){
  const byte off_state=16;
  const byte address=23;
  byte value = off_state;
  if ( String(msg).toInt() == 1 ) {
    value = off_state * 2;
  }
    {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set external error signal enabled to %d"), ((value / off_state) - 1) );
    memcpy(log_msg, tmp, sizeof(tmp));
  }
  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[address] = value;
  }
  return sizeof(panasonicSendQuery);
}

unsigned int set_heatingcontrol(char *msg, unsigned char *cmd, char *log_msg) {

  const byte address=30;
  byte value = 0b01;

  if ( String(msg).toInt() == 1 ) {
    value = 0b10;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set heating control %d"), value - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[address] = value << 2;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_smart_dhw(char *msg, unsigned char *cmd, char *log_msg) {

  const byte address=24;
  byte value = 0b01;

  if ( String(msg).toInt() == 1 ) {
    value = 0b10;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set smart dhw %d"), value - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[address] = value << 6;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_quiet_mode_priority(char *msg, unsigned char *cmd, char *log_msg) {

  const byte address=11;
  byte value = 0b01;

  if ( String(msg).toInt() == 1 ) {
    value = 0b10;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set quiet mode priority %d"), value - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[address] = value << 4;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_pump_flowrate_mode(char *msg, unsigned char *cmd, char *log_msg) {

  const byte address=29;
  byte value = 0b01;

  if ( String(msg).toInt() == 1 ) {
    value = 0b10;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set pump flowrate mode %d"), value - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[address] = value << 4;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_dhw_sensor_selection(char *msg, unsigned char *cmd, char *log_msg) {

  const byte address = 11;
  byte value = 0b01;

  if ( String(msg).toInt() == 1 ) {
    value = 0b10;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set dhw sensor selection %d"), value - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[address] = value;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_dhw_heater_state(char *msg, unsigned char *cmd, char *log_msg) {

  const byte address = 9;
  byte value = 0b01;

  if ( String(msg).toInt() == 1 ) {
    value = 0b10;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set dhw heater state %d"), value - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[address] = value << 2;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_room_heater_state(char *msg, unsigned char *cmd, char *log_msg) {

  const byte address = 9;
  byte value = 0b01;

  if ( String(msg).toInt() == 1 ) {
    value = 0b10;
  }

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set room heater state %d"), value - 1);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[address] = value;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_heater_on_outdoor_temp(char *msg, unsigned char *cmd, char *log_msg) {

  String stringValue(msg);

  byte byteValue = stringValue.toInt() + 128;

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set heater on outdoor temp to %d"), byteValue - 128 );
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[85] = byteValue;
  }

  return sizeof(panasonicSendQuery);
}

unsigned int set_external_compressor_control(char *msg, unsigned char *cmd, char *log_msg){
  const byte off_state=64;
  const byte address=23;
  byte value = off_state;
  if ( String(msg).toInt() == 1 ) {
    value = off_state * 2;
  }
    {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set external compressor control enabled to %d"), ((value / off_state) - 1) );
    memcpy(log_msg, tmp, sizeof(tmp));
  }
  {
    memcpy_P(cmd, panasonicSendQuery, sizeof(panasonicSendQuery));
    cmd[address] = value;
  }
  return sizeof(panasonicSendQuery);
}

//start of optional pcb commands
unsigned int set_byte_6(int val, int base, int bit, char *log_msg, const char *func) {
  unsigned char hex = (optionalPCBQuery[6] & ~(base << bit)) | (val << bit);

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set optional pcb '%s' to state %d (result byte 6: %02x)"), func, val, hex);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    optionalPCBQuery[6] = hex;
  }

  return sizeof(optionalPCBQuery);
}

unsigned int set_byte_9(char *msg, char *log_msg) {
  String set_pcb_string(msg);

  byte set_pcb_value = set_pcb_string.toInt();

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set optional pcb '%s' to %02x"), __FUNCTION__, set_pcb_value);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    optionalPCBQuery[9] = set_pcb_value;
  }
  return sizeof(optionalPCBQuery);
}

unsigned int set_heat_cool_mode(char *msg, char *log_msg) {
  String set_pcb_string(msg);
  int set_pcb_value = (set_pcb_string.toInt() == 1);

  return set_byte_6(set_pcb_value, 0b1, 7, log_msg, __FUNCTION__);
}

unsigned int set_compressor_state(char *msg, char *log_msg) {
  String set_pcb_string(msg);
  int set_pcb_value = (set_pcb_string.toInt() == 1);

  return set_byte_6(set_pcb_value, 0b1, 6, log_msg, __FUNCTION__);
}

unsigned int set_smart_grid_mode(char *msg, char *log_msg) {
  String set_pcb_string(msg);
  int set_pcb_value = set_pcb_string.toInt();

  if (set_pcb_value < 4) {
    return set_byte_6(set_pcb_value, 0b11, 4, log_msg, __FUNCTION__);
  } else {
    return 0;
  }
}

unsigned int set_external_thermostat_1_state(char *msg, char *log_msg) {
  String set_pcb_string(msg);
  int set_pcb_value = set_pcb_string.toInt();

  if (set_pcb_value < 4) {
    return set_byte_6(set_pcb_value, 0b11, 2, log_msg, __FUNCTION__);
  } else {
    return 0;
  }
}

unsigned int set_external_thermostat_2_state(char *msg, char *log_msg) {
  String set_pcb_string(msg);
  int set_pcb_value = set_pcb_string.toInt();

  if (set_pcb_value < 4) {
    return set_byte_6(set_pcb_value, 0b11, 0, log_msg, __FUNCTION__);
  } else {
    return 0;
  }
}

unsigned int set_demand_control(char *msg, char *log_msg) {
  String set_pcb_string(msg);

  byte set_pcb_value = set_pcb_string.toInt();

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set optional pcb '%s' to %02x"), __FUNCTION__, set_pcb_value);
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    optionalPCBQuery[14] = set_pcb_value;
  }

  return sizeof(optionalPCBQuery);
}

unsigned int set_xxx_temp(char *msg, char *log_msg, int byte, const char *func) {
  String set_pcb_string(msg);

  float temp = set_pcb_string.toFloat();

  {
    char tmp[256] = { 0 };
    snprintf_P(tmp, 255, PSTR("set optional pcb '%s' to temp %.2f (%02x)"), func, temp, temp2hex(temp));
    memcpy(log_msg, tmp, sizeof(tmp));
  }

  {
    optionalPCBQuery[byte] = temp2hex(temp);
  }

  return sizeof(optionalPCBQuery);
}

unsigned int set_pool_temp(char *msg, char *log_msg) {
  return set_xxx_temp(msg, log_msg, 7, __FUNCTION__);
}

unsigned int set_buffer_temp(char *msg, char *log_msg) {
  return set_xxx_temp(msg, log_msg, 8, __FUNCTION__);
}

unsigned int set_z1_room_temp(char *msg, char *log_msg) {
  return set_xxx_temp(msg, log_msg, 10, __FUNCTION__);
}

unsigned int set_z1_water_temp(char *msg, char *log_msg) {
  return set_xxx_temp(msg, log_msg, 16, __FUNCTION__);
}

unsigned int set_z2_room_temp(char *msg, char *log_msg) {
  return set_xxx_temp(msg, log_msg, 11, __FUNCTION__);
}

unsigned int set_z2_water_temp(char *msg, char *log_msg) {
  return set_xxx_temp(msg, log_msg, 15, __FUNCTION__);
}

unsigned int set_solar_temp(char *msg, char *log_msg) {
  return set_xxx_temp(msg, log_msg, 13, __FUNCTION__);
}

namespace {

constexpr unsigned long MQTT_COMMAND_GAP_MS = 7000;
constexpr unsigned long MQTT_COMMAND_CONFIRM_TIMEOUT_MS = 30000;
constexpr unsigned long MQTT_COMMAND_QUEUE_RETRY_MS = 1000;
constexpr uint8_t MQTT_COMMAND_MAX_RETRIES = 3;
constexpr uint8_t MQTT_COMMAND_QUEUE_SIZE = 10;
constexpr uint8_t MQTT_COMMAND_MAX_EXPECTATIONS = 16;

struct MqttCommandExpectation {
  uint8_t topic;
  uint8_t commandByte;
  int16_t desired;
  bool confirmed;
};

struct PendingMqttCommand {
  bool active;
  bool waitingToSend;
  bool wasSent;
  uint8_t length;
  uint8_t retries;
  uint8_t expectationCount;
  unsigned long sentAt;
  unsigned long nextSendAt;
  unsigned long sequence;
  char name[29];
  byte command[PANASONICQUERYSIZE];
  MqttCommandExpectation expectations[MQTT_COMMAND_MAX_EXPECTATIONS];
};

struct CommandConfirmation {
  const char* command;
  uint8_t topic;
};

const CommandConfirmation commandConfirmations[] = {
  { "SetHeatpump", 0 },
  { "SetHolidayMode", 19 },
  { "SetQuietMode", 18 },
  { "SetPowerfulMode", 17 },
  { "SetZ1HeatRequestTemperature", 27 },
  { "SetZ1CoolRequestTemperature", 28 },
  { "SetZ2HeatRequestTemperature", 34 },
  { "SetZ2CoolRequestTemperature", 35 },
  { "SetOperationMode", 4 },
  { "SetForceDHW", 2 },
  { "SetDHWTemp", 9 },
  { "SetForceDefrost", 26 },
  { "SetForceSterilization", 69 },
  { "SetMaxPumpDuty", 95 },
  { "SetZones", 94 },
  { "SetFloorHeatDelta", 23 },
  { "SetFloorCoolDelta", 24 },
  { "SetDHWHeatDelta", 22 },
  { "SetHeaterDelayTime", 96 },
  { "SetHeaterStartDelta", 97 },
  { "SetHeaterStopDelta", 98 },
  { "SetMainSchedule", 13 },
  { "SetBufferDelta", 113 },
  { "SetBuffer", 99 },
  { "SetForceHeater", 68 },
  { "SetAltExternalSensor", 108 },
  { "SetExternalPadHeater", 114 },
  { "SetHeatingOffOutdoorTemp", 77 },
  { "SetExternalControl", 119 },
  { "SetExternalError", 121 },
  { "SetExternalCompressorControl", 122 },
  { "SetExternalHeatCoolControl", 120 },
  { "SetBivalentControl", 129 },
  { "SetBivalentMode", 130 },
  { "SetBivalentStartTemp", 131 },
  { "SetBivalentAPStartTemp", 134 },
  { "SetBivalentAPStopTemp", 135 },
  { "SetHeatingControl", 139 },
  { "SetSmartDHW", 140 },
  { "SetQuietModePriority", 141 },
  { "SetPumpFlowrateMode", 106 },
  { "SetDHWSensorSelection", 143 },
  { "SetDHWHeaterState", 58 },
  { "SetRoomHeaterState", 59 },
  { "SetHeaterOnOutdoorTemp", 78 },
};

struct CurveConfirmation {
  uint8_t commandByte;
  uint8_t topic;
};

const CurveConfirmation curveConfirmations[] = {
  { 75, 29 }, { 76, 30 }, { 77, 32 }, { 78, 31 },
  { 79, 82 }, { 80, 83 }, { 81, 85 }, { 82, 84 },
  { 86, 72 }, { 87, 73 }, { 88, 75 }, { 89, 74 },
  { 90, 86 }, { 91, 87 }, { 92, 89 }, { 93, 88 },
};

PendingMqttCommand pendingMqttCommands[MQTT_COMMAND_QUEUE_SIZE] = {};
unsigned long mqttCommandSequence = 0;
unsigned long lastMqttCommandSendAt = 0;
bool mqttCommandWasSent = false;

bool currentDataIsFresh(char* data, unsigned long dataAt, unsigned int waitTime) {
  if (data == nullptr || dataAt == 0 || (uint8_t)data[0] != 0x71 ||
      (uint8_t)data[1] != 0xC8 || (uint8_t)data[2] != 0x01 ||
      (uint8_t)data[3] != 0x10) return false;
  unsigned long maximumAge = (unsigned long)waitTime * 4000UL;
  if (maximumAge < 60000UL) maximumAge = 60000UL;
  return (unsigned long)(millis() - dataAt) <= maximumAge;
}

int confirmationTopicFor(const char* command) {
  for (const auto& confirmation : commandConfirmations) {
    if (strcmp(command, confirmation.command) == 0) return confirmation.topic;
  }
  return -1;
}

bool expectationMatches(const char* commandName,
    const MqttCommandExpectation& expectation, char* data) {
  int actual = getDataValue(data, expectation.topic).toInt();
  if (strcmp(commandName, "SetOperationMode") == 0) {
    if ((expectation.desired == 2 && actual == 7) || (expectation.desired == 6 && actual == 8)) return true;
  }
  if (strcmp(commandName, "SetHolidayMode") == 0 &&
      expectation.desired == 1 && actual == 2) return true;
  return actual == expectation.desired;
}

uint8_t unconfirmedExpectationCount(const PendingMqttCommand& pending) {
  uint8_t count = 0;
  for (uint8_t i = 0; i < pending.expectationCount; i++) {
    if (!pending.expectations[i].confirmed) count++;
  }
  return count;
}

bool sameRequest(const PendingMqttCommand& left, const PendingMqttCommand& right) {
  if (strcmp(left.name, right.name) != 0 || left.expectationCount != right.expectationCount) return false;
  if (left.expectationCount == 0) return memcmp(left.command, right.command, left.length) == 0;
  for (uint8_t i = 0; i < left.expectationCount; i++) {
    if (left.expectations[i].topic != right.expectations[i].topic ||
        left.expectations[i].desired != right.expectations[i].desired) return false;
  }
  return true;
}

void logMqttCommand(void (*log_message)(char*), const char* format, const char* command, int value = -1) {
  char message[192];
  if (value >= 0) {
    snprintf(message, sizeof(message), format, command, value);
  } else {
    snprintf(message, sizeof(message), format, command);
  }
  log_message(message);
}

void addExpectations(PendingMqttCommand& pending, const char* payload) {
  if (strcmp(pending.name, "SetCurves") == 0) {
    for (const auto& curve : curveConfirmations) {
      if (pending.command[curve.commandByte] == 0) continue;
      MqttCommandExpectation& expectation = pending.expectations[pending.expectationCount++];
      expectation.topic = curve.topic;
      expectation.commandByte = curve.commandByte;
      expectation.desired = static_cast<int16_t>(pending.command[curve.commandByte]) - 128;
      expectation.confirmed = false;
    }
    return;
  }

  int topic = confirmationTopicFor(pending.name);
  if (topic >= 0) {
    MqttCommandExpectation& expectation = pending.expectations[0];
    expectation.topic = topic;
    expectation.commandByte = 0;
    expectation.desired = String(payload).toInt();
    expectation.confirmed = false;
    pending.expectationCount = 1;
  }
}

void applyKnownState(PendingMqttCommand& pending, char* data) {
  if (data == nullptr || data[0] != 0x71) return;
  for (uint8_t i = 0; i < pending.expectationCount; i++) {
    MqttCommandExpectation& expectation = pending.expectations[i];
    if (!expectation.confirmed && expectationMatches(pending.name, expectation, data)) {
      expectation.confirmed = true;
      if (expectation.commandByte > 0) pending.command[expectation.commandByte] = 0;
    }
  }
}

}  // namespace

bool heatpump_command_has_changes(const char* topic, const char* payload,
    char* currentData, unsigned long currentDataAt, unsigned int waitTime,
    unsigned char* command, unsigned int length,
    char* status, size_t statusSize) {
  if (topic == nullptr || payload == nullptr ||
      command == nullptr || length == 0) return true;

  if (strcmp(topic, "SetCurves") == 0) {
    uint8_t requested = 0;
    for (const auto& curve : curveConfirmations) {
      if (curve.commandByte >= length || command[curve.commandByte] == 0) continue;
      requested++;
    }
    if (requested == 0) {
      if (status != nullptr && statusSize > 0) {
        snprintf(status, statusSize, "SetCurves skipped: no valid curve values supplied");
      }
      return false;
    }
    if (!currentDataIsFresh(currentData, currentDataAt, waitTime)) return true;

    uint8_t unchanged = 0;
    for (const auto& curve : curveConfirmations) {
      if (curve.commandByte >= length || command[curve.commandByte] == 0) continue;
      int desired = (int)command[curve.commandByte] - 128;
      int actual = getDataValue(currentData, curve.topic).toInt();
      if (actual == desired) {
        command[curve.commandByte] = 0;
        unchanged++;
      }
    }
    if (unchanged == requested) {
      if (status != nullptr && statusSize > 0) {
        snprintf(status, statusSize,
          "SetCurves skipped: Panasonic already reports all requested values");
      }
      return false;
    }
    if (unchanged > 0 && status != nullptr && statusSize > 0) {
      snprintf(status, statusSize,
        "SetCurves: omitted %u unchanged value%s; %u value%s will be sent",
        unchanged, unchanged == 1 ? "" : "s", requested - unchanged,
        requested - unchanged == 1 ? "" : "s");
    }
    return true;
  }

  if (!currentDataIsFresh(currentData, currentDataAt, waitTime)) return true;

  int confirmationTopic = confirmationTopicFor(topic);
  if (confirmationTopic < 0) return true;

  MqttCommandExpectation expectation = {
    (uint8_t)confirmationTopic, 0, (int16_t)String(payload).toInt(), false
  };
  if (!expectationMatches(topic, expectation, currentData)) return true;

  if (status != nullptr && statusSize > 0) {
    snprintf(status, statusSize,
      "%s skipped: Panasonic already reports the requested value %d",
      topic, expectation.desired);
  }
  return false;
}

bool queue_mqtt_heatpump_command(char* topic, char* msg, char* currentData,
    unsigned long currentDataAt, unsigned int waitTime, void (*log_message)(char*)) {
  cmdStruct matchedCommand;
  bool matched = false;
  for (unsigned int i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
    cmdStruct candidate;
    memcpy_P(&candidate, &commands[i], sizeof(candidate));
    if (strcmp(topic, candidate.name) == 0) {
      matchedCommand = candidate;
      matched = true;
      break;
    }
  }
  if (!matched) return false;

  PendingMqttCommand incoming = {};
  strncpy(incoming.name, topic, sizeof(incoming.name) - 1);
  char commandLog[256] = {};
  unsigned int length = matchedCommand.func(msg, incoming.command, commandLog);
  log_message(commandLog);
  if (length == 0 || length > sizeof(incoming.command)) {
    logMqttCommand(log_message, "[MQTT command] %s rejected: invalid value", topic);
    return true;
  }
  incoming.length = length;
  incoming.active = true;
  incoming.waitingToSend = true;
  incoming.nextSendAt = millis();
  addExpectations(incoming, msg);
  if (strcmp(topic, "SetCurves") == 0 && incoming.expectationCount == 0) {
    logMqttCommand(log_message, "[MQTT command] %s rejected: invalid or empty curve JSON", topic);
    return true;
  }

  if (currentDataIsFresh(currentData, currentDataAt, waitTime)) {
    applyKnownState(incoming, currentData);
  }
  if (incoming.expectationCount > 0 && unconfirmedExpectationCount(incoming) == 0) {
    logMqttCommand(log_message, "[MQTT command] %s skipped: Panasonic already reports the requested value", topic);
    return true;
  }

  PendingMqttCommand* freeSlot = nullptr;
  for (auto& pending : pendingMqttCommands) {
    if (pending.active && strcmp(pending.name, topic) == 0) {
      if (sameRequest(pending, incoming)) {
        logMqttCommand(log_message, "[MQTT command] %s ignored: identical request is already pending", topic);
        return true;
      }
      freeSlot = &pending;
      logMqttCommand(log_message, "[MQTT command] %s replaces the pending request", topic);
      break;
    }
    if (!pending.active && freeSlot == nullptr) freeSlot = &pending;
  }
  if (freeSlot == nullptr) {
    logMqttCommand(log_message, "[MQTT command] %s rejected: confirmation queue is full", topic);
    return true;
  }

  incoming.sequence = ++mqttCommandSequence;
  *freeSlot = incoming;
  logMqttCommand(log_message, "[MQTT command] %s queued", topic);
  return true;
}

void confirm_mqtt_heatpump_commands(char* data, void (*log_message)(char*)) {
  for (auto& pending : pendingMqttCommands) {
    if (!pending.active || pending.expectationCount == 0) continue;
    uint8_t before = unconfirmedExpectationCount(pending);
    applyKnownState(pending, data);
    uint8_t after = unconfirmedExpectationCount(pending);
    if (after == 0) {
      logMqttCommand(log_message, pending.wasSent
        ? "[MQTT command] %s confirmed by Panasonic"
        : "[MQTT command] %s cancelled: Panasonic already reports the requested value",
        pending.name);
      pending.active = false;
    } else if (after < before && strcmp(pending.name, "SetCurves") == 0) {
      logMqttCommand(log_message, "[MQTT command] %s partially confirmed; %d curve values still pending", pending.name, after);
    }
  }
}

void process_mqtt_heatpump_commands(bool (*send_command)(byte*, int), void (*log_message)(char*)) {
  unsigned long now = millis();

  for (auto& pending : pendingMqttCommands) {
    if (!pending.active || pending.waitingToSend || !pending.wasSent || pending.expectationCount == 0) continue;
    if ((unsigned long)(now - pending.sentAt) < MQTT_COMMAND_CONFIRM_TIMEOUT_MS) continue;
    if (pending.retries >= MQTT_COMMAND_MAX_RETRIES) {
      logMqttCommand(log_message, "[MQTT command] %s failed: Panasonic did not confirm it after %d attempts",
        pending.name, MQTT_COMMAND_MAX_RETRIES + 1);
      pending.active = false;
      continue;
    }
    pending.retries++;
    pending.waitingToSend = true;
    pending.nextSendAt = now;
    pending.sequence = ++mqttCommandSequence;
    logMqttCommand(log_message, "[MQTT command] %s not confirmed; retry %d/3 queued", pending.name, pending.retries);
  }

  if (mqttCommandWasSent && (unsigned long)(now - lastMqttCommandSendAt) < MQTT_COMMAND_GAP_MS) return;

  PendingMqttCommand* next = nullptr;
  for (auto& pending : pendingMqttCommands) {
    if (!pending.active || !pending.waitingToSend || (long)(now - pending.nextSendAt) < 0) continue;
    if (next == nullptr || pending.sequence < next->sequence) next = &pending;
  }
  if (next == nullptr) return;

  if (!send_command(next->command, next->length)) {
    next->nextSendAt = now + MQTT_COMMAND_QUEUE_RETRY_MS;
    return;
  }

  next->waitingToSend = false;
  next->wasSent = true;
  next->sentAt = now;
  lastMqttCommandSendAt = now;
  mqttCommandWasSent = true;
  logMqttCommand(log_message, "[MQTT command] %s sent (attempt %d/4)", next->name, next->retries + 1);
  if (next->expectationCount == 0) {
    logMqttCommand(log_message, "[MQTT command] %s has no reliable reported state; it will not be retried", next->name);
    next->active = false;
  }
}




void send_heatpump_command(char* topic, char *msg, bool (*send_command)(byte*, int), void (*log_message)(char*), bool optionalPCB) {
  unsigned char cmd[256] = { 0 };
  char log_msg[256] = { 0 };
  unsigned int len = 0;

  for (unsigned int i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
    cmdStruct tmp;
    memcpy_P(&tmp, &commands[i], sizeof(tmp));
    if (strcmp(topic, tmp.name) == 0) {
      len = tmp.func(msg, cmd, log_msg);
      log_message(log_msg);
      if (len > 0) send_command(cmd, len);
    }
  }

  if (optionalPCB) {
    //run for optional pcb commands
    for (unsigned int i = 0; i < sizeof(optionalCommands) / sizeof(optionalCommands[0]); i++) {
      optCmdStruct tmp;
      memcpy_P(&tmp, &optionalCommands[i], sizeof(tmp));
      if (strcmp(topic, tmp.name) == 0) {
        len = tmp.func(msg, log_msg);
        log_message(log_msg);
#ifdef ESP32        
       xQueueOverwrite(pcbQueue, optionalPCBQuery);
#endif          
      }
    }
  }

}


bool saveOptionalPCB(byte* command, int length) {
  if (LittleFS.begin()) {
    File pcbfile = LittleFS.open("/optionalpcb.raw", "w");
    if (pcbfile) {
      pcbfile.write(command, length);
      pcbfile.close();
      return true;
    }

  }
  return false;
}
bool loadOptionalPCB(byte* command, int length) {
  if (LittleFS.begin()) {
    if (LittleFS.exists("/optionalpcb.raw")) {
      File pcbfile = LittleFS.open("/optionalpcb.raw", "r");
      if (pcbfile) {
        pcbfile.read(command, length);
        pcbfile.close();
        return true;
      }
    }
  }
  return false;
}
