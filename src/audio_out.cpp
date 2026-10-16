#include "audio_out.h"
#include "config.h"
#include <Arduino.h>
#include <driver/i2s_std.h>

namespace {
i2s_chan_handle_t s_tx = nullptr;
constexpr uint32_t kRingBytes = 8 * 512 * 2 * 2;  // dma_desc_num * dma_frame_num * slots * 2B
}

namespace audio_out {

bool begin() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 512;
    chan_cfg.auto_clear_after_cb = false;
    if (i2s_new_channel(&chan_cfg, &s_tx, nullptr) != ESP_OK) return false;

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100);
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = (gpio_num_t)cfg::PIN_I2S_BCLK;
    std_cfg.gpio_cfg.ws   = (gpio_num_t)cfg::PIN_I2S_LRCK;
    std_cfg.gpio_cfg.dout = (gpio_num_t)cfg::PIN_I2S_DOUT;
    std_cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv   = false;
    if (i2s_channel_init_std_mode(s_tx, &std_cfg) != ESP_OK) return false;
    if (i2s_channel_enable(s_tx) != ESP_OK) return false;
    return true;
}

void setSampleRate(uint32_t hz) {
    if (!s_tx || hz == 0) return;
    i2s_channel_disable(s_tx);
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(hz);
    i2s_channel_reconfig_std_clock(s_tx, &clk);
    i2s_channel_enable(s_tx);
}

void write(const int16_t* buf, size_t bytes) {
    if (!s_tx || !buf || bytes == 0) return;
    const uint8_t* p = (const uint8_t*)buf;
    size_t left = bytes;
    while (left > 0) {
        size_t w = 0;
        if (i2s_channel_write(s_tx, p, left, &w, pdMS_TO_TICKS(100)) != ESP_OK) return;
        if (w == 0) return;
        p += w;
        left -= w;
    }
}

void silence(size_t bytes) {
    if (!s_tx) return;
    static int16_t zeros[512] = {0};
    size_t left = bytes ? bytes : kRingBytes;
    while (left > 0) {
        size_t chunk = left > sizeof(zeros) ? sizeof(zeros) : left;
        const uint8_t* p = (const uint8_t*)zeros;
        size_t l = chunk;
        while (l > 0) {
            size_t w = 0;
            if (i2s_channel_write(s_tx, p, l, &w, pdMS_TO_TICKS(50)) != ESP_OK) return;
            if (w == 0) return;
            p += w;
            l -= w;
        }
        left -= chunk;
    }
}

}
