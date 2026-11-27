#!/usr/bin/env python3
"""Steady workload for failover runs.  Holds one file open with a byte-range
lock and, every 10 ms, does mkdir + rmdir + one synchronous 4 KiB overwrite of
the locked file + one synchronous append of the loop number to a log file.
Prints every operation slower than 200 ms and every error, then a summary.
At the end it reads the log back and checks that every number is there once,
in order.  usage: workload.py <dir> <seconds> <tag>"""
import os, sys, time, fcntl, datetime
d, secs, tag = sys.argv[1], float(sys.argv[2]), sys.argv[3]
def now(): return datetime.datetime.now(datetime.timezone.utc).strftime("%H:%M:%S.%f")[:-3]
work = os.path.join(d, f"wl-{tag}"); os.makedirs(work, exist_ok=True)
fd = os.open(os.path.join(work, "held"), os.O_CREAT | os.O_RDWR | os.O_SYNC, 0o644)
fcntl.lockf(fd, fcntl.LOCK_EX, 4096, 0)
logp = os.path.join(work, "seq.log")
lf = os.open(logp, os.O_CREAT | os.O_WRONLY | os.O_TRUNC | os.O_APPEND | os.O_SYNC, 0o644)
blk = b"y" * 4096
errs, count, worst, done = 0, 0, {}, []
print(now(), "start", flush=True)
end = time.time() + secs; i = 0
while time.time() < end:
    i += 1; p = os.path.join(work, f"d{i}")
    for name, fn in (("mkdir", lambda: os.mkdir(p)), ("rmdir", lambda: os.rmdir(p)),
                     ("write", lambda: os.pwrite(fd, blk, 0)),
                     ("append", lambda: os.write(lf, b"%d\n" % i))):
        t = time.time(); ts = now()
        try: fn(); res = "ok"
        except OSError as e: res = f"ERR errno={e.errno} {e.strerror}"; errs += 1
        dt = time.time() - t; count += 1
        if name == "append" and res == "ok": done.append(i)
        if dt > worst.get(name, 0): worst[name] = dt
        if dt > 0.2 or res != "ok":
            print(ts, f"{name} #{i} {res} took={dt:.3f}s", flush=True)
    time.sleep(0.01)
os.close(fd); os.close(lf)
got = [int(x) for x in open(logp).read().split()]
print(now(), f"done ops={count} errors={errs} worst=" +
      " ".join(f"{k}={v:.3f}s" for k, v in worst.items()) +
      f" log={'ok' if got == done else 'MISMATCH'} lines={len(got)} expected={len(done)}", flush=True)
