# MEMORY — cardputer-music (player de música p/ M5Stack Cardputer ADV)

> Estado do projeto, decisões, histórico de bugs e pendências.
> Atualizado: 2026-10-15. Manter este arquivo atualizado ao final de cada sessão.

## 1. Objetivo e requisitos do usuário

Player de música para **M5Stack Cardputer ADV**. Requisitos originais:
1. Músicas ficam no **SD**; usuário navega pastas no device, aperta play e ouve.
2. **Máxima qualidade de áudio possível** no hardware (cadeia bit-perfect).
3. Projeto em `/home/roe/projetos/cardputer-music/`.
4. Compatível com o **Launcher** (bmorcelli/Launcher): firmware app-only, instalar via SD/WebUI.
5. UI escolhida pelo usuário: **UI B "Now Playing"** (mockups em `mockups/`).
6. Acervo em `/home/roe/Músicas/` (592 arquivos) — **NUNCA modificar**: 504 .m4a (AAC-LC 44.1/48kHz ~320kbps), 87 .mp3 (320kbps), 1 .flac.

## 2. Hardware — Cardputer ADV (fatos confirmados em hardware)

- SoC: **ESP32-S3FN8**, 240MHz, 8MB flash, **SEM PSRAM** (sondas do boot dão erro benigno).
- LCD ST7789V2 1.14" 240x135 (M5GFX/M5Unified cuida).
- Teclado: controlador I2C **TCA8418** (M5Cardputer lib 1.2.0 cuida; IRQ-driven).
- Codec: **ES8311** (I2C addr **0x18**, bus interno do M5Unified). **Mono**, DAC até 24-bit/48kHz (mixa L+R no hardware). Amp NS4150B + speaker 1W 8Ω. Jack 3.5mm com mute de speaker por hardware.
- I2S: **BCLK=41, WS/LRCK=43, DOUT=42, sem MCLK**.
- SD (SPI): **CS=12, MOSI=14, SCK=40, MISO=39**; montado a 10MHz (fallbacks 5/2.5/1.25MHz).
- Bateria ADC GPIO10; backlight GPIO38; IMU BMI270 + RTC PCF8563 (não usados).

## 3. Stack e arquitetura de áudio (decisões fechadas)

- PlatformIO + **pioarduino 55.03.312-1** (ESP-IDF 5.5), board `m5stack-stamps3`, `-std=gnu++17`, `-DBOARD_HAS_PSRAM=0`, `-DARDUINO_LOOP_STACK_SIZE=16384`.
- Libs: `M5Unified@0.2.26`, `M5Cardputer@1.2.0` (git), **ESP32-audioI2S 3.0.12 VENDORIZADA em `lib/`** (com patches — ver §5).
  - POR QUÊ 3.0.12: é a última versão com fallback de buffer p/ SRAM interna (4.x/3.3.x exigem PSRAM).
- **Cadeia bit-perfect**: decodifica 16-bit e escreve I2S na **sample rate nativa** (sem resample); ganho do software da lib = unitário (`setVolume(maxVolume())`); **volume do usuário = registrador 0x32 do ES8311** (DAC, passos ~2dB, 0xBF=0dB máx, 0x00=mute).
- **ARQUITETURA I2S PRÓPRIA** (`src/audio_out.*`): a lib audioI2S cria o canal I2S dela no ctor global, mas **nós não usamos esse canal**. Implementamos o hook weak `audio_process_i2s()` em `player.cpp`: sempre `*continueI2S = false` (a lib nunca escreve no canal dela) e escrevemos o PCM no **nosso** canal `i2s_std` (I2S_NUM_AUTO, `auto_clear_after_cb=false`, dma 8×512, 16-bit estéreo).
  - Troca de rate: `audio_out::setSampleRate(getSampleRate())` após `connecttoFS`.
  - Flush de silêncio (16KB = anel DMA) após `stopSong` + após `connecttoFS` + ao pausar (anel com auto_clear=false repete dados velhos).
  - M5Unified configurado com `internal_spk=false; internal_mic=false` + `M5.Speaker.end(); M5.Mic.end();` (evitar I2S duplicado).
- Concorrência: task dedicada `audio_pump` (24KB, prio 5, core 1) roda `s_audio.loop()`; chamadas de transporte (start/seek/pause) serializadas via **FreeRTOS queues** (`s_qStart` xQueueOverwrite + `s_qEv` CMD_SEEK/CMD_PAUSE) — nunca chamar `connecttoFS` direto da UI.

## 4. Estrutura de código (src/)

| arquivo | conteúdo |
|---|---|
| `config.h` | pinos, constantes, paleta UI B (rgb565), `CM_LOG` (log serial guardado por `Serial.availableForWrite()>160` — NUNCA bloqueia), `VOLUME_MAX 21` |
| `es8311.h/.cpp` | init ES8311 (seq 0x00=0x80,01=0xB5,02=0x18,0D=01,12=00,13=10,32=BF,37=08 via `M5.In_I2C.writeRegister8(0x18,reg,val,400000)`, delay(2)); volume DAC |
| `audio_out.h/.cpp` | canal I2S próprio (ver §3) |
| `flac_player.h/.cpp` | caminho FLAC próprio: dr_flac (v0.13.4 vendorizado em `src/dr_flac.h`) + **arena estática de 40KB** (bump allocator com free-no-topo; reset em start/stop) — drflac_open determinístico mesmo com heap fragmentado (max block chegava a 7.6KB < 34KB do decoder) |
| `player.h/.cpp` | estado, fila de faixas, task de áudio, hook `audio_process_i2s`, callbacks `audio_eof_mp3`/`audio_id3data`/`audio_info`, NVS `cardmusic` (vol/dir/track) |
| `browser.h/.cpp` | listagem SD via POSIX opendir/readdir (paths estilo VFS `/sd/...`), dirs-first + natural sort, esconde não-áudio, 8 linhas |
| `ui.h/.cpp` | UI B: M5Canvas 240x135 16bpp, Now Playing (título marquee, progresso, espectro, badge codec), browser overlay, OSD volume, tela de erro |
| `main.cpp` | setup (M5Cardputer.begin com spk/mic off, mountSD, es8311, player, browser, ui, task), loop (edge-detection de teclas + auto-repeat próprio: 350ms delay, 90ms repeat; flags enter/backspace/tab na borda) |

**Boundary crítico de paths**: API Arduino (`SD.open`/`SD.exists`/`connecttoFS`) espera path **RELATIVO** ao mountpoint (ex. `/Mr.Kitty.m4a`); API POSIX/VFS (`opendir`/`stat`) usa `/sd/...`. O player faz strip do `/sd` antes de `connecttoFS` (`src/player.cpp`, busca `strncmp(sc.path, "/sd", 3)`).

**Teclas**: `;`/`.` navega (ou volume ± com toast), `,`/`/` prev/next (ou page na lista), `[`/`]` seek ±5s, `b` abre/fecha lista, espaço pause/play ou abrir, ENTER ativar, Backspace/`` ` `` voltar. Auto-repeat só em `; . , / [ ]`.

## 5. Patches na lib vendorizada (`lib/ESP32-audioI2S/`)

1. **Audio.cpp ~1853 (read_ID3_Header, controlCounter 6)** — overflow: `fs` era limitado a **1024** mas `m_ibuff` sem PSRAM tem **576** bytes (heap) → corrupção de heap → crash no ISR I2S (`Stack canary IDLE0`, memset de 4GB). Fix: `if(fs > (size_t)(m_ibuffSize - 1)) fs = m_ibuffSize - 1;`.
2. **Audio.cpp ~140 (bytesWasRead)** — wrap `if(m_readPtr >= m_endPtr)` mapeava `readPtr == endPtr` para `buffer-1` (ponteiro inválido), então `readPtr == endPtr` nunca podia ocorrer. Fix: `>` em vez de `>=` (readPtr==endPtr = 1 byte restante, posição legal).
3. **Audio.cpp states 2 (~1748), 5 (~1812) e 98 (~1942) de read_ID3_Header (skips)** — stall `audioHeader reading timeout` em MP3 com ID3 grande: no estado CHEIO do AudioBuffer (`readPtr == writePtr`), consumir `len` inteiro zerava `writeSpace()` para sempre. Fix: condição `> len` → `>= len` e `take = (len > 1) ? len - 1 : len` (sempre deixa 1 byte; wrapped landa em endPtr que é legal; CHEIO landa em writePtr-1). Comentários `// PATCH` no código.
4. **aac_decoder/aac_decoder.h linhas 10-14 (guard SBR)** — `defined(BOARD_HAS_PSRAM)` é verdadeiro mesmo com `-DBOARD_HAS_PSRAM=0` → SBR (HE-AAC) compilado, buffers de 60KB → OOM (`can't allocate 50788 bytes`) em heap fragmentado. Fix: guard agora exige `&& BOARD_HAS_PSRAM`. Acervo é todo AAC-LC — sem perda.
5. **FLAC**: o guard `FLAC works only with PSRAM!` da lib FICA (restaurado de propósito). Motivo técnico: `m_frameSizeFLAC=16384` > buffer interno (16KB) + reserve 2048 → `getReadPtr()` faz memcpy de overlap até ~16KB em região de 2047 bytes → overflow de heap. **FLAC é impossível sem PSRAM nesta lib** (não é só o guard). Solução: FLAC é tocado por **caminho próprio** (`src/flac_player.*` + dr_flac + arena), fora da lib.

Anotados, NÃO corrigidos (não no acervo): WAV (`m_frameSizeWav=2048` → shadow de 2049 > 2047 = overflow de 2 bytes) e OPUS (`celt.cpp:2014` tem o mesmo `#ifdef BOARD_HAS_PSRAM`).

## 6. Histórico de bugs resolvidos (ordem cronológica)

1. **SD path duplo** (`/sd/sd/...` ENOENT): VFS vs Arduino API — resolvido com strip no player (§4).
2. **Crash `Stack canary watchpoint triggered (IDLE0)`** ao tocar MP3: forense longa (backtrace `i2s_dma_tx_callback` → `gdma_default_tx_isr`, memset de 4GB com handle lixo). Causa raiz: overflow do ID3 (§5.1). O bypass I2S próprio (§3) foi feito no caminho e ficou (é mais robusto que o canal da lib com `auto_clear=true`).
3. **Stall `audioHeader reading timeout`** no MP3: deadlock do AudioBuffer no estado CHEIO (§5.2 + §5.3).
4. **OOM no M4A** após ciclos de faixas (`AACDecoder_AllocateBuffers OOM in SBR, can't allocate 50788 bytes`): SBR compilado por engano (§5.4).
5. **Teclado "lento/rápido demais"**: repeat a 250Hz (keysState é level-triggered) + loop bloqueado por Serial USB CDC sem leitor. Fixes: edge-detection + repeat próprio; `CM_LOG` guardado. Usuário validou "teclado normal".
6. **FLAC reabria falhando (`flac start=0`)** após ciclos MP3/M4A: heap fragmentado (maior bloco contíguo 7.6KB < ~34KB que o drflac_open aloca de uma vez) → resolvido com arena estática de 40KB no flac_player (validado em hardware: dec=1 sempre, arena=37840).
7. **Seek de M4A** na lib 3.0.12 deixa o decoder AAC sujo (resync em loop; 3.4.7 adiciona `m_decoder->clear()`). Contornado: prev/restart = reconnect completo (`startTrack`); `[`/`]` seek instrumentado (se M4A falhar, desabilitar com toast).

## 7. Fluxo de build/flash/monitor (ambiente do usuário)

```bash
cd /home/roe/projetos/cardputer-music
/home/roe/.local/bin/pio run                 # build → .pio/build/cardputer-adv/firmware.bin
ls /dev/ttyACM*                              # PORTA ALTERNA entre ttyACM0/1 a cada re-enumeração!
/home/roe/.local/bin/pio run -t upload --upload-port /dev/ttyACM0
# se ficar em "waiting for download" (boot:0x3) após flash:
/home/roe/.platformio/penv/bin/python /home/roe/.platformio/packages/tool-esptoolpy/esptool.py --port /dev/ttyACM0 --before no-reset --after watchdog-reset chip_id
# monitor (pyserial): abrir a porta RESETA o chip (rst:0x15, benigno)
timeout 120 python3 /tmp/opencode/sermon.py /dev/ttyACM0 115
```

- Abrir a serial reseta o chip (USB_UART_CHIP_RESET) — sempre benigno.
- `esptool` do sistema não funciona (falta rich_click) — usar o do penv.
- MAC do device: 28:84:85:75:e0:d8.
- Instalação final pelo usuário: copiar `firmware.bin` para o SD e instalar pelo Launcher (SD > arquivo > Install), ou via WebUI bmorcelli.github.io/Launcher.

## 8. Estado atual (2026-10-15)

- **GitHub**: repo PÚBLICO `roe-beepppp/cardputer-music` (GPL-3.0), branch `main`; tags v0.1.0/v0.1.1/v0.2.0/v0.3.0 + releases com `.bin` anexado (`dist/cardputer-music-vX.Y.Z.bin`) para instalar pelo Launcher e poder voltar versão. Convenção: cada alteração validada = commit + tag + push.
- **v0.3.0 — WEB RADIO**: tecla `R` alterna modo rádio. WiFi STA lê `/sd/wifi.txt` (SSID linha 1, senha linha 2); estações de `/sd/radio.txt` (parser tolerante: Nome|URL, vírgula, URL sozinha, #, M3U — 20 estações do usuário parseadas OK). Streaming via `connecttohost` (http/https) usa o MESMO I2S próprio + volume ES8311; ICY mostra nome da estação + música atual; retry 3x na mesma URL e depois próxima; teclas: `,`/`/` troca estação, `B` lista (ENTER toca), `;`/`.` volume, `SPACE` pausa, `BACKSPACE`/`R` sai. Canvas vira 8bpp no modo rádio (`ui::setLowMem`) p/ o WiFi caber no heap; WiFi OFF ao sair (libera heap p/ música/FLAC).
- **FIX do WiFi `ESP_ERR_NO_MEM` aplicado e BUILDADO (não gravado no device — estava desconectado na hora)**: no `toggleRadio` (src/main.cpp) o `ui::setLowMem(true)` agora roda ANTES de `radio::enter()` (esp_wifi_init precisa de heap contíguo; antes rodava com canvas 16bpp → ~14KB livres → NO_MEM). Log `[RADIO] wifi init heap=%u max=%u` adicionado em radio.cpp.
- **v0.3.1 (build OK, flash/teste de hardware PENDENTE — device estava desconectado)**: heap do modo rádio — (1) custom_sdkconfig mais enxuto: WiFi STATIC_RX 4→2, DYNAMIC_RX 12→6, STATIC_TX 4→2, DYNAMIC_TX removido (linha inerte), + CONFIG_LWIP_TCP_QUEUE_OOSEQ=n (~10-16KB de ganho); (2) `freeEntries()` no browser ao entrar no rádio (libera o vetor de arquivos); (3) `R` agora SAI do rádio (case 'r'/'R' no bloco radio do keyChar — README prometia e não saía). Objetivo: fazer o decoder (MP3 ~20KB / AAC 27.4KB contíguo) alocar com o WiFi ligado. Baseline medido no v0.3.0: streaming com WiFi conectado deixa só ~10.7KB livres → decoders falhavam com OOM (MP3 e AAC). HTTPS segue inviável sem PSRAM (handshake ~35-45KB). Se ainda faltar memória: patch InBuff 16000→13000 ou desenhar direto sem canvas (32KB) ou aceitar só MP3 no rádio. 4bpp descartado (sprites M5GFX são paletizados, UI usa RGB565).
- **PENDENTE: testar rádio no hardware.** WiFi já validado no v0.3.0 (conecta: `wifi connected ip=192.168.1.35`, ssid 'ROE' passlen=8). Testar no v0.3.1: estações MP3 vs AAC (OOM resolvido?), `R` para sair, lista `B`+ENTER, volume, pausa.
- **v0.2.0 validado em hardware**: FLAC toca e reabre sempre (arena estática 40KB); MP3/M4A intactos. 3 UIs (tecla `V`: Terminal → Now Playing → Tape Deck, persiste na NVS "ui").
- `[`/`]` seek ainda NÃO testado pelo usuário (M4A é o caso de risco — decoder AAC sujo; se der flood, desabilitar seek p/ M4A).
- Logs de debug ATIVOS via CM_LOG (`[HB]`, `[KEY]`, `[PLAYER]`, `[FLAC]`, `[RADIO]`, `[AUDIO]`). Silenciar (CM_LOG_ENABLED 0) quando tudo validado.

## 9. O que NUNCA fazer

- Não modificar `/home/roe/Músicas/`.
- Não usar versões da audioI2S ≥ 3.3 (exigem PSRAM) nem trocar a vendorizada sem reaplicar os patches §5.
- Não chamar `s_audio.connecttoFS` fora da task de áudio (usar `player::playList`/`next`/`prev`).
- Não usar `Serial.printf` direto no loop (usar CM_LOG — evita bloquear o loop por USB CDC).
- Não remover o strip do `/sd` no player (a API Arduino duplica o mountpoint).
