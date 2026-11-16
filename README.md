# Clean Slate Hardware Firmware (`clean_slate_hw`)

ESP-IDF (`v6.0.2`, `esp32` target) thin-client firmware for the legacy 64×32 HUB75 LED matrix hardware (`Sports_Display` pinout), paired with the **Clean Slate** FastAPI cloud backend.

## Architecture Overview

- **Zero-change Hardware Pinout**: Preserves the exact `ESP32_FORUM_PINOUT` HUB75 signals (`board_pins.h`), input-only controls/light sensor on GPIOs 34–39, and DFPlayer Mini UART (`RX=GPIO 13`, `TX=GPIO 0`). GPIO 2 is reserved solely for HUB75 `R1` (no onboard LED conflict).
- **Zero-flicker I2S Parallel HUB75 Driver (`hub75_driver.c`)**: Drives the 64×32 1/16-scan panel via ESP32 I2S0 16-bit LCD DMA parallel mode with BCM (Binary Code Modulation) bitplanes and double-buffered swap.
- **Local SNTP Time + Crisp Bitmap Compositor (`sntp_time.c`, `gfx_font.c`)**: Maintains local wall-clock time via SNTP with server-synced IANA timezone mapping (`timezone` -> POSIX `TZ`) and composites live `ntp_time`, `ntp_date`, `countdown`, and `text` overlays (`3x5`, `4x6`, `5x7` fonts) over the cached RGB565BE background frame.
- **Streaming Alert Animation & DFPlayer Audio (`api_client.c`, `dfplayer.c`, `main.c`)**: Decompresses zlib `sprite_strip` animations incrementally via ESP32 ROM `tinfl_decompress`, scrolls alert `marquee` tickers, and plays local microSD soundpack tracks (`/01`, `/02`, `/03`).
- **Dual-OTA Partition Layout (`partitions.csv`)**: Supports `ota_0` / `ota_1` updates via `/api/v1/firmware/check` and `/api/v1/firmware/download` over HTTPS (Azure App Service TLS certificate bundle) or HTTP.

## Hardware Pinout (`main/board_pins.h`)

| Subsystem | Signal | ESP32 GPIO | Notes |
| --- | --- | --- | --- |
| HUB75 Data | `R1, G1, B1` | `2, 15, 4` | GPIO 2 dedicated to `R1` (do not use as LED) |
| HUB75 Data | `R2, G2, B2` | `14, 27, 23` | |
| HUB75 Address | `A, B, C, D, E` | `5, 18, 19, 21, 12` | Keep GPIO 12 low on boot (3.3V flash) |
| HUB75 Control | `CLK, LAT, OE` | `22, 26, 25` | |
| Controls | Push Button | `35` | Input-only; requires external pull-up/down |
| Controls | Rotary Encoder `A, B` | `39, 36` | Input-only; cycles `auto` + `enabled_apps[]` |
| Sensor | Ambient Light ADC | `34` (`ADC1_CH6`) | Auto-dims across configurable NVS `min..max` range (default `15..255`) |
| DFPlayer Mini | `UART1 RX, TX` | `13, 0` | 9600 8N1; keep GPIO 0 unpulled on reset |

## Physical Controls

- **Rotary Encoder CW / CCW**: Cycles between server **Auto** schedule and each app in `enabled_apps[]`, triggering an immediate `/frame?app_id=...` fetch. Automatically returns to **Auto** schedule after `rotary_idle_sec` of inactivity (default `30` seconds; configurable in the local web UI or `Kconfig`).
- **Button Single Click (GPIO 35)**: Instantly dismisses an active alert animation and stops audio if an alert is playing; when idle, manually plays the **Fight Song** (`fight_folder`/`fight_track`, default global track `1`).
- **Button Double Click (GPIO 35)**: Manually plays the **Victory Song** (`victory_folder`/`victory_track`, default global track `2`).
- **Button Triple Click (GPIO 35)**: Manually plays the **3rd Song** (`third_folder`/`third_track`, default global track `3`).
- **Button Long Press (GPIO 35, >= 900 ms)**: Opens the on-device LED matrix setup menu (`VOLUME` 0–30 bar with knob adjust & double-click `0`/`15`/`30` preset cycle, `BRIGHTNESS` `AUTO` or manual `1–255` bar with double-click `AUTO`/`128` toggle, `WIFI` IP/AP setup trigger, `TIMEZONE` US preset cycling, and `FACTORY RST` hold-to-clear). Long-pressing while editing Volume/Brightness/Timezone cancels changes; idle timeout saves and exits back to normal display.

## Initial Onboarding & Local Web Configuration (`web_portal.c`)

1. **First-Time Setup (Captive Portal AP)**:
   - On a fresh flash (when no Wi-Fi credentials are stored in NVS) or after repeated failed Wi-Fi reconnects (`5` attempts), the ESP32 starts SoftAP **`CleanSlate-XXXX`** (`192.168.4.1`) with an automatic captive-portal DNS redirect.
   - Connect from a phone or laptop; the onboarding page opens automatically (or browse to `http://192.168.4.1/`).
   - Scan and select your 2.4 GHz Wi-Fi network, enter the Wi-Fi password, choose your **Favorite Team** (`ALA`, `TEX`, `OSU`, etc.), and confirm your **Clean Slate Server URL**.
   - On connect, the firmware saves everything to NVS (`settings_store.c`) and syncs your favorite team directly to the Clean Slate backend (`POST /api/v1/device/{device_id}/config`).
2. **Post-Connection Local Settings Page**:
   - Once on Wi-Fi, open **`http://<device-ip>/`** or **`http://<device-id>.local/`** (advertised via mDNS `_http._tcp`).
   - Features a **Live 64×32 Display Mirror** (`/api/local/frame`) with **Quick Active Screen** switcher, live light-sensor/OTA telemetry polling, and instant debounced sliders for volume & brightness.
   - Configure device name, favorite team(s), Clean Slate server URL, IANA timezone, NTP server, quiet hours, server poll interval, automatic OTA updates, active screen override, auto-brightness `min..max` range / manual brightness, knob return-to-auto idle delay, DFPlayer master volume (`0–30`), equalizer preset (`0–5`), startup tone, hourly chime, manual fight/victory song folder & track mapping, and dynamic server app settings (`/api/v1/apps` schema).
   - Use **Device & Audio Controls** for instant `Vol -` / `Mute`/`Unmute` / `Vol +`, `Play Fight Song`, `Play Victory Song`, `Test Boot Tone`, `Test Chime`, `Stop Audio`, `Refresh Display Now`, `Check Firmware Update` (with live status/result), `Export Config` / `Import Config` JSON backup, `Reboot Device`, `Change / Reset Wi-Fi`, and `Factory Reset`.

## Build & Flash (`ESP-IDF v6.0.2`)

1. Open PowerShell and load the ESP-IDF environment:
   ```powershell
   . "C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1"
   ```
2. Optionally set defaults (`CONFIG_CS_API_BASE_URL`, `CONFIG_CS_API_DEVICE_ID`; leave `CONFIG_CS_WIFI_SSID` blank to use Captive Portal onboarding):
   ```powershell
   idf.py set-target esp32
   idf.py menuconfig
   ```
3. Build, flash, and monitor:
   ```powershell
   idf.py build
   idf.py -p COMx flash monitor
   ```

## MicroSD Soundpack Layout (DFPlayer)

Matches `Clean_Slate/assets/soundpack/INDEX.txt`:
- `/01/001.mp3` – `003.mp3`: Team songs (1-click fight song, 2-click victory song, 3-click 3rd song; written first in `soundpack.zip` so legacy `folder = 0` global tracks `1..3` match on a freshly formatted FAT32 card)
- `/02/001.mp3` – `004.mp3`: Generic scoring cues (passing/generic TD/safety/victory horn, rushing TD cannon, field goal chime, turnover siren)
- `/03/001.mp3`: Hourly chime (`CONFIG_CS_PLAY_HOURLY_CHIME`)
- `/03/002.mp3`: Startup boot tone (`CONFIG_CS_PLAY_BOOT_TONE`)
