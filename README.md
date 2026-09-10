# thdat-nc

Quick-and-dirty unpacker for TH06NC's `data/*.dat`. The game dropped ZUN's old
`PBG3` container for a new one (`PKGL`, zstd inside), so `thdat` from THTK just
shrugs at it. I wanted the ECL/ANM files out *now*, so here we are. Same
`-l` / `-x` / `-c` flags as `thdat`, because muscle memory.

> ⚠️ **Fair warning: this is vibe-coded slop.** First pass came out of
> GPT-5.6-Sol, then Claude Fable 5.1 tidied it up while I added comments. It works
> and has tests (also machine-written, obviously), but treat it as a prototype.
> If THTK ever ships proper support, use that instead. The only write-up of the
> format so far is the header comment in `src/pkgl.h`; nothing in `engine/` yet.

Everything here was worked out from my own copy of the game. The tests cook up
their own tiny fixture, so no game bytes live in this directory.

## Usage

Grab `bin/windows-x86_64/thdat-nc.exe` (64-bit, zstd baked in, no DLL). If you're
paranoid, `sha256sum -c SHA256SUMS`.

```powershell
thdat-nc.exe -l th06ST.dat                      # list
thdat-nc.exe -x -C out th06ST.dat               # extract all
thdat-nc.exe -x -C out th06ST.dat ecldata1.ecl  # extract one
thdat-nc.exe -c new.dat new\                    # pack a directory (recursive)
thdat-nc.exe -h
```

Packing sorts entries by name, aligns payloads to 16 bytes, and only keeps the
zstd version if it's actually smaller. Same input + same archive name = same
bytes out, every time.

## Why not just `thdat -6`?

Because the two formats have nothing in common but the extension. TH06's `PBG3`
is a bit-packed entry table with LZSS and no encryption to speak of. `PKGL` is
a flat 32-byte record per entry, Zstandard instead of LZSS, and two layers of
XOR: one key from the archive's own name, one from a per-entry seed. There's no
version flag to flip in `thdat`; it had to be a new reader. Details in
`src/pkgl.h`.

## Things to know before you repack

- **Keep the original file name.** The basename (minus `.dat`) feeds the
  directory key. Rename `th06ST.dat` and the game can't read the index.
- **The per-entry seed is STILL UNKNOWN.** I dumped 1,783 seeds from the retail
  archives: all unique, and none of them match CRC32/Adler32/XXH32/FNV-1a of the
  name or the content, or any simple LCG chain between neighbours. Same file in
  two archives gets two different seeds. So the official generator is still a
  mystery, and rather than dress up a fake one, every entry we pack just gets
  the constant `"poo"` (`0x006f6f70`; you'll see it in a hex editor). The game
  only ever uses the field as an XOR key and never checks it, so the archives
  load fine, but I haven't played through everything with a repacked dat. Keep
  a backup.
- Extraction overwrites without asking. Point it at an empty directory.
- Entries with absolute paths, drive letters or `..` get refused.
- All of this is from one build of the game. If ZUN patches it, re-check.

## Code map

If you only care about the format, read `src/pkgl.h` (layout) and
`src/pkgl.c` (`key_from_seed` for the XOR key; `PKGL_PLACEHOLDER_SEED` in the
header is the part we made up). The packing recipe is the header comment of
`src/pack.c`.
Everything else is plumbing.

```
src/pkgl.h/.c      the format: layout, key schedule, crc32, record (un)packing
src/zstd_api.h/.c  zstd behind one function table (static on Windows, dlopen on Linux)
src/platform.h/.c  seek/size/mkdir/readdir shims
src/unpack.h/.c    open an archive, list / read / extract entries
src/pack.h/.c      walk a directory and pack it
src/common.h/.c    error printing, byte helpers, tiny string/array helpers
src/main.c         the CLI
```

## Building

Linux (dlopens the system `libzstd.so.1`, so no zstd headers needed):

```bash
cmake -S . -B build && cmake --build build
python3 -m unittest tests/test_thdat_nc.py -v
```

The Windows exe is cross-built from Linux with a pinned LLVM-MinGW 20260908 and
zstd 1.5.7, hashes checked, so anyone can reproduce it. Needs `curl`, `tar`,
`make`, `sha256sum`:

```bash
scripts/build-windows.sh     # downloads go to .cache/windows/ (or $THDAT_NC_BUILD_CACHE)
```

## License

The Windows binary statically links Zstandard (BSD): [`LICENSES/zstd.txt`](LICENSES/zstd.txt).
