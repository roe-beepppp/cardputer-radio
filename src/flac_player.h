#pragma once
#include <stdint.h>

// Dedicated FLAC playback path (dr_flac), independent from the audioI2S
// library which cannot decode FLAC without PSRAM.
namespace flac_player {

bool start(const char* relPath);  // path relative to SD mount (no /sd prefix)
void stop();
bool active();

bool decodeChunk();               // decode+write one PCM chunk; false on EOF/error
void setPaused(bool p);
void seekSec(uint32_t sec);

uint32_t currentSec();
uint32_t durationSec();
uint32_t sampleRate();
uint32_t bitrate();

}
