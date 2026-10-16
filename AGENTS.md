# AGENTS.md — cardputer-music

Player de música para M5Stack Cardputer ADV (ESP32-S3, sem PSRAM). Leia `MEMORY.md` para estado completo, decisões e histórico de bugs.

## Comandos

```bash
/home/roe/.local/bin/pio run                                    # build (~40-70s)
ls /dev/ttyACM*                                                 # SEMPRE conferir: porta alterna 0/1
/home/roe/.local/bin/pio run -t upload --upload-port /dev/ttyACMn   # flash (~40s)
timeout 240 python3 /tmp/opencode/sermon.py /dev/ttyACMn 235        # monitor serial
```

- Após flash o chip pode ficar em download mode ("waiting for download") → `--before no-reset --after watchdog-reset chip_id` com o esptool do penv (ver MEMORY.md §7). O esptool do sistema NÃO funciona.
- Abrir a serial reseta o chip (rst:0x15) — benigno.
- Entrega final: copiar `.pio/build/radio/firmware.bin` para o SD e instalar pelo Launcher (SD > arquivo > Install).

## Regras do projeto

- NUNCA modificar `/home/roe/Músicas/` (nem copiar pra lá).
- NUNCA chamar `radio::enter()` sem `ui::setLowMem(true)` antes — o `esp_wifi_init` precisa de heap contíguo; com canvas 16bpp dá `ESP_ERR_NO_MEM`. Ao sair do rádio: `radio::leave()` (WiFi OFF) ANTES de `ui::setLowMem(false)`.
- custom_sdkconfig (platformio.ini) controla o hash TASMOTA de `sdkconfig.defaults` — mudar = reinstall do framework (~350-500s de build). Se der erro estranho no build do core (ex. IPv6/esp-modbus), `rm sdkconfig.cardputer-adv` e rebuildar. NUNCA commitar `sdkconfig.defaults`/`sdkconfig.cardputer-adv` (raiz).
- Logs: usar `CM_LOG` de `src/config.h` (nunca `Serial.printf` direto no loop — USB CDC sem leitor bloqueia o loop).
- ESP32-audioI2S é **vendorizada** em `lib/` com 5 patches (overflow ID3, wrap bytesWasRead, deadlock do AudioBuffer nos skips ID3, guard SBR, guard FLAC — ver MEMORY.md §5). Nunca substituir por versão de registry (≥3.3 exige PSRAM; upstream quebra sem PSRAM).
- FLAC: tocado por caminho PRÓPRIO (`src/flac_player.*` + dr_flac + arena estática de 40KB) — o guard da lib fica desativado de propósito; não reativar o FLAC da lib (overflow sem PSRAM).
- 3 temas de UI (Terminal/Now Playing/Tape Deck) selecionáveis com `V`, persistidos na NVS ("ui").
- Chamadas de transporte da lib (`connecttoFS`/seek/pause) SÓ via queues da task de áudio (`src/player.cpp`).
- API Arduino SD usa path relativo ao mount (`/arquivo.m4a`); POSIX/VFS usa `/sd/...` — strip feito no player.
- Áudio sai pelo canal I2S próprio (`src/audio_out.*`) via hook `audio_process_i2s`; volume no ES8311 (reg 0x32), não em software.

## Versionamento (pedido do usuário: "manter versões a cada alteração")

- Repo público: `https://github.com/roe-beepppp/cardputer-music` (branch `main`; credencial já salva em `~/.git-credentials`). Releases carregam o `.bin` da versão (asset `cardputer-music-vX.Y.Z.bin`) para instalar pelo Launcher.
- A cada alteração concluída e validada: `git add -A && git commit -m "vX.Y.Z: resumo" && git tag vX.Y.Z && git push origin main --tags`.
- Semver simples: v0.1.0 = primeiro estado funcional (validado em hardware); incrementar minor por feature, patch por fix.
- Manter MEMORY.md §8 atualizado no mesmo commit.

## Fluxo de teste com o usuário

Usuário tem o device USB conectado; ele testa fisicamente (teclas: `;`/`.` navega, espaço play, `/` próxima, `[`/`]` seek, `v` cicla tema). Peça para ele tocar: (1) um .m4a, (2) o .mp3 Blind Guardian (regressão dos patches ID3), (3) o FLAC (regressão da arena), (4) pular faixas, pausar, seek, (5) ciclar os 3 temas com `v`. Confirmar posição avançando no `[HB]` do monitor.
