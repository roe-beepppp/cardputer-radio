#include "flac_player.h"

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#include "dr_flac.h"

#include "audio_out.h"
#include "config.h"
#include <SD.h>

namespace {

File s_file;
drflac* s_dec = nullptr;
uint64_t s_framesRead  = 0;
uint64_t s_totalFrames = 0;
uint32_t s_sampleRate  = 44100;
uint8_t  s_channels    = 2;
uint32_t s_bitrate     = 0;
volatile bool s_paused = false;

constexpr size_t kChunkFrames = 2048;
int16_t s_pcm[kChunkFrames * 2];
int16_t s_out[kChunkFrames * 2];

/* Stack arena for dr_flac allocations: deterministic even with fragmented heap.
   drflac_open does one big contiguous alloc (~34KB) that fails after audioI2S
   cycles fragment the heap. All dr_flac allocations are LIFO, so a bump
   allocator with free-at-top semantics is safe. Reset in start()/stop(). */
constexpr size_t kArenaSize = 40 * 1024;
constexpr size_t kArenaHdr  = 16;
alignas(16) uint8_t s_arena[kArenaSize];
size_t s_arenaTop = 0;

void* arenaAlloc(size_t sz, void*) {
    size_t need = (sz + kArenaHdr + 15u) & ~static_cast<size_t>(15u);
    if (s_arenaTop + need > kArenaSize) return nullptr;
    uint8_t* base = s_arena + s_arenaTop;
    *reinterpret_cast<size_t*>(base) = need;
    s_arenaTop += need;
    return base + kArenaHdr;
}

void arenaFree(void* p, void*) {
    if (!p) return;
    uint8_t* base = static_cast<uint8_t*>(p) - kArenaHdr;
    size_t need = *reinterpret_cast<size_t*>(base);
    if (base + need == s_arena + s_arenaTop) s_arenaTop -= need;
}

void* arenaRealloc(void* p, size_t szNew, void* u) {
    if (!p) return arenaAlloc(szNew, u);
    uint8_t* base = static_cast<uint8_t*>(p) - kArenaHdr;
    size_t need = *reinterpret_cast<size_t*>(base);
    if (base + need == s_arena + s_arenaTop) {
        size_t needNew = (szNew + kArenaHdr + 15u) & ~static_cast<size_t>(15u);
        if (s_arenaTop - need + needNew <= kArenaSize) {
            s_arenaTop = s_arenaTop - need + needNew;
            *reinterpret_cast<size_t*>(base) = needNew;
            return p;
        }
    }
    void* np = arenaAlloc(szNew, u);
    if (np) memcpy(np, p, (need - kArenaHdr) < szNew ? (need - kArenaHdr) : szNew);
    arenaFree(p, u);
    return np;
}

drflac_allocation_callbacks s_alloc = { nullptr, arenaAlloc, arenaRealloc, arenaFree };

size_t readCb(void* pUser, void* pOut, size_t bytes) {
    File* f = static_cast<File*>(pUser);
    int n = f->read(static_cast<uint8_t*>(pOut), bytes);
    return n > 0 ? static_cast<size_t>(n) : 0;
}

drflac_bool32 seekCb(void* pUser, int offset, drflac_seek_origin origin) {
    File* f = static_cast<File*>(pUser);
    int64_t pos;
    if (origin == DRFLAC_SEEK_SET)      pos = offset;
    else if (origin == DRFLAC_SEEK_CUR) pos = static_cast<int64_t>(f->position()) + offset;
    else                                pos = static_cast<int64_t>(f->size()) + offset;
    if (pos < 0) pos = 0;
    return f->seek(static_cast<uint32_t>(pos)) ? DRFLAC_TRUE : DRFLAC_FALSE;
}

drflac_bool32 tellCb(void* pUser, drflac_int64* pCursor) {
    File* f = static_cast<File*>(pUser);
    *pCursor = static_cast<drflac_int64>(f->position());
    return DRFLAC_TRUE;
}

}

namespace flac_player {

bool start(const char* relPath) {
    stop();
    s_arenaTop = 0;
    s_file = SD.open(relPath);
    CM_LOG("[FLAC] open=%d heap=%u max=%u\n", (int)(bool)s_file, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    if (!s_file) return false;
    s_dec = drflac_open(readCb, seekCb, tellCb, &s_file, &s_alloc);
    CM_LOG("[FLAC] dec=%d arena=%u heap=%u max=%u\n", (int)(bool)s_dec, (unsigned)s_arenaTop, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    if (!s_dec) {
        s_file.close();
        return false;
    }
    s_sampleRate  = s_dec->sampleRate ? s_dec->sampleRate : 44100;
    s_channels    = s_dec->channels ? s_dec->channels : 2;
    s_totalFrames = s_dec->totalPCMFrameCount;
    s_framesRead  = 0;
    s_paused      = false;
    uint32_t dur  = durationSec();
    s_bitrate     = dur ? static_cast<uint32_t>(static_cast<uint64_t>(s_file.size()) * 8 / dur) : 0;
    audio_out::setSampleRate(s_sampleRate);
    return true;
}

void stop() {
    if (s_dec) {
        drflac_close(s_dec);
        s_dec = nullptr;
    }
    if (s_file) s_file.close();
    s_arenaTop    = 0;
    s_paused      = false;
    s_framesRead  = 0;
    s_totalFrames = 0;
}

bool active() { return s_dec != nullptr; }

bool decodeChunk() {
    if (!s_dec || s_paused) return false;
    drflac_uint64 frames = kChunkFrames;
    if (s_channels > 2) frames = (kChunkFrames * 2) / s_channels;
    drflac_uint64 got = drflac_read_pcm_frames_s16(s_dec, frames, s_pcm);
    if (got == 0) return false;
    if (s_channels == 1) {
        for (size_t i = static_cast<size_t>(got); i-- > 0;) {
            s_out[i * 2]     = s_pcm[i];
            s_out[i * 2 + 1] = s_pcm[i];
        }
        audio_out::write(s_out, static_cast<size_t>(got) * 4);
    } else if (s_channels == 2) {
        audio_out::write(s_pcm, static_cast<size_t>(got) * 4);
    } else {
        for (size_t i = 0; i < static_cast<size_t>(got); i++) {
            s_out[i * 2]     = s_pcm[i * s_channels];
            s_out[i * 2 + 1] = s_pcm[i * s_channels + 1];
        }
        audio_out::write(s_out, static_cast<size_t>(got) * 4);
    }
    s_framesRead += got;
    return true;
}

void setPaused(bool p) { s_paused = p; }

void seekSec(uint32_t sec) {
    if (!s_dec) return;
    uint64_t frame = static_cast<uint64_t>(sec) * s_sampleRate;
    if (s_totalFrames && frame > s_totalFrames) frame = s_totalFrames;
    if (drflac_seek_to_pcm_frame(s_dec, frame)) {
        s_framesRead = frame;
        audio_out::silence(0);
    }
}

uint32_t currentSec()  { return s_sampleRate ? static_cast<uint32_t>(s_framesRead / s_sampleRate) : 0; }
uint32_t durationSec() { return s_sampleRate ? static_cast<uint32_t>(s_totalFrames / s_sampleRate) : 0; }
uint32_t sampleRate()  { return s_sampleRate; }
uint32_t bitrate()     { return s_bitrate; }

}
