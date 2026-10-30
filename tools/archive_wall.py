"""Saves every Staff Photo taped to the wall before ntfy expires it (3 h).

  ./bear archive            # leave running on the Mac during the party
Photos land in ~/Pictures/bear-wall/ (hidden ones too, in hidden/).
"""
import json, os, pathlib, re, sys, time, urllib.request

GUEST = "bearclock-a2039d53"
WALL, HIDE = f"{GUEST}-wall", f"{GUEST}-wall-hide"
OUT = pathlib.Path.home() / "Pictures" / "bear-wall"
(OUT / "hidden").mkdir(parents=True, exist_ok=True)
hidden = set()


def save(m):
    name = re.sub(r"[^A-Za-z0-9]+", "-", m.get("message", "")).strip("-")[:20] or "guest"
    stamp = time.strftime("%H%M%S", time.localtime(m["time"]))
    dest = OUT / f"{stamp}-{name}-{m['id']}.jpg"
    if dest.exists() or any(OUT.glob(f"hidden/*{m['id']}.jpg")):
        return
    try:
        urllib.request.urlretrieve(m["attachment"]["url"], dest)
        print("saved", dest.name, flush=True)
    except Exception as e:  # expired or network blip; nothing else to do
        print("failed", m["id"], e, flush=True)


def hide(pid):
    hidden.add(pid)
    for p in OUT.glob(f"*{pid}.jpg"):
        p.rename(OUT / "hidden" / p.name)


def handle(line):
    m = json.loads(line)
    if m.get("event") != "message":
        return
    if m["topic"] == HIDE:
        hide(m["message"].strip())
    elif m.get("attachment") and m["id"] not in hidden:
        save(m)


backoff = 1
while True:
    try:
        # since=all on (re)connect: picks up anything missed; existing files are skipped.
        url = f"https://ntfy.sh/{HIDE},{WALL}/json?since=all"
        with urllib.request.urlopen(url, timeout=90) as r:
            print("listening; saving to", OUT, flush=True)
            backoff = 1
            for line in r:
                handle(line)
    except KeyboardInterrupt:
        sys.exit(0)
    except Exception as e:
        print("reconnecting:", e, flush=True)
        time.sleep(backoff)
        backoff = min(backoff * 2, 60)
