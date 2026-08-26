#!/usr/bin/env bash
# Copyright 2026 lazyeel (https://github.com/lazyeel)
# SPDX-License-Identifier: Apache-2.0

# verify_termux.sh — full local verification of the ipatool research stack on
# Termux (Android arm64). Runs everything that does NOT require Apple
# credentials:
#   [0] environment sanity
#   [1] library fetch (both stacks)
#   [2] export-topology preflight gate (check_exports.py, stdlib only)
#   [3] research tools (fpmap.py / sap_trace.py, need capstone)
#   [4] compile adi_test + sap_test harnesses
#   [5] adi_test: anonymous ADI provisioning + OTP mint (network, no Apple ID)
#   [6] sap_test: FairPlay SAP/FPDI local calls (no network)
#   [7] full ipatool build (build_termux.sh)
#   [8] ipatool smoke test (--help exit codes)
#
# NOT covered (needs credentials, run by hand):
#   ./ipatool auth login -e EMAIL --sms ; purchase ; download
#
# Usage:  ./verify_termux.sh        (from the repo root)
# Exit:   0 iff every non-skipped section passed.

set -u
cd "$(dirname "$0")"

# Android/Termux: /tmp is usually inaccessible; mktemp honours $TMPDIR
# (Termux sets it to $PREFIX/tmp). Fall back to a dir under $HOME.
VT_TMP="$(mktemp -d 2>/dev/null || true)"
if [ -z "${VT_TMP:-}" ] || [ ! -d "$VT_TMP" ] || [ ! -w "$VT_TMP" ]; then
    VT_TMP="$HOME/.ipatool-verify-tmp"
    mkdir -p "$VT_TMP" || { echo "FATAL: no writable temp dir"; exit 1; }
fi
echo "work logs: $VT_TMP"

PASS=0; FAIL=0; SKIP=0
section() { printf '\n======== %s ========\n' "$1"; }
ok()   { printf '  [PASS] %s\n' "$1"; PASS=$((PASS+1)); }
bad()  { printf '  [FAIL] %s\n' "$1"; FAIL=$((FAIL+1)); }
skip() { printf '  [SKIP] %s\n' "$1"; SKIP=$((SKIP+1)); }

# ── [0] environment ───────────────────────────────────────────────────────
section "[0] environment"
ARCH=$(uname -m)
[ "$ARCH" = "aarch64" ] && ok "arch is aarch64" || bad "arch is $ARCH (need aarch64)"
[ -n "${PREFIX:-}" ] && ok "Termux PREFIX=$PREFIX" || bad "PREFIX unset — not running under Termux?"
command -v python3 >/dev/null && ok "python3: $(python3 --version 2>&1)" || bad "python3 missing"
command -v clang   >/dev/null && ok "clang present"   || bad "clang missing (pkg install clang)"
command -v cmake   >/dev/null && ok "cmake present"   || bad "cmake missing (pkg install cmake)"
command -v curl    >/dev/null && ok "curl present"    || bad "curl missing"

# ── [1] library fetch ─────────────────────────────────────────────────────
section "[1] library fetch (get_libs.sh)"
if ./get_libs.sh >$VT_TMP/vt_getlibs.log 2>&1; then
    ok "get_libs.sh completed"
else
    bad "get_libs.sh failed (tail below)"; tail -15 $VT_TMP/vt_getlibs.log | sed 's/^/      /'
fi
[ -f libs-classic/libstoreservicescore.so ] && ok "libs-classic/libstoreservicescore.so" || bad "missing classic entry lib"
[ -f libs-new/libstoreapi.so ]              && ok "libs-new/libstoreapi.so"              || bad "missing new-stack libstoreapi.so"
[ -f libs-new/libFPDIFor3P.so ]             && ok "libs-new/libFPDIFor3P.so"             || bad "missing libFPDIFor3P.so"

# ── [2] export-topology gate (stdlib only) ────────────────────────────────
section "[2] check_exports.py (preflight gate)"
if python3 tools/check_exports.py >$VT_TMP/vt_exports.log 2>&1; then
    grep -q "RESULT: PASS" $VT_TMP/vt_exports.log && ok "topology gate PASS (34 symbols + edge)" \
        || { bad "gate ran but no PASS line"; tail -5 $VT_TMP/vt_exports.log | sed 's/^/      /'; }
else
    bad "check_exports.py exited non-zero"; tail -10 $VT_TMP/vt_exports.log | sed 's/^/      /'
fi
# also verify the __file__-anchored default works from inside tools/
if ( cd tools && python3 check_exports.py >$VT_TMP/vt_exports2.log 2>&1 ) && grep -q "RESULT: PASS" $VT_TMP/vt_exports2.log; then
    ok "gate also passes when run from inside tools/"
else
    bad "gate failed when run from inside tools/ (path anchoring)"
fi

# ── [3] research tools (capstone) ─────────────────────────────────────────
section "[3] research tools (fpmap.py / sap_trace.py)"
if ! python3 -c "import capstone" >/dev/null 2>&1; then
    printf '  capstone missing — trying pip install...\n'
    python3 -m pip install --quiet capstone >$VT_TMP/vt_pip.log 2>&1 || true
fi
if python3 -c "import capstone" >/dev/null 2>&1; then
    ok "capstone importable"
    # fpmap: fresh output must match the Termux-captured reference table
    if python3 tools/fpmap.py >$VT_TMP/vt_fpmap.txt 2>&1; then
        if VT_TMP="$VT_TMP" python3 - >$VT_TMP/vt_cmp.log 2>&1 <<'PY'
import re, sys, os
def parse(p):
    d = {}
    for line in open(p):
        m = re.match(r'(\S+)\s+(\[.*\])', line.strip())
        if m: d[m.group(1)] = m.group(2)
    return d
cur = parse(os.path.join(os.environ['VT_TMP'], 'vt_fpmap.txt'))
ref = parse('tools/fpmap.txt')
sys.exit(0 if cur == ref and cur else 1)
PY
        then ok "fpmap.py output matches tools/fpmap.txt reference"
        else bad "fpmap.py output differs from reference"; cat $VT_TMP/vt_cmp.log | sed 's/^/      /'; fi
    else
        bad "fpmap.py failed"; tail -8 $VT_TMP/vt_fpmap.txt | sed 's/^/      /'
    fi
    # sap_trace: must disassemble a wrapper without error
    if python3 tools/sap_trace.py FairPlaySAPInit >$VT_TMP/vt_trace.txt 2>&1 \
        && grep -q "FairPlaySAPInit wrapper @" $VT_TMP/vt_trace.txt; then
        ok "sap_trace.py disassembles FairPlaySAPInit"
    else
        bad "sap_trace.py failed"; tail -8 $VT_TMP/vt_trace.txt | sed 's/^/      /'
    fi
else
    skip "capstone unavailable — fpmap.py / sap_trace.py not tested (pkg install capstone; pip install capstone)"
fi

# ── [4] compile harnesses ─────────────────────────────────────────────────
section "[4] compile adi_test + sap_test"
if clang adi_test.c -O2 -Wall -o adi_test -ldl -lcurl -lcrypto 2>$VT_TMP/vt_cc1.log; then
    ok "adi_test compiled clean (-Wall)"
else
    bad "adi_test compile failed"; tail -10 $VT_TMP/vt_cc1.log | sed 's/^/      /'
fi
if clang sap_test.c -O2 -Wall -o sap_test -ldl -lcurl -lcrypto 2>$VT_TMP/vt_cc2.log; then
    ok "sap_test compiled clean (-Wall)"
else
    bad "sap_test compile failed"; tail -10 $VT_TMP/vt_cc2.log | sed 's/^/      /'
fi

# ── [5] adi_test: anonymous provisioning + OTP ────────────────────────────
section "[5] adi_test (anonymous ADI provisioning + OTP mint)"
printf '  (needs network to gsa.apple.com; first run provisions ~10s)\n'
if [ -x ./adi_test ]; then
    if ./adi_test ./libs-classic >$VT_TMP/vt_adi.log 2>&1; then
        if grep -q "=== SUCCESS ===" $VT_TMP/vt_adi.log && grep -q "X-Apple-I-MD:" $VT_TMP/vt_adi.log; then
            ok "adi_test minted anisette tokens locally"
            grep "X-Apple-I-MD" $VT_TMP/vt_adi.log | sed 's/^/      /'
        else
            bad "adi_test exited 0 but no SUCCESS block"; tail -12 $VT_TMP/vt_adi.log | sed 's/^/      /'
        fi
    else
        bad "adi_test failed (rc=$?)"; tail -15 $VT_TMP/vt_adi.log | sed 's/^/      /'
    fi
else
    skip "adi_test not built — section [4] failed"
fi

# ── [6] sap_test: FairPlay SAP/FPDI local calls ───────────────────────────
section "[6] sap_test (FairPlay SAP/FPDI)"
printf '  (local only; FPDICreate is EXPECTED to error per STATUS.md — SAPInit should PASS)\n'
if [ -x ./sap_test ]; then
    if ./sap_test >$VT_TMP/vt_sap.log 2>&1; then
        # SAPInit returning 0 is the known-good signal; FPDICreate errors are expected
        if grep -qE "SAPInit => 0" $VT_TMP/vt_sap.log; then
            ok "sap_test ran; SAPInit => 0 (streaming subsystem OK)"
            grep -E "SAPInit =>|FPDICreate|PlatformInit" $VT_TMP/vt_sap.log | head -6 | sed 's/^/      /'
        else
            bad "sap_test ran but SAPInit did not return 0"; tail -20 $VT_TMP/vt_sap.log | sed 's/^/      /'
        fi
    else
        bad "sap_test crashed or exited non-zero (rc=$?)"; tail -20 $VT_TMP/vt_sap.log | sed 's/^/      /'
    fi
else
    skip "sap_test not built — section [4] failed"
fi

# ── [7] full ipatool build ────────────────────────────────────────────────
section "[7] ipatool build (build_termux.sh)"
printf '  (installs pkg deps, builds with cmake — the long step)\n'
if ./build_termux.sh >$VT_TMP/vt_build.log 2>&1; then
    [ -x ./ipatool ] && ok "ipatool built (./ipatool present)" \
        || bad "build_termux.sh exited 0 but ./ipatool missing"
else
    bad "build_termux.sh failed"; tail -20 $VT_TMP/vt_build.log | sed 's/^/      /'
fi

# ── [8] ipatool smoke test ────────────────────────────────────────────────
section "[8] ipatool smoke test"
if [ -x ./ipatool ]; then
    ./ipatool --help >/dev/null 2>&1; r1=$?
    ./ipatool        >/dev/null 2>&1; r2=$?
    ./ipatool frobnicate >/dev/null 2>&1; r3=$?
    [ "$r1" -eq 0 ] && ok "--help exits 0" || bad "--help exits $r1 (want 0)"
    [ "$r2" -eq 0 ] && ok "bare invocation exits 0" || bad "bare exits $r2 (want 0)"
    [ "$r3" -eq 1 ] && ok "unknown command exits 1" || bad "unknown cmd exits $r3 (want 1)"
else
    skip "ipatool not built — section [7] failed"
fi

# ── summary ───────────────────────────────────────────────────────────────
printf '\n======== SUMMARY ========\n'
printf '  PASS=%d  FAIL=%d  SKIP=%d\n' "$PASS" "$FAIL" "$SKIP"
printf '  Full logs in $VT_TMP/vt_*.log\n'
if [ "$FAIL" -eq 0 ]; then
    printf '  RESULT: ALL GREEN (skips are environment-limited, not code faults)\n'
    exit 0
else
    printf '  RESULT: %d FAILURE(S) — inspect the [FAIL] sections above\n' "$FAIL"
    exit 1
fi
