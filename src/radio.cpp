#include "radio.h"

#include <WiFi.h>
#include <SD.h>

#include "config.h"

namespace {

uint8_t s_state = radio::OFF;
uint32_t s_t0 = 0;
constexpr uint32_t kConnectTimeoutMs = 15000;
constexpr int kMaxStations = 100;
int s_httpsSkipped = 0;

String trimStr(const String& s) {
    int a = 0, b = (int)s.length();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substring(a, b);
}

String stripSep(const String& s) {
    int b = (int)s.length();
    while (b > 0 && (s[b - 1] == '|' || s[b - 1] == ';' || s[b - 1] == ',' || s[b - 1] == '\t' || s[b - 1] == ' ')) b--;
    return s.substring(0, b);
}

String hostFromUrl(const String& url) {
    int p = url.indexOf("://");
    String rest = (p >= 0) ? url.substring(p + 3) : url;
    int e = rest.length();
    for (int i = 0; i < e; i++) {
        if (rest[i] == '/' || rest[i] == ':') { e = i; break; }
    }
    String h = rest.substring(0, e);
    if (h.startsWith("www.")) h = h.substring(4);
    return h;
}

void addStation(std::vector<player::RadioStation>& out, const String& name, const String& url) {
    if (url.length() == 0 || (int)out.size() >= kMaxStations) return;
    String low = url;
    low.toLowerCase();
    if (low.startsWith("https")) { s_httpsSkipped++; return; }  // https TLS stalls without PSRAM: skip
    for (auto& s : out) {
        if (s.url == url) return;
    }
    player::RadioStation st;
    st.name = name.length() ? name : hostFromUrl(url);
    st.url = url;
    out.push_back(st);
}

void parseLine(const String& raw, String& pendingName, std::vector<player::RadioStation>& out) {
    String line = trimStr(raw);
    if (line.length() == 0) return;
    if (line[0] == '#') {
        if (line.startsWith("#EXTINF")) {
            int c = line.lastIndexOf(',');
            if (c >= 0) pendingName = stripSep(trimStr(line.substring(c + 1)));
        }
        return;
    }

    int start = 0;
    while (start <= (int)line.length()) {
        int comma = line.indexOf(',', start);
        String part = (comma < 0) ? line.substring(start) : line.substring(start, comma);
        part = trimStr(part);

        int hp = part.indexOf("http");
        if (hp >= 0) {
            String url = trimStr(part.substring(hp));
            String pre = stripSep(trimStr(part.substring(0, hp)));
            String name = pre.length() ? pre : pendingName;
            pendingName = "";
            addStation(out, name, url);
        } else if (part.length()) {
            pendingName = stripSep(part);
        }

        if (comma < 0) break;
        start = comma + 1;
    }
}

}

namespace radio {

bool enter() {
    File f = SD.open("/wifi.txt");
    if (!f) {
        s_state = FAILED;
        return false;
    }
    char ssid[64] = {0};
    char pass[64] = {0};
    int n = f.readBytesUntil('\n', ssid, sizeof(ssid) - 1);
    ssid[n > 0 ? n : 0] = 0;
    if (n > 0 && ssid[n - 1] == '\r') ssid[n - 1] = 0;
    int m = f.readBytesUntil('\n', pass, sizeof(pass) - 1);
    pass[m > 0 ? m : 0] = 0;
    if (m > 0 && pass[m - 1] == '\r') pass[m - 1] = 0;
    f.close();

    String s = trimStr(String(ssid));
    if (!s.length()) {
        s_state = FAILED;
        return false;
    }

    WiFi.persistent(false);
    CM_LOG("[RADIO] wifi init heap=%u max=%u\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(s.c_str(), pass);
    s_state = CONNECTING;
    s_t0 = millis();
    CM_LOG("[RADIO] wifi begin ssid='%s' passlen=%u\n", s.c_str(), (unsigned)strlen(pass));
    return true;
}

void leave() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    s_state = OFF;
    CM_LOG("[RADIO] wifi off\n");
}

void tick() {
    if (s_state == CONNECTING) {
        if (WiFi.status() == WL_CONNECTED) {
            s_state = CONNECTED;
            CM_LOG("[RADIO] wifi connected ip=%s\n", WiFi.localIP().toString().c_str());
        } else if (millis() - s_t0 > kConnectTimeoutMs) {
            s_state = FAILED;
            s_t0 = millis();
            CM_LOG("[RADIO] wifi timeout status=%d\n", (int)WiFi.status());
        }
    } else if (s_state == FAILED) {
        if (WiFi.status() == WL_CONNECTED) {
            s_state = CONNECTED;
            CM_LOG("[RADIO] wifi connected ip=%s\n", WiFi.localIP().toString().c_str());
        } else if (millis() - s_t0 > 30000) {
            s_t0 = millis();
            WiFi.reconnect();
            CM_LOG("[RADIO] wifi retry status=%d\n", (int)WiFi.status());
        }
    }
}

uint8_t status() { return s_state; }

const char* statusText() {
    switch (s_state) {
        case CONNECTING: return "WiFi: conectando...";
        case CONNECTED:  return "WiFi OK";
        case FAILED:     return "WiFi: falhou";
        default:         return "";
    }
}

bool loadStations(std::vector<player::RadioStation>& out) {
    out.clear();
    s_httpsSkipped = 0;
    File f = SD.open("/radio.txt");
    if (!f) return false;
    String pendingName;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        parseLine(line, pendingName, out);
    }
    f.close();
    CM_LOG("[RADIO] %u estacoes (%d https ignoradas)\n", (unsigned)out.size(), s_httpsSkipped);
    return out.size() > 0;
}

int httpsSkipped() { return s_httpsSkipped; }

}
