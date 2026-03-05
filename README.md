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
- `mdslink [options] <input files...>`
  - Options:
    - `-o <mdsseq.bin> <mdspcm.bin>` or `--output <mdsseq.bin> <mdspcm.bin>`
    - `-i <mdsseq.inc>` or `--asm-header <mdsseq.inc>`
    - `-h <mdsseq.h>` or `--c-header <mdsseq.h>`
  - Inputs can be `.mml` or `.mds`
  - Outputs default to `mdsseq.bin` and `mdspcm.bin` in the current working directory
- `quickrom [--out <rom.bin>] <input files...>`
  - Generates `mdsseq/mdspcm` from the given `.mml/.mds` inputs and patches them into a template ROM.
- `check <path>` or `check --stdin --path <path>`
  - Options:
    - `--json` (emit a JSON report with `errors` and `warnings`; can appear before or after the path)

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

## Check JSON output

`ctrmml-cmd check --json <path>` prints a single JSON object to stdout:

```json
{
  "ok": false,
  "errors": [
    {
      "message": "missing pcm sample: mypcm/909-2.wav",
      "path": "song.mml",
      "line": 1,
      "col": 11,
      "length": 14,
      "code": "pcm_missing"
    }
  ],
  "warnings": [
    {
      "message": "slur may not affect articulation of previous note",
      "path": "song.mml",
      "line": 2,
      "col": 6,
      "length": 0,
      "code": "parse_warning"
    }
  ]
}
```

## C API

Public header: `src/ctrmml_cmd_c_api.h`.

- `ctrmml_cmd_check_file_json`
- `ctrmml_cmd_check_text_json`

## STDIN mode

Use `--stdin --path <path>` when you want to pass MML via stdin. The `--path` value is used as the base path for resolving relative assets.

## Requirements

- CMake and a C++ compiler (C++17).
- Depends on ctrmml and libvgm.
  - https://github.com/superctr/ctrmml
  - https://github.com/ValleyBell/libvgm

## Build

Builds a native CLI using FetchContent to obtain ctrmml/libvgm.
`assets/template.bin` is embedded into the executable at build time.

```sh
cmake -S . -B build
cmake --build build
```

## External

This project is used here:

- https://github.com/ulalume/zed-ctrmml
- https://github.com/ulalume/language-server-ctrmml

## License

GPL v2

Third party licenses: [licenses/THIRD_PARTY_NOTICES.md](licenses/THIRD_PARTY_NOTICES.md)
