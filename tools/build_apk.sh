#!/usr/bin/env bash
# Build the 64-bit Zenonia APK from your own original APK.
#   tools/build_apk.sh ORIGINAL.apk OUTPUT.apk KEYSTORE.jks KEYSTORE_PASS
# Needs: python3 with capstone + pyelftools, Java, apktool.jar and uber-apk-signer.jar in $TOOLS,
# and the Android NDK in $NDK.
set -euo pipefail
ORIG=$(realpath "$1"); OUTAPK=$(realpath -m "$2"); KS=$(realpath "$3"); KSPASS=$4
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TOOLS=${TOOLS:-$ROOT/work/tools}
PY=${PY:-python3}
WORK=$ROOT/work
mkdir -p "$WORK"
rm -rf "$WORK/apk"
java -jar "$TOOLS/apktool.jar" d -f -o "$WORK/apk" "$ORIG"
$PY "$ROOT/recomp/recomp.py" "$WORK/apk/lib/armeabi/libmerkava.so" "$ROOT/generated"
for abi in arm64-v8a x86_64; do
  make -C "$ROOT/android" -j"$(nproc)" ABI=$abi GEN="$ROOT/generated"
done
$PY "$ROOT/tools/apkpatch/patch_app.py" "$WORK/apk"
rm -rf "$WORK/apk/lib/armeabi"
for abi in arm64-v8a x86_64; do
  mkdir -p "$WORK/apk/lib/$abi"
  cp "$ROOT/android/build/$abi/libmerkava.so" "$WORK/apk/lib/$abi/"
done
java -jar "$TOOLS/apktool.jar" b "$WORK/apk" -o "$WORK/unsigned.apk"
rm -rf "$WORK/signed"
java -jar "$TOOLS/uber-apk-signer.jar" -a "$WORK/unsigned.apk" --ks "$KS" --ksAlias zenonia \
  --ksPass "$KSPASS" --ksKeyPass "$KSPASS" -o "$WORK/signed"
cp "$WORK/signed/"*-aligned-signed.apk "$OUTAPK"
echo "built $OUTAPK"
