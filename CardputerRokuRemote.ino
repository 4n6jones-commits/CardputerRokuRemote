#include "M5Cardputer.h"
#include "CardWifiSetup.h"
#include <WiFiUdp.h>

// Flip to 1 and reflash to get connect/keypress logging over USB serial.
#define REMOTE_DEBUG 0
#if REMOTE_DEBUG
  #define DBG_PRINTF(...) Serial.printf(__VA_ARGS__)
  #define DBG_PRINTLN(...) Serial.println(__VA_ARGS__)
#else
  #define DBG_PRINTF(...)
  #define DBG_PRINTLN(...)
#endif

// This clones the RC-AL2's *function*, not its radio: real Roku remotes pair
// over a proprietary RF link this board has no compatible radio for. Instead
// this drives Roku's External Control Protocol (ECP) - the same local-network
// HTTP API the official Roku mobile app uses. Requires the Roku to have
// Settings > System > Advanced system settings > Control by mobile apps
// enabled (default on), and both devices on the same WiFi network.
#define ROKU_PORT 8060
#define ROKU_CONNECT_TIMEOUT 800
#define ROKU_RESPONSE_TIMEOUT 1200
#define SSDP_LOCAL_PORT 1901
#define SSDP_SEARCH_TIMEOUT 3000

#define HEADER_H 16
#define DEBOUNCE_DELAY 220

// Roku-brand-inspired palette (dark purple app look, not the literal brand
// asset) - these are plain 24-bit hex, same pattern CardputerRadio's Theme
// struct already uses successfully with M5GFX's draw calls.
#define BG_DARK      0x1A0B2EUL
#define HEADER_PURPLE 0x662D91UL
#define BTN_PURPLE   0x8654B0UL
#define FLASH_OK     0xFFFFFFUL
#define FLASH_BAD    0xE5484DUL
#define WIFI_GOOD    0x34C759UL
#define WIFI_BAD     0xE5484DUL

String rokuIP;
unsigned long lastButtonPress = 0;

enum Element {
  EL_NONE, EL_UP, EL_DOWN, EL_LEFT, EL_RIGHT, EL_OK,
  EL_BACK, EL_HOME, EL_OPTIONS, EL_REW, EL_PLAY, EL_FF, EL_REPLAY
};
Element flashElement = EL_NONE;
bool flashOk = true;
unsigned long flashUntil = 0;

void loadRokuIP() {
  preferences.begin(NVS_NAMESPACE, true);
  rokuIP = preferences.getString("roku_ip", "");
  preferences.end();
}

void saveRokuIP(const String& ip) {
  preferences.begin(NVS_NAMESPACE, false);
  preferences.putString("roku_ip", ip);
  preferences.end();
  rokuIP = ip;
}

// One SSDP M-SEARCH broadcast, listening on the same socket for unicast
// replies (per SSDP convention responses go back to the sender's port, not
// the multicast group). Fixed 512-byte stack buffer - this board has no
// PSRAM, so a heap allocation isn't worth it for a one-shot discovery.
bool findRokuOnNetwork(String& outIp) {
  WiFiUDP udp;
  if (!udp.begin(SSDP_LOCAL_PORT)) return false;

  udp.beginPacket(IPAddress(239, 255, 255, 250), 1900);
  udp.print("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 3\r\nST: roku:ecp\r\n\r\n");
  udp.endPacket();

  char buf[512];
  unsigned long start = millis();
  bool found = false;

  while (millis() - start < SSDP_SEARCH_TIMEOUT) {
    int sz = udp.parsePacket();
    if (sz > 0) {
      int len = udp.read(buf, sizeof(buf) - 1);
      if (len < 0) len = 0;
      buf[len] = '\0';
      String resp(buf);

      int idx = resp.indexOf("LOCATION:");
      if (idx < 0) idx = resp.indexOf("Location:");

      if (idx >= 0) {
        int lineEnd = resp.indexOf("\r\n", idx);
        String loc = resp.substring(idx + 9, lineEnd < 0 ? resp.length() : lineEnd);
        loc.trim();

        int schemeEnd = loc.indexOf("//");
        if (schemeEnd >= 0) {
          int hostStart = schemeEnd + 2;
          int portColon = loc.indexOf(':', hostStart);
          int pathSlash = loc.indexOf('/', hostStart);
          int hostEnd = portColon >= 0 ? portColon : (pathSlash >= 0 ? pathSlash : loc.length());
          outIp = loc.substring(hostStart, hostEnd);
          found = true;
          break;
        }
      }
    }
    delay(20);
  }

  udp.stop();
  return found;
}

// Fresh TCP connection per keypress, same pattern hasCaptivePortal() in
// CardWifiSetup.h uses - raw WiFiClient instead of HTTPClient to avoid its
// extra RAM overhead on a board with no PSRAM. Roku's ECP server has no
// problem with a new connection per request; this is how the official app
// and every open-source ECP client do it.
bool rokuKeypress(const char* cmd) {
  if (rokuIP.isEmpty()) return false;

  WiFiClient client;
  if (!client.connect(rokuIP.c_str(), ROKU_PORT, ROKU_CONNECT_TIMEOUT)) {
    return false;
  }

  String path = "/keypress/";
  path += cmd;

  // Roku's ECP server validates the Host header against its own ip:port
  // (an anti DNS-rebinding protection) and returns 403 Forbidden if it
  // doesn't match exactly - the bare IP alone isn't enough, the port must
  // be included too.
  client.print("POST " + path + " HTTP/1.1\r\n");
  client.print("Host: " + rokuIP + ":" + String(ROKU_PORT) + "\r\n");
  client.print("Content-Length: 0\r\n");
  client.print("Connection: close\r\n\r\n");

  unsigned long start = millis();
  while (client.connected() && !client.available() && millis() - start < ROKU_RESPONSE_TIMEOUT) {
    delay(5);
  }

  String statusLine = client.readStringUntil('\n');
#if REMOTE_DEBUG
  DBG_PRINTF("[roku] ip=%s POST %s -> %s\n", rokuIP.c_str(), path.c_str(), statusLine.c_str());
  while (client.available()) {
    String h = client.readStringUntil('\n');
    DBG_PRINTF("[roku]   %s\n", h.c_str());
  }
#endif
  client.stop();

  return statusLine.indexOf("200") >= 0;
}

void drawMainScreen();

// Sends the command and lights up the on-screen button that was pressed -
// white on success, red on failure - instead of a separate status line.
// loop() clears the flash ~180ms later via another drawMainScreen() call.
void pressAndShow(const char* cmd, Element el) {
  flashElement = el;
  flashOk = rokuKeypress(cmd);
  flashUntil = millis() + 180;
  drawMainScreen();
}

// Single keypress does the whole job: try auto-discovery first, only ask
// for manual entry if that fails. Works the same whether or not a Roku IP
// is already saved, so it doubles as "switch to a different Roku."
void deviceSetupFlow() {
  M5Cardputer.Display.fillRect(0, HEADER_H, 240, 135 - HEADER_H, BG_DARK);
  M5Cardputer.Display.setTextColor(TFT_WHITE, BG_DARK);
  M5Cardputer.Display.drawCentreString("Finding Roku...", 120, 50);

  String ip;
  if (findRokuOnNetwork(ip)) {
    saveRokuIP(ip);
    M5Cardputer.Display.fillRect(0, HEADER_H, 240, 135 - HEADER_H, BG_DARK);
    M5Cardputer.Display.drawCentreString("Found!", 120, 40);
    M5Cardputer.Display.drawCentreString(ip, 120, 60);
    delay(1000);
  } else {
    M5Cardputer.Display.fillRect(0, HEADER_H, 240, 135 - HEADER_H, BG_DARK);
    M5Cardputer.Display.drawCentreString("Not found.", 120, 30);
    M5Cardputer.Display.drawCentreString("Enter Roku IP:", 120, 50);
    String ipIn = inputText("> ", 4, 80);
    ipIn.trim();
    if (ipIn.length() > 0) saveRokuIP(ipIn);
  }
  drawMainScreen();
}

void drawHeader() {
  M5Cardputer.Display.fillRect(0, 0, 240, HEADER_H, HEADER_PURPLE);
  M5Cardputer.Display.setTextColor(TFT_WHITE, HEADER_PURPLE);
  M5Cardputer.Display.drawString("Roku Remote", 4, 1);

  bool wifiUp = WiFi.status() == WL_CONNECTED;
  M5Cardputer.Display.fillCircle(228, 8, 4, wifiUp ? WIFI_GOOD : WIFI_BAD);
}

uint32_t elementColor(Element e) {
  if (flashElement == e) return flashOk ? FLASH_OK : FLASH_BAD;
  return BTN_PURPLE;
}

void drawDpad() {
  const int cx = 120, cy = 54;
  const int armW = 20, armLen = 18, gap = 12, okR = 14;

  M5Cardputer.Display.fillRect(0, HEADER_H, 240, 92 - HEADER_H, BG_DARK);

  M5Cardputer.Display.fillRoundRect(cx - armW / 2, cy - gap - armLen, armW, armLen, 5, elementColor(EL_UP));
  M5Cardputer.Display.fillRoundRect(cx - armW / 2, cy + gap, armW, armLen, 5, elementColor(EL_DOWN));
  M5Cardputer.Display.fillRoundRect(cx - gap - armLen, cy - armW / 2, armLen, armW, 5, elementColor(EL_LEFT));
  M5Cardputer.Display.fillRoundRect(cx + gap, cy - armW / 2, armLen, armW, 5, elementColor(EL_RIGHT));

  M5Cardputer.Display.fillCircle(cx, cy, okR, elementColor(EL_OK));
  M5Cardputer.Display.drawCircle(cx, cy, okR, TFT_WHITE);
  M5Cardputer.Display.setTextColor(TFT_WHITE, elementColor(EL_OK));
  M5Cardputer.Display.drawCentreString("OK", cx, cy - 6);
}

void drawPill(int x, int y, int w, int h, const char* label, Element e) {
  uint32_t fill = elementColor(e);
  M5Cardputer.Display.fillRoundRect(x, y, w, h, 4, fill);
  M5Cardputer.Display.setTextColor(TFT_WHITE, fill);
  M5Cardputer.Display.drawCentreString(label, x + w / 2, y + 1);
}

void drawPillRow() {
  const int y = 92, h = 16;
  M5Cardputer.Display.fillRect(0, y, 240, h, BG_DARK);
  drawPill(4, y, 72, h, "BACK", EL_BACK);
  drawPill(84, y, 72, h, "HOME", EL_HOME);
  drawPill(164, y, 72, h, "*", EL_OPTIONS);
}

void drawTransportRow() {
  const int y = 110, h = 18, w = 56, gap = 2;
  M5Cardputer.Display.fillRect(0, y, 240, h, BG_DARK);
  int x = 2;
  drawPill(x, y, w, h, "REW", EL_REW);  x += w + gap;
  drawPill(x, y, w, h, "PLAY", EL_PLAY); x += w + gap;
  drawPill(x, y, w, h, "FF", EL_FF);    x += w + gap;
  drawPill(x, y, w, h, "RPLY", EL_REPLAY);
}

void drawBanner(const char* line1, const char* line2) {
  M5Cardputer.Display.fillRect(0, HEADER_H, 240, 135 - HEADER_H, BG_DARK);
  M5Cardputer.Display.setTextColor(TFT_WHITE, BG_DARK);
  M5Cardputer.Display.drawCentreString(line1, 120, 50);
  M5Cardputer.Display.drawCentreString(line2, 120, 70);
}

// Redraws everything below the header from scratch every call - cheap
// (plain vector fills, no sprites) and means every state change (WiFi
// dropping, a button flash starting/ending) just goes through one path
// instead of juggling partial-redraw bookkeeping.
void drawMainScreen() {
  drawHeader();

  if (WiFi.status() != WL_CONNECTED) {
    drawBanner("WiFi Disconnected", "Press W to connect");
    return;
  }
  if (rokuIP.isEmpty()) {
    drawBanner("Roku not set up", "Press N to find it");
    return;
  }

  drawDpad();
  drawPillRow();
  drawTransportRow();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  DBG_PRINTLN("=== CardputerRokuRemote boot ===");

  // Cardputer ADV: GPIO5 must be driven HIGH before any I2C activity, or the
  // TCA8418 keyboard controller's I2C bus fails with ack-wait errors.
  pinMode(5, OUTPUT);
  digitalWrite(5, HIGH);

  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);

  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setFont(&fonts::FreeMonoOblique9pt7b);
  M5Cardputer.Display.setBrightness(150);
  M5Cardputer.Display.fillScreen(BG_DARK);

  loadRokuIP();
  connectToWiFi();

  drawMainScreen();
}

void loop() {
  M5Cardputer.update();

  if (M5Cardputer.Keyboard.isChange() && (millis() - lastButtonPress > DEBOUNCE_DELAY)) {
    if (M5Cardputer.Keyboard.isPressed()) {

      // D-pad: ';' '.' ',' '/' is the established D-pad convention on this
      // board's other sketches (their rough plus-shape position on the
      // physical key layout stands in for dedicated arrow keys, which this
      // keyboard doesn't have) - 'u'/'d'/'l'/'r' work as more obvious
      // letter aliases for the same four directions.
      if      (M5Cardputer.Keyboard.isKeyPressed(';') || M5Cardputer.Keyboard.isKeyPressed('u')) pressAndShow("Up", EL_UP);
      else if (M5Cardputer.Keyboard.isKeyPressed('.') || M5Cardputer.Keyboard.isKeyPressed('d')) pressAndShow("Down", EL_DOWN);
      else if (M5Cardputer.Keyboard.isKeyPressed(',') || M5Cardputer.Keyboard.isKeyPressed('l')) pressAndShow("Left", EL_LEFT);
      else if (M5Cardputer.Keyboard.isKeyPressed('/') || M5Cardputer.Keyboard.isKeyPressed('r')) pressAndShow("Right", EL_RIGHT);
      else if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) pressAndShow("Select", EL_OK);
      else if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) pressAndShow("Back", EL_BACK);

      else if (M5Cardputer.Keyboard.isKeyPressed('h')) pressAndShow("Home", EL_HOME);
      else if (M5Cardputer.Keyboard.isKeyPressed('o')) pressAndShow("Info", EL_OPTIONS);
      else if (M5Cardputer.Keyboard.isKeyPressed('[')) pressAndShow("Rev", EL_REW);
      else if (M5Cardputer.Keyboard.isKeyPressed('f')) pressAndShow("Fwd", EL_FF);
      else if (M5Cardputer.Keyboard.isKeyPressed('p')) pressAndShow("Play", EL_PLAY);
      else if (M5Cardputer.Keyboard.isKeyPressed('i')) pressAndShow("InstantReplay", EL_REPLAY);

      else if (M5Cardputer.Keyboard.isKeyPressed('w')) { openWiFiMenu(); drawMainScreen(); }
      else if (M5Cardputer.Keyboard.isKeyPressed('n')) { deviceSetupFlow(); }

      lastButtonPress = millis();
    }
  }

  if (flashElement != EL_NONE && millis() > flashUntil) {
    flashElement = EL_NONE;
    drawMainScreen();
  }

  // Catches WiFi dropping/reconnecting mid-use so the banner appears and
  // clears on its own - skipped while a button flash is actively showing so
  // it can't stomp on that frame.
  static unsigned long lastRefresh = 0;
  if (flashElement == EL_NONE && millis() - lastRefresh > 1000) {
    lastRefresh = millis();
    drawMainScreen();
  }

  delay(1);
}
