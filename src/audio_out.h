#pragma once
#include <stdint.h>
#include <stddef.h>

namespace audio_out {
bool begin();
void setSampleRate(uint32_t hz);
void write(const int16_t* buf, size_t bytes);
void silence(size_t bytes);
}
