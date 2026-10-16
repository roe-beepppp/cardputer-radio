#pragma once
#include <Arduino.h>
#include <vector>

struct PlayerState {
    bool active   = false;
    bool playing  = false;
    bool paused   = false;
    String path;
    String title;
    String artist;
    String album;
    int trackNo   = 0;
    String codec;
    uint32_t sampleRate = 0;
    uint32_t bitrate    = 0;
    uint32_t duration   = 0;
    uint32_t position   = 0;
    int index  = -1;
    int count  = 0;
    uint8_t volume = 15;
};

namespace player {

struct RadioStation {
    String name;
    String url;
};

void begin();
void task(void* arg);
const PlayerState& state();

bool isPlayable(const String& name);
void playList(const std::vector<String>& items, int index);
void next();
void prev();
void togglePause();
void seekDelta(int32_t sec);
void volumeDelta(int8_t d);

void onEof();
bool consumeEof();
void onId3(const char* info);
void pollMeta();

String savedDir();
void saveDir(const String& dir);

uint8_t uiTheme();
void saveUiTheme(uint8_t idx);

void setRadioStations(const std::vector<RadioStation>& v);
int radioCount();
const RadioStation* radioStation(int i);
int radioIndex();
void radioPlay(int idx);
void radioNext();
void radioPrev();
bool radioMode();
bool radioFreed();
void enterRadio();
void exitRadio();
int radioLastIndex();
void onStreamTitle(const char* info);
void onStation(const char* info);
void onStreamEof();

}
