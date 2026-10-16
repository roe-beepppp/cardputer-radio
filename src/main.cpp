#include <Arduino.h>
#include <M5Unified.h>
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include "config.h"
#include "es8311.h"
#include "player.h"
#include "browser.h"
#include "radio.h"
#include "ui.h"
#include <vector>

namespace {

bool mountSD() {
    pinMode(cfg::PIN_SD_CS, OUTPUT);
    digitalWrite(cfg::PIN_SD_CS, HIGH);
    SPI.begin(cfg::PIN_SD_SCK, cfg::PIN_SD_MISO, cfg::PIN_SD_MOSI, cfg::PIN_SD_CS);
    uint32_t hz = cfg::SD_SPI_HZ;
    for (int i = 0; i < 4; i++) {
        if (SD.begin(cfg::PIN_SD_CS, SPI, hz)) {
            CM_LOG("[SD] mount ok @ %u Hz\n", (unsigned)hz);
            return true;
        }
        SD.end();
        CM_LOG("[SD] mount failed @ %u Hz\n", (unsigned)hz);
        hz /= 2;
    }
    return false;
}

#ifdef CM_APP_PLAYER
void toggleRadio() {
    ui::toastMsg("Radio: use CardRadio.bin");
    ui::invalidate();
}
#else
void toggleRadio() {
    CM_LOG("[RADIO] toggle %s\n", player::radioMode() ? "exit" : "enter");
    if (!player::radioMode()) {
        if (browser::state().open) browser::open(false);
        browser::freeEntries();  // radio doesn't need the file list; free ~KB for WiFi/decoder
        ui::setLowMem(true);  // free canvas RAM BEFORE WiFi driver init (esp_wifi_init needs heap)
        if (!radio::enter()) {
            ui::setLowMem(false);
            ui::toastMsg("wifi.txt?");
            radio::leave();
            ui::invalidate();
            return;
        }
        std::vector<player::RadioStation> v;
        if (!radio::loadStations(v) || v.empty()) {
            ui::setLowMem(false);
            ui::toastMsg("sem estacoes (radio.txt)");
            radio::leave();
            ui::invalidate();
            return;
        }
        player::setRadioStations(v);
        player::enterRadio();
        int idx = player::radioLastIndex();
        if (idx < 0 || idx >= (int)v.size()) idx = 0;
        player::radioPlay(idx);
        char tb[32];
        int sk = radio::httpsSkipped();
        if (sk > 0) snprintf(tb, sizeof(tb), "RADIO ON - %d https off", sk);
        else snprintf(tb, sizeof(tb), "RADIO ON");
        ui::toastMsg(tb);
    } else {
        ui::staOpen(false);
        player::exitRadio();  // task stops the stream, then turns WiFi off (safe order)
        for (int i = 0; i < 100 && !player::radioFreed(); i++) delay(10);
        radio::leave();  // safety net if the task didn't reach it
        ui::setLowMem(false);
        CM_LOG("[RADIO] exit freed=%d heap=%u\n",
                      (int)player::radioFreed(), (unsigned)ESP.getFreeHeap());
        ui::toastMsg("RADIO OFF");
    }
    ui::invalidate();
}
#endif

void keyChar(char c) {
    CM_LOG("[KEY] '%c'\n", c);
    if (player::radioMode()) {
        bool so = ui::staIsOpen();
        switch (c) {
            case ';':
                player::volumeDelta(+1);
                ui::toastVolume(player::state().volume);
                break;
            case '.':
                player::volumeDelta(-1);
                ui::toastVolume(player::state().volume);
                break;
            case ',':
                if (so) ui::staMove(-1);
                else player::radioPrev();
                break;
            case '/':
                if (so) ui::staMove(+1);
                else player::radioNext();
                break;
            case 'b':
            case 'B':
                ui::staOpen(!so);
                break;
            case 'r':
            case 'R':
#ifdef CM_APP_RADIO
                ui::toastMsg("CardRadio: so radio");
                break;
#else
                toggleRadio();
                return;
#endif
            case 'v':
            case 'V': {
                uint8_t t = (uint8_t)((ui::theme() + 1) % 3);
                ui::setTheme(t);
                player::saveUiTheme(t);
                char buf[32];
                snprintf(buf, sizeof(buf), "UI: %s", ui::themeName(t));
                ui::toastMsg(buf);
                break;
            }
            case ' ':
                player::togglePause();
                break;
            default:
                break;
        }
        ui::invalidate();
        return;
    }
#ifndef CM_APP_RADIO
    bool b = browser::state().open;
    switch (c) {
        case ';':
            if (b) browser::move(-1);
            else { player::volumeDelta(+1); ui::toastVolume(player::state().volume); }
            break;
        case '.':
            if (b) browser::move(+1);
            else { player::volumeDelta(-1); ui::toastVolume(player::state().volume); }
            break;
        case ',':
            if (b) browser::page(-1);
            else player::prev();
            break;
        case '/':
            if (b) browser::page(+1);
            else player::next();
            break;
        case '[':
            if (player::state().codec == "M4A" || player::state().codec == "AAC") { ui::toastMsg("sem seek nesse formato"); break; }
            player::seekDelta(-5);
            break;
        case ']':
            if (player::state().codec == "M4A" || player::state().codec == "AAC") { ui::toastMsg("sem seek nesse formato"); break; }
            player::seekDelta(+5);
            break;
        case 'b':
        case 'B':
            browser::open(!b);
            break;
        case 'v':
        case 'V': {
            uint8_t t = (uint8_t)((ui::theme() + 1) % 3);
            ui::setTheme(t);
            player::saveUiTheme(t);
            char buf[32];
            snprintf(buf, sizeof(buf), "UI: %s", ui::themeName(t));
            ui::toastMsg(buf);
            break;
        }
        case ' ':
            if (b) browser::activate();
            else player::togglePause();
            break;
        case 'r':
        case 'R':
            toggleRadio();
            break;
        default:
            break;
    }
#else
    // radio app: 'r' retries enter (e.g. after fixing wifi.txt); 'v' cycles theme
    switch (c) {
        case 'r':
        case 'R':
            toggleRadio();
            break;
        case 'v':
        case 'V': {
            uint8_t t = (uint8_t)((ui::theme() + 1) % 3);
            ui::setTheme(t);
            player::saveUiTheme(t);
            char buf[32];
            snprintf(buf, sizeof(buf), "UI: %s", ui::themeName(t));
            ui::toastMsg(buf);
            break;
        }
        default:
            break;
    }
#endif
    ui::invalidate();
}

constexpr uint32_t kRepeatDelayMs = 350;
constexpr uint32_t kRepeatMs      = 90;

struct HeldKey {
    char c;
    uint32_t downAt;
    uint32_t lastFire;
};
HeldKey s_held[8];
int s_heldCount = 0;

bool repeatable(char c) {
    return c == ';' || c == '.' || c == ',' || c == '/' || c == '[' || c == ']';
}

void processKeys(const char* cur, int n) {
    uint32_t now = millis();
    for (int i = 0; i < n; i++) {
        char c = cur[i];
        int idx = -1;
        for (int j = 0; j < s_heldCount; j++) {
            if (s_held[j].c == c) {
                idx = j;
                break;
            }
        }
        if (idx < 0) {
            keyChar(c);
            if (s_heldCount < 8) s_held[s_heldCount++] = {c, now, now};
        } else if (repeatable(c) && now - s_held[idx].downAt >= kRepeatDelayMs &&
                   now - s_held[idx].lastFire >= kRepeatMs) {
            keyChar(c);
            s_held[idx].lastFire = now;
        }
    }
    int w = 0;
    for (int j = 0; j < s_heldCount; j++) {
        bool still = false;
        for (int i = 0; i < n; i++) {
            if (cur[i] == s_held[j].c) {
                still = true;
                break;
            }
        }
        if (still) s_held[w++] = s_held[j];
    }
    s_heldCount = w;
}

}

void setup() {
    Serial.begin(115200);
    delay(400);
#ifdef CM_APP_RADIO
    CM_LOG("[BOOT] CardRadio\n");
#else
    CM_LOG("[BOOT] CardPlayer\n");
#endif
    auto m5cfg = M5.config();
    m5cfg.internal_spk = false;
    m5cfg.internal_mic = false;
    M5Cardputer.begin(m5cfg, true);
    M5.Speaker.end();
    M5.Mic.end();
    M5.Display.setBrightness(150);

    if (!mountSD()) {
        CM_LOG("[SD] no card\n");
        ui::init();
        ui::showError("Cartao SD nao encontrado");
        for (;;) delay(1000);
    }

    es8311_init();
    CM_LOG("[ES8311] init done\n");
    player::begin();
    browser::begin();
    ui::init();
    ui::setTheme(player::uiTheme());
#ifdef CM_APP_RADIO
    toggleRadio();  // radio app: boot straight into radio mode
#endif
    CM_LOG("[BOOT] ready\n");

    xTaskCreatePinnedToCore(player::task, "audio_pump", cfg::AUDIO_TASK_STACK,
                            nullptr, cfg::AUDIO_TASK_PRIO, nullptr, 1);
}

void loop() {
    M5Cardputer.update();
    auto ks = M5Cardputer.Keyboard.keysState();

    char cur[8];
    int ncur = 0;
    for (char c : ks.word) {
        if (ncur >= 8) break;
        bool dup = false;
        for (int i = 0; i < ncur; i++) {
            if (cur[i] == c) {
                dup = true;
                break;
            }
        }
        if (!dup) cur[ncur++] = c;
    }
    processKeys(cur, ncur);

    static bool prevEnter = false;
    static bool prevBack = false;
    static bool prevTab = false;
    bool back = ks.backspace || ks.del;

    if (ks.enter && !prevEnter) {
        CM_LOG("[KEY] ENTER\n");
        if (player::radioMode()) {
            if (ui::staIsOpen()) {
                player::radioPlay(ui::staSel());
                ui::staOpen(false);
            } else {
                ui::staOpen(true);
            }
        }
#ifndef CM_APP_RADIO
        else if (browser::state().open) {
            browser::activate();
        } else {
            browser::open(true);
        }
#endif
        ui::invalidate();
    }
    if (back && !prevBack) {
        CM_LOG("[KEY] BACKSPACE\n");
        if (player::radioMode()) {
            if (ui::staIsOpen()) ui::staOpen(false);
#ifdef CM_APP_PLAYER
            else toggleRadio();
#endif
        }
#ifndef CM_APP_RADIO
        else if (browser::state().open) {
            browser::upDir();
        } else {
            browser::open(true);
        }
#endif
        ui::invalidate();
    }
    if (ks.tab && !prevTab) {
        CM_LOG("[KEY] TAB\n");
        if (player::radioMode()) ui::staOpen(!ui::staIsOpen());
#ifndef CM_APP_RADIO
        else browser::open(!browser::state().open);
#endif
        ui::invalidate();
    }
    prevEnter = ks.enter;
    prevBack = back;
    prevTab = ks.tab;

    radio::tick();
    player::pollMeta();
    if (player::consumeEof()) {
        if (!player::radioMode()) player::next();
        ui::invalidate();
    }

    static uint32_t lastHb = 0;
    if (millis() - lastHb >= 1000) {
        lastHb = millis();
        const auto& ps = player::state();
        CM_LOG("[HB] act=%d idx=%d/%d pos=%u/%u codec=%s rate=%u br=%u vol=%u pause=%d brw=%d rad=%d wifi=%d\n",
                      (int)ps.active, ps.index + 1, ps.count, (unsigned)ps.position,
                      (unsigned)ps.duration, ps.codec.c_str(), (unsigned)ps.sampleRate,
                      (unsigned)ps.bitrate, (unsigned)ps.volume, (int)ps.paused,
                      (int)browser::state().open, (int)player::radioMode(), (int)radio::status());
    }

    ui::tick();
    delay(4);
}
