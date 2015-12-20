# 3D Printer (RAMPS) WiFi mod

This package contains three separate pieces that I use to add WiFi to my 3D
Printer.  I have only tested this on my Prusa i3v with RAMPS v1.4, so YMMV. Use
at your own risk.

I'll talk about the individual pieces below, including the requirements in each
section.  In each section, I'll explain how to build that project.

The first thing to do is to make sure you've synced all the code. After cloning
this project, do the following:

```
git submodule init
git submodule update
cd AndroidApp
git submodule init
git submodule update
cd ..
```

## Step 1. RAMPS firmware (Marlin)

In the Marlin directory lives the RAMPS firmware, branched from a Release version in August 2005.  I've modified it to communicate with the [Adafruit HUZZAH ESP8266 Breakout](https://www.adafruit.com/products/2471) via Serial.  The RAMPS v1.4 board and the Marlin firmware, by default, claimed the extra Serial pins (the RAMPS v1.4 board has multiple Serial RX/TX lines), and so I had to move one of the endstop pins to another location.  The wiring information is described later.

Since this is a Marlin firmware, you will need to modify Configuration.h and pins.h to suit your printer.  Don't blame me if you don't do this.  **BE SURE TO CHECK THE CONFIGURATION FILES BEFORE USING THIS FIRMWARE.**

Building this project is simple.  Open Marlin/Marlin/Marlin.ino in the Arduino IDE and build it.  The RAMPS v1.4 board uses the ATmega2560 AVR.

## Step 2. ESP8266 Firmware

In the ESP8266_Server directory lies another Arduino project.  It is the firmware that must be downloaded to the ESP8266 and it's job is to communicate with the Marlin firmware, sending commands and receiving updates.

This firmware listens on the local network for broadcast messages and will reply with its IP address. It also runs an HTTP server for communication with  other applications.  In order to connect, you must set the two defines at the top of the file:

```c
#define WIFI_NAME "your_wifi_name_here"
#define WIFI_PASSWORD "your_wifi_password_here"
```

You can also define the PC_SERIAL line if you would like to debug the HTTP server via PC, but with this define set it the program will not be able to communicate with Marlin.  Be sure to undefine PC_SERIAL when putting the ESP8266 to actual use.

This project requires a copy of MarlinWifi.h in order to build, so execute:

```
copy Marlin\Marlin\MarlinWiFi.h ESP8266_Server\
```

on Windows, and

```
cp Marlin/Marlin/MarlinWiFi.h ESP8266_Server/
```

on Linux before opening the ESP8266_Server.ino file in the Arduino IDE.

You will need to have the Adafruit HUZZAH modules installed from the https://github.com/esp8266/Arduino repository before you can build. You should then be able to build and download the code onto the module.  Once powered, the firmware will connect to WiFi and start an http server.

If you know the IP address of the module, you can connect to http://esp8266_ip_address/status to see if it's responding.


## Step 3. Android Application

<img src="https://github.com/sarchar/3DPrinterWiFi/blob/master/docs/1.png" width="300px"> <img src="https://github.com/sarchar/3DPrinterWiFi/blob/master/docs/2.png" width="300px">

**Note**: I've included a prebuilt APK if you choose not to build it yourself.  If you want to build the android application, read on.

This project is a bit harder to build than the last two.  I've only ever built this project on Linux, so if you're using another OS...good luck!

The first thing you'll need to do is get a copy of the support libraries.  The AndroidApp requires `android-support-v7-appcompat.jar` and the [PagerSlidingTabStrip](https://github.com/astuetz/PagerSlidingTabStrip) project requires `android-support-v4.jar`:

```
cp $ANDROID_SDK/extras/android/support/v7/appcompat/libs/android-support-v7-appcompat.jar AndroidApp/libs
mkdir AndroidApp/PagerSlidingTabStrip/library/libs
cp $ANDROID_SDK/extras/android/support/v7/appcompat/libs/android-support-v4.jar AndroidApp/PagerSlidingTabStrip/library/libs
```

Next, you need to create the local project files for building:

```
cd AndroidApp
android update project -p .
```

And create the project files for `PagerSlidingTabStrip`:

```
cd PagerSlidingTabStrip/library
android create lib-project --target android-23 --path . \
	--package com.astuetz.pagerslidingtabstrip \
   	--name PagerSlidingTabStrip
cd ../..
```

If it isn't obvious at this point, you will need to have the Android 6.0 SDK installed.  You can try other targets, but I haven't.

Then you should be able to build:

```
ant debug
ant install
```

## Step 4. Wiring

After all of the above is done, you'll need to wire up your ESP8266 and connect it to the RAMPS board.  The Huzzah ESP8266 is powered on 3.3V logic, and most the pins are not 5V safe, but we are lucky because one of the few 5V-safe pins is the RX line, so we won't need any level shifting on the serial lines.  The Huzzah board will also step down 5V power to it's 3.3V required levels.  So, we have four pins to connect: power, ground, rx, and tx.

For wiring, it will make it easy if you have the following diagrams handy:

* Huzzah pinout: https://learn.adafruit.com/adafruit-huzzah-esp8266-breakout/pinouts
* RAMPS pinout: http://reprap.org/wiki/File:Arduinomega1-4connectors.png

**Z end stop** - Normally, this is on the D18 pin.  Shift it down one row to the D15 pin.  Double check pins.h in Marlin firmware that this matches it.

**Ground and 5V** - From the RAMPS board, the I2C socket right above the endstops block is where I connect ground and 5V.  In the RAMPS pinout, you can see the GND/5V pins in the top-right corner.  On the Huzzah breakout, I connect GND and 5V to GND and the "V+" pins.

**TX/RX** - The serial interface on the RAMPS board uses pins D18 and D19.  These pins are right above where the endstop pins connect to RAMPS. D18 is TX and D19 is RX.  Therefore, D18 from RAMPS will connect to the pin labeled RX on the Huzzah ESP8266.  D19 on RAMPS will connect to the pin labeled TX on the Huzzah ESP8266.  No other components are necessary for the connection.


## Conclusion

That really should be all that's necessary to getting the code running.  If you make changes or find errors, let me know!  Obviously, you'll need
a router with DHCP, supports multicast, and have an Android phone that can run the required SDK.

## License

See the LICENSE file for the license that these projects are licensed under.  The Marlin firmware has its own license and the LICENSE in this project only applies to the Android Application and the ESP8266 firmware, since that code is written by me.













