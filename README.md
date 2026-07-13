# spi-flash-toolkit

STM32/GD32F103 firmware + browser-based (Web Serial) toolkit for working with
SPI EEPROM and NOR flash chips over a simple UART link.

## Features

- **Read / dump** a chip to a `.bin` file, with a live hex viewer.
- **Write & verify** a `.bin` file back to a chip, with automatic per-batch
  checksum + retry to survive a flaky USB-serial link, followed by a full
  read-back comparison against what was written.
- **Identify Chip** (JEDEC ID) and **Read Status** / **Clear Protection** /
  **Protect All** for chips with SPI status-register write protection.
- **Erase Chip** for NOR flash parts that require it before writing.
- **Compare** a chip dump against a reference `.bin` file, with a collapsed
  hex diff view for large files.
- **Bitmap/font viewer and editor** — page through a dump interpreting it as
  a grid of glyphs (configurable width/height/bpp/bit order/packing/rotation/
  stride), realign by clicking, toggle individual pixels, and export the
  edited region or the whole dump back to a `.bin` file.

## Layout

- `user/src/main.c` — firmware: SPI2 + USART1 + DMA, wire protocol documented
  at the top of the file.
- `web/index.html` — the browser UI (Chrome/Edge/Opera, Web Serial API).
- `device/` — CMSIS device headers/startup for the STM32F1 family.
- `cmake/`, `CMakeLists.txt`, `build.ps1`, `setup.ps1` — build system.
- `scripts/` — standalone PowerShell helpers (flashing, size reports, raw
  dump/write without the web UI).

## Building

```powershell
./setup.ps1   # first time only
./build.ps1
```

See `.vscode/tasks.json` for build/flash tasks (ST-Link or CMSIS-DAP).
