"""Drive a local vanilla dedicated server through its console (stdin/stdout).

The server is configured to bind to 127.0.0.1 only, with rcon, query and the
management server disabled. It is always stopped on exit of the context
manager.
"""

import os
import queue
import re
import shutil
import subprocess
import threading
import time

LOG_RE = re.compile(r"^\[[0-9:]+\] \[[^\]]+/(INFO|WARN|ERROR)\]: (.*)$")


LEVEL_TYPES = ("normal", "amplified", "large_biomes")

# Starting a server requires accepting Mojang's EULA (https://aka.ms/MinecraftEULA).
# The tools write eula.txt only when whoever runs them says so: with the
# command line flag --accept-eula (accept_eula()) or this environment variable
# set to 1.
EULA_ENV = "MINECRAFT_ACCEPT_EULA"
_eula_accepted = False


def accept_eula():
    """Records that the person running the tool accepted the EULA."""
    global _eula_accepted
    _eula_accepted = True


def eula_accepted():
    return _eula_accepted or os.environ.get(EULA_ENV) == "1"


def require_eula(flag):
    """Exits with a message unless the EULA was accepted (flag or EULA_ENV)."""
    if flag:
        accept_eula()
    if not eula_accepted():
        raise SystemExit("These tools start a Minecraft server, which requires accepting Mojang's EULA "
                         "(https://aka.ms/MinecraftEULA). Pass --accept-eula (or set %s=1) if you "
                         "accept it." % EULA_ENV)


def level_type_property(mc, level_type="normal"):
    """Value of the server.properties key 'level-type' for a world type.

    level_type is the world preset name without namespace ('normal',
    'amplified', 'large_biomes'). 1.19+ takes the preset id
    ('minecraft:amplified'). 1.18.x only knows its own legacy names
    ('amplified', 'largebiomes') and silently generates a Default world for
    anything else, so always confirm the result with
    mcworld.overworld_noise_settings().
    """
    if level_type not in LEVEL_TYPES:
        raise ValueError("unknown level type %r" % level_type)
    if level_type == "normal":
        return "minecraft:normal"
    if [int(v) for v in re.findall(r"\d+", mc)[:2]] < [1, 19]:
        return {"amplified": "amplified", "large_biomes": "largebiomes"}[level_type]
    return "minecraft:" + level_type


def write_properties(server_dir, seed, level_name, port, level_type="minecraft:normal"):
    """level_type: the 'level-type' value, see level_type_property()."""
    props = {
        "level-seed": str(seed),
        "level-name": level_name,
        "level-type": level_type.replace(":", "\\:"),
        "generate-structures": "true",
        "server-ip": "127.0.0.1",
        "server-port": str(port),
        "enable-rcon": "false",
        "enable-query": "false",
        "enable-status": "false",
        "management-server-enabled": "false",
        "white-list": "true",
        "enforce-whitelist": "true",
        "max-players": "1",
        "view-distance": "3",
        "simulation-distance": "3",
        "pause-when-empty-seconds": "0",
        "sync-chunk-writes": "true",
        "region-file-compression": "deflate",
        "spawn-protection": "0",
        "max-tick-time": "-1",
    }
    path = os.path.join(server_dir, "server.properties")
    lines = []
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                k = line.split("=", 1)[0].strip()
                if k in props or line.startswith("#"):
                    continue
                lines.append(line.rstrip("\n"))
    for k, v in props.items():
        lines.append("%s=%s" % (k, v))
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    # only with the consent of the person running the tool (require_eula())
    if not eula_accepted():
        raise RuntimeError("the Minecraft EULA was not accepted (pass --accept-eula)")
    with open(os.path.join(server_dir, "eula.txt"), "w") as f:
        f.write("eula=true\n")


class Server:
    def __init__(self, server_dir, cmd, log=None, start_timeout=900):
        """cmd: full java command line, run with cwd=server_dir.

        start_timeout: seconds to wait for the server's "Done" line. 1.18.2
        sometimes deadlocks while loading its resources; a start that times
        out is killed and reported as a TimeoutError.
        """
        self.start_timeout = start_timeout
        self.dir = server_dir
        self.cmd = cmd
        self.proc = None
        self.lines = queue.Queue()
        self.ringdump = []
        self.crashed = None
        self.default_states = {}
        self.ringdump_done = threading.Event()
        self.logf = log

    def __enter__(self):
        self.proc = subprocess.Popen(
            self.cmd, cwd=self.dir, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1)
        threading.Thread(target=self._pump, daemon=True).start()
        try:
            self.wait_for(re.compile(r"^Done \("), timeout=self.start_timeout)
        except BaseException:
            # it never finished starting, so there is nothing to save
            self.stop(grace=30)
            raise
        return self

    def __exit__(self, *exc):
        self.stop()

    def _pump(self):
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            if self.logf:
                self.logf.write(line + "\n")
                self.logf.flush()
            if "Caught exception in thread" in line or "Exception generating new chunk" in line:
                # a worldgen worker died; this world can no longer be trusted
                self.crashed = line
            k = line.find("RINGDUMP ")
            if k >= 0:
                rest = line[k + 9:].split()
                if rest and rest[0] == "DEFAULT":
                    # e.g. Block{minecraft:end_portal_frame}[eye=false,facing=north]
                    m = re.match(r"Block\{([^}]+)\}(?:\[(.*)\])?", " ".join(rest[1:]))
                    if m:
                        props = dict(kv.split("=", 1) for kv in (m.group(2) or "").split(",") if kv)
                        self.default_states[m.group(1)] = props
                elif rest and rest[0] in ("END", "ERROR"):
                    if rest[0] == "ERROR":
                        self.ringdump = None
                    self.ringdump_done.set()
                elif self.ringdump is not None:
                    self.ringdump.append(rest)
            self.lines.put(line)
        self.ringdump_done.set()
        self.lines.put(None)

    def send(self, cmd):
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()

    def wait_for(self, pattern, timeout=300):
        """Return the message text of the first log line matching pattern."""
        end = time.time() + timeout
        while True:
            left = end - time.time()
            if self.crashed:
                raise RuntimeError("server reported: %s" % self.crashed)
            if left <= 0:
                raise TimeoutError("timeout waiting for %s" % pattern.pattern)
            try:
                line = self.lines.get(timeout=min(left, 5))
            except queue.Empty:
                continue
            if line is None:
                raise RuntimeError("server exited while waiting for %s" % pattern.pattern)
            m = LOG_RE.match(line)
            msg = m.group(2) if m else line
            hit = pattern.search(msg)
            if hit:
                return hit

    def command(self, cmd, pattern, timeout=300):
        self.send(cmd)
        return self.wait_for(pattern, timeout)

    def save_flush(self):
        self.command("save-all flush", re.compile(r"Saved the game"), timeout=600)

    def stop(self, grace=300):
        if self.proc and self.proc.poll() is None:
            try:
                self.send("stop")
                self.proc.wait(timeout=grace)
            except BaseException:
                self.proc.kill()
                self.proc.wait()
        self.proc = None


def remove_world(server_dir, level_name):
    path = os.path.join(server_dir, level_name)
    if os.path.isdir(path):
        shutil.rmtree(path)
