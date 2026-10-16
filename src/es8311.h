#pragma once
#include <stdint.h>

// ES8311 (Cardputer ADV): codec mono 24-bit, escravo, MCLK derivado do BCLK.
bool es8311_init();

// 0 = mute, 21 = 0 dB (unidade / bit-perfect). Passos de 2 dB abaixo do maximo.
void es8311_set_volume(uint8_t level);
