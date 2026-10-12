#!/usr/bin/env python3
"""Tiny command-line client for the DMX Scene Box file API.

  dmxbox.py <ip> status
  dmxbox.py <ip> ls
  dmxbox.py <ip> get <name>            print a file
  dmxbox.py <ip> put <localfile> [name]
  dmxbox.py <ip> rm <name>
  dmxbox.py <ip> play <name>
  dmxbox.py <ip> probe <ch> [to] [value]
  dmxbox.py <ip> pull <dir>            download every file into <dir>
  dmxbox.py <ip> push <dir>            upload every file from <dir>

The box must be in setup mode (input 16 high). Needs only the Python standard library.
"""
import json, os, sys, urllib.request, urllib.error


def call(base, method, path, body=None):
    req = urllib.request.Request(base + path, data=body, method=method,
                                 headers={"Content-Type": "text/plain"} if body is not None else {})
    try:
        with urllib.request.urlopen(req, timeout=8) as r:
            return r.read().decode()
    except urllib.error.HTTPError as e:
        sys.exit(f"{method} {path}: HTTP {e.code} {e.read().decode().strip()}")
    except urllib.error.URLError as e:
        sys.exit(f"cannot reach {base}: {e.reason} (is the box in setup mode?)")


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    base, cmd, args = "http://" + argv[1], argv[2], argv[3:]

    if cmd == "status":
        print(json.dumps(json.loads(call(base, "GET", "/api/status")), indent=2))
    elif cmd == "ls":
        for f in json.loads(call(base, "GET", "/api/files")):
            print(f"{f['size']:7d}  {f['name']}")
    elif cmd == "get":
        print(call(base, "GET", f"/api/files/{args[0]}"), end="")
    elif cmd == "put":
        name = args[1] if len(args) > 1 else os.path.basename(args[0])
        with open(args[0], "rb") as fh:
            print(call(base, "PUT", f"/api/files/{name}", fh.read()).strip())
    elif cmd == "rm":
        print(call(base, "DELETE", f"/api/files/{args[0]}").strip())
    elif cmd == "play":
        print(call(base, "POST", f"/api/play/{args[0]}").strip())
    elif cmd == "probe":
        q = f"ch={args[0]}" + (f"&to={args[1]}" if len(args) > 1 else "") + (f"&v={args[2]}" if len(args) > 2 else "")
        print(call(base, "GET", f"/api/probe?{q}").strip())
    elif cmd == "pull":
        os.makedirs(args[0], exist_ok=True)
        for f in json.loads(call(base, "GET", "/api/files")):
            data = call(base, "GET", f"/api/files/{f['name']}")
            with open(os.path.join(args[0], f["name"]), "w", encoding="utf-8", newline="\n") as fh:
                fh.write(data)
            print(f"pulled {f['name']} ({len(data)} bytes)")
    elif cmd == "push":
        for name in sorted(os.listdir(args[0])):
            p = os.path.join(args[0], name)
            if os.path.isfile(p) and not name.startswith("."):
                with open(p, "rb") as fh:
                    print(f"{name}: " + call(base, "PUT", f"/api/files/{name}", fh.read()).strip())
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
