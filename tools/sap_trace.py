#!/usr/bin/env python3
# Copyright 2026 lazyeel (https://github.com/lazyeel)
# SPDX-License-Identifier: Apache-2.0

import struct, sys
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

PATH='./libs-classic/libstoreapi.so'
d=open(PATH,'rb').read()
e_shoff,=struct.unpack_from('<Q',d,0x28)
sz,num,strndx=struct.unpack_from('<HHH',d,0x3A)
secs=[]
for i in range(num):
    o=e_shoff+i*sz
    vals=struct.unpack_from('<IIQQQQIIQQ',d,o)
    (nameoff,typ,flags,addr,off,size,link,info,align,entsize)=vals
    secs.append(dict(addr=addr,off=off,size=size,typ=typ,link=link))
text=next((s for s in secs if s['typ']==5), {'addr':0,'off':0,'size':len(d)})
def v2o(a): return a-text['addr']+text['off']
dynsym=next(s for s in secs if s['typ']==11)
dynstr=secs[dynsym['link']]

want = sys.argv[1] if len(sys.argv)>1 else 'FairPlaySAPInit'
target=None
for i in range(dynsym['size']//24):
    o=dynsym['off']+i*24
    nameo,info,oth,shn,val,siz=struct.unpack_from('<IBBHQQ',d,o)
    b=d[dynstr['off']+nameo:]
    nm=b[:b.find(b'\0')].decode(errors='replace')
    if want in nm and val: target=val; break
print(f"=== {want} wrapper @ {hex(target)} ===")

md=Cs(CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN)
insns=list(md.disasm(d[v2o(target):v2o(target)+3000], target))
for ins in insns[:400]:
    mark=''
    if ins.mnemonic in ('bl','blr'): mark=' <<< CALL'
    print(f"  {hex(ins.address)}: {ins.mnemonic} {ins.op_str}{mark}")
