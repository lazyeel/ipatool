#!/usr/bin/env python3
# Copyright 2026 lazyeel (https://github.com/lazyeel)
# SPDX-License-Identifier: Apache-2.0

"""Map FootHillPublic JNI wrappers to internal FairPlay workers in libstoreapi.so.
Filters JNI noise helpers (exception checks, logging) to expose the real worker calls."""
import struct, sys, re, os
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

# Default path is anchored to the repo root (parent of tools/),
# so the script works from any CWD. An explicit argv[1] is used as-is.
_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATH = sys.argv[1] if len(sys.argv) > 1 else os.path.join(_REPO_ROOT, 'libs-new', 'libstoreapi.so')
d = open(PATH,'rb').read()

e_shoff, = struct.unpack_from('<Q', d, 0x28)
sz, num, strndx = struct.unpack_from('<HHH', d, 0x3A)
shstr_off, = struct.unpack_from('<Q', d, e_shoff + strndx*sz + 0x18)

def shname(i):
    nameoff, = struct.unpack_from('<I', d, e_shoff + i*sz)
    b = d[shstr_off + nameoff:]
    return b[:b.find(b'\0')].decode()

secs = []
for i in range(num):
    o = e_shoff + i*sz
    (nameoff, typ, flags, addr, off, size, link,
     info, align, entsize) = struct.unpack_from('<IIQQQQIIQQ', d, o)
    secs.append(dict(idx=i, name=shname(i), typ=typ, addr=addr, off=off,
                     size=size, link=link))

text   = max((s for s in secs if s['typ'] == 1), key=lambda s: s['size'])
dynsym = next(s for s in secs if s['typ'] == 11)
dynstr = next(s for s in secs if s['idx'] == dynsym['link'])

def v2o(a): return a - text['addr'] + text['off']

wrappers = []
for i in range(dynsym['size']//24):
    o = dynsym['off'] + i*24
    nameo, info, oth, shn, val, siz = struct.unpack_from('<IBBHQQ', d, o)
    b = d[dynstr['off'] + nameo:]
    nm = b[:b.find(b'\0')].decode(errors='replace')
    if 'FootHillPublic' in nm and val:
        wrappers.append((nm.split('_00024Companion_')[-1], val))

# Known JNI-noise helpers: exception check/release, logging, env guards.
NOISE = {'0x5bb58','0x17d2c0','0x17d2d0','0x17937c','0x52674','0x529f0',
         '0x52534','0x52acc','0x53178','0x5324c'}

md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)

print(f"{'wrapper':52s} worker candidates")
for short, addr in sorted(wrappers):
    insns = list(md.disasm(d[v2o(addr):v2o(addr)+4000], addr))
    bls = []
    for ins in insns:
        if ins.mnemonic == 'ret':
            break
        if ins.mnemonic == 'bl':
            m = re.match(r'#(0x[0-9a-f]+)', ins.op_str)
            if m and m.group(1) not in NOISE:
                bls.append(m.group(1))
    uniq = list(dict.fromkeys(bls))
    print(f"{short:52s} {uniq}")
