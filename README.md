# CardRadio

Web radio para o **M5Stack Cardputer ADV** (ESP32-S3FN8, sem PSRAM).
Boota direto no radio: WiFi (STA) + lista de estacoes do cartao SD + streaming
MP3/AAC/OGG via HTTP, com nome da estacao e musica atual (ICY) na tela.

App irmao (player de musica): **CardPlayer** — https://github.com/roe-beepppp/cardputer-player

## Instalar via Launcher (bmorcelli/Launcher)

1. No cartao SD (FAT32, MBR/SDHC):
   - `/wifi.txt`: **linha 1 = SSID, linha 2 = senha** (rede 2.4 GHz);
   - `/radio.txt`: uma estacao por linha.
2. Copie `CardRadio.bin` (build local em `.pio/build/radio/firmware.bin`, ou baixe
   dos Releases do GitHub) para o cartao SD.
3. No Cardputer com o Launcher: **SD > CardRadio.bin > Install**. O Launcher cria a
   particao de app adequada e o app aparece no bootscreen/menu.
4. O app nao precisa de bootloader/partition table proprios.

## radio.txt (formatos aceitos por linha)

- `Nome|URL`, `Nome,URL` ou apenas a `URL` (o nome vira o host);
- `#EXTINF:-1,Nome` seguido da URL (padrao M3U);
- linhas comecando com `#` = comentario.

Exemplo:

```
Radio Anos 80|http://live2.livemus.com.br:27400/live
Antena 1 RJ|http://a1rj.streams.com.br:7801/stream
```

> **Somente http://.** Estacoes `https://` sao ignoradas de proposito (o handshake
> TLS nao cabe na memoria sem PSRAM); o app mostra quantas foram ignoradas.

## Teclas

| Tela | Tecla | Acao |
|---|---|---|
| Radio | `,` / `/` | estacao anterior / proxima |
| Radio | `B` / `TAB` | abre/fecha a lista de estacoes (`ENTER` toca) |
| Radio | `;` / `.` | volume + / - (0..21, OSD na tela) |
| Radio | `SPACE` | pausa (desconecta) / continua |
| Radio | `V` | cicla o tema: Terminal / Now Playing / Tape Deck |
| Lista | `;` / `.` / `,` / `/` | navega / pagina |
| Lista | `DEL` / `B` | fecha a lista |

Ultima estacao, volume e tema ficam salvos na NVS.

## Notas de memoria (sem PSRAM)

- A tela e' desenhada **direto no display, sem canvas** (repaint por regioes com
  `startWrite/endWrite`) — os ~65KB economizados vao para o WiFi e os decoders.
- O audio sai pelo canal I2S proprio (16-bit, sample rate nativa do stream) com o
  volume aplicado no DAC ES8311 (reg 0x32) — mesma cadeia bit-perfect do CardPlayer.
- Conexao: timeout curto (4s) e 1 retry por estacao; servidor lento nao trava a UI.

## Build

```sh
pio run                     # gera .pio/build/radio/firmware.bin
```

Stack: platform pioarduino 55.03.312-1 (ESP-IDF 5.5), M5Unified 0.2.26,
M5Cardputer 1.2.0, ESP32-audioI2S 3.0.12. Board `m5stack-stamps3`.

### Pinagem (Cardputer ADV)

- I2S -> ES8311: BCK 41, LRCK 43, DOUT 42 (sem MCLK; codec escravo do BCLK)
- ES8311: I2C interno (SDA 8 / SCL 9), addr 0x18
- SD (SPI): CS 12, MOSI 14, SCK 40, MISO 39 (10 MHz, fallback 5/2.5/1.25 MHz)

## Estrutura

```
src/config.h      pinos, constantes, paletas dos 3 temas
src/es8311.*      init e volume HW do codec
src/audio_out.*   canal I2S proprio (16-bit, sample rate nativa)
src/radio.*       wifi.txt, parser do radio.txt, estado do WiFi
src/player.*      task de audio, ICY, fila de estacoes, NVS
src/ui.*          tela do radio + lista de estacoes + OSD (ASCII direct-draw)
src/main.cpp      setup/loop, teclado, mount do SD
src/browser.*     fontes compartilhadas com o CardPlayer (inativas neste app)
src/flac_player.* fontes compartilhadas com o CardPlayer (inativas neste app)
src/dr_flac.h     dr_flac v0.13.4 vendorizado (single header)
```

## Licenca

GPL-3.0 (ver `LICENSE`). Bibliotecas embutidas com licencas proprias:
ESP32-audioI2S (GPL-3.0, vendorizada com patches em `lib/`), dr_flac (public
domain / MIT-0, em `src/dr_flac.h`), M5Unified e M5Cardputer (MIT).
