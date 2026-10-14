#!/usr/bin/env python3
"""gen_small.py - many small JSON documents, one per line, as an HTTP
service sees request and response bodies: user records, orders and events
of 150 bytes to 2 KB with ASCII and non-ASCII text, escapes, integers,
decimals, booleans, nulls and short nested arrays and objects. Every
document has "id", "email" and "active" members (bench small_select reads
them). Deterministic (fixed seed).

usage: gen_small.py <out.ndjson> [count]
"""
import json
import random
import sys

r = random.Random(20261011)
WORDS = ("alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron pi rho sigma tau "
         "upsilon phi chi psi omega").split()
NOTES = ["", "VIP \u00e9l\u00e8ve", "line1\nline2", "quote \"x\"", "tab\there", "\u65e5\u672c\u8a9e", "\U0001F600 ok",
         "path C:\\temp", "<b>html</b> & co"]


def base(i):
    return {"id": 100000 + i, "email": "user%d@example.com" % i, "active": r.random() < 0.7}


def user(i):
    d = base(i)
    d.update({
        "uuid": "%08x-%04x-%04x-%04x-%012x" % tuple(r.getrandbits(b) for b in (32, 16, 16, 16, 48)),
        "name": " ".join(r.choice(WORDS).capitalize() for _ in range(2)),
        "balance": round(r.uniform(-1000, 100000), 2), "score": r.random(),
        "created_at": "2026-%02d-%02dT%02d:%02d:%02dZ" % (r.randint(1, 12), r.randint(1, 28), r.randint(0, 23),
                                                       r.randint(0, 59), r.randint(0, 59)),
        "tags": r.sample(WORDS, r.randint(0, 5)),
        "address": {"street": "%d %s St" % (r.randint(1, 9999), r.choice(WORDS).capitalize()),
                    "city": r.choice(WORDS).capitalize() + "ville", "zip": "%05d" % r.randint(0, 99999),
                    "geo": {"lat": r.uniform(-90, 90), "lng": r.uniform(-180, 180)}},
        "manager_id": None if r.random() < 0.5 else r.randint(1, 1000), "note": r.choice(NOTES),
    })
    return d


def order(i):
    d = base(i)
    d.update({
        "order_id": "ORD-%010d" % r.randint(0, 10 ** 10), "currency": r.choice(["USD", "EUR", "BRL", "JPY"]),
        "items": [{"sku": "SKU%06d" % r.randint(0, 999999), "qty": r.randint(1, 9),
                   "price": round(r.uniform(0.5, 500), 2)} for _ in range(r.randint(1, 8))],
        "discount": None if r.random() < 0.6 else round(r.random() * 30, 1), "gift": r.random() < 0.1,
        "shipping": {"method": r.choice(["ground", "air", "pickup"]), "days": r.randint(1, 10)},
    })
    return d


def event(i):
    d = base(i)
    d.update({"type": r.choice(["click", "view", "purchase", "login"]), "ts": 1790000000000 + r.randint(0, 10 ** 9),
              "props": {w: r.choice([r.randint(-50, 50), r.random(), r.choice(WORDS), True, None])
                        for w in r.sample(WORDS, r.randint(0, 6))},
              "ua": "Mozilla/5.0 (X11; Linux x86_64) Iron/%d.%d" % (r.randint(1, 9), r.randint(0, 99))})
    return d


def main():
    out = sys.argv[1]
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 40000
    with open(out, "w", encoding="utf-8") as f:
        for i in range(count):
            doc = r.choice([user, order, event])(i)
            f.write(json.dumps(doc, ensure_ascii=False, separators=(",", ":")) + "\n")


if __name__ == "__main__":
    main()
