#pragma once
#include <stdint.h>

namespace cfg {

// ---- Cardputer ADV hardware (pins validados em hardware) ----
static constexpr int PIN_I2S_BCLK = 41;
static constexpr int PIN_I2S_LRCK = 43;
static constexpr int PIN_I2S_DOUT = 42;
static constexpr uint8_t ES8311_ADDR = 0x18;

static constexpr int PIN_SD_CS   = 12;
static constexpr int PIN_SD_MOSI = 14;
static constexpr int PIN_SD_SCK  = 40;
static constexpr int PIN_SD_MISO = 39;
static constexpr uint32_t SD_SPI_HZ = 10000000;

// ---- Tasks / limites ----
static constexpr int AUDIO_TASK_STACK = 24576;
static constexpr int AUDIO_TASK_PRIO  = 5;
static constexpr int MAX_BROWSER_ENTRIES = 256;
static constexpr int BROWSER_ROWS = 8;

// ---- Paleta UI B (Now Playing) ----
static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static constexpr uint16_t COL_BG     = rgb565(0x0C, 0x0A, 0x14);
static constexpr uint16_t COL_PURPLE = rgb565(0xAA, 0x82, 0xFF);
static constexpr uint16_t COL_CYAN   = rgb565(0x5A, 0xDC, 0xE9);
static constexpr uint16_t COL_TEXT   = rgb565(0xFF, 0xFF, 0xFF);
static constexpr uint16_t COL_DIM    = rgb565(0x8A, 0x8A, 0x9A);
static constexpr uint16_t COL_TRACK  = rgb565(0x23, 0x20, 0x30);
static constexpr uint16_t COL_SEL    = rgb565(0x3A, 0x2E, 0x5A);
static constexpr uint16_t COL_DIR    = rgb565(0xC9, 0xB4, 0xFF);

static constexpr uint8_t VOLUME_MAX = 21;

// ---- Log serial (guardado: nunca bloqueia o loop se ninguem estiver lendo) ----
#define CM_LOG_ENABLED 1
#if CM_LOG_ENABLED
#define CM_LOG(...) do { if (Serial.availableForWrite() > 160) Serial.printf(__VA_ARGS__); } while (0)
#else
#define CM_LOG(...) do { } while (0)
#endif

}
