#include "M5Cardputer.h"
#include "CardWifiSetup.h"
#include <WiFiUdp.h>
#include <lwip/sockets.h>
#include <fcntl.h>

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
#define SCAN_BATCH 8
#define SCAN_CONNECT_TIMEOUT 250

#define HEADER_H 16
#define DEBOUNCE_DELAY 220
#define REPEAT_DELAY 450
#define REPEAT_INTERVAL 160
#define TEXT_SHOWN_MAX 64

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

// Which full-screen layout is currently on the display. drawMainScreen()
// only clears the background when this changes, so routine redraws (button
// flashes, the 1s WiFi check) repaint elements in place instead of wiping
// the whole panel purple-black first - that wipe was the visible flicker.
enum Screen { SCR_NONE, SCR_NO_WIFI, SCR_NO_ROKU, SCR_REMOTE, SCR_TEXT };
Screen drawnScreen = SCR_NONE;
bool drawnWifiUp = false;

// Typing mode (Tab toggles): every printable key goes to the Roku as an ECP
// Lit_ keypress, so search boxes can be typed into directly. textShown is
// only a local echo of what was sent - the Roku has no API to read back the
// contents of its on-screen keyboard.
bool textMode = false;
String textShown;
bool textSendFailed = false;

// Hold-to-repeat for the D-pad: the key that started the hold, the ECP
// command it sends, and when the next repeat is due.
char heldKey = 0;
char heldAlias = 0;
const char* heldCmd = nullptr;
Element heldEl = EL_NONE;
unsigned long nextRepeat = 0;

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

      // HTTP header names are case-insensitive - match on a lowercased copy
      // but slice the value out of the original (same offsets).
      String lower = resp;
      lower.toLowerCase();
      int idx = lower.indexOf("location:");

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

// Fresh TCP connection per request, same pattern hasCaptivePortal() in
// CardWifiSetup.h uses - raw WiFiClient instead of HTTPClient to avoid its
// extra RAM overhead on a board with no PSRAM. Roku's ECP server has no
// problem with a new connection per request; this is how the official app
// and every open-source ECP client do it. If body is non-null, up to
// bodyMax bytes of the response (headers included) are copied into it.
bool rokuRequest(const String& ip, const char* method, const String& path,
                 char* body = nullptr, size_t bodyMax = 0) {
  if (ip.isEmpty()) return false;

  WiFiClient client;
  if (!client.connect(ip.c_str(), ROKU_PORT, ROKU_CONNECT_TIMEOUT)) {
    return false;
  }

  // Roku's ECP server validates the Host header against its own ip:port
  // (an anti DNS-rebinding protection) and returns 403 Forbidden if it
  // doesn't match exactly - the bare IP alone isn't enough, the port must
  // be included too.
  client.print(String(method) + " " + path + " HTTP/1.1\r\n");
  client.print("Host: " + ip + ":" + String(ROKU_PORT) + "\r\n");
  client.print("Content-Length: 0\r\n");
  client.print("Connection: close\r\n\r\n");

  unsigned long start = millis();
  while (client.connected() && !client.available() && millis() - start < ROKU_RESPONSE_TIMEOUT) {
    delay(5);
  }

  String statusLine = client.readStringUntil('\n');
  DBG_PRINTF("[roku] ip=%s %s %s -> %s\n", ip.c_str(), method, path.c_str(), statusLine.c_str());

  if (body && bodyMax > 0) {
    size_t len = 0;
    start = millis();
    while (len < bodyMax - 1 && millis() - start < ROKU_RESPONSE_TIMEOUT &&
           (client.connected() || client.available())) {
      if (client.available()) body[len++] = client.read();
      else delay(5);
    }
    body[len] = '\0';
  }
  client.stop();

  return statusLine.indexOf("200") >= 0;
}

bool rokuKeypress(const char* cmd) {
  return rokuRequest(rokuIP, "POST", String("/keypress/") + cmd);
}

// Port 8060 being open is a strong hint but not proof, so confirm with the
// device-info query (every Roku answers it, nothing else is likely to
// return a <device-info> document on that port).
bool isRokuAt(const String& ip) {
  char buf[384];
  if (!rokuRequest(ip, "GET", "/query/device-info", buf, sizeof(buf))) return false;
  return strstr(buf, "<device-info>") != nullptr;
}

// Fallback for networks where SSDP multicast doesn't get through (seen on a
// NetworkManager hotspot, and some older Rokus never answer it): try a TCP
// connect to port 8060 on every host in this /24. Plain blocking connects
// would take minutes, so this opens SCAN_BATCH non-blocking sockets at a
// time and waits on all of them together - about 254/8 * 250ms worst case.
// Only the local /24 is covered even on a wider subnet; anything bigger
// would be too slow to be useful here.
bool scanSubnetForRoku(String& outIp) {
  IPAddress local = WiFi.localIP();

  for (int first = 1; first < 255; first += SCAN_BATCH) {
    M5Cardputer.Display.fillRect(0, 70, 240, 18, BG_DARK);
    M5Cardputer.Display.drawCentreString(String(local[0]) + "." + local[1] + "." + local[2] + "." + first, 120, 70);

    int fds[SCAN_BATCH];
    int hosts[SCAN_BATCH];
    bool done[SCAN_BATCH];
    int n = 0;

    for (int h = first; h < first + SCAN_BATCH && h < 255; h++) {
      if (h == local[3]) continue;
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) break;
      fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

      sockaddr_in addr = {};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(ROKU_PORT);
      addr.sin_addr.s_addr = htonl(((uint32_t)local[0] << 24) | ((uint32_t)local[1] << 16) |
                                   ((uint32_t)local[2] << 8) | (uint32_t)h);
      connect(fd, (sockaddr*)&addr, sizeof(addr));

      fds[n] = fd;
      hosts[n] = h;
      done[n] = false;
      n++;
    }

    int openHosts[SCAN_BATCH];
    int openCount = 0;
    unsigned long start = millis();

    while (millis() - start < SCAN_CONNECT_TIMEOUT) {
      fd_set writable;
      FD_ZERO(&writable);
      int maxFd = -1;
      for (int i = 0; i < n; i++) {
        if (done[i]) continue;
        FD_SET(fds[i], &writable);
        if (fds[i] > maxFd) maxFd = fds[i];
      }
      if (maxFd < 0) break;

      timeval tv = { 0, 20000 };
      if (select(maxFd + 1, nullptr, &writable, nullptr, &tv) <= 0) continue;

      for (int i = 0; i < n; i++) {
        if (done[i] || !FD_ISSET(fds[i], &writable)) continue;
        done[i] = true;
        int err = 0;
        socklen_t errLen = sizeof(err);
        getsockopt(fds[i], SOL_SOCKET, SO_ERROR, &err, &errLen);
        if (err == 0) openHosts[openCount++] = hosts[i];
      }
    }

    for (int i = 0; i < n; i++) close(fds[i]);

    for (int i = 0; i < openCount; i++) {
      String ip = String(local[0]) + "." + local[1] + "." + local[2] + "." + openHosts[i];
      DBG_PRINTF("[scan] 8060 open on %s\n", ip.c_str());
      if (isRokuAt(ip)) {
        outIp = ip;
        return true;
      }
    }
  }
  return false;
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

// Single keypress does the whole job: try SSDP auto-discovery first, then
// the port scan, and only ask for manual entry if both fail. Works the same
// whether or not a Roku IP is already saved, so it doubles as "switch to a
// different Roku."
void deviceSetupFlow() {
  drawnScreen = SCR_NONE;  // this flow paints over everything
  M5Cardputer.Display.fillRect(0, HEADER_H, 240, 135 - HEADER_H, BG_DARK);
  M5Cardputer.Display.setTextColor(TFT_WHITE, BG_DARK);
  M5Cardputer.Display.drawCentreString("Finding Roku...", 120, 50);

  String ip;
  bool found = findRokuOnNetwork(ip);
  if (!found) {
    M5Cardputer.Display.fillRect(0, HEADER_H, 240, 135 - HEADER_H, BG_DARK);
    M5Cardputer.Display.drawCentreString("Scanning network...", 120, 50);
    found = scanSubnetForRoku(ip);
  }

  if (found) {
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

void drawHeader(bool wifiUp) {
  M5Cardputer.Display.fillRect(0, 0, 240, HEADER_H, HEADER_PURPLE);
  M5Cardputer.Display.setTextColor(TFT_WHITE, HEADER_PURPLE);
  M5Cardputer.Display.drawString(textMode ? "Roku Keyboard" : "Roku Remote", 4, 1);
  M5Cardputer.Display.fillCircle(228, 8, 4, wifiUp ? WIFI_GOOD : WIFI_BAD);
}

uint32_t elementColor(Element e) {
  if (flashElement == e) return flashOk ? FLASH_OK : FLASH_BAD;
  return BTN_PURPLE;
}

void drawDpad() {
  const int cx = 120, cy = 54;
  const int armW = 20, armLen = 18, gap = 12, okR = 14;

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
  drawPill(4, y, 72, h, "BACK", EL_BACK);
  drawPill(84, y, 72, h, "HOME", EL_HOME);
  drawPill(164, y, 72, h, "*", EL_OPTIONS);
}

void drawTransportRow() {
  const int y = 110, h = 18, w = 56, gap = 2;
  int x = 2;
  drawPill(x, y, w, h, "REW", EL_REW);  x += w + gap;
  drawPill(x, y, w, h, "PLAY", EL_PLAY); x += w + gap;
  drawPill(x, y, w, h, "FF", EL_FF);    x += w + gap;
  drawPill(x, y, w, h, "RPLY", EL_REPLAY);
}

void drawBanner(const char* line1, const char* line2) {
  M5Cardputer.Display.setTextColor(TFT_WHITE, BG_DARK);
  M5Cardputer.Display.drawCentreString(line1, 120, 50);
  M5Cardputer.Display.drawCentreString(line2, 120, 70);
}

// Echo of what's been typed, tail-trimmed so the cursor end stays visible.
// Red if the last character didn't reach the Roku.
void drawTextLine() {
  const int y = 58, maxChars = 19;
  String tail = textShown.length() > maxChars
              ? textShown.substring(textShown.length() - maxChars)
              : textShown;
  M5Cardputer.Display.fillRoundRect(6, y - 4, 228, 24, 4, BTN_PURPLE);
  M5Cardputer.Display.setTextColor(textSendFailed ? FLASH_BAD : FLASH_OK, BTN_PURPLE);
  M5Cardputer.Display.drawString(tail + "_", 12, y);
}

void drawTextScreen() {
  M5Cardputer.Display.setTextColor(TFT_WHITE, BG_DARK);
  M5Cardputer.Display.drawCentreString("Type into Roku search", 120, 26);
  drawTextLine();
  M5Cardputer.Display.drawCentreString("Del=erase Enter=go", 120, 92);
  M5Cardputer.Display.drawCentreString("Tab = back to remote", 120, 112);
}

Screen currentScreen() {
  if (WiFi.status() != WL_CONNECTED) return SCR_NO_WIFI;
  if (rokuIP.isEmpty()) return SCR_NO_ROKU;
  return textMode ? SCR_TEXT : SCR_REMOTE;
}

// One path for every redraw. The background is only cleared when the layout
// actually changes; otherwise elements are repainted in place (they fully
// cover their own area, so nothing stale is left behind).
void drawMainScreen() {
  Screen scr = currentScreen();
  bool wifiUp = scr != SCR_NO_WIFI;

  if (scr != drawnScreen || wifiUp != drawnWifiUp) drawHeader(wifiUp);
  if (scr != drawnScreen) {
    M5Cardputer.Display.fillRect(0, HEADER_H, 240, 135 - HEADER_H, BG_DARK);
  }

  switch (scr) {
    case SCR_NO_WIFI: drawBanner("WiFi Disconnected", "Press W to connect"); break;
    case SCR_NO_ROKU: drawBanner("Roku not set up", "Press N to find it"); break;
    case SCR_TEXT:    drawTextScreen(); break;
    default:
      drawDpad();
      drawPillRow();
      drawTransportRow();
  }

  drawnScreen = scr;
  drawnWifiUp = wifiUp;
}

// ECP wants each literal character URL-encoded after "Lit_" (UTF-8, but the
// Cardputer keyboard only produces ASCII).
void sendLiteral(char c) {
  char cmd[12];
  if (isalnum((unsigned char)c)) snprintf(cmd, sizeof(cmd), "Lit_%c", c);
  else snprintf(cmd, sizeof(cmd), "Lit_%%%02X", (unsigned char)c);

  textSendFailed = !rokuKeypress(cmd);
  if (!textSendFailed && textShown.length() < TEXT_SHOWN_MAX) textShown += c;
}

void handleTextModeKeys() {
  Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

  if (status.tab) {
    textMode = false;
  } else if (status.enter) {
    textSendFailed = !rokuKeypress("Enter");
    if (!textSendFailed) textShown = "";
  } else if (status.del) {
    textSendFailed = !rokuKeypress("Backspace");
    if (!textSendFailed && textShown.length() > 0) textShown.remove(textShown.length() - 1);
  } else {
    for (char c : status.word) sendLiteral(c);
  }
  drawMainScreen();
}

// Starts a press on a D-pad key and arms hold-to-repeat for it.
void startHold(char key, char alias, const char* cmd, Element el) {
  pressAndShow(cmd, el);
  heldKey = key;
  heldAlias = alias;
  heldCmd = cmd;
  heldEl = el;
  nextRepeat = millis() + REPEAT_DELAY;
}

bool keyHeld(char key, char alias) {
  return M5Cardputer.Keyboard.isKeyPressed(key) || M5Cardputer.Keyboard.isKeyPressed(alias);
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

  // Typing mode skips the debounce - it would drop fast typing, and each
  // character already costs a full HTTP round trip anyway.
  if (textMode && currentScreen() == SCR_TEXT) {
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      handleTextModeKeys();
    }
  } else if (M5Cardputer.Keyboard.isChange() && (millis() - lastButtonPress > DEBOUNCE_DELAY)) {
    if (!M5Cardputer.Keyboard.isPressed()) heldKey = 0;
    if (M5Cardputer.Keyboard.isPressed()) {

      // D-pad: ';' '.' ',' '/' is the established D-pad convention on this
      // board's other sketches (their rough plus-shape position on the
      // physical key layout stands in for dedicated arrow keys, which this
      // keyboard doesn't have) - 'u'/'d'/'l'/'r' work as more obvious
      // letter aliases for the same four directions.
      if      (keyHeld(';', 'u')) startHold(';', 'u', "Up", EL_UP);
      else if (keyHeld('.', 'd')) startHold('.', 'd', "Down", EL_DOWN);
      else if (keyHeld(',', 'l')) startHold(',', 'l', "Left", EL_LEFT);
      else if (keyHeld('/', 'r')) startHold('/', 'r', "Right", EL_RIGHT);
      else if (M5Cardputer.Keyboard.isKeyPressed(KEY_TAB) && currentScreen() == SCR_REMOTE) {
        textMode = true;
        textShown = "";
        textSendFailed = false;
        drawMainScreen();
      }
      else if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) pressAndShow("Select", EL_OK);
      else if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) pressAndShow("Back", EL_BACK);

      else if (M5Cardputer.Keyboard.isKeyPressed('h')) pressAndShow("Home", EL_HOME);
      else if (M5Cardputer.Keyboard.isKeyPressed('o')) pressAndShow("Info", EL_OPTIONS);
      else if (M5Cardputer.Keyboard.isKeyPressed('[')) pressAndShow("Rev", EL_REW);
      else if (M5Cardputer.Keyboard.isKeyPressed('f')) pressAndShow("Fwd", EL_FF);
      else if (M5Cardputer.Keyboard.isKeyPressed('p')) pressAndShow("Play", EL_PLAY);
      else if (M5Cardputer.Keyboard.isKeyPressed('i')) pressAndShow("InstantReplay", EL_REPLAY);

      else if (M5Cardputer.Keyboard.isKeyPressed('w')) { openWiFiMenu(); drawnScreen = SCR_NONE; drawMainScreen(); }
      else if (M5Cardputer.Keyboard.isKeyPressed('n')) { deviceSetupFlow(); }

      lastButtonPress = millis();
    }
  }

  // Hold-to-repeat: keep stepping while the D-pad key that started the hold
  // is still down, so scrolling a long row doesn't need one tap per tile.
  if (heldKey && !textMode) {
    if (!keyHeld(heldKey, heldAlias)) {
      heldKey = 0;
    } else if (millis() >= nextRepeat) {
      pressAndShow(heldCmd, heldEl);
      nextRepeat = millis() + REPEAT_INTERVAL;
      lastButtonPress = millis();
    }
  }

  if (flashElement != EL_NONE && millis() > flashUntil) {
    flashElement = EL_NONE;
    drawMainScreen();
  }

  // Catches WiFi dropping/reconnecting mid-use so the banner appears and
  // clears on its own. Only redraws when the layout would actually change,
  // so an idle remote no longer repaints itself every second.
  static unsigned long lastRefresh = 0;
  if (flashElement == EL_NONE && millis() - lastRefresh > 1000) {
    lastRefresh = millis();
    if (currentScreen() != drawnScreen) drawMainScreen();
  }

  delay(1);
}
