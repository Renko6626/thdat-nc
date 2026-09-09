#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
cache_dir=${THDAT_NC_BUILD_CACHE:-"$project_dir/.cache/windows"}
toolchain_version=20260908
zstd_version=1.5.7
toolchain_archive="$cache_dir/llvm-mingw.tar.xz"
zstd_archive="$cache_dir/zstd.tar.gz"
toolchain_dir="$cache_dir/llvm-mingw-$toolchain_version-msvcrt"
zstd_dir="$cache_dir/zstd-$zstd_version"
output_dir="$project_dir/bin/windows-x86_64"

toolchain_url="https://github.com/mstorsjo/llvm-mingw/releases/download/$toolchain_version/llvm-mingw-$toolchain_version-msvcrt-ubuntu-22.04-x86_64.tar.xz"
toolchain_sha256=4d905bae713182f1a2b4d33875fe5aa544ce9fc04cc153acb47755a90ca62f16
zstd_url="https://github.com/facebook/zstd/releases/download/v$zstd_version/zstd-$zstd_version.tar.gz"
zstd_sha256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3

mkdir -p "$cache_dir" "$output_dir"

if [ ! -f "$toolchain_archive" ]; then
  curl -fL --retry 3 -o "$toolchain_archive" "$toolchain_url"
fi
printf '%s  %s\n' "$toolchain_sha256" "$toolchain_archive" | sha256sum -c -

if [ ! -f "$zstd_archive" ]; then
  curl -fL --retry 3 -o "$zstd_archive" "$zstd_url"
fi
printf '%s  %s\n' "$zstd_sha256" "$zstd_archive" | sha256sum -c -

if [ ! -x "$toolchain_dir/bin/x86_64-w64-mingw32-clang" ]; then
  mkdir -p "$toolchain_dir"
  tar -xf "$toolchain_archive" -C "$toolchain_dir" --strip-components=1
fi
if [ ! -f "$zstd_dir/lib/zstd.h" ]; then
  mkdir -p "$zstd_dir"
  tar -xf "$zstd_archive" -C "$zstd_dir" --strip-components=1
fi

compiler="$toolchain_dir/bin/x86_64-w64-mingw32-clang"
archiver="$toolchain_dir/bin/llvm-ar"
ranlib="$toolchain_dir/bin/llvm-ranlib"

make -C "$zstd_dir/lib" libzstd.a -j2 \
  CC="$compiler" AR="$archiver" RANLIB="$ranlib" \
  CFLAGS='-O2 -DNDEBUG'

"$compiler" \
  -std=c11 -O2 -DNDEBUG -Wall -Wextra -Werror -static -s \
  -I"$zstd_dir/lib" \
  "$project_dir/src/thdat-nc.c" "$zstd_dir/lib/libzstd.a" \
  -o "$output_dir/thdat-nc.exe"

sha256sum "$output_dir/thdat-nc.exe"

