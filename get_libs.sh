#!/usr/bin/env bash
# Copyright 2026 lazyeel (https://github.com/lazyeel)
# SPDX-License-Identifier: Apache-2.0

# Fetch the classic ADI engine libraries from Apple Music 2.9.0 for Android.
# These are Apple's proprietary binaries and are NOT stored in this repo.
# The script downloads them, extracts the needed set, and verifies each file
# carries the expected symbol markers before use.
set -e
cd "$(dirname "$0")"
mkdir -p libs-classic am_old

# androidapksfree hosts a direct APK; fall back to apkpure XAPK if needed
URL="https://assets.androidapksfree.net/s-static-1/sdata/bd0709756286b0b64b60f67569e437eb/com.apple.android.music_v2.9.0-812_Android-5.0.apk"

if [ ! -f am_old/am290.apk ]; then
    echo "[1/3] downloading com.apple.android.music 2.9.0..."
    curl -sL --max-time 240 -A "Mozilla/5.0 (Windows NT 10.0; Win64; x64)" \
        "$URL" -o am_old/am290.apk
fi

echo "[2/3] extracting libraries..."
python3 - << 'PY'
import zipfile, hashlib, sys

WANT = {
    "libstoreservicescore.so": "kq56gsgHG6",   # entry point marker
    "libCoreADI.so":           "cvu8io98wun",  # JNI bridge marker
    "libCoreFP.so":            None,
    "libCoreLSKD.so":          None,
    "libCoreFoundation.so":    None,
    "libc++_shared.so":        None,
    "libdispatch.so":          None,
    "libBlocksRuntime.so":     None,
    "libmediaplatform.so":     None,
    "libicudata_sv_apple.so":  None,
    "libicuuc_sv_apple.so":    None,
    "libicui18n_sv_apple.so":  None,
    "libxml2.so":              None,
}
z = zipfile.ZipFile('am_old/am290.apk')
import os
os.makedirs('libs-classic', exist_ok=True)
for lib, marker in WANT.items():
    name = f'lib/arm64-v8a/{lib}'
    try:
        data = z.read(name)
    except KeyError:
        print(f"  SKIP {lib} (absent)")
        continue
    with open(f'libs-classic/{lib}', 'wb') as f:
        f.write(data)
    sha = hashlib.sha256(data).hexdigest()[:16]
    ok = ""
    if marker:
        ok = " marker=OK" if marker.encode() in data else " MARKER MISSING!"
    print(f"  {lib}: {len(data)/1e6:.1f}MB sha256:{sha}...{ok}")
print("done.")
PY

echo "[3/3] verifying entry-point symbols..."
python3 - << 'PY'
data = open('libs-classic/libstoreservicescore.so','rb').read()
need = [b"kq56gsgHG6", b"Sph98paBcz", b"rsegvyrt87", b"qi864985u0"]
missing = [m.decode() for m in need if m not in data]
if missing:
    print("FATAL: missing obfuscated exports:", missing)
    sys.exit(1)
print("all classic ADI exports present.")
PY

echo ""
echo "Done. Libraries are in ./libs-classic/ (do NOT commit them)."
echo "Run: ./adi_test ./libs-classic"
