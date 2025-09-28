# Contributing

Thanks for considering a contribution!

## Principles
- Keep the render/DMX hot path **non-blocking**.
- Avoid frequent flash writes; debounce NVS saves.
- Prefer **unicast** networking for test setups.

## Dev setup
- Arduino IDE (ESP32 core) or PlatformIO
- Format your code, keep functions focused
- Explain timing-sensitive changes in PR notes
