#include "player.h"
#include "config.h"
#include "es8311.h"
#include "audio_out.h"
#include "flac_player.h"
#include "radio.h"
#include <Audio.h>
#include <SD.h>
#include <WiFi.h>
#include <Preferences.h>
#include <string.h>
#include <stdlib.h>

namespace {

Audio s_audio;
std::vector<String> s_list;
PlayerState s_st;
Preferences s_prefs;
volatile bool s_eofPending = false;
bool s_flacMode = false;  // true while playing via flac_player (audioI2S bypassed)
bool s_radioMode = false; // web radio (stream decoded by lib, output on our I2S)
std::vector<player::RadioStation> s_sta;
int s_staIdx = -1;
int s_radioRetry = 0;
char s_radioUrl[224] = {0};
volatile bool s_streamEof = false;
volatile bool s_radioFreed = true;  // true when the task stopped the stream and turned WiFi off

portMUX_TYPE s_metaMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_id3Pending = false;
char s_id3Title[96]  = {0};
char s_id3Artist[96] = {0};
char s_id3Album[64]  = {0};
int  s_id3Track = 0;
volatile bool s_radioMetaPending = false;
char s_radioTitle[96]   = {0};
char s_radioStation[64] = {0};

enum : uint8_t { CMD_SEEK, CMD_PAUSE };
struct Cmd {
    uint8_t type;
    int32_t arg;
};
struct StartCmd {
    char path[224];
};
QueueHandle_t s_qStart = nullptr;  // length 1, overwrite (always latest)
QueueHandle_t s_qEv    = nullptr;  // seek/pause events
QueueHandle_t s_qRadio = nullptr;  // length 1, overwrite (radio url; empty = stop)

String baseName(const String& path) {
    int i = path.lastIndexOf('/');
    return i >= 0 ? path.substring(i + 1) : path;
}

String dirName(const String& path) {
    int i = path.lastIndexOf('/');
    return i > 0 ? path.substring(0, i) : String("/");
}

String stem(const String& name) {
    int i = name.lastIndexOf('.');
    return i > 0 ? name.substring(0, i) : name;
}

void applyNameMeta() {
    String n = stem(baseName(s_st.path));
    s_st.title  = n;
    s_st.artist = "";
    int d = n.indexOf(" - ");
    if (d > 0) {
        s_st.artist = n.substring(0, d);
        s_st.title  = n.substring(d + 3);
    }
    if (s_st.artist.isEmpty()) {
        String folder = baseName(dirName(s_st.path));
        if (folder.length() && folder != "sd") s_st.artist = folder;
    }
}

void startTrack(int i) {
    if (i < 0 || (size_t)i >= s_list.size()) return;
    s_st.index = i;
    s_st.path  = s_list[i];
    s_st.album = ""; s_st.trackNo = 0;
    s_st.codec = ""; s_st.sampleRate = 0; s_st.bitrate = 0;
    s_st.duration = 0; s_st.position = 0;
    applyNameMeta();
    s_st.active = true; s_st.playing = true; s_st.paused = false;
    portENTER_CRITICAL(&s_metaMux);
    s_id3Pending = false;
    s_id3Title[0] = s_id3Artist[0] = s_id3Album[0] = 0;
    s_id3Track = 0;
    portEXIT_CRITICAL(&s_metaMux);
    StartCmd c;
    strlcpy(c.path, s_st.path.c_str(), sizeof(c.path));
    if (s_qStart) xQueueOverwrite(s_qStart, &c);
    s_prefs.putString("track", s_st.path);
}

}

void audio_eof_mp3(const char*) {
    player::onEof();
}

void audio_id3data(const char* info) { player::onId3(info); }

void audio_process_i2s(int16_t* outBuff, uint16_t validSamples, uint8_t bitsPerSample,
                       uint8_t channels, bool* continueI2S) {
    *continueI2S = false;  // lib never writes to its own I2S channel; we output on ours
    if (!outBuff || validSamples == 0) return;
    audio_out::write(outBuff, (size_t)validSamples * (bitsPerSample / 8) * channels);
}

void audio_info(const char* info) {
    if (info && info[0]) CM_LOG("[AUDIO] %s\n", info);
}

void audio_showstation(const char* info) {
    if (info && info[0]) player::onStation(info);
}

void audio_showstreamtitle(const char* info) {
    if (info && info[0]) player::onStreamTitle(info);
}

void audio_eof_stream(const char*) { player::onStreamEof(); }

namespace player {

void begin() {
    s_prefs.begin("cardmusic", false);
    s_qStart = xQueueCreate(1, sizeof(StartCmd));
    s_qEv    = xQueueCreate(4, sizeof(Cmd));
    s_qRadio = xQueueCreate(1, sizeof(StartCmd));
    s_st.volume = s_prefs.getUChar("vol", 15);
    if (s_st.volume > cfg::VOLUME_MAX) s_st.volume = cfg::VOLUME_MAX;
    audio_out::begin();  // own I2S channel; lib's channel never receives data
    s_audio.setVolume(s_audio.maxVolume());
    s_audio.setConnectionTimeout(4000, 4000);
    es8311_set_volume(s_st.volume);
}

void task(void*) {
    for (;;) {
        StartCmd rc;
        if (xQueueReceive(s_qRadio, &rc, 0) == pdPASS) {
            CM_LOG("[RADIO] start: %s\n", rc.path);
            s_audio.stopSong();
            flac_player::stop();
            s_flacMode = false;
            audio_out::silence(0);
            if (rc.path[0] == 0) {
                s_radioMode = false;
                s_st.active = false; s_st.playing = false; s_st.paused = false;
                radio::leave();  // WiFi OFF from the task, after stopSong() closed the socket
                s_radioFreed = true;
                CM_LOG("[RADIO] stopped + wifi off\n");
            } else {
                s_radioMode = true;
                s_radioFreed = false;
                strlcpy(s_radioUrl, rc.path, sizeof(s_radioUrl));
                for (int i = 0; i < 100 && WiFi.status() != WL_CONNECTED && s_radioMode; i++) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                if (!s_radioMode) {
                    CM_LOG("[RADIO] exit during wifi wait\n");
                } else {
                    bool ok = s_audio.connecttohost(rc.path);
                    audio_out::silence(0);
                    if (ok) {
                        s_radioRetry = 0;
                        uint32_t sr = s_audio.getSampleRate();
                        if (sr > 0) audio_out::setSampleRate(sr);
                        if (s_st.title == "Conectando...") s_st.title = "Ao vivo";
                        CM_LOG("[RADIO] ok sr=%u heap=%u max=%u\n",
                                      (unsigned)sr, (unsigned)ESP.getFreeHeap(),
                                      (unsigned)ESP.getMaxAllocHeap());
                    } else if (!s_radioMode) {
                        // user exited radio while connecting: don't retry/re-enqueue
                        CM_LOG("[RADIO] connect failed; radio mode exited\n");
                    } else {
                        s_radioRetry++;
                        if (s_radioRetry <= 1) {  // one retry max: bad station must not hang the UI
                            StartCmd again;
                            strlcpy(again.path, rc.path, sizeof(again.path));
                            xQueueOverwrite(s_qRadio, &again);
                        } else {
                            s_radioRetry = 0;
                            radioNext();
                        }
                    }
                    CM_LOG("[RADIO] connect=%d retry=%d url=%s\n",
                                  (int)ok, s_radioRetry, rc.path);
                }
            }
        }
        StartCmd sc;
        if (xQueueReceive(s_qStart, &sc, 0) == pdPASS) {
            CM_LOG("[PLAYER] start: %s\n", sc.path);
            const char* ap = (strncmp(sc.path, "/sd", 3) == 0) ? sc.path + 3 : sc.path;
            bool ex = SD.exists(ap);
            File df = SD.open(ap);
            CM_LOG("[DBG] exists=%d open=%d size=%u heap=%u\n",
                          (int)ex, (int)(bool)df, df ? (unsigned)df.size() : 0, (unsigned)ESP.getFreeHeap());
            if (df) df.close();
            if (!ex) {
                CM_LOG("[DBG] hex:");
                for (const char* p = sc.path; *p; p++) CM_LOG(" %02X", (uint8_t)*p);
                CM_LOG("\n");
            }
            s_audio.stopSong();
            flac_player::stop();
            s_flacMode = false;
            s_radioMode = false;  // music start leaves radio mode
            audio_out::silence(0);  // replace stale ring data before switching
            String low = sc.path;
            low.toLowerCase();
            if (low.endsWith(".flac")) {
                bool ok = flac_player::start(ap);
                s_flacMode = ok;
                if (ok) {
                    s_st.codec      = "FLAC";
                    s_st.sampleRate = flac_player::sampleRate();
                    s_st.duration   = flac_player::durationSec();
                    s_st.bitrate    = flac_player::bitrate();
                    s_st.position   = 0;
                }
                audio_out::silence(0);
                CM_LOG("[PLAYER] flac start=%d rate=%u dur=%u br=%u\n",
                              (int)ok, (unsigned)flac_player::sampleRate(),
                              (unsigned)flac_player::durationSec(),
                              (unsigned)flac_player::bitrate());
            } else {
                bool ok = s_audio.connecttoFS(SD, ap);
                audio_out::silence(0);  // flush DMA ring with silence between tracks
                if (ok) audio_out::setSampleRate(s_audio.getSampleRate());
                const char* cn = s_audio.getCodecname();
                CM_LOG("[PLAYER] connect=%d codec=%s rate=%u bit=%u dur=%u\n",
                              (int)ok, cn ? cn : "?",
                              (unsigned)s_audio.getSampleRate(),
                              (unsigned)s_audio.getBitRate(),
                              (unsigned)s_audio.getAudioFileDuration());
            }
        }
        Cmd c;
        while (xQueueReceive(s_qEv, &c, 0) == pdPASS) {
            if (c.type == CMD_SEEK) {
                if (s_radioMode) {
                    // live stream: no seek
                } else if (s_flacMode) {
                    CM_LOG("[PLAYER] flac seek %d (from %u)\n", (int)c.arg,
                                  (unsigned)flac_player::currentSec());
                    flac_player::seekSec((uint32_t)c.arg);
                    s_st.position = (uint32_t)c.arg;
                } else {
                    CM_LOG("[PLAYER] seek %d (from %u)\n", (int)c.arg,
                                  (unsigned)s_audio.getAudioCurrentTime());
                    s_audio.setAudioPlayPosition((uint16_t)c.arg);
                    CM_LOG("[PLAYER] seek done, now=%u\n",
                                  (unsigned)s_audio.getAudioCurrentTime());
                }
            } else if (c.type == CMD_PAUSE) {
                if (s_radioMode) {
                    if (s_st.paused) {
                        s_audio.stopSong();
                        audio_out::silence(0);
                    } else {
                        StartCmd again;
                        strlcpy(again.path, s_radioUrl, sizeof(again.path));
                        xQueueOverwrite(s_qRadio, &again);
                    }
                    CM_LOG("[PLAYER] radio pause -> %d\n", (int)s_st.paused);
                } else if (s_flacMode) {
                    flac_player::setPaused(s_st.paused);
                    if (s_st.paused) audio_out::silence(0);
                    CM_LOG("[PLAYER] flac pause -> %d\n", (int)s_st.paused);
                } else {
                    s_audio.pauseResume();
                    if (s_st.paused) audio_out::silence(0);  // fill ring so pause loops silence
                    CM_LOG("[PLAYER] pause -> %d\n", (int)s_st.paused);
                }
            }
        }
        if (s_radioMode && s_streamEof) {
            s_streamEof = false;
            s_radioRetry++;
            if (s_radioRetry <= 1) {
                StartCmd again;
                strlcpy(again.path, s_radioUrl, sizeof(again.path));
                xQueueOverwrite(s_qRadio, &again);
            } else {
                s_radioRetry = 0;
                radioNext();
            }
            CM_LOG("[RADIO] eof retry=%d\n", s_radioRetry);
        }
        if (s_flacMode) {
            if (!s_st.paused && !flac_player::decodeChunk()) {
                flac_player::stop();
                s_flacMode = false;
                onEof();
            }
            if (s_flacMode) s_st.position = flac_player::currentSec();
        } else {
            s_audio.loop();
        }
        vTaskDelay(1);
    }
}

const PlayerState& state() { return s_st; }

bool isPlayable(const String& name) {
    String n = name;
    n.toLowerCase();
    return n.endsWith(".mp3") || n.endsWith(".m4a") || n.endsWith(".aac") ||
           n.endsWith(".flac") || n.endsWith(".wav") || n.endsWith(".ogg") ||
           n.endsWith(".opus");
}

void playList(const std::vector<String>& items, int index) {
    s_list = items;
    s_st.count = (int)s_list.size();
    startTrack(index);
}

void next() {
    if (s_list.empty()) return;
    int i = s_st.index + 1;
    if (i >= (int)s_list.size()) i = 0;
    startTrack(i);
}

void prev() {
    if (s_list.empty()) return;
    if (s_st.position > 3) {
        // Full restart instead of setAudioPlayPosition(0): the lib's M4A
        // resume path re-syncs the AAC decoder dirty (no decoder clear).
        startTrack(s_st.index);
        s_st.position = 0;
        return;
    }
    int i = s_st.index - 1;
    if (i < 0) i = (int)s_list.size() - 1;
    startTrack(i);
}

void togglePause() {
    if (!s_st.active) return;
    Cmd c{CMD_PAUSE, 0};
    if (s_qEv) xQueueSend(s_qEv, &c, 0);
    s_st.paused = !s_st.paused;
}

void seekDelta(int32_t sec) {
    if (s_radioMode) return;  // live stream
    if (s_st.codec == "M4A" || s_st.codec == "AAC") return;  // lib 3.0.12 doesn't reset AAC decoder on seek
    if (!s_st.active || s_st.duration == 0) return;
    int32_t p = (int32_t)s_st.position + sec;
    if (p < 0) p = 0;
    if (p > (int32_t)s_st.duration - 2) p = (int32_t)s_st.duration - 2;
    Cmd c{CMD_SEEK, p};
    if (s_qEv) xQueueSend(s_qEv, &c, 0);
    s_st.position = (uint32_t)p;
}

void volumeDelta(int8_t d) {
    int v = (int)s_st.volume + d;
    if (v < 0) v = 0;
    if (v > cfg::VOLUME_MAX) v = cfg::VOLUME_MAX;
    s_st.volume = (uint8_t)v;
    es8311_set_volume(s_st.volume);
    s_prefs.putUChar("vol", s_st.volume);
}

void onEof() { s_eofPending = true; }

bool consumeEof() {
    if (!s_eofPending) return false;
    s_eofPending = false;
    return true;
}

void onId3(const char* info) {
    if (!info) return;
    if (!strncmp(info, "Title: ", 7)) {
        portENTER_CRITICAL(&s_metaMux);
        strlcpy(s_id3Title, info + 7, sizeof(s_id3Title));
        s_id3Pending = true;
        portEXIT_CRITICAL(&s_metaMux);
    } else if (!strncmp(info, "Artist: ", 8)) {
        portENTER_CRITICAL(&s_metaMux);
        strlcpy(s_id3Artist, info + 8, sizeof(s_id3Artist));
        s_id3Pending = true;
        portEXIT_CRITICAL(&s_metaMux);
    } else if (!strncmp(info, "Album: ", 7)) {
        portENTER_CRITICAL(&s_metaMux);
        strlcpy(s_id3Album, info + 7, sizeof(s_id3Album));
        s_id3Pending = true;
        portEXIT_CRITICAL(&s_metaMux);
    } else if (!strncmp(info, "Track: ", 7)) {
        int t = atoi(info + 7);
        if (t > 0) {
            portENTER_CRITICAL(&s_metaMux);
            s_id3Track = t;
            s_id3Pending = true;
            portEXIT_CRITICAL(&s_metaMux);
        }
    }
}

void pollMeta() {
    if (s_id3Pending) {
        char t[96], a[96], al[64];
        int tr;
        portENTER_CRITICAL(&s_metaMux);
        strlcpy(t, s_id3Title, sizeof(t));
        strlcpy(a, s_id3Artist, sizeof(a));
        strlcpy(al, s_id3Album, sizeof(al));
        tr = s_id3Track;
        s_id3Pending = false;
        portEXIT_CRITICAL(&s_metaMux);
        if (t[0])   s_st.title  = t;
        if (a[0])   s_st.artist = a;
        if (al[0])  s_st.album  = al;
        if (tr > 0) s_st.trackNo = tr;
    }
    if (!s_st.active) return;
    if (s_radioMode) {
        if (s_radioMetaPending) {
            char t[96], n[64];
            portENTER_CRITICAL(&s_metaMux);
            strlcpy(t, s_radioTitle, sizeof(t));
            strlcpy(n, s_radioStation, sizeof(n));
            s_radioMetaPending = false;
            portEXIT_CRITICAL(&s_metaMux);
            if (t[0]) s_st.title = t;
            if (n[0]) s_st.artist = n;
        }
        s_st.position = s_audio.getAudioCurrentTime();
        uint32_t sr = s_audio.getSampleRate();
        if (sr > 0) {
            if (s_st.sampleRate != sr) {
                audio_out::setSampleRate(sr);  // stream rate known after first frames
                CM_LOG("[RADIO] rate->%u\n", (unsigned)sr);
            }
            s_st.sampleRate = sr;
        }
        if (s_st.bitrate == 0)    s_st.bitrate    = s_audio.getBitRate();
        if (s_st.codec.isEmpty()) {
            const char* c = s_audio.getCodecname();
            if (c && c[0]) s_st.codec = c;
        }
        return;
    }
    if (s_flacMode) return;  // flac path updates position/duration itself
    s_st.position = s_audio.getAudioCurrentTime();
    if (s_st.duration == 0)   s_st.duration   = s_audio.getAudioFileDuration();
    if (s_st.codec.isEmpty()) {
        const char* c = s_audio.getCodecname();
        if (c && c[0]) s_st.codec = c;
    }
    if (s_st.sampleRate == 0) s_st.sampleRate = s_audio.getSampleRate();
    if (s_st.bitrate == 0)    s_st.bitrate    = s_audio.getBitRate();
}

String savedDir() { return s_prefs.getString("dir", "/sd"); }

void saveDir(const String& dir) { s_prefs.putString("dir", dir); }

uint8_t uiTheme() { return s_prefs.getUChar("ui", 1); }

void saveUiTheme(uint8_t idx) { s_prefs.putUChar("ui", idx); }

void setRadioStations(const std::vector<RadioStation>& v) {
    s_sta = v;
    s_staIdx = -1;
}

int radioCount() { return (int)s_sta.size(); }

const RadioStation* radioStation(int i) {
    if (i < 0 || i >= (int)s_sta.size()) return nullptr;
    return &s_sta[i];
}

int radioIndex() { return s_staIdx; }

void radioPlay(int idx) {
    if (idx < 0 || idx >= (int)s_sta.size()) return;
    s_staIdx = idx;
    s_radioRetry = 0;
    s_st.active = true; s_st.playing = true; s_st.paused = false;
    s_st.path   = s_sta[idx].url;
    s_st.title  = "Conectando...";
    s_st.artist = s_sta[idx].name;
    s_st.album  = ""; s_st.trackNo = 0;
    s_st.codec  = "RADIO"; s_st.sampleRate = 0; s_st.bitrate = 0;
    s_st.duration = 0; s_st.position = 0;
    s_st.index = idx; s_st.count = (int)s_sta.size();
    StartCmd c;
    strlcpy(c.path, s_sta[idx].url.c_str(), sizeof(c.path));
    if (s_qRadio) xQueueOverwrite(s_qRadio, &c);
    s_prefs.putInt("rsta", idx);
}

void radioNext() {
    if (s_sta.empty()) return;
    radioPlay((s_staIdx + 1) % (int)s_sta.size());
}

void radioPrev() {
    if (s_sta.empty()) return;
    radioPlay((s_staIdx - 1 + (int)s_sta.size()) % (int)s_sta.size());
}

bool radioMode() { return s_radioMode; }

bool radioFreed() { return s_radioFreed; }

void enterRadio() { s_radioMode = true; }

void exitRadio() {
    s_radioMode = false;
    s_st.active = false; s_st.playing = false; s_st.paused = false;
    StartCmd c;
    c.path[0] = 0;
    if (s_qRadio) xQueueOverwrite(s_qRadio, &c);
}

int radioLastIndex() { return s_prefs.getInt("rsta", 0); }

void onStreamTitle(const char* info) {
    portENTER_CRITICAL(&s_metaMux);
    strlcpy(s_radioTitle, info, sizeof(s_radioTitle));
    s_radioMetaPending = true;
    portEXIT_CRITICAL(&s_metaMux);
}

void onStation(const char* info) {
    portENTER_CRITICAL(&s_metaMux);
    strlcpy(s_radioStation, info, sizeof(s_radioStation));
    s_radioMetaPending = true;
    portEXIT_CRITICAL(&s_metaMux);
}

void onStreamEof() { s_streamEof = true; }

}
