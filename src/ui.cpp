#include "ui.h"
#include "config.h"
#include "player.h"
#include "browser.h"
#include "radio.h"
#include <M5Unified.h>

namespace {

M5Canvas s_canvas(&M5.Display);
#ifndef CM_APP_RADIO
alignas(16) uint16_t s_canvasBuf[240 * 135];  // static 16bpp canvas: never fails, never fragments
#endif
lgfx::LovyanGFX* s_gfx = &s_canvas;  // draw target: sprite (player) or display (radio)
bool s_lowMem = false;

struct Theme {
    const char* name;
    uint16_t bg, primary, accent, text, dim, track, sel, dir;
};

const Theme s_themes[3] = {
    { "Terminal",    0x0000, 0x37EC, 0x9FF3, 0xCFF9, 0x1BC6, 0x0922, 0x11E3, 0x9FF3 },
    { "Now Playing", 0x0842, 0xAC1F, 0x5EFD, 0xFFFF, 0x8C53, 0x2106, 0x38AB, 0xC9B4 },
    { "Tape Deck",   0x1040, 0xFD80, 0xFED0, 0xFF58, 0x8B02, 0x3101, 0x4981, 0xFED0 },
};
uint8_t s_themeIdx = 1;
const Theme& TH() { return s_themes[s_themeIdx]; }

uint32_t s_lastDraw = 0;
uint32_t s_lastSpec = 0;
bool s_invalid = true;

uint32_t s_volToastEnd = 0;
uint8_t s_volToastVal = 0;
uint32_t s_msgToastEnd = 0;
char s_msgToast[28] = "";

int s_marqueeX = 0;
uint32_t s_marqueeLast = 0;

constexpr int SPEC_N = 28;
uint8_t s_spec[SPEC_N];
uint32_t s_seed = 0x12345678;

int s_bat = -1;
uint32_t s_batNext = 0;

uint8_t s_reelStep = 0;

bool s_staOpen = false;
int s_staSel = 0;
int s_staTop = 0;

#ifdef CM_APP_RADIO
// CardRadio region-update caches (ASCII direct draw)
bool s_fullDirty = true;
char cStation[48] = "";
char cTitle[96] = "";
char cBadge[56] = "";
char cStatus[32] = "";
uint32_t cSec = 0xFFFFFFFF;
int cIdx = -1, cCount = -1, cPaused = -1, cLive = -1;
bool cToast = false;
int cListOpen = -1, cListSel = -1, cListTop = -1, cListCount = -1;
#endif

const int8_t s_cos12[12] = { 100, 87, 50, 0, -50, -87, -100, -87, -50, 0, 50, 87 };
const int8_t s_sin12[12] = { 0, 50, 87, 100, 87, 50, 0, -50, -87, -100, -87, -50 };

uint16_t lerp565(uint16_t a, uint16_t b, uint8_t t) {
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int rr = ar + ((br - ar) * t) / 255;
    int rg = ag + ((bg - ag) * t) / 255;
    int rb = ab + ((bb - ab) * t) / 255;
    return (uint16_t)((rr << 11) | (rg << 5) | rb);
}

void fmtTime(char* out, size_t n, uint32_t sec) {
    snprintf(out, n, "%u:%02u", (unsigned)(sec / 60), (unsigned)(sec % 60));
}

void utf8Pop(String& s) {
    if (s.length() == 0) return;
    int i = s.length() - 1;
    while (i > 0 && (((uint8_t)s[i] & 0xC0) == 0x80)) i--;
    s.remove(i);
}

String fitLeft(const String& s, int maxW) {
    if (s_gfx->textWidth(s.c_str()) <= maxW) return s;
    String out = s;
    while (out.length() > 1 && s_gfx->textWidth(("." + out).c_str()) > maxW) utf8Pop(out);
    return "..." + out;
}

String truncRight(const String& s, int maxW) {
    if (s_gfx->textWidth(s.c_str()) <= maxW) return s;
    String out = s;
    while (out.length() > 1 && s_gfx->textWidth((out + ".").c_str()) > maxW) utf8Pop(out);
    return out + ".";
}

void drawBattery() {
    int x = 194, y = 3;
    s_gfx->drawRect(x, y, 16, 9, TH().dim);
    s_gfx->fillRect(x + 16, y + 2, 2, 5, TH().dim);
    char buf[8];
    if (s_bat >= 0) {
        int w = (14 * s_bat) / 100;
        if (w > 0) {
            s_gfx->fillRect(x + 1, y + 1, w, 7, s_bat > 25 ? TH().accent : TH().primary);
        }
        snprintf(buf, sizeof(buf), "%d%%", s_bat);
    } else {
        snprintf(buf, sizeof(buf), "--%%");
    }
    s_gfx->setTextDatum(textdatum_t::top_right);
    s_gfx->setTextColor(TH().dim);
    s_gfx->drawString(buf, 240 - 4, 2);
    s_gfx->setTextDatum(textdatum_t::top_left);
}

void drawHeader() {
    s_gfx->setFont(&fonts::efontJA_10);
    s_gfx->setTextDatum(textdatum_t::top_left);
    s_gfx->setTextColor(TH().primary);
    s_gfx->drawString("CARDPLAYER", 4, 2);
    drawBattery();
}

void drawVolToast() {
    int x = 78, y = 52, w = 84, h = 28;
    s_gfx->fillRoundRect(x, y, w, h, 4, TH().track);
    s_gfx->drawRoundRect(x, y, w, h, 4, TH().primary);
    char buf[16];
    snprintf(buf, sizeof(buf), "VOL %u", (unsigned)s_volToastVal);
    s_gfx->setFont(&fonts::efontJA_10);
    s_gfx->setTextColor(TH().text);
    s_gfx->setTextDatum(textdatum_t::middle_center);
    s_gfx->drawString(buf, x + w / 2, y + 9);
    s_gfx->drawRect(x + 8, y + 19, w - 16, 4, TH().dim);
    if (s_volToastVal > 0) {
        int fw = (w - 18) * s_volToastVal / cfg::VOLUME_MAX;
        if (fw > 0) s_gfx->fillRect(x + 9, y + 20, fw, 2, TH().accent);
    }
    s_gfx->setTextDatum(textdatum_t::middle_center);
}

void drawMsgToast() {
    int w = 150, h = 26, x = (240 - w) / 2, y = 52;
    s_gfx->fillRoundRect(x, y, w, h, 4, TH().track);
    s_gfx->drawRoundRect(x, y, w, h, 4, TH().primary);
    s_gfx->setFont(&fonts::efontJA_10);
    s_gfx->setTextColor(TH().text);
    s_gfx->setTextDatum(textdatum_t::middle_center);
    s_gfx->drawString(s_msgToast, x + w / 2, y + h / 2 + 1);
    s_gfx->setTextDatum(textdatum_t::top_left);
}

int marqueeScroll(const String& title) {
    int tw = s_gfx->textWidth(title.c_str());
    if (tw <= 232) {
        s_marqueeX = 0;
        return 0;
    }
    uint32_t now = millis();
    if (now - s_marqueeLast >= 40) {
        s_marqueeX += 2;
        s_marqueeLast = now;
    }
    int span = tw + 60;
    if (s_marqueeX > span) s_marqueeX = 0;
    return s_marqueeX;
}

float trackFrac(const PlayerState& st) {
    if (!st.duration) return 0;
    float f = (float)st.position / (float)st.duration;
    if (f > 1) f = 1;
    return f;
}

void drawSpectrum(int sx, int sy, int bw, int gap) {
    for (int i = 0; i < SPEC_N; i++) {
        int h = s_spec[i];
        if (h < 2) h = 2;
        int x = sx + i * (bw + gap);
        uint8_t t = (uint8_t)(h * 8 > 255 ? 255 : h * 8);
        uint16_t c = lerp565(TH().primary, TH().accent, t);
        s_gfx->fillRect(x, sy - h, bw, h, c);
    }
}

void drawNowPlayingA() {
    const PlayerState& st = player::state();
    s_gfx->fillScreen(TH().bg);
    drawHeader();

    s_gfx->setFont(&fonts::efontJA_10);
    s_gfx->setTextColor(TH().dim);
    if (st.active) {
        int i = st.path.lastIndexOf('/');
        String folder = i > 0 ? st.path.substring(0, i) : String("/sd");
        String vis = fitLeft("> " + folder, 232);
        s_gfx->drawString(vis.c_str(), 4, 16);
    } else {
        s_gfx->drawString("> nenhuma musica - B abre a lista", 4, 16);
    }

    s_gfx->setFont(&fonts::efontJA_16);
    String title = st.active ? st.title : String("CARDPLAYER");
    int scroll = marqueeScroll(title);
    s_gfx->setTextColor(TH().text);
    s_gfx->setClipRect(4, 28, 232, 22);
    s_gfx->drawString(title.c_str(), 4 - scroll, 29);
    s_gfx->clearClipRect();

    s_gfx->setFont(&fonts::efontJA_10);
    s_gfx->setTextColor(TH().dim);
    s_gfx->drawString(st.artist.c_str(), 4, 52);
    if (st.count > 0) {
        char idx[24];
        snprintf(idx, sizeof(idx), "%d/%d", st.index + 1, st.count);
        s_gfx->setTextDatum(textdatum_t::top_right);
        s_gfx->setTextColor(TH().primary);
        s_gfx->drawString(idx, 236, 52);
        s_gfx->setTextDatum(textdatum_t::top_left);
    }

    float frac = trackFrac(st);
    char bar[32];
    int k = (int)(24 * frac);
    bar[0] = '[';
    for (int i = 0; i < 24; i++) bar[1 + i] = (i < k) ? '#' : '.';
    bar[25] = ']';
    bar[26] = 0;
    s_gfx->setTextColor(TH().primary);
    s_gfx->drawString(bar, 8, 66);
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", (int)(frac * 100));
    s_gfx->setTextDatum(textdatum_t::top_right);
    s_gfx->drawString(pct, 232, 66);
    s_gfx->setTextDatum(textdatum_t::top_left);

    char cur[12], dur[12];
    fmtTime(cur, sizeof(cur), st.position);
    fmtTime(dur, sizeof(dur), st.duration);
    String line = String(cur) + " / " + String(dur);
    s_gfx->setTextColor(TH().dim);
    s_gfx->drawString(line.c_str(), 8, 80);

    drawSpectrum(8, 120, 4, 4);

    if (st.paused && st.active) {
        s_gfx->setFont(&fonts::efontJA_16);
        s_gfx->setTextDatum(textdatum_t::middle_center);
        s_gfx->setTextColor(TH().primary);
        s_gfx->drawString("PAUSA", 120, 101);
        s_gfx->setTextDatum(textdatum_t::top_left);
    }

    char badge[56];
    if (st.active && st.codec.length()) {
        snprintf(badge, sizeof(badge), "%s  %.1fkHz  %lukbps",
                 st.codec.c_str(), st.sampleRate / 1000.0,
                 (unsigned long)(st.bitrate / 1000));
    } else if (st.active) {
        snprintf(badge, sizeof(badge), "lendo...");
    } else {
        badge[0] = 0;
    }
    s_gfx->setFont(&fonts::efontJA_10);
    s_gfx->setTextColor(TH().dim);
    s_gfx->drawString(badge, 4, 124);
    s_gfx->setTextDatum(textdatum_t::top_right);
    s_gfx->drawString("SPC  B  V", 236, 124);
    s_gfx->setTextDatum(textdatum_t::top_left);

    if (millis() < s_volToastEnd) drawVolToast();
    else if (millis() < s_msgToastEnd) drawMsgToast();
}

void drawNowPlayingB() {
    const PlayerState& st = player::state();
    s_gfx->fillScreen(TH().bg);
    drawHeader();

    s_gfx->setFont(&fonts::efontJA_10);
    s_gfx->setTextColor(TH().dim);
    if (st.active) {
        int i = st.path.lastIndexOf('/');
        String folder = i > 0 ? st.path.substring(0, i) : String("/sd");
        String vis = fitLeft(folder, 232);
        s_gfx->drawString(vis.c_str(), 4, 16);
    } else {
        s_gfx->drawString("Nenhuma musica - aperte B para abrir a lista", 4, 16);
    }

    s_gfx->setFont(&fonts::efontJA_16);
    String title = st.active ? st.title : String("CARDPLAYER");
    int scroll = marqueeScroll(title);
    s_gfx->setTextColor(TH().text);
    s_gfx->setClipRect(4, 28, 232, 22);
    s_gfx->drawString(title.c_str(), 4 - scroll, 29);
    s_gfx->clearClipRect();

    s_gfx->setFont(&fonts::efontJA_10);
    s_gfx->setTextColor(TH().accent);
    s_gfx->drawString(st.artist.c_str(), 4, 52);
    if (st.count > 0) {
        char idx[24];
        snprintf(idx, sizeof(idx), "%d/%d", st.index + 1, st.count);
        s_gfx->setTextDatum(textdatum_t::top_right);
        s_gfx->setTextColor(TH().primary);
        s_gfx->drawString(idx, 236, 52);
        s_gfx->setTextDatum(textdatum_t::top_left);
    }

    int px = 8, py = 66, pw = 224, ph = 8;
    s_gfx->fillRoundRect(px, py, pw, ph, 4, TH().track);
    float frac = trackFrac(st);
    int fw = (int)(pw * frac);
    if (fw > 0) {
        s_gfx->fillRoundRect(px, py, fw, ph, 4, TH().primary);
        int kx = px + fw;
        if (kx > px + pw - 4) kx = px + pw - 4;
        if (kx < px + 4) kx = px + 4;
        s_gfx->fillCircle(kx, py + ph / 2, 5, TH().accent);
    }

    char cur[12], rem[12];
    fmtTime(cur, sizeof(cur), st.position);
    uint32_t rd = st.duration > st.position ? st.duration - st.position : 0;
    fmtTime(rem, sizeof(rem), rd);
    s_gfx->setTextColor(TH().dim);
    s_gfx->drawString(cur, px, 78);
    String rems = "-" + String(rem);
    s_gfx->setTextDatum(textdatum_t::top_right);
    s_gfx->drawString(rems.c_str(), px + pw, 78);
    s_gfx->setTextDatum(textdatum_t::top_left);

    drawSpectrum(8, 120, 4, 4);

    if (st.paused && st.active) {
        s_gfx->setFont(&fonts::efontJA_16);
        s_gfx->setTextDatum(textdatum_t::middle_center);
        s_gfx->setTextColor(TH().primary);
        s_gfx->drawString("PAUSA", 120, 101);
        s_gfx->setTextDatum(textdatum_t::top_left);
    }

    char badge[56];
    if (st.active && st.codec.length()) {
        snprintf(badge, sizeof(badge), "%s  %.1fkHz  %lukbps",
                 st.codec.c_str(), st.sampleRate / 1000.0,
                 (unsigned long)(st.bitrate / 1000));
    } else if (st.active) {
        snprintf(badge, sizeof(badge), "lendo...");
    } else {
        badge[0] = 0;
    }
    s_gfx->setFont(&fonts::efontJA_10);
    s_gfx->setTextColor(TH().dim);
    s_gfx->drawString(badge, 4, 124);
    s_gfx->setTextDatum(textdatum_t::top_right);
    s_gfx->drawString("SPC  B  V", 236, 124);
    s_gfx->setTextDatum(textdatum_t::top_left);

    if (millis() < s_volToastEnd) drawVolToast();
    else if (millis() < s_msgToastEnd) drawMsgToast();
}

void drawReel(int cx, int cy) {
    s_gfx->drawCircle(cx, cy, 13, TH().primary);
    for (int k = 0; k < 3; k++) {
        int idx = (s_reelStep + k * 4) % 12;
        int x2 = cx + (s_cos12[idx] * 11) / 100;
        int y2 = cy + (s_sin12[idx] * 11) / 100;
        s_gfx->drawLine(cx, cy, x2, y2, TH().accent);
    }
    s_gfx->fillCircle(cx, cy, 4, TH().primary);
}

void drawNowPlayingC() {
    const PlayerState& st = player::state();
    s_gfx->fillScreen(TH().bg);
    drawHeader();

    s_gfx->drawRoundRect(10, 24, 220, 64, 6, TH().primary);
    s_gfx->fillRoundRect(16, 28, 208, 26, 4, TH().track);

    s_gfx->setFont(&fonts::efontJA_16);
    String title = st.active ? st.title : String("CARDPLAYER");
    int scroll = marqueeScroll(title);
    s_gfx->setTextColor(TH().text);
    s_gfx->setClipRect(18, 29, 204, 19);
    s_gfx->drawString(title.c_str(), 20 - scroll, 30);
    s_gfx->clearClipRect();

    s_gfx->setFont(&fonts::efontJA_10);
    s_gfx->setTextColor(TH().dim);
    s_gfx->setClipRect(18, 44, 204, 11);
    s_gfx->drawString(st.artist.c_str(), 20, 45);
    s_gfx->clearClipRect();

    s_gfx->drawRect(96, 58, 48, 26, TH().primary);
    float frac = trackFrac(st);
    s_gfx->fillRect(100, 68, 40, 6, TH().track);
    int wfw = (int)(40 * frac);
    if (wfw > 0) s_gfx->fillRect(100, 68, wfw, 6, TH().primary);

    drawReel(66, 71);
    drawReel(174, 71);

    char cur[12], rem[12];
    fmtTime(cur, sizeof(cur), st.position);
    uint32_t rd = st.duration > st.position ? st.duration - st.position : 0;
    fmtTime(rem, sizeof(rem), rd);
    s_gfx->setFont(&fonts::efontJA_10);
    s_gfx->setTextColor(TH().primary);
    s_gfx->drawString(cur, 14, 92);
    String rems = "-" + String(rem);
    s_gfx->setTextDatum(textdatum_t::top_right);
    s_gfx->drawString(rems.c_str(), 226, 92);
    if (st.count > 0) {
        char idx[24];
        snprintf(idx, sizeof(idx), "%d/%d", st.index + 1, st.count);
        s_gfx->setTextDatum(textdatum_t::top_center);
        s_gfx->setTextColor(TH().dim);
        s_gfx->drawString(idx, 120, 92);
    }
    s_gfx->setTextDatum(textdatum_t::top_left);

    s_gfx->fillRoundRect(14, 104, 212, 5, 2, TH().track);
    int bfw = (int)(212 * frac);
    if (bfw > 0) s_gfx->fillRoundRect(14, 104, bfw, 5, 2, TH().primary);

    if (st.paused && st.active) {
        s_gfx->fillRoundRect(88, 57, 64, 26, 4, TH().track);
        s_gfx->drawRoundRect(88, 57, 64, 26, 4, TH().primary);
        s_gfx->setFont(&fonts::efontJA_16);
        s_gfx->setTextDatum(textdatum_t::middle_center);
        s_gfx->setTextColor(TH().primary);
        s_gfx->drawString("PAUSA", 120, 71);
        s_gfx->setTextDatum(textdatum_t::top_left);
    }

    char badge[56];
    if (st.active && st.codec.length()) {
        snprintf(badge, sizeof(badge), "%s  %.1fkHz  %lukbps",
                 st.codec.c_str(), st.sampleRate / 1000.0,
                 (unsigned long)(st.bitrate / 1000));
    } else if (st.active) {
        snprintf(badge, sizeof(badge), "lendo...");
    } else {
        badge[0] = 0;
    }
    s_gfx->setFont(&fonts::efontJA_10);
    s_gfx->setTextColor(TH().dim);
    s_gfx->drawString(badge, 4, 122);
    s_gfx->setTextDatum(textdatum_t::top_right);
    s_gfx->drawString("SPC  B  V", 236, 122);
    s_gfx->setTextDatum(textdatum_t::top_left);

    if (millis() < s_volToastEnd) drawVolToast();
    else if (millis() < s_msgToastEnd) drawMsgToast();
}

void drawRadio() {
    const PlayerState& st = player::state();
    s_gfx->fillScreen(TH().bg);
    drawHeader();

    bool live = (millis() / 1000) % 2;
    s_gfx->setFont(&fonts::efontJA_10);
    const char* badgeTxt = (s_themeIdx == 2) ? "ON AIR" : "LIVE";
    int bw = (s_themeIdx == 2) ? 48 : 38;
    int bx = (s_themeIdx == 2) ? 90 : 100;
    if (live) {
        s_gfx->fillRoundRect(bx, 2, bw, 11, 2, TH().primary);
        s_gfx->setTextColor(TH().bg);
    } else {
        s_gfx->drawRoundRect(bx, 2, bw, 11, 2, TH().primary);
        s_gfx->setTextColor(TH().primary);
    }
    s_gfx->setTextDatum(textdatum_t::middle_center);
    s_gfx->drawString(badgeTxt, bx + bw / 2, 8);
    s_gfx->setTextDatum(textdatum_t::top_left);

    s_gfx->setFont(&fonts::efontJA_16);
    String station = st.artist.length() ? st.artist : String("RADIO");
    if (s_themeIdx == 0) station = "> " + station;
    String svis = truncRight(station, 232);
    s_gfx->setTextColor(TH().text);
    s_gfx->drawString(svis.c_str(), 4, 29);

    s_gfx->setFont(&fonts::efontJA_10);
    s_gfx->setTextColor(TH().accent);
    String song = truncRight(st.title, 200);
    s_gfx->drawString(song.c_str(), 4, 52);
    if (st.count > 0) {
        char idx[24];
        snprintf(idx, sizeof(idx), "%d/%d", st.index + 1, st.count);
        s_gfx->setTextDatum(textdatum_t::top_right);
        s_gfx->setTextColor(TH().primary);
        s_gfx->drawString(idx, 236, 52);
        s_gfx->setTextDatum(textdatum_t::top_left);
    }

    char cur[12];
    fmtTime(cur, sizeof(cur), st.position);
    s_gfx->setTextColor(TH().dim);
    s_gfx->drawString(cur, 8, 80);
    s_gfx->setTextDatum(textdatum_t::top_right);
    s_gfx->setTextColor(TH().primary);
    s_gfx->drawString("AO VIVO", 232, 80);
    s_gfx->setTextDatum(textdatum_t::top_left);

    if (radio::status() != radio::CONNECTED) {
        s_gfx->setTextColor(TH().primary);
        s_gfx->setTextDatum(textdatum_t::middle_center);
        s_gfx->drawString(radio::statusText(), 120, 96);
        s_gfx->setTextDatum(textdatum_t::top_left);
    } else if (st.title == "Conectando...") {
        s_gfx->setTextColor(TH().dim);
        s_gfx->setTextDatum(textdatum_t::middle_center);
        s_gfx->drawString("buffering...", 120, 96);
        s_gfx->setTextDatum(textdatum_t::top_left);
    }

    // radio screen: no animated spectrum bars (requested)

    if (st.paused && st.active) {
        s_gfx->setFont(&fonts::efontJA_16);
        s_gfx->setTextDatum(textdatum_t::middle_center);
        s_gfx->setTextColor(TH().primary);
        s_gfx->drawString("PAUSA", 120, 101);
        s_gfx->setTextDatum(textdatum_t::top_left);
    }

    char badge[56];
    if (st.sampleRate) {
        snprintf(badge, sizeof(badge), "%.1fkHz  %lukbps",
                 st.sampleRate / 1000.0, (unsigned long)(st.bitrate / 1000));
    } else {
        snprintf(badge, sizeof(badge), "buffering...");
    }
    s_gfx->setFont(&fonts::efontJA_10);
    s_gfx->setTextColor(TH().dim);
    s_gfx->drawString(badge, 4, 124);
    s_gfx->setTextDatum(textdatum_t::top_right);
    s_gfx->drawString("R sai  B lista", 236, 124);
    s_gfx->setTextDatum(textdatum_t::top_left);

    if (millis() < s_volToastEnd) drawVolToast();
    else if (millis() < s_msgToastEnd) drawMsgToast();
}

void listRow(int i) {
    const int rowH = 12;
    const int listY = 28;
    int r = i - s_staTop;
    if (r < 0 || r >= cfg::BROWSER_ROWS) return;
    const player::RadioStation* rs = player::radioStation(i);
    if (!rs) return;
    int y = listY + r * rowH;
    bool sel = (i == s_staSel);
    s_gfx->fillRect(0, y - 1, 240, rowH, sel ? TH().sel : TH().bg);
    if (sel) s_gfx->fillRoundRect(2, y - 1, 236, rowH, 3, TH().sel);
    s_gfx->setFont(&fonts::efontJA_10);
    s_gfx->setTextColor(sel ? TH().text : (i == player::radioIndex() ? TH().primary : TH().dim));
    String label = String(i == player::radioIndex() ? "* " : "  ") + rs->name;
    String vis = truncRight(label, 228);
    s_gfx->drawString(vis.c_str(), 6, y + 1);
}

void drawStationList() {
    s_gfx->fillScreen(TH().bg);
    drawHeader();

    s_gfx->setFont(&fonts::efontJA_10);
    s_gfx->setTextColor(TH().accent);
    char head[40];
    snprintf(head, sizeof(head), "RADIO - %d estacoes", player::radioCount());
    s_gfx->drawString(head, 4, 16);

    const int rowH = 12;
    const int listY = 28;
    int n = player::radioCount();
    for (int r = 0; r < cfg::BROWSER_ROWS; r++) {
        int i = s_staTop + r;
        if (i >= n) break;
        listRow(i);
    }

    if (n > cfg::BROWSER_ROWS) {
        int trackY = listY;
        int trackH = cfg::BROWSER_ROWS * rowH;
        s_gfx->fillRect(239, trackY, 1, trackH, TH().track);
        int thumbH = trackH * cfg::BROWSER_ROWS / n;
        if (thumbH < 6) thumbH = 6;
        int denom = n - cfg::BROWSER_ROWS;
        int thumbY = trackY + (denom > 0 ? (trackH - thumbH) * s_staTop / denom : 0);
        s_gfx->fillRect(238, thumbY, 2, thumbH, TH().primary);
    }

    s_gfx->setTextColor(TH().dim);
    s_gfx->drawString("ENTER toca   DEL fecha", 4, 124);

    if (millis() < s_volToastEnd) drawVolToast();
    else if (millis() < s_msgToastEnd) drawMsgToast();
}

#ifdef CM_APP_RADIO
// CardRadio: direct draw with small region repaints (no canvas, no flicker)
void radioTick(uint32_t now) {
    const PlayerState& st = player::state();
    bool listOpen = s_staOpen;

    bool toastNow = (now < s_volToastEnd) || (now < s_msgToastEnd);
    if (toastNow && !listOpen) {
        s_gfx->startWrite();
        if (now < s_volToastEnd) drawVolToast(); else drawMsgToast();
        s_gfx->endWrite();
        cToast = true;
        return;
    }
    if (cToast) {
        cToast = false;
        s_gfx->startWrite();
        s_gfx->fillRect(0, 50, 240, 14, TH().bg);
        s_gfx->fillRect(0, 78, 240, 14, TH().bg);
        s_gfx->endWrite();
        cTitle[0] = 0;
        cSec = 0xFFFFFFFF;
    }

    bool full = s_fullDirty || (listOpen != cListOpen)
        || (!listOpen && (st.index != cIdx || st.count != cCount || (int)st.paused != cPaused));
    if (full) {
        s_fullDirty = false;
        cListOpen = listOpen;
        cStation[0] = cTitle[0] = cBadge[0] = cStatus[0] = 0;
        cSec = 0xFFFFFFFF;
        cIdx = st.index;
        cCount = st.count;
        cPaused = (int)st.paused;
        cLive = -1;
        s_gfx->startWrite();
        if (listOpen) drawStationList(); else drawRadio();
        s_gfx->endWrite();
        cListSel = s_staSel;
        cListTop = s_staTop;
        cListCount = player::radioCount();
        return;
    }

    if (listOpen) {
        if (s_staSel != cListSel || s_staTop != cListTop || player::radioCount() != cListCount) {
            int oldSel = cListSel, oldTop = cListTop;
            cListSel = s_staSel;
            cListTop = s_staTop;
            cListCount = player::radioCount();
            s_gfx->startWrite();
            if (oldTop != s_staTop || oldSel < 0) drawStationList();
            else { listRow(oldSel); listRow(cListSel); }
            s_gfx->endWrite();
        }
        return;
    }

    s_gfx->startWrite();

    int live = (int)((millis() / 1000) & 1);
    if (live != cLive) {
        cLive = live;
        const char* badgeTxt = (s_themeIdx == 2) ? "ON AIR" : "LIVE";
        int bw = (s_themeIdx == 2) ? 48 : 38;
        int bx = (s_themeIdx == 2) ? 90 : 100;
        s_gfx->fillRect(bx - 1, 1, bw + 2, 13, TH().bg);
        s_gfx->setFont(&fonts::efontJA_10);
        if (live) {
            s_gfx->fillRoundRect(bx, 2, bw, 11, 2, TH().primary);
            s_gfx->setTextColor(TH().bg);
        } else {
            s_gfx->drawRoundRect(bx, 2, bw, 11, 2, TH().primary);
            s_gfx->setTextColor(TH().primary);
        }
        s_gfx->setTextDatum(textdatum_t::middle_center);
        s_gfx->drawString(badgeTxt, bx + bw / 2, 8);
        s_gfx->setTextDatum(textdatum_t::top_left);
    }

    String station = st.artist.length() ? st.artist : String("RADIO");
    if (s_themeIdx == 0) station = "> " + station;
    String svis = truncRight(station, 232);
    if (svis != cStation) {
        strlcpy(cStation, svis.c_str(), sizeof(cStation));
        s_gfx->fillRect(0, 26, 240, 24, TH().bg);
        s_gfx->setFont(&fonts::efontJA_16);
        s_gfx->setTextColor(TH().text);
        s_gfx->drawString(svis.c_str(), 4, 29);
    }

    char nm[16] = "";
    if (st.count > 0) snprintf(nm, sizeof(nm), "%d/%d", st.index + 1, st.count);
    String tv = truncRight(st.title, 200);
    char titleCache[112];
    snprintf(titleCache, sizeof(titleCache), "%s|%s", tv.c_str(), nm);
    if (strcmp(titleCache, cTitle) != 0) {
        strlcpy(cTitle, titleCache, sizeof(cTitle));
        s_gfx->fillRect(0, 50, 240, 14, TH().bg);
        s_gfx->setFont(&fonts::efontJA_10);
        s_gfx->setTextColor(TH().accent);
        s_gfx->drawString(tv.c_str(), 4, 52);
        if (nm[0]) {
            s_gfx->setTextDatum(textdatum_t::top_right);
            s_gfx->setTextColor(TH().primary);
            s_gfx->drawString(nm, 236, 52);
            s_gfx->setTextDatum(textdatum_t::top_left);
        }
    }

    if (st.position != cSec) {
        cSec = st.position;
        char cur[12];
        fmtTime(cur, sizeof(cur), st.position);
        s_gfx->fillRect(0, 78, 90, 14, TH().bg);
        s_gfx->setFont(&fonts::efontJA_10);
        s_gfx->setTextColor(TH().dim);
        s_gfx->drawString(cur, 8, 80);
    }

    const char* stat = "";
    uint16_t statCol = TH().primary;
    if (radio::status() != radio::CONNECTED) {
        stat = radio::statusText();
    } else if (st.title == "Conectando...") {
        stat = "buffering...";
        statCol = TH().dim;
    }
    if (strcmp(stat, cStatus) != 0) {
        strlcpy(cStatus, stat, sizeof(cStatus));
        s_gfx->fillRect(0, 88, 240, 16, TH().bg);
        if (stat[0]) {
            s_gfx->setFont(&fonts::efontJA_10);
            s_gfx->setTextColor(statCol);
            s_gfx->setTextDatum(textdatum_t::middle_center);
            s_gfx->drawString(stat, 120, 96);
            s_gfx->setTextDatum(textdatum_t::top_left);
        }
    }

    char badge[56];
    if (st.sampleRate) {
        snprintf(badge, sizeof(badge), "%.1fkHz  %lukbps", st.sampleRate / 1000.0, (unsigned long)(st.bitrate / 1000));
    } else {
        snprintf(badge, sizeof(badge), "buffering...");
    }
    if (strcmp(badge, cBadge) != 0) {
        strlcpy(cBadge, badge, sizeof(cBadge));
        s_gfx->fillRect(0, 121, 150, 14, TH().bg);
        s_gfx->setFont(&fonts::efontJA_10);
        s_gfx->setTextColor(TH().dim);
        s_gfx->drawString(badge, 4, 124);
    }

    s_gfx->endWrite();
}
#endif

void drawBrowser() {
    const BrowserState& bs = browser::state();
    s_gfx->fillScreen(TH().bg);
    drawHeader();

    s_gfx->setFont(&fonts::efontJA_10);
    s_gfx->setTextColor(TH().accent);
    String pvis = fitLeft(bs.path, 232);
    s_gfx->drawString(pvis.c_str(), 4, 16);

    const int rowH = 12;
    const int listY = 28;
    for (int r = 0; r < cfg::BROWSER_ROWS; r++) {
        int i = bs.top + r;
        if (i >= (int)bs.entries.size()) break;
        int y = listY + r * rowH;
        bool sel = (i == bs.sel);
        if (sel) s_gfx->fillRoundRect(2, y - 1, 236, rowH, 3, TH().sel);
        s_gfx->setTextColor(sel ? TH().text
                                  : (bs.entries[i].dir ? TH().dir : TH().dim));
        String label = (bs.entries[i].dir ? String("> ") : String("  ")) + bs.entries[i].name;
        String vis = truncRight(label, 230);
        s_gfx->drawString(vis.c_str(), 6, y + 1);
    }

    if ((int)bs.entries.size() > cfg::BROWSER_ROWS) {
        int trackY = listY;
        int trackH = cfg::BROWSER_ROWS * rowH;
        s_gfx->fillRect(239, trackY, 1, trackH, TH().track);
        int n = (int)bs.entries.size();
        int thumbH = trackH * cfg::BROWSER_ROWS / n;
        if (thumbH < 6) thumbH = 6;
        int denom = n - cfg::BROWSER_ROWS;
        int thumbY = trackY + (denom > 0 ? (trackH - thumbH) * bs.top / denom : 0);
        s_gfx->fillRect(238, thumbY, 2, thumbH, TH().primary);
    }

    s_gfx->setTextColor(TH().dim);
    s_gfx->drawString("ENTER toca   DEL sobe   B fecha", 4, 124);

    if (millis() < s_volToastEnd) drawVolToast();
    else if (millis() < s_msgToastEnd) drawMsgToast();
}

}

namespace ui {

void init() {
#ifdef CM_APP_RADIO
    M5.Display.setTextWrap(false);
    s_gfx = &M5.Display;
    s_lowMem = true;  // CardRadio: no canvas at all, heap stays free for WiFi/decoders
#else
    s_canvas.setBuffer(s_canvasBuf, 240, 135, 16);  // static buffer: no heap, no fragmentation
    s_canvas.setTextWrap(false);
    s_gfx = &s_canvas;
    s_lowMem = false;
#endif
    for (int i = 0; i < SPEC_N; i++) s_spec[i] = 2 + (i % 8);
    s_invalid = true;
    s_lastDraw = 0;
}

void invalidate() { s_invalid = true; }

void setTheme(uint8_t idx) {
    if (idx > 2) idx = 0;
    s_themeIdx = idx;
    s_invalid = true;
#ifdef CM_APP_RADIO
    s_fullDirty = true;
#endif
}

uint8_t theme() { return s_themeIdx; }

const char* themeName(uint8_t idx) {
    if (idx > 2) idx = 0;
    return s_themes[idx].name;
}

void toastVolume(uint8_t vol) {
    s_volToastVal = vol;
    s_volToastEnd = millis() + 900;
    s_invalid = true;
}

void toastMsg(const char* msg) {
    strlcpy(s_msgToast, msg, sizeof(s_msgToast));
    s_msgToastEnd = millis() + 1200;
    s_invalid = true;
}

void setLowMem(bool low) {
    (void)low;  // static canvas in both apps: no heap involvement, keep sprite drawing
    s_invalid = true;
}

void staOpen(bool open) {
    s_staOpen = open;
    if (open) {
        int n = player::radioCount();
        s_staSel = player::radioIndex();
        if (s_staSel < 0) s_staSel = 0;
        if (n > 0 && s_staSel >= n) s_staSel = n - 1;
        if (s_staSel < s_staTop) s_staTop = s_staSel;
        if (s_staSel >= s_staTop + cfg::BROWSER_ROWS) s_staTop = s_staSel - cfg::BROWSER_ROWS + 1;
        if (s_staTop < 0) s_staTop = 0;
    }
    s_invalid = true;
}

bool staIsOpen() { return s_staOpen; }

int staSel() { return s_staSel; }

void staMove(int d) {
    int n = player::radioCount();
    if (n == 0) return;
    s_staSel += d;
    if (s_staSel < 0) s_staSel = 0;
    if (s_staSel >= n) s_staSel = n - 1;
    if (s_staSel < s_staTop) s_staTop = s_staSel;
    if (s_staSel >= s_staTop + cfg::BROWSER_ROWS) s_staTop = s_staSel - cfg::BROWSER_ROWS + 1;
    s_invalid = true;
}

void staPage(int d) {
    int n = player::radioCount();
    if (n == 0) return;
    s_staSel += d * cfg::BROWSER_ROWS;
    if (s_staSel < 0) s_staSel = 0;
    if (s_staSel >= n) s_staSel = n - 1;
    if (s_staSel < s_staTop) s_staTop = s_staSel;
    if (s_staSel >= s_staTop + cfg::BROWSER_ROWS) s_staTop = s_staSel - cfg::BROWSER_ROWS + 1;
    s_invalid = true;
}

void tick() {
    uint32_t now = millis();
    const PlayerState& st = player::state();
    bool playing = st.active && st.playing && !st.paused;
    bool brows = browser::state().open || s_staOpen;
#ifdef CM_APP_RADIO
    uint32_t interval = 250;  // radio: steady cadence, single SPI transaction per frame
#else
    uint32_t interval = brows ? 150 : (playing ? 100 : 400);
#endif
    if (!s_invalid && now - s_lastDraw < interval) return;
    s_lastDraw = now;
    s_invalid = false;

    if (now >= s_batNext) {
        s_bat = M5.Power.getBatteryLevel();
        s_batNext = now + 5000;
    }

    if (playing) {
        if (now - s_lastSpec >= 100) {
            s_lastSpec = now;
            for (int i = 0; i < SPEC_N; i++) {
                s_seed = s_seed * 1664525u + 1013904223u;
                int delta = (int)((s_seed >> 24) & 0xFF) - 128;
                delta /= 24;
                int v = (int)s_spec[i] + delta;
                if (v < 2) v = 2;
                if (v > 30) v = 30;
                s_spec[i] = (uint8_t)v;
            }
            s_reelStep = (s_reelStep + 1) % 12;
        }
    } else {
        for (int i = 0; i < SPEC_N; i++) {
            if (s_spec[i] > 2) s_spec[i] -= 2;
            else s_spec[i] = 2;
        }
    }

#ifdef CM_APP_RADIO
    (void)brows;
    radioTick(now);
#else
    if (player::radioMode()) {
        if (s_staOpen) drawStationList();
        else drawRadio();
    }
    else if (brows) drawBrowser();
    else if (s_themeIdx == 0) drawNowPlayingA();
    else if (s_themeIdx == 1) drawNowPlayingB();
    else drawNowPlayingC();
    s_canvas.pushSprite(0, 0);
#endif
}

void showError(const char* msg) {
    s_gfx->fillScreen(TH().bg);
    s_gfx->setFont(&fonts::efontJA_16);
    s_gfx->setTextColor(TH().primary);
    s_gfx->setTextDatum(textdatum_t::middle_center);
    s_gfx->drawString("CARDPLAYER", 120, 46);
    s_gfx->setFont(&fonts::efontJA_10);
    s_gfx->setTextColor(TH().text);
    s_gfx->drawString(msg, 120, 82);
    s_gfx->setTextColor(TH().dim);
    s_gfx->drawString("Insira o cartao SD e reinicie", 120, 98);
    s_gfx->setTextDatum(textdatum_t::top_left);
    if (!s_lowMem) s_canvas.pushSprite(0, 0);
}

}
