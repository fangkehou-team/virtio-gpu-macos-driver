#!/bin/sh
# Atomically stage a freshly built VMTLDriver Metal plugin so WindowServer's
# dlopen(VMTLDriver.bundle) never observes a half-written, code-signature-invalid page.
#
# WHY: the plugin bundle is ad-hoc signed and WindowServer is hardened (CS_KILL).  Writing the
# new executable into Contents/MacOS/ in place (cp, or a `codesign` of the live bundle - both
# rewrite the Mach-O) leaves a window in which pages are torn and the embedded signature does
# not cover them; when WindowServer dlopens the bundle at MTLRegisterDevices, dyld's
# MachOFile::compatibleSlice faults there and the kernel SIGKILLs WindowServer with
# "SIGKILL (Code Signature Invalid)" / "CODESIGNING Invalid Page" - once per launchd respawn
# (~every 10 s) until the write finishes.  The fix: do all writing/signing on a TEMP bundle,
# verify it, then swap the directory into place.  WindowServer dlopens the bundle only once at
# its own init, so the sub-millisecond path-absent window during the swap is harmless.
#
# Signing the bare Mach-O and renaming just that file is NOT enough: a bundle's main
# executable must be signed as part of the bundle (the bare signature makes `codesign -v` on
# the bundle report "code has no resources but signature indicates they must be present").
#
# Usage (on the guest, as root):  sudo build/stage_plugin.sh [built-VMTLDriver]
#   e.g.  echo $VM_PW | ssh -p 2222 lony2003@127.0.0.1 'sudo -S ~/vmtl/stage.sh'
set -eu
[ "$(id -u)" = 0 ] || { echo "STOP: run with sudo"; exit 2; }
SRC=${1:-"$HOME/vmtl/out/VMTLDriver"}
[ -f "$SRC" ] || SRC=/Users/lony2003/vmtl/out/VMTLDriver
[ -f "$SRC" ] || { echo "STOP: no built plugin at $SRC"; exit 2; }
GB=/Library/GPUBundles
LIVE="$GB/VMTLDriver.bundle"
NEW="$GB/.VMTLDriver.bundle.new.$$"
OLD="$GB/.VMTLDriver.bundle.old.$$"
trap 'rm -rf "$NEW" "$OLD"' EXIT

ditto "$LIVE" "$NEW"                                   # full copy incl _CodeSignature, air-runtime
cp -f "$SRC" "$NEW/Contents/MacOS/VMTLDriver"
chown -R root:wheel "$NEW"; chmod -R a+rX "$NEW"
codesign -f -s - "$NEW" >/dev/null 2>&1 || { echo "STOP: bundle sign failed"; exit 3; }
codesign -v "$NEW" >/dev/null 2>&1 || { echo "STOP: temp bundle fails codesign"; exit 4; }

mv "$LIVE" "$OLD"                                      # swap: ms-scale path-absent window
mv "$NEW" "$LIVE"
rm -rf "$OLD"
trap - EXIT

codesign -v "$LIVE" >/dev/null 2>&1 || { echo "STOP: live bundle no longer validates"; exit 5; }
echo "staged $(shasum -a256 "$LIVE/Contents/MacOS/VMTLDriver" | cut -c1-12)  bundle OK  (WS pid $(pgrep -x WindowServer))"
echo "note: a running WindowServer keeps the OLD plugin; restart it (or reboot) to pick this up."
