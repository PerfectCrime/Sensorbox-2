# How to Configure WiFi
* WiFi is used to control your Sensorbox through the built-in web server, (optionally) to export its measurements to an MQTT broker, and (optionally) to send its measurements to the SmartEVSE. It is preferred to send the measurements over Modbus RS485 to the SmartEVSE for reliability reasons.

## What software version does my Sensorbox have?
The version can be determined by looking at the Status LED while powering up the Sensorbox.
- Orange: Software v2.0.2 (flashed by default up to Jan-2026)
- Green: Software v2.1.0 - v2.1.2 
- Red: Software v2.1.3 (latest firmware, flashed from Feb-2026)

### Sensorbox with software v2.0.2 or v2.1.3
The Sensorbox can be connected to WiFi using the menu of the SmartEVSE.

* Connect the Sensorbox to the SmartEVSE (wired connection: A, B, 12V and GND terminals)
  - On the SmartEVSE choose `Sensorbox` as **MAINSMET**
  - Press the right button twice; you should see the **SB WIFI** menu option
  - There are three options:
    - `Disabled`: Sensorbox WiFi is OFF
    - `Enabled`: Sensorbox WiFi is active; the IP address is visible on the top row of the LCD.
    - `SetupWiFi`: Start Portal, use the password shown on the LCD to connect to the portal. The name of the portal is SmartEVSE-xxxxx (or Sensorbox-config). Once connected, go to http://192.168.4.1 to configure your own WiFi network.
    When you save your settings, the Sensorbox will reboot, and the SmartEVSE will exit the menu. You can go back into the menu to read the IP address of the Sensorbox.

* For software v2.1.3, there is another way to connect to WiFi:
  - Unplug all cables connected to the Sensorbox.
  - Only connect the green plug (or micro-USB if you want to power it that way).
  - The status LED will blink RED (indicating v2.1.3). After a few seconds it should start blinking Orange - Red.
  - You now have 3 minutes to set up WiFi. After that time, the portal stops, and the status LED will blink Orange only.
  - Scan for networks on a phone, connect to `Sensorbox-config`, enter the password `12345678`.
  - Once connected, open a browser and go to http://192.168.4.1 to configure your own WiFi network.
  - Save your settings. The Sensorbox will reboot and connect to your WiFi. The status LED will blink Orange - Green if connected successfully.
  
### Sensorbox with Software v2.1.0 or v2.1.1
The Sensorbox uses the ESPTouch app on your phone to connect to WiFi.

* In order to configure WiFi, the following conditions have to be met:
  - The device has to be DISCONNECTED from any WiFi network. If you want to reconfigure an existing WiFi connection, use /erasesettings to wipe out the existing WiFi configuration.
  - The P1 input has to be disconnected.
  - We are in the first 180 seconds after startup of the device. After this period, the device assumes you don't want to configure WiFi.

* If the above conditions are met, you can configure the WiFi as follows:
  - Connect your smartphone to the WiFi network you want your Sensorbox connected to.
  - Download and run the ESPTouch app from your favourite app store:
  [Android](https://play.google.com/store/apps/details?id=com.fyent.esptouch.android&hl=en_US)
  (please ignore the strange Author name) or
  [Apple](https://apps.apple.com/us/app/espressif-esptouch/id1071176700) or
  [Github](https://github.com/EspressifApp/EsptouchForAndroid) (for source code).
  - Choose EspTouch V2.
  - Fill in the password of the WiFi network.
  - Fill in "1" in device count for provisioning.
  - Fill in `0123456789abcdef` in the AES Key field.
  - Leave Custom Data empty.
  - Press "Confirm". Within 30 seconds the app will confirm a MAC address and an IP address.
  You are connected now. If you want special configurations (static IP address, special DNS address), configure them on your AP/router.

* If you don't get it to work with the ESPTouch app, there is a backup procedure:
  - Make your Sensorbox powerless, so it will be off.
  - Connect your Sensorbox with a USB cable to your PC; it will be powered through your USB port now.
  - Install the USB driver (Windows) or not (Linux) for WCH CH340 chipset.
  - Connect your favourite serial terminal to the appropriate port.
  - Use the following settings: 115200bps, 8 bits, no parity, 1 stopbit.
  - Make sure the status LED blinks two or four times; the last blink should be Red. This indicates it's waiting for WiFi configuration.
  - Press enter on your serial terminal; you will be prompted for your WiFi network name and password.
  - The Sensorbox should now connect to WiFi.
  - Its IP address is displayed on your terminal.
 
### Sensorbox with Software v2.1.2
* In order to configure the WiFi, the following conditions have to be met:
  - The device has to be DISCONNECTED from any WiFi network. If you want to reconfigure an existing WiFi connection, use /erasesettings to wipe out the existing WiFi configuration.
  - The P1 input has to be disconnected.
  - We are in the first 180 seconds after startup of the device. After this period, the device assumes you don't want to configure WiFi.

* If the above conditions are met, you can configure the WiFi as follows:
  - On your smartphone, turn your WiFi off and on so it starts scanning for new WiFi networks.
  - Connect to AP `Sensorbox-config` and enter password `0123456789abcdef`.
  - Browse to http://192.168.4.1, and fill in the SSID and password of your WiFi network.
  You are connected now. If you want special configurations (static IP address, special DNS address), configure them on your AP/router.

# Web Server
After configuration of your WiFi parameters, your Sensorbox will present itself on your LAN via a web server. This web server can be accessed through:
* http://ip-address/ where "ip-address" is the IP address of the Sensorbox.
* http://sensorbox-xxxx.local/ where xxxx is the serial number of your Sensorbox. It might be necessary that mDNS is configured on your LAN.
* http://sensorbox-xxxx.lan/ where xxxx is the serial number of your Sensorbox. It might be necessary that mDNS is configured on your LAN.
* OTA update of your firmware:
    - Surf to http://ip-address/update or press the UPDATE button on the web server.
    - Select the firmware.bin from this archive, OR if you want the debug version (via telnet over your WiFi), select firmware.debug.bin.
    - After OK, wait 10-30 seconds and your new firmware including the web server should be online!
* Added WiFi debugging: If you flashed the debug version, telnet http://ip-address/ will bring you to a debugger that shows you what's going on!

# REST API

For the specification of the REST API, see [REST API](REST_API.md)

# MQTT API
Your Sensorbox can now export the most important data to your MQTT server. Just fill in the configuration data on the web server and the data will automatically be announced to your MQTT server. 

You can easily show all the MQTT topics published:
```
mosquitto_sub -v -h ip-of-mosquitto-server -u username -P password  -t '#'
```
If you prefer a nice GUI, try [MQTT Explorer](https://mqtt-explorer.com/)

# Building the Firmware
You can get the latest release from https://github.com/dingo35/Sensorbox-2/releases, but if you want to build it yourself:
* Install platformio-core https://docs.platformio.org/en/latest/core/installation/methods/index.html
* Clone this GitHub project, cd to the `Sensorbox2_ESP` directory where platformio.ini is located.
* Compile firmware.bin: platformio run

If you are not using the web server /update endpoint:
* Windows users: Install USB drivers https://wch-ic.com/downloads/CH341SER_EXE.html
* Upload via USB configured in platformio.ini: platformio run --target upload

# I think I bricked my Sensorbox
Luckily for you, there are no known instances of people who actually bricked their Sensorbox.
But if all else fails, connect your Sensorbox via USB to your laptop and use the following web USB flasher:<br>
https://esp.huhn.me/ <br>
Note that the USB connector on the Sensorbox-2 is quite fragile. Don't use too much force when plugging in the micro-USB plug.

Flash the firmware.bin to both partitions, 0x10000 and 0x1c0000

