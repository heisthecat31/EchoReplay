# Third-party notices

EchoReplay itself is MIT-licensed (see [LICENSE](LICENSE)). It bundles the code and data below.
Each library's full licence text ships next to its source under `third_party/`.

## Vendored libraries

| Library | Version | Used for | Licence | Licence file |
|---|---|---|---|---|
| [MinHook](https://github.com/TsudaKageyu/minhook) | v1.3.4 + 11 commits (`1e9ad1e`) | function hooks in the plugin | BSD 2-Clause (includes Hacker Disassembler Engine, BSD) | `third_party/minhook/LICENSE.txt` |
| [zstd](https://github.com/facebook/zstd) | 1.3.8 (`lib/` subset) | `.echodemo` compression, `.tape` import | BSD 3-Clause (zstd is dual BSD/GPLv2; used under BSD) | `third_party/zstd/LICENSE` |
| [miniz](https://github.com/richgel999/miniz) | 3.0.2 | reading `.echoreplay` zip files | MIT | `third_party/miniz/LICENSE` |
| [yyjson](https://github.com/ibireme/yyjson) | 0.10.0 | parsing `.echoreplay` JSON frames | MIT | `third_party/yyjson/LICENSE` |
| [doctest](https://github.com/doctest/doctest) | 2.4.11 | unit tests only, not in the plugin | MIT | `third_party/doctest/LICENSE.txt` |

Where each copy came from, with hashes, is recorded in `third_party/PROVENANCE.md`.

## Test data

`tests/import/data/sample.echoreplay` and `tests/import/data/sample.tape` are copied from
[EchoTools/tape](https://github.com/EchoTools/tape) (`testdata/`, commit `32b7ba9d`). They are used
only by the import tests and are licensed as follows:

```
MIT License

Copyright (c) 2024 thesprockee

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

The recorded match inside those files contains player names from the upstream test data.

## Game data

EchoReplay contains no game binaries, audio, textures or other game assets. It does contain small
amounts of data derived from the game for compatibility: short byte signatures of `echovr.exe`
functions (at most 32 bytes each) used to find hook sites, structure offsets, and decoded network
message samples used as test fixtures. Echo VR is a trademark of its owners; this project is not
affiliated with or endorsed by them.
