# Building and testing

## What you need

| Tool | Notes |
|---|---|
| Windows 10/11 x64 | The plugin and tools are Windows-only. |
| Visual Studio 2022 or newer, or its Build Tools | Workload "Desktop development with C++", MSVC toolset 14.38 or later. Only MSVC is supported; CMake refuses other compilers. |
| CMake 3.20 or newer, Ninja | Taken from `PATH`, or from the copies that ship with Visual Studio. |
| PowerShell 5.1 | Built into Windows. |
| Python 3 | `python.exe` on `PATH`. Some checks in `tools\test.ps1` and `tools\gen_facts.py` use it. The Microsoft Store "python" alias does not count. |

No network access is needed. Every dependency is vendored under `third_party/`
(see [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)).

## Build

From the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build.ps1
```

`build.ps1` finds Visual Studio through `vswhere`, sets up the MSVC environment and runs CMake with
Ninja.

| Option | Default | Meaning |
|---|---|---|
| `-Config` | `Release` | `Release`, `RelWithDebInfo` or `Debug` |
| `-BuildDir` | `build` | Output folder |
| `-VcVarsVer` | newest installed | Pin an MSVC toolset, e.g. `14.38` |
| `-Clean` | off | Delete the build folder first |

Output goes to `build\bin\`:

| File | What |
|---|---|
| `EchoReplay.dll` | The plugin. Copy it to `<game folder>\bin\win10\plugins\`. |
| `echoreplay_core.dll` | The plugin without its diagnostic observer module, for the tests. Never install it. |
| `echodemo.exe` | Command-line tool for recordings (below). |
| test programs | Unit tests and self-tests. |

The DLL uses the static CRT (`/MT`), so nothing else needs installing next to it.

## Tests

```powershell
powershell -ExecutionPolicy Bypass -File tools\test.ps1
```

This builds nothing. It runs every test program and check in the build folder. None of them launch
the game. The unit tests use [doctest](https://github.com/doctest/doctest), so you can also run one
directly, for example `build\bin\test_import.exe`.

Some checks read `echovr.exe` to confirm that hook sites still match. They find the game the same way
`watch_replay` does: `REPLAY_ECHOVR_EXE` (path to `echovr.exe`), then `ECHOVR_DIR` (the game folder),
then the Oculus/Meta library folders. Without the game those checks print `SKIPPED:` and pass.

```powershell
$env:ECHOVR_DIR = '<game folder>'
```

The plugin targets one PC build of Echo VR (`echovr.exe` MD5 `caa05ee3bca2f5df0809994e93e2cc05`).
Hook sites are checked against it at startup; on another build the affected features switch off and
say so in the log.

## The `echodemo` tool

```
echodemo info    <file>                          duration, players, level
echodemo verify  <file> [--quiet] [--allow-gaps] check the file is intact and readable
echodemo convert <in.echoreplay|in.tape> <out.echodemo> [--session N] [--rate 45] [--report <json>]
echodemo dump    <file> [--chapter N] [--section N] [--stream N] [--hex]
echodemo rows    <file> --what player|body|event|netvar|gamestate|roster [--from s] [--to s]
echodemo voip    <file>                          one CSV row per recorded voice frame
echodemo strip   <in> <out> --voip [--asseen] [--oracle]   remove sections before sharing
echodemo fuzz    <file> [--seed N] [--cases N] [--truncate] [--bitflip]
```

- `--pseudonymise` replaces player names, platform strings and account ids with `playerNN` in any
  command's output.
- `convert` writes `<out>.import.json` next to the output: repairs, gaps, and rows that were inferred.
  Exit code 0 is success, 2 means the input was refused, 3 is a file error.
