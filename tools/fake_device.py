"""Stand-in for the board: subscribes to the same ntfy topics, logs what the
board would receive, and publishes status like the firmware does. Lets you
test the web pages end to end with no hardware.

  python3 tools/fake_device.py <admin-topic> [seconds]
"""
import json, sys, threading, time, urllib.request

GUEST = "bearclock-a2039d53"
ADMIN = sys.argv[1]
RUN_FOR = float(sys.argv[2]) if len(sys.argv) > 2 else 120
state = {"remaining": 3 * 3600, "duration": 3 * 3600, "running": False, "queue": 0,
         "current": "", "brightness": 90, "guests": True, "wifi": "fake", "ntfy": True}
anchor = time.time()

def rem():
    if not state["running"]:
        return state["remaining"]
    return max(0, state["remaining"] - int(time.time() - anchor))

def publish():
    s = dict(state, remaining=rem())
    urllib.request.urlopen(urllib.request.Request(
        f"https://ntfy.sh/{GUEST}-status", data=json.dumps(s).encode(), method="POST"), timeout=10)

def apply(cmd):
    global anchor
    c = cmd.get("cmd")
    if c == "start" and not state["running"]:
        anchor = time.time(); state["running"] = True
    elif c == "pause" and state["running"]:
        state["remaining"] = rem(); state["running"] = False
    elif c == "set":
        state["duration"] = state["remaining"] = cmd.get("h", 0) * 3600 + cmd.get("m", 0) * 60 + cmd.get("s", 0)
        state["running"] = bool(cmd.get("start")); anchor = time.time()

threading.Timer(RUN_FOR, lambda: __import__("os")._exit(0)).start()
publish()
url = f"https://ntfy.sh/{GUEST},{ADMIN}/json"
with urllib.request.urlopen(url, timeout=RUN_FOR + 60) as r:
    print("subscribed", flush=True)
    for line in r:
        m = json.loads(line)
        if m.get("event") != "message":
            continue
        if m["topic"] == ADMIN:
            cmd = json.loads(m["message"])
            print("ADMIN", cmd, flush=True)
            apply(cmd)
            publish()
        else:
            print("GUEST", repr(m["message"]), flush=True)
