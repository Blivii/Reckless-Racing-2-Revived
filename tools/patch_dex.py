#!/usr/bin/env python3
"""Byte patches to Pixelbite's Java code, then fix the dex header checksums.
Usage: patch_dex.py classes.dex"""
import hashlib, sys, zlib

PATCHES = [
    # FuseEgl.EglCreate: config chooser demands red == 5 bits (RGB565) -> 8 (RGB888)
    ("13160500", "13160800"),  # const/16 v22, #5  ->  #8
    # FileCheck.expansionFilesDelivered: sget-object v0, AssetsList -> const/4 v5, #1; return v5
    # (the OBB is inside the APK, so skip the Java check that would start a dead Play download)
    ("6200960c2104", "12150f052104"),
    # IabHelper.startSetup: binds Play's billing service with an implicit intent, which throws on
    # Android 5+ for targetSdk >= 21 (crashing the store screen). if-nez -> goto/16, same target:
    # always take the existing "Billing service unavailable" path.
    ("7210fa1c01000a0139010b00", "7210fa1c01000a0129000b00"),
]

path = sys.argv[1]
d = bytearray(open(path, "rb").read())
for old, new in PATCHES:
    old, new = bytes.fromhex(old), bytes.fromhex(new)
    if d.count(old) != 1:
        sys.exit(f"patch_dex: expected exactly one {old.hex()}, found {d.count(old)}")
    i = d.index(old)
    d[i:i + len(new)] = new
d[12:32] = hashlib.sha1(d[32:]).digest()          # signature covers everything after it
d[8:12] = zlib.adler32(d[12:]).to_bytes(4, "little")  # checksum covers signature onward
open(path, "wb").write(d)
print(f"patch_dex: applied {len(PATCHES)} patch(es)")
