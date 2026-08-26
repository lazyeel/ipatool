#!/usr/bin/env python3
# Copyright 2026 lazyeel (https://github.com/lazyeel)
# SPDX-License-Identifier: Apache-2.0

"""Preflight gate for the FairPlay SAP/FPDI dlsym toolchain (sap_test.c).

Verifies the full export topology the research established, BEFORE any dlsym:
  * 20 SAP workers  -> defined in libstoreapi.so
  *  7 FPDI workers -> defined in libFPDIFor3P.so
  * dependency edge -> those 7 FPDI symbols are undefined imports in
                       libstoreapi.so (resolved at runtime by libFPDIFor3P.so)
  * libCoreFP.so    -> 7-export fingerprint proving the right 6.5.x stack
                       was extracted (sap_test.c dlopen's it in the load chain)

Exits non-zero with per-symbol diagnostics on any failure.
"""
import struct, sys, os

# Default library dir is anchored to the repo root (parent of tools/),
# so the script works from any CWD. An explicit argv[1] is used as-is.
_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIBDIR = sys.argv[1] if len(sys.argv) > 1 else os.path.join(_REPO_ROOT, 'libs-new')

# --- topology (verified against Apple Music 6.5.x arm64) ---
SAP_WORKERS = [  # defined in libstoreapi.so
    'N8jdR29h', 'CjHbHx', 'QHioSBsQR', 'bsawCXd', 'FKgu8fbnvGFG',
    'cp2g1b9ro', 'IPaI1oem5iL', 'Mib5yocT', 'Fc3vhtJDvr', 'XtCqEf5X',
    'df35957c4e0', 'jEHf8Xzsv8K', 'V3lNO', 'jr3lMuU8uaAR', 'fd3fa4R8',
    'PhUojZmspd', 'YMQGEcsGvUj', 'ha0dkchaters6', 'jfkdDAjba3jd', 'gLg1CWr7p',
]
FPDI_WORKERS = [  # defined in libFPDIFor3P.so, imported by libstoreapi.so
    'RhsJgiCAMX', 'jsf09djfs0df', 'RXm4IJLE3xR', 'd2234hmbdf',
    'g9000sds9', 'fsmklk123', 'sldksmfm1n',
]
COREFP_FINGERPRINT = [  # real exports of libCoreFP.so (both 2.9.0 & 6.5.x)
    'JNI_OnLoad', 'WIn9UJ86JKdV4dM', 'X46O5IeS', 'YlCJ3lg',
    'dku592fbFAj', 'fdjkDSAFjklaf2s', 'lxpgvVMLd0S7uRl',
]


def symtab(path):
    """Return {name: (bind, shn, val)} for the dynamic symbol table."""
    d = open(path, 'rb').read()
    e_shoff, = struct.unpack_from('<Q', d, 0x28)
    sz, num, strndx = struct.unpack_from('<HHH', d, 0x3A)
    secs = [struct.unpack_from('<IIQQQQIIQQ', d, e_shoff + i * sz) for i in range(num)]
    dynsym = next(s for s in secs if s[1] == 11)          # SHT_DYNSYM
    dynstr = secs[dynsym[6]]
    out = {}
    for j in range(dynsym[5] // 24):
        o = dynsym[4] + j * 24
        nameo, info, oth, shn, val, siz = struct.unpack_from('<IBBHQQ', d, o)
        if nameo == 0:
            continue
        b = d[dynstr[4] + nameo:]
        nm = b[:b.find(b'\0')].decode(errors='replace')
        out[nm] = (info >> 4, shn, val)
    return out


def is_defined(entry):
    """GLOBAL/WEAK and actually defined (shn != 0, val != 0)."""
    if entry is None:
        return False
    bind, shn, val = entry
    return bind in (1, 2) and shn != 0 and val != 0


def main():
    storeapi = os.path.join(LIBDIR, 'libstoreapi.so')
    fpdi = os.path.join(LIBDIR, 'libFPDIFor3P.so')
    corefp = os.path.join(LIBDIR, 'libCoreFP.so')

    for p in (storeapi, fpdi, corefp):
        if not os.path.exists(p):
            print(f"FATAL: missing {p} — run get_libs.sh first", file=sys.stderr)
            return 1

    st_store = symtab(storeapi)
    st_fpdi = symtab(fpdi)
    st_corefp = symtab(corefp)

    failures = []

    # 1. SAP workers defined in libstoreapi.so
    print(f"[1] SAP workers in libstoreapi.so ({len(SAP_WORKERS)} expected)")
    for n in SAP_WORKERS:
        if is_defined(st_store.get(n)):
            print(f"    OK   {n}")
        else:
            e = st_store.get(n)
            why = 'absent' if e is None else f'bind={e[0]} shn={e[1]} val={hex(e[2])}'
            print(f"    FAIL {n}  ({why})")
            failures.append(('SAP', n, why))

    # 2. FPDI workers defined in libFPDIFor3P.so
    print(f"[2] FPDI workers in libFPDIFor3P.so ({len(FPDI_WORKERS)} expected)")
    for n in FPDI_WORKERS:
        if is_defined(st_fpdi.get(n)):
            print(f"    OK   {n}")
        else:
            e = st_fpdi.get(n)
            why = 'absent' if e is None else f'bind={e[0]} shn={e[1]} val={hex(e[2])}'
            print(f"    FAIL {n}  ({why})")
            failures.append(('FPDI', n, why))

    # 3. dependency edge: FPDI workers present as imports in libstoreapi.so
    print(f"[3] dependency edge: FPDI workers imported by libstoreapi.so")
    for n in FPDI_WORKERS:
        e = st_store.get(n)
        if e is not None and e[1] == 0:  # present, undefined -> import
            print(f"    OK   {n}  (undefined import, resolved by libFPDIFor3P.so)")
        elif e is None:
            print(f"    FAIL {n}  (not referenced by libstoreapi.so — topology changed?)")
            failures.append(('EDGE', n, 'not an import of libstoreapi.so'))
        else:
            print(f"    WARN {n}  (defined in libstoreapi.so, edge not needed)")

    # 4. libCoreFP.so fingerprint (version sanity for the load chain)
    print(f"[4] libCoreFP.so fingerprint ({len(COREFP_FINGERPRINT)} exports)")
    defined_corefp = {n for n, (b, shn, val) in st_corefp.items() if b in (1, 2) and shn != 0}
    for n in COREFP_FINGERPRINT:
        if n in defined_corefp:
            print(f"    OK   {n}")
        else:
            print(f"    FAIL {n}  (missing — wrong stack version?)")
            failures.append(('COREFP', n, 'missing export'))

    print()
    if failures:
        print(f"RESULT: FAIL — {len(failures)} problem(s):")
        for kind, n, why in failures:
            print(f"    [{kind}] {n}: {why}")
        return 1
    total = len(SAP_WORKERS) + len(FPDI_WORKERS) + len(COREFP_FINGERPRINT)
    print(f"RESULT: PASS — all {total} symbols + dependency edge verified. "
          f"sap_test.c dlsym surface is intact.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
