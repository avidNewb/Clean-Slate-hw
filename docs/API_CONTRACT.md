# Clean Slate Device API Contract (`v1`)

This firmware targets the **Clean Slate FastAPI** backend (hosted in Azure App Service or locally during development) using standard HTTP/HTTPS JSON endpoints and local client-side overlay compositing.

## Endpoints Consumed

### 1. Device Configuration & Dynamic App Schemas
- `GET /api/v1/device/{device_id}/config`:
  - `device_name`, `favorite_teams`, `enabled_apps` (for rotary encoder cycling), `quiet_hours.{enabled,start,end,timezone}`, `brightness_override` (`0..255` or `null`).
- `POST /api/v1/device/{device_id}/config`:
  - Pushed automatically after Captive Portal onboarding and whenever settings are saved on the local ESP32 web configuration portal (`http://<device-ip>/`), syncing `device_name`, `favorite_teams`, `quiet_hours`, `brightness_override`, and server `app_settings`.
- `GET /api/v1/apps`:
  - Proxied via `/api/local/apps` on the ESP32 web UI so server-defined `SettingField` schemas render dynamically in the local settings page.

### 2. Display Frame + Overlays + Alerts
`GET /api/v1/device/{device_id}/frame[?app_id=<id>]`

- `app_id` is omitted in **Auto** mode (rotary default) so the server schedule picks the active app (`clock`, `cfb_scoreboard`, etc.), or supplied when the user turns the rotary encoder to pin an enabled app.
- JSON response fields handled:
  - `app_id`: active server app ID.
  - `refresh_seconds`: next poll interval in seconds.
  - `quiet_mode`: bool (suppresses DFPlayer alert audio / chimes).
  - `frame_b64`: base64-encoded 4,096-byte **big-endian RGB565** (`>H`) `64x32` background framebuffer.
  - `overlays[]` (up to 8 entries composited locally every 100 ms over `bg_rgb888`):
    - `type`: `"ntp_time"`, `"ntp_date"`, `"countdown"`, or `"text"`
    - `x`, `y`: pixel anchor coordinates
    - `font`: `"3x5"`, `"4x6"`, or `"5x7"`
    - `align`: `"left"`, `"center"`, or `"right"`
    - `color`: hex color `"#RRGGBB"`
    - `format`: `strftime`-style hint (`%I:%M`, `%I:%M %p`, `%H:%M`, `%H:%M:%S`, optional `NOW ` prefix)
    - `target_epoch`: Unix epoch seconds for live local `"countdown"` ticks
    - `text`: static string for `"text"` overlays
  - `alert` (optional one-time scoring/event overlay):
    - `event_type`: alert type label
    - `sprite_strip`: `{ data_b64, frame_count, frame_duration_ms, compression }` (raw or `zlib`-compressed stream of `64x32` big-endian RGB565 frames; streamed via ESP32 ROM `tinfl_decompress`)
    - `marquee`: `{ text, y, color }` scrolled at 2 px/step across the lower band (`y=25..31` cleared) while looping the last 8 frames
    - `audio`: `{ folder, track, volume }` dispatched to the DFPlayer Mini over UART (`01/001..004.mp3`, `02/...`, `03/...`)

### 3. OTA Firmware Check & Download
- `GET /api/v1/firmware/check?current_version=<semver>`
  - Checks `update_available` and resolves `download_url` (default `/api/v1/firmware/download`).
- Downloads signed/unsigned `.bin` into the passive `ota_0`/`ota_1` slot via `esp_https_ota` using the ESP-IDF mbedTLS certificate bundle (`esp_crt_bundle_attach`) and reboots on success.
