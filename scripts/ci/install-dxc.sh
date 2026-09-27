#!/usr/bin/env bash

set -euo pipefail

# Fetch the pinned DirectX Shader Compiler and drop dxc.exe where the Windows
# build can find it.
#
# dxc is the last step of the DXIL path: spirv-cross turns SPIR-V into SM6.0
# HLSL, dxc compiles that to DXIL, and DXIL is the only shader format the
# Windows build advertises (LIB386/SDL3GPU/CMakeLists.txt). MSYS2 has no dxc
# package and SDL_shadercross publishes no releases, so the binary comes from
# Microsoft's release zip. The pin lives here: docs/TOOLING.md points at this
# file rather than restating the version, and the SHA-256 is the same value
# SDL_shadercross records for this asset.
#
# Usage: install-dxc.sh [DEST_DIR]
#
#   DEST_DIR defaults to $MSYSTEM_PREFIX/bin, which is on PATH inside the
#   UCRT64 shell and is where CMake's find_program looks. The three files have
#   to land in one directory: dxc.exe loads dxcompiler.dll beside itself.

DXC_URL="https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2407/dxc_2024_07_31.zip"
DXC_SHA256="e2627f004f0f9424d8c71ea1314d04f38c5a5096884ae9217f1f18bd320267b5"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) ;;
    *)
        echo "install-dxc.sh: the pinned zip holds Windows binaries; run this under MSYS2." >&2
        exit 1
        ;;
esac

if [ "$#" -gt 1 ]; then
    echo "usage: $0 [DEST_DIR]" >&2
    exit 1
fi

if [ "$#" -eq 1 ]; then
    dest="$1"
elif [ -n "${MSYSTEM_PREFIX:-}" ]; then
    dest="${MSYSTEM_PREFIX}/bin"
else
    echo "install-dxc.sh: MSYSTEM_PREFIX is unset; pass DEST_DIR or run inside an MSYS2 environment." >&2
    exit 1
fi

if ! command -v curl >/dev/null 2>&1 || ! command -v unzip >/dev/null 2>&1; then
    echo "install-dxc.sh: needs curl and unzip (pacman -S curl unzip)." >&2
    exit 1
fi

work="$(mktemp -d)"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT

# Bounded as well as retried: a mirror that stalls returns no error for the
# retry count to catch, so the download would sit there until the job timeout.
curl -fsSL \
    --retry 5 --retry-delay 3 --retry-all-errors \
    --retry-max-time 300 --connect-timeout 15 --max-time 300 \
    -o "$work/dxc.zip" "$DXC_URL"

printf '%s  %s\n' "$DXC_SHA256" "$work/dxc.zip" | sha256sum -c -

unzip -q "$work/dxc.zip" -d "$work/extract"

dxc_dir="$(dirname "$(find "$work/extract" -type f -path '*/bin/x64/dxc.exe' -print -quit)")"
if [ ! -f "$dxc_dir/dxc.exe" ]; then
    echo "install-dxc.sh: no bin/x64/dxc.exe in the archive" >&2
    exit 1
fi

mkdir -p "$dest"
cp "$dxc_dir/dxc.exe" "$dxc_dir/dxcompiler.dll" "$dxc_dir/dxil.dll" "$dest/"

echo "install-dxc: dxc.exe, dxcompiler.dll, dxil.dll -> $dest"
