# ctrmml-cmd

CLI for ctrmml playback, export, and highlight streaming.

## Subcommands

- `play <path>` or `play --stdin --path <path>`
  - Options:
    - `--start line:col` (play from cursor; uses nearest event at/after this position)
    - `--follow` (emit highlight updates while playing)
- `stop`
- `export <path>` or `export --stdin --path <path>`
  - `--vgm` or `--wav`
  - `--out <path>` (defaults to same dir/name with .vgm/.wav)
- `check <path>` or `check --stdin --path <path>`

## Highlight output (when `--follow`)

JSON lines to stdout:

```json
{
  "type": "highlight",
  "ticks": 1234,
  "positions": [
    { "line": 10, "col": 5 },
    { "line": 12, "col": 20 }
  ]
}
```

- `line` and `col` are 0-based, matching `InputRef` (`get_line`, `get_column`).
- Multiple positions represent simultaneous tracks.

## STDIN mode

Use `--stdin --path <path>` when you want to pass MML via stdin. The `--path` value is used as the base path for resolving relative assets.

## Requirements

- CMake and a C++ compiler (C++17).
- Depends on ctrmml and libvgm.
  - https://github.com/superctr/ctrmml
  - https://github.com/ValleyBell/libvgm

## Build

Builds a native CLI using FetchContent to obtain ctrmml/libvgm.

```sh
cmake -S . -B build
cmake --build build
```

## External

This project is used here:
- https://github.com/ulalume/zed-ctrmml
- https://github.com/ulalume/language-server-ctrmml
