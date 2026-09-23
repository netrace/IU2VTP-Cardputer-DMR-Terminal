#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <mbedtls/sha256.h>
#include <M5Cardputer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include "config.h"
#include "tx_ambe_encoder.h"
#include "dmr_ambe_mapping.h"
#include "rewind_tx_protocol.h"

// blip25 AMBE encoding uses substantially more stack than Arduino's default 8 KB loopTask.
// Espressif officially supports overriding the loop task stack this way.
SET_LOOP_TASK_STACK_SIZE(32 * 1024);

extern "C" {
#include <mbelib.h>

static constexpr const char* APP_NAME = "IU2VTP Cardputer DMR Terminal";
static constexpr const char* APP_VERSION = "1.1.0-alpha6";
static constexpr const char* APP_TITLE = "IU2VTP Cardputer DMR Terminal v1.1.0-alpha6";

static constexpr int APP_HEADER_H = 14;
static constexpr int APP_FOOTER_H = 24;
// Header helpers are defined here, before any UI code uses them.
static void drawAppHeader()
{
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, d.width(), APP_HEADER_H, TFT_DARKGREY);
    d.setTextColor(TFT_WHITE, TFT_DARKGREY);
    d.setTextSize(1);
    d.setCursor(3, 3);
    d.print(APP_TITLE);
}

static void clearContentArea()
{
    auto& d = M5Cardputer.Display;
    const int h = d.height() - APP_HEADER_H - APP_FOOTER_H;
    d.fillRect(0, APP_HEADER_H, d.width(), h, TFT_BLACK);
}

static void drawScreenBase()
{
    auto& d = M5Cardputer.Display;
    d.fillScreen(TFT_BLACK);
    drawAppHeader();
}

void mbe_checkGolayBlock(long int *block);
}

// ============================================================
// IU2VTP Cardputer DMR Terminal v1.1.0-alpha6
// Experimental PTT/TX branch.
// Alpha5 enables live group-voice TX only while P is held.
// ============================================================

static constexpr char REWIND_SIGN[] = "REWIND01";
static constexpr size_t HEADER_LEN = 18;

enum PacketType : uint16_t {
    PKT_KEEPALIVE       = 0x0000,
    PKT_CLOSE           = 0x0001,
    PKT_CHALLENGE       = 0x0002,
    PKT_AUTHENTICATION  = 0x0003,
    PKT_REDIRECTION     = 0x0008,
    PKT_REPORT          = 0x0100,
    PKT_BUSY_NOTICE     = 0x0200,
    PKT_CONFIGURATION   = 0x0900,
    PKT_SUBSCRIPTION    = 0x0901,
    PKT_CANCELLING      = 0x0902,
    PKT_DMR_HEADER_FLC  = 0x0911,
    PKT_DMR_TERMINATOR  = 0x0912,
    PKT_DMR_AUDIO       = 0x0920,
    PKT_DMR_EMBEDDED    = 0x0927,
    PKT_SUPERHEADER     = 0x0928,
    PKT_FAILURE         = 0x0929
};

static constexpr uint8_t SERVICE_OPEN_TERMINAL = 0x21;
static constexpr uint32_t SESSION_PRIVATE_VOICE = 5;
static constexpr uint32_t SESSION_GROUP_VOICE = 7;

enum class State {
    WAIT_CHALLENGE,
    WAIT_AUTH_ACK,
    AUTHENTICATED,
    WAIT_SUB_ACK,
    SUBSCRIBED
};

// Experimental TX/PTT state. Alpha1 deliberately does not send DMR voice.
enum class TxState {
    IDLE,
    PTT_HELD_CAPTURE
};

TxState txState = TxState::IDLE;
unsigned long txStateStartedMs = 0;
bool pttWasDown = false;

static constexpr uint32_t TX_MIC_RATE = 16000;
static constexpr size_t TX_MIC_SAMPLES_16K = 320; // 20 ms
static constexpr size_t TX_PCM_SAMPLES_8K = 160;  // 20 ms

struct TxPcmFrame {
    int16_t pcm[TX_PCM_SAMPLES_8K];
};

static int16_t txMicBuffers[2][TX_MIC_SAMPLES_16K];
static QueueHandle_t txPcmQueue = nullptr;
static volatile uint32_t txMicFramesCaptured = 0;
static volatile uint32_t txPcmFramesQueued = 0;
static volatile uint32_t txPcmFramesConsumed = 0;
static volatile uint32_t txPcmQueueDrops = 0;
static volatile uint32_t txMicRecordFailures = 0;
static volatile uint32_t txMicPeak = 0;
static volatile uint32_t txAmbeFramesEncoded = 0;
static volatile uint32_t txAmbeEncodeFailures = 0;
static volatile uint32_t txDmrFramesInterleaved = 0;
static volatile uint32_t txDmrPacketsBuilt = 0;
static uint8_t txDmrPacketBuild[27] = {0};
static uint8_t txDmrFrameIndex = 0;
static uint8_t txLastDmrPayload[27] = {0};

static bool txCaptureActive()
{
    return txState == TxState::PTT_HELD_CAPTURE;
}

WiFiUDP udp;
bool udpStarted = false;
IPAddress bmIP;

State state = State::WAIT_CHALLENGE;

uint32_t seqNo = 0;
uint32_t realtimeSeqNo = 0;

static constexpr uint16_t REWIND_FLAG_REAL_TIME_1 = 0x0001;
static constexpr unsigned long TX_MAX_MS = 180000UL;
static constexpr unsigned long TX_PACKET_PERIOD_US = 60000UL;

struct TxDmrPacket {
    uint8_t payload[27];
};

static QueueHandle_t txDmrPacketQueue = nullptr;
static volatile uint32_t txNetworkPacketsSent = 0;
static volatile uint32_t txNetworkPacketDrops = 0;
static volatile uint32_t txNetworkErrors = 0;
static uint32_t txNextPacketDueUs = 0;
static bool txSessionAnnounced = false;

enum class TxCallMode {
    GROUP,
    PRIVATE
};

static TxCallMode txCallMode = TxCallMode::GROUP;
static uint32_t txPrivateId = 0;

extern uint32_t activeTG;

static const char* txCallModeLabel()
{
    return txCallMode == TxCallMode::PRIVATE ? "PRIVATE" : "GROUP";
}

static uint32_t txDestinationId()
{
    return txCallMode == TxCallMode::PRIVATE ? txPrivateId : activeTG;
}

static uint32_t txSessionType()
{
    return txCallMode == TxCallMode::PRIVATE
        ? SESSION_PRIVATE_VOICE
        : SESSION_GROUP_VOICE;
}

// ============================================================
// Persistent runtime configuration
// ============================================================
static constexpr uint16_t DMR_DEFAULT_PORT = 54006;
static constexpr uint8_t MAX_PROFILES = 6;
static constexpr uint8_t MAX_FAVORITE_TGS = 16;

static constexpr uint8_t MAX_WIFI_SCAN = 20;

struct WifiConfig {
    char ssid[33] = "";
    char password[65] = "";
};

WifiConfig wifiConfig;

struct WifiScanEntry {
    String ssid;
    int32_t rssi = -127;
    bool open = false;
};

WifiScanEntry wifiScan[MAX_WIFI_SCAN];
uint8_t wifiScanCount = 0;
uint8_t wifiMenuIndex = 0;
bool wifiScanning = false;

bool wifiHomeDirty = true;
bool wifiScanDirty = true;

bool wifiEverConnected = false;
unsigned long lastWifiReconnectAttempt = 0;

struct DmrProfile {
    char name[18] = "";
    char host[64] = "";
    uint16_t port = DMR_DEFAULT_PORT;
    uint32_t radioId = 0;
    char password[48] = "";
    bool enabled = true;
};

Preferences prefs;
DmrProfile profiles[MAX_PROFILES];
uint8_t profileCount = 0;
uint8_t activeProfileIndex = 0;

uint32_t favoriteTGs[MAX_FAVORITE_TGS] = {222};
uint8_t favoriteTGCount = 1;
uint8_t activeTGIndex = 0;
uint32_t activeTG = 222;

uint8_t speakerVolume = 170;  // 0..255, persisted in NVS

enum class UiMode {
    MAIN,
    TG_INPUT,
    CONFIG_HOME,
    CONFIG_PROFILES,
    CONFIG_PROFILE_EDIT,
    CONFIG_TGS,
    WIFI_HOME,
    WIFI_SCAN,
    WIFI_PASSWORD,
    AUDIO_VOLUME,
    TEXT_INPUT
};

UiMode uiMode = UiMode::MAIN;

// Central UI navigation state.
// Every screen transition goes through navigateTo()/navigateBack().
static constexpr uint8_t NAV_STACK_MAX = 8;
UiMode navStack[NAV_STACK_MAX];
uint8_t navDepth = 0;
UiMode inputReturnMode = UiMode::MAIN;
bool rxUiDirty = true;

int menuIndex = 0;
int editProfileIndex = 0;
int editFieldIndex = 0;
String inputBuffer;
bool inputNumericOnly = false;
bool inputSecret = false;
String inputTitle;
int inputTarget = 0;

bool setupFlowActive = false;   // first-run guided flow: Wi-Fi -> DMR -> TG -> RX
String uiNotice = "";
unsigned long uiNoticeUntil = 0;

static const char* profileName() {
    return (profileCount && activeProfileIndex < profileCount)
        ? profiles[activeProfileIndex].name : "No profile";
}
static const char* profileHost() {
    return (profileCount && activeProfileIndex < profileCount)
        ? profiles[activeProfileIndex].host : "";
}
static uint32_t profileRadioId() {
    return (profileCount && activeProfileIndex < profileCount)
        ? profiles[activeProfileIndex].radioId : 0;
}
static const char* profilePassword() {
    return (profileCount && activeProfileIndex < profileCount)
        ? profiles[activeProfileIndex].password : "";
}
static uint16_t profilePort() {
    return (profileCount && activeProfileIndex < profileCount)
        ? profiles[activeProfileIndex].port : DMR_DEFAULT_PORT;
}

unsigned long lastKeepaliveTx = 0;
unsigned long lastKeepaliveAck = 0;
unsigned long lastSubscribeTx = 0;

uint32_t audioPackets = 0;
uint32_t ambeFrames = 0;

// Audio path: ODTP loop pushes 27-byte AMBE packets into a queue.
// A dedicated task owns mbelib state and the PCM buffer.
struct AudioPacket {
    uint8_t ambe[27];
};

static constexpr size_t AUDIO_QUEUE_DEPTH = 160;
QueueHandle_t audioQueue = nullptr;
TaskHandle_t audioTaskHandle = nullptr;

volatile uint32_t audioQueueDrops = 0;
volatile uint32_t audioPacketsPlayed = 0;
volatile uint32_t audioFramesDecoded = 0;
volatile uint32_t decodePacketsTimed = 0;
volatile uint64_t decodeMicrosTotal = 0;
volatile uint32_t decodeMicrosMax = 0;
volatile uint32_t pcmClipSamples = 0;

volatile uint32_t pcmPeakSeen = 0;

// ------------------------------------------------------------
// Live call / display state
// ------------------------------------------------------------
struct CallInfo {
    bool active = false;
    bool metadataValid = false;
    bool privateCall = false;
    uint8_t flcByte1 = 0;
    uint8_t feature = 0;
    uint8_t service = 0;
    uint16_t rewindFlags = 0;
    uint32_t source = 0;
    uint32_t destination = 222;
    uint32_t startMs = 0;
    uint32_t endMs = 0;
    uint32_t packets = 0;
    uint32_t lastSeq = 0;
    char callsign[20] = "";
    char targetCall[20] = "";
    char name[32] = "";
    char location[48] = "";
};

static CallInfo callInfo;
static SemaphoreHandle_t callInfoMutex = nullptr;

static volatile uint32_t lookupRequestedId = 0;
static volatile bool lookupPending = false;
static TaskHandle_t lookupTaskHandle = nullptr;

static unsigned long lastUiDraw = 0;
static unsigned long lastVoicePacketMs = 0;


static bool ensureUdpStarted();

static void normalizeActiveProfile();
static void drawScreenChrome(const String& title,
                             const String& footer1,
                             const String& footer2);
static void drawFooter(const String& line1,
                       const String& line2);

static void resetTransientInputState();
static void renderCurrentScreen();
static void navigateTo(UiMode target, bool pushCurrent = true);
static void navigateBack(UiMode fallback = UiMode::MAIN);
static void resetNavigation(UiMode target);

static void drawConfigHome();
static void drawProfilesMenu();
static void drawProfileEditor();
static void drawTgMenu();
static void drawWifiHome();
static void drawWifiScan();
static void drawVolumeMenu();
static void drawInputBox();
static void abortTxSession(const char* reason);

// ------------------------------------------------------------
// Persistent settings
// ------------------------------------------------------------
static void makePresetProfiles()
{
    memset(profiles, 0, sizeof(profiles));
    profileCount = 2;

    snprintf(profiles[0].name, sizeof(profiles[0].name), "BrandMeister");
    snprintf(profiles[0].host, sizeof(profiles[0].host),
             "2222.master.brandmeister.network");
    profiles[0].port = DMR_DEFAULT_PORT;
    profiles[0].enabled = true;

    snprintf(profiles[1].name, sizeof(profiles[1].name), "HamThings");
    snprintf(profiles[1].host, sizeof(profiles[1].host),
             "2221.master.hamthings.it");
    profiles[1].port = DMR_DEFAULT_PORT;
    profiles[1].enabled = true;

    activeProfileIndex = 0;
    favoriteTGCount = 1;
    favoriteTGs[0] = 222;
    activeTGIndex = 0;
    activeTG = 222;
}

static void saveSettings()
{
    prefs.begin("dmrrx", false);
    prefs.putUChar("pcount", profileCount);
    prefs.putUChar("pactive", activeProfileIndex);
    prefs.putBytes("profiles", profiles, sizeof(profiles));
    prefs.putUChar("tgcount", favoriteTGCount);
    prefs.putUChar("tgactive", activeTGIndex);
    prefs.putUInt("currenttg", activeTG);
    prefs.putBytes("tgs", favoriteTGs, sizeof(favoriteTGs));
    prefs.putBytes("wifi", &wifiConfig, sizeof(wifiConfig));
    prefs.putUChar("volume", speakerVolume);
    prefs.end();
}

static void loadSettings()
{
    prefs.begin("dmrrx", true);
    bool initialized = prefs.getBool("init", false);
    prefs.end();

    if (!initialized) {
        makePresetProfiles();
        prefs.begin("dmrrx", false);
        prefs.putBool("init", true);
        prefs.end();
        saveSettings();
        return;
    }

    prefs.begin("dmrrx", true);
    profileCount = prefs.getUChar("pcount", 0);
    activeProfileIndex = prefs.getUChar("pactive", 0);
    prefs.getBytes("profiles", profiles, sizeof(profiles));
    favoriteTGCount = prefs.getUChar("tgcount", 0);
    activeTGIndex = prefs.getUChar("tgactive", 0);
    uint32_t persistedCurrentTG = prefs.getUInt("currenttg", 0);
    prefs.getBytes("tgs", favoriteTGs, sizeof(favoriteTGs));
    prefs.getBytes("wifi", &wifiConfig, sizeof(wifiConfig));
    speakerVolume = prefs.getUChar("volume", 170);
    prefs.end();

    if (profileCount == 0 || profileCount > MAX_PROFILES) {
        makePresetProfiles();
        saveSettings();
    }
    normalizeActiveProfile();

    if (favoriteTGCount == 0 || favoriteTGCount > MAX_FAVORITE_TGS) {
        favoriteTGCount = 1;
        favoriteTGs[0] = 222;
        activeTGIndex = 0;
    }
    if (activeTGIndex >= favoriteTGCount) activeTGIndex = 0;

    // currenttg was added in v1.0.3. For old NVS data, migrate from the
    // previously selected favorite TG once, then persist the explicit value.
    if (persistedCurrentTG > 0 && persistedCurrentTG <= 0xFFFFFF) {
        activeTG = persistedCurrentTG;
    } else {
        activeTG = favoriteTGs[activeTGIndex];
        saveSettings();
    }
}

static bool profileReady(uint8_t index)
{
    if (profileCount == 0 || index >= profileCount) return false;
    const DmrProfile& p = profiles[index];
    return p.enabled && p.host[0] && p.radioId > 0 && p.password[0];
}

static bool activeProfileReady()
{
    return profileReady(activeProfileIndex);
}

static int firstReadyProfile()
{
    for (uint8_t i = 0; i < profileCount; ++i) {
        if (profileReady(i)) return i;
    }
    return -1;
}

static void normalizeActiveProfile()
{
    if (profileCount == 0) {
        activeProfileIndex = 0;
        return;
    }

    if (activeProfileIndex < profileCount && profileReady(activeProfileIndex))
        return;

    int ready = firstReadyProfile();
    if (ready >= 0) {
        activeProfileIndex = (uint8_t)ready;
        return;
    }

    if (activeProfileIndex >= profileCount)
        activeProfileIndex = 0;
}

static void deleteProfileAt(uint8_t index)
{
    if (profileCount <= 1 || index >= profileCount) return;

    const uint8_t oldActive = activeProfileIndex;

    for (uint8_t i = index; i + 1 < profileCount; ++i)
        profiles[i] = profiles[i + 1];

    memset(&profiles[profileCount - 1], 0, sizeof(DmrProfile));
    profileCount--;

    if (oldActive == index) {
        // Deleted the active profile: choose another complete profile if possible.
        activeProfileIndex = 0;
        normalizeActiveProfile();
    } else if (oldActive > index) {
        // Same logical profile shifted one position left.
        activeProfileIndex = oldActive - 1;
    } else {
        activeProfileIndex = oldActive;
    }

    if (activeProfileIndex >= profileCount)
        activeProfileIndex = 0;

    saveSettings();
}

static bool dmrSessionReady()
{
    return activeProfileReady() && state == State::SUBSCRIBED;
}

static bool dmrTransportConnected()
{
    if (!activeProfileReady() || WiFi.status() != WL_CONNECTED)
        return false;

    return state == State::AUTHENTICATED ||
           state == State::WAIT_SUB_ACK ||
           state == State::SUBSCRIBED;
}

static bool dmrTalkgroupChanging()
{
    return activeProfileReady() && state == State::WAIT_SUB_ACK;
}

static void setUiNotice(const String& msg, unsigned long ms = 2500)
{
    uiNotice = msg;
    uiNoticeUntil = millis() + ms;
}

static void resetCallInfo()
{
    if (callInfoMutex && xSemaphoreTake(callInfoMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        CallInfo fresh;
        fresh.destination = activeTG;
        callInfo = fresh;
        xSemaphoreGive(callInfoMutex);
    }
}

// ------------------------------------------------------------
// Helpers
// ------------------------------------------------------------

static inline void put16le(uint8_t* p, uint16_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
}

static inline void put32le(uint8_t* p, uint32_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

static inline uint16_t get16le(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t get32le(const uint8_t* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

void printHex(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (p[i] < 16) Serial.print('0');
        Serial.print(p[i], HEX);
        if (i + 1 < n) Serial.print(' ');
    }
    Serial.println();
}


static inline uint32_t get24be(const uint8_t* p) {
    return ((uint32_t)p[0] << 16) |
           ((uint32_t)p[1] << 8)  |
           ((uint32_t)p[2]);
}


static void copyRewindCallsign(char* dst, size_t dstLen, const uint8_t* src10)
{
    if (!dst || dstLen == 0) return;

    size_t n = 0;
    while (n < 10 && n + 1 < dstLen) {
        uint8_t c = src10[n];
        if (c == 0) break;
        dst[n] = (char)c;
        ++n;
    }

    while (n > 0 && dst[n - 1] == ' ') --n;
    dst[n] = 0;
}

static String jsonStringField(const String& json, const char* key)
{
    String needle = String("\"") + key + "\":";
    int p = json.indexOf(needle);
    if (p < 0) return "";
    p += needle.length();

    while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\t')) ++p;
    if (p >= (int)json.length() || json[p] != '"') return "";
    ++p;

    String out;
    bool esc = false;
    for (; p < (int)json.length(); ++p) {
        char c = json[p];
        if (esc) {
            out += c;
            esc = false;
        } else if (c == '\\') {
            esc = true;
        } else if (c == '"') {
            break;
        } else {
            out += c;
        }
    }
    return out;
}

static void setLookupResult(uint32_t id, const String& call,
                            const String& name, const String& loc)
{
    if (!callInfoMutex) return;
    if (xSemaphoreTake(callInfoMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (callInfo.source == id) {
            snprintf(callInfo.callsign, sizeof(callInfo.callsign), "%s", call.c_str());
            snprintf(callInfo.name, sizeof(callInfo.name), "%s", name.c_str());
            snprintf(callInfo.location, sizeof(callInfo.location), "%s", loc.c_str());
        }
        xSemaphoreGive(callInfoMutex);
    }
}

static void callsignLookupTask(void* param)
{
    (void)param;

    for (;;) {
        if (!lookupPending || lookupRequestedId == 0 || WiFi.status() != WL_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        uint32_t id = lookupRequestedId;
        lookupPending = false;

        WiFiClientSecure secure;
        secure.setInsecure();
        HTTPClient http;

        String url = "https://database.radioid.net/api/dmr/user/?id=" + String(id);
        String call, name, loc;

        if (http.begin(secure, url)) {
            http.setConnectTimeout(2500);
            http.setTimeout(3000);
            int code = http.GET();

            if (code == HTTP_CODE_OK) {
                String body = http.getString();

                call = jsonStringField(body, "callsign");
                String first = jsonStringField(body, "fname");
                String last  = jsonStringField(body, "surname");
                String city  = jsonStringField(body, "city");
                String state = jsonStringField(body, "state");
                String country = jsonStringField(body, "country");

                name = first;
                if (last.length()) {
                    if (name.length()) name += " ";
                    name += last;
                }

                loc = city;
                if (state.length()) {
                    if (loc.length()) loc += ", ";
                    loc += state;
                }
                if (country.length()) {
                    if (loc.length()) loc += " - ";
                    loc += country;
                }
            }
            http.end();
        }

        if (!call.length()) call = String("DMR ") + String(id);
        setLookupResult(id, call, name, loc);
    }
}

static void requestCallsignLookup(uint32_t id)
{
    static uint32_t lastId = 0;
    if (!id) return;

    if (id == lastId) return;
    lastId = id;

    lookupRequestedId = id;
    lookupPending = true;
}

static String shortState()
{
    if (!activeProfileReady()) return "NO DMR";

    switch (state) {
        case State::WAIT_CHALLENGE: return "LOGIN";
        case State::WAIT_AUTH_ACK:  return "AUTH";
        case State::AUTHENTICATED:  return "AUTH OK";
        case State::WAIT_SUB_ACK:   return "TG...";
        case State::SUBSCRIBED:     return "READY";
    }
    return "?";
}

static void drawTextRegion(int x, int y, int w, int h,
                           uint16_t bg, uint16_t fg,
                           uint8_t textSize, const String& s)
{
    auto& d = M5Cardputer.Display;
    d.fillRect(x, y, w, h, bg);
    d.setTextColor(fg, bg);
    d.setTextSize(textSize);
    d.setCursor(x + 2, y + 2);
    d.print(s);
}

static void drawUi()
{
    static CallInfo lastCi;
    static bool haveLast = false;
    static String lastState;
    static int lastRssi = -999;
    static uint32_t lastDurationSec = 0xFFFFFFFF;
    static uint32_t lastQueue = 0xFFFFFFFF;
    static uint32_t lastDrops = 0xFFFFFFFF;
    static unsigned long lastStatusRefresh = 0;

    unsigned long now = millis();
    if (now - lastUiDraw < 100) return;
    lastUiDraw = now;

    CallInfo ci;
    if (callInfoMutex && xSemaphoreTake(callInfoMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        ci = callInfo;
        xSemaphoreGive(callInfoMutex);
    }

    auto& d = M5Cardputer.Display;
    const int W = d.width();
    const int H = d.height();

    String st = shortState();

    if (rxUiDirty) {
        haveLast = false;
        lastState = "";
        lastRssi = -999;
        lastDurationSec = 0xFFFFFFFF;
        lastQueue = 0xFFFFFFFF;
        lastDrops = 0xFFFFFFFF;
        lastStatusRefresh = 0;
        rxUiDirty = false;
    }

    const bool stateChanged = !haveLast || st != lastState;
    const bool txActive = txCaptureActive();

    // Redraw the RX body not only for call metadata changes, but also when
    // the ODTP/DMR state changes. Otherwise old text such as
    // "Connecting to DMR..." can remain visible after SUBSCRIPTION ACK.
    bool metadataChanged =
        !haveLast ||
        stateChanged ||
        ci.active != lastCi.active ||
        ci.metadataValid != lastCi.metadataValid ||
        ci.privateCall != lastCi.privateCall ||
        ci.source != lastCi.source ||
        ci.destination != lastCi.destination ||
        ci.feature != lastCi.feature ||
        ci.service != lastCi.service ||
        strcmp(ci.callsign, lastCi.callsign) != 0 ||
        strcmp(ci.targetCall, lastCi.targetCall) != 0 ||
        strcmp(ci.name, lastCi.name) != 0 ||
        strcmp(ci.location, lastCi.location) != 0;

    int rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -127;
    bool headerChanged = stateChanged || abs(rssi - lastRssi) >= 2 ||
                         ci.active != lastCi.active || txActive;

    if (!haveLast) {
        drawScreenBase();     // ONCE, not every 100-150 ms
        drawFooter("UP/DOWN=TG  LEFT/RIGHT=SERVER",
                   "G0=PTT  C=CALL  I=PRIVATE ID");
    }

    if (headerChanged) {
        uint16_t bg = txActive ? TFT_RED : (ci.active ? TFT_DARKGREEN : TFT_DARKGREY);
        d.fillRect(0, APP_HEADER_H, W, 20, bg);
        d.setTextColor(TFT_WHITE, bg);
        d.setTextSize(1);
        d.setCursor(5, APP_HEADER_H + 6);
        d.printf("%s %s", profileName(), txActive ? "TX" : st.c_str());
        d.setCursor(W - 92, APP_HEADER_H + 6);
        d.printf("WiFi %ddBm ", rssi);
        lastState = st;
        lastRssi = rssi;
    }

    if (metadataChanged) {
        // TG / destination
        String tg = "TG ";
        tg += String(ci.destination ? ci.destination : activeTG);
        drawTextRegion(4, APP_HEADER_H + 23, 160, 24, TFT_BLACK, TFT_CYAN, 2, tg);

        drawTextRegion(W - 74, APP_HEADER_H + 24, 72, 18, TFT_BLACK,
                       ci.privateCall ? TFT_ORANGE : TFT_GREEN, 1,
                       ci.privateCall ? "PRIVATE" : "GROUP");

        // Speaker / callsign
        String speaker;
        if (txActive) {
            speaker = txCallMode == TxCallMode::PRIVATE
                ? "PRIVATE TRANSMITTING"
                : "GROUP TRANSMITTING";
        } else if (txCallMode == TxCallMode::PRIVATE) {
            speaker = "PRIVATE ID ";
            speaker += String(txPrivateId);
        } else if (ci.metadataValid) {
            if (strlen(ci.callsign)) speaker = ci.callsign;
            else if (ci.source) speaker = String(ci.source);
            else speaker = "Metadata...";
        } else if (ci.active) {
            speaker = "RX audio...";
        } else {
            if (dmrTalkgroupChanging())
                speaker = "Switching TG...";
            else if (dmrTransportConnected())
                speaker = "Listening";
            else
                speaker = "Connecting to DMR...";
        }
        drawTextRegion(4, APP_HEADER_H + 49, W - 8, 23, TFT_BLACK, TFT_WHITE, 2, speaker);

        // DMR ID: never show "DMR ID: 0"
        String idline;
        if (txActive) {
            idline = "PCM -> AMBE -> DMR / no TX";
        } else if (ci.metadataValid && ci.source) {
            idline = "DMR ID: ";
            idline += String(ci.source);
        } else if (ci.active) {
            idline = "DMR ID: waiting for metadata";
        } else if (ci.source) {
            idline = "Last DMR ID: ";
            idline += String(ci.source);
        } else {
            if (dmrSessionReady()) {
                idline = "Subscribed TG ";
                idline += String(activeTG);
            } else if (dmrTalkgroupChanging()) {
                idline = "Server connected - TG ";
                idline += String(activeTG);
                idline += "...";
            } else {
                idline = "Server: ";
                idline += profileName();
            }
        }
        drawTextRegion(5, APP_HEADER_H + 73, W - 10, 13, TFT_BLACK, TFT_LIGHTGREY, 1, idline);

        String line2;
        if (txActive) {
            line2 = "Mic ";
            line2 += String(txMicPeak);
            line2 += " pk  q:";
            line2 += String(txPcmQueue ? uxQueueMessagesWaiting(txPcmQueue) : 0);
            line2 += " d:";
            line2 += String(txPcmQueueDrops);
            if (txAmbeEncoderAvailable()) {
                line2 += " a:";
                line2 += String(txAmbeFramesEncoded);
                line2 += " p:";
                line2 += String(txDmrPacketsBuilt);
            }
        } else if (ci.metadataValid && strlen(ci.name)) {
            line2 = ci.name;
        } else if (ci.metadataValid && strlen(ci.targetCall)) {
            line2 = "Dest: ";
            line2 += ci.targetCall;
        } else if (ci.metadataValid) {
            line2 = "FLC feat:";
            line2 += String(ci.feature);
            line2 += " svc:";
            line2 += String(ci.service);
        } else {
            if (dmrTalkgroupChanging())
                line2 = "Waiting for TG confirmation";
            else if (dmrSessionReady())
                line2 = "DMR audio ready";
            else if (dmrTransportConnected())
                line2 = "DMR server connected";
            else
                line2 = String(profileHost());
        }
        drawTextRegion(5, APP_HEADER_H + 86, W - 10, 13, TFT_BLACK, TFT_LIGHTGREY, 1, line2);

    }

    uint32_t durationSec = ci.active ? (now - ci.startMs) / 1000 : 0;
    uint32_t q = audioQueue ? uxQueueMessagesWaiting(audioQueue) : 0;
    uint32_t drops = audioQueueDrops;

    // Compact runtime status lives above the shared footer.
    if (!haveLast ||
        now - lastStatusRefresh >= 1000 ||
        durationSec != lastDurationSec ||
        q != lastQueue ||
        drops != lastDrops) {

        String runtime;
        if (txActive) {
            char tmp[64];
            unsigned long txSec = (now - txStateStartedMs) / 1000;
            if (txCallMode == TxCallMode::PRIVATE)
                snprintf(tmp, sizeof(tmp), "TX %lus  ID %lu",
                         txSec, (unsigned long)txPrivateId);
            else
                snprintf(tmp, sizeof(tmp), "TX %lus  TG %lu",
                         txSec, (unsigned long)activeTG);
            runtime = tmp;
        } else if (ci.active) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "RX %02lu:%02lu pkt:%lu q:%lu",
                     (unsigned long)(durationSec / 60),
                     (unsigned long)(durationSec % 60),
                     (unsigned long)ci.packets,
                     (unsigned long)q);
            runtime = tmp;
        } else {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "%s  TG %lu  q:%lu",
                     dmrSessionReady() ? "READY" :
                     dmrTalkgroupChanging() ? "TG..." :
                     dmrTransportConnected() ? "DMR OK" : "LOGIN",
                     (unsigned long)activeTG,
                     (unsigned long)q);
            runtime = tmp;
        }

        // Footer starts at H-APP_FOOTER_H. Keep this status immediately above it.
        drawTextRegion(4, H - APP_FOOTER_H - 12, W - 8, 11, TFT_BLACK,
                       ci.active ? TFT_YELLOW : TFT_LIGHTGREY, 1, runtime);

        lastStatusRefresh = now;
        lastDurationSec = durationSec;
        lastQueue = q;
        lastDrops = drops;
    }

    lastCi = ci;
    haveLast = true;
}

// ------------------------------------------------------------
// Tiny Golay decoder helpers
//
// We avoid pulling OpenDMR/mbelib into the image.
// This is intentionally tiny and only for diagnostic validation.
// ------------------------------------------------------------

// Exact Golay(23,12) decode from classic mbelib.
// This uses mbelib's golayGenerator[] + golayMatrix[] implementation.
static bool golay23127Decode(uint32_t code, uint32_t &data12, int &corrected)
{
    long int block = (long int)(code & 0x7FFFFFU);

    // mbe_checkGolayBlock replaces the 23-bit codeword with the corrected
    // 12-bit information word.
    mbe_checkGolayBlock(&block);

    data12 = ((uint32_t)block) & 0x0FFFU;

    // mbelib's helper does not return an error count. For this path we only
    // need the corrected information bits, so report 0 as "decoded".
    corrected = 0;
    return true;
}

static bool golay24128Decode(uint32_t code24, uint32_t &data12, int &corrected)
{
    // Same semantics used by OpenDMR:
    // Golay(24,12) decode is Golay(23,12) on code >> 1.
    return golay23127Decode((code24 >> 1) & 0x7FFFFFU, data12, corrected);
}

static uint32_t computePrngMask23(uint32_t aOrig)
{
    uint16_t pr[24];
    pr[0] = static_cast<uint16_t>(16U * aOrig);

    for (int i = 1; i < 24; ++i) {
        pr[i] = static_cast<uint16_t>(
            (173U * static_cast<uint32_t>(pr[i - 1]) + 13849U) % 65536U
        );
    }

    for (int i = 1; i < 24; ++i) {
        pr[i] /= 32768U;
    }

    uint32_t mask = 0;
    for (int i = 1; i <= 23; ++i) {
        if (pr[i]) {
            mask |= (1U << (23 - i));
        }
    }
    return mask;
}

struct AmbeParams49 {
    uint8_t bits[49];
    int correctedA;
    int correctedB;
    bool okA;
    bool okB;
};

static AmbeParams49 decodeAmbe72To49(const uint8_t frame72[9])
{
    AmbeParams49 out{};
    memset(out.bits, 0, sizeof(out.bits));

    uint32_t a = 0;
    for (int i = 0; i < 24; ++i) {
        int byte_idx = i / 8;
        int bit_pos = 7 - (i % 8);
        if ((frame72[byte_idx] >> bit_pos) & 1U) {
            a |= (0x800000U >> i);
        }
    }

    uint32_t b = 0;
    for (int i = 0; i < 23; ++i) {
        int pos = 24 + i;
        int byte_idx = pos / 8;
        int bit_pos = 7 - (pos % 8);
        if ((frame72[byte_idx] >> bit_pos) & 1U) {
            b |= (0x400000U >> i);
        }
    }

    uint32_t c = 0;
    for (int i = 0; i < 25; ++i) {
        int pos = 47 + i;
        int byte_idx = pos / 8;
        int bit_pos = 7 - (pos % 8);
        if ((frame72[byte_idx] >> bit_pos) & 1U) {
            c |= (0x1000000U >> i);
        }
    }

    uint32_t aOrig = 0;
    out.okA = golay24128Decode(a, aOrig, out.correctedA);

    uint32_t prng = computePrngMask23(aOrig);
    uint32_t bDescrambled = b ^ prng;

    uint32_t bOrig = 0;
    out.okB = golay23127Decode(bDescrambled, bOrig, out.correctedB);

    for (int i = 0; i < 12; ++i) {
        out.bits[i] = (aOrig >> (11 - i)) & 1U;
    }

    for (int i = 0; i < 12; ++i) {
        out.bits[12 + i] = (bOrig >> (11 - i)) & 1U;
    }

    for (int i = 0; i < 25; ++i) {
        out.bits[24 + i] = (c >> (24 - i)) & 1U;
    }

    return out;
}

void printAmbe49(const AmbeParams49 &p)
{
    Serial.printf("[AMBE49] A=%s corr=%d | B=%s corr=%d | bits=",
                  p.okA ? "OK" : "FAIL",
                  p.correctedA,
                  p.okB ? "OK" : "FAIL",
                  p.correctedB);

    for (int i = 0; i < 49; ++i) {
        Serial.print(p.bits[i] ? '1' : '0');
    }
    Serial.println();
}



static void audioTask(void *param)
{
    (void)param;

    mbe_parms mbeCur;
    mbe_parms mbePrev;
    mbe_parms mbePrevEnhanced;
    mbe_initMbeParms(&mbeCur, &mbePrev, &mbePrevEnhanced);

    static constexpr int PCM_PER_PACKET = 480;      // 60 ms @ 8 kHz
    static constexpr int PREBUFFER_PACKETS = 34;    // ~2.0 s
    static constexpr int BATCH_PACKETS = 10;        // 600 ms
    static constexpr int PCM_BATCH_SAMPLES =
        BATCH_PACKETS * PCM_PER_PACKET;
    static constexpr int OUTPUT_DIVISOR = 4;

    static int16_t pcmBuffers[3][PCM_BATCH_SAMPLES];
    uint8_t outIndex = 0;
    int batchCount = 0;

    Serial.println("[AUDIO] DMR DEINTERLEAVE ENABLED");
    Serial.println("[AUDIO] 9 byte -> rW/rX/rY/rZ -> mbelib");
    Serial.println("[AUDIO] native 8 kHz / 16-bit / mono");
    Serial.printf("[AUDIO] prebuffer=%d packet (~%d ms)\n",
                  PREBUFFER_PACKETS, PREBUFFER_PACKETS * 60);

    Serial.println("[AUDIO] waiting for prebuffer...");
    while ((int)uxQueueMessagesWaiting(audioQueue) < PREBUFFER_PACKETS) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    Serial.println("[AUDIO] prebuffer ready");

    for (;;) {
        AudioPacket pkt;
        if (xQueueReceive(audioQueue, &pkt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        int16_t *pcm = pcmBuffers[outIndex];
        const int packetBase = batchCount * PCM_PER_PACKET;
        uint32_t t0 = micros();

        for (int frame = 0; frame < 3; ++frame) {
            const uint8_t *frame9 = pkt.ambe + frame * 9;
            int16_t *dst160 = pcm + packetBase + frame * 160;

            char ambe_fr[4][24];
            char ambe_d[49];
            memset(ambe_d, 0, sizeof(ambe_d));

            // Critical correction: DMR-specific deinterleave.
            dmrInterleaved72ToMbelib(frame9, ambe_fr);

            int errs = 0;
            int errs2 = 0;
            char errStr[64] = {0};

            mbe_processAmbe3600x2450Frame(
                dst160,
                &errs,
                &errs2,
                errStr,
                ambe_fr,
                ambe_d,
                &mbeCur,
                &mbePrev,
                &mbePrevEnhanced,
                1
            );

            for (int i = 0; i < 160; ++i) {
                int32_t v = dst160[i];
                uint32_t a = (v < 0) ? (uint32_t)(-v) : (uint32_t)v;
                if (a > pcmPeakSeen) pcmPeakSeen = a;
                if (a >= 32700U) ++pcmClipSamples;
                dst160[i] = (int16_t)(v / OUTPUT_DIVISOR);
            }

            ++audioFramesDecoded;
        }

        uint32_t dt = micros() - t0;
        ++decodePacketsTimed;
        decodeMicrosTotal += dt;
        if (dt > decodeMicrosMax) decodeMicrosMax = dt;

        ++batchCount;
        if (batchCount < BATCH_PACKETS) {
            continue;
        }

        const bool ok = M5Cardputer.Speaker.playRaw(
            pcm,
            PCM_BATCH_SAMPLES,
            8000,
            false,
            1,
            0,
            false
        );

        if (ok) {
            audioPacketsPlayed += BATCH_PACKETS;
            outIndex = (outIndex + 1) % 3;
        } else {
            Serial.println("[AUDIO] playRaw native 8k fallita");
        }

        batchCount = 0;
    }
}

static void queueAudioPacket(const uint8_t *payload27)
{
    if (!audioQueue) return;

    AudioPacket pkt;
    memcpy(pkt.ambe, payload27, 27);

    if (xQueueSend(audioQueue, &pkt, 0) != pdTRUE) {
        // Drop the oldest packet to keep latency bounded.
        AudioPacket old;
        xQueueReceive(audioQueue, &old, 0);

        if (xQueueSend(audioQueue, &pkt, 0) != pdTRUE) {
            ++audioQueueDrops;
            return;
        }

        ++audioQueueDrops;
    }
}

// ------------------------------------------------------------
// ODTP control
// ------------------------------------------------------------

static bool sendRewindPacket(uint16_t type, uint16_t flags, uint32_t sequence,
                             const uint8_t* payload, uint16_t payloadLen)
{
    if (WiFi.status() != WL_CONNECTED || !udpStarted) {
        Serial.printf("[TX] skip type=0x%04X: network not ready\n", type);
        return false;
    }

    uint8_t h[HEADER_LEN] = {0};
    rewindTxBuildHeader(h, type, flags, sequence, payloadLen);

    if (!udp.beginPacket(bmIP, profilePort()))
        return false;

    size_t written = udp.write(h, sizeof(h));
    if (payload && payloadLen)
        written += udp.write(payload, payloadLen);

    if (written != sizeof(h) + payloadLen) {
        udp.endPacket();
        return false;
    }

    return udp.endPacket() == 1;
}

void sendControl(uint16_t type, const uint8_t* payload, uint16_t payloadLen)
{
    if (!sendRewindPacket(type, 0, seqNo++, payload, payloadLen))
        Serial.printf("[TX CTRL] failed type=0x%04X\n", type);
}

static bool sendRealtime(uint16_t type, const uint8_t* payload, uint16_t payloadLen)
{
    const uint32_t seq = realtimeSeqNo++;
    const bool ok = sendRewindPacket(type, REWIND_FLAG_REAL_TIME_1,
                                     seq, payload, payloadLen);
    if (!ok) {
        ++txNetworkErrors;
        Serial.printf("[TX RT] failed type=0x%04X seq=%lu\n",
                      type, (unsigned long)seq);
    }
    return ok;
}

static bool sendTxSuperHeader()
{
    uint8_t payload[32] = {0};

    const uint32_t dst = txDestinationId();
    const uint32_t sessionType = txSessionType();

    // Match the known-working ODMRTP transmitter exactly: populate the
    // source callsign field and leave the 10-byte target callsign field zeroed.
    // Destination routing is already carried by targetId.
    rewindTxBuildSuperHeader(payload,
                             sessionType,
                             profileRadioId(),
                             dst,
                             "IU2VTP",
                             nullptr);

    const bool ok = sendRealtime(PKT_SUPERHEADER, payload, sizeof(payload));
    if (ok)
        Serial.printf("[PTT/TX] SUPERHEADER mode=%s src=%lu dst=%lu\n",
                      txCallModeLabel(),
                      (unsigned long)profileRadioId(),
                      (unsigned long)dst);
    return ok;
}

static bool sendTxTerminator()
{
    const bool ok = sendRealtime(PKT_DMR_TERMINATOR, nullptr, 0);
    if (ok)
        Serial.println("[PTT/TX] TERMINATOR");
    return ok;
}

void sendKeepalive()
{
    static constexpr const char DESC[] = "IU2VTP-Cardputer-DMR-Terminal 0.5.0";

    uint8_t payload[4 + 1 + sizeof(DESC) - 1] = {0};

    put32le(payload, profileRadioId());
    payload[4] = SERVICE_OPEN_TERMINAL;
    memcpy(payload + 5, DESC, sizeof(DESC) - 1);

    sendControl(PKT_KEEPALIVE, payload, sizeof(payload));
    lastKeepaliveTx = millis();

    Serial.println("[TX CTRL] KEEPALIVE");
}

void sendAuthentication(const uint8_t token[4])
{
    const size_t passLen = strlen(profilePassword());
    const size_t total = 4 + passLen;

    uint8_t* buf = (uint8_t*)malloc(total);
    if (!buf) {
        Serial.println("[ERR] auth malloc failed");
        return;
    }

    memcpy(buf, token, 4);
    memcpy(buf + 4, profilePassword(), passLen);

    uint8_t digest[32];

#if MBEDTLS_VERSION_MAJOR >= 3
    mbedtls_sha256(buf, total, digest, 0);
#else
    mbedtls_sha256_ret(buf, total, digest, 0);
#endif

    free(buf);

    sendControl(PKT_AUTHENTICATION, digest, sizeof(digest));
    state = State::WAIT_AUTH_ACK;

    Serial.println("[TX CTRL] AUTHENTICATION");
}

void sendConfiguration()
{
    uint8_t payload[4] = {0};
    // REWIND_OPTION_SUPER_HEADER = 1 << 0
    put32le(payload, 1);
    sendControl(PKT_CONFIGURATION, payload, sizeof(payload));
    Serial.println("[TX CTRL] CONFIGURATION SuperHeader");
}

void sendSubscription()
{
    uint8_t payload[8];

    put32le(payload + 0, SESSION_GROUP_VOICE);
    put32le(payload + 4, activeTG);

    sendControl(PKT_SUBSCRIPTION, payload, sizeof(payload));

    state = State::WAIT_SUB_ACK;
    lastSubscribeTx = millis();

    Serial.printf("[TX CTRL] SUBSCRIBE GroupVoice TG %lu\n",
                  (unsigned long)activeTG);
}



static bool ensureUdpStarted()
{
    if (udpStarted) return true;

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[UDP] Not started: Wi-Fi not connected");
        return false;
    }

    if (!udp.begin(54007)) {
        Serial.println("[UDP] begin(54007) failed");
        return false;
    }

    udpStarted = true;
    Serial.println("[UDP] socket locale 54007 OK");
    return true;
}

void sendCancellation(uint32_t tg)
{
    uint8_t payload[8];
    put32le(payload + 0, SESSION_GROUP_VOICE);
    put32le(payload + 4, tg);
    sendControl(PKT_CANCELLING, payload, sizeof(payload));
    Serial.printf("[TX CTRL] CANCEL TG %lu\n", (unsigned long)tg);
}

static bool resolveActiveServer()
{
    if (!activeProfileReady()) return false;

    Serial.printf("DNS %s ...\n", profileHost());
    if (!WiFi.hostByName(profileHost(), bmIP)) {
        Serial.println("[ERR] DNS failed");
        return false;
    }

    Serial.printf("Server %s -> %s:%u\n",
                  profileName(),
                  bmIP.toString().c_str(),
                  profilePort());
    return true;
}

static void startActiveConnection()
{
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[DMR] Connection deferred: Wi-Fi not connected");
        return;
    }

    if (!ensureUdpStarted()) {
        Serial.println("[DMR] Connection deferred: UDP unavailable");
        return;
    }

    if (!activeProfileReady()) {
        state = State::WAIT_CHALLENGE;
        Serial.println("[CONFIG] Profile incomplete: open settings with M");
        return;
    }

    if (!resolveActiveServer()) return;

    seqNo = 0;
    realtimeSeqNo = 0;
    lastKeepaliveTx = 0;
    lastKeepaliveAck = 0;
    lastSubscribeTx = 0;
    state = State::WAIT_CHALLENGE;
    resetCallInfo();
    if (audioQueue) xQueueReset(audioQueue);
    sendKeepalive();
}

static void switchTalkgroup(uint32_t newTG, bool saveAsCurrent)
{
    if (txCaptureActive()) {
        setUiNotice("Release PTT before changing TG");
        return;
    }
    if (newTG == 0 || newTG > 0xFFFFFF) return;
    uint32_t oldTG = activeTG;

    if (state == State::SUBSCRIBED || state == State::WAIT_SUB_ACK) {
        sendCancellation(oldTG);
    }

    activeTG = newTG;
    resetCallInfo();

    // Persist the actual RX TG, even when it is not part of favorites.
    if (saveAsCurrent) saveSettings();

    if (state == State::SUBSCRIBED ||
        state == State::WAIT_SUB_ACK ||
        state == State::AUTHENTICATED) {
        state = State::AUTHENTICATED;
        sendSubscription();
    }

    Serial.printf("[UI] TG %lu -> %lu\n",
                  (unsigned long)oldTG, (unsigned long)activeTG);
}

static void switchProfile(int delta)
{
    if (txCaptureActive()) {
        setUiNotice("Release PTT before changing server");
        return;
    }
    if (profileCount == 0) return;

    int start = activeProfileIndex;
    int next = start;

    for (int tries = 0; tries < profileCount; ++tries) {
        next += delta;
        while (next < 0) next += profileCount;
        next %= profileCount;

        if (profileReady((uint8_t)next))
            break;
    }

    if (next == start || !profileReady((uint8_t)next)) {
        setUiNotice("No other ready DMR profile");
        return;
    }

    if (state == State::SUBSCRIBED || state == State::WAIT_SUB_ACK) {
        sendCancellation(activeTG);
    }

    activeProfileIndex = (uint8_t)next;
    saveSettings();

    Serial.printf("[CX] Switch server -> %s (%s)\n",
                  profileName(), profileHost());

    startActiveConnection();
}

void handlePacket(uint8_t* buf, size_t n)
{
    if (n < HEADER_LEN) return;
    if (memcmp(buf, REWIND_SIGN, 8) != 0) return;

    uint16_t type = get16le(buf + 8);
    uint32_t seq = get32le(buf + 12);
    uint16_t plen = get16le(buf + 16);

    if (HEADER_LEN + plen > n) return;

    uint8_t* p = buf + HEADER_LEN;

    if (type != PKT_DMR_AUDIO) {
        Serial.printf("[RX] type=0x%04X seq=%lu len=%u\n",
                      type, (unsigned long)seq, plen);
    }

    switch (type) {
        case PKT_CHALLENGE:
            if (plen == 4) {
                Serial.println("[RX] CHALLENGE");
                sendAuthentication(p);
            }
            break;

        case PKT_KEEPALIVE:
            lastKeepaliveAck = millis();

            if (state == State::WAIT_AUTH_ACK ||
                state == State::WAIT_CHALLENGE) {
                Serial.println("[AUTH] LOGIN ACCEPTED");
                state = State::AUTHENTICATED;
                sendConfiguration();
                sendSubscription();
            }
            break;

        case PKT_SUBSCRIPTION:
            state = State::SUBSCRIBED;
            Serial.printf("[RX] SUBSCRIPTION ACK -> TG %lu ACTIVE\n",
                          (unsigned long)activeTG);
            break;

        case PKT_DMR_HEADER_FLC:
            if (plen >= 12) {
                uint8_t flco = p[0];
                uint8_t feature = p[1];
                uint8_t service = p[2];
                uint32_t dst = get24be(p + 3);
                uint32_t src = get24be(p + 6);

                if (callInfoMutex && xSemaphoreTake(callInfoMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    bool wasSameCall = callInfo.active &&
                                       callInfo.source == src &&
                                       callInfo.destination == dst;

                    callInfo.active = true;
                    callInfo.metadataValid = (src != 0 || dst != 0);
                    callInfo.privateCall = ((flco & 0x3F) == 3);
                    callInfo.flcByte1 = flco;
                    callInfo.feature = feature;
                    callInfo.service = service;
                    callInfo.rewindFlags = get16le(buf + 10);
                    callInfo.source = src;
                    callInfo.destination = dst;
                    if (!wasSameCall) {
                        callInfo.startMs = millis();
                        callInfo.packets = 0;
                    }
                    callInfo.endMs = 0;
                    callInfo.lastSeq = seq;
                    xSemaphoreGive(callInfoMutex);
                }

                requestCallsignLookup(src);

                Serial.printf("[CALL/FLC] %s %lu <- %lu feat=%u svc=%u flags=0x%04X\n",
                              ((flco & 0x3F) == 3) ? "PRIVATE" : "GROUP",
                              (unsigned long)dst,
                              (unsigned long)src,
                              feature, service, get16le(buf + 10));
            }
            break;


        case PKT_SUPERHEADER:
            if (plen >= 32) {
                uint32_t sessionType = get32le(p + 0);
                uint32_t src = get32le(p + 4);
                uint32_t dst = get32le(p + 8);
                char srcCall[20] = {0};
                char dstCall[20] = {0};

                copyRewindCallsign(srcCall, sizeof(srcCall), p + 12);
                copyRewindCallsign(dstCall, sizeof(dstCall), p + 22);

                if (callInfoMutex && xSemaphoreTake(callInfoMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    bool wasSameCall = callInfo.active &&
                                       callInfo.source == src &&
                                       callInfo.destination == dst;

                    callInfo.active = true;
                    callInfo.metadataValid = (src != 0 || dst != 0);
                    callInfo.privateCall = (sessionType == 5);
                    callInfo.source = src;
                    callInfo.destination = dst;
                    callInfo.rewindFlags = get16le(buf + 10);
                    callInfo.lastSeq = seq;
                    if (!wasSameCall) {
                        callInfo.startMs = millis();
                        callInfo.packets = 0;
                    }
                    callInfo.endMs = 0;

                    if (srcCall[0]) {
                        snprintf(callInfo.callsign, sizeof(callInfo.callsign), "%s", srcCall);
                    }
                    if (dstCall[0]) {
                        snprintf(callInfo.targetCall, sizeof(callInfo.targetCall), "%s", dstCall);
                    }

                    xSemaphoreGive(callInfoMutex);
                }

                if (src && !srcCall[0]) requestCallsignLookup(src);

                Serial.printf("[CALL/SUPER] type=%lu %lu(%s) -> %lu(%s)\n",
                              (unsigned long)sessionType,
                              (unsigned long)src, srcCall,
                              (unsigned long)dst, dstCall);
            }
            break;

        case PKT_DMR_AUDIO:
            if (state == State::SUBSCRIBED && plen == 27) {
                ++audioPackets;
                lastVoicePacketMs = millis();
                if (!txCaptureActive())
                    queueAudioPacket(p);

                if (callInfoMutex && xSemaphoreTake(callInfoMutex, 0) == pdTRUE) {
                    // Audio proves RX activity, but it does NOT contain source/destination.
                    // Never turn source=0 into fake call metadata.
                    if (!callInfo.active) {
                        callInfo.active = true;
                        callInfo.startMs = millis();
                        callInfo.packets = 0;
                    }
                    callInfo.packets++;
                    callInfo.lastSeq = seq;
                    callInfo.rewindFlags = get16le(buf + 10);
                    xSemaphoreGive(callInfoMutex);
                }

                if (audioPackets == 1) {
                    Serial.println("[AUDIO] primo pacchetto voce accodato");
                }
            }
            break;

        case PKT_DMR_TERMINATOR:
            if (plen >= 9) {
                uint32_t dst = get24be(p + 3);
                uint32_t src = get24be(p + 6);

                if (callInfoMutex && xSemaphoreTake(callInfoMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    if (src) {
                        callInfo.source = src;
                        callInfo.metadataValid = true;
                    }
                    if (dst) callInfo.destination = dst;
                    callInfo.active = false;
                    callInfo.endMs = millis();
                    callInfo.lastSeq = seq;
                    xSemaphoreGive(callInfoMutex);
                }

                if (src) requestCallsignLookup(src);

                Serial.printf("[CALL] END %lu <- %lu\n",
                              (unsigned long)dst, (unsigned long)src);
            }
            break;

        case PKT_BUSY_NOTICE:
            Serial.print("[RX] BUSY: ");
            printHex(p, plen);
            if (txCaptureActive())
                abortTxSession("server busy");
            break;

        case PKT_FAILURE:
            Serial.print("[RX] FAILURE: ");
            printHex(p, plen);
            if (txCaptureActive())
                abortTxSession("server failure");
            break;

        case PKT_CLOSE:
            Serial.println("[RX] SERVER CLOSE");
            state = State::WAIT_CHALLENGE;
            break;

        default:
            break;
    }
}



// ------------------------------------------------------------
// Forward declarations for UI / Wi-Fi helpers
// ------------------------------------------------------------
static void saveWifiConfig();
static void clearWifiConfig();
static bool wifiConfigured();
static void startWifiScan();
static bool connectSavedWifi(uint32_t timeoutMs);
static void drawWifiHome()
{
    auto& d = M5Cardputer.Display;

    static String lastSsid;
    static wl_status_t lastStatus = WL_NO_SHIELD;
    static int lastRssi = -999;

    String ssidNow = wifiConfigured() ? String(wifiConfig.ssid) : String("(none)");
    wl_status_t st = WiFi.status();
    int rssi = (st == WL_CONNECTED) ? WiFi.RSSI() : -127;

    if (wifiHomeDirty) {
        drawScreenChrome("Wi-Fi",
                         "ENTER=scan  R=reconnect",
                         "C=clear  ESC=back");

        d.setTextSize(1);
        d.setTextColor(TFT_WHITE, TFT_BLACK);
        d.setCursor(8, APP_HEADER_H + 35);
        d.print("Saved network:");

        lastSsid = "";
        lastStatus = WL_NO_SHIELD;
        lastRssi = -999;
        wifiHomeDirty = false;
    }

    if (ssidNow != lastSsid) {
        d.fillRect(6, APP_HEADER_H + 46, d.width() - 12, 18, TFT_BLACK);
        d.setTextColor(wifiConfigured() ? TFT_GREEN : TFT_ORANGE, TFT_BLACK);
        d.setTextSize(1);
        d.setCursor(8, APP_HEADER_H + 49);
        d.print(ssidNow);
        lastSsid = ssidNow;
    }

    if (st != lastStatus || abs(rssi - lastRssi) >= 2) {
        d.fillRect(125, APP_HEADER_H + 32, d.width() - 130, 16, TFT_BLACK);
        d.setTextSize(1);
        d.setTextColor(st == WL_CONNECTED ? TFT_GREEN : TFT_ORANGE, TFT_BLACK);
        d.setCursor(128, APP_HEADER_H + 35);

        if (st == WL_CONNECTED)
            d.printf("OK %ddBm", rssi);
        else
            d.print("OFF");

        lastStatus = st;
        lastRssi = rssi;
    }
}

static void drawWifiScan()
{
    auto& d = M5Cardputer.Display;

    static int lastIndex = -1;
    static int lastFirst = -1;
    static int lastCount = -1;
    static bool lastScanning = false;

    int first = 0;
    if (wifiMenuIndex >= 5) first = wifiMenuIndex - 4;

    bool fullRedraw =
        wifiScanDirty ||
        lastCount != wifiScanCount ||
        lastFirst != first ||
        lastScanning != wifiScanning;

    if (fullRedraw) {
        drawScreenChrome("Reti Wi-Fi",
                         "FN+UP/DOWN  ENTER=select",
                         "R=rescan  ESC=back");

        d.setTextSize(1);

        if (wifiScanning) {
            d.setTextColor(TFT_WHITE, TFT_BLACK);
            d.setCursor(8, APP_HEADER_H + 40);
            d.print("Scanning...");
        } else if (!wifiScanCount) {
            d.setTextColor(TFT_ORANGE, TFT_BLACK);
            d.setCursor(8, APP_HEADER_H + 40);
            d.print("No networks found");
        } else {
            for (int row = 0; row < 5; ++row) {
                int idx = first + row;
                if (idx >= wifiScanCount) break;

                int y = APP_HEADER_H + 31 + row * 18;
                bool selected = (idx == wifiMenuIndex);

                if (selected)
                    d.fillRect(3, y - 2, d.width() - 6, 16, TFT_DARKGREY);

                d.setTextColor(selected ? TFT_YELLOW : TFT_WHITE,
                               selected ? TFT_DARKGREY : TFT_BLACK);
                d.setCursor(6, y);

                String ssid = wifiScan[idx].ssid;
                if (ssid.length() > 20) ssid = ssid.substring(0, 20);

                d.printf("%c %-20s %d",
                         wifiScan[idx].open ? 'O' : '*',
                         ssid.c_str(),
                         wifiScan[idx].rssi);
            }
        }

        lastIndex = wifiMenuIndex;
        lastFirst = first;
        lastCount = wifiScanCount;
        lastScanning = wifiScanning;
        wifiScanDirty = false;
        return;
    }

    if (lastIndex != wifiMenuIndex && wifiScanCount) {
        auto redrawRow = [&](int idx) {
            if (idx < first || idx >= first + 5 || idx >= wifiScanCount) return;

            int row = idx - first;
            int y = APP_HEADER_H + 31 + row * 18;
            bool selected = (idx == wifiMenuIndex);

            d.fillRect(3, y - 2, d.width() - 6, 16,
                       selected ? TFT_DARKGREY : TFT_BLACK);
            d.setTextColor(selected ? TFT_YELLOW : TFT_WHITE,
                           selected ? TFT_DARKGREY : TFT_BLACK);
            d.setTextSize(1);
            d.setCursor(6, y);

            String ssid = wifiScan[idx].ssid;
            if (ssid.length() > 20) ssid = ssid.substring(0, 20);

            d.printf("%c %-20s %d",
                     wifiScan[idx].open ? 'O' : '*',
                     ssid.c_str(),
                     wifiScan[idx].rssi);
        };

        redrawRow(lastIndex);
        redrawRow(wifiMenuIndex);
        lastIndex = wifiMenuIndex;
    }
}

static void configureSelectedWifi();



static void clearFooter()
{
    auto& d = M5Cardputer.Display;
    const int y0 = d.height() - APP_FOOTER_H;
    d.fillRect(0, y0, d.width(), APP_FOOTER_H, TFT_BLACK);
}


static void drawFooter(const String& line1 = "", const String& line2 = "")
{
    auto& d = M5Cardputer.Display;
    const int y0 = d.height() - APP_FOOTER_H;

    clearFooter();
    d.drawFastHLine(0, y0, d.width(), TFT_DARKGREY);
    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.setTextSize(1);

    if (line1.length()) {
        d.setCursor(4, y0 + 3);
        d.print(line1);
    }
    if (line2.length()) {
        d.setCursor(4, y0 + 13);
        d.print(line2);
    }
}

static void drawScreenChrome(const String& title,
                             const String& footer1,
                             const String& footer2)
{
    drawScreenBase();

    auto& d = M5Cardputer.Display;
    d.setTextColor(TFT_CYAN, TFT_BLACK);
    d.setTextSize(2);
    d.setCursor(6, APP_HEADER_H + 4);
    d.print(title);

    drawFooter(footer1, footer2);
}

static void clearUiScreen()
{
    drawScreenBase();
}


static void resetTransientInputState()
{
    inputBuffer = "";
    inputTitle = "";
    inputNumericOnly = false;
    inputSecret = false;
    inputTarget = 0;
    inputReturnMode = UiMode::MAIN;
}

static void renderCurrentScreen()
{
    switch (uiMode) {
        case UiMode::MAIN:
            // drawUi() owns the dynamic RX screen. Force one full render.
            rxUiDirty = true;
            lastUiDraw = 0;
            return;

        case UiMode::CONFIG_HOME:
            drawConfigHome();
            return;

        case UiMode::CONFIG_PROFILES:
            drawProfilesMenu();
            return;

        case UiMode::CONFIG_PROFILE_EDIT:
            drawProfileEditor();
            return;

        case UiMode::CONFIG_TGS:
            drawTgMenu();
            return;

        case UiMode::WIFI_HOME:
            wifiHomeDirty = true;
            drawWifiHome();
            return;

        case UiMode::WIFI_SCAN:
            wifiScanDirty = true;
            drawWifiScan();
            return;

        case UiMode::AUDIO_VOLUME:
            drawVolumeMenu();
            return;

        case UiMode::TG_INPUT:
        case UiMode::TEXT_INPUT:
        case UiMode::WIFI_PASSWORD:
            drawInputBox();
            return;
    }
}

static void navigateTo(UiMode target, bool pushCurrent)
{
    if (target == uiMode) {
        if (target == UiMode::MAIN) {
            resetTransientInputState();
            rxUiDirty = true;
            lastUiDraw = 0;
        }
        renderCurrentScreen();
        return;
    }

    if (pushCurrent && navDepth < NAV_STACK_MAX)
        navStack[navDepth++] = uiMode;

    uiMode = target;

    if (target == UiMode::MAIN) {
        resetTransientInputState();
        rxUiDirty = true;
        lastUiDraw = 0;
    }

    renderCurrentScreen();
}

static void navigateBack(UiMode fallback)
{
    UiMode target = fallback;

    if (navDepth > 0)
        target = navStack[--navDepth];

    uiMode = target;

    if (target == UiMode::MAIN) {
        resetTransientInputState();
        rxUiDirty = true;
        lastUiDraw = 0;
    }

    renderCurrentScreen();
}

static void resetNavigation(UiMode target)
{
    navDepth = 0;
    uiMode = target;

    if (target == UiMode::MAIN) {
        resetTransientInputState();
        rxUiDirty = true;
        lastUiDraw = 0;
    }

    renderCurrentScreen();
}

static void applySpeakerVolume()
{
    M5Cardputer.Speaker.setVolume(speakerVolume);
}

static void setSpeakerVolume(int value)
{
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    speakerVolume = (uint8_t)value;
    applySpeakerVolume();
    saveSettings();
}

static void drawVolumeMenu()
{
    auto& d = M5Cardputer.Display;
    drawScreenChrome("Volume",
                     "FN+LEFT/RIGHT = -/+ 10",
                     "FN+UP/DOWN = -/+ 1  ESC=back");

    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setTextSize(3);
    d.setCursor(74, APP_HEADER_H + 42);
    d.printf("%u", speakerVolume);

    int barX = 15;
    int barY = APP_HEADER_H + 78;
    int barW = d.width() - 30;
    int barH = 14;

    d.drawRect(barX, barY, barW, barH, TFT_DARKGREY);
    int fill = (barW - 2) * speakerVolume / 255;
    if (fill > 0)
        d.fillRect(barX + 1, barY + 1, fill, barH - 2, TFT_GREEN);
}

static void drawConfigHome()
{
    auto& d = M5Cardputer.Display;
    drawScreenChrome("Settings",
                     "FN+UP/DOWN  ENTER=open",
                     "ESC=back");

    const char* rows[] = {
        "Start / return to RX",
        "DMR Server",
        "Talkgroup",
        "Wi-Fi",
        "Volume"
    };

    d.setTextSize(1);
    for (int i = 0; i < 5; ++i) {
        int y = APP_HEADER_H + 31 + i * 18;
        bool selected = (i == menuIndex);
        if (selected)
            d.fillRect(4, y - 2, d.width() - 8, 16, TFT_DARKGREY);

        d.setTextColor(selected ? TFT_YELLOW : TFT_WHITE,
                       selected ? TFT_DARKGREY : TFT_BLACK);
        d.setCursor(9, y);
        d.print(rows[i]);
    }
}

static void drawProfilesMenu()
{
    auto& d = M5Cardputer.Display;
    drawScreenChrome(setupFlowActive ? "2. DMR Server" : "DMR Server",
                     "ENTER=open / use if ready",
                     "A=new  D=delete  ESC=back");

    d.setTextSize(1);
    for (int i = 0; i < profileCount; ++i) {
        int y = APP_HEADER_H + 31 + i * 18;
        bool selected = (i == menuIndex);
        bool active = (i == activeProfileIndex);
        bool ready = profileReady(i);

        if (selected)
            d.fillRect(3, y - 2, d.width() - 6, 16, TFT_DARKGREY);

        d.setTextColor(selected ? TFT_YELLOW :
                       active ? TFT_GREEN :
                       ready ? TFT_WHITE : TFT_ORANGE,
                       selected ? TFT_DARKGREY : TFT_BLACK);

        d.setCursor(6, y);
        d.printf("%c %-12s %s",
                 active ? '>' : ' ',
                 profiles[i].name,
                 ready ? (active ? "ACTIVE" : "READY") : "NOT SET");
    }
}

static void drawProfileEditor()
{
    auto& d = M5Cardputer.Display;
    DmrProfile& p = profiles[editProfileIndex];

    drawScreenChrome(p.name[0] ? String(p.name) : String("DMR Profile"),
                     "ENTER=edit/confirm",
                     "ESC=back");

    const char* labels[] = {"Name", "Host", "Radio ID", "Password", "SAVE AND USE"};
    String vals[5] = {
        String(p.name),
        String(p.host),
        p.radioId ? String(p.radioId) : String("(missing)"),
        p.password[0] ? "********" : "(missing)",
        profileReady(editProfileIndex) ? "Enter" : "Complete required fields"
    };

    d.setTextSize(1);
    for (int i = 0; i < 5; ++i) {
        int y = APP_HEADER_H + 31 + i * 18;
        bool selected = (i == editFieldIndex);
        if (selected)
            d.fillRect(3, y - 2, d.width() - 6, 16, TFT_DARKGREY);

        d.setTextColor(selected ? TFT_YELLOW :
                       (i == 4 && !profileReady(editProfileIndex)) ? TFT_ORANGE : TFT_WHITE,
                       selected ? TFT_DARKGREY : TFT_BLACK);
        d.setCursor(6, y);

        String v = vals[i];
        if (v.length() > 25) v = v.substring(0, 25);
        d.printf("%s: %s", labels[i], v.c_str());
    }
}

static void drawTgMenu()
{
    auto& d = M5Cardputer.Display;
    drawScreenChrome(setupFlowActive ? "3. Talkgroup" : "Talkgroup",
                     setupFlowActive ? "ENTER=USE AND START RX" : "ENTER=edit TG",
                     "A=add  D=delete  ESC=back");

    d.setTextSize(1);
    int visible = min((int)favoriteTGCount, 5);
    int first = 0;
    if (menuIndex >= 5) first = menuIndex - 4;

    for (int row = 0; row < visible; ++row) {
        int i = first + row;
        if (i >= favoriteTGCount) break;

        int y = APP_HEADER_H + 31 + row * 18;
        bool selected = (i == menuIndex);
        if (selected)
            d.fillRect(3, y - 2, d.width() - 6, 16, TFT_DARKGREY);

        d.setTextColor(selected ? TFT_YELLOW :
                       i == activeTGIndex ? TFT_GREEN : TFT_WHITE,
                       selected ? TFT_DARKGREY : TFT_BLACK);
        d.setCursor(8, y);
        d.printf("%c TG %lu",
                 i == activeTGIndex ? '>' : ' ',
                 (unsigned long)favoriteTGs[i]);
    }
}

static void beginTextInput(const String& title, const String& initial,
                           bool numericOnly, bool secret, int target)
{
    UiMode returnMode = uiMode;

    resetTransientInputState();

    inputReturnMode = returnMode;
    inputTitle = title;
    inputBuffer = initial;
    inputNumericOnly = numericOnly;
    inputSecret = secret;
    inputTarget = target;

    uiMode = UiMode::TEXT_INPUT;
    drawInputBox();
}

static void beginTgInput()
{
    resetTransientInputState();

    inputReturnMode = UiMode::MAIN;
    inputTitle = "Enter TG";
    inputBuffer = "";
    inputNumericOnly = true;
    inputSecret = false;
    inputTarget = 1000;

    uiMode = UiMode::TG_INPUT;
    drawInputBox();
}

static void beginPrivateIdInput()
{
    resetTransientInputState();

    inputReturnMode = UiMode::MAIN;
    inputTitle = "Private DMR ID";
    inputBuffer = txPrivateId ? String(txPrivateId) : "";
    inputNumericOnly = true;
    inputSecret = false;
    inputTarget = 1001;

    uiMode = UiMode::TEXT_INPUT;
    drawInputBox();
}

static void toggleTxCallMode()
{
    if (txCaptureActive()) {
        setUiNotice("Release PTT before changing call mode");
        return;
    }

    txCallMode = (txCallMode == TxCallMode::GROUP)
        ? TxCallMode::PRIVATE
        : TxCallMode::GROUP;

    if (txCallMode == TxCallMode::GROUP) {
        setUiNotice("TX mode: GROUP");
    } else if (txPrivateId) {
        String msg = "TX PRIVATE ID ";
        msg += String(txPrivateId);
        setUiNotice(msg);
    } else {
        setUiNotice("PRIVATE mode: press I for DMR ID");
    }

    rxUiDirty = true;
    lastUiDraw = 0;
}

static void drawInputBox()
{
    auto& d = M5Cardputer.Display;

    static UiMode lastMode = UiMode::MAIN;
    static String lastTitle = "";
    static String lastShown = "";

    bool entering = (lastMode != uiMode) || (lastTitle != inputTitle);

    if (entering) {
        drawScreenChrome(inputTitle,
                         inputNumericOnly ? "Numbers only  ENTER=OK" : "ENTER=OK",
                         "DEL=delete  ESC=cancel");

        d.drawRect(5, APP_HEADER_H + 45, d.width() - 10, 28, TFT_DARKGREY);

        lastShown = "";
        lastMode = uiMode;
        lastTitle = inputTitle;
    }

    String shown;
    if (inputSecret) {
        for (size_t i = 0; i < inputBuffer.length(); ++i) shown += '*';
    } else {
        shown = inputBuffer;
    }

    if (shown.length() > 34)
        shown = shown.substring(shown.length() - 34);

    if (shown != lastShown || entering) {
        d.fillRect(7, APP_HEADER_H + 47, d.width() - 14, 24, TFT_BLACK);
        d.setTextColor(TFT_WHITE, TFT_BLACK);
        d.setTextSize(1);
        d.setCursor(10, APP_HEADER_H + 55);

        if (shown.length())
            d.print(shown);
        else {
            d.setTextColor(TFT_DARKGREY, TFT_BLACK);
            d.print(inputSecret ? "password..." : "type...");
        }

        int cursorX = 10 + min((int)shown.length(), 32) * 6;
        if (cursorX < d.width() - 8)
            d.drawFastVLine(cursorX, APP_HEADER_H + 53, 10, TFT_WHITE);

        lastShown = shown;
    }
}

static bool navUp()
{
    return M5Cardputer.Keyboard.keysState().fn &&
           M5Cardputer.Keyboard.isKeyPressed(';');
}
static bool navDown()
{
    return M5Cardputer.Keyboard.keysState().fn &&
           M5Cardputer.Keyboard.isKeyPressed('.');
}
static bool navLeft()
{
    return M5Cardputer.Keyboard.keysState().fn &&
           M5Cardputer.Keyboard.isKeyPressed(',');
}
static bool navRight()
{
    return M5Cardputer.Keyboard.keysState().fn &&
           M5Cardputer.Keyboard.isKeyPressed('/');
}


static void enterProfileStep(bool guided)
{
    setupFlowActive = guided;

    if (activeProfileIndex < profileCount)
        menuIndex = activeProfileIndex;
    else
        menuIndex = 0;

    if (uiMode == UiMode::CONFIG_PROFILES) {
        renderCurrentScreen();
    } else {
        navigateTo(UiMode::CONFIG_PROFILES);
    }
}

static void enterTalkgroupStep(bool guided)
{
    setupFlowActive = guided;
    menuIndex = (activeTGIndex < favoriteTGCount) ? activeTGIndex : 0;

    if (uiMode == UiMode::CONFIG_TGS) {
        renderCurrentScreen();
    } else {
        navigateTo(UiMode::CONFIG_TGS);
    }
}

static void routeAfterWifiConnected()
{
    normalizeActiveProfile();

    if (!activeProfileReady()) {
        setUiNotice("Wi-Fi OK. Select a DMR server.");
        enterProfileStep(true);
        return;
    }

    // If there is exactly one usable profile (e.g. only HamThings),
    // do not force the user through unrelated profiles.
    int readyCount = 0;
    for (uint8_t i = 0; i < profileCount; ++i)
        if (profileReady(i)) readyCount++;

    if (setupFlowActive && readyCount == 1) {
        enterTalkgroupStep(true);
        return;
    }

    if (setupFlowActive) {
        enterProfileStep(true);
        return;
    }

    uiMode = UiMode::CONFIG_HOME;
    menuIndex = 0;
    drawConfigHome();
}

static void commitTextInput()
{
    if (inputTarget == 1001) { // private DMR ID
        uint32_t id = (uint32_t)inputBuffer.toInt();
        if (id > 0 && id <= 0xFFFFFF) {
            txPrivateId = id;
            txCallMode = TxCallMode::PRIVATE;
            String msg = "PRIVATE ID ";
            msg += String(txPrivateId);
            setUiNotice(msg);
        } else {
            setUiNotice("Invalid private DMR ID");
        }

        resetNavigation(UiMode::MAIN);
        rxUiDirty = true;
        lastUiDraw = 0;
        return;
    }

    if (inputTarget == 1000) { // direct TG entry
        uint32_t tg = (uint32_t)inputBuffer.toInt();
        if (tg > 0 && tg <= 0xFFFFFF)
            switchTalkgroup(tg, true);

        // Return through the navigation framework so RX is invalidated/redrawn.
        resetNavigation(UiMode::MAIN);
        return;
    }

    if (inputTarget >= 2000 && inputTarget < 2100) {
        int index = inputTarget - 2000;
        if (index >= 0 && index < favoriteTGCount) {
            uint32_t tg = (uint32_t)inputBuffer.toInt();
            if (tg > 0 && tg <= 0xFFFFFF) {
                favoriteTGs[index] = tg;
                if (index == activeTGIndex) activeTG = tg;
                saveSettings();
            }
        }
        uiMode = inputReturnMode;
        renderCurrentScreen();
        return;
    }

    if (inputTarget == 4000) {
        snprintf(wifiConfig.password, sizeof(wifiConfig.password),
                 "%s", inputBuffer.c_str());
        saveWifiConfig();

        clearUiScreen();
        M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        M5Cardputer.Display.setCursor(8, APP_HEADER_H + 40);
        M5Cardputer.Display.print("Connecting to Wi-Fi...");

        bool ok = connectSavedWifi(15000);

        if (ok) {
            setupFlowActive = true;
            routeAfterWifiConnected();
        } else {
            setUiNotice("Wi-Fi connection failed");
            uiMode = UiMode::WIFI_HOME;
            wifiHomeDirty = true;
            drawWifiHome();
        }
        return;
    }

    if (inputTarget >= 3000 && inputTarget < 3100) {
        int packed = inputTarget - 3000;
        int pidx = packed / 10;
        int field = packed % 10;
        if (pidx >= 0 && pidx < profileCount) {
            DmrProfile& p = profiles[pidx];
            if (field == 0) snprintf(p.name, sizeof(p.name), "%s", inputBuffer.c_str());
            if (field == 1) snprintf(p.host, sizeof(p.host), "%s", inputBuffer.c_str());
            if (field == 2) p.radioId = (uint32_t)inputBuffer.toInt();
            if (field == 3) snprintf(p.password, sizeof(p.password), "%s", inputBuffer.c_str());
            saveSettings();
        }
        uiMode = inputReturnMode;
        renderCurrentScreen();
        return;
    }
}

static bool pttKeyDown()
{
    return M5Cardputer.BtnA.isPressed() ||
           M5Cardputer.Keyboard.isKeyPressed('p') ||
           M5Cardputer.Keyboard.isKeyPressed('P');
}

static void txMicBufferReleased(void*, void* data, size_t length)
{
    if (!txCaptureActive() || !data || length != TX_MIC_SAMPLES_16K)
        return;

    const int16_t* src = static_cast<const int16_t*>(data);
    TxPcmFrame frame;
    uint32_t peak = 0;

    for (size_t i = 0; i < TX_PCM_SAMPLES_8K; ++i) {
        int32_t mixed = (int32_t)src[i * 2] + (int32_t)src[i * 2 + 1];
        int16_t v = (int16_t)(mixed / 2);
        frame.pcm[i] = v;

        uint32_t a = (v < 0) ? (uint32_t)(-v) : (uint32_t)v;
        if (a > peak) peak = a;
    }

    txMicPeak = peak;
    ++txMicFramesCaptured;

    if (txPcmQueue) {
        if (xQueueSend(txPcmQueue, &frame, 0) == pdTRUE)
            ++txPcmFramesQueued;
        else
            ++txPcmQueueDrops;
    }

    if (txCaptureActive() &&
        !M5Cardputer.Mic.record(static_cast<int16_t*>(data),
                               TX_MIC_SAMPLES_16K)) {
        ++txMicRecordFailures;
    }
}

static bool startTxMicCapture()
{
    if (!txPcmQueue) {
        txPcmQueue = xQueueCreate(12, sizeof(TxPcmFrame));
        if (!txPcmQueue) {
            Serial.println("[PTT/MIC] PCM queue allocation failed");
            return false;
        }
    }

    if (!txDmrPacketQueue) {
        txDmrPacketQueue = xQueueCreate(12, sizeof(TxDmrPacket));
        if (!txDmrPacketQueue) {
            Serial.println("[PTT/TX] DMR packet queue allocation failed");
            return false;
        }
    }

    xQueueReset(txPcmQueue);
    xQueueReset(txDmrPacketQueue);
    txMicFramesCaptured = 0;
    txPcmFramesQueued = 0;
    txPcmFramesConsumed = 0;
    txPcmQueueDrops = 0;
    txMicRecordFailures = 0;
    txMicPeak = 0;
    txAmbeFramesEncoded = 0;
    txAmbeEncodeFailures = 0;
    txDmrFramesInterleaved = 0;
    txDmrPacketsBuilt = 0;
    txNetworkPacketsSent = 0;
    txNetworkPacketDrops = 0;
    txNetworkErrors = 0;
    txDmrFrameIndex = 0;
    txNextPacketDueUs = micros();
    txSessionAnnounced = false;
    memset(txDmrPacketBuild, 0, sizeof(txDmrPacketBuild));
    memset(txLastDmrPayload, 0, sizeof(txLastDmrPayload));

    if (txAmbeEncoderAvailable()) {
        if (!txAmbeEncoderBegin()) {
            Serial.println("[PTT/AMBE] encoder begin failed");
            return false;
        }
        txAmbeEncoderReset();

        // The vocoder has one frame of algorithmic history. Prime it with
        // 20 ms of digital silence and discard the result so the first AMBE
        // frame sent on-air corresponds to the first captured microphone frame.
        int16_t primePcm[TX_AMBE_PCM_SAMPLES] = {0};
        uint8_t primeAmbe[TX_AMBE_FRAME_BYTES] = {0};
        if (!txAmbeEncodePcm160(primePcm, primeAmbe)) {
            Serial.println("[PTT/AMBE] encoder prime failed");
            txAmbeEncoderEnd();
            return false;
        }
    } else {
        Serial.printf("[PTT/AMBE] embedded backend unavailable (%s)\n",
                      txAmbeEncoderBackendName());
    }

    M5Cardputer.Speaker.stop();
    M5Cardputer.Speaker.end();

    M5Cardputer.Mic.setBufferReleaseCallback(nullptr, txMicBufferReleased);
    if (!M5Cardputer.Mic.begin()) {
        Serial.println("[PTT/MIC] Mic.begin() failed");
        M5Cardputer.Mic.setBufferReleaseCallback(nullptr, nullptr);
        M5Cardputer.Speaker.begin();
        applySpeakerVolume();
        return false;
    }

    bool ok0 = M5Cardputer.Mic.record(txMicBuffers[0],
                                     TX_MIC_SAMPLES_16K,
                                     TX_MIC_RATE);
    bool ok1 = M5Cardputer.Mic.record(txMicBuffers[1],
                                     TX_MIC_SAMPLES_16K);

    if (!ok0 || !ok1) {
        Serial.printf("[PTT/MIC] prime failed: %d %d\n", ok0, ok1);
        M5Cardputer.Mic.end();
        M5Cardputer.Mic.setBufferReleaseCallback(nullptr, nullptr);
        M5Cardputer.Speaker.begin();
        applySpeakerVolume();
        return false;
    }

    Serial.println("[PTT/MIC] capture started: 16 kHz mono -> 8 kHz/160");
    return true;
}

static void stopTxMicCapture()
{
    M5Cardputer.Mic.end();
    M5Cardputer.Mic.setBufferReleaseCallback(nullptr, nullptr);

    if (txAmbeEncoderAvailable())
        txAmbeEncoderEnd();

    if (txPcmQueue)
        xQueueReset(txPcmQueue);

    M5Cardputer.Speaker.begin();
    applySpeakerVolume();

    Serial.printf("[PTT/MIC] stopped frames=%lu queued=%lu consumed=%lu drops=%lu recfail=%lu peak=%lu ambe=%lu encfail=%lu dmrframes=%lu dmrpkts=%lu\n",
                  (unsigned long)txMicFramesCaptured,
                  (unsigned long)txPcmFramesQueued,
                  (unsigned long)txPcmFramesConsumed,
                  (unsigned long)txPcmQueueDrops,
                  (unsigned long)txMicRecordFailures,
                  (unsigned long)txMicPeak,
                  (unsigned long)txAmbeFramesEncoded,
                  (unsigned long)txAmbeEncodeFailures,
                  (unsigned long)txDmrFramesInterleaved,
                  (unsigned long)txDmrPacketsBuilt);
    Serial.printf("[PTT/TX] network sent=%lu drops=%lu errors=%lu\n",
                  (unsigned long)txNetworkPacketsSent,
                  (unsigned long)txNetworkPacketDrops,
                  (unsigned long)txNetworkErrors);
}

static void processTxPcmDebug()
{
    if (!txPcmQueue) return;

    // Consume at most one 20 ms PCM frame per loop iteration. Draining the
    // entire PCM queue in one burst can create several 60 ms DMR packets at
    // once and overflow the paced network queue even though the long-term
    // production rate is correct.
    TxPcmFrame frame;
    if (xQueueReceive(txPcmQueue, &frame, 0) != pdTRUE)
        return;

    ++txPcmFramesConsumed;

    if (txAmbeEncoderAvailable()) {
        uint8_t canonical[TX_AMBE_FRAME_BYTES] = {0};

        if (!txAmbeEncodePcm160(frame.pcm, canonical)) {
            ++txAmbeEncodeFailures;
            return;
        }

        ++txAmbeFramesEncoded;

        // REWIND 0x0920 carries three 9-byte DMR AMBE frames. blip25
        // returns the carrier-neutral c0..c3 code vectors serialized in
        // canonical order, so apply the DMR 72-bit rW/rX/rY/rZ interleave
        // for each individual 9-byte frame. We still do NOT build a 33-byte
        // RF burst here; REWIND transports the three frame9 values directly.
        uint8_t dmr9[9] = {0};
        dmrCanonical72ToInterleaved(canonical, dmr9);
        ++txDmrFramesInterleaved;

        memcpy(txDmrPacketBuild + txDmrFrameIndex * 9, dmr9, 9);
        ++txDmrFrameIndex;

        if (txDmrFrameIndex == 3) {
            memcpy(txLastDmrPayload, txDmrPacketBuild, 27);
            ++txDmrPacketsBuilt;
            txDmrFrameIndex = 0;

            TxDmrPacket pkt;
            memcpy(pkt.payload, txLastDmrPayload, sizeof(pkt.payload));
            if (!txDmrPacketQueue ||
                xQueueSend(txDmrPacketQueue, &pkt, 0) != pdTRUE) {
                ++txNetworkPacketDrops;
            }

            if (txDmrPacketsBuilt == 1) {
                Serial.print("[PTT/AMBE] first DMR-interleaved mode33 payload: ");
                printHex(txLastDmrPayload, 27);
            }
        }
    }
}

static void processTxNetwork()
{
    if (!txSessionAnnounced || !txDmrPacketQueue) return;

    const uint32_t nowUs = micros();
    if ((int32_t)(nowUs - txNextPacketDueUs) < 0)
        return;

    TxDmrPacket pkt;
    if (xQueueReceive(txDmrPacketQueue, &pkt, 0) == pdTRUE) {
        if (sendRealtime(PKT_DMR_AUDIO, pkt.payload, sizeof(pkt.payload)))
            ++txNetworkPacketsSent;

        txNextPacketDueUs += TX_PACKET_PERIOD_US;

        // If the loop was stalled badly, do not burst old voice packets.
        if ((int32_t)(nowUs - txNextPacketDueUs) >
            (int32_t)(TX_PACKET_PERIOD_US * 3UL)) {
            txNextPacketDueUs = nowUs + TX_PACKET_PERIOD_US;
        }
    }
}

static void flushTxNetworkQueue(unsigned long maxWaitMs)
{
    const unsigned long start = millis();

    while (txDmrPacketQueue &&
           uxQueueMessagesWaiting(txDmrPacketQueue) > 0 &&
           millis() - start < maxWaitMs &&
           WiFi.status() == WL_CONNECTED) {
        processTxNetwork();
        delay(1);
    }
}

static void abortTxSession(const char* reason)
{
    if (txState == TxState::IDLE) return;

    Serial.printf("[PTT/TX] abort: %s\n", reason ? reason : "unknown");

    txState = TxState::IDLE;
    txStateStartedMs = 0;

    // Latch the current key-down state so a BUSY/network abort cannot
    // immediately re-key while the operator is still physically holding P.
    pttWasDown = true;

    M5Cardputer.Mic.end();
    M5Cardputer.Mic.setBufferReleaseCallback(nullptr, nullptr);
    if (txAmbeEncoderAvailable())
        txAmbeEncoderEnd();

    if (txPcmQueue) xQueueReset(txPcmQueue);
    if (txDmrPacketQueue) xQueueReset(txDmrPacketQueue);

    if (txSessionAnnounced && WiFi.status() == WL_CONNECTED && udpStarted)
        sendTxTerminator();

    txSessionAnnounced = false;
    txDmrFrameIndex = 0;

    M5Cardputer.Speaker.begin();
    applySpeakerVolume();
    rxUiDirty = true;
    lastUiDraw = 0;
}

static void beginPttTest()
{
    if (txState != TxState::IDLE) return;

    if (uiMode != UiMode::MAIN || !dmrSessionReady()) {
        setUiNotice("PTT unavailable: DMR not ready");
        Serial.println("[PTT] ignored: DMR session not ready");
        return;
    }

    if (!txAmbeEncoderAvailable()) {
        setUiNotice("PTT unavailable: AMBE TX backend missing");
        Serial.println("[PTT] ignored: embedded AMBE backend unavailable");
        return;
    }

    if (txCallMode == TxCallMode::PRIVATE && txPrivateId == 0) {
        setUiNotice("Set private DMR ID first (I)");
        Serial.println("[PTT] ignored: private destination missing");
        return;
    }

    txState = TxState::PTT_HELD_CAPTURE;
    txStateStartedMs = millis();

    if (!startTxMicCapture()) {
        txState = TxState::IDLE;
        txStateStartedMs = 0;
        setUiNotice("Microphone start failed");
        return;
    }

    // Drop queued RX voice and enter explicit half-duplex mode.
    if (audioQueue) xQueueReset(audioQueue);

    // Keep realtimeSeqNo monotonic for the whole REWIND connection.
    // It is reset only by startActiveConnection(), matching reference clients.
    txNextPacketDueUs = micros();

    if (!sendTxSuperHeader()) {
        abortTxSession("superheader failed");
        setUiNotice("PTT TX start failed");
        return;
    }

    txSessionAnnounced = true;
    rxUiDirty = true;
    lastUiDraw = 0;

    Serial.printf("[PTT] TX START mode=%s dst=%lu src=%lu\n",
                  txCallModeLabel(),
                  (unsigned long)txDestinationId(),
                  (unsigned long)profileRadioId());
}

static void endPttTest()
{
    if (txState == TxState::IDLE) return;

    const unsigned long elapsed = millis() - txStateStartedMs;

    // Clear the active capture state BEFORE Mic.end(). A buffer-release callback
    // may run while the driver is shutting down; it must never requeue capture.
    txState = TxState::IDLE;

    M5Cardputer.Mic.end();
    M5Cardputer.Mic.setBufferReleaseCallback(nullptr, nullptr);

    // Drain complete PCM frames already captured before the stop.
    processTxPcmDebug();
    flushTxNetworkQueue(1000);

    // A trailing 1-2 AMBE frame partial packet is intentionally discarded
    // rather than inventing an unverified silence codeword.
    txDmrFrameIndex = 0;

    if (txSessionAnnounced)
        sendTxTerminator();

    txSessionAnnounced = false;
    txStateStartedMs = 0;

    stopTxMicCapture();

    if (audioQueue) xQueueReset(audioQueue);

    rxUiDirty = true;
    lastUiDraw = 0;

    Serial.printf("[PTT] TX END after %lu ms sent=%lu neterr=%lu drop=%lu\n",
                  elapsed,
                  (unsigned long)txNetworkPacketsSent,
                  (unsigned long)txNetworkErrors,
                  (unsigned long)txNetworkPacketDrops);
}

static void handlePttState()
{
    const bool down = (uiMode == UiMode::MAIN) && pttKeyDown();

    if (down && !pttWasDown)
        beginPttTest();
    else if (!down && pttWasDown)
        endPttTest();

    pttWasDown = down;
}

static void handleKeyboard()
{
    if (!M5Cardputer.Keyboard.isChange() ||
        !M5Cardputer.Keyboard.isPressed()) return;

    auto ks = M5Cardputer.Keyboard.keysState();

    // Text/TG input
    if (uiMode == UiMode::TG_INPUT ||
        uiMode == UiMode::TEXT_INPUT ||
        uiMode == UiMode::WIFI_PASSWORD) {

        bool changed = false;

        if (ks.del) {
            if (inputBuffer.length()) {
                inputBuffer.remove(inputBuffer.length() - 1);
                changed = true;
            }
        }

        // ESC cancels input and always returns to the screen that opened it.
        if (M5Cardputer.Keyboard.isKeyPressed('`')) {
            UiMode returnMode = inputReturnMode;
            uiMode = returnMode;

            if (returnMode == UiMode::MAIN) {
                resetTransientInputState();
                rxUiDirty = true;
                lastUiDraw = 0;
            }

            renderCurrentScreen();
            return;
        }

        if (ks.enter) {
            if (uiMode == UiMode::TG_INPUT) inputTarget = 1000;
            commitTextInput();
            return;
        }

        // Cardputer text input comes through status.word.
        // Ignore control characters; preserve printable ASCII exactly.
        for (auto ch : ks.word) {
            if (ch == 0) continue;

            if (inputNumericOnly) {
                if (ch >= '0' && ch <= '9' && inputBuffer.length() < 8) {
                    inputBuffer += (char)ch;
                    changed = true;
                }
            } else {
                if (ch >= 32 && ch <= 126 && inputBuffer.length() < 64) {
                    inputBuffer += (char)ch;
                    changed = true;
                }
            }
        }

        if (changed) {
            Serial.printf("[INPUT] len=%u secret=%s\n",
                          (unsigned)inputBuffer.length(),
                          inputSecret ? "YES" : "NO");
        }

        drawInputBox();
        return;
    }

    if (uiMode == UiMode::MAIN) {
        if (M5Cardputer.Keyboard.isKeyPressed('m') ||
            M5Cardputer.Keyboard.isKeyPressed('M')) {
            menuIndex = 0;
            navigateTo(UiMode::CONFIG_HOME);
            return;
        }

        if (navUp() && favoriteTGCount) {
            activeTGIndex = (activeTGIndex + favoriteTGCount - 1) % favoriteTGCount;
            switchTalkgroup(favoriteTGs[activeTGIndex], true);
            rxUiDirty = true;
            lastUiDraw = 0;
            return;
        }
        if (navDown() && favoriteTGCount) {
            activeTGIndex = (activeTGIndex + 1) % favoriteTGCount;
            switchTalkgroup(favoriteTGs[activeTGIndex], true);
            rxUiDirty = true;
            lastUiDraw = 0;
            return;
        }
        if (navLeft()) {
            switchProfile(-1);
            rxUiDirty = true;
            lastUiDraw = 0;
            return;
        }
        if (navRight()) {
            switchProfile(+1);
            rxUiDirty = true;
            lastUiDraw = 0;
            return;
        }
        if (ks.enter) {
            beginTgInput();
            return;
        }

        if (M5Cardputer.Keyboard.isKeyPressed('c') ||
            M5Cardputer.Keyboard.isKeyPressed('C')) {
            toggleTxCallMode();
            return;
        }

        if (M5Cardputer.Keyboard.isKeyPressed('i') ||
            M5Cardputer.Keyboard.isKeyPressed('I')) {
            beginPrivateIdInput();
            return;
        }

        return;
    }



    if (uiMode == UiMode::AUDIO_VOLUME) {
        if (M5Cardputer.Keyboard.isKeyPressed('`')) {
            menuIndex = 4;
            navigateBack(UiMode::CONFIG_HOME);
            return;
        }

        if (navLeft()) {
            setSpeakerVolume((int)speakerVolume - 10);
            drawVolumeMenu();
            return;
        }

        if (navRight()) {
            setSpeakerVolume((int)speakerVolume + 10);
            drawVolumeMenu();
            return;
        }

        if (navUp()) {
            setSpeakerVolume((int)speakerVolume + 1);
            drawVolumeMenu();
            return;
        }

        if (navDown()) {
            setSpeakerVolume((int)speakerVolume - 1);
            drawVolumeMenu();
            return;
        }

        return;
    }

    if (uiMode == UiMode::WIFI_HOME) {
        if (M5Cardputer.Keyboard.isKeyPressed('`')) {
            menuIndex = 3;
            navigateBack(UiMode::CONFIG_HOME);
            return;
        }

        if (M5Cardputer.Keyboard.isKeyPressed('c') ||
            M5Cardputer.Keyboard.isKeyPressed('C')) {
            WiFi.disconnect(true, false);
            clearWifiConfig();
            drawWifiHome();
            return;
        }

        if (M5Cardputer.Keyboard.isKeyPressed('r') ||
            M5Cardputer.Keyboard.isKeyPressed('R')) {
            clearUiScreen();
            M5Cardputer.Display.setTextColor(TFT_WHITE);
            M5Cardputer.Display.setCursor(8, 40);
            M5Cardputer.Display.print("Connecting...");
            bool ok = connectSavedWifi(15000);
            if (ok) {
                routeAfterWifiConnected();
            } else {
                wifiHomeDirty = true;
                drawWifiHome();
            }
            return;
        }

        if (ks.enter) {
            clearUiScreen();
            M5Cardputer.Display.setTextColor(TFT_WHITE);
            M5Cardputer.Display.setCursor(8, 40);
            M5Cardputer.Display.print("Scanning Wi-Fi...");
            startWifiScan();
            uiMode = UiMode::WIFI_SCAN;
            wifiScanDirty = true;
            drawWifiScan();
            return;
        }
        return;
    }

    if (uiMode == UiMode::WIFI_SCAN) {
        if (M5Cardputer.Keyboard.isKeyPressed('`')) {
            navigateBack(UiMode::WIFI_HOME);
            return;
        }

        if (M5Cardputer.Keyboard.isKeyPressed('r') ||
            M5Cardputer.Keyboard.isKeyPressed('R')) {
            clearUiScreen();
            M5Cardputer.Display.setTextColor(TFT_WHITE);
            M5Cardputer.Display.setCursor(8, 40);
            M5Cardputer.Display.print("Scanning Wi-Fi...");
            startWifiScan();
            wifiScanDirty = true;
            drawWifiScan();
            return;
        }

        if (wifiScanCount) {
            if (navUp())
                wifiMenuIndex = (wifiMenuIndex + wifiScanCount - 1) % wifiScanCount;
            if (navDown())
                wifiMenuIndex = (wifiMenuIndex + 1) % wifiScanCount;
        }

        if (ks.enter && wifiScanCount) {
            configureSelectedWifi();
            return;
        }

        drawWifiScan();
        return;
    }

    if (uiMode == UiMode::CONFIG_HOME) {
        if (M5Cardputer.Keyboard.isKeyPressed('`')) {
            navigateBack(UiMode::MAIN);
            return;
        }

        if (navUp()) menuIndex = (menuIndex + 4) % 5;
        if (navDown()) menuIndex = (menuIndex + 1) % 5;

        if (ks.enter) {
            switch (menuIndex) {
                case 0: // RX
                    if (!activeProfileReady()) {
                        setUiNotice("Configure a DMR server first");
                        enterProfileStep(false);
                        return;
                    }
                    setupFlowActive = false;
                    startActiveConnection();
                    resetNavigation(UiMode::MAIN);
                    return;

                case 1:
                    enterProfileStep(false);
                    return;

                case 2:
                    enterTalkgroupStep(false);
                    return;

                case 3:
                    setupFlowActive = false;
                    navigateTo(UiMode::WIFI_HOME);
                    return;

                case 4:
                    setupFlowActive = false;
                    navigateTo(UiMode::AUDIO_VOLUME);
                    return;
            }
        }

        drawConfigHome();
        return;
    }

    if (uiMode == UiMode::CONFIG_PROFILES) {
        if (M5Cardputer.Keyboard.isKeyPressed('`')) {
            if (setupFlowActive) {
                uiMode = UiMode::WIFI_HOME;
                wifiHomeDirty = true;
                drawWifiHome();
            } else {
                uiMode = UiMode::CONFIG_HOME;
                menuIndex = 1;
                drawConfigHome();
            }
            return;
        }
        if (profileCount) {
            if (navUp()) menuIndex = (menuIndex + profileCount - 1) % profileCount;
            if (navDown()) menuIndex = (menuIndex + 1) % profileCount;
        }
        if (M5Cardputer.Keyboard.isKeyPressed('a') ||
            M5Cardputer.Keyboard.isKeyPressed('A')) {
            if (profileCount < MAX_PROFILES) {
                int i = profileCount++;
                memset(&profiles[i], 0, sizeof(DmrProfile));
                snprintf(profiles[i].name, sizeof(profiles[i].name), "Profile %d", i + 1);
                profiles[i].port = DMR_DEFAULT_PORT;
                profiles[i].enabled = true;
                menuIndex = i;
                saveSettings();
            }
        }
        if ((M5Cardputer.Keyboard.isKeyPressed('d') ||
             M5Cardputer.Keyboard.isKeyPressed('D')) && profileCount > 1) {
            int del = menuIndex;
            deleteProfileAt((uint8_t)del);
            if (menuIndex >= profileCount) menuIndex = profileCount - 1;
            drawProfilesMenu();
            return;
        }
        if (ks.enter && profileCount) {
            // A complete profile can be selected directly from the list.
            // An incomplete profile opens the editor.
            if (profileReady((uint8_t)menuIndex)) {
                activeProfileIndex = (uint8_t)menuIndex;
                saveSettings();

                Serial.printf("[CX] Active profile: %s (%s)\n",
                              profiles[activeProfileIndex].name,
                              profiles[activeProfileIndex].host);

                if (setupFlowActive) {
                    enterTalkgroupStep(true);
                } else {
                    uiMode = UiMode::CONFIG_HOME;
                    menuIndex = 0;
                    drawConfigHome();
                }
                return;
            }

            editProfileIndex = menuIndex;
            editFieldIndex = 0;
            navigateTo(UiMode::CONFIG_PROFILE_EDIT);
            return;
        }
        drawProfilesMenu();
        return;
    }

    if (uiMode == UiMode::CONFIG_PROFILE_EDIT) {
        if (M5Cardputer.Keyboard.isKeyPressed('`')) {
            menuIndex = editProfileIndex;
            navigateBack(UiMode::CONFIG_PROFILES);
            return;
        }
        if (navUp()) editFieldIndex = (editFieldIndex + 4) % 5;
        if (navDown()) editFieldIndex = (editFieldIndex + 1) % 5;
        if (ks.enter) {
            DmrProfile& p = profiles[editProfileIndex];
            if (editFieldIndex == 0)
                beginTextInput("Profile name", p.name, false, false, 3000 + editProfileIndex * 10 + 0);
            else if (editFieldIndex == 1)
                beginTextInput("Host", p.host, false, false, 3000 + editProfileIndex * 10 + 1);
            else if (editFieldIndex == 2)
                beginTextInput("Radio ID", String(p.radioId), true, false, 3000 + editProfileIndex * 10 + 2);
            else if (editFieldIndex == 3)
                beginTextInput("Password", "", false, true, 3000 + editProfileIndex * 10 + 3);
            else {
                if (!profileReady(editProfileIndex)) {
                    setUiNotice("Complete Host, Radio ID and Password");
                    drawProfileEditor();
                    return;
                }

                // The selected profile becomes active only here.
                if (state == State::SUBSCRIBED || state == State::WAIT_SUB_ACK) {
                    sendCancellation(activeTG);
                }

                activeProfileIndex = (uint8_t)editProfileIndex;
                saveSettings();

                Serial.printf("[CX] Active profile: %s (%s)\n",
                              profiles[activeProfileIndex].name,
                              profiles[activeProfileIndex].host);

                if (setupFlowActive) {
                    enterTalkgroupStep(true);
                } else {
                    uiMode = UiMode::CONFIG_HOME;
                    menuIndex = 0;
                    drawConfigHome();
                }
                return;
            }
            drawInputBox();
            return;
        }
        drawProfileEditor();
        return;
    }

    if (uiMode == UiMode::CONFIG_TGS) {
        if (M5Cardputer.Keyboard.isKeyPressed('`')) {
            if (setupFlowActive) {
                enterProfileStep(true);
            } else {
                menuIndex = 2;
                navigateBack(UiMode::CONFIG_HOME);
            }
            return;
        }
        if (favoriteTGCount) {
            if (navUp()) menuIndex = (menuIndex + favoriteTGCount - 1) % favoriteTGCount;
            if (navDown()) menuIndex = (menuIndex + 1) % favoriteTGCount;
        }
        if (M5Cardputer.Keyboard.isKeyPressed('a') ||
            M5Cardputer.Keyboard.isKeyPressed('A')) {
            if (favoriteTGCount < MAX_FAVORITE_TGS) {
                favoriteTGs[favoriteTGCount] = 0;
                menuIndex = favoriteTGCount++;
                saveSettings();
                beginTextInput("New TG", "", true, false, 2000 + menuIndex);
                return;
            }
        }
        if ((M5Cardputer.Keyboard.isKeyPressed('d') ||
             M5Cardputer.Keyboard.isKeyPressed('D')) && favoriteTGCount > 1) {
            int del = menuIndex;
            for (int i = del; i < favoriteTGCount - 1; ++i) favoriteTGs[i] = favoriteTGs[i + 1];
            favoriteTGCount--;
            if (activeTGIndex >= favoriteTGCount) activeTGIndex = 0;
            activeTG = favoriteTGs[activeTGIndex];
            if (menuIndex >= favoriteTGCount) menuIndex = favoriteTGCount - 1;
            saveSettings();
        }
        if (ks.enter && favoriteTGCount) {
            if (setupFlowActive) {
                activeTGIndex = (uint8_t)menuIndex;
                activeTG = favoriteTGs[activeTGIndex];
                saveSettings();  // saves both favorite index and actual current TG

                setupFlowActive = false;
                startActiveConnection();
                resetNavigation(UiMode::MAIN);
                return;
            }

            beginTextInput("Edit TG", String(favoriteTGs[menuIndex]),
                           true, false, 2000 + menuIndex);
            return;
        }
        drawTgMenu();
        return;
    }
}


static void saveWifiConfig()
{
    saveSettings();
}

static void clearWifiConfig()
{
    if (udpStarted) {
        udp.stop();
        udpStarted = false;
    }
    memset(&wifiConfig, 0, sizeof(wifiConfig));
    saveWifiConfig();
}

static bool wifiConfigured()
{
    return wifiConfig.ssid[0] != 0;
}

static void startWifiScan()
{
    wifiScanning = true;
    wifiScanCount = 0;

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);
    delay(100);

    int n = WiFi.scanNetworks(false, true);
    if (n < 0) n = 0;

    int lim = min(n, (int)MAX_WIFI_SCAN);
    for (int i = 0; i < lim; ++i) {
        String ssid = WiFi.SSID(i);
        if (!ssid.length()) continue;

        bool duplicate = false;
        for (int j = 0; j < wifiScanCount; ++j) {
            if (wifiScan[j].ssid == ssid) {
                duplicate = true;
                if (WiFi.RSSI(i) > wifiScan[j].rssi) {
                    wifiScan[j].rssi = WiFi.RSSI(i);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
                    wifiScan[j].open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
#else
                    wifiScan[j].open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
#endif
                }
                break;
            }
        }

        if (!duplicate && wifiScanCount < MAX_WIFI_SCAN) {
            wifiScan[wifiScanCount].ssid = ssid;
            wifiScan[wifiScanCount].rssi = WiFi.RSSI(i);
            wifiScan[wifiScanCount].open =
                (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
            wifiScanCount++;
        }
    }

    WiFi.scanDelete();

    // Sort strongest first.
    for (int i = 0; i < wifiScanCount; ++i) {
        for (int j = i + 1; j < wifiScanCount; ++j) {
            if (wifiScan[j].rssi > wifiScan[i].rssi) {
                WifiScanEntry tmp = wifiScan[i];
                wifiScan[i] = wifiScan[j];
                wifiScan[j] = tmp;
            }
        }
    }

    wifiMenuIndex = 0;
    wifiScanning = false;
}

static bool connectSavedWifi(uint32_t timeoutMs)
{
    if (!wifiConfigured()) return false;

    Serial.printf("[WIFI] Connection a \"%s\"...\n", wifiConfig.ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiConfig.ssid, wifiConfig.password);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
        delay(150);
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WIFI] OK IP=%s RSSI=%d\n",
                      WiFi.localIP().toString().c_str(),
                      WiFi.RSSI());
        return true;
    }

    Serial.println("[WIFI] Connection fallita");
    return false;
}

static void configureSelectedWifi()
{
    if (wifiMenuIndex >= wifiScanCount) return;

    String ssid = wifiScan[wifiMenuIndex].ssid;
    snprintf(wifiConfig.ssid, sizeof(wifiConfig.ssid), "%s", ssid.c_str());

    if (wifiScan[wifiMenuIndex].open) {
        wifiConfig.password[0] = 0;
        saveWifiConfig();

        clearUiScreen();
        M5Cardputer.Display.setTextColor(TFT_WHITE);
        M5Cardputer.Display.setCursor(8, 40);
        M5Cardputer.Display.print("Connecting...");

        bool ok = connectSavedWifi(15000);
        delay(500);

        if (ok) {
            setupFlowActive = true;
            routeAfterWifiConnected();
        } else {
            uiMode = UiMode::WIFI_HOME;
            wifiHomeDirty = true;
            drawWifiHome();
        }
        return;
    }

    uiMode = UiMode::TEXT_INPUT;
    inputTitle = "Wi-Fi password";
    inputBuffer = "";
    inputNumericOnly = false;
    inputSecret = true;
    inputTarget = 4000;
    drawInputBox();
}

static bool connectWiFi()
{
    if (!wifiConfigured()) {
        Serial.println("[WIFI] No network configured");
        return false;
    }

    if (!connectSavedWifi(15000)) {
        Serial.println("[WIFI] Saved Wi-Fi connection failed");
        return false;
    }

    return true;
}

void setup()
{
    Serial.begin(115200);
    delay(1500);

    Serial.println();
    Serial.println("============================================");
    Serial.println(" IU2VTP Cardputer DMR Terminal v1.1.0-alpha6");
    Serial.println(" classic mbelib + Cardputer speaker");
    Serial.println("============================================");
    Serial.println("classic mbelib / speaker 48 kHz / TX experimental");
    Serial.printf("[BOOT] loopTask stack=%lu bytes free=%u\n",
                  (unsigned long)getArduinoLoopTaskStackSize(),
                  (unsigned)uxTaskGetStackHighWaterMark(nullptr));

    if (!dmrAmbeMappingSelfTest()) {
        Serial.println("[STOP] DMR AMBE mapping self-test FAILED");
        while (true) delay(1000);
    }
    Serial.println("[DMR MAP] canonical <-> interleaved self-test OK");

    loadSettings();

    Serial.printf("[BOOT] free heap=%u\n", (unsigned)ESP.getFreeHeap());

    // Cardputer + speaker
    auto m5cfg = M5.config();
    M5Cardputer.begin(m5cfg);

    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setTextWrap(false);


    // Initialize Wi-Fi/lwIP even before an SSID exists.
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);

    callInfoMutex = xSemaphoreCreateMutex();
    if (!callInfoMutex) {
        Serial.println("[STOP] callInfoMutex fallita");
        while (true) delay(1000);
    }

    if (!M5Cardputer.Speaker.begin()) {
        Serial.println("[STOP] Speaker.begin() fallita");
        while (true) delay(1000);
    }

    applySpeakerVolume();

    audioQueue = xQueueCreate(AUDIO_QUEUE_DEPTH, sizeof(AudioPacket));
    if (!audioQueue) {
        Serial.println("[STOP] audioQueue fallita");
        while (true) delay(1000);
    }

    BaseType_t taskOk = xTaskCreatePinnedToCore(
        audioTask,
        "dmr_audio",
        8192,
        nullptr,
        2,
        &audioTaskHandle,
        1
    );

    if (taskOk != pdPASS) {
        Serial.println("[STOP] audioTask fallita");
        while (true) delay(1000);
    }

    BaseType_t lookupOk = xTaskCreatePinnedToCore(
        callsignLookupTask,
        "dmr_lookup",
        6144,
        nullptr,
        1,
        &lookupTaskHandle,
        0
    );

    if (lookupOk != pdPASS) {
        Serial.println("[WARN] callsign lookup task not started");
    }

    Serial.println("[BOOT] speaker, display and audio task OK");

    const bool wifiOk = connectWiFi();

    // IMPORTANT: choose and draw exactly ONE initial screen.
    // Network helpers above never draw anything.
    normalizeActiveProfile();

    if (wifiOk && activeProfileReady()) {
        setupFlowActive = false;
        Serial.printf("[BOOT] Active profile: %s (%s), TG %lu\n",
                      profileName(), profileHost(), (unsigned long)activeTG);
        resetNavigation(UiMode::MAIN);
    } else if (wifiOk) {
        Serial.println("[SETUP] Wi-Fi OK. Select/configure the DMR server.");
        setupFlowActive = true;
        enterProfileStep(true);
    } else {
        Serial.println("[SETUP] 1/3 Configure Wi-Fi.");
        setupFlowActive = true;
        resetNavigation(UiMode::WIFI_HOME);
    }
}

void loop()
{
    M5Cardputer.update();
    handlePttState();

    // Never reboot merely because Wi-Fi is disconnected.
    // During first-time setup this is expected, and rebooting here creates
    // an endless flashing/reboot loop on the Wi-Fi configuration screen.
    if (WiFi.status() != WL_CONNECTED) {
        // If we had a working connection before, retry gently in background,
        // but never while scanning or entering Wi-Fi credentials.
        bool wifiUiBusy =
            uiMode == UiMode::WIFI_HOME ||
            uiMode == UiMode::WIFI_SCAN ||
            uiMode == UiMode::WIFI_PASSWORD ||
            (uiMode == UiMode::TEXT_INPUT && inputTarget == 4000);

        if (wifiEverConnected && wifiConfigured() && !wifiUiBusy &&
            millis() - lastWifiReconnectAttempt >= 10000) {
            lastWifiReconnectAttempt = millis();
            Serial.println("[WIFI] Connection persa: tentativo riconnection");
            WiFi.reconnect();
        }
    } else {
        wifiEverConnected = true;
    }

    static bool prevWifiConnected = false;
    const bool wifiConnectedNow = (WiFi.status() == WL_CONNECTED);

    if (!wifiConnectedNow && prevWifiConnected) {
        if (udpStarted) {
            udp.stop();
            udpStarted = false;
            Serial.println("[UDP] closed after Wi-Fi loss");
        }
    }

    if (wifiConnectedNow && !prevWifiConnected) {
        Serial.println("[WIFI] Connected: transport available");
        if (activeProfileReady()) {
            startActiveConnection();
        }
    }

    prevWifiConnected = wifiConnectedNow;

    int packetSize = (WiFi.status() == WL_CONNECTED && udpStarted) ? udp.parsePacket() : 0;

    if (packetSize > 0) {
        static uint8_t rx[512];
        int n = udp.read(rx, min(packetSize, (int)sizeof(rx)));

        if (n > 0) {
            handlePacket(rx, (size_t)n);
        }
    }

    unsigned long now = millis();

    // Fallback: if a terminator is missed, clear RX state after 1.5 s without voice.
    if (lastVoicePacketMs && now - lastVoicePacketMs > 1500) {
        if (callInfoMutex && xSemaphoreTake(callInfoMutex, 0) == pdTRUE) {
            if (callInfo.active) {
                callInfo.active = false;
                callInfo.endMs = now;
            }
            xSemaphoreGive(callInfoMutex);
        }
    }

    handleKeyboard();
    processTxNetwork();
    processTxPcmDebug();

    if (txCaptureActive()) {
        if (WiFi.status() != WL_CONNECTED || !udpStarted) {
            abortTxSession("network lost");
        } else if (millis() - txStateStartedMs >= TX_MAX_MS) {
            abortTxSession("TX timeout");
            setUiNotice("PTT stopped: TX timeout");
        }
    }

    if (uiMode == UiMode::MAIN) {
        drawUi();
    }

    if (WiFi.status() == WL_CONNECTED &&
        activeProfileReady() &&
        (state == State::AUTHENTICATED ||
         state == State::WAIT_SUB_ACK ||
         state == State::SUBSCRIBED) &&
        now - lastKeepaliveTx >= 5000) {

        sendKeepalive();
    }

    if (WiFi.status() == WL_CONNECTED &&
        activeProfileReady() &&
        state == State::WAIT_CHALLENGE &&
        now - lastKeepaliveTx >= 20000) {

        sendKeepalive();
    }

    if (state == State::WAIT_SUB_ACK &&
        now - lastSubscribeTx >= 1500) {

        sendSubscription();
    }

    static unsigned long lastStats = 0;
    if (now - lastStats >= 10000) {
        lastStats = now;
        uint32_t avgUs = decodePacketsTimed
            ? (uint32_t)(decodeMicrosTotal / decodePacketsTimed)
            : 0;

        Serial.printf("[STACK] loop free=%u bytes\n",
                      (unsigned)uxTaskGetStackHighWaterMark(nullptr));

        Serial.printf(
            "[STATS] rx=%lu played=%lu frames=%lu drops=%lu queue=%u decode_avg=%luus decode_max=%luus realtime=%s peak=%lu clips=%lu\n",
            (unsigned long)audioPackets,
            (unsigned long)audioPacketsPlayed,
            (unsigned long)audioFramesDecoded,
            (unsigned long)audioQueueDrops,
            audioQueue ? (unsigned)uxQueueMessagesWaiting(audioQueue) : 0,
            (unsigned long)avgUs,
            (unsigned long)decodeMicrosMax,
            (avgUs > 0 && avgUs < 60000) ? "YES" : "NO",
            (unsigned long)pcmPeakSeen,
            (unsigned long)pcmClipSamples
        );
    }

    delay(1);
}
