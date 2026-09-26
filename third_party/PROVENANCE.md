# Vendored dependency provenance

Vendored on 2026-08-27. Nothing here is fetched at build time: the tree builds with no network access.

| package | pinned at | upstream |
|---|---|---|
| MinHook | `1e9ad1eb42db11bfcb65461f687c656612d1b555` = `v1.3.4-11-g1e9ad1e` | `https://github.com/TsudaKageyu/minhook`, that commit |
| zstd | 1.3.8 | `https://github.com/facebook/zstd`, tag `v1.3.8` (lib/ subset) |
| doctest | 2.4.11 | `https://github.com/doctest/doctest`, tag `v2.4.11` |
| miniz | 3.0.2 (release amalgamation) | `https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip`, sha256 `ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5` |
| yyjson | 0.10.0 | `https://github.com/ibireme/yyjson/archive/refs/tags/0.10.0.tar.gz` (`src/` + `LICENSE`), sha256 `0d901cb2c45c5586e3f3a4245e58c2252d6b24bf4b402723f6179523d389b165` |

## MinHook is eleven commits past v1.3.4

The vendored subset carries no version string (upstream's `CMakeLists.txt` never bumped the patch
version, and `MinHook.rc` is not vendored). The source checkout answers it:

```
$ git -C minhook-src describe --tags
v1.3.4-11-g1e9ad1e
```

Do not re-pin to the v1.3.4 release. The eleven commits include `07c7c41` (2025-11-02), which fixes
`JMP_REL` / `JMP_REL_SHORT` / `JCC_REL` operands being stored unsigned. Without it a trampoline with a
negative relative offset is wrong, and the plugin's fallback frame tick is a MinHook detour whose
trampoline must chain correctly.

## zstd

Only the minimal libzstd source set is vendored: no dictBuilder, no legacy decoders, no deprecated
zbuff. `ZSTD_MULTITHREAD` is off. `threading.c` is compiled and emits one symbol
(`g_ZSTD_threading_useles_symbol`, outside its guard); everything else in it is guarded.
`zstdmt_compress.c` is not compiled: it has no `ZSTD_MULTITHREAD` guard of its own, and every call
site in `zstd_compress.c` is behind the macro, so leaving it out changes nothing a consumer can see.

One deviation from a verbatim copy: `zstd_errors.h` is moved from `lib/common/` to `lib/`, where
upstream's `make install` puts it. It is byte-identical. Without it on the public include path a
consumer cannot call `ZSTD_getErrorCode()`, which the file-recovery path needs to tell
`checksum_wrong` / `corruption_detected` from `dstSize_tooSmall`. `tests/buildsys` checks the three
codes it depends on.

## doctest

`doctest/doctest_main.cpp` is first-party: two lines, the one translation unit that defines doctest's
`main`. It sits next to the header it configures and is left out of the hash list below, which covers
only copied-in files.

## miniz and yyjson

Used by the legacy `.echoreplay` / `.tape` converter; both MIT. They were fetched once from the
upstream release archives above (miniz's git tree is the split source, not the single-file
amalgamation) and are verbatim copies. The converter library `replay_import` links them, and the
plugin links `replay_import` for convert-on-open. miniz is built read-only (`MINIZ_NO_ARCHIVE_WRITING_APIS`,
`MINIZ_NO_STDIO`, `MINIZ_NO_TIME`, `MINIZ_NO_ZLIB_COMPATIBLE_NAMES`, all PUBLIC because `MINIZ_NO_TIME`
changes a struct layout); yyjson without its writer and utilities.

## Manifest

These are git blob ids, not hashes of the bytes on disk. With `core.autocrlf=true` git rewrites text
files to CRLF in the working tree while the stored blob stays LF, so an on-disk digest depends on the
checkout. A blob id is the same for everyone:

```
git ls-files -s -- third_party
```

Three files are first-party and left out: `PROVENANCE.md`, `CMakeLists.txt` and
`doctest/doctest_main.cpp`. `tests/provenance` checks every row below in both directions: a vendored
file with no row fails too.

```
5ae0eb1052c28da584bcf2fdcc6b7ef45c30952c  doctest/LICENSE.txt
5c754cde08a5461d53074b95a3eb0fed16f357d3  doctest/doctest.h
74dea27229c05b53b095aa22b9ee7ee9f549e414  minhook/LICENSE.txt
ae393508b8852dad600c22ebdfddd54ec133ace4  minhook/include/MinHook.h
940df650dc808deea4c27be378380ffde3cfcc6c  minhook/src/buffer.c
204d551bffb7f11760bc1e474aa835e55f4cbb00  minhook/src/buffer.h
eb6af9b8ac8f3d83ab370f807467f2733fb4210d  minhook/src/hde/hde32.c
1112450d87ae6772ad805f4f7fa07965c5bbbdb0  minhook/src/hde/hde32.h
ce773b1e25f95b312a4c05f3acf053d87ec6d20c  minhook/src/hde/hde64.c
ecbf4df958ac24646c0ba645ee4ba8340e8a74c8  minhook/src/hde/hde64.h
b6ff45a30fb56f0bbd9e398f4997b2d491e25ad3  miniz/LICENSE
8d0032f9e4fb858352a6de1c25bca83ec8d5f17e  miniz/miniz.c
9fcfffcc8cd7ac9e9d9af271cffdbc8d0a58a9ea  miniz/miniz.h
84d82a012311a72f5dfb1fc88554f7820fbdf216  minhook/src/hde/pstdint.h
7b3e12e32fdb7c362ca333c992e3c2e05556edb5  minhook/src/hde/table32.h
01d4541f97765a1fe14ad4a60f3705156abaf176  minhook/src/hde/table64.h
06f56961f75c2680e779de4d2ff0a45a21cd9c24  minhook/src/hook.c
cf8ce058b709547dadce7aa2f38ca9ba03595bfb  minhook/src/trampoline.c
f7efb36081e52aaed2040930e105bfbf1996c82a  minhook/src/trampoline.h
a793a802892567f17d464a831e2e531dc8833f55  zstd/LICENSE
d955bd677b5b22a0aa6b0864d41c8674eb2dd27b  zstd/lib/common/bitstream.h
7f561282ca35ed6c09528c822d0b58f7660e09cc  zstd/lib/common/compiler.h
5f0923fc9289ac448b3149e2efdea98e932f2ef0  zstd/lib/common/cpu.h
3ebdd1cb15a6288e871f361f5d3fbbd8835d1d90  zstd/lib/common/debug.c
b4fc89d49741d1769f8a22505b6b7262dc4966ad  zstd/lib/common/debug.h
b12944e1de93ad10a0349fd9075bb0258128697c  zstd/lib/common/entropy_common.c
7c1bb67a23f4095eecf7e0d410c286bd69323a85  zstd/lib/common/error_private.c
0d2fa7e34b01459d3f5afa858ae133cfdd9db144  zstd/lib/common/error_private.h
f72c519b259d78315f8ae0d432232bcf2484004b  zstd/lib/common/fse.h
72bbead5beea3d5e73e5b4eaaa689ee8b98533d1  zstd/lib/common/fse_decompress.c
6b572c448d9db6ba26440efd997e312557c428aa  zstd/lib/common/huf.h
5da248756ffd4585cf54acd80e099f53070f0151  zstd/lib/common/mem.h
7a829454328022ccab351661bd7d49102cdf4c09  zstd/lib/common/pool.c
458d37f13c3e805d0705871d6f762c6bf4a5263d  zstd/lib/common/pool.h
8be8c8da948a86fe9e7e0af9b6110fea277c1f2a  zstd/lib/common/threading.c
d806c89d01c9cc907f9c0aad946064c9fd82b1b4  zstd/lib/common/threading.h
532b8161929de3bff598729a78ab6cc9a665b3c1  zstd/lib/common/xxhash.c
9bad1f59f63a93308341a1348efa9fbcaddbaa7c  zstd/lib/common/xxhash.h
667f4a27fc695b46ceca6dbcdb293c30917dba0a  zstd/lib/common/zstd_common.c
edeb74b9c3a27fbe45cde4627222ed9ba393be02  zstd/lib/common/zstd_internal.h
60f357bbd24e13856169e126910d3de673c50df9  zstd/lib/compress/fse_compress.c
45b7babc1e23a9abf51d4f3588e15d8ee641df21  zstd/lib/compress/hist.c
8b389358dc1002a8bad427dcc3d9242a2fd7ff67  zstd/lib/compress/hist.h
f074f1e0a95de61ac85d7898c2c1229cccaf147c  zstd/lib/compress/huf_compress.c
c2c9d3bc553d667b973ef874511b7df7a78a5513  zstd/lib/compress/zstd_compress.c
29bca59859ea88cca1a1372ac231bc15e1b0532c  zstd/lib/compress/zstd_compress_internal.h
47faf6d641db507e585a055a5341b0592ae0c61c  zstd/lib/compress/zstd_double_fast.c
4fa31acfc0d6562191d6c6328e87fe2e47493425  zstd/lib/compress/zstd_double_fast.h
40ba0f73e65aa47447a318e9533c52532c6ba3d0  zstd/lib/compress/zstd_fast.c
b74a88c57c81a948a564b0cf4823911bc37ea8e3  zstd/lib/compress/zstd_fast.h
53f998a4374b1a2de72f3dd75be2f51f9d2cc48f  zstd/lib/compress/zstd_lazy.c
ef85a6df9c8f35fd3a98300337765870d114872b  zstd/lib/compress/zstd_lazy.h
58eb2ffe4db3e46babd58718cb6b296887843700  zstd/lib/compress/zstd_ldm.c
a47846128b2089cdaca2574f03b681b4a847399e  zstd/lib/compress/zstd_ldm.h
44de6e97fd201b2b01d28d012088b545de0c04f9  zstd/lib/compress/zstd_opt.c
094f7476650e7971c27f7c3fcf77b08ff18aa947  zstd/lib/compress/zstd_opt.h
2cbd6ffade38c4838b96b8e547fd04a030b9a7e4  zstd/lib/compress/zstdmt_compress.c
ee771681fa59a7e5b4c073e3b02c65e27a736742  zstd/lib/compress/zstdmt_compress.h
3f8bd2973207f4b6c1bdf13867a21a7a072c479d  zstd/lib/decompress/huf_decompress.c
2ad0440684cad92581e1c6646b8fc13cd9e1a46e  zstd/lib/decompress/zstd_ddict.c
0479d11bb032ddcbd9443ca84a16cf83c98cdeb1  zstd/lib/decompress/zstd_ddict.h
feef1ef67abd5ef36990d85150a430adee02edc6  zstd/lib/decompress/zstd_decompress.c
32baad9fbb5c48fef64f7960e4af041c27b60a72  zstd/lib/decompress/zstd_decompress_block.c
7e929604102319a338df136c923c45e16947c70b  zstd/lib/decompress/zstd_decompress_block.h
abd0030519d252a59dea4257dcb488ecba773e41  zstd/lib/decompress/zstd_decompress_internal.h
b18fc8a44bf9f1063d02a5a77e3ac58503be6999  zstd/lib/zstd.h
92a3433896c54f39d688fca61ecb5bdff34bef2b  zstd/lib/zstd_errors.h
a09bff355348932f9a1bd8c2e7417afb1ec0c943  yyjson/LICENSE
16ecbacf521ad8a0415863250ce669a097f4e9b9  yyjson/yyjson.c
88cec2b09e4a9abcb2dcb0757016503d79d0b5d6  yyjson/yyjson.h
```
