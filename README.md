# ESP32-S3 USB Web Radio

![Platform](https://img.shields.io/badge/platform-ESP32--S3-blue)
![Framework](https://img.shields.io/badge/framework-Arduino-green)
![License](https://img.shields.io/badge/license-MIT-lightgrey)

Internetové rádio a jednoduchý webový MP3 přehrávač pro **ESP32-S3** s USB audio výstupem, webovou správou souborů, podporou USB flash disku, FTP serverem a mDNS názvem v síti.

Projekt je určený hlavně jako praktické malé rádio / přehrávač / diagnostická hračka pro ESP32-S3 s PSRAM. Zvuk nejde přes DAC ani Bluetooth, ale přes **USB audio adaptér** připojený k USB hostu ESP32-S3.

## Update v2.15 – multitasking, FTP správa souborů a web rename

Verze **2.15** přináší výrazné zlepšení práce s USB úložištěm, přehráváním MP3 a internetovým rádiem. Hlavním cílem bylo zabránit tomu, aby kopírování souborů přes FTP nebo web blokovalo přehrávání.

### Novinky

* Přidán základní multitasking přes FreeRTOS tasky.
* Audio přehrávání běží odděleně od hlavního web/FTP obslužného kódu.
* Přehrávání rádia a MP3 by se již nemělo zastavovat při FTP přenosu.
* USB remount a obsluha USB jsou méně blokující.
* Webové ovládání pouze zařazuje příkazy do fronty a ihned vrací odpověď.
* Přidána FTP podpora pro vytváření složek:
  * `MKD`
  * `XMKD`
* Přidána FTP podpora pro mazání prázdných složek:
  * `RMD`
  * `XRMD`
* Přidána FTP podpora pro přejmenování souborů a složek:
  * `RNFR`
  * `RNTO`
* Přidáno přejmenování souborů a složek přímo z webového rozhraní.
* Přidán web endpoint `/rename`.
* Upload už zbytečně neflushuje každý blok dat, ale průběžně a na konci přenosu.

## Funkce

- Wi-Fi režim **AP + STA**
- webové rozhraní na portu `80`
- výchozí AP pro první nastavení
- konfigurace Wi-Fi, web účtu, FTP, mDNS a rádia přes web
- přehrávání internetového rádia přes HTTP MP3 stream
- až 6 uložených rádiových stanic
- automatické obnovení rádia po restartu, pokud hrálo před vypnutím
- zapamatování poslední hlasitosti
- přehrávání MP3 souborů z interního FFat nebo USB disku
- přehrávání celé složky
- další / předchozí skladba
- shuffle a repeat režim playlistu
- webový správce souborů
- upload, download, mazání, vytváření souborů a složek
- prohlížení a editace textových souborů
- náhled běžných obrázků a souborů ve webu
- jednoduchý FTP server pro přístup k souborům
- OTA upload nového `.bin` firmwaru přes web
- mDNS adresa, například `http://oris-radio.local/`
- volitelná RGB LED indikace přehrávání
- USB flash disk se zkouší připojit jen jednou po startu, další pokus je ruční přes web

## Hardware

Doporučená sestava:

- ESP32-S3 deska s USB host podporou
- ideálně **16 MB flash / 8 MB PSRAM**
- USB audio adaptér kompatibilní s USB Audio Class
- USB flash disk naformátovaný jako FAT32
- napájený USB hub, pokud používáš zároveň USB zvukovku a flash disk
- stabilní napájení ESP32-S3 i USB periferií

V kódu je RGB LED nastavená na GPIO `48`:

```cpp
#define RGB_LED_PIN 48
```

USB VBUS spínací pin není nastavený:

```cpp
#define USB_POWER_PIN -1
```

To znamená, že deska musí mít USB host napájení vyřešené hardwarově, případně je vhodné použít napájený USB hub.

## Výchozí přístup

Po prvním startu ESP32 vytvoří Wi-Fi AP:

| Položka | Výchozí hodnota |
|---|---|
| AP SSID | `ESP32-FS` |
| AP heslo | `12345678` |
| Web uživatel | `admin` |
| Web heslo | `admin` |
| FTP uživatel | `ftp` |
| FTP heslo | `12345678` |
| mDNS jméno | `oris-radio` |
| mDNS adresa | `http://oris-radio.local/` |

Výchozí hesla jsou jen pro první spuštění. Pro běžný provoz je změň v konfiguraci.

## První spuštění

1. Nahraj firmware do ESP32-S3.
2. Po startu se připoj k Wi-Fi síti `ESP32-FS`.
3. Otevři v prohlížeči:

```text
http://192.168.4.1/
```

4. V menu otevři **Konfigurace**.
5. Nastav Wi-Fi STA připojení do domácí sítě.
6. Nastav mDNS jméno, například:

```text
kuchyn-radio
```

7. Po restartu půjde rádio v síti otevřít jako:

```text
http://kuchyn-radio.local/
```

mDNS musí podporovat operační systém i síť. Na některých sítích je spolehlivější použít přímo IP adresu z DHCP.

## Webové stránky

Webové rozhraní obsahuje hlavní sekce:

| Stránka | Popis |
|---|---|
| `/files` | správce souborů pro FFat a USB disk |
| `/radio` | internetová rádia a ovládání přehrávání |
| `/config` | konfigurace Wi-Fi, hesel, FTP, mDNS, hlasitosti a stanic |
| `/update` | upload nového `.bin` firmwaru |

## Rádio

Rádio přehrává HTTP MP3 streamy. Stanice se nastavují v konfiguraci jako název a URL.

Podporováno je zapamatování stavu:

- když před vypnutím hrálo rádio, po dalším startu se znovu spustí
- uloží se index stanice i URL streamu
- obnovení proběhne až ve chvíli, kdy je připravená Wi-Fi a USB zvukovka
- když přehrávání ručně zastavíš, automatické obnovení se vypne
- když pustíš lokální MP3, automatické obnovení rádia se také vypne

## Hlasitost

Hlasitost se ukládá do NVS paměti ESP32.

- po restartu se obnoví poslední nastavená hlasitost
- změny přes webový slider se ukládají s krátkým zpožděním, aby se zbytečně nezapisovalo do flash paměti při každém pohybu slideru
- hodnota je v rozsahu `0–100`

## USB disk

USB flash disk se připojuje jako `usb0`.

Důležité chování:

- mount USB disku se zkusí jen jednou po startu
- pokud flashka při startu není připojená, firmware ji nebude pořád dokola hledat
- další pokus lze vyvolat ručně přes web tlačítkem **USB remount**
- doporučený filesystem flashky je FAT32

Toto chování je záměrné, aby opakované hledání USB zařízení nerušilo přehrávání rádia.

## Interní úložiště FFat

Konfigurace se ukládá do interního FFat oddílu do souboru:

```text
/config.cfg
```

Příklad konfiguračních klíčů:

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
radio_name=Moje radio
radio_url=
radio_name_1=
radio_url_1=
```

## FTP server

Firmware obsahuje jednoduchý FTP server.

Výchozí přístup:

```text
Host: IP adresa ESP32
Port: 21
User: ftp
Password: 12345678
```

FTP může pracovat s interním FFat nebo USB diskem podle nastavení `ftp_disk`.

Poznámka: FTP server je jednoduchý a určený hlavně pro pohodlný přenos souborů v lokální síti. Nepočítá se zabezpečeným provozem přes internet.

## OTA firmware update

Nový firmware lze nahrát přes webovou stránku:

```text
/update
```

Nahrává se zkompilovaný `.bin` soubor pro stejnou desku a kompatibilní partition scheme.

## Kompilace

Projekt je Arduino sketch pro ESP32-S3.

### Doporučené nastavení v Arduino IDE

- Board: ESP32-S3 podle použité desky
- PSRAM: Enabled
- Flash size: podle desky, doporučeno 16 MB
- Partition scheme: varianta s FFat oddílem a dostatečným prostorem pro aplikaci
- USB CDC: podle způsobu připojení desky
- Serial monitor: `115200 baud`

### Použité knihovny

Sketch používá hlavně:

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

`WiFi`, `WebServer`, `FFat`, `FS`, `Preferences`, `Update` a `ESPmDNS` jsou běžné součásti ESP32 Arduino prostředí.

`PCMFlow` a `EspUsbHost` musí být dostupné v Arduino knihovnách nebo přiložené v projektu podle použité verze.

## Podporované přehrávání

Aktuální přehrávač dekóduje MP3:

- lokální `.mp3` soubory z FFat nebo USB disku
- HTTP MP3 streamy internetových rádií

Správce souborů umí zobrazit i další běžné typy souborů, ale audio přehrávání je v této verzi omezené na MP3.

## Známá omezení

- Bluetooth audio výstup není součástí projektu.
- Lokální přehrávání podporuje jen MP3.
- Internetové rádio očekává přímý MP3 HTTP stream.
- USB flash disk se po startu nehledá opakovaně automaticky.
- mDNS nemusí fungovat ve všech sítích.
- FTP a web používají jednoduché přihlašování bez HTTPS.
- Při slabém napájení USB periferií mohou vznikat výpadky nebo nedetekování zařízení.

## Řešení problémů

### Web nejde otevřít

- zkontroluj sériový výpis na `115200 baud`
- připoj se na AP `ESP32-FS`
- otevři `http://192.168.4.1/`
- po připojení do domácí sítě zkontroluj IP adresu v sériovém monitoru

### mDNS adresa nefunguje

- zkus místo `.local` použít přímo IP adresu ESP32
- ověř, že zařízení je ve stejné síti
- na některých Windows instalacích může být potřeba podpora mDNS/Bonjour

### USB zvukovka není připravená

- zkontroluj USB host zapojení
- použij napájený USB hub
- ověř, že zvukovka je kompatibilní s USB Audio Class
- sleduj sériový výpis, zda firmware našel audio output stream

### USB disk není vidět

- naformátuj flashku jako FAT32
- připoj flashku už před startem ESP32
- použij tlačítko **USB remount** ve webu
- zkontroluj napájení USB zařízení

### Rádio se seká

- zkontroluj kvalitu Wi-Fi signálu
- použij nižší bitrate stream
- ověř stabilitu napájení
- sleduj sériový debug výpis bufferu a underrunů

## Bezpečnost

Projekt je určený pro lokální síť.

Po prvním spuštění změň:

- AP heslo
- web uživatele a heslo
- FTP uživatele a heslo

Nevystavuj zařízení přímo do internetu.

## Licence

Tento projekt je licencovaný pod licencí **MIT**.

Podrobnosti jsou v souboru [LICENSE](LICENSE).
