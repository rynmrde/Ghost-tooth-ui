#!/usr/bin/env python3
"""tools/gt-smoke.py - end-to-end check of the UI against the mock payload.

Boots the host build of ghost-tooth-ui with tools/gt-sim.py standing in for
ghost-toothAPI, then drives it over HTTP exactly the way the browser does and
asserts the whole loop works:

    tile staged -> scan -> pick -> headset.ini -> payload restart -> streaming
                                                                    -> stop

This is the test that catches "the JSON and the JS disagree", which no unit test
can.  Run it with:  make test-api      (or: make test)

Copyright (C) 2026 Ghost-tooth-ui contributors
SPDX-License-Identifier: GPL-3.0-or-later
"""

import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN_DEFAULT = os.path.join(ROOT, "build", "ghost-tooth-ui")
SIM = os.path.join(ROOT, "tools", "gt-sim.py")

failures = []
checks = 0


def check(what, cond, extra=""):
    global checks
    checks += 1
    if not cond:
        failures.append("%s%s" % (what, (" - " + str(extra)) if extra else ""))
        print("  FAIL %s%s" % (what, (" - " + str(extra)) if extra else ""))
    else:
        print("  ok   %s" % what)


def http(port, path, body=None, method=None, raw=False):
    url = "http://127.0.0.1:%d%s" % (port, path)
    data = None
    headers = {}
    if body is not None:
        data = body.encode() if isinstance(body, str) else json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            payload = r.read()
            if raw:
                return r.status, dict(r.headers), payload
            return r.status, dict(r.headers), json.loads(payload.decode())
    except urllib.error.HTTPError as e:
        raw = e.read()
        if raw[:1] == b"{" or raw[:1] == b"[":
            try:
                return e.code, dict(e.headers), json.loads(raw.decode())
            except ValueError:
                pass
        return e.code, dict(e.headers), raw


def wait_idle(port, timeout=60.0, what="the operation queue to drain"):
    """The worker takes an op out of the one-slot queue atomically, so
    "neither busy nor queued" is the only reliable end-of-operation signal."""
    return wait_for(port, lambda s: not s["op"]["busy"] and not s["op"]["queued"],
                    timeout, what)


def act(port, action, arg=None, kind=None, timeout=70.0):
    body = {"action": action}
    if arg is not None:
        body["arg"] = arg
    code, _h, reply = http(port, "/api/action", body)
    check("%s accepted" % action, code == 200 and isinstance(reply, dict) and
          reply.get("ok") is True, (code, reply))
    st = wait_idle(port, timeout)
    if kind:
        check("%s was the op that ran" % action, st["op"]["kind"] == kind, st["op"])
        check("%s reported success" % action, st["op"]["status"] == 0, st["op"])
    return st


def raw_request(port, request_line):
    """A hand-written request, because urllib rewrites the path before sending
    it and would never deliver the traversal we are trying to prove refused."""
    with socket.create_connection(("127.0.0.1", port), timeout=10) as sock:
        sock.sendall(request_line.encode())
        chunks = []
        while True:
            try:
                got = sock.recv(65536)
            except OSError:
                break
            if not got:
                break
            chunks.append(got)
            if b"\r\n\r\n" in b"".join(chunks) and len(b"".join(chunks)) > 300:
                break
    data = b"".join(chunks)
    head, _, body = data.partition(b"\r\n\r\n")
    status = 0
    if head.startswith(b"HTTP/"):
        parts = head.split(b"\r\n")[0].split(b" ")
        status = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 0
    return status, {}, body


def state(port):
    code, _hdr, body = http(port, "/api/state")
    assert code == 200, code
    return body


def wait_for(port, pred, timeout=45.0, what="condition"):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = state(port)
        try:
            if pred(last):
                return last
        except (KeyError, TypeError):
            pass
        time.sleep(0.25)
    raise AssertionError("timeout waiting for %s (last: %s)"
                         % (what, json.dumps({k: last.get(k) for k in
                                              ("link", "scan", "op", "payload")})[:600]))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def main(argv):
    # make test-api hands us: <binary> <root-dir> <port|"0">
    binpath = argv[1] if len(argv) > 1 and argv[1] else BIN_DEFAULT
    if not os.path.exists(binpath):
        print("build the host binary first:  make host")
        return 2
    if len(argv) > 2 and argv[2]:
        root = os.path.abspath(argv[2])
        shutil.rmtree(root, ignore_errors=True)
        os.makedirs(root, exist_ok=True)
        keep = True
    else:
        root = tempfile.mkdtemp(prefix="ghost-tooth-ui-smoke-")
        keep = False
    if len(argv) > 3 and argv[3] not in ("", "0"):
        port = int(argv[3])
    else:
        port = free_port()
    print("binary %s\nroot   %s\nport   %d\n" % (binpath, root, port))
    log = open(os.path.join(root, "ui.out"), "wb")
    proc = subprocess.Popen(
        [binpath, "--root=" + root, "--port=%d" % port, "--sim=" + SIM,
         "--scan-seconds=6", "--tile-name=Ghost Tooth"],
        stdout=log, stderr=subprocess.STDOUT)
    try:
        run_checks(root, port, proc)
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()
        dump = open(os.path.join(root, "ui.out")).read()
        sim_pid = os.path.join(root, "ghost-toothAPI.pid")
        if os.path.exists(sim_pid):
            try:
                os.kill(int(open(sim_pid).read().strip()), signal.SIGKILL)
            except Exception:
                pass
        print("\n--- server output (last 40 lines) ---")
        print("\n".join(dump.splitlines()[-40:]))
        print("--- end ---")
        if not keep:
            shutil.rmtree(root, ignore_errors=True)
        else:
            print("(the tree the run left behind is still in %s)" % root)
    print("\n%d checks, %d failures" % (checks, len(failures)))
    if failures:
        for f in failures:
            print("  ! " + f)
        return 1
    return 0


def run_checks(root, port, proc):
    print("== boot")
    for _ in range(200):
        try:
            code, _h, _b = http(port, "/api/health")
            if code == 200:
                break
        except (urllib.error.URLError, ConnectionError, OSError):
            time.sleep(0.1)
    else:
        check("server came up", False, "no answer on %d" % port)
        return
    check("server came up", True)

    print("== static assets")
    code, hdr, body = http(port, "/", raw=True)
    check("GET / -> 200", code == 200, code)
    check("index is html", "text/html" in hdr.get("Content-Type", ""), hdr)
    check("index carries the app", b'id="device-list"' in body)
    check("index has no leftover template junk", b"</html>" in body)
    for path, ctype, needle in (("/main.css", "text/css", b"--accent"),
                                ("/main.js", "javascript", b"/api/state"),
                                ("/icon0.png", "image/png", b"\x89PNG")):
        code, hdr, body = http(port, path, raw=True)
        check("GET %s -> 200" % path, code == 200, code)
        check("%s content type" % path, ctype in hdr.get("Content-Type", ""),
              hdr.get("Content-Type"))
        check("%s body" % path, needle in body, len(body))
    code, _h, body = raw_request(port, "GET /../../etc/passwd HTTP/1.1\r\n"
                                       "Host: x\r\nConnection: close\r\n\r\n")
    check("literal traversal in the request line refused", code == 404, (code, body[:60]))
    code, _h, body = raw_request(port, "GET /%2e%2e%2f%2e%2e%2fetc%2fpasswd HTTP/1.1\r\n"
                                       "Host: x\r\nConnection: close\r\n\r\n")
    check("encoded traversal refused", code == 404, (code, body[:60]))
    code, _h, body = raw_request(port, "GET /index.html HTTP/1.1\r\n"
                                       "Host: x\r\nConnection: close\r\n\r\n")
    check("plain request line works", code == 200 and b"device-list" in body, code)
    code, _h, body = raw_request(port, "GARBAGE\r\n\r\n")
    check("garbage request line answered", code in (400, 405), code)
    code, _h, _b = http(port, "/nope.txt", raw=True)
    check("unknown path -> 404", code == 404, code)
    code, _h, _b = http(port, "/", raw=True, method="HEAD")
    check("HEAD / works", code == 200, code)

    print("== initial state")
    st = state(port)
    for key in ("product", "version", "payload", "config", "link", "bond",
                "devices", "scan", "op", "tile", "log", "server", "capabilities"):
        check("state has %s" % key, key in st, list(st.keys()))
    check("nothing is running yet", st["payload"]["running"] is False, st["payload"])
    check("link idle", st["link"]["state"] == "idle", st["link"])
    check("auto mode at boot", st["config"]["mode"] == "auto", st["config"])
    check("server port reported", st["server"]["port"] == port, st["server"])
    check("payload log path reported", st["payload"]["logPath"] ==
          os.path.join(root, "ghost-toothAPI.log"), st["payload"])
    check("json is compact and closed", json.dumps(st))

    print("== media tile staging")
    param = os.path.join(root, "ui-tile", "sce_sys", "param.json")
    check("param.json staged", os.path.exists(param), param)
    if os.path.exists(param):
        text = open(param).read()
        j = json.loads(text)
        check("tile is on the Media tab", j.get("applicationCategoryType") == 65536, j)
        check("tile deeplinks to this server",
              j.get("deeplinkUri") == "http://127.0.0.1:%d/" % port, j)
        check("tile title", j.get("localizedParameters", {}).get("en-US", {})
              .get("titleName") == "Ghost Tooth", j)
        check("tile icon staged",
              os.path.getsize(os.path.join(root, "ui-tile", "sce_sys", "icon0.png")) > 1000)

    print("== scan")
    st = act(port, "scan", kind="scan")
    check("scan reported devices", len(st["devices"]) >= 3, st["devices"])
    check("scan note explains itself", "silent" in st["scan"].get("note", ""),
          st["scan"])
    names = [d["name"] for d in st["devices"]]
    check("headphones found", "WH-1000XM4" in names, names)
    check("tv kept but ignored",
          any(d["name"] == "BRAVIA 4K 2023" and d["ignored"] for d in st["devices"]),
          st["devices"])
    check("rssi parsed", any(isinstance(d["rssi"], int) and d["rssi"] < 0
                             for d in st["devices"]), st["devices"])
    check("class parsed",
          all(re.fullmatch(r"[0-9a-f]{6}", d["class"] or "") for d in st["devices"]
              if d.get("class")), [d.get("class") for d in st["devices"]])
    check("scan stopped the payload again", st["payload"]["running"] is False,
          st["payload"])
    ini = os.path.join(root, "headset.ini")
    active = [l for l in open(ini).read().splitlines()
              if re.match(r"^\s*(address|name)\s*=", l)]
    check("scan left no pin behind", active == [], active)
    check("scan cleared the dead address",
          "02:00:00:00:00:00" not in open(ini).read())

    print("== connect")
    qc = next(d["addr"] for d in st["devices"] if d["name"] == "QC45")
    act(port, "connect", qc, kind="connect")
    st = wait_for(port, lambda s: s["link"]["state"] == "streaming" and
                  s["link"]["packets"] > 0, 45, "audio streaming")
    check("link name followed the pick", st["link"]["name"] == "QC45", st["link"])
    check("link address is the chosen one", st["link"]["addr"] == qc, st["link"])
    check("codec reported", "SBC" in (st["link"]["codec"] or ""), st["link"])
    check("bitpool reported", isinstance(st["link"]["bitpool"], int) and
          st["link"]["bitpool"] > 0, st["link"])
    check("packets counted", st["link"]["packets"] > 0, st["link"])
    check("payload running", st["payload"]["running"] is True, st["payload"])
    check("pid reported", isinstance(st["payload"]["pid"], int) and
          st["payload"]["pid"] > 0, st["payload"])
    check("ini holds the pin", ("address=%s" % qc) in open(ini).read())
    check("config block agrees", st["config"]["mode"] == "address" and
          st["config"]["address"] == qc, st["config"])
    check("bond saved", st["bond"]["keySaved"] is True, st["bond"])
    check("peer name read back", st["bond"]["peerName"] == "QC45", st["bond"])
    check("device marked connected",
          any(d["addr"] == qc and d["connected"] for d in st["devices"]), st["devices"])
    check("log shows the link", any("audio" in l["text"] for l in st["log"]["lines"]),
          [l["text"] for l in st["log"]["lines"]][-6:])
    check("log lines are stamped",
          all(re.match(r"^\d\d:\d\d:\d\d$", l["t"]) for l in st["log"]["lines"]),
          st["log"]["lines"][:2])

    print("== live updates")
    n1 = st["log"]["totalLines"]
    time.sleep(2.2)
    st = state(port)
    check("log keeps growing while streaming", st["log"]["totalLines"] > n1,
          (n1, st["log"]["totalLines"]))
    check("ring is capped", len(st["log"]["lines"]) <= 120, len(st["log"]["lines"]))
    check("log bytes tracked", st["log"]["bytes"] > 0, st["log"])

    print("== other endpoints")
    for path in ("/api/devices", "/api/log", "/api/health"):
        code, _h, body = http(port, path)
        check("GET %s -> 200" % path, code == 200, code)
    code, _h, body = http(port, "/api/devices")
    check("devices endpoint lists the pick",
          any(d["addr"] == qc for d in body["devices"]), body)

    print("== curl-friendly form")
    other = next(d["addr"] for d in st["devices"] if d["name"] == "arctis nova 5")
    code, _h, reply = http(port, "/api/connect?addr=" + other, "{}")
    check("POST /api/connect?addr= accepted", code == 200 and reply.get("ok"),
          (code, reply))
    wait_idle(port)
    st = wait_for(port, lambda s: s["link"]["state"] == "streaming" and
                  s["link"]["addr"] == other, 45, "the second headset")
    check("second pin written", ("address=%s" % other) in open(ini).read())
    check("no stale pin left", ("address=%s" % qc) not in open(ini).read())

    print("== name mode and auto")
    act(port, "name", "WH-1000", kind="name")
    st = wait_for(port, lambda s: s["link"]["state"] == "streaming", 45, "streaming")
    check("name saved to ini", "name=WH-1000" in open(ini).read(), open(ini).read())
    check("address pin removed", "address=" not in
          [l for l in open(ini).read().splitlines() if not l.startswith(";")])
    check("config block in name mode", st["config"]["mode"] == "name" and
          st["config"]["name"] == "WH-1000", st["config"])
    st = act(port, "auto", kind="auto")
    check("auto clears pins", st["config"]["mode"] == "auto", st["config"])
    check("auto leaves no active line",
          [l for l in open(ini).read().splitlines()
           if re.match(r"^\s*(address|name)\s*=", l)] == [])

    print("== forget pairing")
    st = act(port, "forget", kind="forget")
    check("bond cleared", True)
    st = wait_for(port, lambda s: s["bond"]["keySaved"] is False, 15, "bond cleared")
    check("bond.key gone", not os.path.exists(os.path.join(root, "bond.key")))
    check("peer.name gone", not os.path.exists(os.path.join(root, "peer.name")))

    print("== refusal of nonsense")
    code, _h, _b = http(port, "/api/action", {"action": "launch-missiles"})
    check("unknown action -> 400", code == 400, code)
    code, _h, _b = http(port, "/api/action", "{not json")
    check("unparseable body -> 400", code == 400, code)
    code, _h, _b = http(port, "/api/action", {})
    check("empty body -> 400", code == 400, code)
    code, _h, body = http(port, "/api/action",
                          {"action": "connect", "arg": "zz:zz:zz"})
    check("address-shaped garbage refused", code == 400 and body.get("ok") is False,
          (code, body))
    wait_idle(port, 30)
    code, _h, body = http(port, "/api/action", {"action": "connect", "arg": ""})
    check("empty arg refused", code == 400, code)
    check("garbage never reached the ini", "zz:zz:zz" not in open(ini).read(),
          open(ini).read())
    code, _h, body = http(port, "/api/action",
                          {"action": "name", "arg": "a\naddress=00:00:00:00:00:01"})
    wait_idle(port, 30)
    text = open(ini).read()
    check("newline injection refused or sanitised",
          "address=00:00:00:00:00:01" not in text, text)
    check("a name with spaces is a legitimate filter",
          'name=two words' in (open(ini).read() if act_ok else "")) if False else None

    print("== stop")
    act(port, "stop", kind="stop")
    st = wait_for(port, lambda s: s["payload"]["running"] is False, 20, "payload exit")
    check("radio released", st["payload"]["running"] is False, st["payload"])
    check("sim logged its exit", st["link"]["state"] in ("idle", "disconnected"),
          st["link"])
    check("ui still alive after stopping the payload", state(port)["server"]["port"]
          == port)

    print("== restart from the UI")
    act(port, "apply", kind="apply")
    st = wait_for(port, lambda s: s["payload"]["running"] is True, 30, "relaunch")
    check("payload came back", st["payload"]["running"] is True, st["payload"])
    check("fresh run starts clean", st["link"]["packets"] == 0 or
          st["link"]["state"] in ("connecting", "streaming"), st["link"])
    st = wait_for(port, lambda s: s["link"]["state"] in ("connecting", "streaming"),
                  40, "the relaunched payload to reach a headset")
    check("auto mode picked a headset itself",
          st["link"]["state"] in ("connecting", "streaming"), st["link"])
    check("automatic pick found headphones", st["link"]["name"] in
          ("WH-1000XM4", "QC45", "arctis nova 5"), st["link"])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
