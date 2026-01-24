# Sensorbox-2
Sensorbox-2 measures the current (A) for up to three phases with CTs and a MAINS voltage input.
It can also receive these measurements directly from a DSMR 5.x smart meter's P1 port.
These measurements are then sent every two seconds to SmartEVSE controllers, either wired over RS485, or over WiFi.

**Installation Location:**
The Sensorbox-2 should be installed inside your electrical cabinet, positioned next to the MAINS meter. This location is essential to:
- **Connect Current Transformers (CTs)**: Position the Sensorbox near where CT connections can be made to your electrical phases.
- **Connect to Smart Meter**: If your Smart Meter has a DSMR 5.x P1 port, install the Sensorbox nearby and connect via the supplied P1 cable, allowing direct data extraction from the meter.

The Sensorbox's wired RS485 connection to SmartEVSE can be extended up to 100 meters if needed. A wired connection is preferred for reliable charging.<br>
Wireless connectivity to the SmartEVSE is also possible, but requires a stable WiFi signal. WiFi instability may cause communication errors that could affect charging. Use WiFi if a stable connection can be guaranteed, and a wired connection is not feasable.

The latest software v2.1.3 will send the following P1 meter data to a configurable MQTT server:
- Voltage per Phase
- Current per Phase
- Power per Phase Delivered
- Power per Phase Returned
- Total Power Delivered
- Total Power Returned
- Energy Delivered Tariff 1
- Energy Returned Tariff 1
- Energy Delivered Tariff 2
- Energy Returned Tariff 2
- Gas meter

See [configuration](docs/configuration.md) for more on MQTT.

## Connecting to WiFi (and updating firmware)

To upgrade your Sensorbox-2 firmware, first make sure your SmartEVSE v3 firmware is v3.7.2 or higher.

The Sensorbox should be wired to the SmartEVSE, using wires A, B, 12V and GND.
Once your SmartEVSE v3 detects your Sensorbox, it will present the LCD screen with a new option: **SB2 WIFI**, which is set to `Disabled` by default.
Select `SetupWiFi` here, and the Sensorbox will present itself as a WiFi Access Point "smartevse-xxxx" or "Sensorbox-xxxx"; the password is shown on the LCD screen.
Connect with your phone to that access point, go to http://192.168.4.1/ and enter your local WiFi credentials.

```
Please note that if you don't see the 'SB2 WIFI' option it is possible you have a Sensorbox hardware version 1.0.6 with an even older firmware. 
You can check the hardware version by removing the P1 cable from the sensorbox, and looking inside. The version is printed on the circuit board.
In this case you will need to flash the firmware with a USB cable.
```

Once the Sensorbox is connected to your WiFi, you can find its IP address on the SmartEVSE's **SB2 WIFI** menu option.
Browse to this IP address, and you will be presented with the Sensorbox2's WiFi status page.<br>

If you want to upgrade your Sensorbox2's firmware, browse to http://ip-address/update where "ip-address" is the IP address of the Sensorbox.<br>
Proceed by uploading the firmware.bin file.

On firmware version 2.1.0 or newer the procedure to configure WiFi has changed:<br>
See [configuration](docs/configuration.md)

## Status LED

The status LED sequence depends on the working mode of the Sensorbox:

1 blink:
- Green: P1 measurement. Orange: CT measurement (no direction of currents). Red: P1 Comm Error 

2 blinks:
- Green: P1 measurement. Orange: CT measurement (no direction of currents). Red: P1 Comm Error
- Green: connected to WiFi. Red: Portal active, waiting to connect 

3 blinks:
1. Phase L1: Green = export, Orange = import
2. Phase L2: Green = export, Orange = import
3. Phase L3: Green = export, Orange = import

4 blinks:
1. Phase L1: Green = export, Orange = import
2. Phase L2: Green = export, Orange = import
3. Phase L3: Green = export, Orange = import
4. Green: connected to WiFi, Red: Portal active, waiting to connect




## Modbus Registers

Modbus RTU address is fixed to 0x0A, speed is 9600 bps 

   
Input Registers (FC=04)(Read Only):

    Register  Register  
     Address   length (16 bits)
     0x0000      1       Sensorbox version 2             = 0x0114 (2 lsb mirror the 3/4 Wire and Rotation configuration data)
                                                           0x0115 = 4 wire, CCW rotation
                                                           0x0116 = 3 wire, CW rotation      
                                                           0x0117 = 3 wire, CCW rotation  
     0x0001      1       DSMR Version(MSB),CT's or P1(LSB) 0x3283 = DSMR version 50, P1 port connected (0x80) CT's Used (0x03)                    
     0x0002      2       Volts L1 (32 bit floating point), Smartmeter P1 data
     0x0004      2       Volts L2 (32 bit floating point), Smartmeter P1 data
     0x0006      2       Volts L3 (32 bit floating point), Smartmeter P1 data
     0x0008      2       Amps L1 (32 bit floating point), Smartmeter P1 data
     0x000A      2       Amps L2 (32 bit floating point), Smartmeter P1 data
     0x000C      2       Amps L3 (32 bit floating point), Smartmeter P1 data
     0x000E      2       Amps L1 (32 bit floating point), CT input 1
     0x0010      2       Amps L2 (32 bit floating point), CT input 2
     0x0012      2       Amps L3 (32 bit floating point), CT input 3
     
     with sensorbox software version >= 0x01xx, the following extra registers are available

     0x0014      1       WiFi Connection Status  xxxxxACL xxxxxxWW = WiFi mode (00=WiFi Off, 01=On, 10=portal started)
                                                      ||\_ Local Time Set
                                                      |\__ Connected to WiFi
                                                      \___ AP_STA mode (portal) active
     0x0015      1       Time Hour(msb) Minute(lsb)
     0x0016      1       Time Month(msb) Day(lsb)
     0x0017      1       Time Year(msb) Weekday(lsb)
     0x0018      2       IP address Sensorbox
     0x001A      2       MAC Sensorbox (4 LSB bytes) http://SmartEVSE-012345.local can be derived from this
     0x001C      4       Password portal (8 bytes)
 
    
    Holding Registers (FC=06)(Write):
    
     Register  Register  
     Address   length (16 bits) 
     0x0800      1       Field rotation setting (bit 0)      00000000 0000000x -> 0= Rotation right 1= Rotation Left
                         3/4 wire configuration (bit 1)      00000000 000000x0 -> 0= 4Wire, 1= 3Wire
     0x0801      1       Set WiFi mode                       00000000 000000xx -> 00 = disabled, 01 = enabled, 02 = start portal.   

## Example:

    Request Sensorbox2 data:
    0a 04 00 00 00 14 f1 7e
    
    Sensorbox2 replies with:
    0a 04 28 00 14 32 83 43 66 80 00 43 66 4c cd 43 69 19 9a 3e 12 9a 61 3f 9f 83 80 be 41 4a 5a 3b ea 00 3b bb 6f f3 6b 3a 45 8d e3 ca cb
    4 wire CW --^^
    DSMR ver(50) --^^
    P1 + CT connected ^^
    Volts L1 ------------| 230.5 V  |			 
    					 
    Request 3 wire grid setting:
    0a 06 08 00 00 02 0b 10		
    
    Next sensorbox2 data:
    0a 04 28 00 16 32 03 43 66 e6 66 43 66 80 00 43 69 19 9a 3d 66 9c 53 3f 97 99 d2 be a2 8a 28 bb 59 73 10 bb 08 29 4d 3b 8f 23 79 ca e6 			 
    ------------^^ changed to 3 wire, CW setting.






