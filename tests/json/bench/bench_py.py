import json, sys, time
path, iters = sys.argv[1], int(sys.argv[2])
data = open(path, 'rb').read()
mb = len(data) / 1048576
bp = min((lambda: (lambda t0: (json.loads(data), time.perf_counter() - t0)[1])(time.perf_counter()))() for _ in range(iters))
doc = json.loads(data)
out = json.dumps(doc, ensure_ascii=False, separators=(',', ':'))
bs = min((lambda: (lambda t0: (json.dumps(doc, ensure_ascii=False, separators=(',', ':')), time.perf_counter() - t0)[1])(time.perf_counter()))() for _ in range(iters))
omb = len(out.encode()) / 1048576
print("%s parse %.2f ms %.0f MB/s | serialize %.2f ms %.0f MB/s" % (path.split('/')[-1], bp*1000, mb/bp, bs*1000, omb/bs))
