#!/usr/bin/env bash
# Run in MSYS2 UCRT64 with GCC, make, SDL2, zlib, FLAC, libiconv, gettext and Git.
# Usage: build-mednafen.sh SOURCE_DIR BUILD_DIR JSON_INCLUDE_DIR [JOBS]
set -euo pipefail
export PATH="/ucrt64/bin:/usr/bin:$PATH"
source_dir="$(cd -- "$1" && pwd)"
mkdir -p -- "$2"
build_dir="$(cd -- "$2" && pwd)"
json_dir="$(cd -- "$3" && pwd)"
kit_dir="$(cd -- "$(dirname -- "$0")/../.." && pwd)"
adapter_dir="$kit_dir/components/xband/adapters/mednafen"
module_dir="$kit_dir/components/xband/include"
jobs="${4:-4}"
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'Invalid job count' >&2; exit 1; }
[[ -f "$json_dir/nlohmann/json.hpp" ]] || { echo 'Missing nlohmann/json.hpp' >&2; exit 1; }
git_bin="$(command -v git || true)"
if [[ -z "$git_bin" && -x '/c/Program Files/Git/cmd/git.exe' ]]; then git_bin='/c/Program Files/Git/cmd/git.exe'; fi
[[ -n "$git_bin" ]] || { echo 'Install Git or add it to PATH' >&2; exit 1; }
cable_enabled=0
if GIT_CEILING_DIRECTORIES="$(cygpath -w "$(dirname -- "$source_dir")")" "$git_bin" -C "$source_dir" apply --reverse --check "$kit_dir/integration/mednafen/mednafen-1.32.1-link-cable.patch" 2>/dev/null; then
  cable_enabled=1
else
  GIT_CEILING_DIRECTORIES="$(cygpath -w "$(dirname -- "$source_dir")")" "$git_bin" -C "$source_dir" apply --reverse --check "$kit_dir/integration/mednafen/mednafen-1.32.1-xband.patch"
fi
# Mednafen ships headers as src/, but expects <mednafen/...> include layout.
mkdir -p "$source_dir/include"
if [[ ! -e "$source_dir/include/mednafen" ]]; then
  ln -s ../src "$source_dir/include/mednafen"
fi
[[ -f "$source_dir/include/mednafen/mednafen.h" ]] || { echo 'Invalid include/mednafen mapping' >&2; exit 1; }
cd "$build_dir"
if [[ ! -f Makefile ]]; then
  "$source_dir/configure" --host=x86_64-w64-mingw32 \
    --disable-alsa --disable-jack --enable-threads=win32 \
    --enable-ss --disable-apple2 --disable-gb --disable-gba --disable-lynx \
    --disable-md --disable-nes --disable-ngp --disable-pce --disable-pce-fast \
    --disable-pcfx --disable-psx --disable-sasplay --disable-sms --disable-snes \
    --disable-snes-faust --disable-ssfplay --disable-vb --disable-wswan \
    --with-sdl-prefix=/ucrt64
fi
mkdir -p xband-runtime
g++ -std=c++20 -O2 -I"$module_dir" -I"$json_dir" -c "$adapter_dir/command_runtime.cpp" -o xband-runtime/command_runtime.o
g++ -std=c++20 -O2 -I"$module_dir" -I"$json_dir" -c "$adapter_dir/card_runtime.cpp" -o xband-runtime/card_runtime.o
link_flags='-no-pie -static-libstdc++ -static-libgcc -Wl,--defsym,mingw_app_type=__mingw_app_type'
libs="\"$build_dir/xband-runtime/command_runtime.o\" \"$build_dir/xband-runtime/card_runtime.o\" -mconsole -lws2_32 -lbcrypt -liphlpapi -ldxguid -lwinmm -ldinput -lole32 -ldsound"
cpp_flags="-DUNICODE=1 -D_UNICODE=1 -D_LFS64_LARGEFILE=1 -I\"$module_dir\" -I\"$adapter_dir\""
if [[ "$cable_enabled" == 1 ]]; then
  cable_dir="$kit_dir/components/link-cable"
  g++ -std=c++20 -O2 -I"$cable_dir" -c "$cable_dir/link_cable_service.cpp" -o xband-runtime/link_cable_service.o
  g++ -std=c++20 -O2 -DXBAND_MEDNAFEN_LINK_CABLE=1 -I"$cable_dir" -I"$adapter_dir" -c "$adapter_dir/cable_runtime.cpp" -o xband-runtime/cable_runtime.o
  libs="\"$build_dir/xband-runtime/link_cable_service.o\" \"$build_dir/xband-runtime/cable_runtime.o\" $libs"
  cpp_flags="$cpp_flags -DXBAND_MEDNAFEN_LINK_CABLE=1 -I\"$cable_dir\""
  printf '%s\n' 'Optional link-cable patch detected; SCI/YLS1 support enabled (launch opt-in).'
fi
make -j"$jobs" CPPFLAGS="$cpp_flags" LDFLAGS="$link_flags" LIBS="$libs"
# Separately compiled C ABI objects are not Automake prerequisites.
make -C src -W ss/libss.a mednafen.exe LDFLAGS="$link_flags" LIBS="$libs"
printf 'Built: %s/src/mednafen.exe\n' "$build_dir"
printf '%s\n' 'Launch in UCRT64 PATH, or provide SDL2/libiconv/zlib runtime DLLs alongside the executable.'
