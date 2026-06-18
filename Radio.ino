// v.2.15

#include <WiFi.h>
#include <WebServer.h>
#include <FFat.h>
#include <FS.h>
#include <Preferences.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <PCMFlow.h>
#include "EspUsbHost.h"
#include "driver/i2s.h"
#include "esp_err.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

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
#define ENCODER_VOLUME_STEP 2


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

QueueHandle_t audioCommandQueue = nullptr;
TaskHandle_t audioTaskHandle = nullptr;
TaskHandle_t usbTaskHandle = nullptr;
volatile bool usbRemountRequested = false;

static const uint8_t AUDIO_COMMAND_QUEUE_LEN = 8;


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
        if (total > 0 && millis() - lastByte > 20) {
          break;
        }

        if (total == 0 && millis() - start > 900) {
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

String currentDisk = "ffat";
String uploadDiskName = "";

bool playlistActive = false;
bool playlistShuffle = false;
bool playlistRepeat = false;
String playlistDisk = "";
String playlistDir = "/";
String playlistLastPath = "";

// ============================================================
// Karaoke režim: audio hraje ESP, text kreslí web podle JSONu
// ============================================================
bool karaokeActive = false;
String karaokeDisk = "";
String karaokeJsonPath = "";
String karaokeAudioPath = "";
String karaokeTitle = "";

uint32_t audioStartedMs = 0;
uint32_t audioPausedAtMs = 0;
uint32_t audioPausedAccumMs = 0;

void clearKaraokeState() {
  karaokeActive = false;
  karaokeDisk = "";
  karaokeJsonPath = "";
  karaokeAudioPath = "";
  karaokeTitle = "";
}

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

uint32_t currentAudioPositionMs() {
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

static const uint8_t MAX_RADIO_STATIONS = 6;

// Ladění web rádia.
// Když rádio občas škubne, zvedej hlavně RADIO_PCM_BUFFER_FRAMES
// a RADIO_DECODER_START_FRAMES. Když dojde interní heap, vrať je níž.
static const size_t RADIO_PREBUFFER_TARGET_BYTES = 65536;      // kolik MP3 dat přednačíst z HTTP
static const size_t RADIO_PREBUFFER_MAX_BYTES    = 131072;     // max PSRAM prebuffer pro start dekodéru
static const size_t RADIO_PCM_BUFFER_FRAMES      = 24576;      // PCMFlow ring buffer
static const size_t RADIO_DECODER_START_FRAMES   = 8192;       // kolik PCM framů mít před spuštěním I2S audio
static const uint32_t RADIO_PREBUFFER_TIMEOUT_MS = 10000;      // čekání na naplnění PCM bufferu

// Lokální MP3 z flashky/FFat už nečteme celé do PSRAM.
// Dekódují se průběžně přes FileByteStream, takže půjdou i dlouhé skladby.
static const size_t FILE_PCM_BUFFER_FRAMES       = 8192;       // menší než rádio, disk je stabilnější než Wi-Fi
static const size_t FILE_DECODER_START_FRAMES    = 2048;       // zásoba před spuštěním USB callbacku
static const uint32_t FILE_PREBUFFER_TIMEOUT_MS  = 5000;


struct AppCfg {
  String apSsid;
  String apPass;
  String staSsid;
  String staPass;
  String mdnsName;
  String webUser;
  String webPass;
  bool ftpEnabled;
  String ftpUser;
  String ftpPass;
  String ftpDisk;
  bool rgbEnabled;
  int audioVolume;
  String radioName[MAX_RADIO_STATIONS];
  String radioUrl[MAX_RADIO_STATIONS];
};

AppCfg cfg;

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

bool isFfatFormatRequested() {
  prefs.begin("webdisk", false);
  bool requested = prefs.getBool("fmt_ffat", false);
  prefs.end();
  return requested;
}

void setFfatFormatRequested(bool value) {
  prefs.begin("webdisk", false);
  prefs.putBool("fmt_ffat", value);
  prefs.end();
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
void clearKaraokeState();
uint32_t currentAudioPositionMs();
void handleKaraokePage();
void handleKaraokeListJson();
void handleKaraokePlay();
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

void startMdns(bool forceRestart = false) {
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

  Serial.println("mDNS: http://" + host + ".local/");
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

  prefs.begin("radio", false);
  prefs.putBool("resume", wanted);
  prefs.putInt("idx", idx);
  prefs.putString("url", url);
  prefs.end();

  radioResumeSavedWanted = wanted;
  radioResumeSavedIndex = idx;
  radioResumeSavedUrl = url;

  radioResumeWanted = wanted;
  radioResumeIndex = idx;
  radioResumeUrl = url;
}

void loadRadioResumeState() {
  prefs.begin("radio", true);
  radioResumeWanted = prefs.getBool("resume", false);
  radioResumeIndex = prefs.getInt("idx", 0);
  radioResumeUrl = prefs.getString("url", "");
  prefs.end();

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
  cfg.staSsid = "";
  cfg.staPass = "";
  cfg.mdnsName = "oris-radio";
  cfg.webUser = "admin";
  cfg.webPass = "admin";
  cfg.ftpEnabled = true;
  cfg.ftpUser = "ftp";
  cfg.ftpPass = "12345678";
  cfg.ftpDisk = "usb0";
  cfg.rgbEnabled = false;
  cfg.audioVolume = 80;
  for (uint8_t i = 0; i < MAX_RADIO_STATIONS; i++) {
    cfg.radioName[i] = "";
    cfg.radioUrl[i] = "";
  }
  cfg.radioName[0] = "Moje radio";
  cfg.radioUrl[0] = "";
}

void loadConfig() {
  setDefaultCfg();

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
    else if (key == "sta_ssid") cfg.staSsid = val;
    else if (key == "sta_pass") cfg.staPass = val;
    else if (key == "mdns_name") cfg.mdnsName = val;
    else if (key == "web_user") cfg.webUser = val;
    else if (key == "web_pass") cfg.webPass = val;
    else if (key == "ftp_enabled") cfg.ftpEnabled = (val == "1" || val == "true" || val == "ON");
    else if (key == "ftp_user") cfg.ftpUser = val;
    else if (key == "ftp_pass") cfg.ftpPass = val;
    else if (key == "ftp_disk") cfg.ftpDisk = val;
    else if (key == "rgb_enabled") cfg.rgbEnabled = (val == "1" || val == "true" || val == "ON");
    else if (key == "audio_volume") cfg.audioVolume = val.toInt();
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
  }

  f.close();

  if (cfg.apSsid.length() == 0) {
    cfg.apSsid = DEFAULT_AP_SSID;
  }

  if (cfg.apPass.length() > 0 && cfg.apPass.length() < 8) {
    cfg.apPass = DEFAULT_AP_PASS;
  }

  cfg.mdnsName = normalizeMdnsName(cfg.mdnsName);
  if (cfg.webUser.length() == 0) cfg.webUser = "admin";
  if (cfg.webPass.length() == 0) cfg.webPass = "admin";
  if (cfg.ftpUser.length() == 0) cfg.ftpUser = "ftp";
  if (cfg.ftpPass.length() == 0) cfg.ftpPass = "12345678";
  if (cfg.ftpDisk != "ffat" && cfg.ftpDisk != "usb0") cfg.ftpDisk = "usb0";
  if (cfg.audioVolume < 0) cfg.audioVolume = 0;
  if (cfg.audioVolume > 100) cfg.audioVolume = 100;
  for (uint8_t i = 0; i < MAX_RADIO_STATIONS; i++) {
    cfg.radioName[i].trim();
    cfg.radioUrl[i].trim();
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
  f.println("sta_ssid=" + cfg.staSsid);
  f.println("sta_pass=" + cfg.staPass);
  f.println("mdns_name=" + normalizeMdnsName(cfg.mdnsName));
  f.println("web_user=" + cfg.webUser);
  f.println("web_pass=" + cfg.webPass);
  f.println("ftp_enabled=" + String(cfg.ftpEnabled ? "1" : "0"));
  f.println("ftp_user=" + cfg.ftpUser);
  f.println("ftp_pass=" + cfg.ftpPass);
  f.println("ftp_disk=" + cfg.ftpDisk);
  f.println("rgb_enabled=" + String(cfg.rgbEnabled ? "1" : "0"));
  f.println("audio_volume=" + String(cfg.audioVolume));
  // Starší názvy nechávám pro kompatibilitu
  f.println("radio_name=" + cfg.radioName[0]);
  f.println("radio_url=" + cfg.radioUrl[0]);
  for (uint8_t i = 0; i < MAX_RADIO_STATIONS; i++) {
    f.println("radio_name_" + String(i) + "=" + cfg.radioName[i]);
    f.println("radio_url_" + String(i) + "=" + cfg.radioUrl[i]);
  }

  f.close();
  return true;
}

void clampAudioVolumeCfg() {
  if (cfg.audioVolume < 0) cfg.audioVolume = 0;
  if (cfg.audioVolume > 100) cfg.audioVolume = 100;
}

void loadAudioVolumeState() {
  prefs.begin("audio", true);
  int saved = prefs.getInt("volume", -1);
  prefs.end();

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

  prefs.begin("audio", false);
  prefs.putInt("volume", cfg.audioVolume);
  prefs.end();

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

void startAp() {
  WiFi.softAPdisconnect(true);
  delay(100);

  if (cfg.apPass.length() >= 8) {
    WiFi.softAP(cfg.apSsid.c_str(), cfg.apPass.c_str());
  } else {
    WiFi.softAP(cfg.apSsid.c_str());
  }
}

void connectStaIfConfigured() {
  if (cfg.staSsid.length() == 0) {
    Serial.println("STA WiFi not configured.");
    return;
  }

  Serial.println("Connecting STA to: " + cfg.staSsid);
  WiFi.begin(cfg.staSsid.c_str(), cfg.staPass.c_str());
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
  if (audioPlaying) {
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

      if (audioPlaying) {
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
    if (!activeOutputIsUsb() || !audioPlaying || audioPaused) {
      request.writtenFrames = 0;
      audioCbCount++;
      audioCbUnderruns++;
      return;
    }

    request.writtenFrames = audio.readFrames(request.data, request.frameCount);

    audioCbCount++;
    audioCbFrames += request.writtenFrames;
    if (request.writtenFrames == 0) {
      audioCbUnderruns++;
    }

    if (request.writtenFrames > 0 && audioOutputFormat.bitsPerSample == 16) {
      int16_t *samples = (int16_t *)request.data;
      size_t sampleCount = (size_t)request.writtenFrames * (size_t)audioOutputFormat.channels;
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

  if (v < 0) v = 0;
  if (v > 100) v = 100;

  return (float)v / 100.0f;
}

void applyAudioVolume() {
  audio.setGain(currentAudioGain());
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
  i2sCfg.dma_buf_count = 8;
  i2sCfg.dma_buf_len = 256;
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
  const size_t bytesPerSample = audioOutputFormat.bitsPerSample / 8;
  size_t bytesToWrite = readFrames * channels * bytesPerSample;
  size_t bytesWritten = 0;

  esp_err_t err = i2s_write(
    I2S_AUDIO_PORT,
    i2sOutBuffer,
    bytesToWrite,
    &bytesWritten,
    pdMS_TO_TICKS(10)
  );

  audioCbCount++;
  audioCbFrames += bytesWritten / (channels * bytesPerSample);

  if (err != ESP_OK || bytesWritten == 0) {
    audioCbUnderruns++;
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

  while (millis() - preStart < FILE_PREBUFFER_TIMEOUT_MS) {
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

  while (millis() - preStart < RADIO_PREBUFFER_TIMEOUT_MS) {
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
      clearKaraokeState();
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
      clearKaraokeState();

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
      if (!playlistActive) {
        audioStatus = "Playlist není aktivní";
      } else if (!playNextPlaylistTrack()) {
        Serial.println("Playlist next failed: " + audioStatus);
      }
      break;

    case AUDIO_CMD_PREV:
      if (!playlistActive) {
        audioStatus = "Playlist není aktivní";
      } else if (!playPrevPlaylistTrack()) {
        audioStatus = "Předchozí skladba není dostupná";
        Serial.println(audioStatus);
      }
      break;

    case AUDIO_CMD_STOP:
      playlistActive = false;
      clearKaraokeState();
      stopAudioPlayback("Zastaveno");
      saveRadioResumeState(false, -1, "");
      break;

    case AUDIO_CMD_TOGGLE_PAUSE:
      toggleAudioPauseInternal();
      break;

    case AUDIO_CMD_USB_REMOUNT:
      playlistActive = false;
      clearKaraokeState();
      stopAudioPlayback("Audio zastaveno kvůli USB remountu");
      usbRemountRequested = true;
      break;

    default:
      break;
  }
}


void toggleAudioPauseInternal() {
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
  applyAudioVolume();
  requestAudioVolumeSave();
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

  if (karaokeActive) {
    audioStatus = "Karaoke: další skladbu vyber na webu";
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

  if (karaokeActive) {
    if (!queueFirstConfiguredRadioStation()) {
      audioStatus = "Není nastavené žádné rádio";
    }
    return;
  }

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


String usbInfoHtml() {
  String h;

  h += "<div class='box'><b>USB disk</b><br>";

  if (!usbHostStarted) {
    h += "<span class='bad'>USB host neběží</span><br>";
    h += htmlEscape(usbStatus);
  } else if (usbMassStorage.mounted()) {
    h += "<span class='good'>Připojeno</span><br>";
    h += "Mount: /usb<br>";

    uint64_t total = 0;
    uint64_t used = 0;
    uint64_t freeBytes = 0;

    if (getUsbDiskSpace(total, used, freeBytes)) {
      if (total > 0) {
        h += "Celkem: " + bytesHuman(total) + "<br>";
        h += "Použito: " + bytesHuman(used) + "<br>";
        h += "Volno: " + bytesHuman(freeBytes) + "<br>";
      } else {
        h += "Celkem: nezjištěno<br>";
        h += "Obsazeno soubory: " + bytesHuman(used) + "<br>";
      }
    } else {
      h += "Kapacita: nezjištěna<br>";
    }

    h += "<span class='small'>Použij FAT32 flashku. ExFAT/NTFS nečekej.</span>";
  } else {
    h += "<span class='warn'>Nepřipojeno</span><br>";
    h += htmlEscape(usbStatus);
  }

  h += "</div>";

  return h;
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
  clearKaraokeState();
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
  clearKaraokeState();
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
  applyAudioVolume();
  requestAudioVolumeSave();

  server.send(200, "text/plain", String(cfg.audioVolume));
}

void handleAudioStopAjax() {
  if (!checkWebAuth()) return;

  playlistActive = false;
  clearKaraokeState();
  audioStatus = "Zastavuji...";
  saveRadioResumeState(false, -1, "");

  if (!queueAudioSimple(AUDIO_CMD_STOP)) {
    server.send(503, "text/plain", "Audio fronta je plná");
    return;
  }

  server.send(200, "text/plain", "Zastavuji...");
}

String audioSidePanelHtml() {
  String h;

  h += "<div class='sidebar-section'>";
  h += "<h3>Audio</h3>";

  h += "<div class='small'>Stav</div>";
  h += "<div id='audioMiniStatus' style='margin-bottom:8px'>";
  if (audioPlaying) {
    h += htmlEscape(audioStatus);
  } else {
    h += "Zastaveno";
  }
  h += "</div>";

  h += "<label>Hlasitost: <span id='audioVolumeLabel' data-audio-volume-label>";
  h += String(cfg.audioVolume);
  h += " %</span></label>";

  h += "<input type='range' min='0' max='100' value='";
  h += String(cfg.audioVolume);
  h += "' data-audio-volume-range oninput='setAudioVolume(this.value)'>";

  h += "<div style='display:grid;grid-template-columns:1fr 1fr;gap:6px;margin-top:6px'>";
  h += "<button class='secondary' type='button' onclick=\"return audioAction('/audio/prev')\">⏮ Předchozí</button>";
  h += "<button class='secondary' type='button' onclick=\"return audioAction('/audio/next')\">Další ⏭</button>";
  h += "<button class='secondary' type='button' onclick=\"return audioAction('/audio/shuffle')\">Shuffle: ";
  h += playlistShuffle ? "ON" : "OFF";
  h += "</button>";
  h += "<button class='secondary' type='button' onclick=\"return audioAction('/audio/repeat')\">Repeat: ";
  h += playlistRepeat ? "ON" : "OFF";
  h += "</button>";
  h += "</div>";

  h += "<button class='secondary' type='button' onclick='return stopAudioAjax()'>Stop audio</button>";

  if (radioPlaying) {
    h += "<div class='small warn'>Hraje rádio</div>";
  } else if (playlistActive) {
    h += "<div class='small good'>Hraje složka</div>";
    h += "<div class='small'>";
    h += playlistShuffle ? "Shuffle ON" : "Shuffle OFF";
    h += " / ";
    h += playlistRepeat ? "Repeat ON" : "Repeat OFF";
    h += "</div>";
  } else if (audioPlaying) {
    h += "<div class='small good'>Hraje soubor</div>";
  } else if (audioReady) {
    h += "<div class='small good'>Audio výstup: ";
    h += audioOutputKindName();
    h += " připraveno</div>";
  } else {
    h += "<div class='small warn'>Audio výstup nepřipraveno</div>";
  }

  h += "</div>";
  return h;
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
// Společné HTML
// ============================================================

String pageHeader(const String& title) {
  String html = R"rawliteral(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>)rawliteral";

  html += htmlEscape(title);

  html += R"rawliteral(</title>
<style>
:root{color-scheme:dark;--bg:#101215;--panel:#181b20;--panel2:#20242b;--line:#303640;--text:#e7e7e7;--muted:#9aa3ad;--accent:#2f80ed;--danger:#b83232;--ok:#7ee787;--warn:#ffd166}
*{box-sizing:border-box}
body{font-family:Arial,Helvetica,sans-serif;background:var(--bg);color:var(--text);margin:0;padding-bottom:32px;font-size:14px}
header{background:#15181d;border-bottom:1px solid var(--line);position:sticky;top:0;z-index:10;box-shadow:0 2px 10px rgba(0,0,0,.22)}
nav{display:flex;gap:6px;align-items:center;min-height:46px;padding:7px 10px;flex-wrap:wrap}
nav a{color:#fff;background:#242a33;border:1px solid #323a45;padding:8px 11px;border-radius:8px;text-decoration:none;font-weight:bold}
nav a:hover{background:var(--accent)}
main{padding:10px;max-width:none;margin:0 auto}
h2{margin:0 0 10px 0;font-size:20px}
h3{margin:0 0 8px 0;font-size:15px;color:#fff}
a{color:#79b8ff;text-decoration:none}
a:hover{text-decoration:underline}
.card,.sidebar,.filepane{background:var(--panel);border:1px solid var(--line);border-radius:10px;box-shadow:0 1px 0 rgba(255,255,255,.03) inset}
.card{padding:12px;margin-bottom:10px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:10px}
.box{background:var(--panel2);padding:10px;border-radius:8px;word-break:break-word;border:1px solid #2d333d}
.game-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:10px}
.game-card{background:var(--panel2);border:1px solid #2d333d;border-radius:10px;padding:12px;display:flex;flex-direction:column;gap:8px;min-height:112px}
.game-card .title{font-weight:bold;font-size:15px;color:#fff;word-break:break-word}
.game-card .meta{color:var(--muted);font-size:12px;word-break:break-word}
.game-card a.play{display:inline-block;text-align:center;background:var(--accent);color:#fff;padding:8px 10px;border-radius:7px;text-decoration:none;font-weight:bold;margin-top:auto}
input,button,select{box-sizing:border-box;width:100%;padding:9px;margin:4px 0;border-radius:7px;border:1px solid #343b45;background:#252a32;color:#eee}
button{background:var(--accent);color:white;font-weight:bold;cursor:pointer;border:0}
button.danger{background:var(--danger)}
button.secondary{background:#444c57}
label{display:block;color:var(--muted);font-size:12px;margin-top:5px}
textarea{width:100%;min-height:65vh;box-sizing:border-box;background:#0f0f0f;color:#eee;border:1px solid #333;border-radius:10px;padding:12px;font-family:Consolas,monospace;font-size:14px}
pre.viewer{white-space:pre-wrap;word-break:break-word;background:#0f0f0f;border:1px solid #333;border-radius:10px;padding:12px;max-height:75vh;overflow:auto}
img.preview{max-width:100%;height:auto;border-radius:10px;background:#000}
video.preview,audio.preview{width:100%;max-height:75vh}
.explorer{display:grid;grid-template-columns:280px minmax(0,1fr);gap:10px;height:calc(100vh - 88px);min-height:520px}
.sidebar{padding:10px;overflow:auto}
.filepane{display:flex;flex-direction:column;overflow:hidden}
.pathbar{display:flex;gap:8px;align-items:center;justify-content:space-between;padding:10px;border-bottom:1px solid var(--line);background:#161a20}
.pathbar .path{font-family:Consolas,monospace;color:#dbeafe;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.toolbar{display:flex;gap:6px;flex-wrap:wrap;padding:8px 10px;border-bottom:1px solid var(--line);background:#14171c}
.toolbar form{display:flex;gap:6px;align-items:center;margin:0;flex-wrap:wrap}
.toolbar input,.toolbar select,.toolbar button{width:auto;margin:0;min-width:130px}
.toolbar input[type=file]{max-width:240px}
.filelist{overflow:auto;flex:1}
table{border-collapse:collapse;width:100%;font-size:13px}
th{position:sticky;top:0;background:#20242b;color:#c9d1d9;z-index:1}
td,th{border-bottom:1px solid #2b313a;padding:8px;text-align:left;vertical-align:middle}
tr:hover td{background:#171b22}
.actions{white-space:nowrap}
.actions a{display:inline-block;margin:2px 4px 2px 0;background:#242a33;border:1px solid #343b45;color:#dbeafe;padding:4px 7px;border-radius:6px;font-size:12px;text-decoration:none}
.actions a:hover{background:#2f80ed;color:white;text-decoration:none}
.sidebar-section{border-bottom:1px solid #2b313a;padding-bottom:10px;margin-bottom:10px}
.sidebar-section:last-child{border-bottom:0;margin-bottom:0}
.small{color:var(--muted);font-size:12px}
.good{color:var(--ok)}.bad{color:#ff7b72}.warn{color:var(--warn)}
.statusbar{position:fixed;left:0;right:0;bottom:0;height:28px;background:#15181d;border-top:1px solid var(--line);display:flex;align-items:center;gap:10px;padding:0 10px;font-size:12px;color:#c9d1d9;z-index:20;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.statusbar span{display:inline-block}.statusbar .ok{color:var(--ok)}.statusbar .warn{color:var(--warn)}.statusbar .bad{color:#ff7b72}
.upload-modal{
  position:fixed;
  inset:0;
  background:rgba(0,0,0,.72);
  display:none;
  align-items:center;
  justify-content:center;
  z-index:9999;
}
.upload-modal.show{
  display:flex;
}
.upload-box{
  width:min(420px,92vw);
  background:#181b20;
  border:1px solid #303640;
  border-radius:14px;
  padding:18px;
  box-shadow:0 10px 40px rgba(0,0,0,.45);
}
.upload-title{
  font-size:18px;
  font-weight:bold;
  margin-bottom:8px;
}
.upload-file{
  color:#9aa3ad;
  font-size:13px;
  word-break:break-word;
  margin-bottom:12px;
}
.upload-bar{
  width:100%;
  height:18px;
  background:#0f1115;
  border-radius:10px;
  overflow:hidden;
  border:1px solid #303640;
}
.upload-fill{
  height:100%;
  width:0%;
  background:#2f80ed;
  transition:width .15s linear;
}
.upload-percent{
  margin-top:10px;
  font-family:Consolas,monospace;
  color:#dbeafe;
}
.upload-error{
  color:#ff7b72;
  margin-top:10px;
  display:none;
}
@media(max-width:800px){.explorer{grid-template-columns:1fr;height:auto}.sidebar{order:2}.filepane{min-height:420px}.toolbar input,.toolbar select,.toolbar button{width:100%}.toolbar form{width:100%}}
@media(max-width:650px){main{padding:8px}table,thead,tbody,tr,td,th{display:block}th{display:none}td{border-bottom:0;padding:5px}tr{border-bottom:1px solid #333;padding:8px 0}.actions a{display:block;padding:5px 0;margin:4px 0;text-align:center}.statusbar{font-size:11px}}
</style>

<script>
function playAudioNoReload(url) {
  fetch(url, { credentials: 'same-origin' })
    .then(function(r) {
      return r.text().then(function(t) {
        if (!r.ok) {
          alert(t || 'Přehrávání selhalo');
          return;
        }
        updateAudioLabels('▶ ' + t);
      });
    })
    .catch(function(e) { alert('Chyba: ' + e); });
  return false;
}
function stopAudioNoReload(url) {
  fetch(url, { method: 'POST', credentials: 'same-origin' })
    .then(function(r) {
      return r.text().then(function(t) {
        if (!r.ok) {
          alert(t || 'Stop selhal');
          return;
        }
        updateAudioLabels(t || 'Zastaveno');
      });
    })
    .catch(function(e) { alert('Chyba: ' + e); });
  return false;
}


function audioAction(url) {
  fetch(url, { method: 'POST', credentials: 'same-origin' })
    .then(function(r) {
      return r.text().then(function(t) {
        if (!r.ok) {
          alert(t || 'Akce selhala');
          return;
        }
        updateAudioUi(t, true);
      });
    })
    .catch(function(e) { alert('Chyba: ' + e); });
  return false;
}

function playRadioNoReload(idx) {
  fetch('/radio/play?i=' + encodeURIComponent(idx || 0), { method: 'POST', credentials: 'same-origin' })
    .then(function(r) {
      return r.text().then(function(t) {
        if (!r.ok) {
          alert(t || 'Radio play selhal');
          return;
        }
        updateAudioUi(t, true);
      });
    })
    .catch(function(e) { alert('Chyba: ' + e); });
  return false;
}

let volumeTimer = null;

function updateAudioLabels(text) {
  var el = document.getElementById('audio-status-inline');
  if (el) el.textContent = text;

  var mini = document.getElementById('audioMiniStatus');
  if (mini) mini.textContent = text.replace(/^▶\s*/, '');
}

function updateAudioUi(text, playing) {
  updateAudioLabels((playing ? '▶ ' : '') + text);
  var bottom = document.getElementById('bottomAudioStatus');
  if (bottom) {
    bottom.textContent = playing ? ('Audio: ' + text) : 'Audio: zastaveno';
    bottom.className = playing ? 'ok' : 'warn';
  }
}

function updateAudioVolumeDisplay(v) {
  v = parseInt(v, 10);
  if (isNaN(v)) return;
  if (v < 0) v = 0;
  if (v > 100) v = 100;

  document.querySelectorAll('[data-audio-volume-label]').forEach(function(label) {
    label.textContent = v + ' %';
  });

  document.querySelectorAll('[data-audio-volume-range]').forEach(function(range) {
    if (document.activeElement !== range) {
      range.value = v;
    }
  });
}

function setAudioVolume(v) {
  updateAudioVolumeDisplay(v);

  if (volumeTimer) clearTimeout(volumeTimer);

  volumeTimer = setTimeout(function() {
    fetch('/audio/volume?v=' + encodeURIComponent(v), { credentials: 'same-origin' })
      .then(function(r) { return r.text(); })
      .then(function(t) {
        volumeTimer = null;
        updateAudioVolumeDisplay(t);
      })
      .catch(function() { volumeTimer = null; });
  }, 120);
}

function stopAudioAjax() {
  fetch('/audio/stop_ajax', { method: 'POST', credentials: 'same-origin' })
    .then(function(r) { return r.text(); })
    .then(function(t) { updateAudioLabels(t || 'Zastaveno'); })
    .catch(function() { alert('Stop selhal'); });
  return false;
}

function renameEntry(disk, encodedPath, encodedReturnPath, encodedOldName) {
  const oldName = decodeURIComponent(encodedOldName || '');
  const newName = prompt('Nový název:', oldName);
  if (newName === null) return false;
  const trimmed = newName.trim();
  if (!trimmed || trimmed === oldName) return false;
  if (trimmed.indexOf('/') >= 0 || trimmed.indexOf('\\') >= 0 || trimmed.indexOf('..') >= 0) {
    alert('Název nesmí obsahovat /, \\ ani ..');
    return false;
  }

  const form = document.createElement('form');
  form.method = 'POST';
  form.action = '/rename';
  form.style.display = 'none';

  const fields = {
    disk: disk,
    f: decodeURIComponent(encodedPath || ''),
    p: decodeURIComponent(encodedReturnPath || '/'),
    name: trimmed
  };

  Object.keys(fields).forEach(function(k) {
    const input = document.createElement('input');
    input.type = 'hidden';
    input.name = k;
    input.value = fields[k];
    form.appendChild(input);
  });

  document.body.appendChild(form);
  form.submit();
  return false;
}
</script>

<script>
function ensureUploadModal(){
  let m = document.getElementById('uploadModal');
  if (m) return m;

  m = document.createElement('div');
  m.id = 'uploadModal';
  m.className = 'upload-modal';
  m.innerHTML = `
    <div class="upload-box">
      <div class="upload-title">Nahrávání souboru</div>
      <div id="uploadFileName" class="upload-file"></div>
      <div class="upload-bar"><div id="uploadFill" class="upload-fill"></div></div>
      <div id="uploadPercent" class="upload-percent">0 %</div>
      <div id="uploadError" class="upload-error"></div>
    </div>
  `;
  document.body.appendChild(m);
  return m;
}

function uploadWithModal(form){
  const fileInput = form.querySelector('input[type="file"]');
  if (!fileInput || !fileInput.files || fileInput.files.length === 0) {
    alert('Vyber soubor k nahrání.');
    return false;
  }

  const modal = ensureUploadModal();
  const fill = document.getElementById('uploadFill');
  const pct = document.getElementById('uploadPercent');
  const err = document.getElementById('uploadError');
  const name = document.getElementById('uploadFileName');

  fill.style.width = '0%';
  pct.textContent = '0 %';
  err.style.display = 'none';
  err.textContent = '';
  name.textContent = fileInput.files[0].name + ' (' + niceBytes(fileInput.files[0].size) + ')';
  modal.classList.add('show');

  const xhr = new XMLHttpRequest();
  const data = new FormData(form);

  xhr.upload.onprogress = function(e){
    if (e.lengthComputable) {
      const p = Math.round((e.loaded / e.total) * 100);
      fill.style.width = p + '%';
      pct.textContent = p + ' %  —  ' + niceBytes(e.loaded) + ' / ' + niceBytes(e.total);
    } else {
      pct.textContent = niceBytes(e.loaded);
    }
  };

  xhr.onload = function(){
    if (xhr.status >= 200 && xhr.status < 400) {
      fill.style.width = '100%';
      pct.textContent = 'Hotovo, obnovuji...';
      setTimeout(function(){
        window.location.reload();
      }, 600);
    } else {
      err.style.display = 'block';
      err.textContent = 'Upload selhal: HTTP ' + xhr.status;
    }
  };

  xhr.onerror = function(){
    err.style.display = 'block';
    err.textContent = 'Upload selhal: chyba spojení.';
  };

  xhr.open(form.method || 'POST', form.action, true);
  xhr.send(data);

  return false;
}

function niceBytes(bytes){
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
  if (bytes < 1024 * 1024 * 1024) return (bytes / 1024 / 1024).toFixed(1) + ' MB';
  return (bytes / 1024 / 1024 / 1024).toFixed(2) + ' GB';
}

function setStatusText(id, value, cls){
  const el = document.getElementById(id);
  if (!el) return;
  el.textContent = value;
  if (cls !== undefined) el.className = cls || '';
}

function refreshBottomStatus(){
  fetch('/status.json', { credentials: 'same-origin', cache: 'no-store' })
    .then(function(r){ if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })
    .then(function(st){
      setStatusText('barHeap', 'Heap: ' + st.heapUsed + ' / ' + st.heapTotal);
      setStatusText('barPsram', 'PSRAM: ' + st.psramUsed + ' / ' + st.psramTotal, st.psramOk ? 'ok' : 'warn');
      setStatusText('barUsb', 'USB: ' + st.usb, st.usbOk ? 'ok' : 'warn');
      setStatusText('barRssi', st.wifiConnected ? ('WiFi: ' + st.rssi + ' dBm') : 'WiFi: AP only', st.wifiConnected ? 'ok' : 'warn');
      setStatusText('barAudio', 'Audio: ' + st.audio, st.audioPlaying ? 'ok' : 'warn');
      setStatusText('barUptime', 'Uptime: ' + st.uptime);
      setStatusText('barAudioDetail', (st.audioPlaying ? '▶ ' : '') + (st.audioDetail || st.audio || ''));
      updateAudioLabels(st.audioDetail || st.audio || '');
      if (typeof st.audioVolume !== 'undefined' && !volumeTimer) {
        updateAudioVolumeDisplay(st.audioVolume);
      }
    })
    .catch(function(){});
}

document.addEventListener('DOMContentLoaded', function(){
  refreshBottomStatus();
  setInterval(refreshBottomStatus, 2000);
});
</script>

</head>
<body>
<header>
<nav>
<a href="/files">Soubory</a>
<a href="/radio">Radio</a>
<a href="/karaoke">Karaoke</a>
<a href="/games">Hry</a>
<a href="/config">Konfigurace</a>
<a href="/update">Updater</a>
</nav>
</header>
<main>
)rawliteral";

  return html;
}

String compactStatusBarHtml() {
  String s;
  s += "<div class='statusbar'>";

  s += "<span id='barHeap'>Heap: " + bytesHuman((uint64_t)ESP.getHeapSize() - ESP.getFreeHeap()) + " / " + bytesHuman(ESP.getHeapSize()) + "</span>";

  bool psOk = psramFound();
  s += "<span id='barPsram' class='";
  s += psOk ? "ok" : "warn";
  s += "'>PSRAM: ";
  if (psOk) {
    s += bytesHuman((uint64_t)ESP.getPsramSize() - ESP.getFreePsram()) + " / " + bytesHuman(ESP.getPsramSize());
  } else {
    s += "nenalezena";
  }
  s += "</span>";

  bool usbOk = usbDiskMounted();
  s += "<span id='barUsb' class='";
  s += usbOk ? "ok" : "warn";
  s += "'>USB: ";
  s += usbOk ? "připojeno" : "nepřipojeno";
  s += "</span>";

  bool wifiOk = WiFi.status() == WL_CONNECTED;
  s += "<span id='barRssi' class='";
  s += wifiOk ? "ok" : "warn";
  s += "'>WiFi: ";
  if (wifiOk) {
    s += String(WiFi.RSSI()) + " dBm";
  } else {
    s += "AP only";
  }
  s += "</span>";

  s += "<span id='barAudio' class='";
  s += audioPlaying ? "ok" : "warn";
  s += "'>Audio: ";
  if (radioPlaying) {
    s += "radio";
  } else if (playlistActive) {
    s += "složka";
  } else if (audioPlaying) {
    s += "soubor";
  } else if (audioReady) {
    s += "připraveno";
  } else {
    s += "nepřipraveno";
  }
  s += "</span>";

  s += "<span id='barUptime'>Uptime: " + uptimeHuman() + "</span>";

  s += "<span id='barAudioDetail'>";
  if (audioPlaying) {
    s += "▶ " + htmlEscape(audioStatus);
  } else {
    s += htmlEscape(audioStatus);
  }
  s += "</span>";

  s += "</div>";
  return s;
}

String pageFooter() {
  return "</main>" + compactStatusBarHtml() + "</body></html>";
}


String statusCardsHtml() {
  String s;

  s += "<div class='grid'>";

  s += "<div class='box'><b>AP hotspot</b><br>";
  s += "SSID: " + htmlEscape(cfg.apSsid) + "<br>";
  s += "IP: " + WiFi.softAPIP().toString() + "<br>";

  if (cfg.apPass.length() >= 8) {
    s += "Heslo: nastaveno";
  } else {
    s += "<span class='warn'>Bez hesla</span>";
  }

  s += "</div>";

  s += "<div class='box'><b>Domácí Wi-Fi</b><br>";

  if (WiFi.status() == WL_CONNECTED) {
    s += "<span class='good'>Připojeno</span><br>";
    s += "SSID: " + htmlEscape(WiFi.SSID()) + "<br>";
    s += "IP: " + WiFi.localIP().toString();
  } else if (cfg.staSsid.length()) {
    s += "Uloženo: " + htmlEscape(cfg.staSsid) + "<br>";
    s += "<span class='bad'>Nepřipojeno</span>";
  } else {
    s += "Nenastaveno";
  }

  s += "</div>";

  s += "<div class='box'><b>mDNS</b><br>";
  if (mdnsStarted) {
    s += "Jméno: " + htmlEscape(mdnsStartedName) + ".local<br>";
    s += "URL: http://" + htmlEscape(mdnsStartedName) + ".local/";
  } else {
    s += "<span class='warn'>Není spuštěno</span><br>";
    s += "Nastaveno: " + htmlEscape(normalizeMdnsName(cfg.mdnsName)) + ".local";
  }
  s += "</div>";

  s += "<div class='box'><b>Interní FFat</b><br>";
  s += "Celkem: " + bytesHuman(FFat.totalBytes()) + "<br>";
  s += "Použito: " + bytesHuman(FFat.usedBytes()) + "<br>";
  s += "Volno: " + bytesHuman(FFat.totalBytes() - FFat.usedBytes());
  s += "</div>";

  s += usbInfoHtml();

  s += "<div class='box'><b>FTP server</b><br>";
  if (cfg.ftpEnabled) {
    s += "Stav: zapnutý<br>";
    s += "Port: 21<br>";
    s += "PASV: 50009<br>";
    s += "Uživatel: " + htmlEscape(cfg.ftpUser) + "<br>";
    s += "Disk: " + diskTitle(cfg.ftpDisk);
  } else {
    s += "Stav: vypnutý";
  }
  s += "</div>";

s += "<div class='box'><b>Audio výstup</b><br>";
if (audioReady) {
  s += "<span class='good'>Připraveno: ";
  s += audioOutputKindName();
  s += "</span><br>";
} else {
  s += "<span class='warn'>Nepřipraveno</span><br>";
}
s += htmlEscape(audioStatus) + "<br>";
if (audioPlaying) {
  s += "<button class='secondary' type='button' onclick=\"return stopAudioNoReload('/audio/stop')\">Stop</button>";
}
s += "</div>";


  s += "</div>";

  return s;
}

// ============================================================
// /config
// ============================================================

void handleConfigPage() {
  String html = pageHeader("ESP32-S3 konfigurace");

  html += "<h2>Konfigurace</h2>";

  html += "<div class='card'>";
  html += statusCardsHtml();
  html += "</div>";

  html += "<div class='card'>";
  html += "<h3>AP hotspot</h3>";
  html += "<form method='POST' action='/config/save'>";
  html += "<label>AP SSID</label>";
  html += "<input name='ap_ssid' value='" + htmlEscape(cfg.apSsid) + "'>";
  html += "<label>AP heslo</label>";
  html += "<input name='ap_pass' type='password' value='" + htmlEscape(cfg.apPass) + "'>";
  html += "<div class='small'>Heslo musí mít aspoň 8 znaků. Prázdné = otevřený hotspot.</div>";

  html += "<h3>Domácí Wi-Fi</h3>";
  html += "<label>STA SSID</label>";
  html += "<input name='sta_ssid' value='" + htmlEscape(cfg.staSsid) + "'>";
  html += "<label>STA heslo</label>";
  html += "<input name='sta_pass' type='password' value='" + htmlEscape(cfg.staPass) + "'>";
  html += "<label>mDNS jméno</label>";
  html += "<input name='mdns_name' value='" + htmlEscape(normalizeMdnsName(cfg.mdnsName)) + "' placeholder='oris-radio'>";
  html += "<div class='small'>Zařízení pak najdeš jako http://" + htmlEscape(normalizeMdnsName(cfg.mdnsName)) + ".local/</div>";

  html += "<h3>Web přihlášení</h3>";
  html += "<label>Web uživatel</label>";
  html += "<input name='web_user' value='" + htmlEscape(cfg.webUser) + "'>";
  html += "<label>Web heslo</label>";
  html += "<input name='web_pass' type='password' value='" + htmlEscape(cfg.webPass) + "'>";
  html += "<div class='small'>Výchozí je admin / admin. Doporučuju změnit hned po prvním spuštění.</div>";

  html += "<h3>FTP přístup</h3>";
  html += "<label><input style='width:auto' type='checkbox' name='ftp_enabled' value='1'";
  if (cfg.ftpEnabled) html += " checked";
  html += "> Zapnout FTP server</label>";
  html += "<label>FTP uživatel</label>";
  html += "<input name='ftp_user' value='" + htmlEscape(cfg.ftpUser) + "'>";
  html += "<label>FTP heslo</label>";
  html += "<input name='ftp_pass' type='password' value='" + htmlEscape(cfg.ftpPass) + "'>";
  html += "<label>FTP disk</label>";
  html += "<select name='ftp_disk'>";
  html += "<option value='ffat'";
  if (cfg.ftpDisk == "ffat") html += " selected";
  html += ">Interní FFat</option>";
  html += "<option value='usb0'";
  if (cfg.ftpDisk == "usb0") html += " selected";
  html += ">USB disk</option>";
  html += "</select>";
  html += "<div class='small'>FTP běží na portu 21, data PASV na portu 50009. Používej pasivní režim.</div>";

  html += "<h3>RGB LED podle hudby</h3>";
  html += "<label><input style='width:auto' type='checkbox' name='rgb_enabled' value='1'";
  if (cfg.rgbEnabled) html += " checked";
  html += "> Zapnout RGB LED na GPIO48</label>";
  html += "<label>Výchozí hlasitost: ";
  html += String(cfg.audioVolume);
  html += " %</label>";
  html += "<input type='range' min='0' max='100' name='audio_volume' value='" + String(cfg.audioVolume) + "' data-audio-volume-range>";

  html += "<h3>Internetové rádio</h3>";
  html += "<label>Název stanice</label>";
  html += "<input name='radio_name' value='" + htmlEscape(cfg.radioName[0]) + "'>";
  html += "<label>URL MP3 streamu</label>";
  html += "<input name='radio_url' value='" + htmlEscape(cfg.radioUrl[0]) + "' placeholder='http://server/stream.mp3'>";
  html += "<div class='small'>Podpora: čistý HTTP MP3 stream. Redirect/playlist/AAC zatím ne.</div>";

  html += "<button type='submit'>Uložit konfiguraci</button>";
  html += "</form>";
  html += "</div>";

  html += "<div class='card'>";
  html += "<h3>Restart</h3>";
  html += "<form method='POST' action='/reboot' onsubmit='return confirm(\"Restartovat ESP32?\")'>";
  html += "<button class='secondary' type='submit'>Restartovat ESP32</button>";
  html += "</form>";
  html += "</div>";

  html += pageFooter();

  server.send(200, "text/html", html);
}

void handleConfigSave() {
  String apSsid = server.arg("ap_ssid");
  String apPass = server.arg("ap_pass");
  String staSsid = server.arg("sta_ssid");
  String staPass = server.arg("sta_pass");
  String mdnsName = server.arg("mdns_name");
  String webUser = server.arg("web_user");
  String webPass = server.arg("web_pass");
  bool ftpEnabled = server.hasArg("ftp_enabled");
  String ftpUser = server.arg("ftp_user");
  String ftpPass = server.arg("ftp_pass");
  String ftpDiskCfg = server.arg("ftp_disk");
  bool rgbEnabled = server.hasArg("rgb_enabled");
  int audioVolume = server.hasArg("audio_volume") ? server.arg("audio_volume").toInt() : cfg.audioVolume;
  String radioName = server.arg("radio_name");
  String radioUrl = server.arg("radio_url");

  apSsid.trim();
  apPass.trim();
  staSsid.trim();
  staPass.trim();
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

  if (radioName.length() == 0) {
    radioName = "Moje radio";
  }

  cfg.apSsid = apSsid;
  cfg.apPass = apPass;
  cfg.staSsid = staSsid;
  cfg.staPass = staPass;
  cfg.mdnsName = mdnsName;
  cfg.webUser = webUser;
  cfg.webPass = webPass;
  cfg.ftpEnabled = ftpEnabled;
  cfg.ftpUser = ftpUser;
  cfg.ftpPass = ftpPass;
  cfg.ftpDisk = ftpDiskCfg;
  cfg.rgbEnabled = rgbEnabled;
  cfg.audioVolume = audioVolume;
  applyAudioVolume();
  saveAudioVolumeStateNow();
  cfg.radioName[0] = radioName;
  cfg.radioUrl[0] = radioUrl;

  if (!saveConfig()) {
    server.send(500, "text/plain", "Nepodarilo se ulozit konfiguraci");
    return;
  }

  WiFi.disconnect(false);
  delay(200);

  startAp();
  connectStaIfConfigured();
  startMdns(true);
  ftpStartServerIfNeeded();

  server.sendHeader("Location", "/config");
  server.send(303);
}

void handleReboot() {
  server.send(200, "text/plain", "Restartuju...");
  delay(500);
  ESP.restart();
}

// ============================================================
// /files
// ============================================================

String diskOptionsHtml(const String& selected) {
  String h;

  h += "<option value='ffat'";
  if (selected == "ffat") h += " selected";
  h += ">Interní FFat</option>";

  h += "<option value='usb0'";
  if (selected == "usb0") h += " selected";
  h += ">USB disk";
  if (!usbDiskMounted()) {
    h += " (nepřipojen)";
  }
  h += "</option>";

  return h;
}

void appendFilesTable(String& html, const String& disk, const String& dirPath) {
  File root = fsOpenGeneric(disk, dirPath, FILE_READ);

  if (!root || !root.isDirectory()) {
    if (root) root.close();
    html += "<p class='bad'>Nelze otevřít složku.</p>";
    return;
  }

  File file = root.openNextFile();

  html += "<table><tr><th>Název</th><th>Velikost</th><th>Akce</th></tr>";

  if (dirPath != "/") {
    String up = parentPath(dirPath);
    html += "<tr>";
    html += "<td>📁 ..</td>";
    html += "<td>nahoru</td>";
    html += "<td class='actions'>";
    html += "<a href='/files?disk=" + disk + "&p=" + urlEncode(up) + "'>otevřít</a>";
    html += "</td>";
    html += "</tr>";
  }

  while (file) {
    String rawName = file.name();
    String shownName = displayNameForEntry(dirPath, rawName);
    String fullPath = fullPathForEntry(dirPath, rawName);
    size_t size = file.size();

    if (shownName.length() == 0 || fullPath.length() == 0) {
      file.close();
      file = root.openNextFile();
      continue;
    }

    String enc = urlEncode(fullPath);
    String encDir = urlEncode(dirPath);

    html += "<tr>";

    if (file.isDirectory()) {
      html += "<td>📁 " + htmlEscape(shownName) + "</td>";
      html += "<td>složka</td>";
    } else {
      html += "<td>📄 " + htmlEscape(shownName) + "</td>";
      html += "<td>" + bytesHuman(size) + "</td>";
    }

    html += "<td class='actions'>";

    if (file.isDirectory()) {
      html += "<a href='/files?disk=" + disk + "&p=" + enc + "'>otevřít</a>";
      html += "<a href='#' onclick=\"return renameEntry('" + disk + "','" + enc + "','" + encDir + "','" + urlEncode(shownName) + "')\">rename</a>";
      html += "<a href='/delete?disk=" + disk + "&f=" + enc + "&p=" + encDir + "' onclick='return confirm(\"Smazat složku včetně obsahu?\")'>delete</a>";
    } else {
      String ext = fileExt(shownName);
      String lowerName = shownName;
      lowerName.toLowerCase();

      html += "<a href='/view?disk=" + disk + "&f=" + enc + "'>náhled</a>";

      if (lowerName.endsWith(".mp3")) {
        html += "<a href='#' onclick=\"return playAudioNoReload('/audio/play?disk=" + disk + "&f=" + enc + "')\">play</a>";
      }

      if (isTextExt(ext)) {
        html += "<a href='/edit?disk=" + disk + "&f=" + enc + "'>edit</a>";
      }

      html += "<a href='/download?disk=" + disk + "&f=" + enc + "'>download</a>";
      html += "<a href='#' onclick=\"return renameEntry('" + disk + "','" + enc + "','" + encDir + "','" + urlEncode(shownName) + "')\">rename</a>";
      html += "<a href='/delete?disk=" + disk + "&f=" + enc + "&p=" + encDir + "' onclick='return confirm(\"Smazat soubor?\")'>delete</a>";
    }

    html += "</td>";
    html += "</tr>";

    file.close();
    file = root.openNextFile();
  }

  root.close();
  html += "</table>";
}

void handleFilesPage() {
  String disk = server.hasArg("disk") ? server.arg("disk") : currentDisk;

  if (disk != "ffat" && disk != "usb0") {
    disk = "ffat";
  }

  String dirPath = server.hasArg("p") ? server.arg("p") : "/";
  dirPath = normalizeDirPath(dirPath);

  currentDisk = disk;

  String html = pageHeader("ESP32-S3 Web Disk");

  html += "<div class='explorer'>";

  html += "<aside class='sidebar'>";
  html += "<div class='sidebar-section'>";
  html += "<h3>Disk</h3>";
  html += "<form method='GET' action='/files'>";
  html += "<select name='disk'>";
  html += diskOptionsHtml(disk);
  html += "</select>";
  html += "<input type='hidden' name='p' value='" + htmlEscape(dirPath) + "'>";
  html += "<button type='submit'>Otevřít</button>";
  html += "</form>";
  html += "<form method='POST' action='/usb/remount'>";
  html += "<button class='secondary' type='submit'>USB remount</button>";
  html += "</form>";
  html += "</div>";

  html += "<div class='sidebar-section'>";
  html += "<h3>Rychlé cesty</h3>";
  html += "<p><a href='/files?disk=" + disk + "&p=/'>/ root</a></p>";
  if (dirPath != "/") {
    html += "<p><a href='/files?disk=" + disk + "&p=" + urlEncode(parentPath(dirPath)) + "'>⬆ nadřazená složka</a></p>";
  }
  html += "<p class='small'>Aktuální disk: " + diskTitle(disk) + "</p>";
  html += "</div>";

  html += audioSidePanelHtml();

  html += "<div class='sidebar-section'>";
  html += "<h3>Vytvořit</h3>";
  if (!diskAvailable(disk)) {
    html += "<p class='bad'>Disk není dostupný.</p>";
  } else {
    html += "<form method='POST' action='/mkdir'>";
    html += "<input type='hidden' name='disk' value='" + disk + "'>";
    html += "<input type='hidden' name='p' value='" + htmlEscape(dirPath) + "'>";
    html += "<input name='name' placeholder='Nová složka'>";
    html += "<button type='submit'>Vytvořit složku</button>";
    html += "</form>";

    html += "<form method='POST' action='/create'>";
    html += "<input type='hidden' name='disk' value='" + disk + "'>";
    html += "<input type='hidden' name='p' value='" + htmlEscape(dirPath) + "'>";
    html += "<input name='name' placeholder='Nový soubor.txt'>";
    html += "<button type='submit'>Vytvořit soubor</button>";
    html += "</form>";
  }
  html += "</div>";

  html += "<div class='sidebar-section'>";
  html += "<h3>Údržba</h3>";
  if (disk == "ffat") {
    html += "<form method='POST' action='/format' onsubmit='return confirm(\"Opravdu FORMÁTOVAT interní FFat?\")'>";
    html += "<input type='hidden' name='disk' value='ffat'>";
    html += "<input name='confirm' placeholder='FORMAT'>";
    html += "<button class='danger' type='submit'>Formát FFat</button>";
    html += "</form>";
  } else {
    html += "<p class='small warn'>USB formátuj v PC jako FAT32.</p>";
  }
  html += "</div>";
  html += "</aside>";

  html += "<section class='filepane'>";
  html += "<div class='pathbar'>";
  html += "<div class='path'><b>" + diskTitle(disk) + ":</b> " + htmlEscape(dirPath) + "</div>";
  html += "<div>";
  html += "<a href='/radio'>Radio</a> &nbsp; <a href='/config'>Nastavení</a>";
  html += "</div>";
  html += "</div>";

  html += "<div class='toolbar'>";
  if (diskAvailable(disk)) {
    html += "<button type='button' class='secondary' onclick=\"return playAudioNoReload('/audio/play_folder?disk=" + disk + "&p=" + urlEncode(dirPath) + "')\">Přehrát složku</button>";
    html += "<form method='POST' action='/upload?disk=" + disk + "&p=" + urlEncode(dirPath) + "' enctype='multipart/form-data' onsubmit='return uploadWithModal(this)'>";
    html += "<input type='file' name='file'>";
    html += "<button type='submit'>Nahrát</button>";
    html += "</form>";
  } else {
    html += "<span class='bad'>Disk není dostupný.</span>";
  }
  html += "</div>";

  html += "<div class='filelist'>";
  if (!diskAvailable(disk)) {
    html += "<p class='bad' style='padding:10px'>Disk není dostupný.</p>";
  } else {
    appendFilesTable(html, disk, dirPath);
  }
  html += "</div>";

  html += "</section>";
  html += "</div>";

  html += pageFooter();

  server.send(200, "text/html", html);
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
  String disk, path;
  if (!getDiskPathArgs(disk, path)) {
    server.send(404, "text/plain", "File not found");
    return;
  }

  File file = fsOpenGeneric(disk, path, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    server.send(400, "text/plain", "Cannot preview directory");
    return;
  }

  String ext = fileExt(path);
  String enc = urlEncode(path);
  String rawUrl = "/raw?disk=" + disk + "&f=" + enc;
  String html = pageHeader("Náhled souboru");
  html += "<h2>Náhled: " + htmlEscape(path) + "</h2>";
  html += "<div class='card'><a href='/files?disk=" + disk + "'>zpět</a> &nbsp; ";
  html += "<a href='/download?disk=" + disk + "&f=" + enc + "'>download</a>";
  if (isTextExt(ext)) html += " &nbsp; <a href='/edit?disk=" + disk + "&f=" + enc + "'>edit</a>";
  html += "</div><div class='card'>";

  if (isImageExt(ext)) {
    html += "<img class='preview' src='" + rawUrl + "'>";
  } else if (isAudioExt(ext)) {
    html += "<audio class='preview' controls src='" + rawUrl + "'></audio>";
  } else if (isVideoExt(ext)) {
    html += "<video class='preview' controls src='" + rawUrl + "'></video>";
  } else if (isTextExt(ext)) {
    if (file.size() > MAX_TEXT_EDIT_SIZE) {
      html += "<p class='warn'>Soubor je moc velký pro náhled v RAM. Použij download.</p>";
    } else {
      String content = file.readString();
      html += "<pre class='viewer'>" + htmlEscape(content) + "</pre>";
    }
  } else {
    html += "<p>Pro tento typ není náhled. Použij download.</p>";
  }

  html += "</div>" + pageFooter();
  file.close();
  server.send(200, "text/html", html);
}

void handleEditFile() {
  String disk, path;
  if (!getDiskPathArgs(disk, path)) {
    server.send(404, "text/plain", "File not found");
    return;
  }

  String ext = fileExt(path);
  if (!isTextExt(ext)) {
    server.send(400, "text/plain", "Only text files can be edited");
    return;
  }

  File file = fsOpenGeneric(disk, path, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    server.send(400, "text/plain", "Cannot open file");
    return;
  }

  if (file.size() > MAX_TEXT_EDIT_SIZE) {
    file.close();
    server.send(413, "text/plain", "File too large for editor");
    return;
  }

  String content = file.readString();
  file.close();

  String enc = urlEncode(path);
  String html = pageHeader("Editor souboru");
  html += "<h2>Editor: " + htmlEscape(path) + "</h2>";
  html += "<div class='card'><form method='POST' action='/save?disk=" + disk + "&f=" + enc + "'>";
  html += "<textarea name='content'>" + htmlEscape(content) + "</textarea>";
  html += "<button type='submit'>Uložit</button>";
  html += "</form></div>";
  html += "<div class='card'><a href='/view?disk=" + disk + "&f=" + enc + "'>náhled</a> &nbsp; <a href='/files?disk=" + disk + "'>zpět</a></div>";
  html += pageFooter();
  server.send(200, "text/html", html);
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
  String html = pageHeader("Firmware updater");
  html += "<h2>Firmware updater</h2>";
  html += "<div class='card'>";
  html += "<p class='warn'>Nahraj .bin firmware pro stejnou desku a partition scheme. Po úspěšném uploadu se ESP32 restartuje.</p>";
  html += "<form method='POST' action='/firmware' enctype='multipart/form-data' onsubmit='return confirm(\"Opravdu nahrát nový firmware?\")'>";
  html += "<input type='file' name='firmware' accept='.bin'>";
  html += "<button class='danger' type='submit'>Nahrát firmware</button>";
  html += "</form></div>";
  html += pageFooter();
  server.send(200, "text/html", html);
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

  server.send(200, "text/html", "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'></head><body style='font-family:Arial;background:#111;color:#ddd'><h2>Firmware nahrán</h2><p>ESP32 se restartuje...</p></body></html>");
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
      "text/html",
      "<!doctype html>"
      "<html>"
      "<head>"
      "<meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<meta http-equiv='refresh' content='12;url=/files'>"
      "<title>Format FFat</title>"
      "<style>"
      "body{font-family:Arial;background:#111;color:#ddd;padding:20px}"
      "a{color:#6cf}"
      "</style>"
      "</head>"
      "<body>"
      "<h2>Formátování FFat naplánováno</h2>"
      "<p>ESP32 se teď restartuje. FFat se smaže hned při startu, ještě před spuštěním webserveru.</p>"
      "<p>Počkej pár sekund a znovu otevři <a href='/files'>/files</a>.</p>"
      "<p>Po formátu bude hotspot opět <b>ESP32-FS</b> / <b>12345678</b>.</p>"
      "</body>"
      "</html>"
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


void handleRadioPage() {
  if (!checkWebAuth()) return;

  String html = pageHeader("Internetové rádio");

  html += "<h2>Internetové rádio</h2>";

  html += "<div class='card'>";
  html += statusCardsHtml();
  html += "</div>";

  html += "<div class='card'>";
  html += "<h3>Stanice</h3>";
  html += "<form method='POST' action='/radio/save'>";
  html += "<table><tr><th>#</th><th>Název</th><th>HTTP MP3 URL</th><th>Akce</th></tr>";

  for (uint8_t i = 0; i < MAX_RADIO_STATIONS; i++) {
    html += "<tr>";
    html += "<td>" + String(i + 1) + "</td>";
    html += "<td><input name='radio_name_" + String(i) + "' value='" + htmlEscape(cfg.radioName[i]) + "' placeholder='Název stanice'></td>";
    html += "<td><input name='radio_url_" + String(i) + "' value='" + htmlEscape(cfg.radioUrl[i]) + "' placeholder='http://server/stream.mp3'></td>";
    html += "<td class='actions'>";
    if (cfg.radioUrl[i].length() > 0) {
      html += "<a href='/radio/play?i=" + String(i) + "' onclick='return playRadioNoReload(" + String(i) + ")'>Play</a>";
    } else {
      html += "<span class='small'>není URL</span>";
    }
    html += "</td>";
    html += "</tr>";
  }

  html += "</table>";
  html += "<button type='submit'>Uložit stanice</button>";
  html += "</form>";
  html += "<div class='small'>Podpora: čistý HTTP MP3 stream. Redirect/playlist/AAC zatím ne.</div>";
  html += "</div>";

  html += "<div class='card'>";
  html += "<h3>Přehrávání</h3>";
  html += "<p><b>Stav:</b> <span id='audio-status-inline'>" + htmlEscape(audioStatus) + "</span></p>";
  html += "<label>Hlasitost: <span data-audio-volume-label>" + String(cfg.audioVolume) + " %</span></label>";
  html += "<input type='range' min='0' max='100' value='" + String(cfg.audioVolume) + "' data-audio-volume-range oninput='setAudioVolume(this.value)'>";
  html += "<div style='display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:8px;margin-top:8px'>";
  html += "<button class='secondary' type='button' onclick=\"return stopAudioAjax()\">Stop</button>";
  html += "<button class='secondary' type='button' onclick=\"return audioAction('/audio/prev')\">⏮ Předchozí</button>";
  html += "<button class='secondary' type='button' onclick=\"return audioAction('/audio/next')\">Další ⏭</button>";
  html += "<button class='secondary' type='button' onclick=\"return audioAction('/audio/shuffle')\">Shuffle: ";
  html += playlistShuffle ? "ON" : "OFF";
  html += "</button>";
  html += "<button class='secondary' type='button' onclick=\"return audioAction('/audio/repeat')\">Repeat: ";
  html += playlistRepeat ? "ON" : "OFF";
  html += "</button>";
  html += "</div>";
  html += "<p class='small'>Ovládání next/prev funguje pro přehrávání složky. Hlasitost platí pro rádio i MP3.</p>";
  html += "</div>";

  html += pageFooter();

  server.send(200, "text/html", html);
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
// Karaoke web: levý sloupec souborů + plátno textu
// ============================================================

bool isKaraokeJsonPath(const String& path) {
  String lower = path;
  lower.toLowerCase();
  return lower.endsWith(".ock") || lower.endsWith(".karaoke.json");
}

String karaokeDisplayNameFromPath(const String& path) {
  String name = fileNameFromPath(path);
  String lower = name;
  lower.toLowerCase();

  if (lower.endsWith(".karaoke.json")) {
    name.remove(name.length() - 13);
  } else if (lower.endsWith(".ock")) {
    name.remove(name.length() - 4);
  }

  return name;
}

String karaokeFileNameOnly(String path) {
  path = urlDecode(path);
  path.replace("\\", "/");
  path.trim();

  int slash = path.lastIndexOf('/');
  if (slash >= 0) {
    path = path.substring(slash + 1);
  }

  int colon = path.lastIndexOf(':');
  if (colon >= 0) {
    path = path.substring(colon + 1);
  }

  path.trim();
  return path;
}

String karaokeJsonBaseName(String jsonPath) {
  String name = karaokeFileNameOnly(jsonPath);
  String lower = name;
  lower.toLowerCase();

  if (lower.endsWith(".karaoke.json")) {
    name.remove(name.length() - 13);
  } else if (lower.endsWith(".ock")) {
    name.remove(name.length() - 4);
  } else {
    int dot = name.lastIndexOf('.');
    if (dot > 0) name.remove(dot);
  }

  return name;
}

bool tryKaraokeAudioCandidate(const String& disk, String candidate, String& resolved, String& tried) {
  candidate.trim();
  if (candidate.length() == 0 || candidate.indexOf("..") >= 0) {
    return false;
  }

  if (!safePath(candidate)) {
    return false;
  }

  if (tried.length() > 0) tried += ", ";
  tried += candidate;

  if (fsExistsGeneric(disk, candidate)) {
    resolved = candidate;
    return true;
  }

  return false;
}

String resolveExistingKaraokeAudioPath(const String& disk, String jsonPath, String audioName, String& tried) {
  String resolved = "";
  String dir = parentPath(jsonPath);

  audioName = urlDecode(audioName);
  audioName.replace("\\", "/");
  audioName.trim();

  if (audioName.length() > 0 && audioName.indexOf("..") < 0) {
    if (audioName.startsWith("/")) {
      if (tryKaraokeAudioCandidate(disk, audioName, resolved, tried)) return resolved;
    }

    String rel = joinPath(dir, audioName);
    if (tryKaraokeAudioCandidate(disk, rel, resolved, tried)) return resolved;

    String justName = karaokeFileNameOnly(audioName);
    if (justName.length() > 0 && justName != audioName) {
      String sameDir = joinPath(dir, justName);
      if (tryKaraokeAudioCandidate(disk, sameDir, resolved, tried)) return resolved;
    }
  }

  String base = karaokeJsonBaseName(jsonPath);
  const char* exts[] = { ".mp3", ".MP3", ".wav", ".WAV" };
  for (uint8_t i = 0; i < 4; i++) {
    String fallback = joinPath(dir, base + String(exts[i]));
    if (tryKaraokeAudioCandidate(disk, fallback, resolved, tried)) return resolved;
  }

  return "";
}

void appendKaraokeListRecursive(String& json, const String& disk, const String& dirPath, bool& first, uint8_t depth) {
  if (depth > 10 || !diskAvailable(disk)) {
    return;
  }

  File root = fsOpenGeneric(disk, dirPath, FILE_READ);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  File file = root.openNextFile();
  while (file) {
    bool isDir = file.isDirectory();
    String rawName = file.name();
    file.close();

    String fullPath = fullPathForEntry(dirPath, rawName);
    if (fullPath.length() > 0) {
      if (isDir) {
        appendKaraokeListRecursive(json, disk, fullPath, first, depth + 1);
      } else if (isKaraokeJsonPath(fullPath)) {
        if (!first) json += ",";
        first = false;
        json += "{";
        json += "\"disk\":\"" + jsonEscape(disk) + "\",";
        json += "\"path\":\"" + jsonEscape(fullPath) + "\",";
        json += "\"name\":\"" + jsonEscape(karaokeDisplayNameFromPath(fullPath)) + "\"";
        json += "}";
      }
    }

    file = root.openNextFile();
  }

  root.close();
}

void handleKaraokeListJson() {
  if (!checkWebAuth()) return;

  String disk = server.hasArg("disk") ? server.arg("disk") : "usb0";
  String json = "[";
  bool first = true;

  if (disk == "all") {
    appendKaraokeListRecursive(json, "usb0", "/", first, 0);
    appendKaraokeListRecursive(json, "ffat", "/", first, 0);
  } else if (disk == "usb0" || disk == "ffat") {
    appendKaraokeListRecursive(json, disk, "/", first, 0);
  }

  json += "]";
  server.send(200, "application/json; charset=utf-8", json);
}

void handleKaraokePlay() {
  if (!checkWebAuth()) return;

  String disk = server.hasArg("disk") ? server.arg("disk") : "usb0";
  String jsonPath = server.arg("json");
  String audioArg = server.arg("audio");
  String title = server.arg("title");

  if ((disk != "usb0" && disk != "ffat") || !safePath(jsonPath) || !isKaraokeJsonPath(jsonPath)) {
    server.send(400, "text/plain", "Bad karaoke path");
    return;
  }

  if (!diskAvailable(disk) || !fsExistsGeneric(disk, jsonPath)) {
    server.send(404, "text/plain", "Karaoke JSON nenalezen");
    return;
  }

  String triedAudioPaths = "";
  String audioPath = resolveExistingKaraokeAudioPath(disk, jsonPath, audioArg, triedAudioPaths);
  if (audioPath.length() == 0) {
    server.send(404, "text/plain; charset=utf-8",
      "Audio k JSONu nenalezeno. JSON: " + jsonPath +
      "; audio z JSONu: " + audioArg +
      "; hledal jsem: " + triedAudioPaths);
    return;
  }

  playlistActive = false;
  karaokeActive = true;
  karaokeDisk = disk;
  karaokeJsonPath = jsonPath;
  karaokeAudioPath = audioPath;
  karaokeTitle = title.length() ? title : karaokeDisplayNameFromPath(jsonPath);
  audioStatus = "Karaoke: startuji " + karaokeTitle;

  if (!queueAudioFilePlay(disk, audioPath)) {
    clearKaraokeState();
    server.send(503, "text/plain", "Audio fronta je plná");
    return;
  }

  server.send(200, "text/plain", audioStatus);
}

void handleKaraokePage() {
  if (!checkWebAuth()) return;

  String html = pageHeader("Karaoke");
  html += R"rawliteral(
<style>
.karaoke-layout{display:grid;grid-template-columns:320px minmax(0,1fr);gap:10px;height:calc(100vh - 96px);min-height:540px}
.karaoke-list{background:#181b20;border:1px solid #303640;border-radius:10px;padding:10px;overflow:hidden;display:flex;flex-direction:column}
.karaoke-results{overflow:auto;display:flex;flex-direction:column;gap:6px;margin-top:8px}
.karaoke-item{background:#242a33;border:1px solid #323a45;border-radius:8px;padding:8px;text-align:left;color:#fff;cursor:pointer}
.karaoke-item:hover{background:#2f80ed}.karaoke-item .small{display:block;margin-top:3px;color:#c9d1d9;word-break:break-word}
.karaoke-stage{background:radial-gradient(circle at center,#172033 0,#090b0f 65%);border:1px solid #303640;border-radius:10px;display:flex;flex-direction:column;overflow:hidden}
.karaoke-top{display:flex;gap:8px;align-items:center;padding:10px;border-bottom:1px solid #303640;background:rgba(0,0,0,.25)}
.karaoke-top button{width:auto;margin:0}.karaoke-top .title{font-weight:bold;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;flex:1}
.karaoke-canvas{flex:1;display:flex;align-items:center;justify-content:center;text-align:center;padding:22px;position:relative}
.karaoke-lines{width:100%;max-width:1100px}.karaoke-prev,.karaoke-next{font-size:clamp(20px,3vw,38px);color:#8b949e;min-height:1.2em}.karaoke-current{font-size:clamp(34px,6vw,86px);font-weight:800;line-height:1.16;color:white;text-shadow:0 3px 12px #000;margin:20px 0}.karaoke-word{display:inline-block;margin:0;white-space:pre}.karaoke-word.done,.karaoke-word.active{color:#ffd166}.karaoke-word.active{text-decoration:underline}.karaoke-status{padding:8px 10px;border-top:1px solid #303640;color:#9aa3ad;background:rgba(0,0,0,.25)}
@media(max-width:800px){.karaoke-layout{grid-template-columns:1fr;height:auto}.karaoke-stage{min-height:60vh}}
</style>
<h2>Karaoke</h2>
<div class="karaoke-layout">
  <section class="karaoke-list">
    <label>Hledat karaoke</label>
    <input id="karaokeSearch" placeholder="název / soubor..." oninput="renderKaraokeList()">
    <label>Disk</label>
    <select id="karaokeDisk" onchange="loadKaraokeList()"><option value="usb0">USB flashka</option><option value="ffat">Interní FFat</option><option value="all">Vše</option></select>
    <button type="button" onclick="loadKaraokeList()">Obnovit seznam</button>
    <div id="karaokeResults" class="karaoke-results"></div>
  </section>
  <section class="karaoke-stage" id="karaokeStage">
    <div class="karaoke-top">
      <button class="secondary" type="button" onclick="toggleFullscreenKaraoke()">Fullscreen</button>
      <button class="secondary" type="button" onclick="playRelativeKaraoke(-1)">⏮</button>
      <button class="secondary" type="button" onclick="playRelativeKaraoke(1)">⏭</button>
      <button class="secondary" type="button" onclick="return audioAction('/audio/stop')">Stop</button>
      <div class="title" id="karaokeTitle">Vyber skladbu vlevo</div>
    </div>
    <div class="karaoke-canvas">
      <div class="karaoke-lines">
        <div id="karaokePrev" class="karaoke-prev"></div>
        <div id="karaokeCurrent" class="karaoke-current">Karaoke připraveno</div>
        <div id="karaokeNext" class="karaoke-next"></div>
      </div>
    </div>
    <div id="karaokeStatus" class="karaoke-status">Čekám na výběr skladby.</div>
  </section>
</div>
<script>
let karaokeItems=[];
let karaokeDoc=null;
let karaokeDisk='usb0';
let karaokeJsonPath='';
let karaokeLastPosition=-1;
let karaokeCurrentIndex=-1;

function kEsc(s){return String(s||'').replace(/[&<>"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
function kFileName(p){p=String(p||'');let i=p.lastIndexOf('/');return i>=0?p.substring(i+1):p;}
function kParent(p){p=String(p||'/');let i=p.lastIndexOf('/');return i<=0?'/':p.substring(0,i);}
function kJoin(dir,name){name=String(name||'').replace(/\\/g,'/');if(name.startsWith('/'))return name; if(dir==='/'||!dir)return '/'+name; return dir+'/'+name;}
function kPick(o,keys,def){
  if(!o)return def;
  for(const k of keys){
    if(Object.prototype.hasOwnProperty.call(o,k) && o[k]!==undefined && o[k]!==null)return o[k];
  }
  return def;
}
function kCleanKaraokeText(s){
  // Lomítko v exportech z KFN bereme jako oddělovač slabik, ne jako znak k zobrazení.
  return String(s||'').replace(/\s*\/\s*/g,'');
}
function kMatchText(s){
  return String(s||'').normalize('NFD').replace(/[\u0300-\u036f]/g,'').toLowerCase().replace(/[^a-z0-9]/g,'');
}
function kBuildTimedSyllableText(lineText,words){
  if(!Array.isArray(words)||!words.length)return [];
  if(!lineText)return words;
  const lineKey=kMatchText(lineText);
  const wordsKey=kMatchText(words.map(w=>w.text||'').join(''));
  if(!lineKey||!wordsKey||lineKey!==wordsKey)return words;

  let pos=0;
  const out=words.map(w=>Object.assign({},w));
  for(let i=0;i<out.length;i++){
    const target=kMatchText(out[i].text||'');
    if(!target)continue;
    let got='';
    let display='';
    while(pos<lineText.length&&got.length<target.length){
      const ch=lineText[pos++];
      display+=ch;
      got+=kMatchText(ch);
    }
    out[i].text=display||out[i].text||'';
  }
  if(pos<lineText.length&&out.length)out[out.length-1].text+=lineText.substring(pos);
  return out;
}
function normalizeKaraokeDoc(doc){
  doc=doc||{};
  const rawLines=kPick(doc,['lines','Lines'],[]);
  const out={
    title:String(kPick(doc,['title','Title'],'')||''),
    artist:String(kPick(doc,['artist','Artist'],'')||''),
    audio:String(kPick(doc,['audio','Audio'],'')||''),
    lines:[]
  };
  if(Array.isArray(rawLines)){
    out.lines=rawLines.map(function(l,idx){
      const rawWords=kPick(l,['words','Words'],[]);
      const lineText=kCleanKaraokeText(String(kPick(l,['text','Text'],'')||''));
      const words=Array.isArray(rawWords)?rawWords.map(function(w){return {
        index:Number(kPick(w,['index','Index'],0)||0),
        text:kCleanKaraokeText(String(kPick(w,['text','Text'],'')||'')),
        timeMs:Number(kPick(w,['timeMs','TimeMs','TimeMS'],0)||0),
        endMs:Number(kPick(w,['endMs','EndMs','EndMS'],0)||0)
      };}):[];
      return {
        index:Number(kPick(l,['index','Index'],idx)||idx),
        text:lineText,
        startMs:Number(kPick(l,['startMs','StartMs','StartMS'],0)||0),
        endMs:Number(kPick(l,['endMs','EndMs','EndMS'],0)||0),
        words:kBuildTimedSyllableText(lineText,words)
      };
    });
  }
  return out;
}

function loadKaraokeList(){
  const disk=document.getElementById('karaokeDisk').value;
  fetch('/karaoke/list.json?disk='+encodeURIComponent(disk),{credentials:'same-origin',cache:'no-store'})
    .then(r=>r.json()).then(j=>{karaokeItems=j||[];renderKaraokeList();})
    .catch(e=>{document.getElementById('karaokeResults').innerHTML='<div class="warn">Seznam nejde načíst.</div>';});
}

function renderKaraokeList(){
  const q=(document.getElementById('karaokeSearch').value||'').toLowerCase();
  const box=document.getElementById('karaokeResults');
  let html='';
  const filtered=karaokeItems.filter(it=>(it.name||it.path||'').toLowerCase().indexOf(q)>=0);
  if(!filtered.length){box.innerHTML='<div class="small">Nic nenalezeno.</div>';return;}
  filtered.forEach((it,idx)=>{
    const real=karaokeItems.indexOf(it);
    html+='<button type="button" class="karaoke-item" onclick="playKaraoke('+real+')">'+kEsc(it.name||kFileName(it.path))+'<span class="small">'+kEsc(it.disk+': '+it.path)+'</span></button>';
  });
  box.innerHTML=html;
}

function playKaraoke(idx){
  const item=karaokeItems[idx]; if(!item)return;
  karaokeCurrentIndex=idx;
  karaokeDisk=item.disk; karaokeJsonPath=item.path;
  fetch('/raw?disk='+encodeURIComponent(item.disk)+'&f='+encodeURIComponent(item.path),{credentials:'same-origin',cache:'no-store'})
    .then(r=>r.json())
    .then(doc=>{
      doc=normalizeKaraokeDoc(doc);
      karaokeDoc=doc;
      document.getElementById('karaokeTitle').textContent=doc.title||item.name||kFileName(item.path);
      const audio=kJoin(kParent(item.path),doc.audio||'');
      const body='disk='+encodeURIComponent(item.disk)+'&json='+encodeURIComponent(item.path)+'&audio='+encodeURIComponent(doc.audio||'')+'&title='+encodeURIComponent(doc.title||item.name||'');
      return fetch('/karaoke/play',{method:'POST',credentials:'same-origin',headers:{'Content-Type':'application/x-www-form-urlencoded'},body}).then(async r=>{
        const text=await r.text();
        if(!r.ok) throw new Error(text||('HTTP '+r.status));
        document.getElementById('karaokeStatus').textContent=text||('Hraju: '+(doc.title||item.name||'')+' / '+audio);
        renderKaraokeAt(0);
      });
    })
    .catch(e=>{document.getElementById('karaokeStatus').textContent='Karaoke nejde spustit: '+e.message;});
}

function playRelativeKaraoke(delta){
  if(!karaokeItems.length)return;
  let idx=karaokeCurrentIndex;
  if(idx<0)idx=0; else idx=(idx+delta+karaokeItems.length)%karaokeItems.length;
  playKaraoke(idx);
}

function lineIndexForTime(ms){
  if(!karaokeDoc||!karaokeDoc.lines)return -1;
  let best=-1;
  for(let i=0;i<karaokeDoc.lines.length;i++){
    const l=karaokeDoc.lines[i];
    if(ms>=Number(l.startMs||0) && ms<=Number(l.endMs||0)) return i;
    if(ms>=Number(l.startMs||0)) best=i;
  }
  return best;
}

function renderKaraokeAt(ms){
  if(!karaokeDoc){return;}
  const lines=karaokeDoc.lines||[];
  const idx=lineIndexForTime(ms);
  const prev=idx>0?lines[idx-1]:null;
  const cur=idx>=0?lines[idx]:null;
  const next=idx+1<lines.length?lines[idx+1]:null;
  document.getElementById('karaokePrev').textContent=prev?prev.text:'';
  document.getElementById('karaokeNext').textContent=next?next.text:'';
  const box=document.getElementById('karaokeCurrent');
  if(!cur){box.textContent=karaokeDoc.title||'Karaoke';return;}
  if(cur.words&&cur.words.length){
    let html='';
    cur.words.forEach(w=>{
      const cls=ms>=Number(w.endMs||0)?'done':(ms>=Number(w.timeMs||0)?'active':'');
      html+='<span class="karaoke-word '+cls+'">'+kEsc(w.text)+'</span>';
    });
    box.innerHTML=html;
  }else{
    box.textContent=cur.text||'';
  }
}

function refreshKaraokeState(){
  fetch('/status.json',{credentials:'same-origin',cache:'no-store'})
    .then(r=>r.json()).then(st=>{
      if(typeof st.positionMs==='number'){
        if(Math.abs(st.positionMs-karaokeLastPosition)>60){
          karaokeLastPosition=st.positionMs;
          renderKaraokeAt(st.positionMs);
        }
      }
      if(st.karaokeActive && st.karaokeTitle){document.getElementById('karaokeTitle').textContent=st.karaokeTitle;}
    }).catch(()=>{});
}

function toggleFullscreenKaraoke(){
  const el=document.getElementById('karaokeStage');
  if(!document.fullscreenElement) el.requestFullscreen && el.requestFullscreen();
  else document.exitFullscreen && document.exitFullscreen();
}

document.addEventListener('DOMContentLoaded',function(){loadKaraokeList();setInterval(refreshKaraokeState,200);});
</script>
)rawliteral";

  html += pageFooter();
  server.send(200, "text/html; charset=utf-8", html);
}

// ============================================================
// /status.json + /games
// ============================================================

String audioModeText() {
  if (karaokeActive) return "karaoke";
  if (radioPlaying) return "radio";
  if (playlistActive) return "složka";
  if (audioPlaying || audioPaused) return "soubor";
  if (audioReady) return "připraveno";
  return "nepřipraveno";
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
  json += "\"rssi\":" + String(wifiOk ? WiFi.RSSI() : 0) + ",";
  json += "\"audioPlaying\":" + String(audioPlaying ? "true" : "false") + ",";
  json += "\"audioPaused\":" + String(audioPaused ? "true" : "false") + ",";
  json += "\"positionMs\":" + String(currentAudioPositionMs()) + ",";
  json += "\"audio\":\"" + jsonEscape(audioModeText()) + "\",";
  json += "\"audioDetail\":\"" + jsonEscape(audioStatus) + "\",";
  json += "\"audioOutput\":\"" + jsonEscape(String(audioOutputKindName())) + "\",";
  json += "\"karaokeActive\":" + String(karaokeActive ? "true" : "false") + ",";
  json += "\"karaokeJson\":\"" + jsonEscape(karaokeJsonPath) + "\",";
  json += "\"karaokeAudio\":\"" + jsonEscape(karaokeAudioPath) + "\",";
  json += "\"karaokeTitle\":\"" + jsonEscape(karaokeTitle) + "\",";
  json += "\"audioVolume\":" + String(cfg.audioVolume) + ",";
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

void appendGamesForDisk(String& html, const String& disk) {
  html += "<div class='card'>";
  html += "<h3>" + diskTitle(disk) + "</h3>";

  if (!diskAvailable(disk)) {
    html += "<p class='warn'>Disk není dostupný.</p>";
    html += "</div>";
    return;
  }

  if (!fsExistsGeneric(disk, "/games")) {
    html += "<p class='small'>Složka <code>/games</code> zatím neexistuje.</p>";
    html += "<form method='POST' action='/games/mkdir'>";
    html += "<input type='hidden' name='disk' value='" + disk + "'>";
    html += "<button type='submit'>Vytvořit /games</button>";
    html += "</form>";
    html += "</div>";
    return;
  }

  File root = fsOpenGeneric(disk, "/games", FILE_READ);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    html += "<p class='bad'>/games nejde otevřít jako složka.</p>";
    html += "</div>";
    return;
  }

  html += "<div class='game-grid'>";
  bool any = false;
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

    if (playable) {
      any = true;
      String url = "/game/" + disk + urlEncode(launchPath);
      html += "<div class='game-card'>";
      html += "<div class='title'>🎮 " + htmlEscape(name) + "</div>";
      html += "<div class='meta'>" + htmlEscape(typeText) + "<br>" + htmlEscape(full) + "</div>";
      html += "<a class='play' href='" + url + "'>Spustit</a>";
      html += "</div>";
    }

    file.close();
    file = root.openNextFile();
  }
  root.close();

  if (!any) {
    html += "<p class='small'>Žádná hra nenalezena. Vlož do <code>/games</code> buď jeden <code>.html</code> soubor, nebo složku s <code>index.html</code>.</p>";
  }

  html += "</div>";
  html += "<p class='small'>Správa souborů: <a href='/files?disk=" + disk + "&p=/games'>otevřít /games</a></p>";
  html += "</div>";
}

void handleGamesPage() {
  String html = pageHeader("Hry");
  html += "<h2>Hry</h2>";

  html += "<div class='card'>";
  html += "<p>HTML5 hry můžeš dát do interní FFat paměti nebo na USB do složky <code>/games</code>.</p>";
  html += "<p class='small'>Doporučená struktura: <code>/games/nazev_hry/index.html</code>. Fungují i single-file <code>.html</code> hry. Klasické Java applety/JAR už běžné prohlížeče nepodporují; pro web hry používej JavaScript/HTML5, případně WASM.</p>";
  html += "</div>";

  appendGamesForDisk(html, "ffat");
  appendGamesForDisk(html, "usb0");

  html += pageFooter();
  server.send(200, "text/html", html);
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
// Routes
// ============================================================

void setupRoutes() {
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Location", "/files");
    server.send(303);
  });

  server.on("/files", HTTP_GET, []() { if (checkWebAuth()) handleFilesPage(); });
  server.on("/radio", HTTP_GET, handleRadioPage);
  server.on("/radio/save", HTTP_POST, handleRadioSave);
  server.on("/radio/play", HTTP_POST, handleRadioPlay);
  server.on("/karaoke", HTTP_GET, handleKaraokePage);
  server.on("/karaoke/list.json", HTTP_GET, handleKaraokeListJson);
  server.on("/karaoke/play", HTTP_POST, handleKaraokePlay);
  server.on("/karaoke/play", HTTP_GET, handleKaraokePlay);
  server.on("/games", HTTP_GET, []() { if (checkWebAuth()) handleGamesPage(); });
  server.on("/games/mkdir", HTTP_POST, []() { if (checkWebAuth()) handleGamesMkdir(); });
  server.on("/status.json", HTTP_GET, []() { if (checkWebAuth()) handleStatusJson(); });
  server.on("/config", HTTP_GET, []() { if (checkWebAuth()) handleConfigPage(); });
  server.on("/config/save", HTTP_POST, []() { if (checkWebAuth()) handleConfigSave(); });
  server.on("/reboot", HTTP_POST, []() { if (checkWebAuth()) handleReboot(); });
  server.on("/update", HTTP_GET, []() { if (checkWebAuth()) handleUpdatePage(); });

  server.on("/raw", HTTP_GET, []() { if (checkWebAuth()) handleRawFile(); });
  server.on("/view", HTTP_GET, []() { if (checkWebAuth()) handleViewFile(); });
  server.on("/edit", HTTP_GET, []() { if (checkWebAuth()) handleEditFile(); });
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
  server.on("/audio/volume", HTTP_GET, handleAudioVolume);

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
    server.send(404, "text/plain", "Not found");
  });
}

// ============================================================
// Setup / loop
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("ESP32-S3 Web Disk");

  formatFfatOnBootIfRequested();

  Serial.println("Mounting FFat...");
  if (!FFat.begin(true)) {
    Serial.println("FFat mount/format failed!");
    return;
  }
  Serial.println("FFat mounted.");

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

  if (!FFat.exists(CONFIG_FILE)) {
    saveConfig();
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  startAp();
  connectStaIfConfigured();
  startMdns(true);

  Serial.print("AP SSID: ");
  Serial.println(cfg.apSsid);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

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
  if (!audioPlaying) {
    return;
  }

  if (radioPlaying) {
    if (!radioClient.connected() && radioClient.available() == 0 && audio.isEof()) {
      stopAudioPlayback("Radio dokončeno");
      radioStatus = "Radio dokončeno";
      return;
    }

    // Rádio pumpujeme adaptivně podle zaplnění PCM bufferu.
    // Čím menší zásoba framů, tím víc pumpnutí dekodéru v jednom průchodu.
    size_t af = audio.availableFrames();

    uint8_t pumps = 8;
    if (af < 1024) {
      pumps = 80;
    } else if (af < 2048) {
      pumps = 56;
    } else if (af < 4096) {
      pumps = 36;
    } else if (af < 8192) {
      pumps = 20;
    } else if (af < 12288) {
      pumps = 12;
    }

    for (uint8_t i = 0; i < pumps; i++) {
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
      audio.pump();
      audioPumpCount++;
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
    serviceAudioCommandQueue();
    serviceAudioPump();
    serviceI2sAudioOutput();
    updateMusicRgbLed();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void usbServiceTask(void *param) {
  (void)param;

  for (;;) {
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

void startBackgroundTasks() {
  if (!audioCommandQueue) {
    audioCommandQueue = xQueueCreate(AUDIO_COMMAND_QUEUE_LEN, sizeof(AudioCommand));
  }

  if (!audioCommandQueue) {
    Serial.println("Audio command queue create FAILED");
    audioStatus = "Audio frontu se nepodařilo vytvořit";
    return;
  }

  if (!audioTaskHandle) {
    BaseType_t ok = xTaskCreatePinnedToCore(
      audioServiceTask,
      "audioSvc",
      12288,
      nullptr,
      2,
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
      4096,
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
}

void loop() {
  server.handleClient();
  ftpHandle();

  if (!usbTaskHandle) {
    serviceUsbBootMountOnce();
  }

  serviceMdns();
  serviceRadioResume();
  serviceAudioVolumeSave();
  serviceEncoderControl();

  delay(1);
}
