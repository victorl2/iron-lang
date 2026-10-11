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
