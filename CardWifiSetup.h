#include <WiFi.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <vector>

#define NVS_NAMESPACE "M5_settings"
#define MIN_WIFI_RSSI -80
#define MAX_NETWORKS 10
#define MAX_SAVED_WIFI 5
#define WIFI_TIMEOUT 9000
#define OPEN_NETWORK_TIMEOUT 5000
#define MAX_OPEN_ATTEMPTS 4
#define PORTAL_CHECK_TIMEOUT 3000
#define MAX_PORTAL_BLACKLIST 5
#define PORTAL_RETRY_COOLDOWN (10UL * 60UL * 1000UL)

void openWiFiMenu();
bool tryKnownAndOpenNetworks();
void saveWiFiAt(int index, const String& ssid, const String& pass);

Preferences preferences;

struct SavedWiFi {
    String ssid;
    String pass;
    int32_t rssi;
};

SavedWiFi saved[MAX_SAVED_WIFI];
int savedCount = 0;
int lastUsed = -1;

// True only when currently connected via the auto-roam open-network
// fallback (tryOpenNetworks()) rather than a saved/known network. Drives
// the on-screen public-network indicator - reset to false on every
// saved-network or manual-menu connect so it always reflects reality.
bool onPublicNetwork = false;

struct WiFiNetwork {
    String ssid;
    int32_t rssi;
    wifi_auth_mode_t encryption;
};
std::vector<WiFiNetwork> networks;

// SSIDs of open networks recently found to be behind a captive portal -
// skipped for PORTAL_RETRY_COOLDOWN so the auto-roam logic below doesn't
// keep re-scanning/re-joining/re-probing the same dead network (McDonald's,
// airport, etc.) every cycle and burning battery on a connection that's
// already known not to work.
struct BlacklistedNetwork {
    String ssid;
    unsigned long until;
};
BlacklistedNetwork portalBlacklist[MAX_PORTAL_BLACKLIST];
int portalBlacklistCount = 0;

int scrollX = 0;

void loadSavedWiFi() {
    preferences.begin(NVS_NAMESPACE, true);
    savedCount = 0;
    lastUsed = preferences.getInt("wifi_last", -1);

    for (int i = 0; i < MAX_SAVED_WIFI; i++) {
        char key[24];

        snprintf(key, sizeof(key), "wifi_%d_ssid", i + 1);
        String ssid = preferences.getString(key, "");

        snprintf(key, sizeof(key), "wifi_%d_pass", i + 1);
        String pass = preferences.getString(key, "");

        if (!ssid.isEmpty()) {
            saved[savedCount++] = { ssid, pass, -100 };
        }
    }
    preferences.end();
}

String inputText(const String& prompt, int x, int y, bool isPassword = false) {

    String data;
    data.reserve(64);

    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setTextScroll(true);
    M5Cardputer.Display.drawString(prompt, x, y);

    while (true) {
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isChange()) {
            if (M5Cardputer.Keyboard.isPressed()) {

                Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

                for (auto i : status.word) {
                    if (data.length() < 63) data += i;
                }

                if (status.del && data.length() > 0) {
                    data.remove(data.length() - 1);
                }

                if (status.enter) {
                    return data;
                }

                M5Cardputer.Display.fillRect(0, y - 4, M5Cardputer.Display.width(), 25, BLACK);

                String display;
                display.reserve(66);
                display = "> ";

                if (isPassword) {
                    for (size_t i = 0; i < data.length(); i++) display += '*';
                } else {
                    display += data;
                }

                M5Cardputer.Display.drawString(display, 4, y);

            }
        }
        delay(10);
    }
}

void displayWiFiInfo() {
    M5Cardputer.Display.fillRect(0, 20, 240, 135, BLACK);
    M5Cardputer.Display.setCursor(1, 1);
    M5Cardputer.Display.drawString("Connected to Wi-Fi", 1, 1);
    M5Cardputer.Display.drawString("SSID: " + WiFi.SSID(), 1, 18);
    M5Cardputer.Display.drawString("IP: " + WiFi.localIP().toString(), 1, 33);
    int8_t rssi = WiFi.RSSI();
    M5Cardputer.Display.drawString("RSSI: " + String(rssi) + " dBm", 1, 48);
    delay(2000);
    M5Cardputer.Display.fillRect(0, 0, 240, 135, BLACK);
}

bool fastConnect(const String& ssid, const String& pass, unsigned long timeoutMs = WIFI_TIMEOUT) {
    M5Cardputer.Display.clear();
    WiFi.begin(ssid.c_str(), pass.c_str());

    unsigned long start = millis();
    unsigned long lastDot = 0;
    int dots = 0;

    while (millis() - start < timeoutMs) {
        M5Cardputer.update();

        if (WiFi.status() == WL_CONNECTED) {
            return true;
        }

        if (millis() - lastDot > 350) {
            lastDot = millis();
            dots = (dots + 1) % 4;

            M5Cardputer.Display.fillRect(20, 60, 200, 16, BLACK);

            String line = "Connecting";
            for (int i = 0; i < dots; i++) line += ".";

            M5Cardputer.Display.drawString(line, 20, 60);
        }

        vTaskDelay(1);
    }

    WiFi.disconnect(true, true);
    delay(150);

    return false;
}

// Picks the strongest saved network out of an already-completed scan of n
// results (WiFi.SSID(i)/RSSI(i)/... over 0..n-1) rather than scanning
// itself, so callers that also need the open-network candidates from the
// same scan (tryKnownAndOpenNetworks() below) don't pay for two full
// WiFi.scanNetworks() passes back to back - each one is 2-4s of blocking
// time on its own.
int findBestSavedNetworkFromScan(int n) {
    int best = -1;
    int bestRSSI = MIN_WIFI_RSSI;

    for (int i = 0; i < n; i++) {
        for (int s = 0; s < savedCount; s++) {
            if (WiFi.SSID(i) == saved[s].ssid) {
                saved[s].rssi = WiFi.RSSI(i);
                if (saved[s].rssi > bestRSSI) {
                    bestRSSI = saved[s].rssi;
                    best = s;
                }
            }
        }
    }

    return best;
}

bool isBlacklisted(const String& ssid) {
    for (int i = 0; i < portalBlacklistCount; i++) {
        if (portalBlacklist[i].ssid == ssid) {
            if (millis() < portalBlacklist[i].until) return true;
            // cooldown expired - drop the entry so it gets a fresh try
            portalBlacklist[i] = portalBlacklist[--portalBlacklistCount];
            return false;
        }
    }
    return false;
}

void blacklistNetwork(const String& ssid) {
    if (portalBlacklistCount >= MAX_PORTAL_BLACKLIST) portalBlacklistCount = 0;
    portalBlacklist[portalBlacklistCount++] = { ssid, millis() + PORTAL_RETRY_COOLDOWN };
}

// Standard captive-portal probe (same endpoint Android uses): a clean
// connection answers this exact path with a bare 204. A portal gateway
// intercepts it and returns something else - a redirect, an HTML login
// page, or nothing at all before it times out. Either way, "not a clean
// 204" means the network can't be used for streaming, since this device
// has no way to render/submit an arbitrary login page.
bool hasCaptivePortal() {
    WiFiClient client;
    if (!client.connect("connectivitycheck.gstatic.com", 80, PORTAL_CHECK_TIMEOUT)) {
        return true;
    }

    client.print("GET /generate_204 HTTP/1.1\r\nHost: connectivitycheck.gstatic.com\r\nConnection: close\r\n\r\n");

    unsigned long start = millis();
    while (client.connected() && !client.available() && millis() - start < PORTAL_CHECK_TIMEOUT) {
        delay(10);
    }

    String statusLine = client.readStringUntil('\n');
    client.stop();

    return statusLine.indexOf("204") < 0;
}

// Joins the first clean open (no-password) network out of an already-
// completed scan of n results, strongest signal first. Only ever called
// after saved networks have already been tried and failed against the
// same scan - home/known networks always take priority. Portal-gated
// SSIDs get blacklisted so a dead network (e.g. a McDonald's or airport
// portal) doesn't get re-tried and re-probed every cycle.
bool tryOpenNetworksFromScan(int n) {
    if (n <= 0) return false;

    std::vector<WiFiNetwork> candidates;
    for (int i = 0; i < n; i++) {
        if (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) continue;
        String ssid = WiFi.SSID(i);
        if (ssid.isEmpty() || isBlacklisted(ssid)) continue;
        candidates.push_back({ ssid, WiFi.RSSI(i), WIFI_AUTH_OPEN });
    }
    if (candidates.empty()) return false;

    std::sort(candidates.begin(), candidates.end(),
              [](const WiFiNetwork& a, const WiFiNetwork& b) { return a.rssi > b.rssi; });

    int attempts = 0;
    for (auto& net : candidates) {
        if (++attempts > MAX_OPEN_ATTEMPTS) break;

        if (!fastConnect(net.ssid, "", OPEN_NETWORK_TIMEOUT)) continue;

        if (hasCaptivePortal()) {
            blacklistNetwork(net.ssid);
            WiFi.disconnect(true, true);
            continue;
        }

        onPublicNetwork = true;
        return true;
    }

    return false;
}

// Saved networks first (unchanged priority), then nearby open networks as
// a fallback - this is the whole "don't make me think about it in a new
// place" auto-roam behavior. Returns false only when neither worked, at
// which point the caller decides whether to fall back to the manual menu.
bool tryKnownAndOpenNetworks() {
    loadSavedWiFi();

    // Try the last-used network first without a scan - usually right, and
    // skips a scan entirely in the common "still in the same place" case.
    if (savedCount > 0 && lastUsed >= 0 && lastUsed < savedCount) {
        if (fastConnect(saved[lastUsed].ssid, saved[lastUsed].pass)) {
            onPublicNetwork = false;
            return true;
        }
    }

    // One scan from here on serves both the "best remaining saved network"
    // and "open network fallback" passes below, instead of each running
    // its own - that was costing an extra 2-4s of blocking scan time on
    // every cycle where the last-used network wasn't reachable.
    WiFi.scanDelete();
    int n = WiFi.scanNetworks(false, false);

    if (savedCount > 0 && n > 0) {
        int best = findBestSavedNetworkFromScan(n);
        // Don't retry lastUsed - it was already just tried above and
        // failed, and it's frequently also the RSSI-strongest saved
        // network (you were just connected to it), which would otherwise
        // waste another full WIFI_TIMEOUT retrying a network already known
        // not to work right now.
        if (best >= 0 && best != lastUsed) {
            if (fastConnect(saved[best].ssid, saved[best].pass)) {
                saveWiFiAt(best, saved[best].ssid, saved[best].pass);
                onPublicNetwork = false;
                return true;
            }
        }
    }

    return tryOpenNetworksFromScan(n);
}

int selectSavedToReplace() {

    String line;
    line.reserve(64);

    int sel = 0;
    int scrollX = 0;
    int lastSel = -1;

    M5Cardputer.update();
    delay(150);

    M5Cardputer.Display.clear();
    M5Cardputer.Display.drawString("Replace WiFi:", 2, 2);

    while (true) {
        if (sel != lastSel) {
            scrollX = 0;
            lastSel = sel;
        }

        for (int i = 0; i < savedCount; i++) {
            int y = 20 + i * 18;

            M5Cardputer.Display.fillRect(0, y, 240, 18, BLACK);

            line = (i == sel ? "> " : "  ");
            line += saved[i].ssid;
            int w = M5Cardputer.Display.textWidth(line);

            if (i == sel && w > 230) {
                scrollX++;
                if (scrollX > w) scrollX = 0;
                M5Cardputer.Display.drawString(line, 2 - scrollX, y);
            } else {
                M5Cardputer.Display.drawString(line, 2, y);
            }
        }

        M5Cardputer.Display.fillRect(0, 108, 240, 18, BLACK);
        M5Cardputer.Display.drawString("ENTER = OK", 2, 110);

        M5Cardputer.update();

        if (M5Cardputer.Keyboard.isChange()) {
            if (M5Cardputer.Keyboard.isPressed()) {

                if (M5Cardputer.Keyboard.isKeyPressed(';') && sel > 0) {
                    sel--;
                }

                if (M5Cardputer.Keyboard.isKeyPressed('.') && sel < savedCount - 1) {
                    sel++;
                }

                if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
                    return sel;
                }
            }
        }

        delay(10);
    }
}

void saveWiFiAt(int index, const String& ssid, const String& pass) {
    preferences.begin(NVS_NAMESPACE, false);

    char key[24];

    snprintf(key, sizeof(key), "wifi_%d_ssid", index + 1);
    preferences.putString(key, ssid);

    snprintf(key, sizeof(key), "wifi_%d_pass", index + 1);
    preferences.putString(key, pass);

    preferences.putInt("wifi_last", index);
    preferences.end();
}

String getSecurityString(wifi_auth_mode_t encType) {
    switch(encType) {
        case WIFI_AUTH_OPEN: return "Open";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        default: return "---";
    }
}

String scanAndDisplayNetworks() {
    scrollX = 0;

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true, true);
    delay(200);

    WiFi.scanDelete();
    WiFi.scanNetworks(true, true);

    M5Cardputer.Display.clear();
    M5Cardputer.Display.drawString("Wi-Fi scanning", 1, 1);

    int16_t scanResult;
    do {
        scanResult = WiFi.scanComplete();
        delay(100);
    } while(scanResult == WIFI_SCAN_RUNNING);

    if (scanResult == 0) {
        M5Cardputer.Display.drawString("No networks found.", 1, 15);
        delay(2000);
        return "";
    }

    networks.clear();
    // Collect every scan result before sorting by RSSI below - truncating
    // to MAX_NETWORKS here first (as this used to) could throw away the
    // actual strongest network if the driver's scan order happened to list
    // it after the 10th entry. The visible list is already scrollable, so
    // there's no need to cap how many get stored.
    networks.reserve(min((int)scanResult, MAX_NETWORKS));
    for (int i = 0; i < scanResult; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0 || ssid.length() > 32) continue;

        networks.push_back({
            ssid,
            WiFi.RSSI(i),
            WiFi.encryptionType(i)
        });
    }

    if (networks.empty()) {
        M5Cardputer.Display.drawString("No usable networks", 1, 20);
        delay(2000);
        return "";
    }

    std::sort(networks.begin(), networks.end(),
             [](const WiFiNetwork& a, const WiFiNetwork& b) {
                 return a.rssi > b.rssi;
             });

    M5Cardputer.Display.clear();
    M5Cardputer.Display.drawString("Available networks:", 1, 1);

    const int NETWORK_VISIBLE = 4; // rows that fit between the header and the footer prompt
    int selectedNetwork = 0;
    int lastSelectedLocal = -1;
    while (true) {
        if (selectedNetwork != lastSelectedLocal) {
            scrollX = 0;
            lastSelectedLocal = selectedNetwork;
        }

        int start = selectedNetwork - NETWORK_VISIBLE / 2;
        if (start < 0) start = 0;
        if (start > (int)networks.size() - NETWORK_VISIBLE)
            start = max(0, (int)networks.size() - NETWORK_VISIBLE);

        M5Cardputer.Display.fillRect(0, 18, 240, 18 * NETWORK_VISIBLE, BLACK);

        for (int row = 0; row < NETWORK_VISIBLE; row++) {
            int i = start + row;
            if (i >= (int)networks.size()) break;

            int y = 18 + row * 18;
            bool selected = (i == selectedNetwork);

            String line;
            line.reserve(96);
            line = selected ? "-> " : "   ";
            line += networks[i].ssid;
            line += " (";
            line += networks[i].rssi;
            line += "dBm ";
            line += getSecurityString(networks[i].encryption);
            line += ")";

            int textWidth = M5Cardputer.Display.textWidth(line);

            if (selected && textWidth > 230) {
                scrollX++;
                if (scrollX > textWidth) scrollX = 0;
                M5Cardputer.Display.drawString(line, 1 - scrollX, y);
            } else {
                M5Cardputer.Display.drawString(line, 1, y);
            }
        }

        M5Cardputer.Display.fillRect(0, 108, 240, 18, BLACK);
        M5Cardputer.Display.drawString("Select ENTER: OK", 1, 108);
        M5Cardputer.update();

        if (M5Cardputer.Keyboard.isChange()) {
            if (M5Cardputer.Keyboard.isPressed()) {

                if (M5Cardputer.Keyboard.isKeyPressed(';') && selectedNetwork > 0) {
                    selectedNetwork--;
                }
                if (M5Cardputer.Keyboard.isKeyPressed('.') &&
                    selectedNetwork < networks.size() - 1) {
                    selectedNetwork++;
                }
                if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
                    scrollX = 0;
                    return networks[selectedNetwork].ssid;
                }
            }
        }
        delay(10);
    }
}

void resetWiFiSettings() {
    M5Cardputer.Display.clear();
    M5Cardputer.Display.drawString("Reset WiFi settings", 20, 50);
    M5Cardputer.Display.drawString("Please wait...", 20, 70);

    preferences.begin(NVS_NAMESPACE, false);

    for (int i = 0; i < MAX_SAVED_WIFI; i++) {

        char key[24];

        snprintf(key, sizeof(key), "wifi_%d_ssid", i + 1);
        preferences.remove(key);

        snprintf(key, sizeof(key), "wifi_%d_pass", i + 1);
        preferences.remove(key);

    }
    preferences.remove("wifi_last");

    preferences.end();

    WiFi.disconnect(true, true);
    delay(200);
    esp_restart();
}

void connectToWiFi() {

    unsigned long resetStart = millis();
    bool resetShown = false;

    while (millis() - resetStart < 1000) {
        M5Cardputer.update();

        if (!resetShown) {
            M5Cardputer.Display.clear();
            M5Cardputer.Display.drawString("Hold BtnG0 to", 40, 50);
            M5Cardputer.Display.drawString("reset WiFi", 50, 70);
            resetShown = true;
        }

        if (M5Cardputer.BtnA.isPressed()) {
            resetWiFiSettings();
            return;
        }

        delay(10);
    }

    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);

    WiFi.setSleep(false);

    if (tryKnownAndOpenNetworks()) {
        displayWiFiInfo();
        return;
    }

    openWiFiMenu();
}

// Manual network scan -> select -> password entry -> save -> connect.
// Split out of connectToWiFi() so it can also be triggered on demand at
// runtime (bound to a key), not just when no saved network works at boot.
void openWiFiMenu() {
    M5Cardputer.Display.clear();

    String ssid = scanAndDisplayNetworks();
    if (ssid.isEmpty()) return;

    M5Cardputer.Display.clear();
    M5Cardputer.Display.drawString("Password:", 2, 40);
    String pass = inputText("> ", 4, 110, true);
    M5Cardputer.Display.clear();

    int idx;

    if (savedCount < MAX_SAVED_WIFI) {
        idx = savedCount;
        savedCount++;
    } else {
        idx = selectSavedToReplace();
        M5Cardputer.Display.clear();
    }

    saveWiFiAt(idx, ssid, pass);

    if (fastConnect(ssid, pass)) {
        onPublicNetwork = false;
        displayWiFiInfo();
    }
}
