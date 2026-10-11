<!-- doctest-imports: crypto encoding -->
# Cryptography and encodings

Two standard library modules cover the cryptography a service needs:

- `import crypto` provides hashing (SHA-1, SHA-2, SHA-3, MD5), HMAC, HKDF,
  PBKDF2, scrypt, password hashing, constant-time comparison, secure random
  numbers, authenticated encryption (AES-256-GCM and ChaCha20-Poly1305) and
  RSA-OAEP. Every primitive is OpenSSL 3's libcrypto: nothing is implemented
  by hand, so the code is the same constant-time, CPU-accelerated code that
  TLS uses.
- `import encoding` provides hex and base64 (standard and URL-safe)
  encoders and strict decoders. It has no dependencies.

This guide is published at
[ironlang.dev/crypto](https://ironlang.dev/crypto/), generated from this
file; every Iron example in it is compiled and run in CI.

## Bytes are Strings

Iron has one type for bytes: `String`. A string holds arbitrary bytes,
including zero, so a digest, a key, a nonce or a ciphertext is a `String`,
exactly like the payloads of `IO.read_bytes` and `TcpSocket.read`. Equality
(`==`) and concatenation (`+`) compare and join bytes. Two things to keep in
mind with binary data:

- `byte_len()` is the size in bytes and `byte_slice(start, end)` cuts by
  byte. `len()`, `substring()` and `s[a..b]` count UTF-8 characters, which
  means nothing for binary data.
- String literals write any byte as `\xHH` (exactly two hex digits,
  `\x00` to `\xFF`) and the zero byte also as `\0`: `"\x89PNG\r\n\x1a\n"`
  is the 8-byte PNG signature. At run time, `Hex.decode`, `Base64.decode`
  and `String.from_byte(n)` build bytes too.

```iron
import crypto
import encoding

func main() {
    val digest = Crypto.sha256("hello")
    println("{digest.byte_len()} bytes: {Hex.encode(digest)}")
    println("first 4 bytes: {Hex.encode(digest.byte_slice(0, 4))}")
    val raw = Hex.decode("00ff10").data
    println("{raw.byte_len()} {raw.byte_at(1)} {raw == String.from_byte(0) + String.from_byte(255) + String.from_byte(16)}")
}
```

```output
32 bytes: 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
first 4 bytes: 2cf24dba
3 255 true
```

## Errors

Calls that can fail on their input return a result object, like the rest of
the standard library: `data` holds the bytes, `error` is 0 on success and
`error_message` says what went wrong. Decoders return a `DecodeResult`, the
crypto calls a `CryptoResult`. A forged or corrupted ciphertext is an
ordinary result with error 9004, never a panic. Calls that cannot fail on
valid input (hashing, HMAC, random bytes) return their value directly.

| Code | Meaning |
|---|---|
| 9000 | built without OpenSSL 3 (crypto unavailable) |
| 9001 | invalid argument (for example an algorithm a call does not accept) |
| 9002 | invalid key (wrong length, unparsable PEM) |
| 9003 | invalid nonce (not 12 bytes) |
| 9004 | authentication failed: the message, nonce, key or associated data do not match |
| 9005 | invalid output length |
| 9006 | invalid cost parameters (iterations, scrypt N, r, p) |
| 9007 | not a password hash string |
| 9008 | libcrypto reported a failure |
| 9100 | invalid character (encoding) |
| 9101 | invalid length (encoding) |
| 9102 | invalid padding (encoding) |

## Hex and base64

`Hex.encode` writes lowercase hex; `Hex.decode` accepts either case.
`Base64.encode` uses the standard alphabet with `=` padding and
`Base64.decode` requires the padding. `Base64.url_encode` uses the URL and
filename safe alphabet without padding (as JWTs do); `Base64.url_decode`
accepts it with or without padding. Decoders reject anything outside the
alphabet, including whitespace, and name the offending byte.

```iron
import encoding

func main() {
    println(Hex.encode("Iron"))
    println(Base64.encode("any carnal pleas"))
    println(Base64.url_encode(Hex.decode("fbff").data))
    val bad = Base64.decode("aGVs bG8=")
    println("{bad.error} {bad.error_message}")
    val ok = Base64.url_decode("aGVsbG8")
    println("{ok.error} {ok.data}")
}
```

```output
49726f6e
YW55IGNhcm5hbCBwbGVhcw==
-_8
9100 invalid base64 character at byte 4
0 hello
```

## Hashing

`Crypto.hash(algorithm, data)` returns the raw digest; `Crypto.sha256(data)`
and its siblings are shortcuts. `HashAlgorithm` has `Sha1`, `Sha224`,
`Sha256`, `Sha384`, `Sha512`, `Sha3_256`, `Sha3_512` and `Md5`. MD5 and
SHA-1 are broken for collision resistance: use them only where a protocol
requires them (MySQL native authentication, legacy checksums).

For data that arrives in pieces (a file read in chunks, a request body),
create a `Hasher`, `update` it with each piece and `finish` it. `finish`
returns the digest and starts the hasher over, so one hasher can digest
several messages. `Hasher.hmac(algorithm, key)` is the streaming HMAC. The
native state is freed when the hasher goes out of scope; `update` and
`finish` change it, so the binding is a `var`.

```iron
import crypto
import encoding

func main() {
    println(Hex.encode(Crypto.hash(HashAlgorithm.Sha3_256, "abc")))
    println("{Crypto.hash_size(HashAlgorithm.Sha512)} {Crypto.md5("").byte_len()}")
    var h = Hasher.new(HashAlgorithm.Sha256)
    h.update("hel")
    h.update("lo")
    val streamed = h.finish()
    println("{streamed == Crypto.sha256("hello")}")
}
```

```output
3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532
64 16
true
```

## HMAC and request signing

`Crypto.hmac(algorithm, key, data)` is HMAC (RFC 2104). Compare a MAC you
receive with the one you compute using `Crypto.constant_time_equal`, which
takes the same time wherever the inputs differ; `==` stops at the first
difference and leaks how much of a forged MAC was right.

```iron
import crypto
import encoding

func sign(secret: String, method: String, path: String, body: String) -> String {
    val canonical = "{method}\n{path}\n{Hex.encode(Crypto.sha256(body))}"
    return Hex.encode(Crypto.hmac(HashAlgorithm.Sha256, secret, canonical))
}

func main() {
    val secret = "shared-secret"
    val body = "\{\"qty\":1\}"
    val signature = sign(secret, "POST", "/v1/orders", body)
    println(signature)
    -- The receiver recomputes the signature and compares in constant time.
    val expected = sign(secret, "POST", "/v1/orders", body)
    val forged = sign(secret, "POST", "/v1/orders", "\{\"qty\":9\}")
    println("valid: {Crypto.constant_time_equal(signature, expected)}")
    println("tampered: {Crypto.constant_time_equal(signature, forged)}")
}
```

```output
0fc76f09bafd3b0970eb8428b522807fba13868ef94511d398bb8ca208114492
valid: true
tampered: false
```

## Key derivation

- `Crypto.hkdf(algorithm, ikm, salt, info, length)` (RFC 5869) turns key
  material that is already random, such as a shared secret, into keys for
  separate purposes. `salt` may be `""`; `length` is 1 to 255 times the
  hash size.
- `Crypto.pbkdf2(algorithm, password, salt, iterations, length)` (RFC 8018)
  and `Crypto.scrypt(password, salt, log_n, r, p, length)` (RFC 7914, with
  N = 2^log_n) stretch passwords. scrypt's working memory is 128 * r * N
  bytes and may not exceed 1 GiB.

```iron
import crypto
import encoding

func main() {
    val master = Hex.decode("000102030405060708090a0b0c0d0e0f").data
    val enc_key = Crypto.hkdf(HashAlgorithm.Sha256, master, "", "orders/encryption", 32)
    val mac_key = Crypto.hkdf(HashAlgorithm.Sha256, master, "", "orders/mac", 32)
    println("{enc_key.error} {enc_key.data.byte_len()} {enc_key.data != mac_key.data}")
    val dk = Crypto.pbkdf2(HashAlgorithm.Sha1, "password", "salt", 2, 20)
    println(Hex.encode(dk.data))
    val too_long = Crypto.hkdf(HashAlgorithm.Sha256, master, "", "", 9000)
    println("{too_long.error} {too_long.error_message}")
}
```

```output
0 32 true
ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957
9005 HKDF length must be 1 to 255 * the hash size
```

## Passwords

`Crypto.hash_password(password)` returns a string to store, in the PHC
string format, and `Crypto.verify_password(password, stored)` checks a
login against it in constant time:

```text
$scrypt$ln=15,r=8,p=3$<salt>$<hash>
```

The default is scrypt with N = 2^15, r = 8, p = 3: 32 MiB and about a
quarter of a second per hash, one of OWASP's recommended settings. The salt
is 16 random bytes and the hash 32 bytes, both in unpadded base64. Because
the parameters travel with the hash, `Crypto.password_needs_rehash(stored)`
tells you when a stored hash predates the current defaults, so you can
replace it right after a successful login.

`Crypto.hash_password_scrypt(password, log_n, r, p)` tunes the costs, and
`Crypto.hash_password_pbkdf2(password, algorithm, iterations)` produces
`$pbkdf2-sha256$i=600000$<salt>$<hash>` for deployments that require PBKDF2
(OWASP suggests 600000 iterations of SHA-256). `verify_password` accepts all
of them; a malformed string never matches.

```iron
import crypto

func main() {
    val stored = Crypto.hash_password_scrypt("hunter2", 12, 8, 1).data
    println(stored.substring(0, 21))
    println("{Crypto.verify_password("hunter2", stored)} {Crypto.verify_password("hunter3", stored)}")
    println("rehash: {Crypto.password_needs_rehash(stored)}")
    val legacy = Crypto.hash_password_pbkdf2("hunter2", HashAlgorithm.Sha256, 1000).data
    println("{legacy.substring(0, 22)} {Crypto.verify_password("hunter2", legacy)}")
    println("{Crypto.verify_password("hunter2", "not a hash")}")
}
```

```output
$scrypt$ln=12,r=8,p=1
true false
rehash: true
$pbkdf2-sha256$i=1000$ true
false
```

## Random numbers

`Crypto.random_bytes(n)`, `Crypto.random_int(low, high)` (uniform in
`[low, high)`, like `range`) and `Crypto.random_token(n)` (n random bytes as
URL-safe text, about 4/3 n characters) draw from OpenSSL's CSPRNG, seeded by
the operating system. Use them for keys, nonces, salts, session ids and API
keys. `Math.random` is not suitable for any of these.

```iron
import crypto

func main() {
    val key = Crypto.random_bytes(32)
    val die = Crypto.random_int(1, 7)
    val session = Crypto.random_token(32)
    println("{key.byte_len()} {die >= 1 and die <= 6} {session.byte_len()}")
}
```

```output
32 true 43
```

## Authenticated encryption

`AeadAlgorithm.Aes256Gcm` and `AeadAlgorithm.ChaCha20Poly1305` both take a
32-byte key and a 12-byte nonce and append a 16-byte tag. Associated data
(`aad`) is authenticated but not encrypted: put there what travels in the
clear but must not be altered, such as a record id or a protocol version.

The one rule: a nonce must never repeat under the same key.
`Crypto.encrypt(algorithm, key, plaintext, aad)` picks a random nonce and
returns `nonce || ciphertext || tag`; `Crypto.decrypt` takes that back.
Use `Crypto.seal` and `Crypto.open` with an explicit nonce only when a
protocol defines the nonce (a counter, a record sequence number).

Any change to the message, a wrong key, nonce or associated data gives
error 9004 and no plaintext.

```iron
import crypto

func main() {
    val key = Crypto.random_bytes(32)
    val boxed = Crypto.encrypt(AeadAlgorithm.Aes256Gcm, key, "card 4242", "order-17")
    println("{boxed.error} {boxed.data.byte_len()}")
    val opened = Crypto.decrypt(AeadAlgorithm.Aes256Gcm, key, boxed.data, "order-17")
    println("{opened.error} {opened.data}")
    val moved = Crypto.decrypt(AeadAlgorithm.Aes256Gcm, key, boxed.data, "order-18")
    println("{moved.error} {moved.error_message}")

    val nonce = Crypto.random_bytes(12)
    val sealed = Crypto.seal(AeadAlgorithm.ChaCha20Poly1305, key, nonce, "hi", "")
    println("{sealed.data.byte_len()} {Crypto.open(AeadAlgorithm.ChaCha20Poly1305, key, nonce, sealed.data, "").data}")
    println("{Crypto.seal(AeadAlgorithm.Aes256Gcm, "short key", nonce, "x", "").error}")
}
```

```output
0 37
0 card 4242
9004 authentication failed: the message, nonce, key or associated data do not match
18 hi
9002
```

## RSA-OAEP and MySQL authentication

`Crypto.rsa_encrypt_oaep(public_key_pem, algorithm, plaintext)` encrypts a
short message to an RSA public key in PEM form (`BEGIN PUBLIC KEY` or
`BEGIN RSA PUBLIC KEY`), with MGF1 on the same hash;
`Crypto.rsa_decrypt_oaep` takes an unencrypted PKCS#8 or PKCS#1 private
key. MySQL's `caching_sha2_password` uses it with SHA-1 to send a password
over a connection without TLS: the password, NUL terminated, XOR the
server's 20-byte nonce (`Crypto.xor` repeats the shorter key).

The scrambles of the two MySQL password plugins are a few lines of hashing:

```iron
import crypto
import encoding

-- mysql_native_password: SHA1(password) XOR SHA1(nonce + SHA1(SHA1(password)))
func native_scramble(password: String, nonce: String) -> String {
    val stage1 = Crypto.sha1(password)
    return Crypto.xor(stage1, Crypto.sha1(nonce + Crypto.sha1(stage1)))
}

-- caching_sha2_password: SHA256(password) XOR SHA256(SHA256(SHA256(password)) + nonce)
func sha2_scramble(password: String, nonce: String) -> String {
    val stage1 = Crypto.sha256(password)
    return Crypto.xor(stage1, Crypto.sha256(Crypto.sha256(stage1) + nonce))
}

func main() {
    val nonce = Hex.decode("1a2b3c4d5e6f708192a3b4c5d6e7f80910213243").data
    println(Hex.encode(native_scramble("secret", nonce)))
    println(Hex.encode(sha2_scramble("secret", nonce)))
}
```

```output
bdde5721c5517af3b51442ad156445d4095bbc1c
be770bf3d61960cea6e5602361cf95f00917b68662622dd40f497e759da12ed3
```

## Building and linking

Importing `crypto` links OpenSSL 3's libcrypto, as importing `http` does.
With the runtime bundles (Windows, macOS, `--target=linux-*`) it comes from
the bundle's static OpenSSL and nothing needs to be installed; a build
against the system compiler uses the OpenSSL that CMake found when the
compiler was built. A compiler built without OpenSSL still compiles crypto
programs: the fallible calls then return error 9000 and the others stop
the program with an explanation. `import encoding` needs nothing.

## API summary

| Call | Result |
|---|---|
| `Hex.encode(data)`, `Base64.encode(data)`, `Base64.url_encode(data)` | `String` |
| `Hex.decode(text)`, `Base64.decode(text)`, `Base64.url_decode(text)` | `DecodeResult` |
| `Crypto.hash(alg, data)`, `Crypto.sha1/sha224/sha256/sha384/sha512/sha3_256/sha3_512/md5(data)` | `String` (raw digest) |
| `Crypto.hash_size(alg)` | `Int` |
| `Crypto.hmac(alg, key, data)` | `String` |
| `Hasher.new(alg)`, `Hasher.hmac(alg, key)`; `h.update(data)`, `h.finish()` | `Hasher`; `String` |
| `Crypto.hkdf(alg, ikm, salt, info, length)` | `CryptoResult` |
| `Crypto.pbkdf2(alg, password, salt, iterations, length)` | `CryptoResult` |
| `Crypto.scrypt(password, salt, log_n, r, p, length)` | `CryptoResult` |
| `Crypto.hash_password(password)` | `String` |
| `Crypto.hash_password_scrypt(password, log_n, r, p)`, `Crypto.hash_password_pbkdf2(password, alg, iterations)` | `CryptoResult` |
| `Crypto.verify_password(password, stored)`, `Crypto.password_needs_rehash(stored)` | `Bool` |
| `Crypto.constant_time_equal(a, b)` | `Bool` |
| `Crypto.xor(data, key)` | `String` |
| `Crypto.random_bytes(n)`, `Crypto.random_token(n)` | `String` |
| `Crypto.random_int(low, high)` | `Int` |
| `Crypto.seal(alg, key, nonce, plaintext, aad)`, `Crypto.open(alg, key, nonce, sealed, aad)` | `CryptoResult` |
| `Crypto.encrypt(alg, key, plaintext, aad)`, `Crypto.decrypt(alg, key, message, aad)` | `CryptoResult` |
| `Crypto.aead_key_size(alg)`, `aead_nonce_size(alg)`, `aead_tag_size(alg)` | `Int` |
| `Crypto.rsa_encrypt_oaep(public_pem, alg, plaintext)`, `Crypto.rsa_decrypt_oaep(private_pem, alg, ciphertext)` | `CryptoResult` |
