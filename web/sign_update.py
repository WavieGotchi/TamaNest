#!/usr/bin/env python3
"""Sign a TamaNest firmware file for updates (Wi-Fi and SD card).

Appends a 128-byte trailer to the .bin: b"TNSIG001", 4-byte key id, 4-byte image length (little endian), the ECDSA
P-256 signature (r||s, 64 bytes) of SHA-256(image), then zero padding. The device (update_install in main/main.c)
refuses an update whose trailer is missing or whose signature does not match the public key in main/update_key.h.
The bootloader and the web installer ignore the trailer (it lies after the end of the app image).

The PRIVATE key never goes into the repo. Default location (the owner's Mac, not synced to any cloud):
    ~/Documents (No Cloud)/TamaNest signing key/tamanest-update-key.pem
or set TAMANEST_SIGN_KEY=/path/to/key.pem.

Usage: python sign_update.py firmware.bin [more.bin ...]      (re-signing replaces an existing trailer)
       python sign_update.py --check firmware.bin              (verify only)
"""
import hashlib, os, struct, sys
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature, encode_dss_signature

MAGIC = b"TNSIG001"
TRAILER = 128
KEY = os.environ.get("TAMANEST_SIGN_KEY",
                     os.path.expanduser("~/Documents (No Cloud)/TamaNest signing key/tamanest-update-key.pem"))


def split(data):
    """(image, trailer or None)"""
    if len(data) > TRAILER and data[-TRAILER:-TRAILER + 8] == MAGIC:
        n = struct.unpack("<I", data[-TRAILER + 12:-TRAILER + 16])[0]
        if n == len(data) - TRAILER:
            return data[:-TRAILER], data[-TRAILER:]
    return data, None


def key_id(pub):
    point = pub.public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
    return hashlib.sha256(point).digest()[:4]


def check(path, pub):
    image, tr = split(open(path, "rb").read())
    if tr is None:
        return "NOT SIGNED"
    if tr[8:12] != key_id(pub):
        return "signed with another key (id %s)" % tr[8:12].hex()
    r = int.from_bytes(tr[16:48], "big"); s = int.from_bytes(tr[48:80], "big")
    try:
        pub.verify(encode_dss_signature(r, s), image, ec.ECDSA(hashes.SHA256()))
        return "OK (key id %s, %d bytes)" % (tr[8:12].hex(), len(image))
    except Exception:
        return "BAD SIGNATURE"


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    if not os.path.exists(KEY):
        sys.exit("Signing key not found: %s\n(Set TAMANEST_SIGN_KEY, or restore the key from your backup.)" % KEY)
    priv = serialization.load_pem_private_key(open(KEY, "rb").read(), password=None)
    pub = priv.public_key()
    if args[0] == "--check":
        for p in args[1:]:
            print("%s: %s" % (p, check(p, pub)))
        return
    for p in args:
        image, _ = split(open(p, "rb").read())
        if image[:1] != b"\xE9":
            sys.exit("%s does not look like an ESP32 app image" % p)
        r, s = decode_dss_signature(priv.sign(image, ec.ECDSA(hashes.SHA256())))
        tr = MAGIC + key_id(pub) + struct.pack("<I", len(image)) + r.to_bytes(32, "big") + s.to_bytes(32, "big")
        tr += b"\0" * (TRAILER - len(tr))
        open(p, "wb").write(image + tr)
        print("%s: signed (key id %s, %d bytes + %d)" % (p, key_id(pub).hex(), len(image), TRAILER))


if __name__ == "__main__":
    main()
