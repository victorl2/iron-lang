# Generates src/stdlib/iron_json_ryu.h (pipe the output through the header
# wrapper in that file). Ryu double tables: POW5_INV_SPLIT[q] = floor(2^(len(5^q)-1+125) / 5^q) + 1,
# POW5_SPLIT[i] = top 125 bits of 5^i. Each as two uint64 words (low, high).
POW5_INV_BITCOUNT = 125
POW5_BITCOUNT = 125
def words(v):
    return (v & ((1 << 64) - 1), v >> 64)
out = []
out.append("static const uint64_t json_pow5_inv_split[342][2] = {")
for q in range(342):
    p = 5 ** q
    j = p.bit_length() - 1 + POW5_INV_BITCOUNT
    v = (1 << j) // p + 1
    assert v.bit_length() <= 128
    lo, hi = words(v)
    out.append("    { 0x%016xULL, 0x%016xULL }," % (lo, hi))
out.append("};")
out.append("static const uint64_t json_pow5_split[326][2] = {")
for i in range(326):
    p = 5 ** i
    n = p.bit_length()
    v = p >> (n - POW5_BITCOUNT) if n > POW5_BITCOUNT else p << (POW5_BITCOUNT - n)
    lo, hi = words(v)
    out.append("    { 0x%016xULL, 0x%016xULL }," % (lo, hi))
out.append("};")
print("\n".join(out))

# Eisel-Lemire (fast_float) table for float parsing: for q in [-342, 308],
# a 128-bit approximation of 5^q with its top bit set, as {high, low}
# (truncated for q >= 0, rounded up for q < 0).
pow5_128 = []
for q in range(-342, 0):
    power5 = 5 ** -q
    z = 0
    while (1 << z) < power5:
        z += 1
    if q >= -27:
        b = z + 127
        c = 2 ** b // power5 + 1
    else:
        b = 2 * z + 2 * 64
        c = 2 ** b // power5 + 1
        while c >= (1 << 128):
            c //= 2
    pow5_128.append(c)
for q in range(0, 309):
    power5 = 5 ** q
    while power5 < (1 << 127):
        power5 *= 2
    while power5 >= (1 << 128):
        power5 //= 2
    pow5_128.append(power5)
lines = ["static const uint64_t json_pow5_128[%d][2] = {" % len(pow5_128)]
for c in pow5_128:
    assert (1 << 127) <= c < (1 << 128)
    lines.append("    { 0x%016xULL, 0x%016xULL }," % (c >> 64, c & ((1 << 64) - 1)))
lines.append("};")
print("\n".join(lines))
