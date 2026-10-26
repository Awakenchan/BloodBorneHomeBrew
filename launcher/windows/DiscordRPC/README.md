
# Bloodborne Discord Rich Presence

A standalone Discord Rich Presence companion for Bloodborne PC,
integrated with Lantern Engine on Windows.

## Features

- Displays the current area and boss encounters.
- Shows Hunter Level, Echoes, Insight, and attributes.
- Starts automatically when the launcher detects the game.
- Exits automatically when the game process closes.
- Uses read-only process memory access.

## Building

Requirements:
- Windows
- MSVC (C++17) or Clang

Run `build.cmd` from this directory to compile
`BloodborneRPC.exe`.

## Runtime

The Windows release should contain:

discord_rpc/
- BloodborneRPC.exe
- config.json

The companion requires Lantern Engine's built-in
Discord Presence to be disabled.

## Compatibility

Designed for offline gameplay. Online and co-op
mode handling must be verified before enabling
the companion for those sessions.

## Credits

- [@erezeth](https://github.com/erezeth/bloodborne-rpc):
  C++ implementation and Discord Rich Presence integration.
- CLS_AMG: Memory pointer and boss/area detection references.
- Shiningami and ASNH0: Player stats pointer references.

## License

GPL-2.0-or-later. See LICENSE.

The bundled nlohmann/json header is MIT-licensed
and retains its original copyright notice.
