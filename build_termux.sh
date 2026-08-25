#!/bin/bash
# Copyright 2026 lazyeel (https://github.com/lazyeel)
# SPDX-License-Identifier: Apache-2.0

# Build ipatool with the native ADI anisette engine (Termux/Android).
set -e
cd "$(dirname "$0")"

echo "[1/3] Installing dependencies..."
pkg install -y clang cmake make openssl libcurl zlib nlohmann-json libminizip

if [ ! -d libs-classic ] || [ ! -f libs-classic/libstoreservicescore.so ]; then
    echo "[2/3] Fetching ADI engine libraries (not stored in the repo)..."
    ./get_libs.sh
else
    echo "[2/3] libs-classic/ already present, skipping fetch."
fi

echo "[3/3] Building..."
rm -rf build-adi
if ! cmake -B build-adi -DCMAKE_BUILD_TYPE=Release; then
    echo "CMAKE FAILED — full log above."
    exit 1
fi
if ! make -C build-adi -j"$(nproc)"; then
    echo "BUILD FAILED — errors above."
    exit 1
fi
cp build-adi/ipatool ./ipatool

cp apple_chain.pem . 2>/dev/null || true

echo ""
echo "=== Done: $(pwd)/ipatool ==="
cat << 'EOS'
Run from this directory so ./libs-classic is discoverable:

  ./get_libs.sh                                   # if not done yet
  ./adi_test ./libs-classic                       # verify the engine
  ./ipatool auth login -e EMAIL --sms             # SMS two-factor login
  ./ipatool purchase -b BUNDLE.ID --store-front 143441
  ./ipatool download -b BUNDLE.ID --store-front 143441 -o app.ipa
EOS
