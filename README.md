# thdat-nc

`thdat-nc` 是针对 `PKGL` 归档的实验性命令行读取器，界面沿用 THTK `thdat` 的
`-l` / `-x` / `-c` 习惯。当前支持目录解密、逐条目 payload 解密、Zstandard 解压、
完整列表、全量或指定文件解包，以及从目录创建确定性的 PKGL 归档。

格式实现来自对研究者自有游戏副本的独立分析。仓库中的自动测试只生成原创的微型 PKGL
夹具，不包含游戏归档、纹理、脚本或游戏可执行文件字节。

## Windows 版

预编译的 64 位单文件版本位于：

```text
bin/windows-x86_64/thdat-nc.exe
```

Zstandard 1.5.7 已静态链接，不需要随程序复制 `zstd.dll`。

```bash
sha256sum -c SHA256SUMS
```

```powershell
# 查看目录
.\thdat-nc.exe -l C:\games\th06nc\data\th06ST.dat

# 解包全部文件
.\thdat-nc.exe -x -C C:\temp\th06ST C:\games\th06nc\data\th06ST.dat

# 只解包指定文件
.\thdat-nc.exe -x -C C:\temp\th06ST C:\games\th06nc\data\th06ST.dat ecldata1.ecl

# 将目录递归打包；归档内文件名使用正斜杠并按字节排序
.\thdat-nc.exe -c C:\temp\new.dat C:\temp\new

# 查看完整命令帮助
.\thdat-nc.exe -h
```

打包器只在 Zstandard 结果小于原文件时采用压缩，否则原样存储。输出采用 16 字节
payload 对齐，且相同归档文件名和相同输入内容会产生逐字节相同的结果。

## 打包 seed 的已知局限

原版工具生成每条目 32 位 seed 的算法目前未知。我们检查过的 1,783 个原版条目 seed
全部唯一，但它们不匹配已测试的文件名 CRC32、内容 CRC32、Adler32、XXH32、FNV-1a
或简单相邻 LCG 关系；相同内容在不同归档中也可拥有不同 seed。因此无法声称已经复现
官方 seed 生成器。

本工具用 `CRC32(归档内文件名 + NUL + 原始文件内容)` 生成确定性 seed。已知读取逻辑只把
该字段用作 payload XOR 密钥，并不把它当作校验和；由本工具进行的打包→解包回归已经
通过。但这个选择只是格式兼容方案，并非已知的官方算法，生成的归档也尚未宣称经过游戏
本体的全面兼容性验证。请保留原文件备份并在副本上测试。

## Linux 构建与测试

Linux 版运行时加载系统的 `libzstd.so.1`：

```bash
cmake -S . -B build
cmake --build build
python3 -m unittest tests/test_thdat_nc.py -v
```

## Windows 可复现构建

构建脚本下载固定版本并校验 SHA-256：LLVM-MinGW 20260908（MSVCRT）和 Zstandard
1.5.7。需要 `curl`、`tar`、`make` 和 `sha256sum`。

```bash
scripts/build-windows.sh
```

缓存默认写入 `.cache/windows/`，可通过 `THDAT_NC_BUILD_CACHE` 改到其他位置。

## 安全边界

- 只接受 `PKGL` magic，并检查目录记录边界和 payload 长度。
- 拒绝绝对路径、盘符和包含 `..` 路径段的归档条目。
- 归档文件名（不含扩展名）参与目录密钥计算；请保留原始 DAT 文件名。
- 解包会覆盖输出目录中同名文件；请使用单独的空目录。
- 这是基于单个已验证构建得到的研究工具，不代表所有同名游戏版本都使用相同格式。

## 第三方组件

Windows 二进制静态链接 Zstandard，其 BSD 许可证见 [`LICENSES/zstd.txt`](LICENSES/zstd.txt)。

---

# thdat-nc (English)

`thdat-nc` is an experimental command-line reader for `PKGL` archives. Its interface follows
the familiar THTK `thdat` conventions: `-l` lists entries, `-x` extracts them, and `-c` creates an
archive from a directory. It supports directory and payload encryption/decryption, Zstandard
compression/decompression, full or selected extraction, and deterministic PKGL creation.

The format implementation was independently derived from a researcher-owned game copy. The test
suite generates a tiny original PKGL fixture at runtime; this repository contains no game archive,
texture, script, or game-executable bytes.

## Windows release

The prebuilt, self-contained 64-bit executable is located at:

```text
bin/windows-x86_64/thdat-nc.exe
```

Zstandard 1.5.7 is linked statically, so no `zstd.dll` is required.

```bash
sha256sum -c SHA256SUMS
```

```powershell
# List entries
.\thdat-nc.exe -l C:\games\th06nc\data\th06ST.dat

# Extract everything
.\thdat-nc.exe -x -C C:\temp\th06ST C:\games\th06nc\data\th06ST.dat

# Extract one file
.\thdat-nc.exe -x -C C:\temp\th06ST C:\games\th06nc\data\th06ST.dat ecldata1.ecl

# Recursively pack a directory
.\thdat-nc.exe -c C:\temp\new.dat C:\temp\new

# Show complete command help
.\thdat-nc.exe -h
```

The creator uses Zstandard only when the compressed result is smaller than the original. Payloads
are 16-byte aligned. Identical input content and the same output archive basename produce a
byte-for-byte reproducible archive.

## Known packing-seed limitation

The algorithm used by the original tool to generate each entry's 32-bit seed is unknown. All 1,783
observed seeds were unique, but they matched none of the tested filename/content CRC32, Adler32,
XXH32, FNV-1a, or simple adjacent-LCG hypotheses. Identical content can also have different seeds
in different original archives. We therefore cannot claim to reproduce the official generator.

This packer uses `CRC32(archive entry name + NUL + original file content)` as a deterministic seed.
The known reader uses this field only as the payload XOR key, not as a checksum, and this tool's
pack-to-extract round-trip tests pass. This is a compatible engineering choice, not the known
official algorithm, and generated archives are not claimed to have comprehensive in-game
compatibility testing. Keep backups and test on copies.

## Linux build and tests

The Linux build loads the system `libzstd.so.1` at runtime:

```bash
cmake -S . -B build
cmake --build build
python3 -m unittest tests/test_thdat_nc.py -v
```

## Reproducible Windows build

The build script downloads pinned artifacts and verifies their SHA-256 hashes: LLVM-MinGW
20260908 (MSVCRT) and Zstandard 1.5.7. It requires `curl`, `tar`, `make`, and `sha256sum`.

```bash
scripts/build-windows.sh
```

Downloads and intermediate files go to `.cache/windows/` by default. Set
`THDAT_NC_BUILD_CACHE` to use another location.

## Safety and scope

- The reader requires the `PKGL` magic and validates directory-record and payload bounds.
- Absolute paths, drive-prefixed names, and archive entries containing a `..` component are rejected.
- The archive basename (without its extension) is part of the directory key; keep the original DAT name.
- Extraction overwrites files with the same name; use a dedicated empty output directory.
- This research tool is based on one verified build. Other releases with the same game title may use
  a different archive format.

## Third-party component

The Windows executable statically links Zstandard. Its BSD license is included at
[`LICENSES/zstd.txt`](LICENSES/zstd.txt).
