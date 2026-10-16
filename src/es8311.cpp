#include "es8311.h"
#include "config.h"
#include <M5Unified.h>
#include <Arduino.h>

namespace {

void wr(uint8_t reg, uint8_t val) {
    M5.In_I2C.writeRegister8(cfg::ES8311_ADDR, reg, val, 400000);
    delay(2);
}

}

bool es8311_init() {
    // Sequencia validada em hardware do Cardputer ADV.
    wr(0x00, 0x80);
    wr(0x01, 0xB5);
    wr(0x02, 0x18);
    wr(0x0D, 0x01);
    wr(0x12, 0x00);
    wr(0x13, 0x10);
    wr(0x32, 0xBF);
    wr(0x37, 0x08);
    return true;
}

void es8311_set_volume(uint8_t level) {
    if (level == 0) {
        wr(0x32, 0x00);         // mute
        return;
    }
    if (level > cfg::VOLUME_MAX) level = cfg::VOLUME_MAX;
    // 0xBF = 0 dB (maximo, sem perda). 2 dB de atenuacao por passo abaixo do max.
    wr(0x32, (uint8_t)(0xBF - (cfg::VOLUME_MAX - level) * 4));
}
