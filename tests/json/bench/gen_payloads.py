import json, random
random.seed(7)
words = "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron pi rho sigma tau".split()
def user(i):
    return {
        "id": 100000 + i,
        "uuid": "%08x-%04x-%04x-%04x-%012x" % tuple(random.getrandbits(b) for b in (32,16,16,16,48)),
        "name": " ".join(random.choice(words).capitalize() for _ in range(2)),
        "email": "user%d@example.com" % i,
        "active": random.random() < 0.7,
        "balance": round(random.uniform(-1000, 100000), 2),
        "score": random.random(),
        "created_at": "2026-%02d-%02dT%02d:%02d:%02dZ" % (random.randint(1,12), random.randint(1,28), random.randint(0,23), random.randint(0,59), random.randint(0,59)),
        "tags": random.sample(words, random.randint(0, 5)),
        "address": {"street": "%d %s St" % (random.randint(1, 9999), random.choice(words).capitalize()),
                    "city": random.choice(words).capitalize() + "ville", "zip": "%05d" % random.randint(0, 99999),
                    "geo": {"lat": random.uniform(-90, 90), "lng": random.uniform(-180, 180)}},
        "manager_id": None if random.random() < 0.5 else random.randint(1, 1000),
        "note": random.choice(["", "VIP élève", "line1\nline2", "quote \"x\"", "tab\there"]),
    }
doc = {"status": "ok", "page": 1, "total": 2600, "data": [user(i) for i in range(2600)]}
s = json.dumps(doc, ensure_ascii=False)
open("api.json", "w").write(s)
print(len(s.encode()))
small = json.dumps(user(1), ensure_ascii=False)
open("small.json", "w").write(small)
print(len(small.encode()))
