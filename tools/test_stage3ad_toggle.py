#!/usr/bin/env python3
# Portable byte-level assertions for Stage3AD's self-inverse runtime toggle.
stock_gldf=bytes.fromhex('840db82dd2000f85')
patch_gldf=bytes.fromhex('4584ed9090900f85')
mask=(0x00009042BD5589C1).to_bytes(8,'little')
def xor(a,b): return bytes(x^y for x,y in zip(a,b))
assert xor(stock_gldf,mask)==patch_gldf
assert xor(patch_gldf,mask)==stock_gldf
assert bytes([0x84^0xB5,0xC9])==bytes.fromhex('31c9')
assert bytes([0x31^0xB5,0xC9])==bytes.fromhex('84c9')
print('Stage3AD toggle assertions PASS')
print('gldf stock ->',xor(stock_gldf,mask).hex())
print('gldf patch ->',xor(patch_gldf,mask).hex())
