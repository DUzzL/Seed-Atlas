#!/usr/bin/env python3
"""Tests the MCP server of Seed Atlas (seed-atlas --mcp) as an MCP client would.

    python3 mcp-test.py PATH/TO/seed-atlas REPO_ROOT WORK_DIR

Run by app-tests.sh, which gives the app settings of its own (the server is
started with a settings id of its own as well, so that the test can check
that it writes no settings). Standard library only.

It checks the protocol (initialize and version negotiation, ping, tools/list,
the JSON-RPC errors of malformed input, cancellation, progress), calls every
tool and compares the answers with what real vanilla servers generated (the
reference data in tools/worldgen-reference/data and tools/portal-reference/
data), runs a search with a known result and stops an open one, checks that
the default rights refuse a file write and the write rights allow it. With
SEED_ATLAS_TEST_VANILLA (the vanilla data of Minecraft 1.21.1) it also loads
the project's test pack and compares its world with the server's.
"""

import json
import os
import queue
import subprocess
import sys
import threading
import time

APP, ROOT, WORK = sys.argv[1], sys.argv[2], sys.argv[3]
REF = os.path.join(ROOT, "tools", "worldgen-reference", "data")
PORTALS = os.path.join(ROOT, "tools", "portal-reference", "data")
failures = []


def fail(message):
    failures.append(message)
    print("FAIL: " + message, file=sys.stderr, flush=True)


def ok(message):
    print("ok: " + message, flush=True)


class Server:
    """One server process; messages are read by a thread."""

    def __init__(self, name, env_extra=None):
        env = dict(os.environ)
        env.update(env_extra or {})
        self.log = open(os.path.join(WORK, "mcp-%s.log" % name), "w")
        self.proc = subprocess.Popen([APP, "--mcp"], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=self.log, env=env)
        self.messages = queue.Queue()
        self.bad_lines = []
        self.next_id = 100
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for raw in self.proc.stdout:
            try:
                msg = json.loads(raw.decode("utf-8"))
                if not isinstance(msg, dict) or msg.get("jsonrpc") != "2.0":
                    raise ValueError("not a JSON-RPC message")
                self.messages.put(msg)
            except ValueError:
                self.bad_lines.append(raw[:200])
        self.messages.put(None)

    def send_raw(self, text):
        self.proc.stdin.write(text.encode("utf-8") + b"\n")
        self.proc.stdin.flush()

    def send(self, msg):
        msg = dict(msg, jsonrpc="2.0")
        self.send_raw(json.dumps(msg))

    def receive(self, want_id, timeout=300, notes=None):
        """The response to want_id; other messages go to notes."""
        end = time.time() + timeout
        while True:
            left = end - time.time()
            if left <= 0:
                raise TimeoutError("no response to %r" % (want_id,))
            msg = self.messages.get(timeout=left)
            if msg is None:
                raise EOFError("the server ended")
            if "id" in msg and msg["id"] == want_id and "method" not in msg:
                return msg
            if notes is not None:
                notes.append(msg)

    def request(self, method, params=None, timeout=300, notes=None):
        self.next_id += 1
        rid = self.next_id
        msg = {"id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self.send(msg)
        return self.receive(rid, timeout, notes)

    def call(self, name, args, timeout=300, notes=None):
        """tools/call; returns (structuredContent or None, result)."""
        r = self.request("tools/call", {"name": name, "arguments": args}, timeout, notes)
        if "error" in r:
            raise RuntimeError("%s: protocol error %s" % (name, r["error"]))
        res = r["result"]
        return res.get("structuredContent"), res

    def tool(self, name, args, timeout=300):
        """A tool call that must succeed."""
        data, res = self.call(name, args, timeout)
        if res.get("isError") or data is None:
            text = res.get("content", [{}])[0].get("text", "")
            raise RuntimeError("%s failed: %s" % (name, text))
        # the same JSON as text (for clients without structured content)
        text = res["content"][0]["text"]
        if json.loads(text[text.index("{"):]) != data:
            fail("%s: the text content differs from structuredContent" % name)
        return data

    def initialize(self, version="2025-06-18"):
        r = self.request("initialize", {"protocolVersion": version, "capabilities": {},
                                        "clientInfo": {"name": "mcp-test", "version": "1"}})
        self.send({"method": "notifications/initialized"})
        return r

    def close(self, timeout=60):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            rc = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            rc = None
        self.reader.join(timeout=10)
        self.log.close()
        return rc


def read_lines(path, tag):
    with open(path, encoding="utf-8") as f:
        return [line.split() for line in f if line.startswith(tag + " ")]


def check(cond, message):
    if not cond:
        fail(message)
    return cond


# ------------------------------------------------------------------ protocol

def test_protocol(s):
    r = s.request("ping")
    check(r.get("result") == {}, "ping before initialize: %r" % r)
    r = s.initialize()
    res = r.get("result", {})
    check(res.get("protocolVersion") == "2025-06-18", "initialize: protocol version %r" % res)
    check("tools" in res.get("capabilities", {}), "initialize: no tools capability")
    check(res.get("serverInfo", {}).get("name") == "seed-atlas", "initialize: serverInfo")
    r = s.request("ping")
    check(r.get("result") == {}, "ping: %r" % r)

    r = s.request("tools/list")
    tools = {t["name"]: t for t in r["result"]["tools"]}
    expected = {"server_info", "biome_at", "surface_height", "biome_map", "structures_in_area",
                "nearest_structure", "spawn", "strongholds", "end_portals", "biome_stats",
                "structure_stats", "start_search", "search_status", "search_results",
                "stop_search", "export_search", "load_datapacks", "datapack_info",
                "unload_datapacks"}
    check(set(tools) == expected, "tools/list: %s" % sorted(set(tools) ^ expected))
    for t in tools.values():
        schema = t.get("inputSchema", {})
        check(schema.get("type") == "object" and isinstance(schema.get("properties"), dict),
              "tool %s: input schema" % t["name"])
        check(t.get("description"), "tool %s: no description" % t["name"])
    check(tools["export_search"]["annotations"]["readOnlyHint"] is False,
          "export_search is not marked as writing")

    # malformed input
    s.send_raw("this is not json")
    r = s.receive(None)
    check(r.get("error", {}).get("code") == -32700, "parse error: %r" % r)
    s.send_raw("[]")
    r = s.receive(None)
    check(r.get("error", {}).get("code") == -32600, "batch: %r" % r)
    s.send({"id": "x1", "method": 5})
    check(s.receive("x1").get("error", {}).get("code") == -32600, "method not a string")
    r = s.request("no/such/method")
    check(r.get("error", {}).get("code") == -32601, "unknown method: %r" % r)
    r = s.request("tools/call", {"name": "no_such_tool", "arguments": {}})
    check(r.get("error", {}).get("code") == -32602, "unknown tool: %r" % r)
    r = s.request("tools/call", {"name": "biome_at", "arguments": [1, 2]})
    check(r.get("error", {}).get("code") == -32602, "arguments not an object: %r" % r)
    r = s.request("tools/call", {"arguments": {}})
    check(r.get("error", {}).get("code") == -32602, "tools/call without a name: %r" % r)
    # bad values are tool errors (the model can correct them)
    for args, what in (({"seed": "1", "x": "abc", "z": 0}, "x not a number"),
                       ({"x": 0, "z": 0}, "no seed"),
                       ({"seed": "1", "x": 0, "z": 0, "mc": "9.99"}, "unknown version"),
                       ({"seed": "1", "x": 0, "z": 0, "bogus": 1}, "unknown argument"),
                       ({"seed": 2 ** 60, "x": 0, "z": 0}, "seed too large for a number"),
                       ({"seed": "1", "x": 10 ** 9, "z": 0}, "x out of range")):
        data, res = s.call("biome_at", args)
        check(res.get("isError") is True and res["content"][0]["text"], "biome_at, %s: %r" % (what, res))
    data, res = s.call("biome_map", {"seed": "1", "x1": -100000, "z1": -100000, "x2": 100000,
                                     "z2": 100000, "scale": 1})
    check(res.get("isError") is True and "scale" in res["content"][0]["text"].lower(),
          "biome_map: a huge area at 1:1 is refused with a hint: %r" % res)
    if not failures:
        ok("protocol (initialize, ping, tools/list, errors)")


def test_version_negotiation():
    s = Server("version")
    r = s.initialize("1999-01-01")
    check(r.get("result", {}).get("protocolVersion") == "2025-06-18",
          "an unknown client version gets the server's: %r" % r)
    check(s.close() == 0, "version: exit code")
    ok("version negotiation")


# ------------------------------------------------------------------ queries

def test_biomes(s):
    before = len(failures)
    for seed in ("12345", "-4172144997902289642"):
        path = os.path.join(REF, "normal_1.21.1_%s.txt" % seed)
        cells = [b for b in read_lines(path, "B") if -256 <= int(b[1]) < 256 and -256 <= int(b[3]) < 256]
        for y in (256, 64):
            pts = [[int(b[1]), int(b[3]), y] for b in cells if int(b[2]) == y][:600]
            want = ["minecraft:" + b[4] for b in cells if int(b[2]) == y][:600]
            got = s.tool("biome_at", {"seed": seed, "mc": "1.21.1", "points": pts,
                                      "resolution": "cell"})
            same = sum(1 for p, w in zip(got["points"], want) if p["biome"] == w)
            check(same >= len(want) - 2, "biome_at seed %s y %d: %d of %d cells as the server"
                  % (seed, y, same, len(want)))
    # the map at 1:4 against the server's cells (every 16 blocks at y 256)
    cells = {(int(b[1]), int(b[3])): "minecraft:" + b[4]
             for b in read_lines(os.path.join(REF, "normal_1.21.1_12345.txt"), "B")
             if int(b[2]) == 256 and -256 <= int(b[1]) < 256 and -256 <= int(b[3]) < 256}
    m = s.tool("biome_map", {"seed": "12345", "mc": "1.21.1", "x1": -256, "z1": -256,
                             "x2": 255, "z2": 255, "scale": 4, "max_cells": 16384})
    legend = {l["char"]: l["biome"] for l in m["legend"]}
    check(m["width"] == 128 and m["height"] == 128, "biome_map size %dx%d" % (m["width"], m["height"]))
    same = sum(1 for (x, z), b in cells.items()
               if legend[m["rows"][(z + 256) // 4][(x + 256) // 4]] == b)
    check(same >= len(cells) - 2, "biome_map: %d of %d cells as the server" % (same, len(cells)))
    # the biome analysis counts the same cells as the map
    st = s.tool("biome_stats", {"seed": "12345", "mc": "1.21.1", "x1": -256, "z1": -256,
                                "x2": 255, "z2": 255, "scale": 4})
    counts = {b["biome"]: b["cells"] for b in st["seeds"][0]["biomes"]}
    mapcounts = {l["biome"]: l["cells"] for l in m["legend"]}
    check(counts == mapcounts, "biome_stats and biome_map count different cells")
    # auto scale stays within max_cells
    m = s.tool("biome_map", {"seed": "12345", "mc": "1.21.1", "x": 0, "z": 0, "radius": 3000,
                             "max_cells": 1000})
    check(m["width"] * m["height"] <= 1000 and m["scale"] == 256, "biome_map auto scale: %d" % m["scale"])
    if len(failures) == before:
        ok("biomes against the vanilla server (biome_at, biome_map, biome_stats)")


def test_heights(s):
    hs = [h for h in read_lines(os.path.join(REF, "normal_1.21.1_12345.txt"), "H")][:800]
    got = s.tool("surface_height", {"seed": "12345", "mc": "1.21.1",
                                    "points": [[int(h[1]), int(h[2])] for h in hs]})
    exact = near = 0
    for h, p in zip(hs, got["points"]):
        d = abs(p["y"] - int(h[4]))   # the ocean floor: highest solid block
        exact += d == 0
        near += d <= 8
    check(exact >= 0.6 * len(hs) and near >= 0.9 * len(hs),
          "surface_height: %d exact, %d within 8 blocks of %d" % (exact, near, len(hs)))
    if exact >= 0.6 * len(hs):
        ok("surface_height: %d of %d columns block-exact" % (exact, len(hs)))


def portal_reference():
    """seed -> list of (engine index, start chunk, centre x, z, eyes)."""
    out = {}
    idx = None
    with open(os.path.join(PORTALS, "portals_1.21.1.txt"), encoding="utf-8") as f:
        for line in f:
            if line.startswith("# engine idx"):
                idx = int(line.split()[3])
                continue
            if line.startswith("#") or not line.strip():
                continue
            v = line.split()
            frames = [v[5 + 4 * i: 9 + 4 * i] for i in range(12)]
            xs = [int(fr[0]) for fr in frames]
            zs = [int(fr[2]) for fr in frames]
            eyes = sum(int(fr[3]) for fr in frames)
            out.setdefault(v[1], []).append((idx, (int(v[3]), int(v[4])),
                                             (min(xs) + max(xs)) // 2, (min(zs) + max(zs)) // 2, eyes))
            idx = None
    return out


def test_strongholds_portals(s):
    before = len(failures)
    ref = portal_reference()
    checked_eyes = 0
    for seed, rows in list(ref.items())[:4]:
        n = max(r[0] for r in rows if r[0] is not None) + 1
        sh = s.tool("strongholds", {"seed": seed, "mc": "1.21.1", "count": n})
        chunks = {(h["chunk_x"], h["chunk_z"]) for h in sh["strongholds"]}
        for r in rows:
            check(r[1] in chunks, "strongholds seed %s: start chunk %s missing" % (seed, r[1]))
        ep = s.tool("end_portals", {"seed": seed, "mc": "1.21.1", "count": n})
        portals = {(p["x"], p["z"]): p for p in ep["portals"]}
        for r in rows:
            p = portals.get((r[2], r[3]))
            if not check(p is not None, "end_portals seed %s: no portal at %d, %d" % (seed, r[2], r[3])):
                continue
            if p["certain"]:
                checked_eyes += 1
                check(p["eyes"] == r[4], "end_portals seed %s at %d, %d: %d eyes, server %d"
                      % (seed, r[2], r[3], p["eyes"], r[4]))
    check(checked_eyes >= 3, "end_portals: only %d certain portals to compare" % checked_eyes)
    if len(failures) == before:
        ok("strongholds and End portal eyes against the vanilla server (%d portals)" % checked_eyes)


def test_structures(s):
    before = len(failures)
    path = os.path.join(REF, "structures_normal_1.21.1_-42.txt")
    covered = set()
    for c in read_lines(path, "C"):
        for x in range(int(c[2]), int(c[3]) + 1):
            covered.add((x, int(c[1])))
    villages = {(int(v[2]), int(v[3])) for v in read_lines(path, "S") if v[1].startswith("minecraft:village_")}
    area = {"x1": -1600, "z1": -1600, "x2": 1599, "z2": 1599}
    found = s.tool("structures_in_area", dict(area, seed="-42", mc="1.21.1", types=["village"],
                                              max_results=5000))
    ours = {(v["x"] >> 4, v["z"] >> 4) for v in found["structures"]}
    inside = {c for c in villages if -100 <= c[0] < 100 and -100 <= c[1] < 100}
    check(inside and inside <= ours, "structures_in_area: server villages missing: %s" % sorted(inside - ours))
    extra = {c for c in ours if c in covered and c not in villages}
    check(not extra, "structures_in_area: villages the server does not have: %s" % sorted(extra))
    stats = s.tool("structure_stats", dict(area, seed="-42", mc="1.21.1", types=["village"]))
    check(stats["seeds"][0]["counts"].get("village") == found["total"],
          "structure_stats counts %r, structures_in_area %d" % (stats["seeds"][0]["counts"], found["total"]))
    near = s.tool("nearest_structure", {"seed": "-42", "mc": "1.21.1", "type": "village", "x": 0, "z": 0})
    st = near["structure"]
    c = (st["x"] >> 4, st["z"] >> 4)
    check(near["found"] and c in villages, "nearest_structure: %s is no server village" % (c,))
    nearer = [v for v in found["structures"] if (v["x"] ** 2 + v["z"] ** 2) ** 0.5 < near["distance"] - 0.5]
    check(not nearer, "nearest_structure: %s is nearer" % nearer[:1])
    sp = s.tool("spawn", {"seed": "-42", "mc": "1.21.1"})
    check(abs(sp["x"]) < 2000 and abs(sp["z"]) < 2000, "spawn far out: %r" % sp)
    info = s.tool("server_info", {"mc": "1.21.1"})
    check("village" in info["structures"]["minecraft:overworld"] and
          info["rights"]["write_files"] is False, "server_info: %r" % info["rights"])
    if len(failures) == before:
        ok("structures against the vanilla server (%d villages)" % len(inside))


# ------------------------------------------------------------------ searches

def wait_search(s, sid, limit=240):
    end = time.time() + limit
    while time.time() < end:
        st = s.tool("search_status", {"search_id": sid})
        if st["state"] != "running" and st["state"] != "starting":
            return st
        time.sleep(0.3)
    raise TimeoutError("search %s still running" % sid)


def test_searches(s):
    before = len(failures)
    # a filter of rc/examples over its known seeds and others (see app-tests.sh)
    conds = []
    with open(os.path.join(ROOT, "rc", "examples", "mushroom_icespike.txt"), encoding="utf-8") as f:
        for line in f:
            if line.startswith("#Cond:"):
                conds.append({"type": "raw", "hex": line[6:].strip()})
    known = ["35492", "40743", "50578", "78063", "84404", "86992", "88937", "93410", "94149"]
    x = 0x5EED
    others = []
    for _ in range(150):
        x = (x * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
        others.append(str(x - (1 << 64) if x >= 1 << 63 else x))
    st = s.tool("start_search", {"mc": "26.3", "conditions": conds, "seeds": others + known})
    sid = st["search_id"]
    st = wait_search(s, sid)
    check(st["state"] == "finished", "list search: state %s" % st["state"])
    res = s.tool("search_results", {"search_id": sid, "limit": 5000})
    check(sorted(res["seeds"]) == sorted(known), "list search: %s" % res["seeds"])

    # Lua conditions are refused (their scripts could reach files): the
    # first condition with its type (the first 16 bits) set to Lua (63)
    lua = {"type": "raw", "hex": "3f00" + conds[0]["hex"][4:]}
    data, r = s.call("start_search", {"mc": "26.3", "conditions": [lua], "seeds": ["1"]})
    check(r.get("isError") is True and "lua" in r["content"][0]["text"].lower(),
          "a Lua condition was not refused: %r" % r)
    # an open search, stopped
    st = s.tool("start_search", {"mc": "1.21.1", "threads": 1, "conditions": [
        {"type": "structure", "structure": "village", "radius": 64},
        {"type": "biome", "biomes": ["mushroom_fields"], "radius": 128, "scale": 16}]})
    sid2 = st["search_id"]
    check(st["state"] == "running", "open search: %s" % st["state"])
    time.sleep(1.5)
    st = s.tool("stop_search", {"search_id": sid2})
    check(st["state"] == "stopped", "stop_search: state %s" % st["state"])
    check(st["progress"]["seeds_checked"] > 0, "stop_search: nothing checked")
    s.tool("search_results", {"search_id": sid2})
    all_ = s.tool("search_status", {})
    check(len(all_["searches"]) >= 2, "search_status without id")
    # a condition the app cannot search is refused with the app's reason
    data, r = s.call("start_search", {"mc": "1.12", "conditions": [
        {"type": "structure", "structure": "ancient_city", "radius": 100}]})
    check(r.get("isError") is True and r["content"][0]["text"], "a condition of another version: %r" % r)
    data, r = s.call("start_search", {"mc": "1.21.1", "conditions": [{"type": "raw", "hex": "zz"}]})
    check(r.get("isError") is True, "a broken raw condition")
    # the default rights write no files
    target = os.path.join(WORK, "mcp-export-refused.txt")
    data, r = s.call("export_search", {"search_id": sid, "path": target})
    check(r.get("isError") is True and "settings" in r["content"][0]["text"].lower()
          and not os.path.exists(target), "export_search in the default rights: %r" % r)
    s.tool("stop_search", {"search_id": sid2, "discard": True})
    data, r = s.call("search_results", {"search_id": sid2})
    check(r.get("isError") is True, "a discarded search")
    if len(failures) == before:
        ok("searches (known result, stop, refusals, no file write by default)")
    return sid


def test_cancel_progress(s):
    before = len(failures)
    s.next_id += 1
    rid = s.next_id
    # (64 seeds with 16 million samples each: far too long to end by itself)
    s.send({"id": rid, "method": "tools/call", "params": {
        "name": "biome_stats", "_meta": {"progressToken": "p1"},
        "arguments": {"seeds": [str(i) for i in range(64)], "mc": "1.21.1", "radius": 60000,
                      "scale": 4, "samples": 16000000}}})
    notes = []
    end = time.time() + 30
    while time.time() < end and not any(n.get("method") == "notifications/progress" for n in notes):
        try:
            msg = s.messages.get(timeout=1)
        except queue.Empty:
            continue
        notes.append(msg)
    progress = [n["params"] for n in notes if n.get("method") == "notifications/progress"]
    check(progress and progress[0]["progressToken"] == "p1", "no progress notifications")
    s.send({"method": "notifications/cancelled", "params": {"requestId": rid, "reason": "test"}})
    t0 = time.time()
    r = s.request("ping", timeout=30, notes=notes)
    check(r.get("result") == {}, "ping after a cancel")
    # the cancelled call gets no response; the server stays usable
    time.sleep(1.0)
    r = s.request("tools/call", {"name": "biome_at", "arguments": {"seed": "1", "x": 0, "z": 0}},
                  notes=notes)
    check(not r["result"].get("isError"), "a call after a cancel")
    while not s.messages.empty():
        notes.append(s.messages.get())
    check(not any(n.get("id") == rid for n in notes), "a cancelled request got a response")
    values = [n["params"]["progress"] for n in notes if n.get("method") == "notifications/progress"]
    check(all(a < b for a, b in zip(values, values[1:])), "progress does not increase: %s" % values)
    check(time.time() - t0 < 20, "the cancel took long")
    if len(failures) == before:
        ok("cancellation and progress")


def test_write_rights(sid_seeds):
    ini = os.path.join(WORK, "mcp-write.ini")
    with open(ini, "w") as f:
        f.write("[mcp]\nrights=1\nsearchThreads=2\nsearchMinutes=1\n")
    s = Server("write", {"SEED_ATLAS_TEST_MCP_SETTINGS": ini})
    s.initialize()
    info = s.tool("server_info", {})
    check(info["rights"]["write_files"] is True and info["rights"]["search_threads"] == 2,
          "server_info with write rights: %r" % info["rights"])
    # the thread budget (2): two searches of one thread, then no more
    open_search = {"mc": "1.21.1", "threads": 1, "conditions": [
        {"type": "structure", "structure": "mansion", "radius": 64}]}
    a = s.tool("start_search", open_search)["search_id"]
    b = s.tool("start_search", open_search)["search_id"]
    data, r = s.call("start_search", open_search)
    check(r.get("isError") is True and "thread" in r["content"][0]["text"],
          "a third search over the thread budget: %r" % r)
    for sid in (a, b):
        s.tool("stop_search", {"search_id": sid})
    st = s.tool("start_search", {"mc": "1.21.1", "seeds": ["1", "2", "3"],
                                 "conditions": [{"type": "spawn", "radius": 100000}]})
    wait_search(s, st["search_id"])
    data, r = s.call("start_search", {"mc": "1.21.1", "seeds": ["1"], "start_seed": "5",
                                      "conditions": [{"type": "spawn", "radius": 100}]})
    check(r.get("isError") is True, "start_seed with a seed list")
    target = os.path.join(WORK, "mcp-export.txt")
    if os.path.exists(target):
        os.remove(target)
    s.tool("export_search", {"search_id": st["search_id"], "path": target})
    with open(target, encoding="utf-8") as f:
        text = f.read()
    check(text.startswith("#Version:") and "#Cond:" in text and
          sorted(l for l in text.splitlines() if not l.startswith("#")) == ["1", "2", "3"],
          "export_search wrote: %r" % text[:300])
    data, r = s.call("export_search", {"search_id": st["search_id"], "path": target})
    check(r.get("isError") is True, "export_search replaced a file without overwrite")
    data, r = s.call("export_search", {"search_id": st["search_id"], "path": "relative.txt"})
    check(r.get("isError") is True, "export_search took a relative path")
    check(s.close() == 0, "write rights: exit code")
    ok("write rights (export_search)")


# ------------------------------------------------------------------ datapacks

def test_datapacks(s):
    before = len(failures)
    pack = os.path.join(ROOT, "tools", "worldgen-reference", "testpack")
    spack = os.path.join(ROOT, "tools", "worldgen-reference", "testpack-structures")
    data, r = s.call("load_datapacks", {"paths": ["relative/pack"], "mc": "1.21.1"})
    check(r.get("isError") is True, "load_datapacks with a relative path")
    w = s.tool("load_datapacks", {"paths": [pack], "mc": "1.21.1"})
    check(w["report"]["ok"] and w["world"].startswith("dp"), "load_datapacks: %r" % w["report"])
    world = w["world"]
    # the test pack's world against a vanilla server with the pack
    for seed in ("12345", "-4172144997902289642"):
        ref = os.path.join(REF, "testpack_1.21.1_%s.txt" % seed)
        cells = [b for b in read_lines(ref, "B")][:800]
        pts = [[int(b[1]), int(b[3]), int(b[2])] for b in cells]
        got = s.tool("biome_at", {"world": world, "seed": seed, "points": pts, "resolution": "cell"})
        want = [b[4] if ":" in b[4] else "minecraft:" + b[4] for b in cells]
        same = sum(1 for p, b in zip(got["points"], want) if p["biome"] == b)
        check(same == len(cells), "datapack biome_at seed %s: %d of %d cells as the server"
              % (seed, same, len(cells)))
    # the vanilla Nether of the same world
    cells = read_lines(os.path.join(REF, "nether_1.21.1_12345.txt"), "B")[:400]
    got = s.tool("biome_at", {"world": world, "dimension": "nether", "seed": "12345",
                              "points": [[int(b[1]), int(b[3]), int(b[2])] for b in cells],
                              "resolution": "cell"})
    same = sum(1 for p, b in zip(got["points"], cells) if p["biome"] == "minecraft:" + b[4])
    check(same == len(cells), "datapack Nether: %d of %d cells" % (same, len(cells)))
    hs = read_lines(os.path.join(REF, "testpack_1.21.1_12345.txt"), "H")[:300]
    got = s.tool("surface_height", {"world": world, "seed": "12345",
                                    "points": [[int(h[1]), int(h[2])] for h in hs]})
    exact = sum(1 for h, p in zip(hs, got["points"]) if p["ocean_floor"] == int(h[4]))
    check(exact >= 0.6 * len(hs), "datapack heights: %d of %d exact" % (exact, len(hs)))
    info = s.tool("datapack_info", {"world": world})
    own = [b["id"] for b in info["biomes"] if b["id"].startswith("testpack:")]
    check(own, "datapack_info lists no biomes of the pack")
    m = s.tool("biome_map", {"world": world, "seed": "12345", "radius": 2048, "scale": 64,
                             "max_cells": 8192})
    check(any(l["biome"].startswith("testpack:") for l in m["legend"]), "datapack biome_map")
    st = s.tool("biome_stats", {"world": world, "seeds": ["1", "2"], "radius": 512, "scale": 16})
    check(len(st["seeds"]) == 2 and st["seeds"][0]["cells"] > 0, "datapack biome_stats")
    # a search with a datapack condition, checked seed by seed with the analysis
    biome = own[0]
    seeds = [str(i) for i in range(1, 13)]
    cond = {"kind": "biome_in_area", "biomes": [biome], "match": "any", "area": "rect",
            "x1": -256, "z1": -256, "x2": 255, "z2": 255, "scale": 16}
    st = s.tool("start_search", {"world": world, "conditions": [cond], "seeds": seeds})
    st = wait_search(s, st["search_id"])
    found = set(s.tool("search_results", {"search_id": st["search_id"]})["seeds"])
    want = set()
    stats = s.tool("biome_stats", {"world": world, "seeds": seeds, "x1": -256, "z1": -256,
                                   "x2": 255, "z2": 255, "scale": 16})
    for e in stats["seeds"]:
        if any(b["biome"] == biome for b in e["biomes"]):
            want.add(e["seed"])
    check(st["state"] == "finished" and found == want,
          "datapack search %s: %s, the analysis: %s" % (st["state"], sorted(found), sorted(want)))
    # structures of the structure test pack
    w2 = s.tool("load_datapacks", {"paths": [spack], "mc": "1.21.1"})
    found = s.tool("structures_in_area", {"world": w2["world"], "seed": "12345", "radius": 1024,
                                          "include_unknown": True})
    check(found["total"] > 0, "datapack structures_in_area found nothing")
    stats = s.tool("structure_stats", {"world": w2["world"], "seed": "12345", "radius": 1024,
                                       "include_unknown": True})
    check(sum(stats["seeds"][0]["counts"].values()) == found["total"], "datapack structure_stats")
    data, r = s.call("spawn", {"world": w2["world"], "seed": "1"})
    check(r.get("isError") is True, "spawn takes no datapack world")
    for h in (world, w2["world"]):
        s.tool("unload_datapacks", {"world": h})
    data, r = s.call("biome_at", {"world": world, "seed": "1", "x": 0, "z": 0})
    check(r.get("isError") is True, "an unloaded world")
    if len(failures) == before:
        ok("datapack worlds against the vanilla server with the test pack")


def main():
    s = Server("main")
    try:
        test_protocol(s)
        test_biomes(s)
        test_heights(s)
        test_strongholds_portals(s)
        test_structures(s)
        sid = test_searches(s)
        test_cancel_progress(s)
        if os.environ.get("SEED_ATLAS_TEST_VANILLA"):
            test_datapacks(s)
        else:
            print("skipped: MCP datapack worlds (set SEED_ATLAS_TEST_VANILLA)")
    except Exception as e:  # a crash or timeout is a failure, with the log
        fail("main server: %s: %s" % (type(e).__name__, e))
    t0 = time.time()
    rc = s.close()
    check(rc == 0, "the server did not end cleanly after stdin closed (exit %r)" % rc)
    check(time.time() - t0 < 30, "the server took %.0f s to end" % (time.time() - t0))
    check(not s.bad_lines, "stdout had lines that are no MCP messages: %r" % s.bad_lines[:3])
    try:
        test_version_negotiation()
        test_write_rights(None)
    except Exception as e:
        fail("%s: %s" % (type(e).__name__, e))
    if failures:
        print("%d MCP failure(s); logs in %s" % (len(failures), WORK), file=sys.stderr)
        sys.exit(1)
    print("all MCP tests passed")


main()
