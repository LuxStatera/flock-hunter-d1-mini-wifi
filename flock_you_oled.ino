// ============================================================
// Flock Hunter — ESP8266 D1 Mini + SH1106 128×64
// ============================================================
// WiFi-based Flock Safety surveillance camera detector with
// SH1106 OLED dashboard, piezo buzzer, and LittleFS persistence.
//
// Based on Flock You by colonelpanichacks/flock-you
// OUI dataset: @NitekryDPaul, DeFlockJoplin, @wgreenberg
// Wildcard-probe signature: DeFlockJoplin
//
// Hardware:
//   ESP8266 D1 Mini (or any ESP8266 board)
//   SH1106 128×64 I2C OLED (SDA=D2/GPIO4, SCL=D1/GPIO5, VCC=3.3V)
//   Piezo buzzer on D5/GPIO14 (optional)
// ============================================================

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "types.h"

// ESP8266 SDK — needed for promiscuous mode
extern "C" {
  #include "user_interface.h"
}

// ============================================================
// PIN CONFIG (overridable via platformio.ini build_flags)
// ============================================================

#ifndef BUZZER_PIN
  #define BUZZER_PIN 14   // D5
#endif
#ifndef USE_BUZZER
  #define USE_BUZZER 1
#endif
#ifndef OLED_SDA
  #define OLED_SDA 4      // D2
#endif
#ifndef OLED_SCL
  #define OLED_SCL 5      // D1
#endif

// ============================================================
// CONFIG
// ============================================================

// Channel hopping
#define CHANNEL_MODE_FULL_HOP   0
#define CHANNEL_MODE_CUSTOM     1
#define CHANNEL_MODE_SINGLE     2

#define CHANNEL_MODE       CHANNEL_MODE_CUSTOM
#define CHANNEL_DWELL_MS   350
#define SINGLE_CHANNEL     6

static const uint8_t customChannels[]    = {1, 6, 11};
static const size_t  customChannelCount  = sizeof(customChannels) / sizeof(customChannels[0]);
static const uint8_t fullHopChannels[]   = {1,2,3,4,5,6,7,8,9,10,11,12,13};
static const size_t  fullHopChannelCount = sizeof(fullHopChannels) / sizeof(fullHopChannels[0]);

// Timing
#define RSSI_MIN            -95
#define ALERT_COOLDOWN_MS   5000
#define REDISCOVER_MS       30000
#define HB_BEEP_INTERVAL_MS 10000
#define HB_DEVICE_ACTIVE_MS 3000
#define SCREEN_REFRESH_MS   750    // OLED refresh (I2C is slower)
#define HEARTBEAT_MS        30000
#define AUTOSAVE_INTERVAL_MS 60000

// Audio
#define NEW_CHIRP_LO_HZ    2000
#define NEW_CHIRP_HI_HZ    2800
#define NEW_CHIRP_NOTE_MS   55
#define NEW_CHIRP_GAP_MS    25
#define HB_BEEP_HZ         1500
#define HB_BEEP_NOTE_MS     70
#define HB_BEEP_GAP_MS      70

// Storage — keep smaller for ESP8266 (~80KB usable RAM)
#define MAX_DETECTIONS       100
#define FY_SESSION_FILE      "/session.json"
#define FY_SESSION_TMP       "/session.tmp"
#define FY_PREV_FILE         "/prev.json"

// ============================================================
// TARGET OUI LIST (lowercase, colons) — 32 known Flock prefixes
// ============================================================

static const char* target_ouis[] = {
  "70:c9:4e", "3c:91:80", "d8:f3:bc", "80:30:49", "b8:35:32",
  "14:5a:fc", "74:4c:a1", "08:3a:88", "9c:2f:9d", "c0:35:32",
  "94:08:53", "e4:aa:ea", "f4:6a:dd", "f8:a2:d6", "24:b2:b9",
  "00:f4:8d", "d0:39:57", "e8:d0:fc", "e0:4f:43", "b8:1e:a4",
  "70:08:94", "58:8e:81", "ec:1b:bd", "3c:71:bf", "58:00:e3",
  "90:35:ea", "5c:93:a2", "64:6e:69", "48:27:ea", "a4:cf:12",
  "82:6b:f2",  // DeFlockJoplin — 31st OUI
  "b4:1e:52"   // registered Flock Safety OUI — 32nd
};
static const size_t OUI_COUNT = sizeof(target_ouis) / sizeof(target_ouis[0]);
static uint8_t oui_bytes[OUI_COUNT][3];

// ============================================================
// DISPLAY — SH1106 128×64 I2C
// ============================================================

U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

#define DISP_W         128
#define DISP_H         64
#define FONT_H         8
#define LOG_ROWS       5
#define HEADER_Y       7
#define LIST_Y_START   17
#define FOOTER_Y       63

// ============================================================
// ALERT QUEUE (promisc callback -> loop)
// ============================================================

#define ALERT_QUEUE_SIZE 16

static volatile AlertEntry alertQueue[ALERT_QUEUE_SIZE];
static volatile size_t alertHead = 0;
static volatile size_t alertTail = 0;

static void IRAM_ATTR enqueueAlert(AlertType type, const uint8_t* mac,
                                    int8_t rssi, uint8_t ch) {
  size_t next = (alertHead + 1) % ALERT_QUEUE_SIZE;
  if (next == alertTail) return;
  AlertEntry* e = (AlertEntry*)&alertQueue[alertHead];
  e->type    = type;
  e->rssi    = rssi;
  e->channel = ch;
  memcpy((void*)e->mac, mac, 6);
  alertHead = next;
}

// ============================================================
// DETECTION TABLE
// ============================================================

static FYDetection fyDet[MAX_DETECTIONS];
static int           fyDetCount       = 0;
static bool          fyFsReady        = false;
static bool          fyDirty          = false;
static unsigned long fyLastSaveAt     = 0;
static int           fyLastSaveCount  = 0;

// Channel state
static uint8_t       currentChannel   = 1;
static size_t        customChIdx      = 0;
static size_t        fullHopIdx       = 0;
static unsigned long lastHop          = 0;
static unsigned long lastHeartbeat    = 0;

// Dedup
#define DEDUPE_SLOTS 8
static struct { char mac[18]; unsigned long ts; } dedupeTable[DEDUPE_SLOTS];
static size_t dedupeIdx = 0;

// Heartbeat audio
static unsigned long fyLastTargetSeen  = 0;
static unsigned long fyLastHbBeepAt    = 0;

// Screen state
static unsigned long lastScreenRefresh = 0;
static bool          screenDirty       = true;
static unsigned long alertFlashUntil   = 0;
static unsigned long uptimeStart       = 0;
static uint8_t       scanDotPhase      = 0;

// ============================================================
// HELPERS
// ============================================================

static void macToStr(const uint8_t* mac, char* buf, size_t len) {
  snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void ouiFromMac(const uint8_t* mac, char* buf, size_t len) {
  snprintf(buf, len, "%02x:%02x:%02x", mac[0], mac[1], mac[2]);
}

static void precompileOuis() {
  for (size_t i = 0; i < OUI_COUNT; i++) {
    const char* o = target_ouis[i];
    oui_bytes[i][0] = (uint8_t)strtol(o,     nullptr, 16);
    oui_bytes[i][1] = (uint8_t)strtol(o + 3, nullptr, 16);
    oui_bytes[i][2] = (uint8_t)strtol(o + 6, nullptr, 16);
  }
}

static inline bool IRAM_ATTR isMulticast(const uint8_t* mac) {
  return mac[0] & 0x01;
}

static bool IRAM_ATTR matchOuiRaw(const uint8_t* mac) {
  if (mac[0] & 0x02) return false;
  for (size_t i = 0; i < OUI_COUNT; i++) {
    if (mac[0] == oui_bytes[i][0] &&
        mac[1] == oui_bytes[i][1] &&
        mac[2] == oui_bytes[i][2]) return true;
  }
  return false;
}

static bool shouldSuppressDuplicate(const char* macStr) {
  unsigned long now = millis();
  for (size_t i = 0; i < DEDUPE_SLOTS; i++) {
    if (strcmp(dedupeTable[i].mac, macStr) == 0) {
      if ((now - dedupeTable[i].ts) < ALERT_COOLDOWN_MS) return true;
      dedupeTable[i].ts = now;
      return false;
    }
  }
  strncpy(dedupeTable[dedupeIdx].mac, macStr, 17);
  dedupeTable[dedupeIdx].mac[17] = '\0';
  dedupeTable[dedupeIdx].ts = now;
  dedupeIdx = (dedupeIdx + 1) % DEDUPE_SLOTS;
  return false;
}

static const char* alertTypeToMethod(AlertType t) {
  switch (t) {
    case ALERT_OUI_ADDR2:      return "oui_tx";
    case ALERT_OUI_ADDR1:      return "oui_rx";
    case ALERT_OUI_ADDR3:      return "oui_bss";
    case ALERT_WILDCARD_PROBE: return "wc_prb";
    default:                   return "unk";
  }
}

// ============================================================
// BUZZER
// ============================================================

static void newDetectChirp() {
#if USE_BUZZER
  tone(BUZZER_PIN, NEW_CHIRP_LO_HZ, NEW_CHIRP_NOTE_MS);
  delay(NEW_CHIRP_NOTE_MS + NEW_CHIRP_GAP_MS);
  tone(BUZZER_PIN, NEW_CHIRP_HI_HZ, NEW_CHIRP_NOTE_MS);
  delay(NEW_CHIRP_NOTE_MS);
  noTone(BUZZER_PIN);
#endif
}

static void heartbeatBeep() {
#if USE_BUZZER
  tone(BUZZER_PIN, HB_BEEP_HZ, HB_BEEP_NOTE_MS);
  delay(HB_BEEP_NOTE_MS + HB_BEEP_GAP_MS);
  tone(BUZZER_PIN, HB_BEEP_HZ, HB_BEEP_NOTE_MS);
  delay(HB_BEEP_NOTE_MS);
  noTone(BUZZER_PIN);
#endif
}

static void startupBeep() {
#if USE_BUZZER
  static const uint16_t notes[6] = { 523, 262, 440, 220, 415, 208 };
  for (int i = 0; i < 6; i++) {
    tone(BUZZER_PIN, notes[i]);
    delay((i == 5) ? 160 : 95);
    noTone(BUZZER_PIN);
    if (i < 5) delay(22);
  }
#endif
}

// ============================================================
// DETECTION TABLE OPS
// ============================================================

static int fyAddDetection(const char* mac, const char* method,
                          int8_t rssi, uint8_t ch,
                          bool* outChirpWorthy) {
  uint32_t now = millis();
  for (int i = 0; i < fyDetCount; i++) {
    if (strcasecmp(fyDet[i].mac, mac) == 0) {
      bool rediscover = (now - fyDet[i].lastSeen) > REDISCOVER_MS;
      if (fyDet[i].count < 0xFFFF) fyDet[i].count++;
      fyDet[i].lastSeen = now;
      fyDet[i].rssi     = rssi;
      fyDet[i].channel  = ch;
      fyDirty = true;
      if (outChirpWorthy) *outChirpWorthy = rediscover;
      return i;
    }
  }
  if (fyDetCount >= MAX_DETECTIONS) {
    if (outChirpWorthy) *outChirpWorthy = false;
    return -1;
  }
  FYDetection& d = fyDet[fyDetCount];
  strncpy(d.mac, mac, 17); d.mac[17] = '\0';
  strncpy(d.method, method ? method : "", 11); d.method[11] = '\0';
  d.rssi      = rssi;
  d.channel   = ch;
  d.firstSeen = now;
  d.lastSeen  = now;
  d.count     = 1;
  fyDetCount++;
  fyDirty = true;
  if (outChirpWorthy) *outChirpWorthy = true;
  return fyDetCount - 1;
}

// ============================================================
// LittleFS PERSISTENCE
// ============================================================

static size_t fySerializeDet(const FYDetection& d, char* dst, size_t cap) {
  int n = snprintf(dst, cap,
      "{\"mac\":\"%s\",\"m\":\"%s\",\"r\":%d,\"ch\":%u,"
      "\"f\":%lu,\"l\":%lu,\"c\":%u}",
      d.mac, d.method, d.rssi, (unsigned)d.channel,
      (unsigned long)d.firstSeen, (unsigned long)d.lastSeen,
      (unsigned)d.count);
  return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

static void fySaveSession() {
  if (!fyFsReady || !fyDirty) return;
  if (fyDetCount == fyLastSaveCount && !fyDirty) return;

  File f = LittleFS.open(FY_SESSION_TMP, "w");
  if (!f) return;

  f.printf("{\"v\":1,\"count\":%d}\n[", fyDetCount);
  char line[256];
  for (int i = 0; i < fyDetCount; i++) {
    if (i > 0) f.write(',');
    size_t n = fySerializeDet(fyDet[i], line, sizeof(line));
    if (n > 0) f.write((uint8_t*)line, n);
  }
  f.print(']');
  f.close();

  if (LittleFS.exists(FY_SESSION_FILE)) LittleFS.remove(FY_SESSION_FILE);
  LittleFS.rename(FY_SESSION_TMP, FY_SESSION_FILE);

  fyLastSaveAt    = millis();
  fyLastSaveCount = fyDetCount;
  fyDirty         = false;
  Serial.printf("[fy] saved %d det\n", fyDetCount);
}

static void fyPromotePrevSession() {
  if (!fyFsReady) return;
  if (!LittleFS.exists(FY_SESSION_FILE)) return;

  File src = LittleFS.open(FY_SESSION_FILE, "r");
  if (!src) return;
  File dst = LittleFS.open(FY_PREV_FILE, "w");
  if (!dst) { src.close(); return; }
  uint8_t buf[128];
  int n;
  while ((n = src.read(buf, sizeof(buf))) > 0) dst.write(buf, n);
  src.close();
  dst.close();
  LittleFS.remove(FY_SESSION_FILE);
  if (LittleFS.exists(FY_SESSION_TMP)) LittleFS.remove(FY_SESSION_TMP);
  Serial.println("[fy] prev session promoted");
}

// ============================================================
// CHANNEL HOPPING
// ============================================================

static void applyInitialChannel() {
#if CHANNEL_MODE == CHANNEL_MODE_SINGLE
  currentChannel = SINGLE_CHANNEL;
#elif CHANNEL_MODE == CHANNEL_MODE_CUSTOM
  currentChannel = customChannels[0];
#else
  currentChannel = fullHopChannels[0];
#endif
  wifi_set_channel(currentChannel);
  lastHop = millis();
}

static void updateChannelMode() {
#if CHANNEL_MODE == CHANNEL_MODE_SINGLE
  return;
#else
  if (millis() - lastHop < CHANNEL_DWELL_MS) return;
  #if CHANNEL_MODE == CHANNEL_MODE_CUSTOM
    customChIdx = (customChIdx + 1) % customChannelCount;
    currentChannel = customChannels[customChIdx];
  #else
    fullHopIdx = (fullHopIdx + 1) % fullHopChannelCount;
    currentChannel = fullHopChannels[fullHopIdx];
  #endif
  wifi_set_channel(currentChannel);
  lastHop = millis();
#endif
}

// ============================================================
// PROMISCUOUS CALLBACK — ESP8266 version
// ============================================================

static int IRAM_ATTR isWildcardProbeIE(const uint8_t* body, int len) {
  if (!body || len < 2) return -1;
  while (len >= 2) {
    uint8_t id   = body[0];
    uint8_t elen = body[1];
    if ((int)elen + 2 > len) break;
    if (id == 0) return (elen == 0) ? 1 : 0;
    body += elen + 2;
    len  -= elen + 2;
  }
  return -1;
}

static void IRAM_ATTR promiscCallback(uint8_t* buf, uint16_t len) {
  if (len <= 12) return;

  struct ieee80211_hdr* hdr;
  int8_t rssi;
  uint8_t ch;
  int frameBodyLen = 0;
  const uint8_t* frameBody = nullptr;

  if (len == 128) {
    struct sniffer_buf* sb = (struct sniffer_buf*)buf;
    rssi = sb->rx_ctrl.rssi;
    ch   = sb->rx_ctrl.channel;
    hdr  = (struct ieee80211_hdr*)sb->buf;
  } else {
    struct sniffer_buf2* sb2 = (struct sniffer_buf2*)buf;
    rssi = sb2->rx_ctrl.rssi;
    ch   = sb2->rx_ctrl.channel;
    hdr  = (struct ieee80211_hdr*)sb2->buf;
    int hdrSize = sizeof(struct ieee80211_hdr);
    int available = (int)(len - sizeof(struct RxControl));
    if (available > hdrSize) {
      frameBody    = sb2->buf + hdrSize;
      frameBodyLen = available - hdrSize;
    }
  }

  if (rssi < RSSI_MIN) return;

  if (matchOuiRaw(hdr->addr2)) {
    bool emitted = false;
    if (len != 128 && frameBody && frameBodyLen > 0) {
      uint8_t fc0     = hdr->frame_ctrl & 0xFF;
      uint8_t ftype   = (fc0 >> 2) & 0x03;
      uint8_t subtype = (fc0 >> 4) & 0x0F;
      if (ftype == 0 && subtype == 4) {
        int r = isWildcardProbeIE(frameBody, frameBodyLen);
        if (r == -1 && frameBodyLen > 4)
          r = isWildcardProbeIE(frameBody, frameBodyLen - 4);
        if (r == 1) {
          enqueueAlert(ALERT_WILDCARD_PROBE, hdr->addr2, rssi, ch);
          emitted = true;
        }
      }
    }
    if (!emitted) {
      enqueueAlert(ALERT_OUI_ADDR2, hdr->addr2, rssi, ch);
    }
  }

  if (!isMulticast(hdr->addr1) && matchOuiRaw(hdr->addr1)) {
    enqueueAlert(ALERT_OUI_ADDR1, hdr->addr1, rssi, ch);
  }
}

// ============================================================
// DRAIN ALERT QUEUE (called from loop)
// ============================================================

static void drainAlertQueue() {
  while (alertTail != alertHead) {
    AlertEntry e;
    noInterrupts();
    memcpy(&e, (const void*)&alertQueue[alertTail], sizeof(AlertEntry));
    alertTail = (alertTail + 1) % ALERT_QUEUE_SIZE;
    interrupts();

    char macStr[18];
    macToStr(e.mac, macStr, sizeof(macStr));
    const char* method = alertTypeToMethod(e.type);

    bool chirpWorthy = false;
    fyAddDetection(macStr, method, e.rssi, e.channel, &chirpWorthy);

    fyLastTargetSeen = millis();

    if (shouldSuppressDuplicate(macStr)) continue;

    char oui[9];
    ouiFromMac(e.mac, oui, sizeof(oui));
    Serial.printf("{\"event\":\"detection\",\"method\":\"wifi_%s\","
                  "\"mac\":\"%s\",\"oui\":\"%s\",\"rssi\":%d,\"ch\":%u}\n",
                  method, macStr, oui, e.rssi, (unsigned)e.channel);

    if (chirpWorthy) {
      newDetectChirp();
      fyLastHbBeepAt = millis();
      alertFlashUntil = millis() + 600;
    }

    screenDirty = true;
  }
}

// ============================================================
// OLED RENDERING
// ============================================================

static char rssiChar(int8_t rssi) {
  if (rssi > -60) return 'H';
  if (rssi > -75) return 'M';
  return 'L';
}

static void drawScreen() {
  display.clearBuffer();

  bool isFlashing = (alertFlashUntil > 0 && millis() < alertFlashUntil);

  display.setFont(u8g2_font_5x8_tr);

  if (isFlashing) {
    display.drawBox(2, 0, DISP_W - 4, 9);
    display.setDrawColor(0);
    display.drawStr(2, HEADER_Y, "! FLOCK HUNTER !");
  } else {
    display.setDrawColor(1);
    display.drawStr(2, HEADER_Y, "FLK-HUNT");
  }
  display.setDrawColor(1);

  char chBuf[8];
  snprintf(chBuf, sizeof(chBuf), "CH:%d", currentChannel);
  display.drawStr(60, HEADER_Y, chBuf);

  char detBuf[8];
  snprintf(detBuf, sizeof(detBuf), "D:%d", fyDetCount);
  int detW = display.getStrWidth(detBuf);
  display.drawStr(DISP_W - detW - 2, HEADER_Y, detBuf);

  display.drawHLine(2, 9, DISP_W - 4);

  if (fyDetCount == 0) {
    scanDotPhase = (scanDotPhase + 1) % 4;
    char scanMsg[20];
    snprintf(scanMsg, sizeof(scanMsg), "Scanning%.*s", scanDotPhase, "...");
    int sw = display.getStrWidth(scanMsg);
    display.drawStr((DISP_W - sw) / 2, 30, scanMsg);

    char ouiMsg[16];
    snprintf(ouiMsg, sizeof(ouiMsg), "%d OUI targets", (int)OUI_COUNT);
    int ow = display.getStrWidth(ouiMsg);
    display.drawStr((DISP_W - ow) / 2, 42, ouiMsg);
  } else {
    int y = LIST_Y_START;
    int startIdx = fyDetCount - 1;

    for (int row = 0; row < LOG_ROWS && startIdx - row >= 0; row++) {
      int i = startIdx - row;
      FYDetection& d = fyDet[i];

      bool recent = (millis() - d.lastSeen) < 5000;

      if (recent) {
        display.drawBox(2, y - 7, DISP_W - 4, 9);
        display.setDrawColor(0);
      }

      char shortMac[12];
      snprintf(shortMac, sizeof(shortMac), "%.11s", d.mac);

      char rssiBuf[6];
      snprintf(rssiBuf, sizeof(rssiBuf), "%d", d.rssi);

      char chStr[4];
      snprintf(chStr, sizeof(chStr), "%d", d.channel);

      display.drawStr(2, y, shortMac);
      display.drawStr(68, y, rssiBuf);

      char sq = rssiChar(d.rssi);
      char sqBuf[2] = {sq, '\0'};
      display.drawStr(95, y, sqBuf);

      display.drawStr(105, y, chStr);

      if (d.count > 1 && d.count < 100) {
        char cBuf[5];
        snprintf(cBuf, sizeof(cBuf), "x%d", d.count);
        display.drawStr(118, y, cBuf);
      }

      display.setDrawColor(1);
      y += 9;
    }
  }

  display.drawHLine(2, 55, DISP_W - 4);

  unsigned long upSec = (millis() - uptimeStart) / 1000;
  char upBuf[10];
  if (upSec < 3600)
    snprintf(upBuf, sizeof(upBuf), "%lum%02lu", upSec / 60, upSec % 60);
  else
    snprintf(upBuf, sizeof(upBuf), "%luh%02lu", upSec / 3600, (upSec % 3600) / 60);
  display.drawStr(2, FOOTER_Y, upBuf);

  display.drawStr(40, FOOTER_Y, fyFsReady ? "FS:OK" : "FS:--");

  char slotBuf[10];
  snprintf(slotBuf, sizeof(slotBuf), "%d/%d", fyDetCount, MAX_DETECTIONS);
  int slotW = display.getStrWidth(slotBuf);
  display.drawStr(DISP_W - slotW - 2, FOOTER_Y, slotBuf);

#if USE_BUZZER
  display.drawStr(80, FOOTER_Y, "BZ");
#endif

  display.sendBuffer();
}

static void drawSplash() {
  display.clearBuffer();
  display.setFont(u8g2_font_7x14B_tr);
  const char* t1 = "FLOCK HUNTER";
  int w1 = display.getStrWidth(t1);
  display.drawStr((DISP_W - w1) / 2, 20, t1);

  display.setFont(u8g2_font_5x8_tr);
  const char* t2 = "Based on Flock You";
  int w2 = display.getStrWidth(t2);
  display.drawStr((DISP_W - w2) / 2, 34, t2);

  const char* t3 = "32 OUI Signatures";
  int w3 = display.getStrWidth(t3);
  display.drawStr((DISP_W - w3) / 2, 48, t3);

  const char* t4 = "Initializing...";
  int w4 = display.getStrWidth(t4);
  display.drawStr((DISP_W - w4) / 2, 62, t4);

  display.sendBuffer();
}

// ============================================================
// TICK FUNCTIONS
// ============================================================

static void autosaveTick() {
  if (!fyFsReady || !fyDirty) return;
  if (millis() - fyLastSaveAt < AUTOSAVE_INTERVAL_MS) return;
  fySaveSession();
}

static void heartbeatTick() {
  if (fyLastTargetSeen == 0) return;
  unsigned long now = millis();
  if (now - fyLastTargetSeen > HB_DEVICE_ACTIVE_MS) return;
  if (now - fyLastHbBeepAt < HB_BEEP_INTERVAL_MS) return;
  heartbeatBeep();
  fyLastHbBeepAt = now;
}

static void screenTick() {
  unsigned long now = millis();

  if (alertFlashUntil > 0 && now >= alertFlashUntil) {
    alertFlashUntil = 0;
    screenDirty = true;
  }

  if (now - lastScreenRefresh >= SCREEN_REFRESH_MS) {
    screenDirty = true;
    lastScreenRefresh = now;
  }

  if (screenDirty) {
    drawScreen();
    screenDirty = false;
  }
}

static void printHeartbeat() {
  if (millis() - lastHeartbeat >= HEARTBEAT_MS) {
    Serial.printf("[fy] ch=%u det=%d heap=%u\n",
                  currentChannel, fyDetCount, ESP.getFreeHeap());
    lastHeartbeat = millis();
  }
}

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(100);

  Wire.begin(OLED_SDA, OLED_SCL);
  display.begin();
  display.setFont(u8g2_font_5x8_tr);
  drawSplash();

#if USE_BUZZER
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
#endif

  startupBeep();

  precompileOuis();
  memset(dedupeTable, 0, sizeof(dedupeTable));

  if (LittleFS.begin()) {
    fyFsReady = true;
    Serial.println("[fy] LittleFS ready");
    fyPromotePrevSession();
  } else {
    Serial.println("[fy] LittleFS FAILED — formatting");
    if (LittleFS.format() && LittleFS.begin()) {
      fyFsReady = true;
      Serial.println("[fy] LittleFS formatted OK");
    }
  }

  delay(1200);

  WiFi.mode(WIFI_OFF);
  wifi_set_opmode(STATION_MODE);
  wifi_promiscuous_enable(0);
  wifi_set_promiscuous_rx_cb(promiscCallback);
  wifi_promiscuous_enable(1);

  applyInitialChannel();

  uptimeStart       = millis();
  lastHeartbeat     = millis();
  fyLastSaveAt      = millis();
  lastScreenRefresh = 0;
  screenDirty       = true;

  Serial.println("[fy] OLED edition started");
  Serial.printf("[fy] ouis=%d heap=%u\n", (int)OUI_COUNT, ESP.getFreeHeap());
}

// ============================================================
// LOOP
// ============================================================

void loop() {
  updateChannelMode();
  drainAlertQueue();
  autosaveTick();
  heartbeatTick();
  screenTick();
  printHeartbeat();
  delay(1);
}
