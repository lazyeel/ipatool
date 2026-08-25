#!/usr/bin/env python3
# Copyright 2026 lazyeel (https://github.com/lazyeel)
# SPDX-License-Identifier: Apache-2.0

import struct
PATH='./libs-classic/libCoreFP.so'
d=open(PATH,'rb').read()
e_shoff, = struct.unpack_from('<Q', d, 0x28)
sz, num, strndx = struct.unpack_from('<HHH', d, 0x3A)
def shname(i):
    o = e_shoff + i*sz
    nameoff, = struct.unpack_from('<I', d, o)
    end = d.index(b'\0', e_shoff + strndx*sz + nameoff)
    return d[e_shoff + strndx*sz + nameoff:end].decode(errors='replace')
secs = []
for i in range(num):
    o = e_shoff + i*sz
    vals = struct.unpack_from('<IIQQQQIIQQ', d, o)
    secs.append(dict(idx=i,name=shname(i),typ=vals[1],addr=vals[3],off=vals[4],size=vals[5],link=vals[6]))
dynsym = next(s for s in secs if s['typ']==11)
dynstr = next(s for s in secs if s['idx']==dynsym['link'])
exports = []
for i in range(dynsym['size']//24):
    o = dynsym['off']+i*24
    nameo, info, oth, shn, val, siz = struct.unpack_from('<IBBHQQ', d, o)
    if nameo == 0 or val == 0: continue
    bind = info >> 4
    if bind != 1: continue
    b = d[dynstr['off']+nameo:]
    nm = b[:b.find(b'\0')].decode(errors='replace')
    exports.append(nm)

print(f"libCoreFP.so 6.5.x: {len(exports)} global defined exports")
needed = ['N8jdR29h','CjHbHx','QHioSBsQR','bsawCXd','FKgu8fbnvGFG',
          'cp2g1b9ro','IPaI1oem5iL','Mib5yocT','Fc3vhtJDvr','XtCqEf5X',
          'df35957c4e0','jEHf8Xzsv8K','V3lNO','jr3lMuU8uaAR','fd3fa4R8',
          'PhUojZmspd','YMQGEcsGvUj','ha0dkchaters6','jfkdDAjba3jd',
          'gLg1CWr7p','g9000sds9','d2234hmbdf','fsmklk123','sldksmfm1n',
          'RhsJgiCAMX']
found = set(exports) & set(needed)
missing = set(needed) - set(exports)
print(f"\nNeeded FairPlay exports: {len(needed)}")
print(f"Found: {len(found)}")
if missing:
    print(f"MISSING: {sorted(missing)}")
else:
    print("ALL PRESENT!")
for e in sorted(exports):
    print(f"  {e}")
