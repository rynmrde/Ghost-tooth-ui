#!/usr/bin/env python3
"""tools/gt-sim.py - a stand-in for ghost-toothAPI, for development only.

The real payload is a closed ELF that talks to a USB Bluetooth dongle, so this
script reproduces only the parts the UI can observe, using the exact file
formats the payload uses (verified against ghost-toothAPI.elf):

  * /data/ghost-toothAPI/ghost-toothAPI.lock - exclusive flock while running
  * /data/ghost-toothAPI/ghost-toothAPI.log   - "HH:MM:SS <message>" lines,
                                                opened in append mode, flushed
                                                per line
  * /data/ghost-toothAPI/headset.ini          - "name=.." / "address=.." pick
  * /data/ghost-toothAPI/bond.key, peer.name  - pairing state

Message texts are the payload's own, so the UI's parser is exercised by real
strings rather than invented ones.  Nothing here is part of the console build.

  python3 tools/gt-sim.py --root=/path/to/fake/data/ghost-toothAPI
"""

import argparse
import errno
import fcntl
import os
import re
import signal
import sys
import time

SIM_VERSION = "0.9.4-sim"

# (address, name, class-of-device, rssi, score) - a room of mixed devices,
# including the two kinds the payload is supposed to skip.
DEVICES = [
    ("30:3A:64:11:22:33", "WH-1000XM4",        "5a020c", -47, 132),
    ("A4:83:E7:44:55:66", "QC45",              "5a020c", -58, 118),
    ("6C:B3:11:77:88:99", "Galaxy Buds3 Pro",  "5a020c", -63, 96),
    ("94:DB:56:AA:BB:CC", "Soundcore Life Q30", "5a020c", -71, 74),
    ("C4:62:6B:DD:EE:FF", "BRAVIA 4K 2023",    "060104", -55, -1),
    ("F8:E0:79:01:02:03", "QLED TV",           "060104", -74, -1),
    ("11:22:33:44:55:66", "arctis nova 5",     "5a020c", -80, 51),
]

NEVER_ANSWER = "02:00:00:00:00:00"

INI_RE = re.compile(r"^\s*(name|address)\s*=\s*(.*?)\s*$")


class Log:
    def __init__(self, path):
        self.f = open(path, "ab", buffering=0)

    def line(self, msg):
        stamp = time.strftime("%H:%M:%S", time.gmtime())
        self.f.write(("%s %s\n" % (stamp, msg)).encode())

    def close(self):
        try:
            self.f.close()
        except Exception:
            pass


def read_ini(path):
    mode, value = "auto", ""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for raw in f:
                line = raw.strip()
                if line.startswith(";") or line.startswith("#"):
                    continue
                m = INI_RE.match(line)
                if not m:
                    continue
                key, val = m.group(1).lower(), m.group(2)
                if key == "address" and valid_addr(val):
                    mode, value = "address", val.upper()
                elif key == "name" and val:
                    mode, value = "name", val
    except OSError as e:
        if e.errno != errno.ENOENT:
            raise
    return mode, value


def valid_addr(addr):
    return bool(re.fullmatch(r"[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}", addr))


class Sim:
    def __init__(self, root):
        self.root = root
        self.stop = False
        self.log = None
        self.lock_fd = None

    # -- lifecycle ------------------------------------------------------
    def acquire(self):
        path = os.path.join(self.root, "ghost-toothAPI.lock")
        os.makedirs(self.root, exist_ok=True)
        fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o644)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            os.close(fd)
            log = Log(os.path.join(self.root, "ghost-toothAPI.log"))
            log.line("another copy is already running")
            log.close()
            return False
        self.lock_fd = fd
        self.log = Log(os.path.join(self.root, "ghost-toothAPI.log"))
        return True

    def on_signal(self, _sig, _frm):
        self.stop = True

    # -- main flow ------------------------------------------------------
    def run(self):
        if not self.acquire():
            return 1
        signal.signal(signal.SIGTERM, self.on_signal)
        signal.signal(signal.SIGINT, self.on_signal)

        log = self.log
        log.line("ghost-toothAPI %s" % SIM_VERSION)
        log.line("pid:%d" % os.getpid())
        log.line("hci: USB_FS_OPEN 0x2f1 (transfer 3) errno=0")
        log.line("hci: controller open alongside the system, 1 event and 1 ACL reads")
        log.line("radio ready")
        log.line("searching for a headset in pairing mode")

        mode, value = read_ini(os.path.join(self.root, "headset.ini"))
        if mode == "address":
            log.line("config: address %s" % value)
        elif mode == "name":
            log.line("config: name '%s'" % value)

        # -------- inquiry pass
        found = []
        deadline = time.time() + 4.0          # 15 s on a real console
        for dev in DEVICES:
            if self.stop:
                break
            time.sleep(0.35)
            addr, name, cls, rssi, score = dev
            ignored = name in ("BRAVIA 4K 2023", "QLED TV")
            log.line("found %s '%s' (class %s rssi %d)%s"
                     % (addr, name, cls, rssi, " [ignored]" if ignored else ""))
            if ignored:
                log.line("ignored %s '%s' class %s" % (addr, name, cls))
            else:
                found.append(dev)

        if self.stop:
            return self.finish()

        # -------- pick: the same decisions, and the same messages, the
        # payload makes: a pin first, the best-scored headphones as a fallback
        pick = None
        if mode == "address":
            pick = next((d for d in found if d[0] == value), None)
            if pick is None:
                if value == NEVER_ANSWER:
                    # the UI's silent scan pass: scan, log, page nothing
                    log.line("connection: no answer (try 1)")
                    log.line("could not connect - see log")
                    return self.finish()
                log.line("no device named '%s'; picking the most likely headphones"
                         % value)
        elif mode == "name":
            pick = next((d for d in found if value.lower() in d[1].lower()), None)
            if pick is None:
                log.line("no device named '%s'; picking the most likely headphones"
                         % value)

        if not found:
            log.line("no headphones yet, scanning again")
            time.sleep(0.8)
            log.line("no headphones found (TVs are ignored)")
            return self.finish()

        if pick is None:
            pick = max(found, key=lambda d: d[4])

        for d in sorted(found, key=lambda x: -x[4])[:3]:
            log.line("trying %s '%s' (score %d)" % (d[0], d[1], d[4]))

        addr, name = pick[0], pick[1]
        log.line("connecting  %s" % addr)
        if not os.path.exists(os.path.join(self.root, "bond.key")):
            log.line("no stored key: pairing")
            time.sleep(0.5)
            write_small(os.path.join(self.root, "bond.key"),
                        "link-key-for-%s\n" % addr)
            write_small(os.path.join(self.root, "peer.name"), "%s\n" % name)
            log.line("paired with %s, key saved" % addr)
        else:
            log.line("headset paired before: %s" % addr)
            write_small(os.path.join(self.root, "peer.name"), "%s\n" % name)

        time.sleep(0.6)
        log.line("connection complete: status 0 handle 0x0c01")
        log.line("sdp: service search -> 2 record(s)")
        log.line("avdtp: SBC sink 1, capabilities 15 15, bitpool 2-53")
        log.line("sbc: 512-byte frames, 6 per packet (27 ms)")
        log.line("encryption complete: status 0")
        log.line("capture: running, 48000 Hz stereo")
        log.line("avdtp: streaming SBC 48000 Hz, bitpool 40")
        log.line("audio  %s" % addr)
        log.line("waiting for audio")

        # -------- streaming
        packets = 0
        while not self.stop:
            time.sleep(1.0)
            packets += 375
            log.line("stream: %d packets, %d capture records, queue %d ms, "
                     "trimmed 0, overruns 0, restarts 0, 48000 Hz, credits 6, "
                     "completions assumed 0, reports missing 0"
                     % (packets, packets, 8 + (packets % 7)))
        log.line("disconnected: status 0x16 reason 0x13")
        return self.finish()

    def finish(self):
        self.log.line("ghost-toothAPI done")
        self.log.close()
        self.log = None
        try:
            os.unlink(os.path.join(self.root, "ghost-toothAPI.pid"))
        except OSError:
            pass
        if self.lock_fd is not None:
            fcntl.flock(self.lock_fd, fcntl.LOCK_UN)
            os.close(self.lock_fd)
            self.lock_fd = None
        return 0


def write_small(path, text):
    tmp = path + ".sim-tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    args = ap.parse_args()
    sys.exit(Sim(args.root).run())
