# ESP32-S3 USB Web Radio

![Platform](https://img.shields.io/badge/platform-ESP32--S3-blue)
![Framework](https://img.shields.io/badge/framework-Arduino-green)
![License](https://img.shields.io/badge/license-MIT-lightgrey)

An internet radio and simple web-based MP3 player for **ESP32-S3** with USB audio output, file management over the web, USB flash drive support, FTP server, and mDNS network name.

This project is mainly intended as a small practical radio / player / diagnostic toy for ESP32-S3 boards with PSRAM. Audio is not sent through DAC or Bluetooth. It is sent through a **USB audio adapter** connected to the ESP32-S3 USB host port.

## Features

- **AP + STA** Wi-Fi mode
- web interface on port `80`
- default AP for initial setup
- web configuration for Wi-Fi, web login, FTP, mDNS, and radio stations
- internet radio playback through HTTP MP3 streams
- up to 6 saved radio stations
- automatic radio resume after reboot if the radio was playing before power-off
- last volume memory
- MP3 playback from internal FFat storage or USB flash drive
- folder playback
- next / previous track controls
- shuffle and repeat playlist modes
- web file manager
- upload, download, delete, create files and folders
- text file viewing and editing
- preview of common image and file types in the web interface
- simple FTP server for file access
- OTA `.bin` firmware upload through the web interface
- mDNS address, for example `http://oris-radio.local/`
- optional RGB LED playback indication
- USB flash drive mounting is attempted only once after boot; further attempts are manual from the web interface

## Hardware

Recommended setup:

- ESP32-S3 board with USB host support
- preferably **16 MB flash / 8 MB PSRAM**
- USB Audio Class compatible USB audio adapter
- USB flash drive formatted as FAT32
- powered USB hub when using both USB audio and USB flash drive
- stable power supply for the ESP32-S3 and USB peripherals

In the code, the RGB LED is configured on GPIO `48`:

```cpp
#define RGB_LED_PIN 48
```

The USB VBUS power switching pin is not configured:

```cpp
#define USB_POWER_PIN -1
```

This means the board must provide USB host power in hardware, or a powered USB hub should be used.

## Default Access

After the first boot, the ESP32 creates a Wi-Fi access point:

| Item | Default value |
|---|---|
| AP SSID | `ESP32-FS` |
| AP password | `12345678` |
| Web username | `admin` |
| Web password | `admin` |
| FTP username | `ftp` |
| FTP password | `12345678` |
| mDNS name | `oris-radio` |
| mDNS address | `http://oris-radio.local/` |

The default passwords are intended only for initial setup. Change them before normal use.

## First Start

1. Flash the firmware to the ESP32-S3.
2. After boot, connect to the Wi-Fi network `ESP32-FS`.
3. Open the following address in a browser:

```text
http://192.168.4.1/
```

4. Open **Configuration** from the menu.
5. Configure STA Wi-Fi connection to your home network.
6. Set an mDNS name, for example:

```text
kitchen-radio
```

7. After restart, the radio should be reachable on the network as:

```text
http://kitchen-radio.local/
```

mDNS must be supported by the operating system and the network. On some networks, using the DHCP IP address directly is more reliable.

## Web Pages

The web interface contains these main sections:

| Page | Description |
|---|---|
| `/files` | file manager for FFat and USB drive |
| `/radio` | internet radio stations and playback controls |
| `/config` | Wi-Fi, passwords, FTP, mDNS, volume, and station configuration |
| `/update` | upload a new `.bin` firmware file |

## Radio

The radio plays HTTP MP3 streams. Stations are configured as name and URL pairs.

State memory is supported:

- if the radio was playing before power-off, it starts again after the next boot
- station index and stream URL are saved
- resume starts only when Wi-Fi and USB audio are ready
- when playback is stopped manually, automatic resume is disabled
- when a local MP3 file is started, automatic radio resume is also disabled

## Volume

Volume is stored in the ESP32 NVS memory.

- the last selected volume is restored after reboot
- web slider changes are saved with a short delay to avoid unnecessary flash writes while dragging the slider
- volume range is `0–100`

## USB Drive

The USB flash drive is mounted as `usb0`.

Important behavior:

- USB mounting is attempted only once after boot
- if the flash drive is not connected during boot, the firmware will not keep searching for it repeatedly
- another mount attempt can be triggered manually using the **USB remount** button in the web interface
- FAT32 is the recommended filesystem for the USB flash drive

This behavior is intentional to prevent repeated USB detection attempts from disturbing radio playback.

## Internal FFat Storage

Configuration is stored in the internal FFat partition in this file:

```text
/config.cfg
```

Example configuration keys:

```ini
ap_ssid=ESP32-FS
ap_pass=12345678
sta_ssid=
sta_pass=
mdns_name=oris-radio
web_user=admin
web_pass=admin
ftp_enabled=1
ftp_user=ftp
ftp_pass=12345678
ftp_disk=usb0
rgb_enabled=0
audio_volume=80
radio_name=My Radio
radio_url=
radio_name_1=
radio_url_1=
```

## FTP Server

The firmware includes a simple FTP server.

Default access:

```text
Host: ESP32 IP address
Port: 21
User: ftp
Password: 12345678
```

FTP can work with either internal FFat storage or the USB drive depending on the `ftp_disk` setting.

Note: The FTP server is simple and intended mainly for convenient file transfer inside a local network. It is not intended for secure internet-facing use.

## OTA Firmware Update

New firmware can be uploaded through the web page:

```text
/update
```

Upload a compiled `.bin` file for the same board and a compatible partition scheme.

## Compilation

This project is an Arduino sketch for ESP32-S3.

### Recommended Arduino IDE Settings

- Board: ESP32-S3 matching your board
- PSRAM: Enabled
- Flash size: according to your board, 16 MB recommended
- Partition scheme: a variant with FFat partition and enough application space
- USB CDC: according to your board connection method
- Serial monitor: `115200 baud`

### Used Libraries

The sketch mainly uses:

```cpp
#include <WiFi.h>
#include <WebServer.h>
#include <FFat.h>
#include <FS.h>
#include <Preferences.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <PCMFlow.h>
#include "EspUsbHost.h"
```

`WiFi`, `WebServer`, `FFat`, `FS`, `Preferences`, `Update`, and `ESPmDNS` are common parts of the ESP32 Arduino environment.

`PCMFlow` and `EspUsbHost` must be available in the Arduino libraries folder or included with the project, depending on the version used.

## Supported Playback

The current player decodes MP3:

- local `.mp3` files from FFat or USB drive
- HTTP MP3 streams from internet radio stations

The file manager can display other common file types, but audio playback in this version is limited to MP3.

## Known Limitations

- Bluetooth audio output is not included.
- Local playback supports MP3 only.
- Internet radio expects a direct HTTP MP3 stream.
- The USB flash drive is not repeatedly searched for after boot.
- mDNS may not work on every network.
- FTP and web login use simple authentication without HTTPS.
- Weak USB peripheral power may cause dropouts or device detection problems.

## Troubleshooting

### The web interface does not open

- check the serial output at `115200 baud`
- connect to the `ESP32-FS` AP
- open `http://192.168.4.1/`
- after connecting to your home network, check the IP address in the serial monitor

### The mDNS address does not work

- try using the ESP32 IP address instead of `.local`
- make sure the device is on the same network
- some Windows installations may require mDNS/Bonjour support

### The USB audio adapter is not ready

- check the USB host wiring
- use a powered USB hub
- make sure the adapter is USB Audio Class compatible
- check the serial output to see whether the firmware found an audio output stream

### The USB drive is not visible

- format the flash drive as FAT32
- connect the flash drive before starting the ESP32
- use the **USB remount** button in the web interface
- check USB device power

### Radio playback stutters

- check Wi-Fi signal quality
- use a lower bitrate stream
- verify power supply stability
- watch the serial debug output for buffer and underrun messages

## Security

This project is intended for local network use.

After the first start, change:

- AP password
- web username and password
- FTP username and password

Do not expose the device directly to the internet.

## License

This project is licensed under the **MIT License**.

See the [LICENSE](LICENSE) file for details.
