import json, sys, time
data = open(sys.argv[1], 'rb').read(); n = int(sys.argv[2])
t0 = time.perf_counter(); s = 0
for _ in range(n): s += json.loads(data)["id"]
dt = time.perf_counter() - t0
print("python loads : %.0f ns/doc, %.0f MB/s" % (dt / n * 1e9, len(data) * n / 1048576 / dt))
u = {"id": 100001, "name": "Alpha Beta", "email": "user1@example.com", "active": True, "balance": 1234.5}
t0 = time.perf_counter(); b = 0
for i in range(n):
    u["id"] = 100001 + i
    b += len(json.dumps(u, separators=(',', ':')))
dt = time.perf_counter() - t0
print("python dumps : %.0f ns/doc, %.0f MB/s" % (dt / n * 1e9, b / 1048576 / dt))
