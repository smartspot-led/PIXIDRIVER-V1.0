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

## Pull Request checklist
- [ ] Clear description and motivation
- [ ] Tested on hardware (ESP32 + at least one LED model)
- [ ] No blocking delays in hot paths
- [ ] No API/route changes without documenting README