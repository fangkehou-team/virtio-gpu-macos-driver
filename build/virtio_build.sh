#!/bin/bash
# Build VirtIOGPU.kext (and later the sibling VM display kexts) for macOS.
# Needs the macOS SDK; no NVIDIA sources, no ogkm610 - the whole point.
# Usage: build/virtio_build.sh <outdir>   (on a Mac; xcrun required)
set -uo pipefail
OUT=${1:?usage: virtio_build.sh <outdir>}
SRC=$(cd "$(dirname "$0")/../kexts/VirtIOGPU" && pwd)
RE=$(cd "$(dirname "$0")/../kexts/NVRM/accel" && pwd)   # reconstructed OGKM headers (re/, iofam/) for VIOAccelProbe
mkdir -p "$OUT/VirtIOGPU.kext/Contents/MacOS"
SDK=$(xcrun --show-sdk-path); KHDR="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
CFLAGS=( -arch x86_64 -fapple-kext -mkernel -nostdinc -I"$KHDR" -I"$SRC" -I"$RE/re" -I"$RE/iofam"
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
  -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common
  -Wall -Wno-unused-parameter -Wno-unused-function -O2 )
xcrun clang++ "${CFLAGS[@]}" -c "$SRC/VirtIOGPU.cpp" -o "$OUT/VirtIOGPU.o" 2>"$OUT/gpu.err" \
  || { grep error "$OUT/gpu.err" | head -20; exit 1; }
xcrun clang++ "${CFLAGS[@]}" -c "$SRC/VIOAccelProbe.cpp" -o "$OUT/VIOAccelProbe.o" 2>"$OUT/probe.err" \
  || { grep error "$OUT/probe.err" | head -20; exit 1; }
xcrun clang++ "${CFLAGS[@]}" -DVIO_FWD -c "$SRC/VirtIOAccel.cpp" -o "$OUT/VirtIOAccel.o" 2>"$OUT/accel.err" \
  || { grep error "$OUT/accel.err" | head -20; exit 1; }
xcrun clang++ -arch x86_64 -fapple-kext -nostdlib -Xlinker -kext -lkmodc++ -lkmod -lcc_kext \
  "$OUT/VirtIOGPU.o" "$OUT/VIOAccelProbe.o" "$OUT/VirtIOAccel.o" -o "$OUT/VirtIOGPU.kext/Contents/MacOS/VirtIOGPU" 2>"$OUT/gpuld.err" \
  || { head -30 "$OUT/gpuld.err"; exit 1; }
cp "$SRC/Info.plist" "$OUT/VirtIOGPU.kext/Contents/Info.plist"
echo "VirtIOGPU $(md5 -q "$OUT/VirtIOGPU.kext/Contents/MacOS/VirtIOGPU") $(stat -f %z "$OUT/VirtIOGPU.kext/Contents/MacOS/VirtIOGPU") B  ($(xcrun clang++ --version | head -1))"
