// v.2.18

#include <WiFi.h>
#include <WebServer.h>
#include <FFat.h>
#include <FS.h>
#include <Preferences.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <PCMFlow.h>
#include "EspUsbHost.h"
#include "driver/i2s.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "mbedtls/sha1.h"
#include "mbedtls/base64.h"
#include "mbedtls/version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#define USB_POWER_PIN -1  // tahle deska nema znamy GPIO power switch pro OTG VBUS

// ============================================================
// HW piny pro verzi s PCM5102 I2S DAC + rotační enkodér
// ============================================================
// GPIO19/20 nech volné pro USB-OTG flashku. GPIO48 už používá RGB LED.
// Když má tvoje deska jiné vyvedené piny, změň jen tyto define.
#define I2S_AUDIO_PORT I2S_NUM_0
#define I2S_BCK_PIN    4    // PCM5102 BCK / BCLK
#define I2S_LRCK_PIN   5    // PCM5102 LCK / LRCK / WS
#define I2S_DOUT_PIN   6    // PCM5102 DIN
#define I2S_MUTE_PIN   -1   // volitelné: pin na MUTE/EN zesilovače, -1 = nepoužito

#define ENCODER_S1_PIN   7   // LaskaKit S1 / A / CLK
#define ENCODER_S2_PIN   15  // LaskaKit S2 / B / DT
#define ENCODER_KEY_PIN  16  // LaskaKit Key / SW
#define ENCODER_VOLUME_STEP 1

// Volitelný OLED 1,3" 128x64 SH1106 přes I2C.
// Stejný firmware funguje i bez displeje: při startu se zkusí adresy
// 0x3C a 0x3D, a když se nic nenajde, obsluha OLED se úplně přeskočí.
#define OLED_SDA_PIN 8
#define OLED_SCL_PIN 9

// Měření jedné LiPol baterie přes odporový dělič.
// Doporučené zapojení: BAT+ -> 100k -> GPIO1 -> 100k -> GND,
// a 100 nF z GPIO1 na GND. Pro dvě baterie paralelně je napětí stejné.
#define BATTERY_ADC_PIN 1


// ============================================================
// Nastavení
// ============================================================

const char* DEFAULT_AP_SSID = "ESP32-FS";
const char* DEFAULT_AP_PASS = "12345678";

const char* CONFIG_FILE = "/config.cfg";

WebServer server(80);
Preferences prefs;

EspUsbHost usb;
EspUsbHostMscFS usbMassStorage;

PCMFlow audio;

enum AudioOutputKind : uint8_t {
  AUDIO_OUTPUT_NONE = 0,
  AUDIO_OUTPUT_I2S,
  AUDIO_OUTPUT_USB
};

static uint8_t audioAddress = 0;
static AudioOutputKind activeAudioOutput = AUDIO_OUTPUT_NONE;
static bool audioReady = false;
static bool audioPlaying = false;
static String audioDisk = "";
static String audioPath = "";
static String audioStatus = "Audio výstup startuje";
static PCMFormat audioOutputFormat = {48000, 2, 16};
static constexpr float AUDIO_GAIN = 0.8f;

// ============================================================
// FreeRTOS multitasking / audio příkazy
// ============================================================
// Web handler jen zařadí příkaz do fronty a hned vrátí odpověď.
// Těžké věci jako HTTP connect, prebuffer rádia, dekódování startu
// nebo hledání další MP3 běží v audio tasku mimo hlavní web loop.

enum AudioCommandType : uint8_t {
  AUDIO_CMD_PLAY_FILE = 1,
  AUDIO_CMD_PLAY_FOLDER,
  AUDIO_CMD_PLAY_RADIO,
  AUDIO_CMD_NEXT,
  AUDIO_CMD_PREV,
  AUDIO_CMD_STOP,
  AUDIO_CMD_TOGGLE_PAUSE,
  AUDIO_CMD_USB_REMOUNT
};

struct AudioCommand {
  AudioCommandType type;
  int index;
  bool saveResume;
  char disk[12];
  char path[512];
  char label[128];
};

enum SendspinOutboundType : uint8_t {
  SENDSPIN_OUT_CONTROLLER = 1
};

struct SendspinOutboundCommand {
  SendspinOutboundType type;
  char value[32];
};

QueueHandle_t audioCommandQueue = nullptr;
QueueHandle_t sendspinCommandQueue = nullptr;

// Stav přehrávače (hlavně hlasitost z enkodéru) se nesmí hromadit ve
// frontě. Drží se jen poslední požadovaný stav; povely jako NEXT zůstávají
// ve skutečné frontě a síťový task je odešle před stavovou aktualizací.
portMUX_TYPE sendspinStateMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool sendspinPlayerStatePending = false;
char sendspinPendingForcedState[32] = {0};

// Dlouhé startovací operace (HTTP hlavičky, rádio/MP3 prebuffer)
// pravidelně kontrolují frontu. Nový STOP/NEXT/PREV tak nemusí čekat
// několik sekund na dokončení právě probíhajícího startu.
inline bool audioCommandPending() {
  return audioCommandQueue && uxQueueMessagesWaiting(audioCommandQueue) > 0;
}

TaskHandle_t audioTaskHandle = nullptr;
TaskHandle_t usbTaskHandle = nullptr;
TaskHandle_t networkTaskHandle = nullptr;
TaskHandle_t webTaskHandle = nullptr;
volatile bool usbRemountRequested = false;
volatile bool audioGainApplyPending = false;

volatile uint32_t audioTaskHeartbeatMs = 0;
volatile uint32_t usbTaskHeartbeatMs = 0;
volatile uint32_t networkTaskHeartbeatMs = 0;
volatile uint32_t webTaskHeartbeatMs = 0;

// Serializuje WebSocket TX. Sendspin odpovědi mohou vznikat v síťovém
// tasku, ale změna lokálního audio zdroje také v audio tasku.
SemaphoreHandle_t sendspinTxMutex = nullptr;

// Preferences používá několik tasků (radio resume + odložené uložení
// hlasitosti). Jeden sdílený objekt Preferences nesmí mít paralelní begin().
SemaphoreHandle_t prefsMutex = nullptr;

static const uint8_t AUDIO_COMMAND_QUEUE_LEN = 4;
static const uint8_t SENDSPIN_COMMAND_QUEUE_LEN = 12;
static const uint32_t AUDIO_TASK_STACK = 12288;
static const uint32_t USB_TASK_STACK = 4096;
static const uint32_t NETWORK_TASK_STACK = 8192;
static const uint32_t WEB_TASK_STACK = 8192;

bool lockPreferences(TickType_t timeout = pdMS_TO_TICKS(250));
void unlockPreferences();
void requestAudioGainApply();
void serviceAudioGainApply();

uint8_t* audioFileBuffer = nullptr;
size_t audioFileBufferSize = 0;

void freeAudioBuffer() {
  if (audioFileBuffer) {
    free(audioFileBuffer);
    audioFileBuffer = nullptr;
    audioFileBufferSize = 0;
  }
}

#define RGB_LED_PIN 48

// PCMFlow pri dr_mp3_init potrebuje dostat souvisly kus MP3 dat.
// Obycejny StreamByteStream nad WiFiClientem umi vratit 0 nebo kratky blok,
// a dr_mp3 pak spadne na DecoderInitFailed. Proto tady mame vlastni stream:
// 1) nejdriv vraci predem nacteny radioPreBuffer,
// 2) az potom cte dal z WiFiClientu a kratce blokuje na nova data.
class RadioBufferedByteStream : public ByteStream {
public:
  RadioBufferedByteStream() = default;

  void setSource(Stream *stream, const uint8_t *preBuffer, size_t preSize) {
    stream_ = stream;
    preBuffer_ = preBuffer;
    preSize_ = preSize;
    prePos_ = 0;
    pos_ = 0;
    seekLocked_ = false;
  }

  void clear() {
    stream_ = nullptr;
    preBuffer_ = nullptr;
    preSize_ = 0;
    prePos_ = 0;
    pos_ = 0;
    seekLocked_ = false;
  }

  size_t read(void *dst, size_t count) override {
    if (!dst || count == 0) {
      return 0;
    }

    uint8_t *out = static_cast<uint8_t *>(dst);
    size_t total = 0;

    // Nejdřív odsloužit přednačtený blok. Ten je seekovatelný,
    // takže dr_mp3 si při inicializaci může číst a vracet se zpět.
    while (total < count && preBuffer_ && prePos_ < preSize_) {
      size_t remain = preSize_ - prePos_;
      size_t n = count - total;
      if (n > remain) n = remain;

      memcpy(out + total, preBuffer_ + prePos_, n);
      prePos_ += n;
      pos_ += n;
      total += n;
    }

    if (total >= count) {
      return total;
    }

    // Jakmile se začne číst živý Wi-Fi stream za prebufferem,
    // zpětné seekování už nejde bezpečně udělat.
    if (pos_ >= preSize_) {
      seekLocked_ = true;
    }

    if (!stream_) {
      return total;
    }

    unsigned long start = millis();
    unsigned long lastByte = millis();

    // U síťového streamu krátce počkáme na data. Pokud už máme aspoň
    // něco, vrátíme částečnou dávku; pokud nemáme nic, po timeoutu 0.
    while (total < count) {
      int avail = stream_->available();

      if (avail > 0) {
        size_t want = count - total;
        if ((size_t)avail < want) want = (size_t)avail;

        size_t got = stream_->readBytes(out + total, want);
        if (got > 0) {
          total += got;
          pos_ += got;
          lastByte = millis();
        }
      } else {
        if (total > 0 && millis() - lastByte > 3) {
          break;
        }

        if (total == 0 && millis() - start > 25) {
          break;
        }

        delay(1);
      }
    }

    return total;
  }

  bool isEof() const override {
    return false;
  }

  bool isSeekable() const override {
    // Potřebujeme, aby dr_mp3_init mohl při startu udělat seek/tell
    // v rámci přednačteného MP3 začátku. Po najetí do živého streamu
    // seek zamkneme.
    return !seekLocked_ && preBuffer_ && preSize_ > 0;
  }

  bool seek(size_t offset) override {
    if (seekLocked_ || !preBuffer_ || offset > preSize_) {
      return false;
    }

    prePos_ = offset;
    pos_ = offset;
    return true;
  }

  size_t size() const override {
    // Nevracíme celkovou velikost, protože živý stream ji nezná.
    return 0;
  }

  size_t position() const override {
    return pos_;
  }

private:
  Stream *stream_ = nullptr;
  const uint8_t *preBuffer_ = nullptr;
  size_t preSize_ = 0;
  size_t prePos_ = 0;
  size_t pos_ = 0;
  bool seekLocked_ = false;
};


// Používáme FileByteStream přímo z PCMFlow knihovny.
// Lokální MP3 se tak čte průběžně ze souboru a necpe se celé do PSRAM.
File audioStreamFile;
FileByteStream audioFileStream;

WiFiClient radioClient;
RadioBufferedByteStream radioStream;
uint8_t* radioPreBuffer = nullptr;
size_t radioPreBufferSize = 0;
bool radioPlaying = false;
String radioStatus = "Radio stojí";
String radioUrlActive = "";
volatile uint16_t audioLevel = 0;
volatile uint32_t audioCbCount = 0;
volatile uint32_t audioCbFrames = 0;
volatile uint32_t audioCbUnderruns = 0;
uint32_t audioPumpCount = 0;
uint32_t lastAudioDebugMs = 0;
uint8_t rgbHue = 0;
uint32_t lastRgbLedMs = 0;

bool i2sAudioStarted = false;
bool audioPaused = false;
int16_t i2sOutBuffer[256 * 2]; // 256 stereo framů, 16 bit

int encoderLastAB = 0;
int encoderMoveAccum = 0;
bool encoderBtnRawDown = false;
bool encoderBtnStableDown = false;
uint32_t encoderBtnRawChangedMs = 0;
uint32_t encoderBtnDownMs = 0;
uint32_t encoderLastClickMs = 0;
uint8_t encoderClickCount = 0;
bool encoderLongFired = false;

void freeRadioPreBuffer() {
  if (radioPreBuffer) {
    free(radioPreBuffer);
    radioPreBuffer = nullptr;
    radioPreBufferSize = 0;
  }
}

int findMp3StartOffset(const uint8_t *buf, size_t len) {
  if (!buf || len < 2) return -1;

  if (len >= 3 && buf[0] == 'I' && buf[1] == 'D' && buf[2] == '3') {
    return 0;
  }

  for (size_t i = 0; i + 1 < len; i++) {
    if (buf[i] == 0xFF && (buf[i + 1] & 0xE0) == 0xE0) {
      return (int)i;
    }
  }

  return -1;
}


bool fillRadioPreBuffer(WiFiClient &client, size_t targetBytes, size_t maxBytes) {
  freeRadioPreBuffer();

  radioPreBuffer = (uint8_t*)ps_malloc(maxBytes);
  if (!radioPreBuffer) {
    radioStatus = "Nedostatek PSRAM pro radio prebuffer";
    audioStatus = radioStatus;
    return false;
  }

  radioPreBufferSize = 0;
  unsigned long start = millis();
  unsigned long lastData = millis();

  while (radioPreBufferSize < targetBytes && radioPreBufferSize < maxBytes) {
    if (audioCommandPending()) {
      freeRadioPreBuffer();
      radioStatus = "Start rádia přerušen novým povelem";
      audioStatus = radioStatus;
      return false;
    }

    int avail = client.available();

    if (avail > 0) {
      size_t want = maxBytes - radioPreBufferSize;
      if ((size_t)avail < want) want = (size_t)avail;

      size_t got = client.readBytes(radioPreBuffer + radioPreBufferSize, want);
      if (got > 0) {
        radioPreBufferSize += got;
        lastData = millis();
      }
    } else {
      if (!client.connected() && millis() - lastData > 300) break;
      if (millis() - start > 6000) break;
      delay(2);
    }
  }

  Serial.printf("Radio prebuffer raw: size=%u first=%02X %02X %02X %02X\n",
                (unsigned)radioPreBufferSize,
                radioPreBufferSize > 0 ? radioPreBuffer[0] : 0,
                radioPreBufferSize > 1 ? radioPreBuffer[1] : 0,
                radioPreBufferSize > 2 ? radioPreBuffer[2] : 0,
                radioPreBufferSize > 3 ? radioPreBuffer[3] : 0);

  int mp3Start = findMp3StartOffset(radioPreBuffer, radioPreBufferSize);
  Serial.printf("Radio MP3 start offset: %d\n", mp3Start);

  if (mp3Start < 0) {
    freeRadioPreBuffer();
    radioStatus = "V radiu nenalezen MP3 frame";
    audioStatus = radioStatus;
    return false;
  }

  if (mp3Start > 0) {
    memmove(radioPreBuffer, radioPreBuffer + mp3Start, radioPreBufferSize - (size_t)mp3Start);
    radioPreBufferSize -= (size_t)mp3Start;
  }

  return radioPreBufferSize >= 1024;
}

bool uploadActive = false;
bool audioWasPlayingBeforeUpload = false;

File uploadFile;
uint32_t uploadLastFlushMs = 0;

// Samostatný upload log stanice do /www/logos.
File radioLogoUploadFile;
int radioLogoUploadIndex = -1;
String radioLogoUploadTempPath = "";
String radioLogoUploadTargetPath = "";
String radioLogoUploadError = "";
size_t radioLogoUploadBytes = 0;
static const size_t MAX_RADIO_LOGO_SIZE = 512 * 1024;

String currentDisk = "ffat";
String uploadDiskName = "";

bool playlistActive = false;
bool playlistShuffle = false;
bool playlistRepeat = false;
String playlistDisk = "";
String playlistDir = "/";
String playlistLastPath = "";

// ============================================================
// Časování aktuálního lokálního audia
// ============================================================
uint32_t audioStartedMs = 0;
uint32_t audioPausedAtMs = 0;
uint32_t audioPausedAccumMs = 0;

void markAudioPositionStart() {
  audioStartedMs = millis();
  audioPausedAtMs = 0;
  audioPausedAccumMs = 0;
}

void markAudioPositionStop() {
  audioStartedMs = 0;
  audioPausedAtMs = 0;
  audioPausedAccumMs = 0;
}

extern volatile bool sendspinStreamActive;
uint32_t currentSendspinPositionMs();

uint32_t currentAudioPositionMs() {
  if (sendspinStreamActive) {
    return currentSendspinPositionMs();
  }
  if (audioStartedMs == 0) {
    return 0;
  }

  uint32_t now = audioPaused ? audioPausedAtMs : millis();
  if (now < audioStartedMs) {
    return 0;
  }

  uint32_t raw = now - audioStartedMs;
  if (raw <= audioPausedAccumMs) {
    return 0;
  }

  return raw - audioPausedAccumMs;
}


String usbStatus = "USB host startuje";
bool usbHostStarted = false;
bool usbBootMountPending = false;
uint32_t usbBootMountAfterMs = 0;

uint32_t usbHostStartMs = 0;
uint32_t usbLastDeviceEventMs = 0;

static const uint32_t USB_STARTUP_MIN_MS   = 8000;
static const uint32_t USB_STARTUP_QUIET_MS = 2500;
static const uint32_t USB_STARTUP_MAX_MS   = 15000;

bool usbStartupSettled() {
  if (!usbHostStarted) {
    return false;
  }

  uint32_t now = millis();

  bool minDone = (now - usbHostStartMs) >= USB_STARTUP_MIN_MS;
  bool quietDone = (now - usbLastDeviceEventMs) >= USB_STARTUP_QUIET_MS;
  bool maxDone = (now - usbHostStartMs) >= USB_STARTUP_MAX_MS;

  return maxDone || (minDone && quietDone);
}

bool mdnsStarted = false;
bool mdnsStartedWithSta = false;
String mdnsStartedName = "";
uint32_t lastMdnsStartTryMs = 0;

bool radioResumeWanted = false;
bool radioResumeAttempted = false;
int radioResumeIndex = 0;
String radioResumeUrl = "";
bool radioResumeSavedWanted = false;
int radioResumeSavedIndex = -1;
String radioResumeSavedUrl = "";

int audioVolumeSaved = -1;
bool audioVolumeSavePending = false;
uint32_t audioVolumeLastChangeMs = 0;

static const uint8_t MAX_RADIO_STATIONS = 30;
static const uint8_t MAX_SAVED_WIFI = 8;
static const uint8_t MAX_WIFI_SCAN_RESULTS = 24;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 12000;
static const uint32_t WIFI_RETRY_INTERVAL_MS = 20000;

// Ladění web rádia.
// Když rádio občas škubne, zvedej hlavně RADIO_PCM_BUFFER_FRAMES
// a RADIO_DECODER_START_FRAMES. Když dojde interní heap, vrať je níž.
static const size_t RADIO_PREBUFFER_TARGET_BYTES = 131072;      // kolik MP3 dat přednačíst z HTTP
static const size_t RADIO_PREBUFFER_MAX_BYTES    = 262144;     // max PSRAM prebuffer pro start dekodéru
static const size_t RADIO_PCM_BUFFER_FRAMES      = 98304;      // PCMFlow ring buffer
static const size_t RADIO_DECODER_START_FRAMES   = 49152;       // kolik PCM framů mít před spuštěním I2S audio
static const uint32_t RADIO_PREBUFFER_TIMEOUT_MS = 10000;      // čekání na naplnění PCM bufferu

// Lokální MP3 z flashky/FFat už nečteme celé do PSRAM.
// Dekódují se průběžně přes FileByteStream, takže půjdou i dlouhé skladby.
static const size_t FILE_PCM_BUFFER_FRAMES       = 8192;       // menší než rádio, disk je stabilnější než Wi-Fi
static const size_t FILE_DECODER_START_FRAMES    = 2048;       // zásoba před spuštěním USB callbacku
static const uint32_t FILE_PREBUFFER_TIMEOUT_MS  = 5000;

static const uint8_t AUDIO_EQ_BANDS = 10;

enum ApOperatingMode : uint8_t {
  AP_MODE_ALWAYS = 0,      // AP běží stále
  AP_MODE_AUTO = 1,        // AP běží jen bez připojené STA Wi-Fi
  AP_MODE_OFF = 2          // AP je vždy vypnuté
};

struct AppCfg {
  String apSsid;
  String apPass;
  ApOperatingMode apMode;
  // Původní jedna STA síť zůstává kvůli kompatibilitě starého config.cfg.
  String staSsid;
  String staPass;
  uint8_t wifiCount;
  String wifiSsid[MAX_SAVED_WIFI];
  String wifiPass[MAX_SAVED_WIFI];
  String mdnsName;
  bool smartSpeakerEnabled;
  String webUser;
  String webPass;
  bool ftpEnabled;
  String ftpUser;
  String ftpPass;
  String ftpDisk;
  bool rgbEnabled;
  int audioVolume;
  // Staré dvoupásmové hodnoty zůstávají kvůli migraci a kompatibilitě API.
  int audioBassDb;
  int audioTrebleDb;
  bool audioEqEnabled;
  int audioEqPreampDb;
  bool audioEqAutoHeadroom;
  int audioEqBandDb[AUDIO_EQ_BANDS];
  int audioOutputGainDb;
  float audioVolumeCurve;
  bool batteryEnabled;
  float batteryDividerRatio;
  float batteryCalibration;
  String radioName[MAX_RADIO_STATIONS];
  String radioUrl[MAX_RADIO_STATIONS];
  String radioLogo[MAX_RADIO_STATIONS];
};

AppCfg cfg;

// ============================================================
// Síťový reproduktor Sendspin
// ============================================================
// Implementace používá serverem iniciované spojení podle Sendspin v1.
// ESP se přes mDNS ohlásí jako _sendspin._tcp, Music Assistant se připojí
// WebSocketem na /sendspin a posílá nekomprimované PCM 16-bit stereo.
// Obal alba se do ESP nestahuje: do webového rozhraní předáváme artwork_url.

static const uint16_t SENDSPIN_PORT = 8928;
static const char* SENDSPIN_PATH = "/sendspin";
static const size_t SENDSPIN_PCM_BUFFER_BYTES = 768 * 1024;
static const uint16_t SENDSPIN_PCM_CHUNK_COUNT = 192;
static const size_t SENDSPIN_WS_RX_BYTES = 128 * 1024;
static const uint32_t SENDSPIN_TIME_SYNC_INTERVAL_MS = 2000;
static const uint32_t SENDSPIN_TIME_SYNC_FAST_MS = 250;
static const uint32_t SENDSPIN_CLIENT_TIMEOUT_MS = 45000;
static const int64_t SENDSPIN_I2S_WRITE_AHEAD_US = 18000;
static const int64_t SENDSPIN_DROP_LATE_AFTER_US = 450000;

struct SendspinPcmChunk {
  size_t offset;
  size_t length;
  size_t consumed;
  int64_t serverTimestampUs;
};

WiFiServer sendspinServer(SENDSPIN_PORT);
WiFiClient sendspinClient;

bool sendspinServerStarted = false;
bool sendspinWebSocketReady = false;
bool sendspinProtocolReady = false;
bool sendspinMobileClientActive = false;
bool sendspinExternalSource = false;
volatile bool sendspinStreamActive = false;
volatile bool sendspinGroupPlaying = false;
volatile bool sendspinMuted = false;

String sendspinHttpHeaders = "";
uint8_t* sendspinWsRx = nullptr;
size_t sendspinWsRxUsed = 0;

uint8_t* sendspinPcmBuffer = nullptr;
size_t sendspinPcmWritePos = 0;
size_t sendspinPcmReadPos = 0;
size_t sendspinPcmUsed = 0;
SendspinPcmChunk sendspinPcmChunks[SENDSPIN_PCM_CHUNK_COUNT];
uint16_t sendspinPcmChunkHead = 0;
uint16_t sendspinPcmChunkTail = 0;
uint16_t sendspinPcmChunkUsed = 0;
SemaphoreHandle_t sendspinPcmMutex = nullptr;

volatile uint32_t sendspinSampleRate = 48000;
volatile uint8_t sendspinChannels = 2;
volatile uint8_t sendspinBitDepth = 16;
volatile int64_t sendspinClockOffsetUs = 0;
volatile bool sendspinClockSynced = false;

String sendspinServerId = "";
String sendspinTitle = "";
String sendspinArtist = "";
String sendspinAlbum = "";
String sendspinArtworkUrl = "";
String sendspinPlaybackState = "stopped";
String sendspinStatus = "Síťový reproduktor čeká";
bool sendspinControllerCanPlay = false;
bool sendspinControllerCanPause = false;
bool sendspinControllerCanStop = false;
bool sendspinControllerCanNext = false;
bool sendspinControllerCanPrevious = false;
bool sendspinControllerCanVolume = false;
bool sendspinControllerCanMute = false;

uint32_t sendspinTrackProgressMs = 0;
uint32_t sendspinTrackDurationMs = 0;
int32_t sendspinPlaybackSpeed = 0;
int64_t sendspinMetadataTimestampServerUs = 0;

uint32_t sendspinLastActivityMs = 0;
uint32_t sendspinLastTimeSyncMs = 0;
uint8_t sendspinTimeSyncBurstLeft = 0;
uint32_t sendspinDroppedChunks = 0;
uint32_t sendspinReceivedChunks = 0;
uint32_t sendspinPlayedFrames = 0;
uint32_t sendspinLateChunks = 0;

// Dopředné deklarace funkcí síťového reproduktoru.
void serviceSendspin();
void stopSendspinService(bool stopListener);
void sendspinClearPcm();
void sendspinSendPlayerState(const char* forcedState = nullptr);
void sendspinSendPlayerStateNow(const char* forcedState = nullptr);
void sendspinSetExternalSource(bool external);
void sendspinSendControllerCommand(const char* command);
void sendspinSendControllerCommandNow(const char* command);
void serviceSendspinCommandQueue();
void serviceSendspinI2sAudioOutput();
size_t sendspinReadPcmFrames(void* dst, size_t maxFrames);
uint32_t currentSendspinPositionMs();
bool sendspinIsPlaying();
void handleSmartSpeakerSave();
bool serveConfigPageWithSmartSpeakerPanel();
void startMdns(bool forceRestart = false);

// ============================================================
// Stav baterie přes ADC1 / GPIO1
// ============================================================

static const uint32_t BATTERY_READ_INTERVAL_MS = 5000;
static const uint8_t BATTERY_SAMPLE_COUNT = 32;

float batteryVoltage = 0.0f;
int batteryPercent = -1;
uint32_t batteryRawMilliVolts = 0;
bool batteryMeasurementValid = false;
uint32_t lastBatteryReadMs = 0;

struct BatteryCurvePoint {
  float voltage;
  int percent;
};

static const BatteryCurvePoint BATTERY_CURVE[] = {
  {3.20f,   0},
  {3.40f,   5},
  {3.55f,  10},
  {3.65f,  20},
  {3.72f,  30},
  {3.79f,  40},
  {3.85f,  50},
  {3.91f,  60},
  {3.97f,  70},
  {4.03f,  80},
  {4.10f,  90},
  {4.20f, 100}
};

void resetBatteryMeasurement();
void initBatteryMonitor();
float readBatteryVoltageNow();
int voltageToBatteryPercent(float voltage);
void serviceBatteryMonitor(bool force = false);
String batteryStatusText();

void resetBatteryMeasurement() {
  batteryVoltage = 0.0f;
  batteryPercent = -1;
  batteryRawMilliVolts = 0;
  batteryMeasurementValid = false;
  lastBatteryReadMs = 0;
}

void initBatteryMonitor() {
  resetBatteryMeasurement();

  if (!cfg.batteryEnabled) {
    Serial.println("Battery monitor disabled");
    return;
  }

  pinMode(BATTERY_ADC_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
  Serial.printf(
    "Battery monitor ready: GPIO=%d divider=%.3f calibration=%.3f\n",
    BATTERY_ADC_PIN,
    cfg.batteryDividerRatio,
    cfg.batteryCalibration
  );
  serviceBatteryMonitor(true);
}

float readBatteryVoltageNow() {
  // První převod po delší pauze zahodíme. Kondenzátor 100 nF pomáhá
  // vysokoodporovému děliči 100k/100k ustálit ADC vstup.
  analogReadMilliVolts(BATTERY_ADC_PIN);

  uint32_t sumMv = 0;
  for (uint8_t i = 0; i < BATTERY_SAMPLE_COUNT; i++) {
    sumMv += analogReadMilliVolts(BATTERY_ADC_PIN);
    delayMicroseconds(200);
  }

  batteryRawMilliVolts = sumMv / BATTERY_SAMPLE_COUNT;
  const float adcVoltage = (float)batteryRawMilliVolts / 1000.0f;
  return adcVoltage * cfg.batteryDividerRatio * cfg.batteryCalibration;
}

int voltageToBatteryPercent(float voltage) {
  const size_t count = sizeof(BATTERY_CURVE) / sizeof(BATTERY_CURVE[0]);

  if (voltage <= BATTERY_CURVE[0].voltage) return 0;
  if (voltage >= BATTERY_CURVE[count - 1].voltage) return 100;

  for (size_t i = 1; i < count; i++) {
    if (voltage <= BATTERY_CURVE[i].voltage) {
      const float v1 = BATTERY_CURVE[i - 1].voltage;
      const float v2 = BATTERY_CURVE[i].voltage;
      const int p1 = BATTERY_CURVE[i - 1].percent;
      const int p2 = BATTERY_CURVE[i].percent;
      const float part = (voltage - v1) / (v2 - v1);
      int percent = p1 + (int)lroundf((float)(p2 - p1) * part);
      if (percent < 0) percent = 0;
      if (percent > 100) percent = 100;
      return percent;
    }
  }

  return 0;
}

void serviceBatteryMonitor(bool force) {
  if (!cfg.batteryEnabled) {
    if (batteryMeasurementValid || batteryPercent >= 0 || batteryVoltage > 0.0f) {
      resetBatteryMeasurement();
    }
    return;
  }

  const uint32_t now = millis();
  if (!force && now - lastBatteryReadMs < BATTERY_READ_INTERVAL_MS) return;
  lastBatteryReadMs = now;

  const float measured = readBatteryVoltageNow();
  const bool valid = isfinite(measured) && measured >= 2.0f && measured <= 5.0f;

  if (!valid) {
    batteryMeasurementValid = false;
    batteryPercent = -1;
    return;
  }

  if (!batteryMeasurementValid || batteryVoltage < 0.1f) {
    batteryVoltage = measured;
  } else {
    // Vyhlazení proti propadům od Wi-Fi a zesilovače.
    batteryVoltage = batteryVoltage * 0.75f + measured * 0.25f;
  }

  batteryMeasurementValid = true;
  batteryPercent = voltageToBatteryPercent(batteryVoltage);
}

String batteryStatusText() {
  if (!cfg.batteryEnabled) return "vypnuto";
  if (!batteryMeasurementValid) return "bez platného měření";
  return String(batteryPercent) + " % / " + String(batteryVoltage, 2) + " V";
}

enum WifiScanOwner : uint8_t {
  WIFI_SCAN_NONE = 0,
  WIFI_SCAN_AUTO,
  WIFI_SCAN_MANUAL
};

enum WifiManagerState : uint8_t {
  WIFI_MANAGER_IDLE = 0,
  WIFI_MANAGER_SCANNING,
  WIFI_MANAGER_CONNECTING
};

WifiScanOwner wifiScanOwner = WIFI_SCAN_NONE;
WifiManagerState wifiManagerState = WIFI_MANAGER_IDLE;
int8_t wifiCandidateIndex[MAX_SAVED_WIFI];
int32_t wifiCandidateRssi[MAX_SAVED_WIFI];
uint8_t wifiCandidateCount = 0;
uint8_t wifiCandidatePos = 0;
uint32_t wifiConnectStartedMs = 0;
uint32_t wifiLastRetryMs = 0;
uint32_t wifiManualScanStartedMs = 0;
String wifiManagerStatus = "Wi-Fi čeká";
bool apRunning = false;
bool apPolicyApplyPending = false;
bool apPolicyForceRestart = false;
uint32_t apPolicyApplyAtMs = 0;
uint32_t apStaConnectedSinceMs = 0;
bool apMdnsRestartPending = false;
uint32_t apMdnsRestartAtMs = 0;

static const uint32_t AP_AUTO_DISABLE_DELAY_MS = 5000;
static const uint32_t AP_MDNS_RESTART_DELAY_MS = 500;

bool saveConfig();
void syncLegacyStaFromSavedWifi();
int findSavedWifiIndex(const String& ssid);
bool addOrUpdateSavedWifi(const String& ssid, const String& password, int &indexOut);
bool removeSavedWifi(uint8_t index);
void requestSavedWifiReconnect(bool immediate);
void serviceSavedWifiManager();
void stopAudioPlayback(const String& reason);
bool shouldApRun();
void applyApPolicy(bool forceRestart = false);
void scheduleApPolicyApply(bool forceRestart, uint32_t delayMs = 500);
void serviceApPolicy();

// ============================================================
// Více uložených Wi-Fi sítí + volitelný režim AP hotspotu
// ============================================================

void syncLegacyStaFromSavedWifi() {
  if (cfg.wifiCount > 0) {
    cfg.staSsid = cfg.wifiSsid[0];
    cfg.staPass = cfg.wifiPass[0];
  } else {
    cfg.staSsid = "";
    cfg.staPass = "";
  }
}

int findSavedWifiIndex(const String& ssid) {
  for (uint8_t i = 0; i < cfg.wifiCount && i < MAX_SAVED_WIFI; i++) {
    if (cfg.wifiSsid[i] == ssid) return (int)i;
  }
  return -1;
}

bool addOrUpdateSavedWifi(const String& ssidValue, const String& passwordValue, int &indexOut) {
  String ssid = ssidValue;
  String password = passwordValue;
  ssid.trim();

  indexOut = -1;
  if (ssid.length() == 0 || ssid.length() > 32 || password.length() > 64) return false;

  int existing = findSavedWifiIndex(ssid);
  if (existing >= 0) {
    // Prázdné heslo u existující sítě znamená „ponechat původní heslo“.
    if (password.length() > 0 || cfg.wifiPass[existing].length() == 0) {
      cfg.wifiPass[existing] = password;
    }
    indexOut = existing;
    syncLegacyStaFromSavedWifi();
    return true;
  }

  if (cfg.wifiCount >= MAX_SAVED_WIFI) return false;

  uint8_t idx = cfg.wifiCount++;
  cfg.wifiSsid[idx] = ssid;
  cfg.wifiPass[idx] = password;
  indexOut = idx;
  syncLegacyStaFromSavedWifi();
  return true;
}

bool removeSavedWifi(uint8_t index) {
  if (index >= cfg.wifiCount || index >= MAX_SAVED_WIFI) return false;

  for (uint8_t i = index; i + 1 < cfg.wifiCount; i++) {
    cfg.wifiSsid[i] = cfg.wifiSsid[i + 1];
    cfg.wifiPass[i] = cfg.wifiPass[i + 1];
  }

  if (cfg.wifiCount > 0) cfg.wifiCount--;
  cfg.wifiSsid[cfg.wifiCount] = "";
  cfg.wifiPass[cfg.wifiCount] = "";
  syncLegacyStaFromSavedWifi();
  return true;
}

void sortWifiCandidates() {
  for (uint8_t i = 0; i < wifiCandidateCount; i++) {
    for (uint8_t j = i + 1; j < wifiCandidateCount; j++) {
      if (wifiCandidateRssi[j] > wifiCandidateRssi[i]) {
        int8_t idxTmp = wifiCandidateIndex[i];
        wifiCandidateIndex[i] = wifiCandidateIndex[j];
        wifiCandidateIndex[j] = idxTmp;

        int32_t rssiTmp = wifiCandidateRssi[i];
        wifiCandidateRssi[i] = wifiCandidateRssi[j];
        wifiCandidateRssi[j] = rssiTmp;
      }
    }
  }
}

void startWifiCandidate(uint8_t candidatePos) {
  if (candidatePos >= wifiCandidateCount) {
    wifiManagerState = WIFI_MANAGER_IDLE;
    wifiLastRetryMs = millis();
    wifiManagerStatus = "Žádná uložená Wi-Fi se nepřipojila";
    Serial.println(wifiManagerStatus);
    return;
  }

  int idx = wifiCandidateIndex[candidatePos];
  if (idx < 0 || idx >= cfg.wifiCount) {
    wifiCandidatePos++;
    startWifiCandidate(wifiCandidatePos);
    return;
  }

  String ssid = cfg.wifiSsid[idx];
  wifiManagerStatus = "Připojuji: " + ssid;
  Serial.println(wifiManagerStatus);

  WiFi.begin(ssid.c_str(), cfg.wifiPass[idx].c_str());
  wifiConnectStartedMs = millis();
  wifiManagerState = WIFI_MANAGER_CONNECTING;
}

void buildWifiCandidatesFromScan(int found) {
  wifiCandidateCount = 0;
  wifiCandidatePos = 0;

  // Nejdřív známé sítě, které jsou skutečně vidět, podle síly signálu.
  for (uint8_t saved = 0; saved < cfg.wifiCount && saved < MAX_SAVED_WIFI; saved++) {
    int32_t bestRssi = -1000;
    for (int scan = 0; scan < found; scan++) {
      if (WiFi.SSID(scan) == cfg.wifiSsid[saved] && WiFi.RSSI(scan) > bestRssi) {
        bestRssi = WiFi.RSSI(scan);
      }
    }

    if (bestRssi > -1000 && wifiCandidateCount < MAX_SAVED_WIFI) {
      wifiCandidateIndex[wifiCandidateCount] = (int8_t)saved;
      wifiCandidateRssi[wifiCandidateCount] = bestRssi;
      wifiCandidateCount++;
    }
  }

  sortWifiCandidates();

  // Skryté nebo momentálně neviditelné uložené sítě zkusíme až nakonec.
  for (uint8_t saved = 0; saved < cfg.wifiCount && saved < MAX_SAVED_WIFI; saved++) {
    bool alreadyAdded = false;
    for (uint8_t i = 0; i < wifiCandidateCount; i++) {
      if (wifiCandidateIndex[i] == (int8_t)saved) {
        alreadyAdded = true;
        break;
      }
    }
    if (!alreadyAdded && wifiCandidateCount < MAX_SAVED_WIFI) {
      wifiCandidateIndex[wifiCandidateCount] = (int8_t)saved;
      wifiCandidateRssi[wifiCandidateCount] = -1000;
      wifiCandidateCount++;
    }
  }
}

bool startSavedWifiAutoScan() {
  if (cfg.wifiCount == 0 || wifiScanOwner != WIFI_SCAN_NONE) return false;

  WiFi.scanDelete();
  int result = WiFi.scanNetworks(true, true);
  if (result == WIFI_SCAN_FAILED) {
    wifiManagerState = WIFI_MANAGER_IDLE;
    wifiLastRetryMs = millis();
    wifiManagerStatus = "Wi-Fi scan se nepodařilo spustit";
    return false;
  }

  wifiScanOwner = WIFI_SCAN_AUTO;
  wifiManagerState = WIFI_MANAGER_SCANNING;
  wifiManagerStatus = "Hledám uložené Wi-Fi sítě";
  return true;
}

void requestSavedWifiReconnect(bool immediate) {
  if (cfg.wifiCount == 0) {
    wifiManagerState = WIFI_MANAGER_IDLE;
    wifiManagerStatus = "Není uložená žádná Wi-Fi";
    return;
  }

  if (wifiScanOwner == WIFI_SCAN_MANUAL) return;

  wifiManagerState = WIFI_MANAGER_IDLE;
  wifiCandidateCount = 0;
  wifiCandidatePos = 0;
  wifiLastRetryMs = immediate ? 0 : millis();

  if (immediate) startSavedWifiAutoScan();
}

void connectSavedWifiDirect(uint8_t index) {
  if (index >= cfg.wifiCount) return;

  wifiCandidateCount = 1;
  wifiCandidatePos = 0;
  wifiCandidateIndex[0] = (int8_t)index;
  wifiCandidateRssi[0] = 0;

  if (radioPlaying) {
    stopAudioPlayback("Wi-Fi se přepojuje");
    radioResumeAttempted = false;
  }

  WiFi.disconnect(false, false);
  delay(50);
  startWifiCandidate(0);
}

void serviceSavedWifiManager() {
  // Když prohlížeč po ručním scanu zmizí, po 30 s výsledky uklidíme,
  // aby automatické připojování nezůstalo trvale zablokované.
  if (wifiScanOwner == WIFI_SCAN_MANUAL &&
      wifiManualScanStartedMs > 0 &&
      millis() - wifiManualScanStartedMs > 30000) {
    WiFi.scanDelete();
    wifiScanOwner = WIFI_SCAN_NONE;
    wifiManualScanStartedMs = 0;
    wifiLastRetryMs = millis();
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (wifiManagerState != WIFI_MANAGER_IDLE) {
      wifiManagerStatus = "Připojeno: " + WiFi.SSID();
      Serial.println(wifiManagerStatus + " / " + WiFi.localIP().toString());
      radioResumeAttempted = false;
    }
    wifiManagerState = WIFI_MANAGER_IDLE;
    wifiCandidateCount = 0;
    return;
  }

  if (wifiScanOwner == WIFI_SCAN_AUTO && wifiManagerState == WIFI_MANAGER_SCANNING) {
    int found = WiFi.scanComplete();
    if (found == WIFI_SCAN_RUNNING) return;

    if (found < 0) {
      WiFi.scanDelete();
      wifiScanOwner = WIFI_SCAN_NONE;
      wifiManagerState = WIFI_MANAGER_IDLE;
      wifiLastRetryMs = millis();
      wifiManagerStatus = "Automatický Wi-Fi scan selhal";
      return;
    }

    buildWifiCandidatesFromScan(found);
    WiFi.scanDelete();
    wifiScanOwner = WIFI_SCAN_NONE;

    if (wifiCandidateCount == 0) {
      wifiManagerState = WIFI_MANAGER_IDLE;
      wifiLastRetryMs = millis();
      wifiManagerStatus = "Žádná uložená Wi-Fi není dostupná";
      return;
    }

    startWifiCandidate(0);
    return;
  }

  if (wifiManagerState == WIFI_MANAGER_CONNECTING) {
    if (millis() - wifiConnectStartedMs < WIFI_CONNECT_TIMEOUT_MS) return;

    wifiCandidatePos++;
    WiFi.disconnect(false, false);
    delay(20);
    startWifiCandidate(wifiCandidatePos);
    return;
  }

  // Ruční scan ovládá webový endpoint; automatiku během něj nespouštíme.
  if (wifiScanOwner == WIFI_SCAN_MANUAL || cfg.wifiCount == 0) return;

  uint32_t now = millis();
  if (wifiLastRetryMs == 0 || now - wifiLastRetryMs >= WIFI_RETRY_INTERVAL_MS) {
    wifiLastRetryMs = now;
    startSavedWifiAutoScan();
  }
}

// ============================================================
// Softwarový 10pásmový grafický ekvalizér + bezpečný výstupní gain
// ============================================================
// Aktuální firmware používá Arduino + PCMFlow, nikoli ESP-ADF audio pipeline.
// Proto je zde vlastní realtime 10pásmový ekvalizér inspirovaný rozložením
// pásem Espressif ADF: 31, 62, 125, 250, 500 Hz, 1, 2, 4, 8 a 16 kHz.
//
// Každé pásmo je RBJ peaking biquad. Nastavení se aplikuje na 16bit PCM
// těsně před odesláním do I2S nebo USB. Kladné zdvihy lze automaticky
// kompenzovat headroomem, aby se omezilo digitální ořezání.
//
// Výstupní gain a křivka hlasitosti jsou zpracované v currentAudioGain().
// Díky tomu 100 % hlasitosti znamená nastavený bezpečný strop a ne vždy
// plný výstup DACu.

static const float AUDIO_EQ_FREQUENCIES[AUDIO_EQ_BANDS] = {
  31.0f, 62.0f, 125.0f, 250.0f, 500.0f,
  1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f
};

struct EqCoefficients {
  float b0 = 1.0f;
  float b1 = 0.0f;
  float b2 = 0.0f;
  float a1 = 0.0f;
  float a2 = 0.0f;
};

struct EqFilterState {
  float z1 = 0.0f;
  float z2 = 0.0f;
};

struct AudioEqProcessor {
  EqCoefficients band[AUDIO_EQ_BANDS];
  EqFilterState state[AUDIO_EQ_BANDS][2];
  uint32_t sampleRate = 0;
  uint8_t channels = 0;
  bool enabled = false;
  bool autoHeadroom = true;
  int preampDb = 99;
  int bandDb[AUDIO_EQ_BANDS];
  uint16_t activeMask = 0;
  float preamp = 1.0f;

  AudioEqProcessor() {
    for (uint8_t i = 0; i < AUDIO_EQ_BANDS; i++) {
      bandDb[i] = 99;
    }
  }
};

AudioEqProcessor i2sEqProcessor;
AudioEqProcessor usbEqProcessor;
static constexpr float EQ_PI = 3.14159265358979323846f;

// Explicitní prototypy jsou nutné kvůli Arduino preprocesoru.
int clampEqDb(int value);
int clampEqPreampDb(int value);
int clampOutputGainDb(int value);
float clampVolumeCurve(float value);
void clampDetailedAudioConfig();
void syncLegacyEqFields();
String audioEqBandsJson();
void resetEqFilterStates(AudioEqProcessor &eq);
void resetAudioEqualizers();
EqCoefficients makePeakingEq(float sampleRate, float frequency, float q, float gainDb);
float processEqBiquad(float input, const EqCoefficients &c, EqFilterState &state);
void prepareAudioEqualizer(AudioEqProcessor &eq, uint32_t sampleRate, uint8_t channels);
void processAudioEqualizer(
  int16_t *samples,
  size_t frameCount,
  uint8_t channels,
  uint32_t sampleRate,
  AudioEqProcessor &eq
);

int clampEqDb(int value) {
  if (value < -12) return -12;
  if (value > 12) return 12;
  return value;
}

int clampEqPreampDb(int value) {
  if (value < -24) return -24;
  if (value > 6) return 6;
  return value;
}

int clampOutputGainDb(int value) {
  if (value < -40) return -40;
  if (value > 0) return 0;
  return value;
}

float clampVolumeCurve(float value) {
  if (!isfinite(value)) return 1.8f;
  if (value < 1.0f) return 1.0f;
  if (value > 3.0f) return 3.0f;
  return value;
}

void syncLegacyEqFields() {
  int bassSum = 0;
  for (uint8_t i = 0; i < 4; i++) bassSum += cfg.audioEqBandDb[i];
  int trebleSum = 0;
  for (uint8_t i = 7; i < AUDIO_EQ_BANDS; i++) trebleSum += cfg.audioEqBandDb[i];
  cfg.audioBassDb = clampEqDb((int)lroundf((float)bassSum / 4.0f));
  cfg.audioTrebleDb = clampEqDb((int)lroundf((float)trebleSum / 3.0f));
}

void clampDetailedAudioConfig() {
  cfg.audioEqPreampDb = clampEqPreampDb(cfg.audioEqPreampDb);
  cfg.audioOutputGainDb = clampOutputGainDb(cfg.audioOutputGainDb);
  cfg.audioVolumeCurve = clampVolumeCurve(cfg.audioVolumeCurve);
  for (uint8_t i = 0; i < AUDIO_EQ_BANDS; i++) {
    cfg.audioEqBandDb[i] = clampEqDb(cfg.audioEqBandDb[i]);
  }
  syncLegacyEqFields();
}

String audioEqBandsJson() {
  String json = "[";
  json.reserve(48);
  for (uint8_t i = 0; i < AUDIO_EQ_BANDS; i++) {
    if (i) json += ",";
    json += String(cfg.audioEqBandDb[i]);
  }
  json += "]";
  return json;
}

void resetEqFilterStates(AudioEqProcessor &eq) {
  for (uint8_t band = 0; band < AUDIO_EQ_BANDS; band++) {
    for (uint8_t ch = 0; ch < 2; ch++) {
      eq.state[band][ch].z1 = 0.0f;
      eq.state[band][ch].z2 = 0.0f;
    }
  }
}

void resetAudioEqualizers() {
  i2sEqProcessor.sampleRate = 0;
  usbEqProcessor.sampleRate = 0;
  resetEqFilterStates(i2sEqProcessor);
  resetEqFilterStates(usbEqProcessor);
}

EqCoefficients makePeakingEq(float sampleRate, float frequency, float q, float gainDb) {
  EqCoefficients c;
  if (fabsf(gainDb) < 0.01f || sampleRate <= 0.0f || q <= 0.0f) return c;

  // Pásma nad Nyquistem se na nižších sample rate nepoužijí.
  if (frequency >= sampleRate * 0.47f) return c;

  const float A = powf(10.0f, gainDb / 40.0f);
  const float w0 = 2.0f * EQ_PI * frequency / sampleRate;
  const float alpha = sinf(w0) / (2.0f * q);
  const float cosw = cosf(w0);

  const float b0 = 1.0f + alpha * A;
  const float b1 = -2.0f * cosw;
  const float b2 = 1.0f - alpha * A;
  const float a0 = 1.0f + alpha / A;
  const float a1 = -2.0f * cosw;
  const float a2 = 1.0f - alpha / A;

  c.b0 = b0 / a0;
  c.b1 = b1 / a0;
  c.b2 = b2 / a0;
  c.a1 = a1 / a0;
  c.a2 = a2 / a0;
  return c;
}

inline float processEqBiquad(float input, const EqCoefficients &c, EqFilterState &state) {
  const float output = c.b0 * input + state.z1;
  state.z1 = c.b1 * input - c.a1 * output + state.z2;
  state.z2 = c.b2 * input - c.a2 * output;
  return output;
}

void prepareAudioEqualizer(AudioEqProcessor &eq, uint32_t sampleRate, uint8_t channels) {
  const bool enabled = cfg.audioEqEnabled;
  const bool autoHeadroom = cfg.audioEqAutoHeadroom;
  const int preampDb = clampEqPreampDb(cfg.audioEqPreampDb);

  bool changed =
    eq.sampleRate != sampleRate ||
    eq.channels != channels ||
    eq.enabled != enabled ||
    eq.autoHeadroom != autoHeadroom ||
    eq.preampDb != preampDb;

  for (uint8_t i = 0; i < AUDIO_EQ_BANDS; i++) {
    if (eq.bandDb[i] != clampEqDb(cfg.audioEqBandDb[i])) {
      changed = true;
      break;
    }
  }
  if (!changed) return;

  eq.sampleRate = sampleRate;
  eq.channels = channels;
  eq.enabled = enabled;
  eq.autoHeadroom = autoHeadroom;
  eq.preampDb = preampDb;
  eq.activeMask = 0;

  int maxPositiveBoostDb = 0;
  for (uint8_t i = 0; i < AUDIO_EQ_BANDS; i++) {
    const int gainDb = clampEqDb(cfg.audioEqBandDb[i]);
    eq.bandDb[i] = gainDb;
    eq.band[i] = makePeakingEq(
      (float)sampleRate,
      AUDIO_EQ_FREQUENCIES[i],
      1.40f,
      (float)gainDb
    );

    if (enabled && gainDb != 0 && AUDIO_EQ_FREQUENCIES[i] < (float)sampleRate * 0.47f) {
      eq.activeMask |= (uint16_t)(1U << i);
    }
    if (gainDb > maxPositiveBoostDb) maxPositiveBoostDb = gainDb;
  }

  float effectivePreampDb = enabled ? (float)preampDb : 0.0f;
  if (enabled && autoHeadroom && maxPositiveBoostDb > 0) {
    effectivePreampDb -= (float)maxPositiveBoostDb;
  }
  eq.preamp = powf(10.0f, effectivePreampDb / 20.0f);

  resetEqFilterStates(eq);
}

void processAudioEqualizer(
  int16_t *samples,
  size_t frameCount,
  uint8_t channels,
  uint32_t sampleRate,
  AudioEqProcessor &eq
) {
  if (!samples || frameCount == 0 || sampleRate == 0) return;
  if (channels == 0 || channels > 2) return;

  prepareAudioEqualizer(eq, sampleRate, channels);

  if (!eq.enabled) return;
  const bool usePreamp = fabsf(eq.preamp - 1.0f) > 0.0001f;
  if (!usePreamp && eq.activeMask == 0) return;

  const size_t sampleCount = frameCount * (size_t)channels;
  for (size_t index = 0; index < sampleCount; index++) {
    const uint8_t ch = channels == 2 ? (uint8_t)(index & 1U) : 0;
    float value = usePreamp ? (float)samples[index] * eq.preamp : (float)samples[index];

    uint16_t mask = eq.activeMask;
    uint8_t band = 0;
    while (mask) {
      if (mask & 1U) {
        value = processEqBiquad(value, eq.band[band], eq.state[band][ch]);
      }
      mask >>= 1;
      band++;
    }

    if (value > 32767.0f) value = 32767.0f;
    else if (value < -32768.0f) value = -32768.0f;
    samples[index] = (int16_t)lroundf(value);
  }
}


// ============================================================
// Sendspin: síťový reproduktor pro Music Assistant
// ============================================================

static int sendspinHexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void sendspinAppendUtf8(String& out, uint32_t codepoint) {
  if (codepoint <= 0x7F) {
    out += (char)codepoint;
  } else if (codepoint <= 0x7FF) {
    out += (char)(0xC0 | ((codepoint >> 6) & 0x1F));
    out += (char)(0x80 | (codepoint & 0x3F));
  } else if (codepoint <= 0xFFFF) {
    out += (char)(0xE0 | ((codepoint >> 12) & 0x0F));
    out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
    out += (char)(0x80 | (codepoint & 0x3F));
  } else if (codepoint <= 0x10FFFF) {
    out += (char)(0xF0 | ((codepoint >> 18) & 0x07));
    out += (char)(0x80 | ((codepoint >> 12) & 0x3F));
    out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
    out += (char)(0x80 | (codepoint & 0x3F));
  }
}

static int sendspinJsonValuePos(const String& json, const char* key, int from = 0) {
  String needle = "\"" + String(key) + "\"";
  int keyPos = json.indexOf(needle, from);
  if (keyPos < 0) return -1;

  int colon = json.indexOf(':', keyPos + needle.length());
  if (colon < 0) return -1;

  int pos = colon + 1;
  while (pos < (int)json.length()) {
    char c = json[pos];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
    pos++;
  }
  return pos;
}

static bool sendspinJsonParseStringAt(const String& json, int pos, String& value) {
  value = "";
  if (pos < 0 || pos >= (int)json.length() || json[pos] != '"') return false;

  value.reserve(96);
  bool escaped = false;

  for (int i = pos + 1; i < (int)json.length(); i++) {
    char c = json[i];

    if (!escaped) {
      if (c == '"') return true;
      if (c == '\\') {
        escaped = true;
      } else {
        value += c;
      }
      continue;
    }

    escaped = false;
    switch (c) {
      case '"': value += '"'; break;
      case '\\': value += '\\'; break;
      case '/': value += '/'; break;
      case 'b': value += '\b'; break;
      case 'f': value += '\f'; break;
      case 'n': value += '\n'; break;
      case 'r': value += '\r'; break;
      case 't': value += '\t'; break;
      case 'u': {
        if (i + 4 >= (int)json.length()) return false;
        uint32_t codepoint = 0;
        for (int j = 0; j < 4; j++) {
          int h = sendspinHexDigit(json[i + 1 + j]);
          if (h < 0) return false;
          codepoint = (codepoint << 4) | (uint32_t)h;
        }
        i += 4;

        // Základní podpora UTF-16 surrogate páru.
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF &&
            i + 6 < (int)json.length() && json[i + 1] == '\\' && json[i + 2] == 'u') {
          uint32_t low = 0;
          bool lowOk = true;
          for (int j = 0; j < 4; j++) {
            int h = sendspinHexDigit(json[i + 3 + j]);
            if (h < 0) {
              lowOk = false;
              break;
            }
            low = (low << 4) | (uint32_t)h;
          }
          if (lowOk && low >= 0xDC00 && low <= 0xDFFF) {
            codepoint = 0x10000 + (((codepoint - 0xD800) << 10) | (low - 0xDC00));
            i += 6;
          }
        }

        sendspinAppendUtf8(value, codepoint);
        break;
      }
      default:
        value += c;
        break;
    }
  }

  return false;
}

static bool sendspinJsonGetString(const String& json, const char* key, String& value) {
  int pos = sendspinJsonValuePos(json, key);
  return sendspinJsonParseStringAt(json, pos, value);
}

static bool sendspinJsonGetNullableString(
  const String& json,
  const char* key,
  String& value,
  bool& isNull
) {
  int pos = sendspinJsonValuePos(json, key);
  if (pos < 0) return false;

  if (json.startsWith("null", pos)) {
    value = "";
    isNull = true;
    return true;
  }

  isNull = false;
  return sendspinJsonParseStringAt(json, pos, value);
}

static bool sendspinJsonGetInt64(const String& json, const char* key, int64_t& value) {
  int pos = sendspinJsonValuePos(json, key);
  if (pos < 0) return false;

  const char* start = json.c_str() + pos;
  char* end = nullptr;
  long long parsed = strtoll(start, &end, 10);
  if (end == start) return false;
  value = (int64_t)parsed;
  return true;
}

static bool sendspinJsonGetBool(const String& json, const char* key, bool& value) {
  int pos = sendspinJsonValuePos(json, key);
  if (pos < 0) return false;
  if (json.startsWith("true", pos)) {
    value = true;
    return true;
  }
  if (json.startsWith("false", pos)) {
    value = false;
    return true;
  }
  return false;
}

static bool sendspinJsonExtractObject(const String& json, const char* key, String& object) {
  int pos = sendspinJsonValuePos(json, key);
  if (pos < 0 || pos >= (int)json.length() || json[pos] != '{') return false;

  int depth = 0;
  bool inString = false;
  bool escaped = false;

  for (int i = pos; i < (int)json.length(); i++) {
    char c = json[i];

    if (inString) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        inString = false;
      }
      continue;
    }

    if (c == '"') {
      inString = true;
    } else if (c == '{') {
      depth++;
    } else if (c == '}') {
      depth--;
      if (depth == 0) {
        object = json.substring(pos, i + 1);
        return true;
      }
    }
  }

  return false;
}

static String sendspinHeaderValue(const String& headers, const char* wantedName) {
  String wanted = String(wantedName);
  wanted.toLowerCase();

  int lineStart = 0;
  while (lineStart < (int)headers.length()) {
    int lineEnd = headers.indexOf("\r\n", lineStart);
    if (lineEnd < 0) lineEnd = headers.length();

    String line = headers.substring(lineStart, lineEnd);
    int colon = line.indexOf(':');
    if (colon > 0) {
      String name = line.substring(0, colon);
      name.trim();
      name.toLowerCase();
      if (name == wanted) {
        String value = line.substring(colon + 1);
        value.trim();
        return value;
      }
    }

    lineStart = lineEnd + 2;
  }

  return "";
}

static String sendspinWebSocketAccept(const String& key) {
  String source = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  unsigned char digest[20];

#if MBEDTLS_VERSION_NUMBER >= 0x03000000
  mbedtls_sha1(
    reinterpret_cast<const unsigned char*>(source.c_str()),
    source.length(),
    digest
  );
#else
  mbedtls_sha1_ret(
    reinterpret_cast<const unsigned char*>(source.c_str()),
    source.length(),
    digest
  );
#endif

  unsigned char encoded[40];
  size_t encodedLength = 0;
  if (mbedtls_base64_encode(encoded, sizeof(encoded) - 1, &encodedLength, digest, sizeof(digest)) != 0) {
    return "";
  }
  encoded[encodedLength] = 0;
  return String(reinterpret_cast<char*>(encoded));
}

static bool sendspinWriteAll(const uint8_t* data, size_t length) {
  if (!sendspinClient || !sendspinClient.connected()) return false;

  size_t written = 0;
  uint32_t started = millis();
  while (written < length) {
    size_t n = sendspinClient.write(data + written, length - written);
    if (n > 0) {
      written += n;
      started = millis();
    } else {
      if (millis() - started > 1500) return false;
      delay(1);
    }
  }
  return true;
}

static bool sendspinSendFrame(uint8_t opcode, const uint8_t* payload, size_t length) {
  if (!sendspinWebSocketReady || !sendspinClient.connected()) return false;

  bool locked = !sendspinTxMutex ||
                xSemaphoreTake(sendspinTxMutex, pdMS_TO_TICKS(1600)) == pdTRUE;
  if (!locked) return false;

  uint8_t header[10];
  size_t headerLength = 0;
  header[headerLength++] = 0x80 | (opcode & 0x0F);

  if (length < 126) {
    header[headerLength++] = (uint8_t)length;
  } else if (length <= 0xFFFF) {
    header[headerLength++] = 126;
    header[headerLength++] = (uint8_t)((length >> 8) & 0xFF);
    header[headerLength++] = (uint8_t)(length & 0xFF);
  } else {
    header[headerLength++] = 127;
    uint64_t value = (uint64_t)length;
    for (int shift = 56; shift >= 0; shift -= 8) {
      header[headerLength++] = (uint8_t)((value >> shift) & 0xFF);
    }
  }

  bool ok = sendspinWriteAll(header, headerLength);
  if (ok && length > 0 && payload) {
    ok = sendspinWriteAll(payload, length);
  }

  if (sendspinTxMutex) xSemaphoreGive(sendspinTxMutex);
  return ok;
}

static bool sendspinSendText(const String& message) {
  return sendspinSendFrame(0x1, reinterpret_cast<const uint8_t*>(message.c_str()), message.length());
}

static String sendspinClientId() {
  uint64_t chip = ESP.getEfuseMac();
  char id[40];
  snprintf(
    id,
    sizeof(id),
    "oris-%04X%08X",
    (unsigned)((chip >> 32) & 0xFFFF),
    (unsigned)(chip & 0xFFFFFFFF)
  );
  return String(id);
}

static void sendspinSendHello() {
  String host = normalizeMdnsName(cfg.mdnsName);
  uint32_t preferredRate = activeOutputIsUsb() ? audioOutputFormat.sampleRate : 48000;
  uint8_t preferredChannels = activeOutputIsUsb() ? audioOutputFormat.channels : 2;
  uint8_t preferredBits = activeOutputIsUsb() ? audioOutputFormat.bitsPerSample : 16;

  if ((preferredRate != 44100 && preferredRate != 48000) ||
      preferredChannels != 2 || preferredBits != 16) {
    preferredRate = 48000;
    preferredChannels = 2;
    preferredBits = 16;
  }

  String hello;
  hello.reserve(760);
  hello += "{\"type\":\"client/hello\",\"payload\":{";
  hello += "\"client_id\":\"" + jsonEscape(sendspinClientId()) + "\",";
  hello += "\"name\":\"" + jsonEscape(host) + "\",";
  hello += "\"device_info\":{";
  hello += "\"product_name\":\"ORIS ESP32-S3 radio\",";
  hello += "\"manufacturer\":\"ORIS core\",";
  hello += "\"software_version\":\"sendspin-beta-1\",";
  hello += "\"mac_address\":\"" + jsonEscape(WiFi.macAddress()) + "\"},";
  hello += "\"version\":1,";
  hello += "\"supported_roles\":[\"player@v1\",\"metadata@v1\",\"controller@v1\"],";
  hello += "\"player@v1_support\":{";
  hello += "\"supported_formats\":[";
  hello += "{\"codec\":\"pcm\",\"channels\":" + String(preferredChannels) +
           ",\"sample_rate\":" + String(preferredRate) +
           ",\"bit_depth\":" + String(preferredBits) + "}";
  if (!activeOutputIsUsb()) {
    uint32_t alternateRate = preferredRate == 48000 ? 44100 : 48000;
    hello += ",{\"codec\":\"pcm\",\"channels\":2,\"sample_rate\":" +
             String(alternateRate) + ",\"bit_depth\":16}";
  }
  hello += "],";
  hello += "\"buffer_capacity\":" + String((unsigned)SENDSPIN_PCM_BUFFER_BYTES) + ",";
  hello += "\"supported_commands\":[\"volume\",\"mute\"]";
  hello += "}}}";

  sendspinSendText(hello);
}

static bool sendspinEnsureBuffers() {
  if (!sendspinPcmMutex) {
    sendspinPcmMutex = xSemaphoreCreateMutex();
  }
  if (!sendspinPcmMutex) {
    sendspinStatus = "Sendspin: nelze vytvořit mutex";
    return false;
  }

  if (!sendspinPcmBuffer) {
    sendspinPcmBuffer = static_cast<uint8_t*>(heap_caps_malloc(
      SENDSPIN_PCM_BUFFER_BYTES,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    ));
    if (!sendspinPcmBuffer) {
      sendspinPcmBuffer = static_cast<uint8_t*>(malloc(SENDSPIN_PCM_BUFFER_BYTES));
    }
  }

  if (!sendspinWsRx) {
    sendspinWsRx = static_cast<uint8_t*>(heap_caps_malloc(
      SENDSPIN_WS_RX_BYTES,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    ));
    if (!sendspinWsRx) {
      sendspinWsRx = static_cast<uint8_t*>(malloc(SENDSPIN_WS_RX_BYTES));
    }
  }

  if (!sendspinPcmBuffer || !sendspinWsRx) {
    sendspinStatus = "Sendspin: nedostatek paměti";
    return false;
  }

  return true;
}

static void sendspinDropOldestChunkLocked() {
  if (sendspinPcmChunkUsed == 0) return;

  SendspinPcmChunk& chunk = sendspinPcmChunks[sendspinPcmChunkHead];
  size_t remaining = chunk.length > chunk.consumed ? chunk.length - chunk.consumed : 0;
  if (remaining > sendspinPcmUsed) remaining = sendspinPcmUsed;

  sendspinPcmReadPos = (sendspinPcmReadPos + remaining) % SENDSPIN_PCM_BUFFER_BYTES;
  sendspinPcmUsed -= remaining;
  sendspinPcmChunkHead = (sendspinPcmChunkHead + 1) % SENDSPIN_PCM_CHUNK_COUNT;
  sendspinPcmChunkUsed--;
}

void sendspinClearPcm() {
  if (!sendspinPcmMutex) return;
  if (xSemaphoreTake(sendspinPcmMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;

  sendspinPcmWritePos = 0;
  sendspinPcmReadPos = 0;
  sendspinPcmUsed = 0;
  sendspinPcmChunkHead = 0;
  sendspinPcmChunkTail = 0;
  sendspinPcmChunkUsed = 0;

  xSemaphoreGive(sendspinPcmMutex);
}

static bool sendspinQueuePcm(int64_t serverTimestampUs, const uint8_t* data, size_t length) {
  if (!sendspinPcmBuffer || !sendspinPcmMutex || !data || length == 0) return false;

  size_t frameBytes = (size_t)sendspinChannels * ((size_t)sendspinBitDepth / 8U);
  if (frameBytes == 0) return false;
  length -= length % frameBytes;
  if (length == 0 || length > SENDSPIN_PCM_BUFFER_BYTES) return false;

  if (xSemaphoreTake(sendspinPcmMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
    sendspinDroppedChunks++;
    return false;
  }

  while ((SENDSPIN_PCM_BUFFER_BYTES - sendspinPcmUsed < length ||
          sendspinPcmChunkUsed >= SENDSPIN_PCM_CHUNK_COUNT) &&
         sendspinPcmChunkUsed > 0) {
    sendspinDropOldestChunkLocked();
    sendspinDroppedChunks++;
  }

  if (SENDSPIN_PCM_BUFFER_BYTES - sendspinPcmUsed < length ||
      sendspinPcmChunkUsed >= SENDSPIN_PCM_CHUNK_COUNT) {
    xSemaphoreGive(sendspinPcmMutex);
    sendspinDroppedChunks++;
    return false;
  }

  size_t first = length;
  if (first > SENDSPIN_PCM_BUFFER_BYTES - sendspinPcmWritePos) {
    first = SENDSPIN_PCM_BUFFER_BYTES - sendspinPcmWritePos;
  }
  memcpy(sendspinPcmBuffer + sendspinPcmWritePos, data, first);
  if (length > first) {
    memcpy(sendspinPcmBuffer, data + first, length - first);
  }

  SendspinPcmChunk& chunk = sendspinPcmChunks[sendspinPcmChunkTail];
  chunk.offset = sendspinPcmWritePos;
  chunk.length = length;
  chunk.consumed = 0;
  chunk.serverTimestampUs = serverTimestampUs;

  sendspinPcmWritePos = (sendspinPcmWritePos + length) % SENDSPIN_PCM_BUFFER_BYTES;
  sendspinPcmUsed += length;
  sendspinPcmChunkTail = (sendspinPcmChunkTail + 1) % SENDSPIN_PCM_CHUNK_COUNT;
  sendspinPcmChunkUsed++;
  sendspinReceivedChunks++;

  xSemaphoreGive(sendspinPcmMutex);
  return true;
}

static uint32_t sendspinQueuedMsLocked() {
  const size_t frameBytes = (size_t)sendspinChannels * ((size_t)sendspinBitDepth / 8U);
  if (frameBytes == 0 || sendspinSampleRate == 0) return 0;
  uint64_t frames = sendspinPcmUsed / frameBytes;
  return (uint32_t)((frames * 1000ULL) / sendspinSampleRate);
}

size_t sendspinReadPcmFrames(void* dst, size_t maxFrames) {
  if (!dst || maxFrames == 0 || !sendspinPcmMutex || !sendspinStreamActive || !sendspinGroupPlaying) {
    return 0;
  }

  if (xSemaphoreTake(sendspinPcmMutex, 0) != pdTRUE) return 0;

  const size_t frameBytes = (size_t)sendspinChannels * ((size_t)sendspinBitDepth / 8U);
  if (frameBytes == 0 || sendspinSampleRate == 0) {
    xSemaphoreGive(sendspinPcmMutex);
    return 0;
  }

  const int64_t nowUs = esp_timer_get_time();

  while (sendspinPcmChunkUsed > 0) {
    SendspinPcmChunk& chunk = sendspinPcmChunks[sendspinPcmChunkHead];
    size_t remaining = chunk.length - chunk.consumed;
    uint64_t consumedFrames = chunk.consumed / frameBytes;
    int64_t sampleServerTimestamp = chunk.serverTimestampUs +
      (int64_t)((consumedFrames * 1000000ULL) / sendspinSampleRate);
    int64_t localTargetUs = sendspinClockSynced
      ? sampleServerTimestamp - sendspinClockOffsetUs
      : nowUs;

    if (sendspinClockSynced && nowUs - localTargetUs > SENDSPIN_DROP_LATE_AFTER_US) {
      sendspinDropOldestChunkLocked();
      sendspinLateChunks++;
      continue;
    }

    if (sendspinClockSynced && nowUs + SENDSPIN_I2S_WRITE_AHEAD_US < localTargetUs) {
      xSemaphoreGive(sendspinPcmMutex);
      return 0;
    }

    size_t maxBytes = maxFrames * frameBytes;
    size_t copyBytes = remaining < maxBytes ? remaining : maxBytes;
    copyBytes -= copyBytes % frameBytes;
    if (copyBytes == 0) {
      sendspinDropOldestChunkLocked();
      continue;
    }

    size_t first = copyBytes;
    if (first > SENDSPIN_PCM_BUFFER_BYTES - sendspinPcmReadPos) {
      first = SENDSPIN_PCM_BUFFER_BYTES - sendspinPcmReadPos;
    }
    memcpy(dst, sendspinPcmBuffer + sendspinPcmReadPos, first);
    if (copyBytes > first) {
      memcpy(static_cast<uint8_t*>(dst) + first, sendspinPcmBuffer, copyBytes - first);
    }

    sendspinPcmReadPos = (sendspinPcmReadPos + copyBytes) % SENDSPIN_PCM_BUFFER_BYTES;
    sendspinPcmUsed -= copyBytes;
    chunk.consumed += copyBytes;

    if (chunk.consumed >= chunk.length) {
      sendspinPcmChunkHead = (sendspinPcmChunkHead + 1) % SENDSPIN_PCM_CHUNK_COUNT;
      sendspinPcmChunkUsed--;
    }

    size_t frames = copyBytes / frameBytes;
    sendspinPlayedFrames += frames;
    xSemaphoreGive(sendspinPcmMutex);
    return frames;
  }

  xSemaphoreGive(sendspinPcmMutex);
  return 0;
}

static void sendspinApplyVolume(int16_t* samples, size_t frames, uint8_t channels) {
  if (!samples || frames == 0 || channels == 0) return;

  float gain = sendspinMuted ? 0.0f : currentAudioGain();
  if (gain >= 0.999f) return;

  size_t sampleCount = frames * (size_t)channels;
  for (size_t i = 0; i < sampleCount; i++) {
    samples[i] = (int16_t)((float)samples[i] * gain);
  }
}

static void sendspinUpdateAudioLevel(const int16_t* samples, size_t sampleCount) {
  if (!samples || sampleCount == 0) return;

  uint32_t sum = 0;
  size_t step = sampleCount > 96 ? sampleCount / 96 : 1;
  size_t counted = 0;
  for (size_t i = 0; i < sampleCount; i += step) {
    int32_t value = samples[i];
    if (value < 0) value = -value;
    sum += (uint32_t)value;
    counted++;
  }
  if (counted > 0) audioLevel = (uint16_t)(sum / counted);
}

void serviceSendspinI2sAudioOutput() {
  if (!activeOutputIsI2s() || !sendspinStreamActive || !sendspinGroupPlaying) return;

  const size_t maxFrames = sizeof(i2sOutBuffer) / (sizeof(int16_t) * 2);
  size_t frames = sendspinReadPcmFrames(i2sOutBuffer, maxFrames);
  if (frames == 0) return;

  sendspinApplyVolume(i2sOutBuffer, frames, sendspinChannels);
  processAudioEqualizer(
    i2sOutBuffer,
    frames,
    sendspinChannels,
    sendspinSampleRate,
    i2sEqProcessor
  );

  size_t bytesToWrite = frames * (size_t)sendspinChannels * sizeof(int16_t);
  size_t bytesWritten = 0;
  esp_err_t err = i2s_write(
    I2S_AUDIO_PORT,
    i2sOutBuffer,
    bytesToWrite,
    &bytesWritten,
    portMAX_DELAY
  );

  audioCbCount++;
  audioCbFrames += bytesWritten / ((size_t)sendspinChannels * sizeof(int16_t));
  if (err != ESP_OK || bytesWritten != bytesToWrite) audioCbUnderruns++;
  sendspinUpdateAudioLevel(i2sOutBuffer, bytesWritten / sizeof(int16_t));
}

static void sendspinSendTime() {
  if (!sendspinProtocolReady) return;
  int64_t nowUs = esp_timer_get_time();
  char nowBuffer[32];
  snprintf(nowBuffer, sizeof(nowBuffer), "%lld", (long long)nowUs);
  String message = "{\"type\":\"client/time\",\"payload\":{\"client_transmitted\":" +
                   String(nowBuffer) + "}}";
  if (sendspinSendText(message)) {
    sendspinLastTimeSyncMs = millis();
  }
}

void sendspinSendPlayerStateNow(const char* forcedState) {
  if (!sendspinProtocolReady) return;

  String state = forcedState ? String(forcedState) :
    String(sendspinExternalSource ? "external_source" : "synchronized");

  String message;
  message.reserve(320);
  message += "{\"type\":\"client/state\",\"payload\":{";
  message += "\"state\":\"" + state + "\",";
  message += "\"player\":{";
  message += "\"volume\":" + String(cfg.audioVolume) + ",";
  message += "\"muted\":" + String(sendspinMuted ? "true" : "false") + ",";
  message += "\"static_delay_ms\":0";
  message += "}}}";
  sendspinSendText(message);
}

static bool queueSendspinOutbound(SendspinOutboundType type, const char* value) {
  if (!sendspinCommandQueue) return false;

  SendspinOutboundCommand cmd = {};
  cmd.type = type;
  if (value && value[0]) {
    strncpy(cmd.value, value, sizeof(cmd.value) - 1);
    cmd.value[sizeof(cmd.value) - 1] = '\0';
  }

  return xQueueSend(sendspinCommandQueue, &cmd, 0) == pdTRUE;
}

void sendspinSendPlayerState(const char* forcedState) {
  const bool inNetworkTask = networkTaskHandle &&
    xTaskGetCurrentTaskHandle() == networkTaskHandle;

  if (!inNetworkTask && networkTaskHandle) {
    portENTER_CRITICAL(&sendspinStateMux);
    if (forcedState && forcedState[0]) {
      strncpy(sendspinPendingForcedState, forcedState, sizeof(sendspinPendingForcedState) - 1);
      sendspinPendingForcedState[sizeof(sendspinPendingForcedState) - 1] = '\0';
    } else {
      sendspinPendingForcedState[0] = '\0';
    }
    sendspinPlayerStatePending = true;
    portEXIT_CRITICAL(&sendspinStateMux);
    return;
  }

  sendspinSendPlayerStateNow(forcedState);
}

void sendspinSendControllerCommand(const char* command) {
  if (!command || !command[0]) return;

  const bool inNetworkTask = networkTaskHandle &&
    xTaskGetCurrentTaskHandle() == networkTaskHandle;

  if (!inNetworkTask && networkTaskHandle && sendspinCommandQueue) {
    if (!queueSendspinOutbound(SENDSPIN_OUT_CONTROLLER, command)) {
      Serial.println(String("Sendspin command queue full: ") + command);
    }
    return;
  }

  sendspinSendControllerCommandNow(command);
}

void serviceSendspinCommandQueue() {
  // Ovládací povely mají přednost před kosmetickou aktualizací stavu.
  if (sendspinCommandQueue) {
    SendspinOutboundCommand cmd;
    uint8_t processed = 0;
    while (processed < 4 && xQueueReceive(sendspinCommandQueue, &cmd, 0) == pdTRUE) {
      if (cmd.type == SENDSPIN_OUT_CONTROLLER) {
        sendspinSendControllerCommandNow(cmd.value);
      }
      processed++;
    }
  }

  bool statePending = false;
  char forcedState[sizeof(sendspinPendingForcedState)] = {0};

  portENTER_CRITICAL(&sendspinStateMux);
  if (sendspinPlayerStatePending) {
    statePending = true;
    strncpy(forcedState, sendspinPendingForcedState, sizeof(forcedState) - 1);
    forcedState[sizeof(forcedState) - 1] = '\0';
    sendspinPlayerStatePending = false;
  }
  portEXIT_CRITICAL(&sendspinStateMux);

  if (statePending) {
    sendspinSendPlayerStateNow(forcedState[0] ? forcedState : nullptr);
  }
}

void sendspinSetExternalSource(bool external) {
  if (sendspinExternalSource == external) return;
  sendspinExternalSource = external;
  if (external) {
    sendspinStreamActive = false;
    sendspinGroupPlaying = false;
    sendspinClearPcm();
  }
  sendspinSendPlayerState(external ? "external_source" : "synchronized");
}

void sendspinSendControllerCommandNow(const char* command) {
  if (!sendspinProtocolReady || !command || !command[0]) return;
  String message = "{\"type\":\"client/command\",\"payload\":{\"controller\":{\"command\":\"" +
                   String(command) + "\"}}}";
  sendspinSendText(message);
}

static void sendspinRequestPcmFormat() {
  if (!sendspinProtocolReady) return;
  uint32_t rate = activeOutputIsUsb() ? audioOutputFormat.sampleRate : 48000;
  uint8_t channels = activeOutputIsUsb() ? audioOutputFormat.channels : 2;
  uint8_t bits = activeOutputIsUsb() ? audioOutputFormat.bitsPerSample : 16;
  if ((rate != 44100 && rate != 48000) || channels != 2 || bits != 16) {
    rate = 48000;
    channels = 2;
    bits = 16;
  }

  String request = "{\"type\":\"stream/request-format\",\"payload\":{\"player\":{";
  request += "\"codec\":\"pcm\",\"channels\":" + String(channels) +
             ",\"sample_rate\":" + String(rate) +
             ",\"bit_depth\":" + String(bits) + "}}}";
  sendspinSendText(request);
}

static void sendspinEndStream(const String& reason) {
  bool wasNetworkStream = sendspinStreamActive;
  sendspinStreamActive = false;
  sendspinGroupPlaying = false;
  sendspinPlaybackState = "stopped";
  sendspinClearPcm();

  if (wasNetworkStream) {
    resetAudioEqualizers();
    audioPlaying = false;
    audioPaused = false;
    audioLevel = 0;
    if (activeOutputIsI2s()) i2s_zero_dma_buffer(I2S_AUDIO_PORT);
  }

  sendspinStatus = reason.length() > 0 ? reason : String("Síťový reproduktor čeká");
  if (wasNetworkStream || !sendspinExternalSource) {
    audioStatus = sendspinStatus;
  }
}

static void sendspinStartPcmStream(uint32_t sampleRate, uint8_t channels, uint8_t bitDepth) {
  bool localSourceActive = (audioPlaying || audioPaused || radioPlaying || playlistActive) &&
                           !sendspinStreamActive;
  if (localSourceActive) {
    playlistActive = false;
    stopAudioPlayback("");
    saveRadioResumeState(false, -1, "");
  }

  sendspinExternalSource = false;
  sendspinSampleRate = sampleRate;
  sendspinChannels = channels;
  sendspinBitDepth = bitDepth;
  audioOutputFormat = {sampleRate, channels, bitDepth};

  if (activeOutputIsI2s() && i2sAudioStarted) {
    i2s_channel_t channelMode = channels == 1 ? I2S_CHANNEL_MONO : I2S_CHANNEL_STEREO;
    i2s_set_clk(I2S_AUDIO_PORT, sampleRate, I2S_BITS_PER_SAMPLE_16BIT, channelMode);
    i2s_zero_dma_buffer(I2S_AUDIO_PORT);
  }

  sendspinClearPcm();
  resetAudioEqualizers();
  radioPlaying = false;
  playlistActive = false;
  audioDisk = "";
  audioPath = "";
  audioPaused = false;
  audioPlaying = true;
  sendspinStreamActive = true;
  sendspinGroupPlaying = true;
  sendspinPlaybackState = "playing";
  sendspinStatus = "Síťový reproduktor: přijímám stream";
  audioStatus = sendspinStatus;
  markAudioPositionStop();
  sendspinSendPlayerState("synchronized");
}

static void sendspinHandleMetadata(const String& metadata) {
  String value;
  bool isNull = false;

  if (sendspinJsonGetNullableString(metadata, "title", value, isNull)) {
    sendspinTitle = isNull ? String("") : value;
  }
  if (sendspinJsonGetNullableString(metadata, "artist", value, isNull)) {
    sendspinArtist = isNull ? String("") : value;
  }
  if (sendspinJsonGetNullableString(metadata, "album", value, isNull)) {
    sendspinAlbum = isNull ? String("") : value;
  }
  if (sendspinJsonGetNullableString(metadata, "artwork_url", value, isNull)) {
    sendspinArtworkUrl = isNull ? String("") : value;
  }

  int64_t timestamp = 0;
  if (sendspinJsonGetInt64(metadata, "timestamp", timestamp)) {
    sendspinMetadataTimestampServerUs = timestamp;
  }

  String progress;
  if (sendspinJsonExtractObject(metadata, "progress", progress)) {
    int64_t number = 0;
    if (sendspinJsonGetInt64(progress, "track_progress", number)) {
      sendspinTrackProgressMs = number < 0 ? 0 : (uint32_t)number;
    }
    if (sendspinJsonGetInt64(progress, "track_duration", number)) {
      sendspinTrackDurationMs = number < 0 ? 0 : (uint32_t)number;
    }
    if (sendspinJsonGetInt64(progress, "playback_speed", number)) {
      sendspinPlaybackSpeed = (int32_t)number;
    }
  } else {
    int pos = sendspinJsonValuePos(metadata, "progress");
    if (pos >= 0 && metadata.startsWith("null", pos)) {
      sendspinTrackProgressMs = 0;
      sendspinTrackDurationMs = 0;
      sendspinPlaybackSpeed = 0;
    }
  }

  if (sendspinTitle.length() > 0) {
    sendspinStatus = "Síťový reproduktor: " + sendspinTitle;
  } else if (sendspinArtist.length() > 0) {
    sendspinStatus = "Síťový reproduktor: " + sendspinArtist;
  } else {
    sendspinStatus = "Síťový reproduktor: přehrávání";
  }
  audioStatus = sendspinStatus;
}

static void sendspinHandleController(const String& controller) {
  int supportedPos = controller.indexOf("\"supported_commands\"");
  if (supportedPos >= 0) {
    int arrayEnd = controller.indexOf(']', supportedPos);
    String supported = arrayEnd > supportedPos
      ? controller.substring(supportedPos, arrayEnd + 1)
      : controller.substring(supportedPos);

    sendspinControllerCanPlay = supported.indexOf("\"play\"") >= 0;
    sendspinControllerCanPause = supported.indexOf("\"pause\"") >= 0;
    sendspinControllerCanStop = supported.indexOf("\"stop\"") >= 0;
    sendspinControllerCanNext = supported.indexOf("\"next\"") >= 0;
    sendspinControllerCanPrevious = supported.indexOf("\"previous\"") >= 0;
    sendspinControllerCanVolume = supported.indexOf("\"volume\"") >= 0;
    sendspinControllerCanMute = supported.indexOf("\"mute\"") >= 0;
  }
}

static void sendspinHandleServerTime(const String& payload) {
  int64_t clientTransmitted = 0;
  int64_t serverReceived = 0;
  int64_t serverTransmitted = 0;
  if (!sendspinJsonGetInt64(payload, "client_transmitted", clientTransmitted) ||
      !sendspinJsonGetInt64(payload, "server_received", serverReceived) ||
      !sendspinJsonGetInt64(payload, "server_transmitted", serverTransmitted)) {
    return;
  }

  int64_t clientReceived = esp_timer_get_time();
  int64_t sampleOffset = ((serverReceived - clientTransmitted) +
                          (serverTransmitted - clientReceived)) / 2;

  if (sendspinPcmMutex && xSemaphoreTake(sendspinPcmMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    if (!sendspinClockSynced) {
      sendspinClockOffsetUs = sampleOffset;
      sendspinClockSynced = true;
    } else {
      sendspinClockOffsetUs = (sendspinClockOffsetUs * 7 + sampleOffset) / 8;
    }
    xSemaphoreGive(sendspinPcmMutex);
  } else {
    sendspinClockOffsetUs = sampleOffset;
    sendspinClockSynced = true;
  }
}

static void sendspinHandleText(const uint8_t* data, size_t length) {
  if (!data || length == 0) return;

  String message;
  message.reserve(length + 1);
  for (size_t i = 0; i < length; i++) message += (char)data[i];

  String type;
  if (!sendspinJsonGetString(message, "type", type)) return;

  String payload;
  sendspinJsonExtractObject(message, "payload", payload);

  if (type == "server/hello") {
    sendspinProtocolReady = true;
    sendspinJsonGetString(payload, "server_id", sendspinServerId);
    sendspinTimeSyncBurstLeft = 8;
    sendspinLastTimeSyncMs = 0;
    sendspinStatus = "Síťový reproduktor připojen";
    sendspinSendPlayerState();
    sendspinSendTime();
    return;
  }

  if (type == "server/time") {
    sendspinHandleServerTime(payload);
    if (sendspinTimeSyncBurstLeft > 0) sendspinTimeSyncBurstLeft--;
    return;
  }

  if (type == "stream/start") {
    String player;
    if (!sendspinJsonExtractObject(payload, "player", player)) return;

    String codec;
    int64_t sampleRate = 0;
    int64_t channels = 0;
    int64_t bitDepth = 0;
    sendspinJsonGetString(player, "codec", codec);
    sendspinJsonGetInt64(player, "sample_rate", sampleRate);
    sendspinJsonGetInt64(player, "channels", channels);
    sendspinJsonGetInt64(player, "bit_depth", bitDepth);

    bool formatSupported = codec == "pcm" &&
                           (sampleRate == 44100 || sampleRate == 48000) &&
                           channels == 2 && bitDepth == 16;
    bool outputSupported = audioReady &&
      (activeOutputIsI2s() ||
       (activeOutputIsUsb() &&
        sampleRate == audioOutputFormat.sampleRate &&
        channels == audioOutputFormat.channels &&
        bitDepth == audioOutputFormat.bitsPerSample));
    if (!formatSupported || !outputSupported) {
      sendspinStatus = audioReady
        ? String("Sendspin: žádám formát audio výstupu")
        : String("Sendspin: audio výstup není připravený");
      sendspinRequestPcmFormat();
      return;
    }

    sendspinStartPcmStream((uint32_t)sampleRate, (uint8_t)channels, (uint8_t)bitDepth);
    return;
  }

  if (type == "stream/clear") {
    if (payload.length() == 0 || payload.indexOf("\"player\"") >= 0) {
      sendspinClearPcm();
      if (activeOutputIsI2s()) i2s_zero_dma_buffer(I2S_AUDIO_PORT);
    }
    return;
  }

  if (type == "stream/end") {
    if (payload.length() == 0 || payload.indexOf("\"player\"") >= 0 ||
        payload.indexOf("\"roles\"") < 0) {
      sendspinEndStream("Síťový reproduktor čeká");
    }
    return;
  }

  if (type == "group/update") {
    String state;
    if (sendspinJsonGetString(payload, "playback_state", state)) {
      sendspinPlaybackState = state;
      if (state == "playing") {
        sendspinGroupPlaying = true;
        if (sendspinStreamActive) {
          audioPlaying = true;
          audioPaused = false;
        }
      } else if (state == "stopped") {
        sendspinGroupPlaying = false;
        audioPlaying = false;
        audioPaused = sendspinStreamActive;
      }
    }
    return;
  }

  if (type == "server/state") {
    String metadata;
    if (sendspinJsonExtractObject(payload, "metadata", metadata)) {
      sendspinHandleMetadata(metadata);
    }
    String controller;
    if (sendspinJsonExtractObject(payload, "controller", controller)) {
      sendspinHandleController(controller);
    }
    return;
  }

  if (type == "server/command") {
    String player;
    if (!sendspinJsonExtractObject(payload, "player", player)) return;

    String command;
    if (!sendspinJsonGetString(player, "command", command)) return;

    if (command == "volume") {
      int64_t volume = cfg.audioVolume;
      if (sendspinJsonGetInt64(player, "volume", volume)) {
        cfg.audioVolume = constrain((int)volume, 0, 100);
        requestAudioGainApply();
        requestAudioVolumeSave();
        sendspinSendPlayerState();
      }
    } else if (command == "mute") {
      bool muted = false;
      if (sendspinJsonGetBool(player, "mute", muted)) {
        sendspinMuted = muted;
        sendspinSendPlayerState();
      }
    }
    return;
  }
}

static int64_t sendspinReadBigEndian64(const uint8_t* data) {
  uint64_t value = 0;
  for (int i = 0; i < 8; i++) {
    value = (value << 8) | (uint64_t)data[i];
  }
  return (int64_t)value;
}

static void sendspinHandleBinary(uint8_t* data, size_t length) {
  if (!data || length < 9) return;
  uint8_t messageType = data[0];

  if (messageType == 4 && sendspinStreamActive) {
    int64_t timestamp = sendspinReadBigEndian64(data + 1);
    sendspinQueuePcm(timestamp, data + 9, length - 9);
  }
}

static void sendspinDisconnectClient(const String& reason) {
  if (sendspinClient) sendspinClient.stop();
  sendspinWebSocketReady = false;
  sendspinProtocolReady = false;
  sendspinMobileClientActive = false;
  sendspinHttpHeaders = "";
  sendspinWsRxUsed = 0;
  sendspinServerId = "";
  sendspinClockSynced = false;
  sendspinClockOffsetUs = 0;
  sendspinTimeSyncBurstLeft = 0;
  sendspinLastActivityMs = 0;
  sendspinEndStream(reason.length() ? reason : String("Síťový reproduktor čeká"));
  sendspinTitle = "";
  sendspinArtist = "";
  sendspinAlbum = "";
  sendspinArtworkUrl = "";
  sendspinTrackProgressMs = 0;
  sendspinTrackDurationMs = 0;
  sendspinPlaybackSpeed = 0;
  sendspinMetadataTimestampServerUs = 0;
}

static bool sendspinHandleHttpHandshake() {
  int requestEnd = sendspinHttpHeaders.indexOf("\r\n\r\n");
  if (requestEnd < 0) return false;

  int firstLineEnd = sendspinHttpHeaders.indexOf("\r\n");
  String firstLine = firstLineEnd >= 0
    ? sendspinHttpHeaders.substring(0, firstLineEnd)
    : sendspinHttpHeaders;

  String key = sendspinHeaderValue(sendspinHttpHeaders, "Sec-WebSocket-Key");
  String userAgent = sendspinHeaderValue(sendspinHttpHeaders, "User-Agent");
  bool pathOk = firstLine.startsWith("GET " + String(SENDSPIN_PATH) + " ");
  if (!pathOk || key.length() == 0) {
    sendspinClient.print("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n");
    sendspinDisconnectClient("Sendspin: neplatný WebSocket požadavek");
    return false;
  }

  String accept = sendspinWebSocketAccept(key);
  if (accept.length() == 0) {
    sendspinDisconnectClient("Sendspin: SHA1 handshake selhal");
    return false;
  }

  sendspinClient.print("HTTP/1.1 101 Switching Protocols\r\n");
  sendspinClient.print("Upgrade: websocket\r\n");
  sendspinClient.print("Connection: Upgrade\r\n");
  sendspinClient.print("Sec-WebSocket-Accept: ");
  sendspinClient.print(accept);
  sendspinClient.print("\r\n\r\n");

  sendspinWebSocketReady = true;
  sendspinProtocolReady = false;
  sendspinMobileClientActive = userAgent.startsWith("ORIS-Mobile-Audio/");
  sendspinHttpHeaders = "";
  sendspinWsRxUsed = 0;
  sendspinLastActivityMs = millis();
  sendspinStatus = "Sendspin WebSocket připojen";
  sendspinSendHello();
  return true;
}

static void sendspinReadHandshake() {
  while (sendspinClient.available() > 0 && !sendspinWebSocketReady) {
    char c = (char)sendspinClient.read();
    if (sendspinHttpHeaders.length() >= 4096) {
      sendspinDisconnectClient("Sendspin: HTTP hlavička je příliš dlouhá");
      return;
    }
    sendspinHttpHeaders += c;
    sendspinLastActivityMs = millis();
    if (sendspinHttpHeaders.endsWith("\r\n\r\n")) {
      sendspinHandleHttpHandshake();
      return;
    }
  }
}

static void sendspinProcessWebSocketFrames() {
  size_t parsed = 0;

  while (sendspinWsRxUsed - parsed >= 2) {
    uint8_t* frame = sendspinWsRx + parsed;
    bool fin = (frame[0] & 0x80) != 0;
    uint8_t opcode = frame[0] & 0x0F;
    bool masked = (frame[1] & 0x80) != 0;
    uint64_t payloadLength = frame[1] & 0x7F;
    size_t headerLength = 2;

    if (payloadLength == 126) {
      if (sendspinWsRxUsed - parsed < 4) break;
      payloadLength = ((uint64_t)frame[2] << 8) | frame[3];
      headerLength = 4;
    } else if (payloadLength == 127) {
      if (sendspinWsRxUsed - parsed < 10) break;
      payloadLength = 0;
      for (int i = 0; i < 8; i++) payloadLength = (payloadLength << 8) | frame[2 + i];
      headerLength = 10;
    }

    size_t maskOffset = headerLength;
    if (masked) headerLength += 4;

    if (!fin || payloadLength > SENDSPIN_WS_RX_BYTES - headerLength) {
      sendspinDisconnectClient("Sendspin: nepodporovaný WebSocket rámec");
      return;
    }

    uint64_t frameLength64 = (uint64_t)headerLength + payloadLength;
    if (frameLength64 > SIZE_MAX) {
      sendspinDisconnectClient("Sendspin: WebSocket rámec je příliš velký");
      return;
    }
    size_t frameLength = (size_t)frameLength64;
    if (sendspinWsRxUsed - parsed < frameLength) break;

    uint8_t* payload = frame + headerLength;
    if (masked) {
      uint8_t* mask = frame + maskOffset;
      for (size_t i = 0; i < (size_t)payloadLength; i++) {
        payload[i] ^= mask[i & 3U];
      }
    }

    sendspinLastActivityMs = millis();

    if (opcode == 0x1) {
      sendspinHandleText(payload, (size_t)payloadLength);
    } else if (opcode == 0x2) {
      sendspinHandleBinary(payload, (size_t)payloadLength);
    } else if (opcode == 0x8) {
      sendspinSendFrame(0x8, payload, (size_t)payloadLength);
      sendspinDisconnectClient("Síťový reproduktor odpojen");
      return;
    } else if (opcode == 0x9) {
      sendspinSendFrame(0xA, payload, (size_t)payloadLength);
    }

    parsed += frameLength;
  }

  if (parsed > 0 && parsed <= sendspinWsRxUsed) {
    size_t remaining = sendspinWsRxUsed - parsed;
    if (remaining > 0) memmove(sendspinWsRx, sendspinWsRx + parsed, remaining);
    sendspinWsRxUsed = remaining;
  }
}

static void sendspinReadWebSocket() {
  if (!sendspinWsRx) return;

  while (sendspinClient.available() > 0) {
    if (sendspinWsRxUsed >= SENDSPIN_WS_RX_BYTES) {
      sendspinDisconnectClient("Sendspin: přetekl WebSocket buffer");
      return;
    }

    size_t freeBytes = SENDSPIN_WS_RX_BYTES - sendspinWsRxUsed;
    int available = sendspinClient.available();
    size_t wanted = available > 0 ? (size_t)available : 1;
    if (wanted > freeBytes) wanted = freeBytes;
    int received = sendspinClient.read(sendspinWsRx + sendspinWsRxUsed, wanted);
    if (received <= 0) break;
    sendspinWsRxUsed += (size_t)received;
    sendspinLastActivityMs = millis();

    sendspinProcessWebSocketFrames();
    if (!sendspinClient.connected()) return;
  }
}

static bool startSendspinServer() {
  if (sendspinServerStarted) return true;
  if (!sendspinEnsureBuffers()) return false;

  sendspinServer.begin();
  sendspinServer.setNoDelay(true);
  sendspinServerStarted = true;
  sendspinStatus = "Síťový reproduktor čeká na Music Assistant";
  Serial.printf("Sendspin server: ws://%s.local:%u%s\n",
                normalizeMdnsName(cfg.mdnsName).c_str(), SENDSPIN_PORT, SENDSPIN_PATH);
  return true;
}

void stopSendspinService(bool stopListener) {
  if (sendspinClient) {
    if (sendspinWebSocketReady) {
      String bye = "{\"type\":\"client/goodbye\",\"payload\":{\"reason\":\"shutdown\"}}";
      sendspinSendText(bye);
    }
    sendspinDisconnectClient("Síťový reproduktor vypnut");
  }

  if (stopListener && sendspinServerStarted) {
    sendspinServer.end();
    sendspinServerStarted = false;
    sendspinStatus = "Síťový reproduktor vypnut";
  }
}

void serviceSendspin() {
  if (!cfg.smartSpeakerEnabled) {
    if (sendspinServerStarted || sendspinClient) stopSendspinService(true);
    return;
  }

  bool networkAvailable = WiFi.status() == WL_CONNECTED || apRunning;
  if (!networkAvailable) return;
  if (!startSendspinServer()) return;

  WiFiClient incoming = sendspinServer.available();
  if (incoming) {
    if (sendspinClient && sendspinClient.connected() && sendspinMobileClientActive) {
      incoming.print("HTTP/1.1 409 Conflict\r\n");
      incoming.print("Connection: close\r\n");
      incoming.print("Content-Type: text/plain; charset=utf-8\r\n\r\n");
      incoming.print("ORIS Mobile Audio má právě přednost.\r\n");
      incoming.stop();
      Serial.println("Sendspin: odmítnuto další spojení, mobilní přehrávač je aktivní");
    } else {
      if (sendspinClient) {
        sendspinDisconnectClient("Sendspin: připojuje se nový server");
      }
      sendspinClient = incoming;
      sendspinClient.setNoDelay(true);
      sendspinClient.setTimeout(50);
      sendspinHttpHeaders = "";
      sendspinWsRxUsed = 0;
      sendspinWebSocketReady = false;
      sendspinProtocolReady = false;
      sendspinMobileClientActive = false;
      sendspinLastActivityMs = millis();
      sendspinStatus = "Sendspin: navazuji spojení";
      Serial.printf("Sendspin client connected from %s\n", sendspinClient.remoteIP().toString().c_str());
    }
  }

  if (!sendspinClient) return;
  if (!sendspinClient.connected()) {
    sendspinDisconnectClient("Síťový reproduktor: spojení přerušeno");
    return;
  }

  if (!sendspinWebSocketReady) {
    sendspinReadHandshake();
  } else {
    sendspinReadWebSocket();
  }

  if (!sendspinClient || !sendspinClient.connected()) return;

  uint32_t now = millis();
  if (sendspinProtocolReady) {
    uint32_t interval = sendspinTimeSyncBurstLeft > 0
      ? SENDSPIN_TIME_SYNC_FAST_MS
      : SENDSPIN_TIME_SYNC_INTERVAL_MS;
    if (now - sendspinLastTimeSyncMs >= interval) sendspinSendTime();
  }

  if (sendspinLastActivityMs > 0 && now - sendspinLastActivityMs > SENDSPIN_CLIENT_TIMEOUT_MS) {
    sendspinDisconnectClient("Síťový reproduktor: časový limit spojení");
  }
}

bool sendspinIsPlaying() {
  return sendspinStreamActive && sendspinGroupPlaying;
}

uint32_t currentSendspinPositionMs() {
  if (sendspinMetadataTimestampServerUs == 0) return sendspinTrackProgressMs;

  int64_t serverNowUs = esp_timer_get_time() + sendspinClockOffsetUs;
  int64_t deltaUs = serverNowUs - sendspinMetadataTimestampServerUs;
  int64_t calculated = (int64_t)sendspinTrackProgressMs;
  if (deltaUs > 0 && sendspinPlaybackSpeed != 0) {
    calculated += (deltaUs * (int64_t)sendspinPlaybackSpeed) / 1000000LL;
  }
  if (calculated < 0) calculated = 0;
  if (sendspinTrackDurationMs > 0 && calculated > sendspinTrackDurationMs) {
    calculated = sendspinTrackDurationMs;
  }
  return (uint32_t)calculated;
}

bool serveConfigPageWithSmartSpeakerPanel() {
  if (!checkWebAuth()) return false;

  const char* path = "/www/config.html";
  if (!FFat.exists(path)) {
    server.sendHeader("Location", "/rescue");
    server.send(303);
    return false;
  }

  File file = FFat.open(path, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    server.send(500, "text/plain; charset=utf-8", "Konfiguraci nelze otevřít");
    return false;
  }

  String page = file.readString();
  file.close();

  String panel;
  panel.reserve(1400);
  panel += "<section id='oris-smart-speaker-panel' style='margin:18px auto;padding:18px;max-width:900px;";
  panel += "border:1px solid rgba(127,127,127,.35);border-radius:14px'>";
  panel += "<h2 style='margin-top:0'>Síťový reproduktor</h2>";
  panel += "<form method='post' action='/smart-speaker/save'>";
  panel += "<label style='display:flex;gap:10px;align-items:center'>";
  panel += "<input type='checkbox' name='enabled' value='1'";
  if (cfg.smartSpeakerEnabled) panel += " checked";
  panel += "> <strong>Zapnout Sendspin síťový reproduktor</strong></label>";
  panel += "<p>Název zařízení: <code>" + htmlEscape(normalizeMdnsName(cfg.mdnsName)) + "</code>. ";
  panel += "Music Assistant jej hledá na portu " + String(SENDSPIN_PORT) + ".</p>";
  panel += "<p>Stav: <strong>" + htmlEscape(sendspinStatus) + "</strong></p>";
  panel += "<button type='submit'>Uložit síťový reproduktor</button>";
  panel += "</form></section>";

  int insertAt = page.lastIndexOf("</main>");
  if (insertAt < 0) insertAt = page.lastIndexOf("</body>");
  if (insertAt < 0) page += panel;
  else page = page.substring(0, insertAt) + panel + page.substring(insertAt);

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.send(200, "text/html; charset=utf-8", page);
  return true;
}

void handleSmartSpeakerSave() {
  if (!checkWebAuth()) return;

  cfg.smartSpeakerEnabled = server.hasArg("enabled");
  if (!saveConfig()) {
    server.send(500, "text/plain; charset=utf-8", "Nastavení síťového reproduktoru nelze uložit");
    return;
  }

  if (!cfg.smartSpeakerEnabled) {
    stopSendspinService(true);
  }

  startMdns(true);
  server.sendHeader("Location", "/config");
  server.send(303);
}

// ============================================================
// Vestavěný jednoduchý FTP server
// ============================================================

const uint16_t FTP_CMD_PORT = 21;
const uint16_t FTP_DATA_PORT = 50009;

WiFiServer ftpServer(FTP_CMD_PORT);
WiFiServer ftpDataServer(FTP_DATA_PORT);
WiFiClient ftpClient;
WiFiClient ftpDataClient;

bool ftpServerStarted = false;
bool ftpLoggedIn = false;
bool ftpUserOk = false;
bool ftpPassiveReady = false;
String ftpCwd = "/";
String ftpRenameFrom = "";
String ftpLine = "";

// ============================================================
// Preferences / odložené formátování FFat
// ============================================================

bool lockPreferences(TickType_t timeout) {
  return !prefsMutex || xSemaphoreTake(prefsMutex, timeout) == pdTRUE;
}

void unlockPreferences() {
  if (prefsMutex) xSemaphoreGive(prefsMutex);
}

bool isFfatFormatRequested() {
  if (!lockPreferences()) return false;
  prefs.begin("webdisk", false);
  bool requested = prefs.getBool("fmt_ffat", false);
  prefs.end();
  unlockPreferences();
  return requested;
}

void setFfatFormatRequested(bool value) {
  if (!lockPreferences()) return;
  prefs.begin("webdisk", false);
  prefs.putBool("fmt_ffat", value);
  prefs.end();
  unlockPreferences();
}

void formatFfatOnBootIfRequested() {
  if (!isFfatFormatRequested()) {
    return;
  }

  // Nejdřív zrušit příznak, ať nevznikne boot loop.
  setFfatFormatRequested(false);

  Serial.println("FFat format requested from web.");
  Serial.println("Mounting FFat before format...");

  if (!FFat.begin(true)) {
    Serial.println("FFat begin before format failed!");
    return;
  }

  Serial.println("Formatting FFat...");
  bool ok = FFat.format();

  FFat.end();

  if (!ok) {
    Serial.println("FFat format failed!");
    return;
  }

  Serial.println("FFat format OK. Restarting...");
  delay(1000);
  ESP.restart();
}

// ============================================================
// Pomocné funkce HTML / URL / PATH
// ============================================================

String htmlEscape(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  return s;
}

String urlEncode(const String& s) {
  String out;
  char buf[4];

  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];

    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '/') {
      out += c;
    } else if (c == ' ') {
      out += "%20";
    } else {
      sprintf(buf, "%%%02X", (uint8_t)c);
      out += buf;
    }
  }

  return out;
}

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + c - 'a';
  if (c >= 'A' && c <= 'F') return 10 + c - 'A';
  return -1;
}

String urlDecode(String s) {
  String out;

  for (int i = 0; i < (int)s.length(); i++) {
    char c = s[i];

    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < (int)s.length()) {
      int h1 = hexVal(s[i + 1]);
      int h2 = hexVal(s[i + 2]);

      if (h1 >= 0 && h2 >= 0) {
        out += (char)((h1 << 4) | h2);
        i += 2;
      } else {
        out += c;
      }
    } else {
      out += c;
    }
  }

  return out;
}

bool safePath(String &path) {
  path = urlDecode(path);

  if (!path.startsWith("/")) {
    path = "/" + path;
  }

  path.replace("\\", "/");

  if (path.indexOf("..") >= 0) {
    return false;
  }

  if (path.length() < 1) {
    return false;
  }

  return true;
}

String fileNameFromPath(String path) {
  int p = path.lastIndexOf('/');
  if (p >= 0) return path.substring(p + 1);
  return path;
}

String bytesHuman(uint64_t b) {
  char buf[32];

  if (b < 1024ULL) {
    sprintf(buf, "%llu B", (unsigned long long)b);
  } else if (b < 1024ULL * 1024ULL) {
    sprintf(buf, "%.1f KB", (double)b / 1024.0);
  } else if (b < 1024ULL * 1024ULL * 1024ULL) {
    sprintf(buf, "%.1f MB", (double)b / 1024.0 / 1024.0);
  } else {
    sprintf(buf, "%.2f GB", (double)b / 1024.0 / 1024.0 / 1024.0);
  }

  return String(buf);
}

String uptimeHuman() {
  uint32_t sec = millis() / 1000UL;
  uint32_t days = sec / 86400UL;
  sec %= 86400UL;
  uint8_t hours = sec / 3600UL;
  sec %= 3600UL;
  uint8_t minutes = sec / 60UL;
  uint8_t seconds = sec % 60UL;

  char buf[32];
  if (days > 0) {
    sprintf(buf, "%ud %02u:%02u:%02u", (unsigned)days, hours, minutes, seconds);
  } else {
    sprintf(buf, "%02u:%02u:%02u", hours, minutes, seconds);
  }
  return String(buf);
}

String jsonEscape(String s) {
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  s.replace("\n", "\\n");
  s.replace("\r", "\\r");
  s.replace("\t", "\\t");
  return s;
}


void pollUsbMount();
bool startRadioStream(const String& url);
void stopAudioPlayback(const String& reason);
void serviceAudioPump();
void startBackgroundTasks();
bool queueAudioFilePlay(const String& disk, const String& path);
bool queueAudioFolderPlay(const String& disk, const String& dirPath);
bool queueAudioRadioPlay(int idx, const String& url, const String& label, bool saveResume);
bool queueAudioSimple(AudioCommandType type);
bool enqueueAudioCommand(const AudioCommand& cmd);
void processAudioCommand(const AudioCommand& cmd);
uint32_t currentAudioPositionMs();
bool initI2sAudioOutput();
void serviceI2sAudioOutput();
const char* audioOutputKindName();
void activateI2sAudioOutput(const String& status);
void deactivateAudioOutput(const String& status);
void initEncoderControl();
void serviceEncoderControl();
void toggleAudioPauseInternal();

// ============================================================
// mDNS / stav rádia / jednorázový USB mount
// ============================================================

String normalizeMdnsName(String name) {
  name.trim();
  name.toLowerCase();

  if (name.endsWith(".local")) {
    name = name.substring(0, name.length() - 6);
  }

  String out;
  bool lastHyphen = false;

  for (size_t i = 0; i < name.length() && out.length() < 32; i++) {
    char c = name[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');

    if (ok) {
      out += c;
      lastHyphen = false;
    } else if ((c == '-' || c == '_' || c == ' ' || c == '.') && out.length() > 0 && !lastHyphen) {
      out += '-';
      lastHyphen = true;
    }
  }

  while (out.endsWith("-")) {
    out.remove(out.length() - 1);
  }

  if (out.length() == 0) {
    out = "oris-radio";
  }

  return out;
}

void startMdns(bool forceRestart) {
  String host = normalizeMdnsName(cfg.mdnsName);

  if (mdnsStarted && !forceRestart && mdnsStartedName == host) {
    return;
  }

  if (mdnsStarted) {
    MDNS.end();
    mdnsStarted = false;
    mdnsStartedWithSta = false;
    mdnsStartedName = "";
    delay(20);
  }

  if (!MDNS.begin(host.c_str())) {
    Serial.println("mDNS start failed: " + host);
    return;
  }

  mdnsStarted = true;
  mdnsStartedWithSta = (WiFi.status() == WL_CONNECTED);
  mdnsStartedName = host;
  MDNS.addService("http", "tcp", 80);
  if (cfg.ftpEnabled) {
    MDNS.addService("ftp", "tcp", 21);
  }
  if (cfg.smartSpeakerEnabled) {
    MDNS.addService("sendspin", "tcp", SENDSPIN_PORT);
    MDNS.addServiceTxt("sendspin", "tcp", "path", SENDSPIN_PATH);
    MDNS.addServiceTxt("sendspin", "tcp", "name", host.c_str());
  }

  Serial.println("mDNS: http://" + host + ".local/");
  if (cfg.smartSpeakerEnabled) {
    Serial.println("Sendspin mDNS: " + host + " port " + String(SENDSPIN_PORT));
  }
}

void serviceMdns() {
  if (mdnsStarted) {
    if (!mdnsStartedWithSta && WiFi.status() == WL_CONNECTED) {
      startMdns(true);
    }
    return;
  }

  uint32_t now = millis();
  if (now - lastMdnsStartTryMs < 5000) {
    return;
  }
  lastMdnsStartTryMs = now;

  if (WiFi.status() == WL_CONNECTED || WiFi.softAPIP() != IPAddress(0, 0, 0, 0)) {
    startMdns(false);
  }
}

void saveRadioResumeState(bool wanted, int idx, const String& url) {
  if (radioResumeSavedWanted == wanted &&
      radioResumeSavedIndex == idx &&
      radioResumeSavedUrl == url) {
    return;
  }

  if (!lockPreferences()) {
    Serial.println("NVS radio resume: mutex timeout");
    return;
  }
  prefs.begin("radio", false);
  prefs.putBool("resume", wanted);
  prefs.putInt("idx", idx);
  prefs.putString("url", url);
  prefs.end();
  unlockPreferences();

  radioResumeSavedWanted = wanted;
  radioResumeSavedIndex = idx;
  radioResumeSavedUrl = url;

  radioResumeWanted = wanted;
  radioResumeIndex = idx;
  radioResumeUrl = url;
}

void loadRadioResumeState() {
  if (!lockPreferences()) {
    Serial.println("NVS radio resume load: mutex timeout");
    return;
  }
  prefs.begin("radio", true);
  radioResumeWanted = prefs.getBool("resume", false);
  radioResumeIndex = prefs.getInt("idx", 0);
  radioResumeUrl = prefs.getString("url", "");
  prefs.end();
  unlockPreferences();

  if (radioResumeIndex < 0 || radioResumeIndex >= MAX_RADIO_STATIONS) {
    radioResumeIndex = 0;
  }

  radioResumeSavedWanted = radioResumeWanted;
  radioResumeSavedIndex = radioResumeIndex;
  radioResumeSavedUrl = radioResumeUrl;

  if (radioResumeWanted) {
    radioResumeAttempted = false;
    Serial.println("Radio resume requested from NVS");
  }
}

void serviceRadioResume() {
  if (!usbStartupSettled()) {
    return;
  }
  if (!radioResumeWanted || radioResumeAttempted || audioPlaying || uploadActive) {
    return;
  }

  if (millis() < 3000) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (!audioReady) {
    return;
  }

  String url = "";
  int idx = radioResumeIndex;

  if (idx >= 0 && idx < MAX_RADIO_STATIONS && cfg.radioUrl[idx].length() > 0) {
    url = cfg.radioUrl[idx];
  } else {
    url = radioResumeUrl;
    idx = -1;
  }

  url.trim();
  if (url.length() == 0) {
    radioResumeAttempted = true;
    saveRadioResumeState(false, -1, "");
    audioStatus = "Radio autostart: není uložená URL";
    radioStatus = audioStatus;
    return;
  }

  radioResumeAttempted = true;

  String station = url;
  if (idx >= 0 && idx < MAX_RADIO_STATIONS && cfg.radioName[idx].length() > 0) {
    station = cfg.radioName[idx];
  }

  Serial.println("Radio autostart queued: " + url);

  if (queueAudioRadioPlay(idx, url, station, true)) {
    audioStatus = "Webradio: startuji " + station;
    radioStatus = audioStatus;
  } else {
    audioStatus = "Radio autostart: fronta je plná";
    radioStatus = audioStatus;
    Serial.println(audioStatus);
  }
}

void serviceUsbBootMountOnce() {
  if (!usbBootMountPending) {
    return;
  }

  if (!usbStartupSettled()) {
    return;
  }

  usbBootMountPending = false;

  Serial.printf(
    "USB startup settled: total=%lu ms, quiet=%lu ms, audioReady=%u, mounting MSC...\n",
    (unsigned long)(millis() - usbHostStartMs),
    (unsigned long)(millis() - usbLastDeviceEventMs),
    audioReady ? 1 : 0
  );

  pollUsbMount();
}

// ============================================================
// Konfigurace
// ============================================================

void setDefaultCfg() {
  cfg.apSsid = DEFAULT_AP_SSID;
  cfg.apPass = DEFAULT_AP_PASS;
  cfg.apMode = AP_MODE_ALWAYS;
  cfg.staSsid = "";
  cfg.staPass = "";
  cfg.wifiCount = 0;
  for (uint8_t i = 0; i < MAX_SAVED_WIFI; i++) {
    cfg.wifiSsid[i] = "";
    cfg.wifiPass[i] = "";
  }
  cfg.mdnsName = "oris-radio";
  cfg.smartSpeakerEnabled = true;
  cfg.webUser = "admin";
  cfg.webPass = "admin";
  cfg.ftpEnabled = true;
  cfg.ftpUser = "ftp";
  cfg.ftpPass = "12345678";
  cfg.ftpDisk = "usb0";
  cfg.rgbEnabled = false;
  cfg.audioVolume = 80;
  cfg.audioBassDb = 0;
  cfg.audioTrebleDb = 0;
  cfg.audioEqEnabled = true;
  cfg.audioEqPreampDb = 0;
  cfg.audioEqAutoHeadroom = true;
  for (uint8_t i = 0; i < AUDIO_EQ_BANDS; i++) cfg.audioEqBandDb[i] = 0;
  // Bezpečný výchozí strop: 100 % hlasitosti = -12 dB.
  cfg.audioOutputGainDb = -12;
  // Jemnější spodní část rozsahu, aby už 20–30 % nebylo příliš hlasité.
  cfg.audioVolumeCurve = 1.8f;
  cfg.batteryEnabled = false;
  cfg.batteryDividerRatio = 2.0f;
  cfg.batteryCalibration = 1.0f;
  for (uint8_t i = 0; i < MAX_RADIO_STATIONS; i++) {
    cfg.radioName[i] = "";
    cfg.radioUrl[i] = "";
    cfg.radioLogo[i] = "";
  }
  cfg.radioName[0] = "Moje radio";
  cfg.radioUrl[0] = "";
}

void loadConfig() {
  setDefaultCfg();
  bool detailedEqLoaded = false;

  if (!FFat.exists(CONFIG_FILE)) {
    return;
  }

  File f = FFat.open(CONFIG_FILE, FILE_READ);
  if (!f) {
    return;
  }

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();

    if (line.length() == 0) continue;
    if (line.startsWith("#")) continue;

    int eq = line.indexOf('=');
    if (eq <= 0) continue;

    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);

    key.trim();
    val.trim();

    if (key == "ap_ssid") cfg.apSsid = val;
    else if (key == "ap_pass") cfg.apPass = val;
    else if (key == "ap_mode") {
      int mode = val.toInt();
      if (mode < AP_MODE_ALWAYS || mode > AP_MODE_OFF) mode = AP_MODE_ALWAYS;
      cfg.apMode = (ApOperatingMode)mode;
    }
    else if (key == "sta_ssid") cfg.staSsid = val;
    else if (key == "sta_pass") cfg.staPass = val;
    else if (key == "wifi_count") cfg.wifiCount = (uint8_t)constrain(val.toInt(), 0, MAX_SAVED_WIFI);
    else if (key.startsWith("wifi_ssid_")) {
      int idx = key.substring(10).toInt();
      if (idx >= 0 && idx < MAX_SAVED_WIFI) cfg.wifiSsid[idx] = val;
    }
    else if (key.startsWith("wifi_pass_")) {
      int idx = key.substring(10).toInt();
      if (idx >= 0 && idx < MAX_SAVED_WIFI) cfg.wifiPass[idx] = val;
    }
    else if (key == "mdns_name") cfg.mdnsName = val;
    else if (key == "smart_speaker_enabled") cfg.smartSpeakerEnabled = (val == "1" || val == "true" || val == "ON");
    else if (key == "web_user") cfg.webUser = val;
    else if (key == "web_pass") cfg.webPass = val;
    else if (key == "ftp_enabled") cfg.ftpEnabled = (val == "1" || val == "true" || val == "ON");
    else if (key == "ftp_user") cfg.ftpUser = val;
    else if (key == "ftp_pass") cfg.ftpPass = val;
    else if (key == "ftp_disk") cfg.ftpDisk = val;
    else if (key == "rgb_enabled") cfg.rgbEnabled = (val == "1" || val == "true" || val == "ON");
    else if (key == "audio_volume") cfg.audioVolume = val.toInt();
    else if (key == "audio_bass_db") cfg.audioBassDb = val.toInt();
    else if (key == "audio_treble_db") cfg.audioTrebleDb = val.toInt();
    else if (key == "audio_eq_enabled") cfg.audioEqEnabled = (val == "1" || val == "true" || val == "ON");
    else if (key == "audio_eq_preamp_db") cfg.audioEqPreampDb = val.toInt();
    else if (key == "audio_eq_auto_headroom") cfg.audioEqAutoHeadroom = (val == "1" || val == "true" || val == "ON");
    else if (key == "audio_output_gain_db") cfg.audioOutputGainDb = val.toInt();
    else if (key == "audio_volume_curve") cfg.audioVolumeCurve = val.toFloat();
    else if (key.startsWith("audio_eq_band_")) {
      int idx = key.substring(14).toInt();
      if (idx >= 0 && idx < AUDIO_EQ_BANDS) {
        cfg.audioEqBandDb[idx] = val.toInt();
        detailedEqLoaded = true;
      }
    }
    else if (key == "battery_enabled") cfg.batteryEnabled = (val == "1" || val == "true" || val == "ON");
    else if (key == "battery_divider_ratio") cfg.batteryDividerRatio = val.toFloat();
    else if (key == "battery_calibration") cfg.batteryCalibration = val.toFloat();
    else if (key == "radio_name") cfg.radioName[0] = val;      // kompatibilita se starším configem
    else if (key == "radio_url") cfg.radioUrl[0] = val;        // kompatibilita se starším configem
    else if (key.startsWith("radio_name_")) {
      int idx = key.substring(11).toInt();
      if (idx >= 0 && idx < MAX_RADIO_STATIONS) cfg.radioName[idx] = val;
    }
    else if (key.startsWith("radio_url_")) {
      int idx = key.substring(10).toInt();
      if (idx >= 0 && idx < MAX_RADIO_STATIONS) cfg.radioUrl[idx] = val;
    }
    else if (key.startsWith("radio_logo_")) {
      int idx = key.substring(11).toInt();
      if (idx >= 0 && idx < MAX_RADIO_STATIONS) cfg.radioLogo[idx] = val;
    }
  }

  f.close();

  if (cfg.apSsid.length() == 0) {
    cfg.apSsid = DEFAULT_AP_SSID;
  }

  if (cfg.apPass.length() > 0 && cfg.apPass.length() < 8) {
    cfg.apPass = DEFAULT_AP_PASS;
  }

  if (cfg.apMode < AP_MODE_ALWAYS || cfg.apMode > AP_MODE_OFF) {
    cfg.apMode = AP_MODE_ALWAYS;
  }

  uint8_t detectedWifiCount = 0;
  for (uint8_t i = 0; i < MAX_SAVED_WIFI; i++) {
    cfg.wifiSsid[i].trim();
    if (cfg.wifiSsid[i].length() > 0) detectedWifiCount = i + 1;
  }
  if (detectedWifiCount > cfg.wifiCount) cfg.wifiCount = detectedWifiCount;
  if (cfg.wifiCount > MAX_SAVED_WIFI) cfg.wifiCount = MAX_SAVED_WIFI;

  // Migrace starého configu s jediným sta_ssid / sta_pass.
  if (cfg.wifiCount == 0 && cfg.staSsid.length() > 0) {
    cfg.wifiSsid[0] = cfg.staSsid;
    cfg.wifiPass[0] = cfg.staPass;
    cfg.wifiCount = 1;
  }
  syncLegacyStaFromSavedWifi();

  cfg.mdnsName = normalizeMdnsName(cfg.mdnsName);
  if (cfg.webUser.length() == 0) cfg.webUser = "admin";
  if (cfg.webPass.length() == 0) cfg.webPass = "admin";
  if (cfg.ftpUser.length() == 0) cfg.ftpUser = "ftp";
  if (cfg.ftpPass.length() == 0) cfg.ftpPass = "12345678";
  if (cfg.ftpDisk != "ffat" && cfg.ftpDisk != "usb0") cfg.ftpDisk = "usb0";
  if (cfg.audioVolume < 0) cfg.audioVolume = 0;
  if (cfg.audioVolume > 100) cfg.audioVolume = 100;

  // Migrace původního dvoupásmového ekvalizéru.
  if (!detailedEqLoaded) {
    const int legacyBass = clampEqDb(cfg.audioBassDb);
    const int legacyTreble = clampEqDb(cfg.audioTrebleDb);
    for (uint8_t i = 0; i < 4; i++) cfg.audioEqBandDb[i] = legacyBass;
    for (uint8_t i = 4; i < 7; i++) cfg.audioEqBandDb[i] = 0;
    for (uint8_t i = 7; i < AUDIO_EQ_BANDS; i++) cfg.audioEqBandDb[i] = legacyTreble;
  }
  clampDetailedAudioConfig();

  if (!isfinite(cfg.batteryDividerRatio) || cfg.batteryDividerRatio < 1.0f || cfg.batteryDividerRatio > 10.0f) {
    cfg.batteryDividerRatio = 2.0f;
  }
  if (!isfinite(cfg.batteryCalibration) || cfg.batteryCalibration < 0.5f || cfg.batteryCalibration > 1.5f) {
    cfg.batteryCalibration = 1.0f;
  }
  for (uint8_t i = 0; i < MAX_RADIO_STATIONS; i++) {
    cfg.radioName[i].trim();
    cfg.radioUrl[i].trim();
    cfg.radioLogo[i].trim();
    if (cfg.radioLogo[i].indexOf("..") >= 0 ||
        (cfg.radioLogo[i].length() > 0 && !cfg.radioLogo[i].startsWith("/www/logos/"))) {
      cfg.radioLogo[i] = "";
    }
  }
  if (cfg.radioName[0].length() == 0) cfg.radioName[0] = "Moje radio";
}

bool saveConfig() {
  File f = FFat.open(CONFIG_FILE, FILE_WRITE);
  if (!f) {
    return false;
  }

  f.println("ap_ssid=" + cfg.apSsid);
  f.println("ap_pass=" + cfg.apPass);
  f.println("ap_mode=" + String((int)cfg.apMode));
  syncLegacyStaFromSavedWifi();
  f.println("sta_ssid=" + cfg.staSsid);
  f.println("sta_pass=" + cfg.staPass);
  f.println("wifi_count=" + String(cfg.wifiCount));
  for (uint8_t i = 0; i < MAX_SAVED_WIFI; i++) {
    f.println("wifi_ssid_" + String(i) + "=" + cfg.wifiSsid[i]);
    f.println("wifi_pass_" + String(i) + "=" + cfg.wifiPass[i]);
  }
  f.println("mdns_name=" + normalizeMdnsName(cfg.mdnsName));
  f.println("smart_speaker_enabled=" + String(cfg.smartSpeakerEnabled ? "1" : "0"));
  f.println("web_user=" + cfg.webUser);
  f.println("web_pass=" + cfg.webPass);
  f.println("ftp_enabled=" + String(cfg.ftpEnabled ? "1" : "0"));
  f.println("ftp_user=" + cfg.ftpUser);
  f.println("ftp_pass=" + cfg.ftpPass);
  f.println("ftp_disk=" + cfg.ftpDisk);
  f.println("rgb_enabled=" + String(cfg.rgbEnabled ? "1" : "0"));
  f.println("audio_volume=" + String(cfg.audioVolume));
  syncLegacyEqFields();
  f.println("audio_bass_db=" + String(cfg.audioBassDb));
  f.println("audio_treble_db=" + String(cfg.audioTrebleDb));
  f.println("audio_eq_enabled=" + String(cfg.audioEqEnabled ? "1" : "0"));
  f.println("audio_eq_preamp_db=" + String(cfg.audioEqPreampDb));
  f.println("audio_eq_auto_headroom=" + String(cfg.audioEqAutoHeadroom ? "1" : "0"));
  f.println("audio_output_gain_db=" + String(cfg.audioOutputGainDb));
  f.println("audio_volume_curve=" + String(cfg.audioVolumeCurve, 2));
  for (uint8_t i = 0; i < AUDIO_EQ_BANDS; i++) {
    f.println("audio_eq_band_" + String(i) + "=" + String(cfg.audioEqBandDb[i]));
  }
  f.println("battery_enabled=" + String(cfg.batteryEnabled ? "1" : "0"));
  f.println("battery_divider_ratio=" + String(cfg.batteryDividerRatio, 4));
  f.println("battery_calibration=" + String(cfg.batteryCalibration, 4));
  // Starší názvy nechávám pro kompatibilitu
  f.println("radio_name=" + cfg.radioName[0]);
  f.println("radio_url=" + cfg.radioUrl[0]);
  for (uint8_t i = 0; i < MAX_RADIO_STATIONS; i++) {
    f.println("radio_name_" + String(i) + "=" + cfg.radioName[i]);
    f.println("radio_url_" + String(i) + "=" + cfg.radioUrl[i]);
    f.println("radio_logo_" + String(i) + "=" + cfg.radioLogo[i]);
  }

  f.close();
  return true;
}

void clampAudioVolumeCfg() {
  if (cfg.audioVolume < 0) cfg.audioVolume = 0;
  if (cfg.audioVolume > 100) cfg.audioVolume = 100;
}

void loadAudioVolumeState() {
  int saved = -1;
  if (lockPreferences()) {
    prefs.begin("audio", true);
    saved = prefs.getInt("volume", -1);
    prefs.end();
    unlockPreferences();
  }

  if (saved >= 0 && saved <= 100) {
    cfg.audioVolume = saved;
  }

  clampAudioVolumeCfg();
  audioVolumeSaved = cfg.audioVolume;
  audioVolumeSavePending = false;
}

void saveAudioVolumeStateNow() {
  clampAudioVolumeCfg();

  if (audioVolumeSaved == cfg.audioVolume && !audioVolumeSavePending) {
    return;
  }

  if (!lockPreferences()) {
    Serial.println("NVS audio volume: mutex timeout");
    return;
  }
  prefs.begin("audio", false);
  prefs.putInt("volume", cfg.audioVolume);
  prefs.end();
  unlockPreferences();

  audioVolumeSaved = cfg.audioVolume;
  audioVolumeSavePending = false;
}

void requestAudioVolumeSave() {
  clampAudioVolumeCfg();

  if (audioVolumeSaved == cfg.audioVolume) {
    audioVolumeSavePending = false;
    return;
  }

  audioVolumeLastChangeMs = millis();
  audioVolumeSavePending = true;
}

void serviceAudioVolumeSave() {
  if (!audioVolumeSavePending) {
    return;
  }

  if (millis() - audioVolumeLastChangeMs < 800) {
    return;
  }

  saveAudioVolumeStateNow();
}

bool shouldApRun() {
  switch (cfg.apMode) {
    case AP_MODE_ALWAYS:
      apStaConnectedSinceMs = 0;
      return true;

    case AP_MODE_AUTO:
      if (WiFi.status() != WL_CONNECTED) {
        apStaConnectedSinceMs = 0;
        return true;
      }

      // AP vypneme až po několika sekundách stabilního STA spojení.
      // Tím se hotspot nerozkmitá při krátkých změnách stavu Wi-Fi.
      if (apStaConnectedSinceMs == 0) {
        apStaConnectedSinceMs = millis();
      }
      return millis() - apStaConnectedSinceMs < AP_AUTO_DISABLE_DELAY_MS;

    case AP_MODE_OFF:
    default:
      apStaConnectedSinceMs = 0;
      return false;
  }
}

void scheduleApMdnsRestart() {
  apMdnsRestartPending = true;
  apMdnsRestartAtMs = millis() + AP_MDNS_RESTART_DELAY_MS;
}

void applyApPolicy(bool forceRestart) {
  const bool wanted = shouldApRun();

  if (wanted) {
    if (apRunning && !forceRestart) {
      return;
    }

    // Při změně SSID/hesla restartujeme pouze SoftAP konfiguraci.
    // Nikdy zde nepoužívat softAPdisconnect(true): true ukončuje Wi-Fi AP rozhraní
    // a při bootu v této verzi Arduino-ESP32 způsobovalo Cache/MMU panic.
    if (apRunning) {
      WiFi.softAPdisconnect(false);
      apRunning = false;
      delay(50);
    }

    // AP+STA umožní dál hledat a připojovat uložené sítě.
    WiFi.mode(WIFI_AP_STA);
    delay(50);

    bool ok;
    if (cfg.apPass.length() >= 8) {
      ok = WiFi.softAP(cfg.apSsid.c_str(), cfg.apPass.c_str());
    } else {
      ok = WiFi.softAP(cfg.apSsid.c_str());
    }

    apRunning = ok;
    if (ok) {
      Serial.println("AP zapnuto: " + cfg.apSsid + " / " + WiFi.softAPIP().toString());
    } else {
      Serial.println("AP se nepodařilo zapnout");
    }

    scheduleApMdnsRestart();
    return;
  }

  if (!apRunning && !forceRestart) {
    return;
  }

  // Vypnout pouze SoftAP a ponechat STA rádio aktivní.
  if (apRunning) {
    WiFi.softAPdisconnect(false);
    delay(50);
  }
  WiFi.mode(WIFI_STA);
  apRunning = false;
  Serial.println("AP vypnuto podle nastavení");
  scheduleApMdnsRestart();
}

void scheduleApPolicyApply(bool forceRestart, uint32_t delayMs) {
  apPolicyApplyPending = true;
  apPolicyForceRestart = apPolicyForceRestart || forceRestart;
  apPolicyApplyAtMs = millis() + delayMs;
}

void serviceApPolicy() {
  if (apPolicyApplyPending && (int32_t)(millis() - apPolicyApplyAtMs) >= 0) {
    bool forceRestart = apPolicyForceRestart;
    apPolicyApplyPending = false;
    apPolicyForceRestart = false;
    applyApPolicy(forceRestart);
  } else if (shouldApRun() != apRunning) {
    applyApPolicy(false);
  }

  // mDNS restartujeme až po ustálení Wi-Fi rozhraní, ne přímo uvnitř přepnutí AP.
  if (apMdnsRestartPending && (int32_t)(millis() - apMdnsRestartAtMs) >= 0) {
    apMdnsRestartPending = false;
    startMdns(true);
  }
}

void connectStaIfConfigured() {
  requestSavedWifiReconnect(true);
}

// ============================================================
// USB MSC přes EspUsbHost
// ============================================================

void enableUsbPower() {
#if USB_POWER_PIN >= 0
  pinMode(USB_POWER_PIN, OUTPUT);
  digitalWrite(USB_POWER_PIN, HIGH);
  delay(500);
#else
  Serial.println("USB VBUS power pin not configured; use USB-OTG/IN-OUT solder bridge if board needs VBUS.");
#endif
}

bool isUsbDiskName(const String& disk) {
  return disk == "usb0";
}

const char* audioOutputKindName() {
  switch (activeAudioOutput) {
    case AUDIO_OUTPUT_I2S: return "I2S PCM5102";
    case AUDIO_OUTPUT_USB: return "USB zvukovka";
    default: return "žádný";
  }
}

void deactivateAudioOutput(const String& status) {
  activeAudioOutput = AUDIO_OUTPUT_NONE;
  audioAddress = 0;
  audioReady = false;
  audioStatus = status.length() ? status : String("Audio výstup není připravený");
}

void activateI2sAudioOutput(const String& status) {
  if (!i2sAudioStarted) {
    deactivateAudioOutput(status.length() ? status : String("I2S audio není připravené"));
    return;
  }

  activeAudioOutput = AUDIO_OUTPUT_I2S;
  audioAddress = 0;
  audioOutputFormat = {48000, 2, 16};
  audioReady = true;
  audioStatus = status.length() ? status : String("I2S audio připraveno");
}

bool activeOutputIsUsb() {
  return activeAudioOutput == AUDIO_OUTPUT_USB && audioAddress != 0;
}

bool activeOutputIsI2s() {
  return activeAudioOutput == AUDIO_OUTPUT_I2S && i2sAudioStarted;
}

bool chooseAudioOutputStream(uint8_t address) {
  EspUsbHostAudioStreamInfo streams[ESP_USB_HOST_MAX_AUDIO_STREAMS];

  const size_t count = usb.getAudioStreams(
    address,
    streams,
    ESP_USB_HOST_MAX_AUDIO_STREAMS
  );

  Serial.printf("USB audio streams: %u\n", (unsigned)count);

  for (size_t i = 0; i < count; i++) {
    espUsbHostPrint(streams[i]);
  }

  const EspUsbHostAudioStreamSelection selected =
    espUsbHostSelectAudioOutputStream(streams, count);

  if (!selected) {
    if (i2sAudioStarted) {
      activateI2sAudioOutput("USB zvukovka bez vhodného výstupu, používám I2S");
    } else {
      deactivateAudioOutput("USB audio stream nenalezen");
    }
    return false;
  }

  const EspUsbHostAudioStreamInfo &stream = streams[selected.index];

  // Přepnutí výstupu za běhu by mohlo rozhodit PCM buffer, proto aktuální
  // přehrávání raději zastavíme a další play už pojede do nového výstupu.
  if (sendspinStreamActive) {
    sendspinEndStream("Síťový stream zastaven kvůli změně audio výstupu");
  } else if (audioPlaying) {
    stopAudioPlayback("Audio výstup přepnut na USB zvukovku");
  }

  audioOutputFormat = {
    selected.sampleRate,
    stream.channels,
    stream.bitsPerSample
  };

  Serial.printf(
    "USB audio selected: %lu Hz, %u ch, %u-bit\n",
    (unsigned long)audioOutputFormat.sampleRate,
    audioOutputFormat.channels,
    audioOutputFormat.bitsPerSample
  );

  if (!usb.audioOutputStart(stream, selected.sampleRate, address)) {
    if (i2sAudioStarted) {
      activateI2sAudioOutput("USB audioOutputStart selhal, používám I2S");
    } else {
      deactivateAudioOutput("USB audioOutputStart selhal");
    }
    return false;
  }

  audioAddress = address;
  activeAudioOutput = AUDIO_OUTPUT_USB;
  audioReady = true;
  audioStatus = "USB zvukovka připravena";

  return true;
}

void initUsbHost() {
  Serial.println("Starting USB host...");

  usb.onDeviceConnected([](const EspUsbHostDeviceInfo &device) {
    usbLastDeviceEventMs = millis();
    Serial.println();
    Serial.print("connected: ");
    espUsbHostPrint(device);
    Serial.println();

    // Výstup je AUTO: pokud se najde USB zvukovka, dostane přednost.
    // Když USB audio není, zůstává aktivní I2S/PCM5102.
    if (usb.audioOutputReady(device.address)) {
      Serial.println("USB audio output ready candidate");
      chooseAudioOutputStream(device.address);
    }
  });

  usb.onDeviceDisconnected([](const EspUsbHostDeviceInfo &device) {
    usbLastDeviceEventMs = millis();
    Serial.println();
    Serial.print("disconnected: ");
    espUsbHostPrint(device);
    Serial.println();

    usbMassStorage.end();
    usbStatus = "USB disk odpojen";

    if (activeAudioOutput == AUDIO_OUTPUT_USB && audioAddress != 0 && device.address == audioAddress) {
      audioAddress = 0;

      if (!sendspinStreamActive && audioPlaying) {
        stopAudioPlayback("USB zvukovka odpojena");
      }

      if (i2sAudioStarted) {
        activateI2sAudioOutput("USB zvukovka odpojena, používám I2S");
      } else {
        deactivateAudioOutput("USB audio odpojeno");
      }
    }
  });

  usb.onAudioOutputRequest([](EspUsbHostAudioOutputRequest &request) {
    bool networkReady = sendspinStreamActive && sendspinGroupPlaying;
    bool localReady = !sendspinStreamActive && audioPlaying && !audioPaused;

    if (!activeOutputIsUsb() || (!networkReady && !localReady)) {
      request.writtenFrames = 0;
      audioCbCount++;
      audioCbUnderruns++;
      return;
    }

    if (sendspinStreamActive) {
      request.writtenFrames = sendspinReadPcmFrames(request.data, request.frameCount);
      if (request.writtenFrames > 0 && sendspinBitDepth == 16) {
        sendspinApplyVolume(
          (int16_t *)request.data,
          request.writtenFrames,
          sendspinChannels
        );
        processAudioEqualizer(
          (int16_t *)request.data,
          request.writtenFrames,
          sendspinChannels,
          sendspinSampleRate,
          usbEqProcessor
        );
      }
    } else {
      request.writtenFrames = audio.readFrames(request.data, request.frameCount);

      if (request.writtenFrames > 0 && audioOutputFormat.bitsPerSample == 16) {
        processAudioEqualizer(
          (int16_t *)request.data,
          request.writtenFrames,
          audioOutputFormat.channels,
          audioOutputFormat.sampleRate,
          usbEqProcessor
        );
      }
    }

    audioCbCount++;
    audioCbFrames += request.writtenFrames;
    if (request.writtenFrames == 0) {
      audioCbUnderruns++;
    }

    if (request.writtenFrames > 0 && audioOutputFormat.bitsPerSample == 16) {
      int16_t *samples = (int16_t *)request.data;
      size_t levelChannels = sendspinStreamActive ? (size_t)sendspinChannels : (size_t)audioOutputFormat.channels;
      size_t sampleCount = (size_t)request.writtenFrames * levelChannels;
      uint32_t sum = 0;
      size_t step = sampleCount > 96 ? sampleCount / 96 : 1;
      size_t counted = 0;

      for (size_t i = 0; i < sampleCount; i += step) {
        int32_t v = samples[i];
        if (v < 0) v = -v;
        sum += (uint32_t)v;
        counted++;
      }

      if (counted > 0) {
        audioLevel = (uint16_t)(sum / counted);
      }
    }
  });

  if (!usb.begin()) {
    usbStatus = "usb.begin() failed: ";
    usbStatus += usb.lastErrorName();
    Serial.println(usbStatus);
    usbHostStarted = false;
    return;
  }

  usbHostStarted = true;
  usbHostStartMs = millis();
  usbLastDeviceEventMs = usbHostStartMs;

  usbStatus = "USB host aktivní, čekám na FAT32 flashku";
  Serial.println(usbStatus);
}

void pollUsbMount() {
  if (!usbHostStarted) {
    return;
  }

  if (usbMassStorage.mounted()) {
    return;
  }

  Serial.println("Trying USB MSC FAT mount...");

  if (usbMassStorage.begin(usb, "/usb")) {
    usbStatus = "USB disk připojen";
    Serial.println("USB MSC mounted as /usb");

    File root = usbMassStorage.open("/");
    if (root && root.isDirectory()) {
      Serial.println("USB root open OK");
      File entry = root.openNextFile();
      while (entry) {
        Serial.printf("USB entry: %s size=%u\n", entry.name(), (unsigned)entry.size());
        entry.close();
        entry = root.openNextFile();
      }
      root.close();
    } else {
      Serial.println("USB root open FAILED");
    }
  } else {
    usbStatus = "USB disk nepřipojen nebo mount selhal: ";
    usbStatus += usb.lastErrorName();
    Serial.println(usbStatus);
  }
}

bool usbDiskMounted() {
  return usbMassStorage.mounted();
}

uint64_t sumFilesInDir(fs::FS &fs, const char *dirname) {
  uint64_t total = 0;

  File root = fs.open(dirname);
  if (!root || !root.isDirectory()) {
    return 0;
  }

  File file = root.openNextFile();

  while (file) {
    if (file.isDirectory()) {
      String child = String(dirname);
      if (!child.endsWith("/")) {
        child += "/";
      }
      child += file.name();
      total += sumFilesInDir(fs, child.c_str());
    } else {
      total += file.size();
    }

    file.close();
    file = root.openNextFile();
  }

  root.close();
  return total;
}

bool getUsbDiskSpace(uint64_t &total, uint64_t &used, uint64_t &freeBytes) {
  total = 0;
  used = 0;
  freeBytes = 0;

  if (!usbMassStorage.mounted()) {
    return false;
  }

  // EspUsbHostMscFS v této Arduino verzi nemá totalBytes()/usedBytes()
  // a sys/statvfs.h tady není dostupné. Umíme tedy spolehlivě spočítat
  // aspoň obsazené místo sečtením souborů. Celková kapacita USB disku
  // zůstane nezjištěná.
  used = sumFilesInDir(usbMassStorage, "/");
  return true;
}

float currentAudioGain() {
  int v = cfg.audioVolume;
  if (v <= 0) return 0.0f;
  if (v > 100) v = 100;

  // 100 % odpovídá nastavenému audioOutputGainDb. Křivka > 1 zjemní
  // spodní část rozsahu, takže například 30 % už není 30 % amplitudy.
  const float normalized = (float)v / 100.0f;
  const float shaped = powf(normalized, clampVolumeCurve(cfg.audioVolumeCurve));
  const float outputLimit = powf(10.0f, (float)clampOutputGainDb(cfg.audioOutputGainDb) / 20.0f);
  float gain = shaped * outputLimit;
  if (gain < 0.0f) gain = 0.0f;
  if (gain > 1.0f) gain = 1.0f;
  return gain;
}

void applyAudioVolume() {
  audio.setGain(currentAudioGain());
}

void requestAudioGainApply() {
  const bool inAudioTask = audioTaskHandle &&
    xTaskGetCurrentTaskHandle() == audioTaskHandle;

  if (audioTaskHandle && !inAudioTask) {
    // Změny z enkodéru, webu a Sendspin tasku pouze nastaví požadavek.
    // Na objekt PCMFlow pak sahá kvůli gainu jen audio task.
    audioGainApplyPending = true;
    return;
  }

  applyAudioVolume();
}

void serviceAudioGainApply() {
  if (!audioGainApplyPending) return;
  audioGainApplyPending = false;
  applyAudioVolume();
}

bool initI2sAudioOutput() {
  if (i2sAudioStarted) {
    if (activeAudioOutput != AUDIO_OUTPUT_USB) {
      activateI2sAudioOutput("I2S audio připraveno");
    }
    return true;
  }

#if I2S_MUTE_PIN >= 0
  pinMode(I2S_MUTE_PIN, OUTPUT);
  digitalWrite(I2S_MUTE_PIN, LOW);
#endif

  audioOutputFormat = {48000, 2, 16};

  i2s_config_t i2sCfg = {};
  i2sCfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  i2sCfg.sample_rate = audioOutputFormat.sampleRate;
  i2sCfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2sCfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
#if defined(I2S_COMM_FORMAT_STAND_I2S)
  i2sCfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
#else
  i2sCfg.communication_format = I2S_COMM_FORMAT_I2S;
#endif
  i2sCfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2sCfg.dma_buf_count = 16;
  i2sCfg.dma_buf_len = 512;
  i2sCfg.use_apll = false;
  i2sCfg.tx_desc_auto_clear = true;
  i2sCfg.fixed_mclk = 0;

  esp_err_t err = i2s_driver_install(I2S_AUDIO_PORT, &i2sCfg, 0, nullptr);
  if (err != ESP_OK) {
    audioReady = false;
    audioStatus = String("I2S driver install selhal: ") + esp_err_to_name(err);
    Serial.println(audioStatus);
    return false;
  }

  i2s_pin_config_t pinCfg = {};
#if ESP_IDF_VERSION_MAJOR >= 5
  pinCfg.mck_io_num = I2S_PIN_NO_CHANGE;
#endif
  pinCfg.bck_io_num = I2S_BCK_PIN;
  pinCfg.ws_io_num = I2S_LRCK_PIN;
  pinCfg.data_out_num = I2S_DOUT_PIN;
  pinCfg.data_in_num = I2S_PIN_NO_CHANGE;

  err = i2s_set_pin(I2S_AUDIO_PORT, &pinCfg);
  if (err != ESP_OK) {
    i2s_driver_uninstall(I2S_AUDIO_PORT);
    audioReady = false;
    audioStatus = String("I2S piny selhaly: ") + esp_err_to_name(err);
    Serial.println(audioStatus);
    return false;
  }

  i2s_zero_dma_buffer(I2S_AUDIO_PORT);
  i2sAudioStarted = true;
  if (activeAudioOutput != AUDIO_OUTPUT_USB) {
    activateI2sAudioOutput("I2S audio připraveno");
  }
  Serial.printf(
    "I2S audio ready: BCK=%d LRCK=%d DOUT=%d %lu Hz %u ch %u-bit\n",
    I2S_BCK_PIN,
    I2S_LRCK_PIN,
    I2S_DOUT_PIN,
    (unsigned long)audioOutputFormat.sampleRate,
    audioOutputFormat.channels,
    audioOutputFormat.bitsPerSample
  );

#if I2S_MUTE_PIN >= 0
  digitalWrite(I2S_MUTE_PIN, HIGH);
#endif

  return true;
}

void serviceI2sAudioOutput() {
  if (sendspinStreamActive) {
    serviceSendspinI2sAudioOutput();
    return;
  }

  if (!activeOutputIsI2s() || !audioPlaying || audioPaused) {
    return;
  }

  size_t available = audio.availableFrames();
  if (available == 0) {
    return;
  }

  const size_t maxFrames = sizeof(i2sOutBuffer) / (sizeof(int16_t) * 2);
  size_t frames = available;
  if (frames > maxFrames) frames = maxFrames;

  size_t readFrames = audio.readFrames(i2sOutBuffer, frames);
  if (readFrames == 0) {
    audioCbCount++;
    audioCbUnderruns++;
    return;
  }

  const size_t channels = audioOutputFormat.channels > 0 ? audioOutputFormat.channels : 2;

  if (audioOutputFormat.bitsPerSample == 16) {
    processAudioEqualizer(
      i2sOutBuffer,
      readFrames,
      (uint8_t)channels,
      audioOutputFormat.sampleRate,
      i2sEqProcessor
    );
  }

  const size_t bytesPerSample = audioOutputFormat.bitsPerSample / 8;
  size_t bytesToWrite = readFrames * channels * bytesPerSample;
  size_t bytesWritten = 0;

  esp_err_t err = i2s_write(
    I2S_AUDIO_PORT,
    i2sOutBuffer,
    bytesToWrite,
    &bytesWritten,
    portMAX_DELAY
  );

  audioCbCount++;
  audioCbFrames += bytesWritten / (channels * bytesPerSample);

  if (err != ESP_OK || bytesWritten != bytesToWrite) {
    audioCbUnderruns++;
    Serial.printf(
      "I2S neúplný zápis: %u/%u bytes\n",
      (unsigned)bytesWritten,
      (unsigned)bytesToWrite
    );
  }

  if (bytesWritten > 0 && audioOutputFormat.bitsPerSample == 16) {
    int16_t *samples = (int16_t *)i2sOutBuffer;
    size_t sampleCount = bytesWritten / sizeof(int16_t);
    uint32_t sum = 0;
    size_t step = sampleCount > 96 ? sampleCount / 96 : 1;
    size_t counted = 0;

    for (size_t i = 0; i < sampleCount; i += step) {
      int32_t v = samples[i];
      if (v < 0) v = -v;
      sum += (uint32_t)v;
      counted++;
    }

    if (counted > 0) {
      audioLevel = (uint16_t)(sum / counted);
    }
  }
}

bool startAudioFile(const String& disk, const String& path) {
  if (!audioReady) {
    audioStatus = "Audio výstup není připravený";
    return false;
  }

  String lower = path;
  lower.toLowerCase();

  if (!lower.endsWith(".mp3")) {
    audioStatus = "Zatím podporuju jen MP3";
    return false;
  }

  File test = fsOpenGeneric(disk, path, FILE_READ);
  if (!test || test.isDirectory()) {
    if (test) test.close();
    audioStatus = "Soubor nelze otevřít";
    return false;
  }

  size_t size = test.size();
  test.close();

  if (size == 0) {
    audioStatus = "Soubor je prázdný";
    return false;
  }

  if (sendspinStreamActive) {
    sendspinEndStream("Přepínám na lokální MP3");
  }
  stopAudioPlayback("");
  saveRadioResumeState(false, -1, "");

  audioStreamFile = fsOpenGeneric(disk, path, FILE_READ);
  if (!audioStreamFile || audioStreamFile.isDirectory()) {
    if (audioStreamFile) audioStreamFile.close();
    audioStatus = "Soubor nelze otevřít pro stream";
    return false;
  }

  audioFileStream.setFile(audioStreamFile);

  audio.close();
  audio.setOutputFormat(audioOutputFormat);
  applyAudioVolume();
  audio.setBufferFrames(FILE_PCM_BUFFER_FRAMES);
  audio.setInput(audioFileStream, PCMFlow::CodecKind::Mp3);

  audioStatus = "Soubor: bufferuji...";
  audioDisk = disk;
  audioPath = path;
  audioPaused = false;
  markAudioPositionStart();
  radioPlaying = false;

  audioCbCount = 0;
  audioCbFrames = 0;
  audioCbUnderruns = 0;
  audioPumpCount = 0;

  Serial.printf("MP3 file stream start: %s size=%u\n", path.c_str(), (unsigned)size);

  uint32_t prePumps = 0;
  uint32_t preGoodPumps = 0;
  unsigned long preStart = millis();

  bool startCancelled = false;

  while (millis() - preStart < FILE_PREBUFFER_TIMEOUT_MS) {
    if (audioCommandPending()) {
      startCancelled = true;
      break;
    }

    if (audio.pump()) {
      preGoodPumps++;
    }
    prePumps++;

    if (audio.availableFrames() >= FILE_DECODER_START_FRAMES) {
      break;
    }

    if (audio.lastError() != PCMFlow::Error::None &&
        audio.lastError() != PCMFlow::Error::NotReady) {
      break;
    }

    delay(1);
  }

  size_t preFrames = audio.availableFrames();

  if (startCancelled) {
    audio.close();
    if (audioStreamFile) audioStreamFile.close();
    audioPlaying = false;
    radioPlaying = false;
    audioStatus = "Start MP3 přerušen novým povelem";
    return false;
  }

  Serial.printf(
    "MP3 file prebuffer done: pumps=%u good=%u availFrames=%u ready=%u eof=%u err=%s freeHeap=%u freePsram=%u\n",
    (unsigned)prePumps,
    (unsigned)preGoodPumps,
    (unsigned)preFrames,
    audio.isReady() ? 1 : 0,
    audio.isEof() ? 1 : 0,
    pcmFlowErrorName(audio.lastError()),
    (unsigned)ESP.getFreeHeap(),
    (unsigned)ESP.getFreePsram()
  );

  if (preFrames == 0 || !audio.isReady()) {
    PCMFlow::Error realError = audio.lastError();
    bool wasReady = audio.isReady();
    bool wasEof = audio.isEof();

    audio.close();
    if (audioStreamFile) audioStreamFile.close();

    audioStatus = String("PCMFlow nedekóduje MP3 soubor: ") + pcmFlowErrorName(realError) +
                  " ready=" + String(wasReady ? 1 : 0) +
                  " eof=" + String(wasEof ? 1 : 0);
    Serial.println(audioStatus);
    return false;
  }

  sendspinSetExternalSource(true);
  audioPlaying = true;
  radioPlaying = false;
  audioStatus = "Přehrávám: " + path;

  return true;
}


static const uint8_t AUDIO_PLAYLIST_SCAN_MAX_DEPTH = 10;

bool isMp3Path(const String& path) {
  String lower = fileNameFromPath(path);
  lower.toLowerCase();
  return lower.endsWith(".mp3");
}

bool findFirstMp3Recursive(const String& disk, const String& dirPath, String& pathOut, uint8_t depth) {
  if (depth > AUDIO_PLAYLIST_SCAN_MAX_DEPTH) {
    return false;
  }

  File root = fsOpenGeneric(disk, dirPath, FILE_READ);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return false;
  }

  File file = root.openNextFile();
  while (file) {
    bool isDir = file.isDirectory();
    String rawName = file.name();
    file.close();

    String fullPath = fullPathForEntry(dirPath, rawName);
    if (fullPath.length() > 0) {
      if (isDir) {
        if (findFirstMp3Recursive(disk, fullPath, pathOut, depth + 1)) {
          root.close();
          return true;
        }
      } else if (isMp3Path(fullPath)) {
        pathOut = fullPath;
        root.close();
        return true;
      }
    }

    file = root.openNextFile();
  }

  root.close();
  return false;
}

bool findLastMp3Recursive(const String& disk, const String& dirPath, String& pathOut, uint8_t depth) {
  if (depth > AUDIO_PLAYLIST_SCAN_MAX_DEPTH) {
    return false;
  }

  bool found = false;
  File root = fsOpenGeneric(disk, dirPath, FILE_READ);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return false;
  }

  File file = root.openNextFile();
  while (file) {
    bool isDir = file.isDirectory();
    String rawName = file.name();
    file.close();

    String fullPath = fullPathForEntry(dirPath, rawName);
    if (fullPath.length() > 0) {
      if (isDir) {
        if (findLastMp3Recursive(disk, fullPath, pathOut, depth + 1)) {
          found = true;
        }
      } else if (isMp3Path(fullPath)) {
        pathOut = fullPath;
        found = true;
      }
    }

    file = root.openNextFile();
  }

  root.close();
  return found;
}

bool findNextMp3Recursive(const String& disk, const String& dirPath, const String& afterPath, bool& takeNext, String& nextPath, uint8_t depth) {
  if (depth > AUDIO_PLAYLIST_SCAN_MAX_DEPTH) {
    return false;
  }

  File root = fsOpenGeneric(disk, dirPath, FILE_READ);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return false;
  }

  File file = root.openNextFile();
  while (file) {
    bool isDir = file.isDirectory();
    String rawName = file.name();
    file.close();

    String fullPath = fullPathForEntry(dirPath, rawName);
    if (fullPath.length() > 0) {
      if (isDir) {
        if (findNextMp3Recursive(disk, fullPath, afterPath, takeNext, nextPath, depth + 1)) {
          root.close();
          return true;
        }
      } else if (isMp3Path(fullPath)) {
        if (takeNext) {
          nextPath = fullPath;
          root.close();
          return true;
        }

        if (fullPath == afterPath) {
          takeNext = true;
        }
      }
    }

    file = root.openNextFile();
  }

  root.close();
  return false;
}

bool findPrevMp3Recursive(const String& disk, const String& dirPath, const String& beforePath, String& lastSeen, String& prevPath, uint8_t depth) {
  if (depth > AUDIO_PLAYLIST_SCAN_MAX_DEPTH) {
    return false;
  }

  File root = fsOpenGeneric(disk, dirPath, FILE_READ);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return false;
  }

  File file = root.openNextFile();
  while (file) {
    bool isDir = file.isDirectory();
    String rawName = file.name();
    file.close();

    String fullPath = fullPathForEntry(dirPath, rawName);
    if (fullPath.length() > 0) {
      if (isDir) {
        if (findPrevMp3Recursive(disk, fullPath, beforePath, lastSeen, prevPath, depth + 1)) {
          root.close();
          return true;
        }
      } else if (isMp3Path(fullPath)) {
        if (fullPath == beforePath) {
          prevPath = lastSeen;
          root.close();
          return prevPath.length() > 0;
        }

        lastSeen = fullPath;
      }
    }

    file = root.openNextFile();
  }

  root.close();
  return false;
}

void countMp3Recursive(const String& disk, const String& dirPath, uint16_t& count, uint8_t depth) {
  if (depth > AUDIO_PLAYLIST_SCAN_MAX_DEPTH || count == 0xFFFF) {
    return;
  }

  File root = fsOpenGeneric(disk, dirPath, FILE_READ);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  File file = root.openNextFile();
  while (file && count < 0xFFFF) {
    bool isDir = file.isDirectory();
    String rawName = file.name();
    file.close();

    String fullPath = fullPathForEntry(dirPath, rawName);
    if (fullPath.length() > 0) {
      if (isDir) {
        countMp3Recursive(disk, fullPath, count, depth + 1);
      } else if (isMp3Path(fullPath)) {
        count++;
      }
    }

    file = root.openNextFile();
  }

  root.close();
}

bool findMp3ByIndexRecursive(const String& disk, const String& dirPath, uint16_t target, uint16_t& index, String& pathOut, uint8_t depth) {
  if (depth > AUDIO_PLAYLIST_SCAN_MAX_DEPTH) {
    return false;
  }

  File root = fsOpenGeneric(disk, dirPath, FILE_READ);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return false;
  }

  File file = root.openNextFile();
  while (file) {
    bool isDir = file.isDirectory();
    String rawName = file.name();
    file.close();

    String fullPath = fullPathForEntry(dirPath, rawName);
    if (fullPath.length() > 0) {
      if (isDir) {
        if (findMp3ByIndexRecursive(disk, fullPath, target, index, pathOut, depth + 1)) {
          root.close();
          return true;
        }
      } else if (isMp3Path(fullPath)) {
        if (index == target) {
          pathOut = fullPath;
          root.close();
          return true;
        }
        index++;
      }
    }

    file = root.openNextFile();
  }

  root.close();
  return false;
}

bool findFirstMp3InFolder(const String& disk, const String& dirPath, String& pathOut) {
  pathOut = "";

  if (!diskAvailable(disk)) {
    return false;
  }

  return findFirstMp3Recursive(disk, dirPath, pathOut, 0);
}

bool findLastMp3InFolder(const String& disk, const String& dirPath, String& pathOut) {
  pathOut = "";

  if (!diskAvailable(disk)) {
    return false;
  }

  return findLastMp3Recursive(disk, dirPath, pathOut, 0);
}

bool findNextMp3InFolder(const String& disk, const String& dirPath, const String& afterPath, String& nextPath) {
  nextPath = "";

  if (!diskAvailable(disk)) {
    return false;
  }

  bool takeNext = afterPath.length() == 0;
  return findNextMp3Recursive(disk, dirPath, afterPath, takeNext, nextPath, 0);
}

bool findPrevMp3InFolder(const String& disk, const String& dirPath, const String& beforePath, String& prevPath) {
  prevPath = "";

  if (!diskAvailable(disk)) {
    return false;
  }

  String lastSeen = "";
  return findPrevMp3Recursive(disk, dirPath, beforePath, lastSeen, prevPath, 0);
}

uint16_t countMp3InFolder(const String& disk, const String& dirPath) {
  if (!diskAvailable(disk)) {
    return 0;
  }

  uint16_t count = 0;
  countMp3Recursive(disk, dirPath, count, 0);
  return count;
}

bool findRandomMp3InFolder(const String& disk, const String& dirPath, const String& avoidPath, String& randomPath) {
  randomPath = "";

  uint16_t count = countMp3InFolder(disk, dirPath);
  if (count == 0) {
    return false;
  }

  for (uint8_t attempt = 0; attempt < 4; attempt++) {
    uint16_t target = (uint16_t)random(count);
    uint16_t idx = 0;
    String candidate = "";

    if (findMp3ByIndexRecursive(disk, dirPath, target, idx, candidate, 0)) {
      if (count == 1 || candidate != avoidPath) {
        randomPath = candidate;
        return true;
      }
    }
  }

  return findNextMp3InFolder(disk, dirPath, avoidPath, randomPath) ||
         findFirstMp3InFolder(disk, dirPath, randomPath);
}

bool startPlaylistTrack(const String& path) {
  if (!startAudioFile(playlistDisk, path)) {
    return false;
  }

  playlistActive = true;
  playlistLastPath = path;
  audioStatus = "Playlist: " + path;
  return true;
}

bool startPlaylistFolder(const String& disk, const String& dirPath) {
  String firstMp3;

  if (!findFirstMp3InFolder(disk, dirPath, firstMp3)) {
    audioStatus = "Ve složce není žádné MP3";
    return false;
  }

  playlistActive = false;
  playlistDisk = disk;
  playlistDir = dirPath;
  playlistLastPath = "";

  if (!startPlaylistTrack(firstMp3)) {
    playlistActive = false;
    return false;
  }

  return true;
}

bool playNextPlaylistTrack() {
  if (!playlistActive) {
    return false;
  }

  String nextMp3;

  if (playlistShuffle) {
    if (!findRandomMp3InFolder(playlistDisk, playlistDir, playlistLastPath, nextMp3)) {
      playlistActive = false;
      stopAudioPlayback("Playlist dokončen");
      return false;
    }
  } else if (!findNextMp3InFolder(playlistDisk, playlistDir, playlistLastPath, nextMp3)) {
    if (playlistRepeat) {
      findFirstMp3InFolder(playlistDisk, playlistDir, nextMp3);
    }

    if (nextMp3.length() == 0) {
      playlistActive = false;
      stopAudioPlayback("Playlist dokončen");
      return false;
    }
  }

  if (!startPlaylistTrack(nextMp3)) {
    playlistActive = false;
    return false;
  }

  return true;
}

bool playPrevPlaylistTrack() {
  if (!playlistActive) {
    return false;
  }

  String prevMp3;

  if (playlistShuffle) {
    if (!findRandomMp3InFolder(playlistDisk, playlistDir, playlistLastPath, prevMp3)) {
      return false;
    }
  } else if (!findPrevMp3InFolder(playlistDisk, playlistDir, playlistLastPath, prevMp3)) {
    if (playlistRepeat) {
      findLastMp3InFolder(playlistDisk, playlistDir, prevMp3);
    }

    if (prevMp3.length() == 0) {
      return false;
    }
  }

  if (!startPlaylistTrack(prevMp3)) {
    playlistActive = false;
    return false;
  }

  return true;
}


void stopAudioPlayback(const String& reason) {
  audioPlaying = false;
  audioPaused = false;
  markAudioPositionStop();
  radioPlaying = false;
  if (activeOutputIsI2s()) {
    i2s_zero_dma_buffer(I2S_AUDIO_PORT);
  }
  audio.close();
  resetAudioEqualizers();
  if (audioStreamFile) {
    audioStreamFile.close();
  }
  freeAudioBuffer();

  if (radioClient.connected()) {
    radioClient.stop();
  }

  radioStream.clear();
  freeRadioPreBuffer();
  radioStatus = reason.length() > 0 ? reason : "Radio zastaveno";
  radioUrlActive = "";

  if (reason.length() > 0) {
    audioStatus = reason;
    if (sendspinExternalSource) {
      sendspinSetExternalSource(false);
    }
  }

  audioLevel = 0;
}



struct RadioHttpInfo {
  int statusCode = 0;
  String contentType = "";
  String location = "";
  bool isRedirect = false;
};

RadioHttpInfo lastRadioHttp;

bool parseHttpUrl(const String& url, String& host, uint16_t& port, String& path) {
  String u = url;
  u.trim();

  if (!u.startsWith("http://")) {
    return false;
  }

  u = u.substring(7);

  int slash = u.indexOf('/');
  String hostPort;

  if (slash >= 0) {
    hostPort = u.substring(0, slash);
    path = u.substring(slash);
  } else {
    hostPort = u;
    path = "/";
  }

  int colon = hostPort.lastIndexOf(':');

  if (colon >= 0) {
    host = hostPort.substring(0, colon);
    port = (uint16_t)hostPort.substring(colon + 1).toInt();
    if (port == 0) port = 80;
  } else {
    host = hostPort;
    port = 80;
  }

  host.trim();

  return host.length() > 0;
}

bool skipHttpHeaders(WiFiClient& client) {
  String line;
  unsigned long start = millis();
  bool firstLine = true;

  lastRadioHttp = RadioHttpInfo();

  while (millis() - start < 8000) {
    if (audioCommandPending()) {
      audioStatus = "Start rádia přerušen novým povelem";
      radioStatus = audioStatus;
      return false;
    }

    while (client.available()) {
      char c = client.read();

      if (c == '\r') {
        continue;
      }

      if (c == '\n') {
        line.trim();

        if (line.length() == 0) {
          if (lastRadioHttp.statusCode == 200) {
            return true;
          }

          if (lastRadioHttp.isRedirect) {
            audioStatus = "Radio HTTP redirect " + String(lastRadioHttp.statusCode) + ": " + lastRadioHttp.location;
          } else if (lastRadioHttp.statusCode > 0) {
            audioStatus = "Radio HTTP chyba: " + String(lastRadioHttp.statusCode);
          } else {
            audioStatus = "Radio neposlalo HTTP status";
          }

          radioStatus = audioStatus;
          return false;
        }

        Serial.println("RADIO HDR: " + line);

        if (firstLine) {
          firstLine = false;

          if (line.startsWith("HTTP/")) {
            int sp1 = line.indexOf(' ');
            if (sp1 >= 0 && sp1 + 3 <= (int)line.length()) {
              lastRadioHttp.statusCode = line.substring(sp1 + 1, sp1 + 4).toInt();
            }

            if (lastRadioHttp.statusCode == 301 ||
                lastRadioHttp.statusCode == 302 ||
                lastRadioHttp.statusCode == 303 ||
                lastRadioHttp.statusCode == 307 ||
                lastRadioHttp.statusCode == 308) {
              lastRadioHttp.isRedirect = true;
            }
          }
        } else {
          String low = line;
          low.toLowerCase();

          if (low.startsWith("content-type:")) {
            lastRadioHttp.contentType = line.substring(line.indexOf(':') + 1);
            lastRadioHttp.contentType.trim();
          } else if (low.startsWith("location:")) {
            lastRadioHttp.location = line.substring(line.indexOf(':') + 1);
            lastRadioHttp.location.trim();
          }
        }

        line = "";
      } else if (line.length() < 280) {
        line += c;
      }
    }

    if (!client.connected()) {
      audioStatus = "Radio spojení spadlo při HTTP hlavičkách";
      radioStatus = audioStatus;
      return false;
    }

    delay(1);
  }

  audioStatus = "Radio HTTP timeout";
  radioStatus = audioStatus;
  return false;
}

const char* pcmFlowErrorName(PCMFlow::Error e) {
  switch (e) {
    case PCMFlow::Error::None: return "None";
    case PCMFlow::Error::NotReady: return "NotReady";
    case PCMFlow::Error::NoInput: return "NoInput";
    case PCMFlow::Error::InvalidOutputFormat: return "InvalidOutputFormat";
    case PCMFlow::Error::UnsupportedCodec: return "UnsupportedCodec";
    case PCMFlow::Error::DecoderInitFailed: return "DecoderInitFailed";
    case PCMFlow::Error::ScratchAllocFailed: return "ScratchAllocFailed";
    case PCMFlow::Error::RingBufferAllocFailed: return "RingBufferAllocFailed";
    case PCMFlow::Error::SniffFailed: return "SniffFailed";
    case PCMFlow::Error::FileOpenFailed: return "FileOpenFailed";
    default: return "Unknown";
  }
}

void closeRadioStream() {
  radioPlaying = false;
  radioUrlActive = "";

  audio.close();

  if (radioClient.connected()) {
    radioClient.stop();
  }

  radioStream.clear();
  freeRadioPreBuffer();
  radioStatus = "Radio zastaveno";
}

bool startRadioStream(const String& url) {
  if (!audioReady) {
    audioStatus = "Audio výstup není připravený";
    radioStatus = audioStatus;
    return false;
  }

  String host;
  String path;
  uint16_t port = 80;

  if (!parseHttpUrl(url, host, port, path)) {
    audioStatus = "Radio URL musí začínat http://";
    radioStatus = audioStatus;
    return false;
  }

  if (sendspinStreamActive) {
    sendspinEndStream("Přepínám na webové rádio");
  }
  stopAudioPlayback("");

  Serial.println("Radio connect: " + host + ":" + String(port) + path);

  if (!radioClient.connect(host.c_str(), port)) {
    audioStatus = "Nepodařilo se připojit k radiu";
    radioStatus = audioStatus;
    return false;
  }

  radioClient.setTimeout(250);

  radioClient.print("GET ");
  radioClient.print(path);
  radioClient.println(" HTTP/1.0");
  radioClient.print("Host: ");
  radioClient.println(host);
  radioClient.println("User-Agent: ESP32-S3-WebDisk/1.0");
  radioClient.println("Accept: audio/mpeg,*/*");
  radioClient.println("Icy-MetaData: 0");
  radioClient.println("Connection: close");
  radioClient.println();

  if (!skipHttpHeaders(radioClient)) {
    radioClient.stop();
    Serial.println(audioStatus);
    return false;
  }

  String ctLow = lastRadioHttp.contentType;
  ctLow.toLowerCase();
  if (ctLow.length() > 0 &&
      ctLow.indexOf("audio/mpeg") < 0 &&
      ctLow.indexOf("audio/mp3") < 0 &&
      ctLow.indexOf("application/octet-stream") < 0) {
    radioClient.stop();
    audioStatus = "Radio neni MP3 stream: " + lastRadioHttp.contentType;
    radioStatus = audioStatus;
    Serial.println(audioStatus);
    return false;
  }

  Serial.println("Radio HTTP OK, Content-Type: " + lastRadioHttp.contentType);
  Serial.println("Radio prebuffer fill start");
  if (!fillRadioPreBuffer(radioClient, RADIO_PREBUFFER_TARGET_BYTES, RADIO_PREBUFFER_MAX_BYTES)) {
    radioClient.stop();
    radioStream.clear();
    Serial.println(audioStatus);
    return false;
  }

  Serial.printf("Radio prebuffer ready: %u bytes first=%02X %02X %02X %02X clientAvail=%d\n",
                (unsigned)radioPreBufferSize,
                radioPreBufferSize > 0 ? radioPreBuffer[0] : 0,
                radioPreBufferSize > 1 ? radioPreBuffer[1] : 0,
                radioPreBufferSize > 2 ? radioPreBuffer[2] : 0,
                radioPreBufferSize > 3 ? radioPreBuffer[3] : 0,
                radioClient.available());

  radioStream.setSource(&radioClient, radioPreBuffer, radioPreBufferSize);

  audio.close();
  audio.setOutputFormat(audioOutputFormat);
  applyAudioVolume();
  audio.setBufferFrames(RADIO_PCM_BUFFER_FRAMES);
  audio.setInput(radioStream, PCMFlow::CodecKind::Mp3);

  audioStatus = "Webradio: bufferuji...";
  radioStatus = "Bufferuji: " + url;
  audioPaused = false;
  markAudioPositionStart();

  audioCbCount = 0;
  audioCbFrames = 0;
  audioCbUnderruns = 0;
  audioPumpCount = 0;

  Serial.println("Radio decoder prebuffer start");
  uint32_t prePumps = 0;
  uint32_t preGoodPumps = 0;
  unsigned long preStart = millis();

  bool startCancelled = false;

  while (millis() - preStart < RADIO_PREBUFFER_TIMEOUT_MS) {
    if (audioCommandPending()) {
      startCancelled = true;
      break;
    }

    if (audio.pump()) {
      preGoodPumps++;
    }
    prePumps++;

    if (audio.availableFrames() >= RADIO_DECODER_START_FRAMES) {
      break;
    }

    if (audio.lastError() != PCMFlow::Error::None &&
        audio.lastError() != PCMFlow::Error::NotReady) {
      break;
    }

    delay(1);
  }

  size_t preFrames = audio.availableFrames();

  if (startCancelled) {
    audio.close();
    radioStream.clear();
    freeRadioPreBuffer();
    radioClient.stop();
    audioPlaying = false;
    radioPlaying = false;
    audioStatus = "Start rádia přerušen novým povelem";
    radioStatus = audioStatus;
    return false;
  }

  Serial.printf(
    "Radio prebuffer done: pumps=%u good=%u clientAvail=%d availFrames=%u ready=%u eof=%u err=%s cb=%u frames=%u underruns=%u freeHeap=%u freePsram=%u\n",
    (unsigned)prePumps,
    (unsigned)preGoodPumps,
    radioClient.available(),
    (unsigned)preFrames,
    audio.isReady() ? 1 : 0,
    audio.isEof() ? 1 : 0,
    pcmFlowErrorName(audio.lastError()),
    (unsigned)audioCbCount,
    (unsigned)audioCbFrames,
    (unsigned)audioCbUnderruns,
    (unsigned)ESP.getFreeHeap(),
    (unsigned)ESP.getFreePsram()
  );

  if (preFrames == 0 || !audio.isReady()) {
    PCMFlow::Error realError = audio.lastError();
    bool wasReady = audio.isReady();
    bool wasEof = audio.isEof();

    audio.close();
    radioStream.clear();
    freeRadioPreBuffer();
    radioClient.stop();

    audioStatus = String("PCMFlow nedekóduje radio: ") + pcmFlowErrorName(realError) +
                  " ready=" + String(wasReady ? 1 : 0) +
                  " eof=" + String(wasEof ? 1 : 0) +
                  " ct=" + lastRadioHttp.contentType;
    radioStatus = audioStatus;
    Serial.println(audioStatus);
    return false;
  }

  sendspinSetExternalSource(true);
  audioPlaying = true;
  radioPlaying = true;
  radioUrlActive = url;
  audioStatus = "Webradio: " + url;
  radioStatus = "Hraje: " + url;

  Serial.println("Radio playback started");
  return true;
}

void copyCommandText(char *dst, size_t dstSize, const String& value) {
  if (!dst || dstSize == 0) {
    return;
  }

  String tmp = value;
  tmp.trim();
  strncpy(dst, tmp.c_str(), dstSize - 1);
  dst[dstSize - 1] = '\0';
}

bool enqueueAudioCommand(const AudioCommand& cmd) {
  if (!audioCommandQueue) {
    audioStatus = "Audio task není spuštěný";
    return false;
  }

  return xQueueSend(audioCommandQueue, &cmd, 0) == pdTRUE;
}

bool queueAudioFilePlay(const String& disk, const String& path) {
  AudioCommand cmd = {};
  cmd.type = AUDIO_CMD_PLAY_FILE;
  copyCommandText(cmd.disk, sizeof(cmd.disk), disk);
  copyCommandText(cmd.path, sizeof(cmd.path), path);
  return enqueueAudioCommand(cmd);
}

bool queueAudioFolderPlay(const String& disk, const String& dirPath) {
  AudioCommand cmd = {};
  cmd.type = AUDIO_CMD_PLAY_FOLDER;
  copyCommandText(cmd.disk, sizeof(cmd.disk), disk);
  copyCommandText(cmd.path, sizeof(cmd.path), dirPath);
  return enqueueAudioCommand(cmd);
}

bool queueAudioRadioPlay(int idx, const String& url, const String& label, bool saveResume) {
  AudioCommand cmd = {};
  cmd.type = AUDIO_CMD_PLAY_RADIO;
  cmd.index = idx;
  cmd.saveResume = saveResume;
  copyCommandText(cmd.path, sizeof(cmd.path), url);
  copyCommandText(cmd.label, sizeof(cmd.label), label);
  return enqueueAudioCommand(cmd);
}

bool queueAudioSimple(AudioCommandType type) {
  AudioCommand cmd = {};
  cmd.type = type;
  return enqueueAudioCommand(cmd);
}

void processAudioCommand(const AudioCommand& cmd) {
  switch (cmd.type) {
    case AUDIO_CMD_PLAY_FILE: {
      playlistActive = false;
      String disk = String(cmd.disk);
      String path = String(cmd.path);
      audioStatus = "Soubor: startuji " + path;
      if (!startAudioFile(disk, path)) {
        Serial.println("Audio file start failed: " + audioStatus);
      }
      break;
    }

    case AUDIO_CMD_PLAY_FOLDER: {
      String disk = String(cmd.disk);
      String dirPath = String(cmd.path);
      audioStatus = "Playlist: startuji " + dirPath;
      if (!startPlaylistFolder(disk, dirPath)) {
        Serial.println("Playlist start failed: " + audioStatus);
      }
      break;
    }

    case AUDIO_CMD_PLAY_RADIO: {
      // Prepiname na radio, tak uz nesmi zustat aktivni USB playlist.
      // Jinak dvojklik na enkoderu porad skoci na dalsi MP3 misto dalsi stanice.
      playlistActive = false;
      String url = String(cmd.path);
      String label = String(cmd.label);
      if (label.length() == 0) {
        label = url;
      }

      audioStatus = "Webradio: startuji " + label;
      radioStatus = audioStatus;

      if (startRadioStream(url)) {
        audioStatus = "Webradio: " + label;
        radioStatus = "Hraje: " + label;
        if (cmd.saveResume) {
          saveRadioResumeState(true, cmd.index, url);
        }
      } else {
        Serial.println("Radio start failed: " + audioStatus);
      }
      break;
    }

    case AUDIO_CMD_NEXT:
      if (sendspinStreamActive) {
        sendspinSendControllerCommand("next");
      } else if (!playlistActive) {
        audioStatus = "Playlist není aktivní";
      } else if (!playNextPlaylistTrack()) {
        Serial.println("Playlist next failed: " + audioStatus);
      }
      break;

    case AUDIO_CMD_PREV:
      if (sendspinStreamActive) {
        sendspinSendControllerCommand("previous");
      } else if (!playlistActive) {
        audioStatus = "Playlist není aktivní";
      } else if (!playPrevPlaylistTrack()) {
        audioStatus = "Předchozí skladba není dostupná";
        Serial.println(audioStatus);
      }
      break;

    case AUDIO_CMD_STOP:
      playlistActive = false;
      if (sendspinStreamActive) {
        sendspinSendControllerCommand("stop");
        sendspinEndStream("Zastaveno");
      } else {
        stopAudioPlayback("Zastaveno");
      }
      sendspinSetExternalSource(false);
      saveRadioResumeState(false, -1, "");
      break;

    case AUDIO_CMD_TOGGLE_PAUSE:
      toggleAudioPauseInternal();
      break;

    case AUDIO_CMD_USB_REMOUNT:
      playlistActive = false;
      stopAudioPlayback("Audio zastaveno kvůli USB remountu");
      sendspinSetExternalSource(false);
      usbRemountRequested = true;
      break;

    default:
      break;
  }
}


void toggleAudioPauseInternal() {
  if (sendspinStreamActive) {
    sendspinSendControllerCommand(sendspinGroupPlaying ? "pause" : "play");
    return;
  }

  if (audioPaused) {
    if (audioPausedAtMs > 0) {
      audioPausedAccumMs += millis() - audioPausedAtMs;
      audioPausedAtMs = 0;
    }
    audioPaused = false;
    audioPlaying = true;
    audioStatus = radioPlaying ? "Radio: pokračuji" : "Přehrávání pokračuje";
    if (radioPlaying) {
      radioStatus = "Radio pokračuje";
    }
    return;
  }

  if (!audioPlaying) {
    audioStatus = "Není co pozastavit";
    return;
  }

  audioPaused = true;
  audioPausedAtMs = millis();
  audioPlaying = false;
  audioStatus = "Pauza";
  if (radioPlaying) {
    radioStatus = "Radio pauza";
  }

  if (i2sAudioStarted) {
    i2s_zero_dma_buffer(I2S_AUDIO_PORT);
  }
}

int readEncoderAB() {
  int a = digitalRead(ENCODER_S1_PIN) ? 1 : 0;
  int b = digitalRead(ENCODER_S2_PIN) ? 1 : 0;
  return (a << 1) | b;
}

void setHardwareVolumeDelta(int delta) {
  int v = cfg.audioVolume + delta;
  if (v < 0) v = 0;
  if (v > 100) v = 100;

  if (v == cfg.audioVolume) {
    return;
  }

  cfg.audioVolume = v;
  requestAudioGainApply();
  requestAudioVolumeSave();
  sendspinSendPlayerState();
  Serial.printf("Encoder volume: %d %%\n", cfg.audioVolume);
}

bool queueConfiguredRadioStation(int idx) {
  if (idx < 0 || idx >= MAX_RADIO_STATIONS) {
    return false;
  }

  String url = cfg.radioUrl[idx];
  url.trim();
  if (url.length() == 0) {
    return false;
  }

  String label = cfg.radioName[idx];
  label.trim();
  if (label.length() == 0) {
    label = url;
  }

  audioPaused = false;
  audioStatus = "Webradio: startuji " + label;
  return queueAudioRadioPlay(idx, url, label, true);
}

bool queueFirstConfiguredRadioStation() {
  for (int i = 0; i < MAX_RADIO_STATIONS; i++) {
    if (queueConfiguredRadioStation(i)) {
      return true;
    }
  }
  return false;
}

bool queueNextConfiguredRadioStation() {
  int current = -1;

  if (radioUrlActive.length() > 0) {
    for (int i = 0; i < MAX_RADIO_STATIONS; i++) {
      if (cfg.radioUrl[i] == radioUrlActive) {
        current = i;
        break;
      }
    }
  }

  if (current < 0 && radioResumeIndex >= 0 && radioResumeIndex < MAX_RADIO_STATIONS) {
    current = radioResumeIndex;
  }

  for (int offset = 1; offset <= MAX_RADIO_STATIONS; offset++) {
    int idx = (current + offset + MAX_RADIO_STATIONS) % MAX_RADIO_STATIONS;
    if (queueConfiguredRadioStation(idx)) {
      return true;
    }
  }

  return false;
}

void handleEncoderSingleClick() {
  if (!queueAudioSimple(AUDIO_CMD_TOGGLE_PAUSE)) {
    audioStatus = "Audio fronta je plná";
  }
}

void handleEncoderDoubleClick() {
  audioPaused = false;

  if (sendspinStreamActive) {
    sendspinSendControllerCommand("next");
    return;
  }

  if (playlistActive) {
    audioStatus = "Playlist: další skladba...";
    if (!queueAudioSimple(AUDIO_CMD_NEXT)) {
      audioStatus = "Audio fronta je plná";
    }
    return;
  }

  if (!queueNextConfiguredRadioStation()) {
    audioStatus = "Není nastavená další rádio stanice";
  }
}

void handleEncoderLongPress() {
  audioPaused = false;

  if (playlistActive && playlistDisk == "usb0") {
    if (!queueFirstConfiguredRadioStation()) {
      audioStatus = "Není nastavené žádné rádio";
    }
    return;
  }

  if (!usbDiskMounted()) {
    audioStatus = "USB flashka není připojená";
    return;
  }

  audioStatus = "USB: prohledávám flashku a přehrávám MP3 včetně podsložek...";
  if (!queueAudioFolderPlay("usb0", "/")) {
    audioStatus = "USB playlist se nepodařilo zařadit";
  }
}

void initEncoderControl() {
  pinMode(ENCODER_S1_PIN, INPUT_PULLUP);
  pinMode(ENCODER_S2_PIN, INPUT_PULLUP);
  pinMode(ENCODER_KEY_PIN, INPUT_PULLUP);

  encoderLastAB = readEncoderAB();
  encoderMoveAccum = 0;

  encoderBtnRawDown = (digitalRead(ENCODER_KEY_PIN) == LOW);
  encoderBtnStableDown = encoderBtnRawDown;
  encoderBtnRawChangedMs = millis();
  encoderBtnDownMs = encoderBtnStableDown ? millis() : 0;
  encoderClickCount = 0;
  encoderLongFired = false;

  Serial.printf(
    "Encoder ready: S1=%d S2=%d KEY=%d step=%d\n",
    ENCODER_S1_PIN,
    ENCODER_S2_PIN,
    ENCODER_KEY_PIN,
    ENCODER_VOLUME_STEP
  );
}

void serviceEncoderRotation() {
  static const int8_t table[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
  };

  int ab = readEncoderAB();
  if (ab == encoderLastAB) {
    return;
  }

  int idx = (encoderLastAB << 2) | ab;
  encoderLastAB = ab;

  int8_t delta = table[idx & 0x0F];
  if (delta == 0) {
    return;
  }

  encoderMoveAccum += delta;
  if (encoderMoveAccum >= 4) {
    encoderMoveAccum = 0;
    setHardwareVolumeDelta(ENCODER_VOLUME_STEP);
  } else if (encoderMoveAccum <= -4) {
    encoderMoveAccum = 0;
    setHardwareVolumeDelta(-ENCODER_VOLUME_STEP);
  }
}

void serviceEncoderButton() {
  const uint32_t now = millis();
  const bool rawDown = (digitalRead(ENCODER_KEY_PIN) == LOW);

  if (rawDown != encoderBtnRawDown) {
    encoderBtnRawDown = rawDown;
    encoderBtnRawChangedMs = now;
  }

  if ((now - encoderBtnRawChangedMs) >= 35 && rawDown != encoderBtnStableDown) {
    encoderBtnStableDown = rawDown;

    if (encoderBtnStableDown) {
      encoderBtnDownMs = now;
      encoderLongFired = false;
    } else {
      if (!encoderLongFired) {
        encoderClickCount++;
        encoderLastClickMs = now;
      }
    }
  }

  if (encoderBtnStableDown && !encoderLongFired && (now - encoderBtnDownMs) >= 900) {
    encoderLongFired = true;
    encoderClickCount = 0;
    handleEncoderLongPress();
  }

  if (!encoderBtnStableDown && encoderClickCount > 0 && (now - encoderLastClickMs) >= 320) {
    uint8_t clicks = encoderClickCount;
    encoderClickCount = 0;

    if (clicks >= 2) {
      handleEncoderDoubleClick();
    } else {
      handleEncoderSingleClick();
    }
  }
}

void serviceEncoderControl() {
  serviceEncoderRotation();
  serviceEncoderButton();
}

void serviceAudioCommandQueue() {
  if (!audioCommandQueue) {
    return;
  }

  AudioCommand cmd;
  uint8_t processed = 0;

  while (processed < 3 && xQueueReceive(audioCommandQueue, &cmd, 0) == pdTRUE) {
    processAudioCommand(cmd);
    processed++;
  }
}

uint32_t rgbWheel(uint8_t pos) {
  pos = 255 - pos;

  if (pos < 85) {
    return ((uint32_t)(255 - pos * 3) << 16) | ((uint32_t)0 << 8) | (pos * 3);
  }

  if (pos < 170) {
    pos -= 85;
    return ((uint32_t)0 << 16) | ((uint32_t)(pos * 3) << 8) | (255 - pos * 3);
  }

  pos -= 170;
  return ((uint32_t)(pos * 3) << 16) | ((uint32_t)(255 - pos * 3) << 8) | 0;
}

void updateMusicRgbLed() {
  uint32_t now = millis();

  if (now - lastRgbLedMs < 25) {
    return;
  }

  lastRgbLedMs = now;

  if (!cfg.rgbEnabled || !audioPlaying) {
    rgbLedWrite(RGB_LED_PIN, 0, 0, 0);
    return;
  }

  uint16_t level = audioLevel;
  uint8_t brightness = map(constrain((int)level, 0, 14000), 0, 14000, 8, 160);

  rgbHue += map(constrain((int)level, 0, 14000), 0, 14000, 1, 7);

  uint32_t color = rgbWheel(rgbHue);

  uint8_t r = ((color >> 16) & 0xFF) * brightness / 255;
  uint8_t g = ((color >> 8) & 0xFF) * brightness / 255;
  uint8_t b = (color & 0xFF) * brightness / 255;

  rgbLedWrite(RGB_LED_PIN, r, g, b);

  audioLevel = (audioLevel * 7) / 10;
}



// ============================================================
// Disk abstrakce
// ============================================================

bool diskAvailable(const String& disk) {
  if (disk == "ffat") return true;
  if (disk == "usb0") return usbDiskMounted();
  return false;
}

String diskTitle(const String& disk) {
  if (disk == "ffat") return "Interní FFat";
  if (disk == "usb0") return "USB disk";
  return "Neznámý disk";
}

bool fsExistsGeneric(const String& disk, const String& path) {
  if (disk == "ffat") {
    return FFat.exists(path.c_str());
  }

  if (disk == "usb0") {
    return usbMassStorage.exists(path.c_str());
  }

  return false;
}

bool fsRemoveGeneric(const String& disk, const String& path) {
  if (disk == "ffat") {
    return FFat.remove(path.c_str());
  }

  if (disk == "usb0") {
    return usbMassStorage.remove(path.c_str());
  }

  return false;
}

File fsOpenGeneric(const String& disk, const String& path, const char* mode) {
  if (disk == "ffat") {
    return FFat.open(path.c_str(), mode);
  }

  if (disk == "usb0") {
    return usbMassStorage.open(path.c_str(), mode);
  }

  return File();
}

bool fsMkdirGeneric(const String& disk, const String& path) {
  if (disk == "ffat") {
    return FFat.mkdir(path.c_str());
  }

  if (disk == "usb0") {
    return usbMassStorage.mkdir(path.c_str());
  }

  return false;
}

bool fsRmdirGeneric(const String& disk, const String& path) {
  if (disk == "ffat") {
    return FFat.rmdir(path.c_str());
  }

  if (disk == "usb0") {
    return usbMassStorage.rmdir(path.c_str());
  }

  return false;
}

bool fsRenameGeneric(const String& disk, const String& fromPath, const String& toPath) {
  if (fromPath == "/" || toPath == "/" || fromPath == toPath) {
    return false;
  }

  if (disk == "ffat") {
    return FFat.rename(fromPath.c_str(), toPath.c_str());
  }

  if (disk == "usb0") {
    // USB MSC je připojené do VFS jako /usb, proto rename řešíme přes POSIX rename().
    // Je to bezpečnější než spoléhat na to, že konkrétní verze EspUsbHostMscFS má metodu rename().
    String fromFsPath = "/usb" + fromPath;
    String toFsPath = "/usb" + toPath;
    return ::rename(fromFsPath.c_str(), toFsPath.c_str()) == 0;
  }

  return false;
}

String normalizeDirPath(String path) {
  if (!safePath(path)) {
    return "/";
  }

  while (path.length() > 1 && path.endsWith("/")) {
    path.remove(path.length() - 1);
  }

  return path;
}

String joinPath(String dir, String name) {
  dir = normalizeDirPath(dir);
  name = urlDecode(name);
  name.replace("\\", "/");

  while (name.startsWith("/")) {
    name.remove(0, 1);
  }

  if (name.indexOf("..") >= 0 || name.length() == 0) {
    return "";
  }

  if (dir == "/") {
    return "/" + name;
  }

  return dir + "/" + name;
}

String parentPath(String path) {
  path = normalizeDirPath(path);

  if (path == "/") {
    return "/";
  }

  int pos = path.lastIndexOf('/');
  if (pos <= 0) {
    return "/";
  }

  return path.substring(0, pos);
}

String displayNameForEntry(const String& dirPath, const String& rawName) {
  String dir = normalizeDirPath(dirPath);
  String name = rawName;

  if (name.startsWith(dir) && dir != "/") {
    name = name.substring(dir.length());
    if (name.startsWith("/")) name.remove(0, 1);
  } else if (name.startsWith("/")) {
    name = fileNameFromPath(name);
  }

  return name;
}

String fullPathForEntry(const String& dirPath, const String& rawName) {
  String dir = normalizeDirPath(dirPath);
  String name = rawName;

  if (name.startsWith("/")) {
    return normalizeDirPath(name);
  }

  return joinPath(dir, name);
}

bool removeRecursiveGeneric(const String& disk, const String& path, uint8_t depth = 0) {
  if (path == "/" || depth > 12) {
    return false;
  }

  File f = fsOpenGeneric(disk, path, FILE_READ);
  if (!f) {
    return false;
  }

  if (!f.isDirectory()) {
    f.close();
    return fsRemoveGeneric(disk, path);
  }

  File entry = f.openNextFile();
  while (entry) {
    String rawName = entry.name();
    String child = fullPathForEntry(path, rawName);
    entry.close();

    if (child.length() > 0) {
      removeRecursiveGeneric(disk, child, depth + 1);
    }

    entry = f.openNextFile();
  }

  f.close();
  return fsRmdirGeneric(disk, path);
}


// ============================================================
// Web autorizace
// ============================================================

bool checkWebAuth() {
  if (cfg.webUser.length() == 0 || cfg.webPass.length() == 0) {
    return true;
  }

  if (server.authenticate(cfg.webUser.c_str(), cfg.webPass.c_str())) {
    return true;
  }

  server.requestAuthentication(BASIC_AUTH, "ESP32 Web Disk", "Přihlášení vyžadováno");
  return false;
}


// ============================================================
// Náhledy / editor / MIME
// ============================================================

const size_t MAX_TEXT_EDIT_SIZE = 220 * 1024;

String lowerString(String s) {
  s.toLowerCase();
  return s;
}

String fileExt(const String &path) {
  int slash = path.lastIndexOf('/');
  int dot = path.lastIndexOf('.');
  if (dot < 0 || dot < slash) return "";
  return lowerString(path.substring(dot + 1));
}

bool isTextExt(const String &ext) {
  return ext == "txt" || ext == "md" || ext == "ini" || ext == "cfg" ||
         ext == "log" || ext == "json" || ext == "ock" || ext == "xml" || ext == "csv" ||
         ext == "html" || ext == "htm" || ext == "css" || ext == "js" ||
         ext == "ino" || ext == "cpp" || ext == "h" || ext == "hpp" ||
         ext == "c" || ext == "py" || ext == "sh" || ext == "bat" ||
         ext == "yml" || ext == "yaml";
}

bool isImageExt(const String &ext) {
  return ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" ||
         ext == "bmp" || ext == "webp" || ext == "svg";
}

bool isAudioExt(const String &ext) {
  return ext == "mp3" || ext == "wav" || ext == "ogg" || ext == "m4a" || ext == "aac" || ext == "flac";
}

bool isVideoExt(const String &ext) {
  return ext == "mp4" || ext == "webm" || ext == "mov" || ext == "avi" || ext == "mkv";
}

String mimeForExt(const String &ext) {
  if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
  if (ext == "css") return "text/css; charset=utf-8";
  if (ext == "js") return "application/javascript; charset=utf-8";
  if (ext == "json" || ext == "ock") return "application/json; charset=utf-8";
  if (ext == "xml") return "application/xml; charset=utf-8";
  if (ext == "wasm") return "application/wasm";
  if (ext == "ico") return "image/x-icon";
  if (ext == "woff") return "font/woff";
  if (ext == "woff2") return "font/woff2";
  if (ext == "ttf") return "font/ttf";
  if (ext == "otf") return "font/otf";
  if (ext == "map") return "application/json; charset=utf-8";
  if (ext == "txt" || ext == "md" || ext == "ino" || ext == "cpp" || ext == "h" || ext == "hpp" || ext == "c" || ext == "py" || ext == "log" || ext == "cfg" || ext == "ini") return "text/plain; charset=utf-8";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "png") return "image/png";
  if (ext == "gif") return "image/gif";
  if (ext == "bmp") return "image/bmp";
  if (ext == "webp") return "image/webp";
  if (ext == "svg") return "image/svg+xml";
  if (ext == "mp3") return "audio/mpeg";
  if (ext == "wav") return "audio/wav";
  if (ext == "ogg") return "audio/ogg";
  if (ext == "m4a") return "audio/mp4";
  if (ext == "aac") return "audio/aac";
  if (ext == "flac") return "audio/flac";
  if (ext == "mp4") return "video/mp4";
  if (ext == "webm") return "video/webm";
  if (ext == "mov") return "video/quicktime";
  if (ext == "avi") return "video/x-msvideo";
  if (ext == "mkv") return "video/x-matroska";
  return "application/octet-stream";
}

bool getDiskPathArgs(String &disk, String &path) {
  disk = server.hasArg("disk") ? server.arg("disk") : "ffat";
  if (disk != "ffat" && disk != "usb0") return false;
  if (!server.hasArg("f")) return false;
  path = server.arg("f");
  if (!safePath(path)) return false;
  return diskAvailable(disk) && fsExistsGeneric(disk, path);
}

uint64_t usedBytesRecursive(fs::FS &fs, const String &path, uint8_t depth = 0) {
  if (depth > 8) return 0;

  File root = fs.open(path.c_str(), FILE_READ);
  if (!root) return 0;

  if (!root.isDirectory()) {
    uint64_t size = root.size();
    root.close();
    return size;
  }

  uint64_t total = 0;
  File entry = root.openNextFile();

  while (entry) {
    String name = entry.name();
    bool dir = entry.isDirectory();
    uint64_t size = entry.size();
    entry.close();

    if (dir) {
      if (name != "/System Volume Information" && name.indexOf("/System Volume Information/") < 0) {
        total += usedBytesRecursive(fs, name, depth + 1);
      }
    } else {
      total += size;
    }

    entry = root.openNextFile();
  }

  root.close();
  return total;
}

// ============================================================
// Jednoduchý FTP server pro FFat/USB
// ============================================================

void ftpReply(const String &msg) {
  if (ftpClient && ftpClient.connected()) {
    ftpClient.print(msg);
    ftpClient.print("\r\n");
  }
}

void ftpResetSession() {
  if (ftpDataClient) ftpDataClient.stop();
  ftpDataServer.end();
  ftpPassiveReady = false;
  ftpLoggedIn = false;
  ftpUserOk = false;
  ftpCwd = "/";
  ftpRenameFrom = "";
  ftpLine = "";
}

String ftpDisk() {
  if (cfg.ftpDisk == "ffat") return "ffat";
  if (cfg.ftpDisk == "usb0") return "usb0";
  return "usb0";
}

String ftpResolvePath(String arg) {
  arg.trim();

  if (arg.startsWith("\"") && arg.endsWith("\"") && arg.length() >= 2) {
    arg = arg.substring(1, arg.length() - 1);
  }

  arg.replace("\\", "/");

  String path;
  if (arg.length() == 0) {
    path = ftpCwd;
  } else if (arg.startsWith("/")) {
    path = arg;
  } else if (ftpCwd == "/") {
    path = "/" + arg;
  } else {
    path = ftpCwd + "/" + arg;
  }

  while (path.indexOf("//") >= 0) path.replace("//", "/");

  if (path.indexOf("..") >= 0) return "/";
  if (!path.startsWith("/")) path = "/" + path;
  if (path.length() > 1 && path.endsWith("/")) path.remove(path.length() - 1);

  return path;
}

bool ftpOpenPassiveData() {
  if (!ftpPassiveReady) {
    ftpReply("425 Use PASV first.");
    return false;
  }

  unsigned long start = millis();
  while (millis() - start < 8000) {
    ftpDataClient = ftpDataServer.available();
    if (ftpDataClient && ftpDataClient.connected()) {
      return true;
    }
    delay(1);
  }

  ftpReply("425 Data connection timeout.");
  ftpDataServer.end();
  ftpPassiveReady = false;
  return false;
}

void ftpCloseData() {
  if (ftpDataClient) ftpDataClient.stop();
  ftpDataServer.end();
  ftpPassiveReady = false;
}

void ftpStartServerIfNeeded() {
  if (cfg.ftpEnabled && !ftpServerStarted) {
    ftpServer.begin();
    ftpServerStarted = true;
    Serial.println("FTP server started on port 21");
  } else if (!cfg.ftpEnabled && ftpServerStarted) {
    if (ftpClient) ftpClient.stop();
    if (ftpDataClient) ftpDataClient.stop();
    ftpDataServer.end();
    ftpServer.end();
    ftpServerStarted = false;
    ftpResetSession();
    Serial.println("FTP server stopped");
  }
}

String ftpListLine(File &file) {
  String name = fileNameFromPath(file.name());
  String line;

  if (file.isDirectory()) {
    line += "drwxr-xr-x 1 owner group ";
    line += "0";
  } else {
    line += "-rw-r--r-- 1 owner group ";
    line += String((unsigned long)file.size());
  }

  line += " Jan 01 00:00 ";
  line += name;
  line += "\r\n";
  return line;
}

void ftpDoList(bool namesOnly, String arg) {
  if (!ftpOpenPassiveData()) return;

  String disk = ftpDisk();
  if (!diskAvailable(disk)) {
    ftpCloseData();
    ftpReply("550 FTP disk not available.");
    return;
  }

  String path = ftpResolvePath(arg);
  File root = fsOpenGeneric(disk, path, FILE_READ);

  if (!root) {
    ftpCloseData();
    ftpReply("550 Cannot open directory.");
    return;
  }

  ftpReply("150 Opening data connection.");

  if (root.isDirectory()) {
    File file = root.openNextFile();
    while (file) {
      if (namesOnly) {
        ftpDataClient.print(fileNameFromPath(file.name()));
        ftpDataClient.print("\r\n");
      } else {
        ftpDataClient.print(ftpListLine(file));
      }
      file.close();
      file = root.openNextFile();
      delay(1);
    }
  } else {
    if (namesOnly) {
      ftpDataClient.print(fileNameFromPath(root.name()));
      ftpDataClient.print("\r\n");
    } else {
      ftpDataClient.print(ftpListLine(root));
    }
  }

  root.close();
  ftpCloseData();
  ftpReply("226 Transfer complete.");
}


void ftpTransferYield(uint32_t &lastYieldMs) {
  uint32_t now = millis();
  if (now - lastYieldMs >= 2) {
    // FTP běží v hlavním loopu, audio/USB běží ve vlastních FreeRTOS taskech.
    // Krátké uvolnění CPU zabrání tomu, aby dlouhé kopírování zadusilo dekodér.
    vTaskDelay(pdMS_TO_TICKS(1));
    lastYieldMs = millis();
  }
}

void ftpDoRetr(String arg) {
  String disk = ftpDisk();
  String path = ftpResolvePath(arg);

  if (!diskAvailable(disk) || !fsExistsGeneric(disk, path)) {
    ftpReply("550 File not found.");
    return;
  }

  File file = fsOpenGeneric(disk, path, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    ftpReply("550 Cannot open file.");
    return;
  }

  if (!ftpOpenPassiveData()) {
    file.close();
    return;
  }

  ftpReply("150 Opening binary mode data connection.");

  uint8_t buf[1460];
  uint32_t lastYieldMs = millis();
  while (file.available() && ftpDataClient.connected()) {
    size_t n = file.read(buf, sizeof(buf));
    if (n) ftpDataClient.write(buf, n);
    ftpTransferYield(lastYieldMs);
  }

  file.close();
  ftpCloseData();
  ftpReply("226 Transfer complete.");
}

void ftpDoStor(String arg) {
  String disk = ftpDisk();
  String path = ftpResolvePath(arg);

  if (!diskAvailable(disk)) {
    ftpReply("550 FTP disk not available.");
    return;
  }

  if (fsExistsGeneric(disk, path)) {
    fsRemoveGeneric(disk, path);
  }

  File file = fsOpenGeneric(disk, path, FILE_WRITE);
  if (!file) {
    ftpReply("550 Cannot create file.");
    return;
  }

  if (!ftpOpenPassiveData()) {
    file.close();
    return;
  }

  ftpReply("150 Ok to send data.");

  uint8_t buf[1460];
  unsigned long lastData = millis();
  uint32_t lastYieldMs = millis();

  while (ftpDataClient.connected() || ftpDataClient.available()) {
    int avail = ftpDataClient.available();
    if (avail > 0) {
      size_t n = ftpDataClient.read(buf, min((int)sizeof(buf), avail));
      if (n) {
        file.write(buf, n);
        lastData = millis();
        ftpTransferYield(lastYieldMs);
      }
    } else {
      if (millis() - lastData > 2000) break;
      ftpTransferYield(lastYieldMs);
    }
  }

  file.close();
  ftpCloseData();
  ftpReply("226 Transfer complete.");
}

void ftpDoSize(String arg) {
  String disk = ftpDisk();
  String path = ftpResolvePath(arg);

  File file = fsOpenGeneric(disk, path, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    ftpReply("550 Cannot get size.");
    return;
  }

  ftpReply("213 " + String((unsigned long)file.size()));
  file.close();
}

void ftpDoMkd(String arg) {
  String disk = ftpDisk();
  String path = ftpResolvePath(arg);

  if (!diskAvailable(disk)) {
    ftpReply("550 FTP disk not available.");
    return;
  }

  if (!safePath(path) || path == "/" || fsExistsGeneric(disk, path)) {
    ftpReply("550 Bad or existing directory name.");
    return;
  }

  if (fsMkdirGeneric(disk, path)) {
    ftpReply("257 \"" + path + "\" directory created.");
  } else {
    ftpReply("550 Create directory failed.");
  }
}

void ftpDoRmd(String arg) {
  String disk = ftpDisk();
  String path = ftpResolvePath(arg);

  if (!diskAvailable(disk)) {
    ftpReply("550 FTP disk not available.");
    return;
  }

  File d = fsOpenGeneric(disk, path, FILE_READ);
  bool isDir = d && d.isDirectory();
  if (d) d.close();

  if (!isDir || path == "/") {
    ftpReply("550 Directory not found.");
    return;
  }

  if (fsRmdirGeneric(disk, path)) {
    ftpReply("250 Directory removed.");
  } else {
    ftpReply("550 Remove directory failed. Directory may not be empty.");
  }
}

void ftpDoRnfr(String arg) {
  String disk = ftpDisk();
  String path = ftpResolvePath(arg);

  if (!diskAvailable(disk)) {
    ftpReply("550 FTP disk not available.");
    return;
  }

  if (!safePath(path) || path == "/" || !fsExistsGeneric(disk, path)) {
    ftpRenameFrom = "";
    ftpReply("550 File or directory not found.");
    return;
  }

  ftpRenameFrom = path;
  ftpReply("350 Ready for RNTO.");
}

void ftpDoRnto(String arg) {
  if (ftpRenameFrom.length() == 0) {
    ftpReply("503 RNFR required first.");
    return;
  }

  String disk = ftpDisk();
  String toPath = ftpResolvePath(arg);

  if (!diskAvailable(disk)) {
    ftpRenameFrom = "";
    ftpReply("550 FTP disk not available.");
    return;
  }

  if (!safePath(toPath) || toPath == "/" || fsExistsGeneric(disk, toPath)) {
    ftpRenameFrom = "";
    ftpReply("550 Bad or existing target name.");
    return;
  }

  if (parentPath(ftpRenameFrom) != parentPath(toPath)) {
    ftpRenameFrom = "";
    ftpReply("550 Rename across directories is disabled.");
    return;
  }

  if (fsRenameGeneric(disk, ftpRenameFrom, toPath)) {
    ftpRenameFrom = "";
    ftpReply("250 Rename successful.");
  } else {
    ftpRenameFrom = "";
    ftpReply("550 Rename failed.");
  }
}

void ftpDoCommand(String line) {
  line.trim();
  if (line.length() == 0) return;

  int sp = line.indexOf(' ');
  String cmd = sp >= 0 ? line.substring(0, sp) : line;
  String arg = sp >= 0 ? line.substring(sp + 1) : "";
  cmd.toUpperCase();

  if (cmd == "USER") {
    ftpUserOk = (arg == cfg.ftpUser);
    ftpReply("331 Password required.");
    return;
  }

  if (cmd == "PASS") {
    if (ftpUserOk && arg == cfg.ftpPass) {
      ftpLoggedIn = true;
      ftpReply("230 Logged in.");
    } else {
      ftpLoggedIn = false;
      ftpReply("530 Login incorrect.");
    }
    return;
  }

  if (cmd == "QUIT") {
    ftpReply("221 Bye.");
    ftpClient.stop();
    ftpResetSession();
    return;
  }

  if (!ftpLoggedIn) {
    ftpReply("530 Please login with USER and PASS.");
    return;
  }

  if (cmd == "SYST") ftpReply("215 UNIX Type: L8");
  else if (cmd == "FEAT") ftpReply("211-Features\r\n PASV\r\n SIZE\r\n MKD\r\n RMD\r\n RNFR\r\n RNTO\r\n UTF8\r\n211 End");
  else if (cmd == "OPTS") ftpReply("200 OK");
  else if (cmd == "TYPE") ftpReply("200 Type set.");
  else if (cmd == "NOOP") ftpReply("200 OK");
  else if (cmd == "PWD" || cmd == "XPWD") ftpReply("257 \"" + ftpCwd + "\"");
  else if (cmd == "CWD") {
    String p = ftpResolvePath(arg);
    File d = fsOpenGeneric(ftpDisk(), p, FILE_READ);
    if (d && d.isDirectory()) {
      ftpCwd = p;
      ftpReply("250 Directory changed.");
    } else {
      ftpReply("550 Directory not found.");
    }
    if (d) d.close();
  }
  else if (cmd == "CDUP") {
    if (ftpCwd != "/") {
      int pos = ftpCwd.lastIndexOf('/');
      ftpCwd = pos <= 0 ? "/" : ftpCwd.substring(0, pos);
    }
    ftpReply("250 Directory changed.");
  }
  else if (cmd == "PASV") {
    ftpDataServer.end();
    ftpDataServer.begin();
    ftpPassiveReady = true;

    IPAddress ip = WiFi.softAPIP();
    if (WiFi.status() == WL_CONNECTED) {
      ip = WiFi.localIP();
    }

    uint8_t p1 = FTP_DATA_PORT / 256;
    uint8_t p2 = FTP_DATA_PORT % 256;
    ftpReply("227 Entering Passive Mode (" + String(ip[0]) + "," + String(ip[1]) + "," + String(ip[2]) + "," + String(ip[3]) + "," + String(p1) + "," + String(p2) + ").");
  }
  else if (cmd == "LIST") ftpDoList(false, arg);
  else if (cmd == "NLST") ftpDoList(true, arg);
  else if (cmd == "RETR") ftpDoRetr(arg);
  else if (cmd == "STOR") ftpDoStor(arg);
  else if (cmd == "SIZE") ftpDoSize(arg);
  else if (cmd == "DELE") {
    String p = ftpResolvePath(arg);
    if (fsRemoveGeneric(ftpDisk(), p)) ftpReply("250 Deleted.");
    else ftpReply("550 Delete failed.");
  }
  else if (cmd == "MKD" || cmd == "XMKD") ftpDoMkd(arg);
  else if (cmd == "RMD" || cmd == "XRMD") ftpDoRmd(arg);
  else if (cmd == "RNFR") ftpDoRnfr(arg);
  else if (cmd == "RNTO") ftpDoRnto(arg);
  else {
    ftpReply("502 Command not implemented.");
  }
}

void handleAudioPlay() {
  if (!checkWebAuth()) return;

  String disk = server.arg("disk");
  String path = server.arg("f");

  if (!safePath(path)) {
    server.send(400, "text/plain", "Bad path");
    return;
  }

  playlistActive = false;
  audioStatus = "Soubor: startuji " + path;

  if (!queueAudioFilePlay(disk, path)) {
    server.send(503, "text/plain", "Audio fronta je plná");
    return;
  }

  server.send(200, "text/plain", audioStatus);
}

void handleAudioPlayFolder() {
  if (!checkWebAuth()) return;

  String disk = server.hasArg("disk") ? server.arg("disk") : currentDisk;
  if (disk != "ffat" && disk != "usb0") {
    server.send(400, "text/plain", "Bad disk");
    return;
  }

  String dirPath = server.hasArg("p") ? server.arg("p") : "/";
  dirPath = normalizeDirPath(dirPath);

  audioStatus = "Playlist: startuji " + dirPath;

  if (!queueAudioFolderPlay(disk, dirPath)) {
    server.send(503, "text/plain", "Audio fronta je plná");
    return;
  }

  server.send(200, "text/plain", audioStatus);
}



void handleAudioNext() {
  if (!checkWebAuth()) return;

  if (!playlistActive) {
    server.send(400, "text/plain", "Playlist není aktivní");
    return;
  }

  audioStatus = "Playlist: další skladba...";

  if (!queueAudioSimple(AUDIO_CMD_NEXT)) {
    server.send(503, "text/plain", "Audio fronta je plná");
    return;
  }

  server.send(200, "text/plain", audioStatus);
}

void handleAudioPrev() {
  if (!checkWebAuth()) return;

  if (!playlistActive) {
    server.send(400, "text/plain", "Playlist není aktivní");
    return;
  }

  audioStatus = "Playlist: předchozí skladba...";

  if (!queueAudioSimple(AUDIO_CMD_PREV)) {
    server.send(503, "text/plain", "Audio fronta je plná");
    return;
  }

  server.send(200, "text/plain", audioStatus);
}

void handleAudioShuffle() {
  if (!checkWebAuth()) return;

  playlistShuffle = !playlistShuffle;
  server.send(200, "text/plain", playlistShuffle ? "Shuffle zapnuto" : "Shuffle vypnuto");
}

void handleAudioRepeat() {
  if (!checkWebAuth()) return;

  playlistRepeat = !playlistRepeat;
  server.send(200, "text/plain", playlistRepeat ? "Repeat složky zapnuto" : "Repeat složky vypnuto");
}

void handleAudioStop() {
  if (!checkWebAuth()) return;

  playlistActive = false;
  audioStatus = "Zastavuji...";
  saveRadioResumeState(false, -1, "");

  if (!queueAudioSimple(AUDIO_CMD_STOP)) {
    server.send(503, "text/plain", "Audio fronta je plná");
    return;
  }

  server.send(200, "text/plain", "Zastavuji...");
}

void handleAudioVolume() {
  if (!checkWebAuth()) return;

  if (!server.hasArg("v")) {
    server.send(400, "text/plain", "Missing volume");
    return;
  }

  int v = server.arg("v").toInt();

  if (v < 0) v = 0;
  if (v > 100) v = 100;

  cfg.audioVolume = v;
  requestAudioGainApply();
  requestAudioVolumeSave();

  server.send(200, "text/plain", String(cfg.audioVolume));
}


void handleAudioEqJson() {
  if (!checkWebAuth()) return;

  String json = "{";
  json.reserve(256);
  json += "\"enabled\":" + String(cfg.audioEqEnabled ? "true" : "false") + ",";
  json += "\"preampDb\":" + String(cfg.audioEqPreampDb) + ",";
  json += "\"autoHeadroom\":" + String(cfg.audioEqAutoHeadroom ? "true" : "false") + ",";
  json += "\"bands\":" + audioEqBandsJson() + ",";
  json += "\"outputGainDb\":" + String(cfg.audioOutputGainDb) + ",";
  json += "\"volumeCurve\":" + String(cfg.audioVolumeCurve, 2) + ",";
  json += "\"volume\":" + String(cfg.audioVolume) + ",";
  json += "\"effectiveGain\":" + String(currentAudioGain(), 5);
  json += "}";

  server.send(200, "application/json; charset=utf-8", json);
}

void handleAudioEqSave() {
  if (!checkWebAuth()) return;

  if (server.hasArg("enabled")) {
    cfg.audioEqEnabled = server.arg("enabled").toInt() != 0;
  }
  if (server.hasArg("preamp_db")) {
    cfg.audioEqPreampDb = clampEqPreampDb(server.arg("preamp_db").toInt());
  }
  if (server.hasArg("auto_headroom")) {
    cfg.audioEqAutoHeadroom = server.arg("auto_headroom").toInt() != 0;
  }
  if (server.hasArg("output_gain_db")) {
    cfg.audioOutputGainDb = clampOutputGainDb(server.arg("output_gain_db").toInt());
  }
  if (server.hasArg("volume_curve")) {
    cfg.audioVolumeCurve = clampVolumeCurve(server.arg("volume_curve").toFloat());
  }

  for (uint8_t i = 0; i < AUDIO_EQ_BANDS; i++) {
    String argName = "band_" + String(i);
    if (server.hasArg(argName)) {
      cfg.audioEqBandDb[i] = clampEqDb(server.arg(argName).toInt());
    }
  }

  clampDetailedAudioConfig();
  resetAudioEqualizers();
  requestAudioGainApply();

  if (!saveConfig()) {
    server.send(500, "text/plain; charset=utf-8", "Nepodařilo se uložit nastavení zvuku");
    return;
  }

  handleAudioEqJson();
}


void handleAudioStopAjax() {
  if (!checkWebAuth()) return;

  playlistActive = false;
  audioStatus = "Zastavuji...";
  saveRadioResumeState(false, -1, "");

  if (!queueAudioSimple(AUDIO_CMD_STOP)) {
    server.send(503, "text/plain", "Audio fronta je plná");
    return;
  }

  server.send(200, "text/plain", "Zastavuji...");
}

void handleAudioTogglePause() {
  if (!checkWebAuth()) return;

  if (!audioPlaying && !audioPaused) {
    server.send(400, "text/plain; charset=utf-8", "Není co pozastavit");
    return;
  }

  if (!queueAudioSimple(AUDIO_CMD_TOGGLE_PAUSE)) {
    server.send(503, "text/plain; charset=utf-8", "Audio fronta je plná");
    return;
  }

  server.send(200, "text/plain; charset=utf-8", audioPaused ? "Pokračuji..." : "Pozastavuji...");
}


void ftpHandle() {
  ftpStartServerIfNeeded();
  if (!cfg.ftpEnabled || !ftpServerStarted) return;

  if (!ftpClient || !ftpClient.connected()) {
    WiFiClient newClient = ftpServer.available();
    if (newClient) {
      if (ftpClient) ftpClient.stop();
      ftpClient = newClient;
      ftpResetSession();
      ftpClient = newClient;
      ftpReply("220 ESP32 WebDisk FTP ready.");
      Serial.println("FTP client connected");
    }
    return;
  }

  while (ftpClient.available()) {
    char c = ftpClient.read();
    if (c == '\r') continue;
    if (c == '\n') {
      String line = ftpLine;
      ftpLine = "";
      Serial.println("FTP> " + line);
      ftpDoCommand(line);
    } else if (ftpLine.length() < 256) {
      ftpLine += c;
    }
  }
}


// ============================================================
// Statický web z FFat
// ============================================================

bool serveWebPage(const char* path) {
  if (!checkWebAuth()) return false;

  if (!path || !FFat.exists(path)) {
    server.sendHeader("Location", "/rescue");
    server.send(303);
    return false;
  }

  File file = FFat.open(path, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    server.send(500, "text/plain; charset=utf-8", "Webovou stránku nelze otevřít");
    return false;
  }

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.streamFile(file, "text/html; charset=utf-8");
  file.close();
  return true;
}

// ============================================================
// Společné HTML
// ============================================================








void handleWifiScanStart() {
  if (!checkWebAuth()) return;

  if (radioPlaying) {
    server.send(409, "text/plain; charset=utf-8", "Během internetového rádia Wi-Fi scan nespouštím. Nejdřív rádio zastav.");
    return;
  }
  if (wifiScanOwner != WIFI_SCAN_NONE || WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
    server.send(409, "text/plain; charset=utf-8", "Wi-Fi scan už běží");
    return;
  }

  WiFi.scanDelete();
  int result = WiFi.scanNetworks(true, true);
  if (result == WIFI_SCAN_FAILED) {
    server.send(500, "text/plain; charset=utf-8", "Wi-Fi scan se nepodařilo spustit");
    return;
  }

  wifiScanOwner = WIFI_SCAN_MANUAL;
  wifiManualScanStartedMs = millis();
  server.send(202, "text/plain; charset=utf-8", "Scan spuštěn");
}

void handleWifiScanJson() {
  if (!checkWebAuth()) return;

  if (wifiScanOwner != WIFI_SCAN_MANUAL) {
    server.send(200, "application/json; charset=utf-8", "{\"running\":false,\"ok\":false,\"error\":\"Ruční scan neběží\"}");
    return;
  }

  int found = WiFi.scanComplete();
  if (found == WIFI_SCAN_RUNNING) {
    server.send(200, "application/json; charset=utf-8", "{\"running\":true}");
    return;
  }

  if (found < 0) {
    WiFi.scanDelete();
    wifiScanOwner = WIFI_SCAN_NONE;
    wifiManualScanStartedMs = 0;
    server.send(200, "application/json; charset=utf-8", "{\"running\":false,\"ok\":false,\"error\":\"Scan selhal\"}");
    return;
  }

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json; charset=utf-8", "");
  server.sendContent("{\"running\":false,\"ok\":true,\"networks\":[");

  bool first = true;
  uint8_t sent = 0;
  for (int i = 0; i < found && sent < MAX_WIFI_SCAN_RESULTS; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;

    bool duplicate = false;
    for (int j = 0; j < i; j++) {
      if (WiFi.SSID(j) == ssid && WiFi.RSSI(j) >= WiFi.RSSI(i)) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;

    if (!first) server.sendContent(",");
    first = false;
    String item = "{\"ssid\":\"" + jsonEscape(ssid) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
                  ",\"secure\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true") + "}";
    server.sendContent(item);
    sent++;
  }

  server.sendContent("]}");
  server.sendContent("");
  WiFi.scanDelete();
  wifiScanOwner = WIFI_SCAN_NONE;
  wifiManualScanStartedMs = 0;
  wifiLastRetryMs = millis();
}

void handleWifiAdd() {
  if (!checkWebAuth()) return;

  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  ssid.trim();

  int index = -1;
  if (!addOrUpdateSavedWifi(ssid, pass, index)) {
    server.send(400, "text/plain; charset=utf-8", "SSID je neplatné nebo je seznam plný");
    return;
  }

  if (!saveConfig()) {
    server.send(500, "text/plain; charset=utf-8", "Wi-Fi se nepodařilo uložit");
    return;
  }

  server.sendHeader("Location", "/config");
  server.send(303);
  delay(100);
  connectSavedWifiDirect((uint8_t)index);
}

void handleWifiConnect() {
  if (!checkWebAuth()) return;
  int index = server.arg("i").toInt();
  if (index < 0 || index >= cfg.wifiCount) {
    server.send(400, "text/plain; charset=utf-8", "Neplatná Wi-Fi síť");
    return;
  }

  if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == cfg.wifiSsid[index]) {
    server.sendHeader("Location", "/config");
    server.send(303);
    return;
  }

  server.sendHeader("Location", "/config");
  server.send(303);
  delay(100);
  connectSavedWifiDirect((uint8_t)index);
}

void handleWifiDelete() {
  if (!checkWebAuth()) return;
  int index = server.arg("i").toInt();
  if (index < 0 || index >= cfg.wifiCount) {
    server.send(400, "text/plain; charset=utf-8", "Neplatná Wi-Fi síť");
    return;
  }

  String removedSsid = cfg.wifiSsid[index];
  bool removingCurrent = WiFi.status() == WL_CONNECTED && WiFi.SSID() == removedSsid;
  removeSavedWifi((uint8_t)index);
  saveConfig();

  server.sendHeader("Location", "/config");
  server.send(303);

  if (removingCurrent) {
    delay(100);
    WiFi.disconnect(false, false);
    requestSavedWifiReconnect(true);
  }
}

// ============================================================
// /config
// ============================================================

void handleConfigPage() {
  serveConfigPageWithSmartSpeakerPanel();
}


void handleConfigSave() {
  String apSsid = server.arg("ap_ssid");
  String apPass = server.arg("ap_pass");
  int apModeValue = server.hasArg("ap_mode") ? server.arg("ap_mode").toInt() : (int)cfg.apMode;
  String mdnsName = server.arg("mdns_name");
  String webUser = server.arg("web_user");
  String webPass = server.arg("web_pass");
  bool ftpEnabled = server.hasArg("ftp_enabled");
  String ftpUser = server.arg("ftp_user");
  String ftpPass = server.arg("ftp_pass");
  String ftpDiskCfg = server.arg("ftp_disk");
  bool rgbEnabled = server.hasArg("rgb_enabled");
  int audioVolume = server.hasArg("audio_volume") ? server.arg("audio_volume").toInt() : cfg.audioVolume;
  int audioBassDb = server.hasArg("audio_bass_db") ? server.arg("audio_bass_db").toInt() : cfg.audioBassDb;
  int audioTrebleDb = server.hasArg("audio_treble_db") ? server.arg("audio_treble_db").toInt() : cfg.audioTrebleDb;
  bool audioEqEnabled = server.hasArg("audio_eq_enabled")
    ? server.arg("audio_eq_enabled").toInt() != 0
    : cfg.audioEqEnabled;
  int audioEqPreampDb = server.hasArg("audio_eq_preamp_db")
    ? server.arg("audio_eq_preamp_db").toInt()
    : cfg.audioEqPreampDb;
  bool audioEqAutoHeadroom = server.hasArg("audio_eq_auto_headroom")
    ? server.arg("audio_eq_auto_headroom").toInt() != 0
    : cfg.audioEqAutoHeadroom;
  int audioOutputGainDb = server.hasArg("audio_output_gain_db")
    ? server.arg("audio_output_gain_db").toInt()
    : cfg.audioOutputGainDb;
  float audioVolumeCurve = server.hasArg("audio_volume_curve")
    ? server.arg("audio_volume_curve").toFloat()
    : cfg.audioVolumeCurve;
  int audioEqBandDb[AUDIO_EQ_BANDS];
  bool detailedEqSubmitted = server.hasArg("audio_eq_band_0");
  for (uint8_t i = 0; i < AUDIO_EQ_BANDS; i++) {
    String argName = "audio_eq_band_" + String(i);
    audioEqBandDb[i] = server.hasArg(argName)
      ? server.arg(argName).toInt()
      : cfg.audioEqBandDb[i];
  }
  bool batteryEnabled = server.hasArg("battery_enabled");
  float batteryDividerRatio = server.hasArg("battery_divider_ratio") ? server.arg("battery_divider_ratio").toFloat() : cfg.batteryDividerRatio;
  float batteryCalibration = server.hasArg("battery_calibration") ? server.arg("battery_calibration").toFloat() : cfg.batteryCalibration;
  String radioName = server.arg("radio_name");
  String radioUrl = server.arg("radio_url");

  apSsid.trim();
  apPass.trim();
  mdnsName = normalizeMdnsName(mdnsName);
  webUser.trim();
  webPass.trim();
  ftpUser.trim();
  ftpPass.trim();
  ftpDiskCfg.trim();
  radioName.trim();
  radioUrl.trim();

  if (apSsid.length() == 0) {
    server.send(400, "text/plain", "AP SSID nesmi byt prazdne");
    return;
  }

  if (apPass.length() > 0 && apPass.length() < 8) {
    server.send(400, "text/plain", "AP heslo musi mit aspon 8 znaku, nebo ho nech prazdne");
    return;
  }

  if (apModeValue < AP_MODE_ALWAYS || apModeValue > AP_MODE_OFF) {
    server.send(400, "text/plain", "Neplatny rezim AP");
    return;
  }

  if (webUser.length() == 0 || webPass.length() == 0) {
    server.send(400, "text/plain", "Web uzivatel a heslo nesmi byt prazdne");
    return;
  }

  if (ftpEnabled && (ftpUser.length() == 0 || ftpPass.length() == 0)) {
    server.send(400, "text/plain", "FTP uzivatel a heslo nesmi byt prazdne");
    return;
  }

  if (ftpDiskCfg != "ffat" && ftpDiskCfg != "usb0") {
    ftpDiskCfg = "usb0";
  }

  if (audioVolume < 0) audioVolume = 0;
  if (audioVolume > 100) audioVolume = 100;
  audioBassDb = clampEqDb(audioBassDb);
  audioTrebleDb = clampEqDb(audioTrebleDb);

  // Starší klient může stále poslat jen basy a výšky.
  if (!detailedEqSubmitted &&
      (server.hasArg("audio_bass_db") || server.hasArg("audio_treble_db"))) {
    for (uint8_t i = 0; i < 4; i++) audioEqBandDb[i] = audioBassDb;
    for (uint8_t i = 7; i < AUDIO_EQ_BANDS; i++) audioEqBandDb[i] = audioTrebleDb;
  }
  audioEqPreampDb = clampEqPreampDb(audioEqPreampDb);
  audioOutputGainDb = clampOutputGainDb(audioOutputGainDb);
  audioVolumeCurve = clampVolumeCurve(audioVolumeCurve);
  for (uint8_t i = 0; i < AUDIO_EQ_BANDS; i++) {
    audioEqBandDb[i] = clampEqDb(audioEqBandDb[i]);
  }

  if (!isfinite(batteryDividerRatio) || batteryDividerRatio < 1.0f || batteryDividerRatio > 10.0f) {
    server.send(400, "text/plain", "Pomer baterioveho delice musi byt 1.0 az 10.0");
    return;
  }
  if (!isfinite(batteryCalibration) || batteryCalibration < 0.5f || batteryCalibration > 1.5f) {
    server.send(400, "text/plain", "Kalibrace baterie musi byt 0.5 az 1.5");
    return;
  }

  if (radioName.length() == 0) {
    radioName = "Moje radio";
  }

  cfg.apSsid = apSsid;
  cfg.apPass = apPass;
  cfg.apMode = (ApOperatingMode)apModeValue;
  cfg.mdnsName = mdnsName;
  cfg.webUser = webUser;
  cfg.webPass = webPass;
  cfg.ftpEnabled = ftpEnabled;
  cfg.ftpUser = ftpUser;
  cfg.ftpPass = ftpPass;
  cfg.ftpDisk = ftpDiskCfg;
  cfg.rgbEnabled = rgbEnabled;
  cfg.audioVolume = audioVolume;
  cfg.audioEqEnabled = audioEqEnabled;
  cfg.audioEqPreampDb = audioEqPreampDb;
  cfg.audioEqAutoHeadroom = audioEqAutoHeadroom;
  cfg.audioOutputGainDb = audioOutputGainDb;
  cfg.audioVolumeCurve = audioVolumeCurve;
  for (uint8_t i = 0; i < AUDIO_EQ_BANDS; i++) {
    cfg.audioEqBandDb[i] = audioEqBandDb[i];
  }
  syncLegacyEqFields();
  cfg.batteryEnabled = batteryEnabled;
  cfg.batteryDividerRatio = batteryDividerRatio;
  cfg.batteryCalibration = batteryCalibration;
  initBatteryMonitor();
  resetAudioEqualizers();
  requestAudioGainApply();
  saveAudioVolumeStateNow();
  cfg.radioName[0] = radioName;
  cfg.radioUrl[0] = radioUrl;

  if (!saveConfig()) {
    server.send(500, "text/plain", "Nepodarilo se ulozit konfiguraci");
    return;
  }

  // Odpověď odešleme ještě přes současné spojení. Změna AP se provede
  // až potom v loopu, takže se neztratí HTTP redirect při vypnutí AP.
  ftpStartServerIfNeeded();
  server.sendHeader("Location", "/config");
  server.send(303);
  scheduleApPolicyApply(true, 700);
}

void handleReboot() {
  server.send(200, "text/plain", "Restartuju...");
  delay(500);
  ESP.restart();
}

// ============================================================
// /files
// ============================================================



void handleFilesPage() {
  serveWebPage("/www/files.html");
}



// ============================================================
// Náhled / editor / updater
// ============================================================

void handleRawFile() {
  String disk, path;
  if (!getDiskPathArgs(disk, path)) {
    server.send(404, "text/plain", "File not found");
    return;
  }

  File file = fsOpenGeneric(disk, path, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    server.send(400, "text/plain", "Cannot open file");
    return;
  }

  String ext = fileExt(path);
  server.streamFile(file, mimeForExt(ext));
  file.close();
}

void handleViewFile() {
  serveWebPage("/www/view.html");
}


void handleEditFile() {
  serveWebPage("/www/edit.html");
}


void handleSaveFile() {
  String disk = server.hasArg("disk") ? server.arg("disk") : "ffat";
  if (disk != "ffat" && disk != "usb0") {
    server.send(400, "text/plain", "Bad disk");
    return;
  }
  if (!server.hasArg("f")) {
    server.send(400, "text/plain", "Missing file");
    return;
  }
  if (!diskAvailable(disk)) {
    server.send(404, "text/plain", "Disk not available");
    return;
  }

  String path = server.arg("f");
  if (!safePath(path)) {
    server.send(400, "text/plain", "Bad path");
    return;
  }

  String ext = fileExt(path);
  if (!isTextExt(ext)) {
    server.send(400, "text/plain", "Only text files can be edited");
    return;
  }

  String content = server.arg("content");
  if (content.length() > MAX_TEXT_EDIT_SIZE) {
    server.send(413, "text/plain", "Text too large");
    return;
  }

  File file = fsOpenGeneric(disk, path, FILE_WRITE);
  if (!file) {
    server.send(500, "text/plain", "Save open failed");
    return;
  }
  file.print(content);
  file.close();

  server.sendHeader("Location", "/view?disk=" + disk + "&f=" + urlEncode(path));
  server.send(303);
}

void handleUpdatePage() {
  serveWebPage("/www/update.html");
}


void handleFirmwareUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    Serial.println("Firmware update start: " + upload.filename);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("Firmware update OK: %u bytes\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.end();
    Serial.println("Firmware update aborted");
  }
}

void handleFirmwareDone() {
  if (Update.hasError()) {
    server.send(500, "text/plain", "Firmware update failed");
    return;
  }

  server.send(200, "text/plain; charset=utf-8", "Firmware nahrán. ESP32 se restartuje...");
  delay(800);
  ESP.restart();
}

// ============================================================
// Download / Delete / Upload / Format
// ============================================================

void handleDownload() {
  String disk = server.hasArg("disk") ? server.arg("disk") : "ffat";

  if (!server.hasArg("f")) {
    server.send(400, "text/plain", "Missing file");
    return;
  }

  if (!diskAvailable(disk)) {
    server.send(404, "text/plain", "Disk not available");
    return;
  }

  String path = server.arg("f");

  if (!safePath(path)) {
    server.send(400, "text/plain", "Bad path");
    return;
  }

  if (!fsExistsGeneric(disk, path)) {
    server.send(404, "text/plain", "Not found");
    return;
  }

  File file = fsOpenGeneric(disk, path, FILE_READ);

  if (!file) {
    server.send(500, "text/plain", "Open failed");
    return;
  }

  if (file.isDirectory()) {
    file.close();
    server.send(400, "text/plain", "Cannot download directory");
    return;
  }

  String fn = fileNameFromPath(path);

  server.sendHeader("Content-Disposition", "attachment; filename=\"" + fn + "\"");
  server.streamFile(file, "application/octet-stream");

  file.close();
}

void handleDelete() {
  String disk = server.hasArg("disk") ? server.arg("disk") : "ffat";
  String returnPath = server.hasArg("p") ? server.arg("p") : "/";
  returnPath = normalizeDirPath(returnPath);

  if (!server.hasArg("f")) {
    server.send(400, "text/plain", "Missing file");
    return;
  }

  if (!diskAvailable(disk)) {
    server.send(404, "text/plain", "Disk not available");
    return;
  }

  String path = server.arg("f");

  if (!safePath(path) || path == "/") {
    server.send(400, "text/plain", "Bad path");
    return;
  }

  if (fsExistsGeneric(disk, path)) {
    removeRecursiveGeneric(disk, path);
  }

  server.sendHeader("Location", "/files?disk=" + disk + "&p=" + urlEncode(returnPath));
  server.send(303);
}

void handleUploadPage() {
  String disk = server.hasArg("disk") ? server.arg("disk") : "ffat";
  String dirPath = server.hasArg("p") ? server.arg("p") : "/";
  dirPath = normalizeDirPath(dirPath);

  server.sendHeader("Location", "/files?disk=" + disk + "&p=" + urlEncode(dirPath));
  server.send(303);
}

void handleUploadData() {
  HTTPUpload& upload = server.upload();
  String disk = server.hasArg("disk") ? server.arg("disk") : "ffat";
  String dirPath = server.hasArg("p") ? server.arg("p") : "/";

  dirPath = normalizeDirPath(dirPath);

  if (!diskAvailable(disk)) {
    return;
  }

  if (upload.status == UPLOAD_FILE_START) {
    uploadActive = true;
    audioWasPlayingBeforeUpload = audioPlaying;

    if (audioPlaying) {
      Serial.println("Upload bezi paralelne s audiem");
      audioStatus = radioPlaying ? "Webradio hraje + upload" : "MP3 hraje + upload";
    }

    uploadDiskName = disk;
    uploadLastFlushMs = millis();

    String filename = upload.filename;
    String fullPath = joinPath(dirPath, filename);

    if (fullPath.length() == 0) {
      Serial.println("Bad upload filename");
      uploadActive = false;
      return;
    }

    if (fsExistsGeneric(disk, fullPath)) {
      fsRemoveGeneric(disk, fullPath);
    }

    uploadFile = fsOpenGeneric(disk, fullPath, FILE_WRITE);

    if (!uploadFile) {
      Serial.println("Upload open failed: " + disk + ":" + fullPath);
      uploadActive = false;
      return;
    }

    Serial.println("Upload start: " + disk + ":" + fullPath);
  }

  else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) {
      size_t written = uploadFile.write(upload.buf, upload.currentSize);

      if (written != upload.currentSize) {
        Serial.printf(
          "Upload write short: %u/%u\n",
          (unsigned)written,
          (unsigned)upload.currentSize
        );
      }

      if (millis() - uploadLastFlushMs > 1000) {
        uploadFile.flush();
        uploadLastFlushMs = millis();
      }

      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }

  else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.flush();
      uploadFile.close();
    }

    Serial.printf("Upload end: %u bytes\n", upload.totalSize);

    uploadDiskName = "";
    uploadLastFlushMs = 0;
    uploadActive = false;
    audioWasPlayingBeforeUpload = false;
  }

  else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) {
      uploadFile.close();
    }

    Serial.println("Upload aborted");

    uploadDiskName = "";
    uploadLastFlushMs = 0;
    uploadActive = false;
    audioWasPlayingBeforeUpload = false;
  }
}

void handleRename() {
  String disk = server.arg("disk");
  String returnPath = server.hasArg("p") ? server.arg("p") : "/";
  String oldPath = server.arg("f");
  String newName = server.arg("name");

  if (disk != "ffat" && disk != "usb0") disk = "ffat";
  returnPath = normalizeDirPath(returnPath);
  newName.trim();

  if (!diskAvailable(disk)) {
    server.send(404, "text/plain", "Disk not available");
    return;
  }

  if (!safePath(oldPath) || oldPath == "/" || !fsExistsGeneric(disk, oldPath)) {
    server.send(400, "text/plain", "Bad source path");
    return;
  }

  if (newName.length() == 0 || newName.indexOf('/') >= 0 || newName.indexOf('\\') >= 0 || newName.indexOf("..") >= 0) {
    server.send(400, "text/plain", "Bad new name");
    return;
  }

  String newPath = joinPath(parentPath(oldPath), newName);
  if (newPath.length() == 0 || fsExistsGeneric(disk, newPath)) {
    server.send(400, "text/plain", "Bad or existing target name");
    return;
  }

  if (!fsRenameGeneric(disk, oldPath, newPath)) {
    server.send(500, "text/plain", "Rename failed");
    return;
  }

  server.sendHeader("Location", "/files?disk=" + disk + "&p=" + urlEncode(returnPath));
  server.send(303);
}

void handleMkdir() {
  String disk = server.arg("disk");
  String dirPath = server.hasArg("p") ? server.arg("p") : "/";
  String name = server.arg("name");

  if (disk != "ffat" && disk != "usb0") disk = "ffat";
  dirPath = normalizeDirPath(dirPath);
  name.trim();

  if (!diskAvailable(disk)) {
    server.send(404, "text/plain", "Disk not available");
    return;
  }

  String fullPath = joinPath(dirPath, name);
  if (fullPath.length() == 0 || fsExistsGeneric(disk, fullPath)) {
    server.send(400, "text/plain", "Bad or existing folder name");
    return;
  }

  if (!fsMkdirGeneric(disk, fullPath)) {
    server.send(500, "text/plain", "Folder create failed");
    return;
  }

  server.sendHeader("Location", "/files?disk=" + disk + "&p=" + urlEncode(dirPath));
  server.send(303);
}

void handleCreateFile() {
  String disk = server.arg("disk");
  String dirPath = server.hasArg("p") ? server.arg("p") : "/";
  String name = server.arg("name");

  if (disk != "ffat" && disk != "usb0") disk = "ffat";
  dirPath = normalizeDirPath(dirPath);
  name.trim();

  if (!diskAvailable(disk)) {
    server.send(404, "text/plain", "Disk not available");
    return;
  }

  String fullPath = joinPath(dirPath, name);
  if (fullPath.length() == 0 || fsExistsGeneric(disk, fullPath)) {
    server.send(400, "text/plain", "Bad or existing file name");
    return;
  }

  File f = fsOpenGeneric(disk, fullPath, FILE_WRITE);
  if (!f) {
    server.send(500, "text/plain", "File create failed");
    return;
  }
  f.close();

  server.sendHeader("Location", "/edit?disk=" + disk + "&f=" + urlEncode(fullPath));
  server.send(303);
}

void handleFormat() {
  String disk = server.arg("disk");
  String confirm = server.arg("confirm");

  confirm.trim();

  if (confirm != "FORMAT") {
    server.send(400, "text/plain", "Pro potvrzeni napis FORMAT");
    return;
  }

  if (disk == "ffat") {
    if (uploadFile) {
      uploadFile.close();
    }

    setFfatFormatRequested(true);

    server.send(
      200,
      "text/plain; charset=utf-8",
      "Formátování FFat naplánováno. ESP32 se restartuje a při startu FFat smaže."
    );

    delay(1000);
    ESP.restart();
    return;
  }

  if (disk == "usb0") {
    server.send(501, "text/plain", "USB format neni v EspUsbHostMscFS dostupny. Naformatuj flashku v PC jako FAT32.");
    return;
  }

  server.send(400, "text/plain", "Bad disk");
}

void handleUsbRemount() {
  if (!checkWebAuth()) return;

  if (uploadFile) {
    uploadFile.close();
  }

  uploadActive = false;
  audioWasPlayingBeforeUpload = false;

  audioStatus = "USB remount zařazen";
  usbStatus = "USB remount čeká ve frontě";

  if (!queueAudioSimple(AUDIO_CMD_USB_REMOUNT)) {
    server.send(503, "text/plain", "Audio fronta je plná, USB remount neproveden");
    return;
  }

  server.sendHeader("Location", "/files?disk=usb0");
  server.send(303);
}


String radioLogoExtension(String filename) {
  filename.toLowerCase();
  int dot = filename.lastIndexOf('.');
  if (dot < 0) return "";
  String ext = filename.substring(dot + 1);
  if (ext == "jpeg") ext = "jpg";
  if (ext == "png" || ext == "jpg" || ext == "webp" || ext == "gif" || ext == "svg") {
    return ext;
  }
  return "";
}

void removeStationLogoFiles(uint8_t index, const String& keepPath = "") {
  const char* extensions[] = {"png", "jpg", "webp", "gif", "svg"};
  for (uint8_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
    String path = "/www/logos/station_" + String(index) + "." + extensions[i];
    if (path != keepPath && FFat.exists(path.c_str())) {
      FFat.remove(path.c_str());
    }
  }
}

void resetRadioLogoUploadState(bool removeTemp) {
  if (radioLogoUploadFile) radioLogoUploadFile.close();
  if (removeTemp && radioLogoUploadTempPath.length() > 0 && FFat.exists(radioLogoUploadTempPath.c_str())) {
    FFat.remove(radioLogoUploadTempPath.c_str());
  }
  radioLogoUploadIndex = -1;
  radioLogoUploadTempPath = "";
  radioLogoUploadTargetPath = "";
  radioLogoUploadBytes = 0;
}

void handleRadioLogoUploadData() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    resetRadioLogoUploadState(true);
    radioLogoUploadError = "";

    int index = server.hasArg("i") ? server.arg("i").toInt() : -1;
    String ext = radioLogoExtension(upload.filename);

    if (index < 0 || index >= MAX_RADIO_STATIONS) {
      radioLogoUploadError = "Neplatná stanice";
      return;
    }
    if (ext.length() == 0) {
      radioLogoUploadError = "Podporované logo: PNG, JPG, WEBP, GIF nebo SVG";
      return;
    }

    if (!FFat.exists("/www/logos")) FFat.mkdir("/www/logos");

    radioLogoUploadIndex = index;
    radioLogoUploadTempPath = "/www/logos/.station_" + String(index) + ".upload";
    radioLogoUploadTargetPath = "/www/logos/station_" + String(index) + "." + ext;
    if (FFat.exists(radioLogoUploadTempPath.c_str())) FFat.remove(radioLogoUploadTempPath.c_str());

    radioLogoUploadFile = FFat.open(radioLogoUploadTempPath.c_str(), FILE_WRITE);
    if (!radioLogoUploadFile) {
      radioLogoUploadError = "Logo nelze uložit do FFat";
      resetRadioLogoUploadState(true);
    }
  }
  else if (upload.status == UPLOAD_FILE_WRITE) {
    if (radioLogoUploadError.length() > 0 || !radioLogoUploadFile) return;

    radioLogoUploadBytes += upload.currentSize;
    if (radioLogoUploadBytes > MAX_RADIO_LOGO_SIZE) {
      radioLogoUploadError = "Logo je větší než 512 KB";
      resetRadioLogoUploadState(true);
      return;
    }

    size_t written = radioLogoUploadFile.write(upload.buf, upload.currentSize);
    if (written != upload.currentSize) {
      radioLogoUploadError = "Zápis loga do FFat selhal";
      resetRadioLogoUploadState(true);
    }
  }
  else if (upload.status == UPLOAD_FILE_END) {
    if (radioLogoUploadError.length() > 0 || radioLogoUploadIndex < 0 || !radioLogoUploadFile) {
      resetRadioLogoUploadState(true);
      return;
    }

    radioLogoUploadFile.flush();
    radioLogoUploadFile.close();

    int index = radioLogoUploadIndex;
    String tempPath = radioLogoUploadTempPath;
    String targetPath = radioLogoUploadTargetPath;

    removeStationLogoFiles((uint8_t)index, targetPath);
    if (FFat.exists(targetPath.c_str())) FFat.remove(targetPath.c_str());

    if (!FFat.rename(tempPath.c_str(), targetPath.c_str())) {
      radioLogoUploadError = "Přejmenování loga selhalo";
      resetRadioLogoUploadState(true);
      return;
    }

    cfg.radioLogo[index] = targetPath;
    if (!saveConfig()) {
      radioLogoUploadError = "Logo je uložené, ale konfiguraci se nepodařilo zapsat";
    }

    resetRadioLogoUploadState(false);
  }
  else if (upload.status == UPLOAD_FILE_ABORTED) {
    radioLogoUploadError = "Upload loga byl přerušen";
    resetRadioLogoUploadState(true);
  }
}

void handleRadioLogoUploadDone() {
  if (radioLogoUploadError.length() > 0) {
    String error = radioLogoUploadError;
    radioLogoUploadError = "";
    server.send(400, "text/plain; charset=utf-8", error);
    return;
  }
  server.send(200, "text/plain; charset=utf-8", "Logo stanice bylo uloženo");
}

void handleRadioLogoDelete() {
  if (!checkWebAuth()) return;
  int index = server.hasArg("i") ? server.arg("i").toInt() : -1;
  if (index < 0 || index >= MAX_RADIO_STATIONS) {
    server.send(400, "text/plain; charset=utf-8", "Neplatná stanice");
    return;
  }

  removeStationLogoFiles((uint8_t)index);
  cfg.radioLogo[index] = "";
  if (!saveConfig()) {
    server.send(500, "text/plain; charset=utf-8", "Logo bylo smazáno, ale konfiguraci se nepodařilo uložit");
    return;
  }
  server.send(200, "text/plain; charset=utf-8", "Logo stanice bylo smazáno");
}

void handleRadioPage() {
  serveWebPage("/www/radio.html");
}


void handleRadioSave() {
  if (!checkWebAuth()) return;

  for (uint8_t i = 0; i < MAX_RADIO_STATIONS; i++) {
    String name = server.arg("radio_name_" + String(i));
    String url = server.arg("radio_url_" + String(i));

    name.trim();
    url.trim();

    cfg.radioName[i] = name;
    cfg.radioUrl[i] = url;
  }

  if (cfg.radioName[0].length() == 0) {
    cfg.radioName[0] = "Moje radio";
  }

  saveConfig();

  server.sendHeader("Location", "/radio");
  server.send(303);
}

void handleRadioPlay() {
  if (!checkWebAuth()) return;

  int idx = server.hasArg("i") ? server.arg("i").toInt() : 0;
  if (idx < 0 || idx >= MAX_RADIO_STATIONS) idx = 0;

  if (cfg.radioUrl[idx].length() == 0) {
    server.send(400, "text/plain", "Radio URL neni nastavena");
    return;
  }

  String station = cfg.radioName[idx].length() ? cfg.radioName[idx] : cfg.radioUrl[idx];
  audioStatus = "Webradio: startuji " + station;
  radioStatus = audioStatus;

  if (!queueAudioRadioPlay(idx, cfg.radioUrl[idx], station, true)) {
    server.send(503, "text/plain", "Audio fronta je plná");
    return;
  }

  server.send(200, "text/plain", audioStatus);
}




// ============================================================
// /status.json + /games
// ============================================================

String audioModeText() {
  if (sendspinStreamActive) return "radio";
  if (radioPlaying) return "radio";
  if (playlistActive) return "složka";
  if (audioPlaying || audioPaused) return "soubor";
  if (audioReady) return "připraveno";
  return "nepřipraveno";
}

int currentRadioStationIndex() {
  if (radioUrlActive.length() > 0) {
    for (uint8_t i = 0; i < MAX_RADIO_STATIONS; i++) {
      if (cfg.radioUrl[i].length() > 0 && cfg.radioUrl[i] == radioUrlActive) return i;
    }
  }
  if (radioPlaying && radioResumeIndex >= 0 && radioResumeIndex < MAX_RADIO_STATIONS) {
    return radioResumeIndex;
  }
  return -1;
}

String currentAudioTitle() {
  if (sendspinStreamActive) {
    if (sendspinTitle.length() > 0) return sendspinTitle;
    if (sendspinArtist.length() > 0) return sendspinArtist;
    return "Síťový reproduktor";
  }

  if (radioPlaying) {
    int index = currentRadioStationIndex();
    if (index >= 0 && cfg.radioName[index].length() > 0) return cfg.radioName[index];
    return "Internetové rádio";
  }

  if ((audioPlaying || audioPaused) && audioPath.length() > 0) {
    return fileNameFromPath(audioPath);
  }

  return audioReady ? String("Přehrávač připraven") : String("Audio není připravené");
}

// ============================================================
// Volitelný OLED SH1106 128x64
// ============================================================

U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(
  U8G2_R0,
  U8X8_PIN_NONE
);

bool oledAvailable = false;
uint8_t oledI2cAddress = 0;
uint32_t oledLastRefreshMs = 0;
uint32_t oledLastScrollMs = 0;
size_t oledScrollOffset = 0;
String oledCachedTitleSource = "";
String oledCachedTitleAscii = "";

static const uint32_t OLED_REFRESH_INTERVAL_MS = 300;
static const uint32_t OLED_SCROLL_INTERVAL_MS = 550;
static const size_t OLED_LINE_CHARS = 20;
static const size_t OLED_WINDOW_CHARS = OLED_LINE_CHARS * 2;

bool oledI2cDeviceExists(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

String oledAsciiText(String value) {
  // Použitý malý OLED font je úsporný, ale nemá kompletní českou sadu.
  // Diakritiku proto pro displej převedeme na základní ASCII znaky.
  const char* from[] = {
    "á", "č", "ď", "é", "ě", "í", "ň", "ó", "ř", "š", "ť", "ú", "ů", "ý", "ž",
    "Á", "Č", "Ď", "É", "Ě", "Í", "Ň", "Ó", "Ř", "Š", "Ť", "Ú", "Ů", "Ý", "Ž"
  };
  const char* to[] = {
    "a", "c", "d", "e", "e", "i", "n", "o", "r", "s", "t", "u", "u", "y", "z",
    "A", "C", "D", "E", "E", "I", "N", "O", "R", "S", "T", "U", "U", "Y", "Z"
  };

  for (size_t i = 0; i < sizeof(from) / sizeof(from[0]); i++) {
    value.replace(from[i], to[i]);
  }

  value.replace("_", " ");
  value.replace("\\", "/");
  value.trim();

  String lower = value;
  lower.toLowerCase();
  if (lower.endsWith(".mp3") && value.length() >= 4) {
    value.remove(value.length() - 4);
  }

  return value;
}

const String& oledCurrentTitleAscii() {
  String source = currentAudioTitle();

  if (source != oledCachedTitleSource) {
    oledCachedTitleSource = source;
    oledCachedTitleAscii = oledAsciiText(source);
    if (oledCachedTitleAscii.length() == 0) {
      oledCachedTitleAscii = "ORIS radio";
    }
    oledScrollOffset = 0;
    oledLastScrollMs = millis();
  }

  return oledCachedTitleAscii;
}

String oledModeText() {
  if (audioPaused) return "PAUZA";
  if (sendspinStreamActive) return "SIT";
  if (radioPlaying) return "RADIO";
  if (playlistActive) return "MP3 SLOZKA";
  if (audioPlaying) return "MP3";
  if (audioReady) return "PRIPRAVENO";
  return "AUDIO CHYBA";
}

String oledWifiText() {
  if (WiFi.status() == WL_CONNECTED) {
    return "W " + String(WiFi.RSSI());
  }
  if (apRunning) return "AP";
  return "OFF";
}

void oledDrawPercentBar(int x, int y, int width, int height, int percent) {
  oled.drawFrame(x, y, width, height);

  if (percent < 0) return;
  if (percent > 100) percent = 100;

  const int innerWidth = width - 2;
  int fillWidth = (innerWidth * percent) / 100;
  if (fillWidth > 0) {
    oled.drawBox(x + 1, y + 1, fillWidth, height - 2);
  }
}

void oledSplitTitle(const String& title, String& line1, String& line2) {
  line1 = "";
  line2 = "";

  if (title.length() <= OLED_LINE_CHARS) {
    line1 = title;
    return;
  }

  if (title.length() <= OLED_WINDOW_CHARS) {
    int breakAt = title.lastIndexOf(' ', OLED_LINE_CHARS);
    if (breakAt < 8) breakAt = OLED_LINE_CHARS;

    line1 = title.substring(0, breakAt);
    line1.trim();

    size_t secondStart = (size_t)breakAt;
    while (secondStart < title.length() && title[secondStart] == ' ') secondStart++;
    line2 = title.substring(secondStart, secondStart + OLED_LINE_CHARS);
    line2.trim();
    return;
  }

  // Dlouhý název posouváme po znacích přes dvě řádky.
  const String scrolling = title + "   ";
  const size_t cycleLength = scrolling.length();
  String window;
  window.reserve(OLED_WINDOW_CHARS);

  for (size_t i = 0; i < OLED_WINDOW_CHARS; i++) {
    window += scrolling[(oledScrollOffset + i) % cycleLength];
  }

  line1 = window.substring(0, OLED_LINE_CHARS);
  line2 = window.substring(OLED_LINE_CHARS, OLED_WINDOW_CHARS);
}

void drawOledBootScreen() {
  if (!oledAvailable) return;

  oled.clearBuffer();
  oled.setFont(u8g2_font_6x13B_tf);
  oled.drawStr(20, 25, "ORIS RADIO");
  oled.setFont(u8g2_font_6x12_tf);
  oled.drawStr(31, 45, "startuji...");
  oled.sendBuffer();
}

void initOptionalOled() {
  oledAvailable = false;
  oledI2cAddress = 0;

  if (!Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN)) {
    Serial.println("OLED: I2C se nepodařilo spustit");
    return;
  }

  Wire.setClock(400000);
  delay(10);

  if (oledI2cDeviceExists(0x3C)) {
    oledI2cAddress = 0x3C;
  } else if (oledI2cDeviceExists(0x3D)) {
    oledI2cAddress = 0x3D;
  } else {
    Serial.println("OLED nenalezen, displejová obsluha vypnuta");
    Wire.end();
    return;
  }

  oled.setI2CAddress(oledI2cAddress << 1);
  oled.begin();
  oled.setPowerSave(0);
  oled.setContrast(160);
  oled.setFontMode(1);
  oledAvailable = true;

  Serial.printf(
    "OLED SH1106 ready: SDA=%d SCL=%d address=0x%02X\n",
    OLED_SDA_PIN,
    OLED_SCL_PIN,
    oledI2cAddress
  );

  drawOledBootScreen();
}

void serviceOled() {
  if (!oledAvailable) return;

  const uint32_t now = millis();
  if (now - oledLastRefreshMs < OLED_REFRESH_INTERVAL_MS) return;
  oledLastRefreshMs = now;

  const String& title = oledCurrentTitleAscii();
  if (title.length() > OLED_WINDOW_CHARS && now - oledLastScrollMs >= OLED_SCROLL_INTERVAL_MS) {
    oledLastScrollMs = now;
    const size_t cycleLength = title.length() + 3;
    oledScrollOffset = (oledScrollOffset + 1) % cycleLength;
  }

  String titleLine1;
  String titleLine2;
  oledSplitTitle(title, titleLine1, titleLine2);

  oled.clearBuffer();

  // Horní stavový řádek: režim přehrávání + Wi-Fi/AP.
  oled.setFont(u8g2_font_5x8_tf);
  String mode = oledModeText();
  String wifi = oledWifiText();
  oled.drawStr(0, 8, mode.c_str());
  int wifiWidth = oled.getStrWidth(wifi.c_str());
  oled.drawStr(128 - wifiWidth, 8, wifi.c_str());
  oled.drawHLine(0, 10, 128);

  // Název stanice nebo MP3 skladby.
  oled.setFont(u8g2_font_6x12_tf);
  oled.drawStr(0, 23, titleLine1.c_str());
  if (titleLine2.length() > 0) {
    oled.drawStr(0, 35, titleLine2.c_str());
  }

  // Hlasitost.
  oled.setFont(u8g2_font_5x8_tf);
  char volumeText[16];
  snprintf(volumeText, sizeof(volumeText), "VOL %3d%%", cfg.audioVolume);
  oled.drawStr(0, 47, volumeText);
  oledDrawPercentBar(46, 39, 82, 9, cfg.audioVolume);

  // Baterie. Bez zapojeného/aktivního měření zůstane BAT --.
  if (cfg.batteryEnabled && batteryMeasurementValid) {
    char batteryText[20];
    snprintf(
      batteryText,
      sizeof(batteryText),
      "BAT %3d%%",
      batteryPercent
    );
    oled.drawStr(0, 61, batteryText);
    oledDrawPercentBar(46, 53, 82, 9, batteryPercent);
  } else {
    oled.drawStr(0, 61, cfg.batteryEnabled ? "BAT  ?" : "BAT --");
    oledDrawPercentBar(46, 53, 82, 9, -1);
  }

  oled.sendBuffer();
}

uint32_t taskStackHighWater(TaskHandle_t handle) {
  return handle ? (uint32_t)uxTaskGetStackHighWaterMark(handle) : 0;
}

uint32_t taskHeartbeatAge(uint32_t heartbeat) {
  return heartbeat == 0 ? 0 : (uint32_t)(millis() - heartbeat);
}

void handleStatusJson() {
  bool psOk = psramFound();
  bool usbOk = usbDiskMounted();
  bool wifiOk = WiFi.status() == WL_CONNECTED;

  String json = "{";
  uint64_t heapTotal = ESP.getHeapSize();
  uint64_t heapFree = ESP.getFreeHeap();
  uint64_t heapUsed = heapTotal > heapFree ? (heapTotal - heapFree) : 0;

  uint64_t psramTotal = psOk ? ESP.getPsramSize() : 0;
  uint64_t psramFree = psOk ? ESP.getFreePsram() : 0;
  uint64_t psramUsed = psramTotal > psramFree ? (psramTotal - psramFree) : 0;

  json += "\"heapUsed\":\"" + jsonEscape(bytesHuman(heapUsed)) + "\",";
  json += "\"heapTotal\":\"" + jsonEscape(bytesHuman(heapTotal)) + "\",";
  json += "\"heapFree\":\"" + jsonEscape(bytesHuman(heapFree)) + "\",";
  json += "\"heapMaxBlock\":\"" + jsonEscape(bytesHuman(ESP.getMaxAllocHeap())) + "\",";
  json += "\"psramOk\":" + String(psOk ? "true" : "false") + ",";
  json += "\"psramUsed\":\"" + jsonEscape(psOk ? bytesHuman(psramUsed) : String("nenalezena")) + "\",";
  json += "\"psramTotal\":\"" + jsonEscape(psOk ? bytesHuman(psramTotal) : String("0 B")) + "\",";
  json += "\"psramFree\":\"" + jsonEscape(psOk ? bytesHuman(psramFree) : String("nenalezena")) + "\",";
  json += "\"psramMaxBlock\":\"" + jsonEscape(psOk ? bytesHuman(ESP.getMaxAllocPsram()) : String("0 B")) + "\",";
  json += "\"usbOk\":" + String(usbOk ? "true" : "false") + ",";
  json += "\"usb\":\"" + String(usbOk ? "připojeno" : "nepřipojeno") + "\",";
  json += "\"wifiConnected\":" + String(wifiOk ? "true" : "false") + ",";
  json += "\"wifiSsid\":\"" + jsonEscape(wifiOk ? WiFi.SSID() : String("")) + "\",";
  json += "\"wifiSavedCount\":" + String(cfg.wifiCount) + ",";
  json += "\"rssi\":" + String(wifiOk ? WiFi.RSSI() : 0) + ",";
  json += "\"audioPlaying\":" + String(audioPlaying ? "true" : "false") + ",";
  json += "\"audioPaused\":" + String(audioPaused ? "true" : "false") + ",";
  json += "\"positionMs\":" + String(currentAudioPositionMs()) + ",";
  json += "\"audio\":\"" + jsonEscape(audioModeText()) + "\",";
  json += "\"audioDetail\":\"" + jsonEscape(audioStatus) + "\",";
  json += "\"audioOutput\":\"" + jsonEscape(String(audioOutputKindName())) + "\",";
  int activeRadioIndex = currentRadioStationIndex();
  String activeArtwork = sendspinStreamActive
    ? sendspinArtworkUrl
    : (activeRadioIndex >= 0 ? cfg.radioLogo[activeRadioIndex] : String(""));
  json += "\"audioTitle\":\"" + jsonEscape(currentAudioTitle()) + "\",";
  json += "\"audioPath\":\"" + jsonEscape(audioPath) + "\",";
  json += "\"radioIndex\":" + String(activeRadioIndex) + ",";
  json += "\"radioLogo\":\"" + jsonEscape(activeArtwork) + "\",";
  json += "\"smartSpeakerEnabled\":" + String(cfg.smartSpeakerEnabled ? "true" : "false") + ",";
  json += "\"smartSpeakerConnected\":" + String(sendspinProtocolReady ? "true" : "false") + ",";
  json += "\"smartSpeakerActive\":" + String(sendspinStreamActive ? "true" : "false") + ",";
  json += "\"smartSpeakerStatus\":\"" + jsonEscape(sendspinStatus) + "\",";
  json += "\"smartSpeakerTitle\":\"" + jsonEscape(sendspinTitle) + "\",";
  json += "\"smartSpeakerArtist\":\"" + jsonEscape(sendspinArtist) + "\",";
  json += "\"smartSpeakerAlbum\":\"" + jsonEscape(sendspinAlbum) + "\",";
  json += "\"smartSpeakerArtwork\":\"" + jsonEscape(sendspinArtworkUrl) + "\",";
  json += "\"smartSpeakerDurationMs\":" + String(sendspinTrackDurationMs) + ",";
  json += "\"audioVolume\":" + String(cfg.audioVolume) + ",";
  json += "\"audioBassDb\":" + String(cfg.audioBassDb) + ",";
  json += "\"audioTrebleDb\":" + String(cfg.audioTrebleDb) + ",";
  json += "\"audioEqEnabled\":" + String(cfg.audioEqEnabled ? "true" : "false") + ",";
  json += "\"audioEqPreampDb\":" + String(cfg.audioEqPreampDb) + ",";
  json += "\"audioEqAutoHeadroom\":" + String(cfg.audioEqAutoHeadroom ? "true" : "false") + ",";
  json += "\"audioEqBands\":" + audioEqBandsJson() + ",";
  json += "\"audioOutputGainDb\":" + String(cfg.audioOutputGainDb) + ",";
  json += "\"audioVolumeCurve\":" + String(cfg.audioVolumeCurve, 2) + ",";
  json += "\"audioEffectiveGain\":" + String(currentAudioGain(), 5) + ",";
  json += "\"batteryEnabled\":" + String(cfg.batteryEnabled ? "true" : "false") + ",";
  json += "\"batteryValid\":" + String(batteryMeasurementValid ? "true" : "false") + ",";
  json += "\"batteryVoltage\":" + String(batteryMeasurementValid ? batteryVoltage : 0.0f, 3) + ",";
  json += "\"batteryPercent\":" + String(batteryMeasurementValid ? batteryPercent : -1) + ",";
  json += "\"batteryRawMv\":" + String(batteryRawMilliVolts) + ",";
  json += "\"batteryStatus\":\"" + jsonEscape(batteryStatusText()) + "\",";
  json += "\"taskAudio\":" + String(audioTaskHandle ? "true" : "false") + ",";
  json += "\"taskUsb\":" + String(usbTaskHandle ? "true" : "false") + ",";
  json += "\"taskNetwork\":" + String(networkTaskHandle ? "true" : "false") + ",";
  json += "\"taskWeb\":" + String(webTaskHandle ? "true" : "false") + ",";
  json += "\"taskAudioStackHwm\":" + String(taskStackHighWater(audioTaskHandle)) + ",";
  json += "\"taskUsbStackHwm\":" + String(taskStackHighWater(usbTaskHandle)) + ",";
  json += "\"taskNetworkStackHwm\":" + String(taskStackHighWater(networkTaskHandle)) + ",";
  json += "\"taskWebStackHwm\":" + String(taskStackHighWater(webTaskHandle)) + ",";
  json += "\"taskAudioAgeMs\":" + String(taskHeartbeatAge(audioTaskHeartbeatMs)) + ",";
  json += "\"taskUsbAgeMs\":" + String(taskHeartbeatAge(usbTaskHeartbeatMs)) + ",";
  json += "\"taskNetworkAgeMs\":" + String(taskHeartbeatAge(networkTaskHeartbeatMs)) + ",";
  json += "\"taskWebAgeMs\":" + String(taskHeartbeatAge(webTaskHeartbeatMs)) + ",";
  json += "\"uptime\":\"" + jsonEscape(uptimeHuman()) + "\"";
  json += "}";

  server.send(200, "application/json; charset=utf-8", json);
}

bool gameHasIndex(const String& disk, const String& dirPath) {
  String idx = dirPath;
  if (!idx.endsWith("/")) idx += "/";
  idx += "index.html";
  return diskAvailable(disk) && fsExistsGeneric(disk, idx);
}


void handleGamesPage() {
  serveWebPage("/www/games.html");
}


void handleGamesMkdir() {
  String disk = server.arg("disk");
  if (disk != "ffat" && disk != "usb0") disk = "ffat";

  if (!diskAvailable(disk)) {
    server.send(400, "text/plain", "Disk není dostupný");
    return;
  }

  if (!fsExistsGeneric(disk, "/games")) {
    fsMkdirGeneric(disk, "/games");
  }

  server.sendHeader("Location", "/games");
  server.send(303);
}

bool handleGameFsRequest() {
  String uri = server.uri();
  String disk;
  String prefix;

  if (uri.startsWith("/game/ffat")) {
    disk = "ffat";
    prefix = "/game/ffat";
  } else if (uri.startsWith("/game/usb0")) {
    disk = "usb0";
    prefix = "/game/usb0";
  } else {
    return false;
  }

  if (!checkWebAuth()) {
    return true;
  }

  if (!diskAvailable(disk)) {
    server.send(404, "text/plain", "Disk není dostupný");
    return true;
  }

  String path = uri.substring(prefix.length());
  if (path.length() == 0) path = "/games/";
  if (!safePath(path)) {
    server.send(400, "text/plain", "Neplatná cesta");
    return true;
  }

  File file = fsOpenGeneric(disk, path, FILE_READ);
  if (file && file.isDirectory()) {
    file.close();
    if (!uri.endsWith("/")) {
      server.sendHeader("Location", uri + "/");
      server.send(303);
      return true;
    }
    if (!path.endsWith("/")) path += "/";
    path += "index.html";
    file = fsOpenGeneric(disk, path, FILE_READ);
  }

  if (!file || file.isDirectory()) {
    if (file) file.close();
    server.send(404, "text/plain", "Game file not found");
    return true;
  }

  String ext = fileExt(path);
  server.streamFile(file, mimeForExt(ext));
  file.close();
  return true;
}



// ============================================================
// JSON API pro frontend uložený ve FFat
// ============================================================

void beginChunkedJson() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json; charset=utf-8", "");
}

void endChunkedResponse() {
  server.sendContent("");
}

void handleConfigJson() {
  if (!checkWebAuth()) return;

  bool wifiOk = WiFi.status() == WL_CONNECTED;
  bool usbOk = usbDiskMounted();

  beginChunkedJson();
  server.sendContent("{");
  server.sendContent("\"apSsid\":\"" + jsonEscape(cfg.apSsid) + "\",");
  server.sendContent("\"apPass\":\"" + jsonEscape(cfg.apPass) + "\",");
  server.sendContent("\"apMode\":" + String((int)cfg.apMode) + ",");
  server.sendContent("\"apRunning\":" + String(apRunning ? "true" : "false") + ",");
  server.sendContent("\"apIp\":\"" + String(apRunning ? WiFi.softAPIP().toString() : String("")) + "\",");
  server.sendContent("\"mdnsName\":\"" + jsonEscape(normalizeMdnsName(cfg.mdnsName)) + "\",");
  server.sendContent("\"mdnsStarted\":" + String(mdnsStarted ? "true" : "false") + ",");
  server.sendContent("\"smartSpeakerEnabled\":" + String(cfg.smartSpeakerEnabled ? "true" : "false") + ",");
  server.sendContent("\"smartSpeakerConnected\":" + String(sendspinProtocolReady ? "true" : "false") + ",");
  server.sendContent("\"smartSpeakerStatus\":\"" + jsonEscape(sendspinStatus) + "\",");
  server.sendContent("\"webUser\":\"" + jsonEscape(cfg.webUser) + "\",");
  server.sendContent("\"webPass\":\"" + jsonEscape(cfg.webPass) + "\",");
  server.sendContent("\"ftpEnabled\":" + String(cfg.ftpEnabled ? "true" : "false") + ",");
  server.sendContent("\"ftpUser\":\"" + jsonEscape(cfg.ftpUser) + "\",");
  server.sendContent("\"ftpPass\":\"" + jsonEscape(cfg.ftpPass) + "\",");
  server.sendContent("\"ftpDisk\":\"" + jsonEscape(cfg.ftpDisk) + "\",");
  server.sendContent("\"rgbEnabled\":" + String(cfg.rgbEnabled ? "true" : "false") + ",");
  server.sendContent("\"audioVolume\":" + String(cfg.audioVolume) + ",");
  server.sendContent("\"audioBassDb\":" + String(cfg.audioBassDb) + ",");
  server.sendContent("\"audioTrebleDb\":" + String(cfg.audioTrebleDb) + ",");
  server.sendContent("\"audioEqEnabled\":" + String(cfg.audioEqEnabled ? "true" : "false") + ",");
  server.sendContent("\"audioEqPreampDb\":" + String(cfg.audioEqPreampDb) + ",");
  server.sendContent("\"audioEqAutoHeadroom\":" + String(cfg.audioEqAutoHeadroom ? "true" : "false") + ",");
  server.sendContent("\"audioEqBands\":" + audioEqBandsJson() + ",");
  server.sendContent("\"audioOutputGainDb\":" + String(cfg.audioOutputGainDb) + ",");
  server.sendContent("\"audioVolumeCurve\":" + String(cfg.audioVolumeCurve, 2) + ",");
  server.sendContent("\"audioEffectiveGain\":" + String(currentAudioGain(), 5) + ",");
  server.sendContent("\"batteryEnabled\":" + String(cfg.batteryEnabled ? "true" : "false") + ",");
  server.sendContent("\"batteryDividerRatio\":" + String(cfg.batteryDividerRatio, 4) + ",");
  server.sendContent("\"batteryCalibration\":" + String(cfg.batteryCalibration, 4) + ",");
  server.sendContent("\"batteryPin\":" + String(BATTERY_ADC_PIN) + ",");
  server.sendContent("\"batteryValid\":" + String(batteryMeasurementValid ? "true" : "false") + ",");
  server.sendContent("\"batteryVoltage\":" + String(batteryMeasurementValid ? batteryVoltage : 0.0f, 3) + ",");
  server.sendContent("\"batteryPercent\":" + String(batteryMeasurementValid ? batteryPercent : -1) + ",");
  server.sendContent("\"batteryRawMv\":" + String(batteryRawMilliVolts) + ",");
  server.sendContent("\"batteryStatus\":\"" + jsonEscape(batteryStatusText()) + "\",");
  server.sendContent("\"audioReady\":" + String(audioReady ? "true" : "false") + ",");
  server.sendContent("\"audioPlaying\":" + String(audioPlaying ? "true" : "false") + ",");
  server.sendContent("\"audioStatus\":\"" + jsonEscape(audioStatus) + "\",");
  server.sendContent("\"audioOutput\":\"" + jsonEscape(String(audioOutputKindName())) + "\",");
  server.sendContent("\"ffatTotal\":" + String((unsigned long)FFat.totalBytes()) + ",");
  server.sendContent("\"ffatUsed\":" + String((unsigned long)FFat.usedBytes()) + ",");
  server.sendContent("\"usbMounted\":" + String(usbOk ? "true" : "false") + ",");
  server.sendContent("\"usbStatus\":\"" + jsonEscape(usbStatus) + "\",");
  server.sendContent("\"wifiConnected\":" + String(wifiOk ? "true" : "false") + ",");
  server.sendContent("\"wifiSsid\":\"" + jsonEscape(wifiOk ? WiFi.SSID() : String("")) + "\",");
  server.sendContent("\"wifiIp\":\"" + String(wifiOk ? WiFi.localIP().toString() : String("")) + "\",");
  server.sendContent("\"wifiRssi\":" + String(wifiOk ? WiFi.RSSI() : 0) + ",");
  server.sendContent("\"wifiStatus\":\"" + jsonEscape(wifiManagerStatus) + "\",");

  server.sendContent("\"savedWifi\":[");
  for (uint8_t i = 0; i < cfg.wifiCount; i++) {
    if (i) server.sendContent(",");
    bool connected = wifiOk && WiFi.SSID() == cfg.wifiSsid[i];
    server.sendContent("{\"index\":" + String(i) +
                       ",\"ssid\":\"" + jsonEscape(cfg.wifiSsid[i]) + "\"" +
                       ",\"connected\":" + String(connected ? "true" : "false") + "}");
  }
  server.sendContent("],");

  server.sendContent("\"radioStations\":[");
  for (uint8_t i = 0; i < MAX_RADIO_STATIONS; i++) {
    if (i) server.sendContent(",");
    server.sendContent("{\"index\":" + String(i) +
                       ",\"name\":\"" + jsonEscape(cfg.radioName[i]) + "\"" +
                       ",\"url\":\"" + jsonEscape(cfg.radioUrl[i]) + "\"" +
                       ",\"logo\":\"" + jsonEscape(cfg.radioLogo[i]) + "\"}");
  }
  server.sendContent("]}");
  endChunkedResponse();
}

void handleFilesJson() {
  if (!checkWebAuth()) return;

  String disk = server.hasArg("disk") ? server.arg("disk") : "ffat";
  if (disk != "ffat" && disk != "usb0") {
    server.send(400, "application/json; charset=utf-8", "{\"error\":\"Neplatný disk\"}");
    return;
  }

  String dirPath = server.hasArg("p") ? server.arg("p") : "/";
  dirPath = normalizeDirPath(dirPath);
  bool available = diskAvailable(disk);

  if (!available) {
    server.send(200, "application/json; charset=utf-8",
      "{\"disk\":\"" + jsonEscape(disk) + "\",\"title\":\"" + jsonEscape(diskTitle(disk)) +
      "\",\"path\":\"" + jsonEscape(dirPath) + "\",\"available\":false,\"items\":[]}");
    return;
  }

  File root = fsOpenGeneric(disk, dirPath, FILE_READ);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    server.send(404, "application/json; charset=utf-8", "{\"error\":\"Složku nelze otevřít\"}");
    return;
  }

  currentDisk = disk;
  beginChunkedJson();
  server.sendContent("{\"disk\":\"" + jsonEscape(disk) +
                     "\",\"title\":\"" + jsonEscape(diskTitle(disk)) +
                     "\",\"path\":\"" + jsonEscape(dirPath) +
                     "\",\"parent\":\"" + jsonEscape(parentPath(dirPath)) +
                     "\",\"available\":true,\"usbMounted\":" + String(usbDiskMounted() ? "true" : "false") +
                     ",\"items\":[");

  bool first = true;
  File file = root.openNextFile();
  while (file) {
    bool isDir = file.isDirectory();
    String rawName = file.name();
    size_t size = file.size();
    file.close();

    String name = displayNameForEntry(dirPath, rawName);
    String fullPath = fullPathForEntry(dirPath, rawName);
    if (name.length() > 0 && fullPath.length() > 0) {
      if (!first) server.sendContent(",");
      first = false;
      server.sendContent("{\"name\":\"" + jsonEscape(name) +
                         "\",\"path\":\"" + jsonEscape(fullPath) +
                         "\",\"dir\":" + String(isDir ? "true" : "false") +
                         ",\"size\":" + String((unsigned long)size) +
                         ",\"ext\":\"" + jsonEscape(fileExt(name)) + "\"}");
    }
    file = root.openNextFile();
  }
  root.close();
  server.sendContent("]}");
  endChunkedResponse();
}

void appendGamesJsonDisk(const String& disk, bool firstDisk) {
  if (!firstDisk) server.sendContent(",");
  bool available = diskAvailable(disk);
  bool hasDir = available && fsExistsGeneric(disk, "/games");

  server.sendContent("{\"disk\":\"" + jsonEscape(disk) +
                     "\",\"title\":\"" + jsonEscape(diskTitle(disk)) +
                     "\",\"available\":" + String(available ? "true" : "false") +
                     ",\"hasGamesDir\":" + String(hasDir ? "true" : "false") +
                     ",\"items\":[");

  bool first = true;
  if (hasDir) {
    File root = fsOpenGeneric(disk, "/games", FILE_READ);
    if (root && root.isDirectory()) {
      File file = root.openNextFile();
      while (file) {
        String rawName = file.name();
        String name = displayNameForEntry("/games", rawName);
        String full = fullPathForEntry("/games", rawName);
        String lower = name;
        lower.toLowerCase();
        bool playable = false;
        String launchPath = full;
        String typeText;

        if (file.isDirectory()) {
          if (gameHasIndex(disk, full)) {
            playable = true;
            if (!launchPath.endsWith("/")) launchPath += "/";
            typeText = "HTML5 složka / index.html";
          }
        } else if (lower.endsWith(".html") || lower.endsWith(".htm")) {
          playable = true;
          typeText = "single-file HTML";
        }
        file.close();

        if (playable) {
          if (!first) server.sendContent(",");
          first = false;
          String url = "/game/" + disk + urlEncode(launchPath);
          server.sendContent("{\"name\":\"" + jsonEscape(name) +
                             "\",\"path\":\"" + jsonEscape(full) +
                             "\",\"type\":\"" + jsonEscape(typeText) +
                             "\",\"url\":\"" + jsonEscape(url) + "\"}");
        }
        file = root.openNextFile();
      }
      root.close();
    } else if (root) {
      root.close();
    }
  }

  server.sendContent("]}");
}

void handleGamesJson() {
  if (!checkWebAuth()) return;
  beginChunkedJson();
  server.sendContent("{\"disks\":[");
  appendGamesJsonDisk("ffat", true);
  appendGamesJsonDisk("usb0", false);
  server.sendContent("]}");
  endChunkedResponse();
}

// ============================================================
// Webové soubory uložené ve FFat: /www
// ============================================================

void ensureWebRootDirectory() {
  if (!FFat.exists("/www")) {
    FFat.mkdir("/www");
  }
}

bool webUiCoreAvailable() {
  const char* required[] = {
    "/www/style.css", "/www/app.js", "/www/index.html", "/www/home.js",
    "/www/icons/music.svg", "/www/icons/radio.svg",
    "/www/files.html", "/www/files.js",
    "/www/config.html", "/www/config.js",
    "/www/radio.html", "/www/radio.js",
    "/www/games.html", "/www/games.js",
    "/www/update.html", "/www/view.html", "/www/view.js",
    "/www/edit.html", "/www/edit.js"
  };

  for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
    if (!FFat.exists(required[i])) return false;
  }
  return true;
}


bool handleFfatWebAssetRequest() {
  String uri = server.uri();
  if (!uri.startsWith("/www/")) {
    return false;
  }

  if (!checkWebAuth()) {
    return true;
  }

  String path = uri;
  if (!safePath(path) || !FFat.exists(path.c_str())) {
    server.send(404, "text/plain; charset=utf-8", "Webový soubor nenalezen");
    return true;
  }

  File file = FFat.open(path.c_str(), FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    server.send(404, "text/plain; charset=utf-8", "Webový soubor nelze otevřít");
    return true;
  }

  // Bez cache se změna provedená přes FTP projeví ihned po obnovení stránky.
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.streamFile(file, mimeForExt(fileExt(path)));
  file.close();
  return true;
}

void handleWebRescuePage() {
  if (!checkWebAuth()) return;

  ensureWebRootDirectory();

  String html = F(
    "<!doctype html><html lang='cs'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Obnova webu</title><style>"
    "body{font-family:Arial;background:#101215;color:#eee;margin:0;padding:18px;max-width:760px}"
    ".card{background:#1b1f26;border:1px solid #343a44;border-radius:12px;padding:16px;margin:12px 0}"
    "input,button{width:100%;padding:10px;margin:6px 0;box-sizing:border-box;border-radius:8px}"
    "button{background:#2f80ed;color:#fff;border:0;font-weight:bold}a{color:#79b8ff}code{color:#ffd166}"
    "</style></head><body><h2>Nouzové nahrání webu do FFat</h2>"
    "<div class='card'><p>Firmware a API fungují, ale ve <code>/www</code> chybí kompletní frontend.</p>"
    "<p>Nahraj do <code>/www</code> všechny soubory ze složky <code>data/www</code> v projektu.</p>"
    "<form method='POST' action='/upload?disk=ffat&p=/www' enctype='multipart/form-data'>"
    "<input type='file' name='file' required><button type='submit'>Nahrát jeden soubor do /www</button></form></div>"
    "<div class='card'><a href='/files?disk=ffat&p=/www'>Otevřít správce /www</a></div>"
    "</body></html>"
  );

  server.send(200, "text/html; charset=utf-8", html);
}


// ============================================================
// Routes
// ============================================================

void setupRoutes() {
  server.on("/", HTTP_GET, []() {
    if (!webUiCoreAvailable()) {
      server.sendHeader("Location", "/rescue");
      server.send(303);
      return;
    }
    serveWebPage("/www/index.html");
  });

  server.on("/rescue", HTTP_GET, handleWebRescuePage);

  server.on("/files", HTTP_GET, handleFilesPage);
  server.on("/config", HTTP_GET, handleConfigPage);
  server.on("/radio", HTTP_GET, handleRadioPage);
  server.on("/games", HTTP_GET, handleGamesPage);
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/view", HTTP_GET, handleViewFile);
  server.on("/edit", HTTP_GET, handleEditFile);

  server.on("/api/config.json", HTTP_GET, handleConfigJson);
  server.on("/api/files.json", HTTP_GET, handleFilesJson);
  server.on("/api/games.json", HTTP_GET, handleGamesJson);
  server.on("/status.json", HTTP_GET, []() { if (checkWebAuth()) handleStatusJson(); });

  server.on("/radio/save", HTTP_POST, handleRadioSave);
  server.on("/radio/play", HTTP_POST, handleRadioPlay);
  server.on("/radio/logo/delete", HTTP_POST, handleRadioLogoDelete);
  server.on(
    "/radio/logo",
    HTTP_POST,
    []() { if (checkWebAuth()) handleRadioLogoUploadDone(); },
    []() { if (checkWebAuth()) handleRadioLogoUploadData(); }
  );
  server.on("/games/mkdir", HTTP_POST, []() { if (checkWebAuth()) handleGamesMkdir(); });
  server.on("/config/save", HTTP_POST, []() { if (checkWebAuth()) handleConfigSave(); });
  server.on("/smart-speaker/save", HTTP_POST, []() { if (checkWebAuth()) handleSmartSpeakerSave(); });
  server.on("/wifi/scan/start", HTTP_POST, handleWifiScanStart);
  server.on("/wifi/scan.json", HTTP_GET, handleWifiScanJson);
  server.on("/wifi/add", HTTP_POST, handleWifiAdd);
  server.on("/wifi/connect", HTTP_POST, handleWifiConnect);
  server.on("/wifi/delete", HTTP_POST, handleWifiDelete);
  server.on("/reboot", HTTP_POST, []() { if (checkWebAuth()) handleReboot(); });

  server.on("/raw", HTTP_GET, []() { if (checkWebAuth()) handleRawFile(); });
  server.on("/save", HTTP_POST, []() { if (checkWebAuth()) handleSaveFile(); });
  server.on("/download", HTTP_GET, []() { if (checkWebAuth()) handleDownload(); });
  server.on("/delete", HTTP_GET, []() { if (checkWebAuth()) handleDelete(); });
  server.on("/rename", HTTP_POST, []() { if (checkWebAuth()) handleRename(); });
  server.on("/mkdir", HTTP_POST, []() { if (checkWebAuth()) handleMkdir(); });
  server.on("/create", HTTP_POST, []() { if (checkWebAuth()) handleCreateFile(); });
  server.on("/format", HTTP_POST, []() { if (checkWebAuth()) handleFormat(); });
  server.on("/usb/remount", HTTP_POST, []() { if (checkWebAuth()) handleUsbRemount(); });

  server.on("/audio/play", HTTP_GET, handleAudioPlay);
  server.on("/audio/play_folder", HTTP_GET, handleAudioPlayFolder);
  server.on("/audio/next", HTTP_POST, handleAudioNext);
  server.on("/audio/prev", HTTP_POST, handleAudioPrev);
  server.on("/audio/shuffle", HTTP_POST, handleAudioShuffle);
  server.on("/audio/repeat", HTTP_POST, handleAudioRepeat);
  server.on("/audio/stop", HTTP_POST, handleAudioStop);
  server.on("/audio/stop_ajax", HTTP_POST, handleAudioStopAjax);
  server.on("/audio/toggle_pause", HTTP_POST, handleAudioTogglePause);
  server.on("/audio/volume", HTTP_GET, handleAudioVolume);
  server.on("/audio/eq.json", HTTP_GET, handleAudioEqJson);
  server.on("/audio/eq/save", HTTP_POST, handleAudioEqSave);

  server.on(
    "/upload",
    HTTP_POST,
    []() { if (checkWebAuth()) handleUploadPage(); },
    []() { if (checkWebAuth()) handleUploadData(); }
  );

  server.on(
    "/firmware",
    HTTP_POST,
    []() { if (checkWebAuth()) handleFirmwareDone(); },
    []() { if (checkWebAuth()) handleFirmwareUpload(); }
  );

  server.onNotFound([]() {
    if (handleGameFsRequest()) return;
    if (handleFfatWebAssetRequest()) return;
    server.send(404, "text/plain; charset=utf-8", "Not found");
  });
}


// ============================================================
// Setup / loop
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  prefsMutex = xSemaphoreCreateMutex();
  sendspinTxMutex = xSemaphoreCreateMutex();
  if (!prefsMutex) Serial.println("Preferences mutex create FAILED");
  if (!sendspinTxMutex) Serial.println("Sendspin TX mutex create FAILED");

  Serial.println();
  Serial.println("ESP32-S3 Web Disk");

  formatFfatOnBootIfRequested();

  Serial.println("Mounting FFat...");
  if (!FFat.begin(true)) {
    Serial.println("FFat mount/format failed!");
    return;
  }
  Serial.println("FFat mounted.");
  ensureWebRootDirectory();

  Serial.printf("FFat total: %u bytes\n", FFat.totalBytes());
  Serial.printf("FFat used:  %u bytes\n", FFat.usedBytes());
  Serial.printf("PSRAM found: %s\n", psramFound() ? "YES" : "NO");
  Serial.printf("PSRAM size: %u bytes\n", ESP.getPsramSize());
  Serial.printf("Free PSRAM: %u bytes\n", ESP.getFreePsram());
  Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
  randomSeed((uint32_t)micros());

  loadConfig();
  loadAudioVolumeState();
  loadRadioResumeState();
  initBatteryMonitor();
  initOptionalOled();

  if (!FFat.exists(CONFIG_FILE)) {
    saveConfig();
  }

  // Při bootu nejdřív nastavíme správný režim a AP pouze vytvoříme.
  // Nesmí se zde volat nucené odpojování ještě neexistujícího AP.
  WiFi.mode(cfg.apMode == AP_MODE_OFF ? WIFI_STA : WIFI_AP_STA);
  WiFi.setSleep(false);
  applyApPolicy(false);
  connectStaIfConfigured();
  startMdns(true);

  Serial.print("AP režim: ");
  Serial.println((int)cfg.apMode);
  Serial.print("AP stav: ");
  Serial.println(apRunning ? "zapnuto" : "vypnuto");

  initI2sAudioOutput();
  initEncoderControl();

  enableUsbPower();

  initUsbHost();
  usbBootMountPending = true;
  usbBootMountAfterMs = 0;
  ftpStartServerIfNeeded();

  setupRoutes();

  server.begin();
  Serial.println("Web server started");

  startBackgroundTasks();
}


void serviceAudioPump() {
  if (sendspinStreamActive) {
    return;
  }

  if (!audioPlaying) {
    return;
  }

  if (radioPlaying) {
    if (!radioClient.connected() && radioClient.available() == 0 && audio.isEof()) {
      stopAudioPlayback("Radio dokončeno");
      sendspinSetExternalSource(false);
      radioStatus = "Radio dokončeno";
      return;
    }

    // Rádio pumpujeme adaptivně podle zaplnění PCM bufferu.
    // Čím menší zásoba framů, tím víc pumpnutí dekodéru v jednom průchodu.
    size_t af = audio.availableFrames();

    uint8_t pumps = 0;

if (af < 1024) {
  pumps = 64;
} else if (af < 2048) {
  pumps = 40;
} else if (af < 4096) {
  pumps = 24;
} else if (af < 8192) {
  pumps = 12;
} else if (af < 24576) {
  pumps = 6;
} else if (af < RADIO_DECODER_START_FRAMES) {
  pumps = 2;
}

for (uint8_t i = 0; i < pumps; i++) {
  if (audioCommandPending()) break;

  serviceI2sAudioOutput();

  audio.pump();
  audioPumpCount++;

  taskYIELD();
}

    uint32_t now = millis();
    if (now - lastAudioDebugMs > 2000) {
      lastAudioDebugMs = now;
      Serial.printf(
        "RADIO DBG: conn=%u avail=%d eof=%u ready=%u af=%u target=%u err=%s pumps=%u cb=%u frames=%u underruns=%u level=%u freeHeap=%u freePsram=%u\n",
        radioClient.connected() ? 1 : 0,
        radioClient.available(),
        audio.isEof() ? 1 : 0,
        audio.isReady() ? 1 : 0,
        (unsigned)audio.availableFrames(),
        (unsigned)RADIO_DECODER_START_FRAMES,
        pcmFlowErrorName(audio.lastError()),
        (unsigned)audioPumpCount,
        (unsigned)audioCbCount,
        (unsigned)audioCbFrames,
        (unsigned)audioCbUnderruns,
        (unsigned)audioLevel,
        (unsigned)ESP.getFreeHeap(),
        (unsigned)ESP.getFreePsram()
      );
    }

    return;
  }

  if (audio.isEof()) {
    if (playlistActive) {
      playNextPlaylistTrack();
    } else {
      stopAudioPlayback("Přehrávání dokončeno");
      sendspinSetExternalSource(false);
    }
  } else {
    // Lokální MP3 se čte průběžně z FFat/USB souboru.
    // Když je PCM buffer nízko, přidej víc pumpnutí, aby se I2S audio nevyhladovělo.
    size_t af = audio.availableFrames();

    uint8_t pumps = 2;
    if (af < 512) {
      pumps = 32;
    } else if (af < 1024) {
      pumps = 20;
    } else if (af < 2048) {
      pumps = 12;
    } else if (af < 4096) {
      pumps = 6;
    }

    for (uint8_t i = 0; i < pumps; i++) {
      if (audioCommandPending()) break;

      audio.pump();
      audioPumpCount++;

      // EQ, dekodér a I2S běží ve stejném tasku. Průběžné krmení I2S
      // zabrání vyhladovění DMA při delší sérii pumpnutí dekodéru.
      if ((i & 3U) == 3U) {
        serviceI2sAudioOutput();
      }

      taskYIELD();
    }

    uint32_t now = millis();
    if (now - lastAudioDebugMs > 3000) {
      lastAudioDebugMs = now;
      Serial.printf(
        "MP3 DBG: ready=%u eof=%u af=%u err=%s pumps=%u cb=%u frames=%u underruns=%u level=%u freeHeap=%u freePsram=%u\n",
        audio.isReady() ? 1 : 0,
        audio.isEof() ? 1 : 0,
        (unsigned)audio.availableFrames(),
        pcmFlowErrorName(audio.lastError()),
        (unsigned)audioPumpCount,
        (unsigned)audioCbCount,
        (unsigned)audioCbFrames,
        (unsigned)audioCbUnderruns,
        (unsigned)audioLevel,
        (unsigned)ESP.getFreeHeap(),
        (unsigned)ESP.getFreePsram()
      );
    }
  }
}

void audioServiceTask(void *param) {
  (void)param;

  for (;;) {
    audioTaskHeartbeatMs = millis();
    serviceAudioGainApply();
    serviceAudioCommandQueue();
    serviceAudioPump();
    serviceI2sAudioOutput();
    updateMusicRgbLed();

    bool audioActive =
      (audioPlaying && !audioPaused) ||
      (sendspinStreamActive && sendspinGroupPlaying);

    if (audioActive) {
      vTaskDelay(1);
    } else {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
}

void usbServiceTask(void *param) {
  (void)param;

  for (;;) {
    usbTaskHeartbeatMs = millis();
    serviceUsbBootMountOnce();

    if (usbRemountRequested) {
      usbRemountRequested = false;
      usbStatus = "USB remount vyžádán";
      usbMassStorage.end();
      pollUsbMount();
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void serviceNetworkAndHousekeeping() {
  // Síťové a pomalejší periodické služby neběží v Arduino loop tasku.
  // Krátké blokace Wi-Fi, mDNS, NVS, ADC nebo OLED tak nevynechají enkodér.
  serviceApPolicy();
  serviceSavedWifiManager();
  serviceMdns();
  serviceSendspinCommandQueue();
  serviceSendspin();
  serviceSendspinCommandQueue();
  serviceRadioResume();
  serviceAudioVolumeSave();
  serviceBatteryMonitor();
  serviceOled();
}

void networkServiceTask(void *param) {
  (void)param;

  for (;;) {
    networkTaskHeartbeatMs = millis();
    serviceNetworkAndHousekeeping();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void webServiceTask(void *param) {
  (void)param;

  for (;;) {
    webTaskHeartbeatMs = millis();
    // HTTP a FTP zůstávají úmyslně v jednom management tasku.
    // Oba sahají na FFat/USB; jejich serializace omezuje souběžné zápisy,
    // ale dlouhý upload/download už neblokuje enkodér, Sendspin ani audio.
    server.handleClient();
    ftpHandle();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void startBackgroundTasks() {
  if (!audioCommandQueue) {
    audioCommandQueue = xQueueCreate(AUDIO_COMMAND_QUEUE_LEN, sizeof(AudioCommand));
  }
  if (!sendspinCommandQueue) {
    sendspinCommandQueue = xQueueCreate(
      SENDSPIN_COMMAND_QUEUE_LEN,
      sizeof(SendspinOutboundCommand)
    );
  }

  if (!audioCommandQueue) {
    Serial.println("Audio command queue create FAILED");
    audioStatus = "Audio frontu se nepodařilo vytvořit";
    return;
  }

  if (!sendspinCommandQueue) {
    Serial.println("Sendspin command queue create FAILED; použije se přímý fallback");
  }

  if (!audioTaskHandle) {
    BaseType_t ok = xTaskCreatePinnedToCore(
      audioServiceTask,
      "audioSvc",
      AUDIO_TASK_STACK,
      nullptr,
      3,
      &audioTaskHandle,
      APP_CPU_NUM
    );

    if (ok != pdPASS) {
      Serial.println("Audio task create FAILED");
      audioTaskHandle = nullptr;
      audioStatus = "Audio task se nepodařilo spustit";
    }
  }

  if (!usbTaskHandle) {
    BaseType_t ok = xTaskCreatePinnedToCore(
      usbServiceTask,
      "usbSvc",
      USB_TASK_STACK,
      nullptr,
      1,
      &usbTaskHandle,
      PRO_CPU_NUM
    );

    if (ok != pdPASS) {
      Serial.println("USB task create FAILED");
      usbTaskHandle = nullptr;
      usbStatus = "USB task se nepodařilo spustit";
    }
  }

  if (!networkTaskHandle) {
    BaseType_t ok = xTaskCreatePinnedToCore(
      networkServiceTask,
      "networkSvc",
      NETWORK_TASK_STACK,
      nullptr,
      2,
      &networkTaskHandle,
      PRO_CPU_NUM
    );

    if (ok != pdPASS) {
      Serial.println("Network task create FAILED; fallback poběží v loop()");
      networkTaskHandle = nullptr;
    }
  }

  if (!webTaskHandle) {
    BaseType_t ok = xTaskCreatePinnedToCore(
      webServiceTask,
      "webSvc",
      WEB_TASK_STACK,
      nullptr,
      1,
      &webTaskHandle,
      PRO_CPU_NUM
    );

    if (ok != pdPASS) {
      Serial.println("Web task create FAILED; fallback poběží v loop()");
      webTaskHandle = nullptr;
    }
  }

  Serial.printf(
    "Tasks: audio=%s usb=%s network=%s web=%s freeHeap=%u freePsram=%u\n",
    audioTaskHandle ? "OK" : "FAIL",
    usbTaskHandle ? "OK" : "FAIL",
    networkTaskHandle ? "OK" : "FAIL",
    webTaskHandle ? "OK" : "FAIL",
    (unsigned)ESP.getFreeHeap(),
    (unsigned)ESP.getFreePsram()
  );
}

void loop() {
  // Jediná časově citlivá obsluha v Arduino loop tasku je enkodér.
  // Všechny potenciálně dlouhé operace běží mimo něj.
  serviceEncoderControl();

  // Bezpečný fallback: když kvůli nedostatku RAM některý task nevznikne,
  // funkce zařízení zůstane zachovaná, pouze už nebude plně oddělená.
  if (!webTaskHandle) {
    server.handleClient();
    ftpHandle();
  }

  if (!networkTaskHandle) {
    serviceNetworkAndHousekeeping();
  }

  if (!usbTaskHandle) {
    serviceUsbBootMountOnce();
  }

  delay(1);
}
