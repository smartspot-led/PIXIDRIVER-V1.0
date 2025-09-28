# Firmware Notes

- Target: ESP32
- Core: Arduino ESP32
- Effects: WS2812FX (optional)
- Persistence: Preferences (NVS) for global brightness and templates (if enabled)

## Build
- Arduino IDE: open `firmware/PIXIDRIVER_Pro_*.ino`
- PlatformIO: `pio run -t upload`

## Key files
- `PIXIDRIVER_Pro_*.ino` — main firmware
- Web UI is embedded as PROGMEM HTML