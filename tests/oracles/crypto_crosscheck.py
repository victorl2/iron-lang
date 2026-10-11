#!/usr/bin/env python3
"""Cross-check the crypto and encoding modules against Python on random inputs.

Builds tests/oracles/crypto_crosscheck.iron, feeds it a few thousand random
cases (digests, HMAC, streaming hashers, HKDF, PBKDF2, scrypt, AES-256-GCM and
ChaCha20-Poly1305 seal/open including forgeries, password hash strings made by
Python, hex and base64) and compares every answer with hashlib, hmac, base64
and the `cryptography` package. The AEAD cases are skipped when `cryptography`
is not installed.

usage: crypto_crosscheck.py <iron-binary> [--cases N] [--seed S]
"""
import argparse
import base64
import hashlib
import hmac
import os
import random
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent

try:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM, ChaCha20Poly1305
    from cryptography.hazmat.primitives.kdf.hkdf import HKDF
    from cryptography.hazmat.primitives import hashes
    HAVE_CRYPTOGRAPHY = True
except ImportError:
    HAVE_CRYPTOGRAPHY = False

HASHES = ["sha1", "sha224", "sha256", "sha384", "sha512", "sha3_256", "sha3_512", "md5"]
SIZES = {"sha1": 20, "sha224": 28, "sha256": 32, "sha384": 48, "sha512": 64,
         "sha3_256": 32, "sha3_512": 64, "md5": 16}


def hkdf_py(alg, ikm, salt, info, length):
    # RFC 5869 directly on hmac, so the check needs no third-party package.
    size = SIZES[alg]
    prk = hmac.new(salt or b"\0" * size, ikm, alg).digest()
    okm, t, i = b"", b"", 1
    while len(okm) < length:
        t = hmac.new(prk, t + info + bytes([i]), alg).digest()
        okm += t
        i += 1
    return okm[:length]


def b64_nopad(b):
    return base64.b64encode(b).decode().rstrip("=")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("iron")
    ap.add_argument("--cases", type=int, default=3000)
    ap.add_argument("--seed", type=int, default=None)
    a = ap.parse_args()
    a.iron = str(Path(a.iron).resolve())
    seed = a.seed if a.seed is not None else random.SystemRandom().randrange(1 << 32)
    rng = random.Random(seed)

    def rb(lo, hi):
        return rng.randbytes(rng.randint(lo, hi))

    def size_skew():
        # Mostly small, sometimes block-boundary-ish or larger.
        return rng.choice([rng.randint(0, 3), rng.randint(0, 200), rng.choice([55, 56, 63, 64, 65, 111, 112, 127, 128, 129, 135, 136, 137]), rng.randint(0, 5000)])

    cases = []  # (line, expected)

    def add(fields, expected):
        cases.append(("|".join(fields), expected))

    for _ in range(a.cases):
        k = rng.randrange(100)
        alg = rng.choice(HASHES)
        if k < 22:
            data = rng.randbytes(size_skew())
            add([f"hash:{alg}", data.hex()], hashlib.new(alg, data).hexdigest())
        elif k < 36:
            key = rng.randbytes(rng.choice([0, 1, 16, 32, 64, 65, 128, 129, 200]))
            data = rng.randbytes(size_skew())
            add([f"hmac:{alg}", key.hex(), data.hex()], hmac.new(key, data, alg).hexdigest())
        elif k < 44:
            data = rng.randbytes(size_skew())
            cuts = [str(rng.randint(0, 80)) for _ in range(rng.randint(1, 6))]
            add([f"stream:{alg}", data.hex(), ",".join(cuts)], hashlib.new(alg, data).hexdigest())
        elif k < 48:
            key, data = rb(0, 150), rng.randbytes(size_skew())
            cuts = [str(rng.randint(0, 80)) for _ in range(rng.randint(1, 6))]
            add([f"hmacstream:{alg}", key.hex(), data.hex(), ",".join(cuts)], hmac.new(key, data, alg).hexdigest())
        elif k < 54:
            alg = rng.choice(HASHES[:7])
            ikm, salt, info = rb(1, 80), rb(0, 80), rb(0, 80)
            length = rng.randint(1, 255 * SIZES[alg])
            add([f"hkdf:{alg}", ikm.hex(), salt.hex(), info.hex(), str(length)], hkdf_py(alg, ikm, salt, info, length).hex())
        elif k < 59:
            alg = rng.choice(HASHES[:7])
            pw, salt = rb(0, 70), rb(0, 40)
            iters, length = rng.randint(1, 40), rng.randint(1, 150)
            add([f"pbkdf2:{alg}", pw.hex(), salt.hex(), str(iters), str(length)],
                hashlib.pbkdf2_hmac(alg, pw, salt, iters, length).hex())
        elif k < 61:
            pw, salt = rb(0, 40), rb(0, 40)
            log_n, r, p, length = rng.randint(1, 8), rng.randint(1, 8), rng.randint(1, 3), rng.randint(1, 80)
            add(["scrypt", pw.hex(), salt.hex(), str(log_n), str(r), str(p), str(length)],
                hashlib.scrypt(pw, salt=salt, n=1 << log_n, r=r, p=p, dklen=length).hex())
        elif k < 77 and HAVE_CRYPTOGRAPHY:
            which = rng.choice(["gcm", "chacha"])
            cls = AESGCM if which == "gcm" else ChaCha20Poly1305
            key, nonce = rng.randbytes(32), rng.randbytes(12)
            pt, aad = rng.randbytes(size_skew()), rb(0, 100)
            sealed = cls(key).encrypt(nonce, pt, aad)
            mode = rng.randrange(4)
            if mode == 0:
                add([f"seal:{which}", key.hex(), nonce.hex(), pt.hex(), aad.hex()], sealed.hex())
            elif mode == 1:
                add([f"open:{which}", key.hex(), nonce.hex(), sealed.hex(), aad.hex()], pt.hex())
            elif mode == 2:
                # A forgery: one flipped bit anywhere, or a changed aad.
                if rng.random() < 0.8:
                    i = rng.randrange(len(sealed))
                    forged = sealed[:i] + bytes([sealed[i] ^ (1 << rng.randrange(8))]) + sealed[i + 1:]
                    add([f"open:{which}", key.hex(), nonce.hex(), forged.hex(), aad.hex()], "ERR9004")
                else:
                    add([f"open:{which}", key.hex(), nonce.hex(), sealed.hex(), (aad + b"x").hex()], "ERR9004")
            else:
                add([f"decrypt:{which}", key.hex(), (nonce + sealed).hex(), aad.hex()], pt.hex())
        elif k < 82:
            # Password hash strings made here, verified by Iron.
            pw = rb(0, 30)
            salt = rb(8, 24)
            if rng.random() < 0.5:
                alg = rng.choice(["sha1", "sha256", "sha512"])
                it = rng.randint(1, 50)
                dk = hashlib.pbkdf2_hmac(alg, pw, salt, it, SIZES[alg])
                enc = f"$pbkdf2-{alg}$i={it}${b64_nopad(salt)}${b64_nopad(dk)}"
            else:
                log_n, r, p = rng.randint(1, 8), rng.randint(1, 8), rng.randint(1, 2)
                dk = hashlib.scrypt(pw, salt=salt, n=1 << log_n, r=r, p=p, dklen=rng.choice([16, 32, 64]))
                enc = f"$scrypt$ln={log_n},r={r},p={p}${b64_nopad(salt)}${b64_nopad(dk)}"
            right = rng.random() < 0.6
            guess = pw if right else pw + b"!"
            add(["verify", guess.hex(), enc.encode().hex()], "true" if right else "false")
        elif k < 85:
            x = rb(0, 64)
            y = x if rng.random() < 0.5 else (rb(0, 64) if rng.random() < 0.5 else x[:-1] + b"\x01" if x else b"\x01")
            add(["cteq", x.hex(), y.hex()], "true" if x == y else "false")
        elif k < 88:
            data, key = rng.randbytes(size_skew()), rb(0, 20)
            out = bytes(b ^ key[i % len(key)] for i, b in enumerate(data)) if key else data
            add(["xor", data.hex(), key.hex()], out.hex())
        else:
            data = rng.randbytes(size_skew())
            op = rng.choice(["hexenc", "b64enc", "b64urlenc", "hexdec", "b64dec", "b64urldec"])
            if op == "hexenc":
                add([op, data.hex()], data.hex().encode().hex())
            elif op == "b64enc":
                add([op, data.hex()], base64.b64encode(data).hex())
            elif op == "b64urlenc":
                add([op, data.hex()], base64.urlsafe_b64encode(data).rstrip(b"=").hex())
            elif op == "hexdec":
                text = data.hex().upper() if rng.random() < 0.5 else data.hex()
                add([op, text.encode().hex()], data.hex())
            elif op == "b64dec":
                add([op, base64.b64encode(data).hex()], data.hex())
            else:
                text = base64.urlsafe_b64encode(data)
                if rng.random() < 0.5:
                    text = text.rstrip(b"=")
                add([op, text.hex()], data.hex())

    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "crypto_crosscheck.iron"
        src.write_text((HERE / "crypto_crosscheck.iron").read_text())
        build = subprocess.run([a.iron, "build", str(src)], cwd=tmp, capture_output=True, text=True)
        if build.returncode != 0:
            print(build.stdout + build.stderr)
            print("crypto_crosscheck: build failed")
            return 1
        exe = Path(tmp) / ("crypto_crosscheck.exe" if os.name == "nt" else "crypto_crosscheck")
        inp = Path(tmp) / "cases.txt"
        inp.write_text("\n".join(line for line, _ in cases) + "\n")
        run = subprocess.run([str(exe), str(inp)], capture_output=True, text=True)
        got = run.stdout.splitlines()
        if run.returncode != 0 or len(got) != len(cases):
            print(run.stderr[-2000:])
            print(f"crypto_crosscheck: driver exited {run.returncode} after {len(got)} of {len(cases)} cases (seed {seed})")
            return 1
    bad = 0
    counts = {}
    for (line, want), have in zip(cases, got):
        op = line.split("|", 1)[0].split(":")[0]
        counts[op] = counts.get(op, 0) + 1
        if want != have:
            bad += 1
            if bad <= 10:
                print(f"MISMATCH {line[:200]}\n  python {want[:200]}\n  iron   {have[:200]}")
    summary = " ".join(f"{k}={v}" for k, v in sorted(counts.items()))
    note = "" if HAVE_CRYPTOGRAPHY else " (AEAD skipped: no cryptography package)"
    print(f"crypto_crosscheck: {len(cases) - bad}/{len(cases)} agree, seed {seed}{note}\n  {summary}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
