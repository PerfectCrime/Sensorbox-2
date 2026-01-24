/*
;   Modbus RTU slave address is fixed to 0x0A, speed is 9600 bps 
:   
;   Input Registers (FC=04)(Read Only):
;  
;   Register  Register  
;   Address   length (16 bits)
;   0x0000      1       Sensorbox version 2             = 0x0014 (2 lsb mirror the 3/4 Wire and Rotation configuration data)
;                                                         0x0015 = 4 wire, CCW rotation
;                                                         0x0016 = 3 wire, CW rotation      
;                                                         0x0017 = 3 wire, CCW rotation  
;   0x0001      1       DSMR Version(MSB),CT's or P1(LSB) 0x3283 = DSMR version 50, P1 port connected (0x80) CT's Used (0x03)                    
;   0x0002      2       Volts L1 (32 bit floating point), Smartmeter P1 data
;   0x0004      2       Volts L2 (32 bit floating point), Smartmeter P1 data
;   0x0006      2       Volts L3 (32 bit floating point), Smartmeter P1 data
;   0x0008      2       Amps L1 (32 bit floating point), Smartmeter P1 data
;   0x000A      2       Amps L2 (32 bit floating point), Smartmeter P1 data
;   0x000C      2       Amps L3 (32 bit floating point), Smartmeter P1 data
;   0x000E      2       Amps L1 (32 bit floating point), CT imput 1
;   0x0010      2       Amps L2 (32 bit floating point), CT input 2
;   0x0012      2       Amps L3 (32 bit floating point), CT input 3
;
;   If the sensorbox software version >= 0x01xx, the following extra registers are available
;
;   0x0014      1       WiFi Connection Status  xxxxxACL xxxxxxWW = WiFi mode (00=Wifi Off, 01=On, 10=portal started)
;                                                    ||\_ Local Time Set
;                                                    |\__ Connected to WiFi
;                                                    \___ AP_STA mode (portal) active
;   0x0015      1       Time Hour(msb) Minute(lsb)
;   0x0016      1       Time Month(msb) Day(lsb)
;   0x0017      1       Time Year(msb) Weekday(lsb)
;   0x0018      2       IP address Sensorbox
;   0x001A      2       MAC Sensorbox (4 LSB bytes) http://SmartEVSE-012345.local can be derived from this
;   0x001C      4       Password portal (8 bytes)
;
;
;   Holding Registers (FC=06)(Write):
;
;   Register  Register  
;   Address   length (16 bits) 
;   0x0800      1       Field rotation setting (bit 0)      00000000 0000000x -> 0= Rotation right 1= Rotation Left
;                       3/4 wire configuration (bit 1)      00000000 000000x0 -> 0= 4Wire, 1= 3Wire
;   0x0801      1       Set WiFi mode                       00000000 000000xx -> 00 = disabled, 01 = enabled, 02 = start portal.
;                        
*/

#include <Arduino.h>
#include <Preferences.h>
#include <FS.h>
#include <LittleFS.h>

#include <WiFi.h>
#include "network_common.h"

#include <esp_task_wdt.h>

#include "Logging.h"
#include "ModbusServerRTU.h"        // Slave/node eModbus
#include "ModbusClientRTU.h"        // Master eModbus

#include "time.h"

#include "main.h"
#include "utils.h"
#include "prg_pic.h"
#include <SPI.h>
#include <HTTPClient.h>

extern "C" const char *mg_unpack(const char *name, size_t *size, time_t *mtime);

extern struct tm timeinfo;

//String APhostname = "SmartEVSE-" + String( MacId() & 0xffff, 10);           // SmartEVSE access point Name = SmartEVSE-xxxxx
extern String APhostname;
extern void network_loop(void);
extern webServerRequest* request;

// Create a ModbusRTU server and client instance on Serial1 
ModbusServerRTU MBserver(2000, ToggleRS485);                        // TCP timeout set to 2000 ms
//ModbusClientRTU MBclient(ToggleRS485);

Preferences preferences;

uint16_t DeviceID, Revision, UserID0, FlashADR, FlashData ;

uint8_t P1data[2000];
uint16_t ModbusData[50];    // 50 registers

extern uint8_t WIFImode;
String SmartEVSEHost = "";

unsigned long ModbusTimer=0;
unsigned long ModbusLedTimer=0;  // Track last Modbus LED update
unsigned char dataready=0, datareadyAPI=0, CTcount, DSMRver, IrmsMode = 0;
unsigned char LedCnt, LedState, LedSeq[4] = {0,0,0,0};
float EnergyTariff1 = 0, EnergyTariff2 = 0;
float EnergyReturnTariff1 = 0, EnergyReturnTariff2 = 0;
float Power[3] = {0,0,0};
float PowerReturn[3] = {0,0,0};
float TotalPower = 0, TotalPowerReturn = 0;
float GasDelivered = 0;
float Irms[3], Volts[3], IrmsCT[3];                                             // float is 32 bits; current in A
int16_t MainsMeterIrms[3];                                                      // current in dA (10 * A) !!!
bool lockedToP1 = false;
unsigned char led = 0, Wire = WIRES4 + CW;
uint16_t blinkram = 0, P1taskram = 0;
extern bool LocalTimeSet;
int phasesLastUpdate = 0;
unsigned long PortalTimeout = 0;
bool WiFiModbusCtrl = false;


void SetLedSequence(uint8_t source) {
    uint8_t n = 1;

    // Set Led sequence for the next 2 seconds
    // reset to first part of sequence
    LedCnt = 0;
    LedState = 8;

    // When we have received a valid measurement from the SmartMeter, the led will blink only once.
    if (source & 0xC0) {
        // DSMR version not 5.x! Blink RED
        if (DSMRver < 50) LedSeq[0] = LED_RED;
        // DSMR version OK, Blink GREEN
        else LedSeq[0] = LED_GREEN;

    // No SmartMeter connected, we use the CT's
    // CT measurement available?
    } else if (source & 0x03) {
        // CT measurements with current direction?
        if (IrmsMode == 0) {
            if (IrmsCT[0] < -0.1 ) LedSeq[0]=LED_GREEN; else LedSeq[0]=LED_ORANGE;
            if (IrmsCT[1] < -0.1 ) LedSeq[1]=LED_GREEN; else LedSeq[1]=LED_ORANGE;
            if (IrmsCT[2] < -0.1 ) LedSeq[2]=LED_GREEN; else LedSeq[2]=LED_ORANGE;
            // 6 States, ON/OFF (3 CT's)
            n = 3;
        } else {
            // Blink 1x Orange when not using MAINS input
            LedSeq[0] = LED_ORANGE;                
        }
    // LED_RED_ON (PIC chip not programmed, or communication with P1 lost)
    } else {
        LedSeq[0] = LED_RED;
    }

    // Show WiFi mode as last blink in sequence
    if (WIFImode == 2) LedSeq[n] = LED_RED;
    else if (WIFImode == 1) LedSeq[n] = LED_GREEN;
}


// Every ~2 seconds send measurement data to APIs
void Timer2S(void * parameter) {
    while (true) {
        if (WiFi.status() == WL_CONNECTED) {

          // We have valid data. Does not allow switching from P1 back to CT data !
          // A broken P1 connection now creates a comm error instead of (invalid) CT measurements being used.
          if ( (lockedToP1 == true && datareadyAPI & 0x80) || (lockedToP1 == false && datareadyAPI & 0x03) ) {

            for (uint8_t x = 0; x < 3; x++) {
              // P1 data has precedence over CT data, although CT data might be newer.
              if (datareadyAPI & 0x80) MainsMeterIrms[x] = round(Irms[x] * 10);
              else MainsMeterIrms[x] = round(IrmsCT[x] * 10);
            }
  
    #if MQTT
            //publishing to the broker
            MQTTclient.publish(MQTTprefix + "/MainsCurrentL1", MainsMeterIrms[0], false, 0);
            MQTTclient.publish(MQTTprefix + "/MainsCurrentL2", MainsMeterIrms[1], false, 0);
            MQTTclient.publish(MQTTprefix + "/MainsCurrentL3", MainsMeterIrms[2], false, 0);
            if (lockedToP1) { //we don't have volts from CT
                for (uint8_t i = 0; i < 3; i++) {
                    MQTTclient.publish((String(MQTTprefix) + "/MainsVoltageL" + (i + 1)).c_str(), String(Volts[i], 1).c_str(), false, 0);
                }
                for (uint8_t i = 0; i < 3; i++) {    
                    MQTTclient.publish((String(MQTTprefix) + "/PowerDeliveredL" + (i + 1)).c_str(), Power[i], false, 0);        // Watt
                }
                for (uint8_t i = 0; i < 3; i++) {    
                    MQTTclient.publish((String(MQTTprefix) + "/PowerReturnedL" + (i + 1)).c_str(), PowerReturn[i], false, 0);   // Watt
                }
                for (uint8_t i = 0; i < 3; i++) {        
                    MQTTclient.publish((String(MQTTprefix) + "/CTCurrentL" + (i + 1)).c_str(), round(IrmsCT[i] * 10), false, 0); // dA
                }

                MQTTclient.publish(MQTTprefix + "/PowerDelivered", TotalPower, false, 0);         // Watt
                MQTTclient.publish(MQTTprefix + "/PowerReturned", TotalPowerReturn, false, 0);    // Watt
                   
                MQTTclient.publish(MQTTprefix + "/EnergyDeliveredTariff1", String(EnergyTariff1, 3), false, 0);  // kWh
                MQTTclient.publish(MQTTprefix + "/EnergyDeliveredTariff2", String(EnergyTariff2, 3), false, 0);  // kWh

                MQTTclient.publish(MQTTprefix + "/EnergyReturnedTariff1", String(EnergyReturnTariff1, 3), false, 0);  // kWh
                MQTTclient.publish(MQTTprefix + "/EnergyReturnedTariff2", String(EnergyReturnTariff2, 3), false, 0);  // kWh

                if (GasDelivered > 0) MQTTclient.publish(MQTTprefix + "/GasDelivered", String(GasDelivered, 3), false, 0);   // m3

            } 
            
            MQTTclient.publish(MQTTprefix + "/ESPUptime", esp_timer_get_time() / 1000000, false, 0);
            MQTTclient.publish(MQTTprefix + "/WiFiRSSI", String(WiFi.RSSI()), false, 0);
 
    #endif
            if (SmartEVSEHost != "") {                                              // we have a configured wifi host
                if (SmartEVSEHost.substring(0,4) == "http") {                       // not MQTT, but http[s]
                    char *URL = NULL;
                    asprintf(&URL, "%s/currents?L1=%i&L2=%i&L3=%i", SmartEVSEHost.c_str(), (int) MainsMeterIrms[0], (int) MainsMeterIrms[1], (int) MainsMeterIrms[2]); //will be freed
                    HTTPClient httpClient;
                    httpClient.begin(URL);
                    httpClient.addHeader("Content-Length", "0");
                    int httpCode = httpClient.POST("");  //Make the request

                    // only handle 200/301, fail on everything else
                    if( httpCode != HTTP_CODE_OK && httpCode != HTTP_CODE_MOVED_PERMANENTLY ) {
                        _LOG_A("Error on HTTP request (httpCode=%i)\n", httpCode);
                    }
                    httpClient.end();
                    FREE(URL);
                } else {                                                            // MQTT
    #if MQTT
                    //publishing to the SmartEVSE host directly
                    //mosquitto_pub  -h ip-of-mosquitto-server -u username -P password -t 'SmartEVSE-xxxxx/Set/MainsMeter' -m L1:L2:L3
                    //in deci Ampères
                    char *currents;
                    asprintf(&currents, "%i:%i:%i", (int) MainsMeterIrms[0], (int) MainsMeterIrms[1], (int) MainsMeterIrms[2]);
                    MQTTclient.publish(SmartEVSEHost + "/Set/MainsMeter", currents, false, 0);
                    FREE(currents);
    #endif
                }
            }
          } else datareadyAPI = 0;  // When locked to P1 and no data, the led blinks RED
        } // Wifi connected
        
        // Only set LED sequence from API if Modbus hasn't been active recently
        if (millis() - ModbusLedTimer > MODBUS_LED_TIMEOUT) {
            SetLedSequence(datareadyAPI);
        }
        datareadyAPI = 0;

        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}

String readMqttCaCert() {
    if (!LittleFS.exists("/mqtt_ca.pem")) {
        _LOG_A("No /mqtt_ca.pem found.\n");
        return "";
    }
    File file = LittleFS.open("/mqtt_ca.pem", "r");
    if (!file) {
        _LOG_A("Failed to open /mqtt_ca.pem for reading.\n");
        return "";
    }
    String cert = file.readString();
    file.close();
    return cert;
}

void writeMqttCaCert(const String& cert) {
    if (cert.isEmpty()) {
        LittleFS.remove("/mqtt_ca.pem");
        _LOG_D("Removed /mqtt_ca.pem.\n");
        return;
    }
    File file = LittleFS.open("/mqtt_ca.pem", "w");
    if (!file) {
        _LOG_A("Failed to open /mqtt_ca.pem for writing.\n");
        return;
    }
    file.print(cert);
    file.close();
    _LOG_D("Wrote %d bytes to /mqtt_ca.pem.\n", cert.length());
}


// ------------------------------------------------ Settings -----------------------------------------------------
// 
// ---------------------------------------------------------------------------------------------------------------


void write_settings(void) {
  // Check if stored data is valid
  if (WIFImode > 2) WIFImode = WIFI_MODE;

  if (preferences.begin("settings", false) ) {

    preferences.putUChar("Wire", Wire);
    preferences.putUChar("WIFImode", WIFImode);
    preferences.end();

  } else {
      _LOG_A("Can not open preferences!\n");
  }
}


void read_settings(bool write) {    
  if (preferences.begin("settings", false) == true) {
    
    Wire = preferences.getUChar("Wire", WIRES4);
    WIFImode = preferences.getUChar("WIFImode", WIFI_MODE);
    SmartEVSEHost = preferences.getString("SmartEVSEHost", "");
    preferences.end();       

    if (write) write_settings();

  } else {
      _LOG_A("Can not open preferences!\n");
  }
}


// ----------------------------------------------------------------------------------------------------------------
// CT data is measured by the PIC, is send on a serial line to Serial0 of the ESP32
// The final calculations and phase angle corrections are done by the ESP32.  
// We use / as start line char, ! is end of line, just like a P1 message the data is followed by a CRC16 checksum
// Called by P1Task every 100ms
//
void CTReceive() {
	char *ret, ch;
  int Samples = 0;
  unsigned char x, CTwire = 0;
  static unsigned char CTstring[100], CTeot = 0;
  static unsigned char CTlength = 0, CTptr = 0;
  uint16_t crccal, crcdata;
  int Power1A = 0, Power1B = 0, Power2A = 0, Power2B = 0, Power3A = 0, Power3B = 0;
  const float y1=-3.114637, x1=4.043093, y2=-3.057741, x2=3.988535, y3=-3.000724, x3=3.933821; //60 samples 19.00 Ilead


  while (Serial.available()) {
    ch = Serial.read();

    _LOG_V_NO_FUNC("%c",ch);

    if (ch == '/') {                                                            // Start character
      CTptr = 0;
      CTeot = 0;                                                                // start from beginning of buffer
    }

    if (CTeot) {                                                                // end of transmission?
      if (CTeot > 4) ch = 0;                                                    // we have also received the CRC, null terminate
      CTeot++;
    }

    CTstring[CTptr] = ch;                                                       // Store in buffer
    if (CTptr < 90) CTptr++;                                                    // prevent overflow of buffer

    if (ch == '!' && CTstring[0] == '/') {
      CTlength = CTptr;                                                         // store pointer of start of CRC
      CTeot = 1;
    }  

    if (CTeot > 5) {
      
      crcdata = (uint16_t) strtol((const char *)CTstring+CTlength, NULL, 16);   // get crc from data, convert to int
      crccal = CRC16(0, CTstring, CTlength);                                    // calculate CRC16 from data
      _LOG_V(" length: %u, CRC16: %04x : %04x\n\r",CTlength, crccal, crcdata);

      if (crcdata == crccal) {

        ret = strstr((const char *)CTstring,(const char *)"1A:");               // Extract CT measurements from the buffer.
        if (ret != NULL) Power1A = atoi((const char *)ret+3);                   
        ret = strstr((const char *)CTstring,(const char *)"1B:");
        if (ret != NULL) Power1B = atoi((const char *)ret+3);
        ret = strstr((const char *)CTstring,(const char *)"2A:");
        if (ret != NULL) Power2A = atoi((const char *)ret+3);
        ret = strstr((const char *)CTstring,(const char *)"2B:");
        if (ret != NULL) Power2B = atoi((const char *)ret+3);
        ret = strstr((const char *)CTstring,(const char *)"3A:");
        if (ret != NULL) Power3A = atoi((const char *)ret+3);
        ret = strstr((const char *)CTstring,(const char *)"3B:");
        if (ret != NULL) Power3B = atoi((const char *)ret+3);
        ret = strstr((const char *)CTstring,(const char *)"SA:");
        if (ret != NULL) Samples = atoi((const char *)ret+3);
        // as we divide by Samples, it can not be 0!
        if (Samples < 1) Samples = 1;

        ret = strstr((const char *)CTstring,(const char *)"WI:");               // CT wire setting 4Wire=0, 3Wire=1 (bit1)
        if (ret != NULL) CTwire = atoi((const char *)ret+3);                    // and phase rotation CW=0, CCW=1 (bit0)
        
        // Irms data when there is no mains plug connected.
        // There is no way of knowing the direction of the current.
        ret = strstr((const char *)CTstring,(const char *)"1R:");
        if (ret != NULL) Power1A = atoi((const char *)ret+3);
        ret = strstr((const char *)CTstring,(const char *)"2R:");
        if (ret != NULL) Power2A = atoi((const char *)ret+3);
        ret = strstr((const char *)CTstring,(const char *)"3R:");
        if (ret != NULL) {
          Power3A = atoi((const char *)ret+3);
        
          IrmsCT[0] = sqrt((float)Power1A / Samples) * CALRMS;
          IrmsCT[1] = sqrt((float)Power2A / Samples) * CALRMS;
          IrmsCT[2] = sqrt((float)Power3A / Samples) * CALRMS;

          // CT Measurement with no current direction information
          IrmsMode = 1; 

        } else {

          // We do have enough data to calculate the Irms and direction of current for each phase
          IrmsCT[0] = (x1 * ((float)Power1A / Samples) + y1 * ((float)Power1B / Samples) ) / CAL;
          IrmsCT[1] = (x2 * ((float)Power2A / Samples) + y2 * ((float)Power2B / Samples) ) / CAL;
          IrmsCT[2] = (x3 * ((float)Power3A / Samples) + y3 * ((float)Power3B / Samples) ) / CAL;

          IrmsMode = 0;      
        }

        phasesLastUpdate = time(NULL);

        // very small values will be displayed as 0.0A
        for (x=0; x<3 ;x++) {
          if ((IrmsCT[x] > -0.05) && (IrmsCT[x] < 0.05)) IrmsCT[x] = 0.0;
        }
        
        // if selected Wire setting (3-Wire or 4-Wire) and CW and CCW phase rotation are not correctly set, we can toggle the PGC pin to set it.
        if ((CTwire != Wire) && IrmsMode == 0) {
          x = (4 + Wire - CTwire) % 4;
          _LOG_A("\nWire:%u CTwire:%u pulses %u\n", Wire, CTwire, x);
          do {
            digitalWrite (PIN_PGC, HIGH); 
            digitalWrite (PIN_PGC, LOW); 
            vTaskDelay(1 / portTICK_PERIOD_MS);
          } while (--x);
        }
        
        // update dataready, so the Master knows the CT's have been read with new data
        dataready |= 0x03;
        datareadyAPI |= 0x03;
        _LOG_V("\rCT1: %2.1f A CT2: %2.1f A CT3: %2.1f A  ",IrmsCT[0],IrmsCT[1],IrmsCT[2] );

      } else {
          _LOG_A("CRC error in CTdata\n");
      }
      
      CTeot = 0;
      CTptr = 0;
      memset(CTstring, 0u, 100u);
    }
  }

}

// ----------------------------------------------------------------------------------------------------------------
// Extracts Voltage and Current measurement data from the P1data buffer.
// updates variables Irms[] and the modbus Ireg registers.
//
void P1Extract() {

  char *ret;
  bool fluvius = false;

  DSMRver = 0;

  ret = strstr((const char *)P1data,(const char *)"/FLU5\\");                 // Belgium Fluvius SMR 5 meter
  if (ret != NULL) fluvius = true;

  ret = strstr((const char *)P1data,(const char *)":0.2.8");                  // DSMR version
  if (ret != NULL) DSMRver = atoi((const char *)ret+7);
  
  // Voltage on the phases (Volt*1)
  ret = strstr((const char *)P1data,(const char *)":32.7.0");                 // Phase 1
  if (ret != NULL) Volts[0] = atof((const char *)ret+8);                      // Some Grid types have 0.0 volts on one of the phases.
  if (Volts[0] < 1.0) Volts[0] = 1;                                           // We divide by Volts later on, so it can not be 0.0
  ret = strstr((const char *)P1data,(const char *)":52.7.0");                 // Phase 2
  if (ret != NULL) Volts[1] = atof((const char *)ret+8);
  if (Volts[1] < 1.0) Volts[1] = 1;
  ret = strstr((const char *)P1data,(const char *)":72.7.0");                 // Phase 3
  if (ret != NULL) Volts[2] = atof((const char *)ret+8);
  if (Volts[2] < 1.0) Volts[2] = 1;
  
  // Power received from Grid (Watt*1)
  ret = strstr((const char *)P1data,(const char *)":21.7.0");                 // Phase 1
  if (ret != NULL) {
      Power[0] = atof((const char *)ret+8)*1000;
      if (DSMRver == 0) DSMRver = 50;                                         // Sagemcom T211-D does not send DSMR version
  }                                                                           // but it does send Power and Volts per phase.        
  ret = strstr((const char *)P1data,(const char *)":41.7.0");                 // Phase 2
  if (ret != NULL) Power[1] = atof((const char *)ret+8)*1000;
  ret = strstr((const char *)P1data,(const char *)":61.7.0");                 // Phase 3
  if (ret != NULL) Power[2] = atof((const char *)ret+8)*1000;
  
  // Power delivered to Grid (Watt*1)
  ret = strstr((const char *)P1data,(const char *)":22.7.0");                 
  if (ret != NULL) PowerReturn[0] = atof((const char *)ret+8)*1000;
  ret = strstr((const char *)P1data,(const char *)":42.7.0");
  if (ret != NULL) PowerReturn[1] = atof((const char *)ret+8)*1000;
  ret = strstr((const char *)P1data,(const char *)":62.7.0");
  if (ret != NULL) PowerReturn[2] = atof((const char *)ret+8)*1000;
  ret = strstr((const char *)P1data,(const char *)":1.7.0");                  // Total Power from Grid
  if (ret != NULL) TotalPower = atof((const char *)ret+7)*1000;
  ret = strstr((const char *)P1data,(const char *)":2.7.0");                  // Total Power delivered to Grid (Watt)
  if (ret != NULL) TotalPowerReturn = atof((const char *)ret+7)*1000;

  // Total Energy
  ret = strstr((const char *)P1data,(const char *)":1.8.1");                  // Energy delivered tariff 1 (kWh)
  if (ret != NULL) EnergyTariff1 = atof((const char *)ret+7);
  ret = strstr((const char *)P1data,(const char *)":1.8.2");                  // Energy delivered tariff 2
  if (ret != NULL) EnergyTariff2 = atof((const char *)ret+7);
  ret = strstr((const char *)P1data,(const char *)":2.8.1");                  // Energy returned tariff 1
  if (ret != NULL) EnergyReturnTariff1 = atof((const char *)ret+7);
  ret = strstr((const char *)P1data,(const char *)":2.8.2");                  // Energy returned tariff 2
  if (ret != NULL) EnergyReturnTariff2 = atof((const char *)ret+7);

  // Gas meter 
  // 0-1:24.2.1(210809124005S)(06415.108*m3)
  // 0-1:24.2.3(210204163500W)(00343.925*m3)
  ret = strstr((const char *)P1data,(const char *)":24.2.1");
  if (ret != NULL) GasDelivered = atof((const char *)ret+23);
  ret = strstr((const char *)P1data,(const char *)":24.2.3");
  if (ret != NULL) GasDelivered = atof((const char *)ret+23);

  // Fluvius meter, but no voltage on phase 2 and 3
  if (fluvius && DSMRver == 0 && Volts[1] == 1 && Volts[2] == 1) {
    // So it's a one phase meter without :21.7.0 data (some models of the Sagemcom S211)
    // we can use the Total power measurements instead.
    Power[0] = TotalPower;
    PowerReturn[0] = TotalPowerReturn;
    DSMRver = 50;
  }

  Irms[0] = (Power[0]-PowerReturn[0])/Volts[0];                              	// Irms (Amps *1)
  Irms[1] = (Power[1]-PowerReturn[1])/Volts[1];
  Irms[2] = (Power[2]-PowerReturn[2])/Volts[2];
  
  phasesLastUpdate = time(NULL);

  if (DSMRver >= 50) {
    dataready |= 0x80;                                     	                  // P1 dataready
    datareadyAPI |= 0x80;
    lockedToP1 = true;
  } else dataready |= 0x40;                                                   // DSMR version not 5.0 !!

  _LOG_V("L1: %3d V L2: %3d V L3: %3d V  ",(int)(Volts[0]),(int)(Volts[1]),(int)(Volts[2]) );
  _LOG_V("L1: %2.1f A L2: %2.1f A L3: %2.1f A  \r\n",Irms[0],Irms[1],Irms[2] );
  _LOG_V("Total Power %5d W  ",(int)(TotalPower-TotalPowerReturn) );
}


// ----------------------------------------------------------------------------------------------------------------
//  Reads P1 data from Serial2, checks crc, and stores in P1data buffer
//  Called by P1Task every 100ms
//
void P1Receive() {
  uint8_t RXbyte;
  static uint16_t P1ptr = 0, P1length = 0;
  static uint8_t P1Eot = 0;
  static unsigned long P1LastMillis = 0;
  

  while (Serial2.available()) {                                                 // Uart2 data available? P1 PORT

    RXbyte = Serial2.read();
    if (RXbyte == '/') {                                                        // Start character
      P1ptr = 0;                                                                // start from beginning of buffer
      P1Eot = 0;                                                                // reset End of Transmission flag
    }
    if (P1Eot) {
      if (P1Eot > 4) break;                                                     // if EOT>4 the CRC16 has been received, and the message can be verified.
      P1Eot++;                                                                  // count bytes after End of transmission
    }
    P1data[P1ptr] = RXbyte;                                                     // Store in buffer
    if (P1ptr < 1990) P1ptr++;                                                  // prevent overflow of buffer, reserve some bytes for CRC

    if (RXbyte == '!' && P1data[0] == '/') {
      P1length = P1ptr;                                                         // store start of CRC.
      P1Eot = 1;                                                                // end character, flag end of transmission

    }
  }

  if (P1Eot > 4) {

    uint16_t csP1 = CRC16(0, P1data, P1length);                                      // calculate CRC16 from data
    uint16_t crcP1 = (uint16_t) strtol((const char *)P1data + P1length, NULL, 16);   // get crc from data, convert to int

    if (millis() > P1LastMillis + 1500) {
        LOG_W("Missed P1 message\n");
    }

    P1LastMillis = millis();

    if (crcP1 == csP1) {                                                        // check if crc's match
      // Send telegram to debug output
      for (uint16_t x=0; x<P1length+4; x++) _LOG_V_NO_FUNC("%c",P1data[x]);
      _LOG_V("\n");

      P1Extract();                                                              // Extract Voltage and Current values from Smart Meter telegram
    } else {
      _LOG_A("P1 crc error, length %d\n", P1length);
      //for (uint16_t x=0;x<P1length;x++) _LOG_A("%c",P1data[x]);
      _LOG_A("\n");
    }
    P1Eot = 0;                                                                  // Mark as processed
    P1ptr = 0;
    // clear P1buffer
    memset(P1data, 0u, 2000u);
  }
}


// ----------------------------------------------------------------------------------------------------------------
// Task that handles incoming P1 and CT data
// Will call P1Receive and CTreceive every 100ms 
//
void P1Task(void * parameter) {
  while(1) {
  
    // Check if there is new P1 data.
    P1Receive();
    if (!heap_caps_check_integrity_all(true)) {
        _LOG_A("\nheap error after P1 receive\n");
    }

    // Check if there is a new measurement from the PIC (CT measurements)
    CTReceive();
    if (!heap_caps_check_integrity_all(true)) {
        _LOG_A("\nheap error after CT receive\n");
    }

    // start wifi portal at powerup
    if (WiFi.status() != WL_CONNECTED && esp_timer_get_time() / 1000000 > 5 && WIFImode != 2 && esp_timer_get_time() / 1000000 < PORTAL_TIMEOUT && WiFiModbusCtrl == false) {
        // if we have no wifi
        // and we are not in the first 5 seconds of startup (to give the existing wifi time to connect and P1 data to be entered)
        // and we are not already in wifimode 2
        // and we are not later then the first 180s after startup; perhaps we are not interested in having a wifi connection?
        // and not overruled by modbus control
        // we go to wifimode 2
        _LOG_A("Starting WiFi portal mode\n");
        WIFImode = 2;
        PortalTimeout = millis();
        handleWIFImode();
    }

    // Auto-exit portal mode after timeout
    if (WIFImode == 2 && (millis() > (PortalTimeout + (PORTAL_TIMEOUT * 1000)) ) ) {
        _LOG_A("Portal timeout, stop WiFi...\n");
        WIFImode = 0;
        write_settings();
        handleWIFImode();
    }

    // keep track of available stack ram
    P1taskram = uxTaskGetStackHighWaterMark( NULL );

    if (!heap_caps_check_integrity_all(true)) {
        _LOG_A("\nheap error after printfAll\n");
    }

    // delay task for 100mS
    vTaskDelay(100 / portTICK_PERIOD_MS);

  } // while(1) loop
}  


// ----------------------------------------------------------------------------------------------------------------
//
// Task BlinkLed
//
void BlinkLed(void * parameter) {

    while(1) {
      
      if (LedState) {                                                               // 8 states

        if ((LedState-- %2) == 0) {
          // check for Orange LED blink
          if (LedSeq[LedCnt] == LED_ORANGE) {
            digitalWrite(PIN_LED_RED,LOW);                                          // LED_RED_ON
            digitalWrite(PIN_LED_GREEN,LOW);                                        // Led green on
          // check for Red LED blink  
          } else if (LedSeq[LedCnt] == LED_RED) {
            digitalWrite(PIN_LED_RED,LOW);                                          // LED_RED_ON
            // blink LED Green
          } else if (LedSeq[LedCnt] == LED_GREEN) {  
            digitalWrite(PIN_LED_GREEN,LOW);                                        // Led green on    
          }
          LedSeq[LedCnt] = LED_OFF; 
          if (LedCnt < 3) LedCnt++;  

        } else {
          digitalWrite(PIN_LED_GREEN,HIGH);                                         // LED_GREEN_OFF;
          digitalWrite(PIN_LED_RED,HIGH);                                           // LED_RED_OFF;
        }
      }

      // keep track of available stack ram
      blinkram = uxTaskGetStackHighWaterMark( NULL );
      // delay task for 200mS
      vTaskDelay(200 / portTICK_PERIOD_MS);
    }
}    





// ----------------------------------------------------------------------------------------------------------------
// callback from Modbus register read
//
ModbusMessage MBReadFC04(ModbusMessage request) {
  ModbusMessage response;     // response message to be sent back
  uint16_t addr = 0, words = 0; 
  uint8_t n, x;
  char *pBytes;
    
  request.get(2, addr);   // 00 00
  request.get(4, words);  // 00 14

  // address valid and length not zero?
  if ((addr + words > 32) || (words == 0)) {
    // No. Return error response
    response.setError(request.getServerID(), request.getFunctionCode(), ILLEGAL_DATA_ADDRESS);
    return response;
  }
  
  // Prepare start of response
  response.add(request.getServerID(), request.getFunctionCode(), (uint8_t)(words * 2));

  // We have valid data. Does not allow switching from P1 back to CT data !
  // A broken P1 connection now creates a comm error instead of (invalid) CT measurements being used.
  if (!((lockedToP1 == true && dataready & 0x80) || (lockedToP1 == false && dataready & 0x03))) {
      dataready = 0;          // Set to 0 to indicate broken P1 connection
  }
  SetLedSequence(dataready);
  ModbusLedTimer = millis();  // Update Modbus LED timer to claim LED control
  
  // Set Modbus Sensorbox version 2.0 (20) + Wire settings.
  // Set Software version in MSB
  ModbusData[0] = (SENSORBOX_SWVER << 8) + SENSORBOX_VERSION + Wire;
  // Set DSMR version + dataready
  ModbusData[1] = (uint16_t)(DSMRver<<8) + (dataready & 0xC3);

  n = 2;
  // Volts P1
  for (x=0; x<3 ;x++) {
    pBytes = (char*)&Volts[x];
    ModbusData[n++] = (uint16_t) (pBytes[3]<<8)+pBytes[2];
    ModbusData[n++] = (uint16_t) (pBytes[1]<<8)+pBytes[0];
  }
  // Current P1
  for (x=0; x<3 ;x++) {
    pBytes = (char*)&Irms[x];
    ModbusData[n++] = (uint16_t) (pBytes[3]<<8)+pBytes[2];
    ModbusData[n++] = (uint16_t) (pBytes[1]<<8)+pBytes[0];
  }
  // Current CT inputs
  for (x=0; x<3 ;x++) {
    pBytes = (char*)&IrmsCT[x];
    ModbusData[n++] = (uint16_t) (pBytes[3]<<8)+pBytes[2];
    ModbusData[n++] = (uint16_t) (pBytes[1]<<8)+pBytes[0];
  }

  bool wifimodeAPSTA = false;
  bool wificonnected = false;
  if (WiFi.getMode() == WIFI_AP_STA) wifimodeAPSTA = true;
  if (WiFi.status() == WL_CONNECTED) wificonnected = true;
  
  ModbusData[n++] = (uint16_t) ((LocalTimeSet << 8) | (wificonnected << 9) | (wifimodeAPSTA << 10)) + WIFImode;
  ModbusData[n++] = (uint16_t) (timeinfo.tm_hour << 8) + timeinfo.tm_min;   // hours since midnight	0-23, minutes after the hour 0-59
  ModbusData[n++] = (uint16_t) (timeinfo.tm_mday << 8) + timeinfo.tm_mon;   // day of the month	1-31, months since January 0-11
  ModbusData[n++] = (uint16_t) (timeinfo.tm_year << 8) + timeinfo.tm_wday;  // years since 1900, days since Sunday 0-6
  IPAddress localIp = WiFi.localIP();
  ModbusData[n++] = (uint16_t) (localIp[0] << 8) + localIp[1];
  ModbusData[n++] = (uint16_t) (localIp[2] << 8) + localIp[3];
  ModbusData[n++] = (uint16_t) (MacId() >> 16);
  ModbusData[n++] = (uint16_t) (MacId() & 0xFFFF);
  ModbusData[n++] = 0x3837; // '8' << 8 | '7'
  ModbusData[n++] = 0x3635; // '6' << 8 | '5'
  ModbusData[n++] = 0x3433; // '4' << 8 | '3'
  ModbusData[n++] = 0x3231; // '2' << 8 | '1'

  dataready = 0;                                                                // reset dataready and DSMRversion
  DSMRver = 0;
	
  if ((millis() - ModbusTimer) > 2500) {
      LOG_W("Missed modbus response\n");
  } 
  ModbusTimer = millis();


  // Loop over all words to be sent
  for (uint16_t i = 0; i < words; i++) {
    // Add word to response buffer
    response.add(ModbusData[addr + i]);
  }
    
  // Return the data response
  return response;
}    


// Write Modbus data FC06
//
// Wire settings @ 0x0800
// Field rotation setting (bit 0)      00000000 0000000x -> 0= Rotation right 1= Rotation Left
// 3/4 wire configuration (bit 1)      00000000 000000x0 -> 0= 4Wire, 1= 3Wire
// 
ModbusMessage MBWriteFC06(ModbusMessage request) {
  ModbusMessage response;     // response message to be sent back

  uint16_t addr, value; 
    
  request.get(2, addr);   // 08 00 (address 0x800)
  request.get(4, value);  // 00 02 (4 wire, rotation right)

  // address valid?
  if (addr == 0x800) {
    // Set Wire bits 
    Wire = value & 3u;
    write_settings();
    return ECHO_RESPONSE;  

  } else if (addr == 0x801) {
    // Set WiFimode
    WIFImode = value & 3u;
    if (WIFImode == 2) PortalTimeout = millis();
    WiFiModbusCtrl = true;
    handleWIFImode();
    write_settings();
    return ECHO_RESPONSE;  
  }
  
  // No. Return error response
  response.setError(request.getServerID(), request.getFunctionCode(), ILLEGAL_DATA_ADDRESS);
  return response;
}


//
// Toggle DE and /RE pins of RS485 transceiver
// (Called by eModbus)
void ToggleRS485(bool level) {

  digitalWrite (PIN_RS485_DE, level);
  digitalWrite (PIN_RS485_RE, level);
}

// ----------------------------------------------------------------------------------------------------------------
// Setup everything
//
//
void setup() {
  
	PicSetup();

  //lower the CPU frequency to 160 MHz
  //setCpuFrequencyMhz(160);

  pinMode(PIN_LED_GREEN, OUTPUT_OPEN_DRAIN);
  pinMode(PIN_LED_RED, OUTPUT_OPEN_DRAIN);

  pinMode (PIN_RS485_RX, INPUT);                                                // RS485 RX input
  pinMode (PIN_RS485_TX, OUTPUT);                                               // RS485 TX output
  pinMode (PIN_RS485_DE, OUTPUT);
  pinMode (PIN_RS485_RE, OUTPUT);
  pinMode (PIN_RX, INPUT);                                                      // P1 data input
  pinMode (PIN_TX, INPUT);                                                      // extra debug output on P1 port (unused)
  digitalWrite (PIN_RS485_RE, LOW);                                             // Read enabled
  
  // Green status LED = firmware 2.1.0
  // Firmware 2.0.x powered on with an Orange status LED
  // Firmware 2.1.3 Red status LED
  digitalWrite(PIN_LED_GREEN, HIGH);                                            // LED Green OFF;
  digitalWrite(PIN_LED_RED, LOW);                                               // LED Red ON;
  delay(500);

  // Setup Serial ports
  Serial.begin(115200, SERIAL_8N1, PIN_PGD, PIN_TXD, false);                    // Input from TX of PIC, and debug output to USB
  Serial1.begin(9600, SERIAL_8N1, PIN_RS485_RX, PIN_RS485_TX, false);           // Modbus connection
  Serial2.setRxBufferSize(2048);                                                // Important! first increase buffer, then setup Uart2
  Serial2.begin(115200, SERIAL_8N1, PIN_RX, -1, true);                          // P1 smartmeter connection, TX pin unused (RX inverted)
  
  while(!Serial); 
  _LOG_A("\nSensorbox 2 powerup\n");

  // Check if the PIC needs updating..
  if (Pic18ReadConfigs() == 0x6980) {
    _LOG_A("PIC18F26K40 found\n");
    size_t fs_size = 0;
    const uint8_t *fs_data = (const uint8_t *)mg_unpack("/data/PIC18F26K40.hex", &fs_size, NULL);

    if (fs_data) {
        _LOG_A("PIC18F26K40.hex found in packed FS\n");
        if (preferences.begin("settings", false)) {
            if (preferences.getUInt("picSWVersion", 0) != SENSORBOX_SWVER) {
                ProgramPIC(fs_data, fs_size);
                preferences.putUInt("picSWVersion", SENSORBOX_SWVER);
            } else _LOG_A("firmware already up to date\n");
            preferences.end();
        }
    } else {
        _LOG_A("PIC18F26K40.hex -not- found\n");
    }

  } else if (Pic16ReadConfigs() == 0x3043 || Pic16ReadConfigs() == 0x3055) {
    _LOG_A("PIC16F1704/5 found\n");
    size_t fs_size = 0;
    const uint8_t *fs_data = (const uint8_t *)mg_unpack("/data/PIC16F1704.hex", &fs_size, NULL);
    
    if (fs_data) {
        _LOG_A("PIC16F1704.hex found in packed FS\n");
        if (preferences.begin("settings", false)) {
            if (preferences.getUInt("picSWVersion", 0) != SENSORBOX_SWVER) {
                ProgramPIC16F(fs_data, fs_size);
                preferences.putUInt("picSWVersion", SENSORBOX_SWVER);
            } else _LOG_A("firmware already up to date\n");
            preferences.end();
        }
    } else {
        _LOG_A("PIC16F1704.hex -not- found\n");
    }
  } else {
      _LOG_A("No PIC found, Not possible to do CT measurements!\n");
  }

  pinMode(PIN_PGD, INPUT);
  pinMode(PIN_PGC, OUTPUT);


  // Read all settings from non volatile memory
  // store default values if uninitialized
  read_settings(true);

  if(!LittleFS.begin(true)) {
        _LOG_A("LittleFS Mount Failed\n");
  }
 
  
  // Setup Modbus Server/Node
  // Set eModbus LogLevel to 1, to suppress possible E5 errors
  MBUlogLvl = LOG_LEVEL_CRITICAL;
  // Register worker. at address '10', function code 03 & 04
  MBserver.registerWorker(10U, READ_HOLD_REGISTER, &MBReadFC04);  
  MBserver.registerWorker(10U, READ_INPUT_REGISTER, &MBReadFC04);
  // Register worker for address '10', function code 06
  MBserver.registerWorker(10U, WRITE_HOLD_REGISTER, &MBWriteFC06);
  // Start ModbusRTU Node background task
  MBserver.begin(Serial1);
  
  
  // Create Task that handles P1 and CT data
  xTaskCreate(
    P1Task,               // Function that should be called
    "P1Task",             // Name of the task (for debugging)
    3000,                 // Stack size (bytes) 
    NULL,                 // Parameter to pass
    5,                    // Task priority - high
    NULL                  // Task handle
  );

// Create Task that blinks the LED, and cleans up network clients
  xTaskCreate(
    BlinkLed,             // Function that should be called
    "BlinkLed",           // Name of the task (for debugging)
    2000,                 // Stack size (bytes)
    NULL,                 // Parameter to pass
    1,                    // Task priority - low
    NULL                  // Task handle
  );

// Create Task Second Timer (2000ms)
  xTaskCreate(
    Timer2S,        // Function that should be called
    "Timer2S",      // Name of the task (for debugging)
    4096,           // Stack size (bytes)
    NULL,           // Parameter to pass
    4,              // Task priority - medium/high
    NULL  // Task handle
  );

  _LOG_A("Configuring WDT...\n");
  esp_task_wdt_init(WDT_TIMEOUT, true);     // Setup watchdog
  esp_task_wdt_add(NULL);                   // add current thread to WDT watch

  WiFiSetup();

}


#if MQTT
void mqtt_receive_callback(const String topic, const String payload) {
    // Make sure MQTT updates directly to prevent debounces
    lastMqttUpdate = 10;    // TODO unused
}


void SetupMQTTClient() {
    // Set up subscriptions
    MQTTclient.subscribe(MQTTprefix + "/Set/#",1);
    MQTTclient.publish(MQTTprefix+"/connected", "online", true, 0);

    //set the parameters for and announce sensors with device class 'current':
    String optional_payload = MQTTclient.jsna("device_class","current") + MQTTclient.jsna("state_class","measurement") + MQTTclient.jsna("unit_of_measurement","A") + MQTTclient.jsna("value_template", R"({{ value | int / 10 }})");
    MQTTclient.announce("Mains Current L1", "sensor", optional_payload);
    MQTTclient.announce("Mains Current L2", "sensor", optional_payload);
    MQTTclient.announce("Mains Current L3", "sensor", optional_payload);
    MQTTclient.announce("CT Current L1", "sensor", optional_payload);
    MQTTclient.announce("CT Current L2", "sensor", optional_payload);
    MQTTclient.announce("CT Current L3", "sensor", optional_payload);

    if (lockedToP1) { //we don't have volts from CT
        optional_payload = MQTTclient.jsna("device_class","voltage") + MQTTclient.jsna("state_class","measurement") + MQTTclient.jsna("unit_of_measurement","V") + MQTTclient.jsna("value_template", R"({{ value | float | round(1) }})");
        MQTTclient.announce("Mains Voltage L1", "sensor", optional_payload);
        MQTTclient.announce("Mains Voltage L2", "sensor", optional_payload);
        MQTTclient.announce("Mains Voltage L3", "sensor", optional_payload);

        // Power sensors (Watt)
        optional_payload = MQTTclient.jsna("device_class","power") + MQTTclient.jsna("state_class","measurement") + MQTTclient.jsna("unit_of_measurement","W");
        MQTTclient.announce("Power Delivered L1", "sensor", optional_payload);
        MQTTclient.announce("Power Delivered L2", "sensor", optional_payload);
        MQTTclient.announce("Power Delivered L3", "sensor", optional_payload);
        MQTTclient.announce("Power Returned L1", "sensor", optional_payload);
        MQTTclient.announce("Power Returned L2", "sensor", optional_payload);
        MQTTclient.announce("Power Returned L3", "sensor", optional_payload);
        MQTTclient.announce("Power Delivered", "sensor", optional_payload);
        MQTTclient.announce("Power Returned", "sensor", optional_payload);

        // Energy sensors (kWh)
        optional_payload = MQTTclient.jsna("device_class","energy") + MQTTclient.jsna("state_class","total_increasing") + MQTTclient.jsna("unit_of_measurement","kWh");
        MQTTclient.announce("Energy Delivered Tariff1", "sensor", optional_payload);
        MQTTclient.announce("Energy Delivered Tariff2", "sensor", optional_payload);
        MQTTclient.announce("Energy Returned Tariff1", "sensor", optional_payload);
        MQTTclient.announce("Energy Returned Tariff2", "sensor", optional_payload);

        // Gas sensor (m³)
        optional_payload = MQTTclient.jsna("device_class","gas") + MQTTclient.jsna("state_class","total_increasing") + MQTTclient.jsna("unit_of_measurement","m³");
        MQTTclient.announce("Gas Delivered", "sensor", optional_payload);
    }

    //set the parameters for and announce diagnostic sensor entities:
    optional_payload = MQTTclient.jsna("entity_category","diagnostic");
    MQTTclient.announce("WiFi SSID", "sensor", optional_payload);
    MQTTclient.announce("WiFi BSSID", "sensor", optional_payload);
    optional_payload = MQTTclient.jsna("entity_category","diagnostic") + MQTTclient.jsna("device_class","signal_strength") + MQTTclient.jsna("unit_of_measurement","dBm") + MQTTclient.jsna("state_class","measurement");
    MQTTclient.announce("WiFi RSSI", "sensor", optional_payload);
    optional_payload = MQTTclient.jsna("entity_category","diagnostic") + MQTTclient.jsna("device_class","duration") + MQTTclient.jsna("unit_of_measurement","s") + MQTTclient.jsna("state_class","measurement") + MQTTclient.jsna("entity_registry_enabled_default","False");
    MQTTclient.announce("ESP Uptime", "sensor", optional_payload);

    MQTTclient.publish(MQTTprefix + "/WiFiSSID", String(WiFi.SSID()), true, 0);
    MQTTclient.publish(MQTTprefix + "/WiFiBSSID", String(WiFi.BSSIDstr()), true, 0);
}
#endif //MQTT

//make mongoose 7.14 compatible with 7.13
#define mg_http_match_uri(X,Y) mg_match(X->uri, mg_str(Y), NULL)

// handles URI, returns true if handled, false if not
bool handle_URI(struct mg_connection *c, struct mg_http_message *hm,  webServerRequest* request) {
//    if (mg_match(hm->uri, mg_str("/settings"), NULL)) {               // REST API call?
    if (mg_http_match_uri(hm, "/settings")) {                            // REST API call?
          if (!memcmp("GET", hm->method.buf, hm->method.len)) {                     // if GET
/*            String mode = "N/A";
            int modeId = -1;
            if(Access_bit == 0)  {
                mode = "OFF";
                modeId=0;
            } else {
                switch(Mode) {
                    case MODE_NORMAL: mode = "NORMAL"; modeId=1; break;
                    case MODE_SOLAR: mode = "SOLAR"; modeId=2; break;
                    case MODE_SMART: mode = "SMART"; modeId=3; break;
                }
            }
            String backlight = "N/A";
            switch(BacklightSet) {
                case 0: backlight = "OFF"; break;
                case 1: backlight = "ON"; break;
                case 2: backlight = "DIMMED"; break;
            }
            String evstate = StrStateNameWeb[State];
            String error = getErrorNameWeb(ErrorFlags);
            int errorId = getErrorId(ErrorFlags);

            if (ErrorFlags & NO_SUN) {
                evstate += " - " + error;
                error = "None";
                errorId = 0;
            }

            boolean evConnected = pilot != PILOT_12V;                    //when access bit = 1, p.ex. in OFF mode, the STATEs are no longer updated
*/
            DynamicJsonDocument doc(1600); // https://arduinojson.org/v6/assistant/
            doc["version"] = String(VERSION);
            doc["serialnr"] = serialnr;
            doc["smartevse_host"] = SmartEVSEHost;

            if(WiFi.isConnected()) {
                switch(WiFi.status()) {
                    case WL_NO_SHIELD:          doc["wifi"]["status"] = "WL_NO_SHIELD"; break;
                    case WL_IDLE_STATUS:        doc["wifi"]["status"] = "WL_IDLE_STATUS"; break;
                    case WL_NO_SSID_AVAIL:      doc["wifi"]["status"] = "WL_NO_SSID_AVAIL"; break;
                    case WL_SCAN_COMPLETED:     doc["wifi"]["status"] = "WL_SCAN_COMPLETED"; break;
                    case WL_CONNECTED:          doc["wifi"]["status"] = "WL_CONNECTED"; break;
                    case WL_CONNECT_FAILED:     doc["wifi"]["status"] = "WL_CONNECT_FAILED"; break;
                    case WL_CONNECTION_LOST:    doc["wifi"]["status"] = "WL_CONNECTION_LOST"; break;
                    case WL_DISCONNECTED:       doc["wifi"]["status"] = "WL_DISCONNECTED"; break;
                    default:                    doc["wifi"]["status"] = "UNKNOWN"; break;
                }

                doc["wifi"]["ssid"] = WiFi.SSID();
                doc["wifi"]["rssi"] = WiFi.RSSI();
                doc["wifi"]["bssid"] = WiFi.BSSIDstr();
            }

            // stuff to keep compatibility with SmartEVSE
            doc["settings"]["mains_meter"] = "Sensorbox";
            doc["evse"]["temp"] = 0;
            //doc["evse"]["temp"] = TempEVSE; TODO
            doc["ev_meter"]["description"] = "Disabled"; //TODO
            doc["ev_meter"]["currents"]["TOTAL"] = 0; //TODO
            doc["ev_meter"]["currents"]["L1"] = 0;
            doc["ev_meter"]["currents"]["L2"] = 0;
            doc["ev_meter"]["currents"]["L3"] = 0;


    #if MQTT
            doc["mqtt"]["host"] = MQTTHost;
            doc["mqtt"]["port"] = MQTTPort;
            doc["mqtt"]["topic_prefix"] = MQTTprefix;
            doc["mqtt"]["username"] = MQTTuser;
            doc["mqtt"]["password_set"] = MQTTpassword != "";
            doc["mqtt"]["tls"] = MQTTtls;
            if (MQTTclient.connected) {
                doc["mqtt"]["status"] = "Connected";
            } else {
                doc["mqtt"]["status"] = "Disconnected";
            }
    #endif
  /*
            doc["ev_meter"]["description"] = EMConfig[EVMeter.Type].Desc;
            doc["ev_meter"]["address"] = EVMeter.Address;
            doc["ev_meter"]["import_active_power"] = round((float)EVMeter.PowerMeasured / 100)/10; //in kW, precision 1 decimal
            doc["ev_meter"]["total_kwh"] = round((float)EVMeter.Energy / 100)/10; //in kWh, precision 1 decimal
            doc["ev_meter"]["charged_kwh"] = round((float)EVMeter.EnergyCharged / 100)/10; //in kWh, precision 1 decimal
            doc["ev_meter"]["currents"]["TOTAL"] = EVMeter.Irms[0] + EVMeter.Irms[1] + EVMeter.Irms[2];
            doc["ev_meter"]["currents"]["L1"] = EVMeter.Irms[0];
            doc["ev_meter"]["currents"]["L2"] = EVMeter.Irms[1];
            doc["ev_meter"]["currents"]["L3"] = EVMeter.Irms[2];
            doc["ev_meter"]["import_active_energy"] = round((float)EVMeter.Import_active_energy / 100)/10; //in kWh, precision 1 decimal
            doc["ev_meter"]["export_active_energy"] = round((float)EVMeter.Export_active_energy / 100)/10; //in kWh, precision 1 decimal

            doc["mains_meter"]["import_active_energy"] = round((float)MainsMeter.Import_active_energy / 100)/10; //in kWh, precision 1 decimal
            doc["mains_meter"]["export_active_energy"] = round((float)MainsMeter.Export_active_energy / 100)/10; //in kWh, precision 1 decimal
   */
            doc["phase_currents"]["TOTAL"] = (MainsMeterIrms[0] + MainsMeterIrms[1] + MainsMeterIrms[2]);
            doc["phase_currents"]["L1"] = MainsMeterIrms[0];
            doc["phase_currents"]["L2"] = MainsMeterIrms[1];
            doc["phase_currents"]["L3"] = MainsMeterIrms[2];
            doc["phase_currents"]["last_data_update"] = phasesLastUpdate;

            String json;
            serializeJson(doc, json);
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\n", json.c_str());    // Yes. Respond JSON
            return true;
          } else if (!memcmp("POST", hm->method.buf, hm->method.len)) {
            if (request->hasParam("upload_update") && request->getParam("upload_update")->value().toInt() == 1) {

                if(request->hasParam("smartevse_host")) {
                    SmartEVSEHost = request->getParam("smartevse_host")->value();
                    DynamicJsonDocument doc(64);
                    doc["smartevse_host"] = SmartEVSEHost;
                    if (preferences.begin("settings", false) ) {
                        preferences.putString("SmartEVSEHost", SmartEVSEHost);
                        preferences.end();
                    }
                    String json;
                    serializeJson(doc, json);
                    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\r\n", json.c_str());    // Yes. Respond JSON
                    return true;
                }
            }
          }
    }
    return false;
}

//
// This code will run forever
//
void loop() {
    network_loop();
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck >= 1000) {
        lastCheck = millis();
        //this block is for non-time critical stuff that needs to run approx 1 / second
        // reset the WDT every second
        esp_task_wdt_reset();

        static uint8_t RebootDelay = 5;      
        if (shouldReboot) {
            if (RebootDelay-- == 0) {                                           //give user some time to read any message on the webserver
                ESP.restart();                                                  //use non-blocking code so network_loop() keeps working.
            }
        }
        //_LOG_A("Status: %04x Time: %02u:%02u Date: %02u/%02u/%02u Day:%u ", WIFImode + (LocalTimeSet << 8), timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_mday, timeinfo.tm_mon+1, timeinfo.tm_year%100, timeinfo.tm_wday);
        //_LOG_A("Connected to AP: %s Local IP: %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    }
    //vTaskDelay(1000 / portTICK_PERIOD_MS);
}
