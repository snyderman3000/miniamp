#!/usr/bin/env python3
"""
Cider Bridge
------------
Runs on the Pop!_OS machine next to Cider (Apple Music). It does two jobs:

  1. AUDIO: creates a virtual PipeWire sink, moves Cider's audio into it (so the
     PC speakers stay silent), and encodes that audio to an MP3 stream that
     MiniAmp on the Miyoo plays through its headphone jack.

  2. CONTROL: exposes a small LAN-friendly JSON API that MiniAmp uses to browse
     your Apple Music library, search, and control playback. It relays those
     calls to Cider's own RPC API (which only listens on localhost).

Standard library only. Needs: python3, ffmpeg, pactl (pipewire-pulse).
"""
import json
import os
import queue
import signal
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))

DEFAULTS = {
    "cider_host": "127.0.0.1",
    "cider_port": 10767,
    "cider_token": "",          # from Cider: Settings > Connectivity > Manage External Application Access
    "listen_host": "0.0.0.0",   # 0.0.0.0 = reachable from the Miyoo on your LAN
    "listen_port": 8780,
    "bitrate": "192k",
    "sink_name": "cider_bridge",
    "storefront": "us",         # Apple Music catalog region for search
    "app_match": ["cider"],     # audio streams whose app/process name contains this get captured
    "artwork_size": 160,
}


def load_config():
    cfg = dict(DEFAULTS)
    path = os.environ.get("CIDER_BRIDGE_CONFIG", os.path.join(HERE, "config.json"))
    if os.path.exists(path):
        with open(path) as f:
            cfg.update(json.load(f))
    if os.environ.get("CIDER_TOKEN"):
        cfg["cider_token"] = os.environ["CIDER_TOKEN"]
    return cfg


CFG = load_config()


def log(*args):
    print(time.strftime("%H:%M:%S"), *args, flush=True)


# --------------------------------------------------------------------------
# Cider RPC client
# --------------------------------------------------------------------------
class Cider:
    def __init__(self, cfg):
        self.base = f"http://{cfg['cider_host']}:{cfg['cider_port']}/api/v1"
        self.token = cfg["cider_token"]

    def _req(self, method, path, body=None, timeout=6):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.base + path, data=data, method=method)
        if self.token:
            req.add_header("apptoken", self.token)  # Cider's header name is "apptoken"
        if data is not None:
            req.add_header("Content-Type", "application/json")
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read()
            return json.loads(raw) if raw else {}

    def get(self, path):
        return self._req("GET", path)

    def post(self, path, body=None):
        return self._req("POST", path, body if body is not None else {})

    def alive(self):
        try:
            self._req("GET", "/playback/active", timeout=2)
            return True
        except Exception:
            return False

    def amapi(self, path):
        """Run an Apple Music API request through Cider (it holds the user token)."""
        r = self.post("/amapi/run-v3", {"path": path})
        # Cider wraps Apple's JSON; unwrap until we reach the object with data/results.
        while (isinstance(r, dict) and isinstance(r.get("data"), dict)
               and ("data" in r["data"] or "results" in r["data"])):
            r = r["data"]
        return r


cider = Cider(CFG)


def art_url(template, size=None):
    if not template:
        return None
    size = size or CFG["artwork_size"]
    return template.replace("{w}", str(size)).replace("{h}", str(size)).replace("{f}", "jpg")


def bridge_art(url):
    """Rewrite Apple's https artwork URL to go through the bridge (Miyoo-friendly http)."""
    return "/api/art?u=" + urllib.parse.quote(url, safe="") if url else None


def simplify(item):
    a = item.get("attributes", {}) or {}
    return {
        "id": item.get("id"),
        "type": item.get("type"),
        "href": item.get("href"),
        "name": a.get("name"),
        "artist": a.get("artistName") or a.get("curatorName"),
        "album": a.get("albumName"),
        "duration_ms": a.get("durationInMillis"),
        "track_count": a.get("trackCount"),
        "artwork": bridge_art(art_url((a.get("artwork") or {}).get("url"))),
    }


def simplify_list(resp):
    items = resp.get("data", []) if isinstance(resp, dict) else []
    out = {"items": [simplify(i) for i in items]}
    nxt = resp.get("next") if isinstance(resp, dict) else None
    if nxt:
        q = urllib.parse.parse_qs(urllib.parse.urlparse(nxt).query)
        if "offset" in q:
            out["next_offset"] = int(q["offset"][0])
    return out


# --------------------------------------------------------------------------
# Audio: virtual sink + routing + MP3 encoder + fan-out to listeners
# --------------------------------------------------------------------------
class Broadcaster:
    """Every listener gets its own small queue. Slow listeners drop old audio
    rather than lagging further behind, so the stream stays close to live."""

    def __init__(self):
        self.clients = set()
        self.lock = threading.Lock()

    def add(self):
        q = queue.Queue(maxsize=48)
        with self.lock:
            self.clients.add(q)
        return q

    def remove(self, q):
        with self.lock:
            self.clients.discard(q)

    def count(self):
        with self.lock:
            return len(self.clients)

    def publish(self, chunk):
        with self.lock:
            clients = list(self.clients)
        for q in clients:
            try:
                q.put_nowait(chunk)
            except queue.Full:
                try:
                    q.get_nowait()
                    q.put_nowait(chunk)
                except Exception:
                    pass


broadcaster = Broadcaster()


def pactl_json(*args):
    out = subprocess.run(["pactl", "-f", "json", *args], capture_output=True, text=True, check=True)
    return json.loads(out.stdout or "[]")


class Audio:
    def __init__(self, cfg):
        self.sink = cfg["sink_name"]
        self.match = [m.lower() for m in cfg["app_match"]]
        self.bitrate = cfg["bitrate"]
        self.module_id = None
        self.ffmpeg = None
        self.parec = None
        self.running = True

    def ensure_sink(self):
        sinks = pactl_json("list", "sinks")
        if any(s.get("name") == self.sink for s in sinks):
            return
        out = subprocess.run(
            ["pactl", "load-module", "module-null-sink", f"sink_name={self.sink}",
             "sink_properties=device.description=Cider_Bridge"],
            capture_output=True, text=True, check=True)
        self.module_id = out.stdout.strip()
        log(f"created virtual sink '{self.sink}' (module {self.module_id})")

    def sink_index(self):
        for s in pactl_json("list", "sinks"):
            if s.get("name") == self.sink:
                return s.get("index")
        return None

    def _is_cider(self, si):
        props = si.get("properties", {}) or {}
        hay = " ".join(str(props.get(k, "")) for k in (
            "application.name", "application.process.binary", "application.icon_name",
            "node.name", "application.id")).lower()
        if any(m in hay for m in self.match):
            return True
        # Electron apps often report as "Chromium"; check the owning process instead.
        pid = props.get("application.process.id")
        if pid:
            try:
                with open(f"/proc/{pid}/cmdline", "rb") as f:
                    cmd = f.read().replace(b"\0", b" ").decode(errors="ignore").lower()
                return any(m in cmd for m in self.match)
            except OSError:
                pass
        return False

    def route_loop(self):
        """Every 2s, move any Cider audio stream into our virtual sink."""
        was_alive = False
        while self.running:
            try:
                self.ensure_sink()
                idx = self.sink_index()
                for si in pactl_json("list", "sink-inputs"):
                    if si.get("sink") != idx and self._is_cider(si):
                        subprocess.run(["pactl", "move-sink-input", str(si["index"]), self.sink],
                                       capture_output=True)
                        log(f"routed Cider stream {si['index']} into {self.sink}")
            except Exception as e:
                log("routing error:", e)
            # Pin Cider's own volume at 100%; volume is controlled on the Miyoo.
            alive = cider.alive()
            if alive and not was_alive:
                log("Cider is reachable")
                try:
                    cider.post("/playback/volume", {"volume": 1})
                except Exception as e:
                    log("couldn't set Cider volume:", e)
            elif was_alive and not alive:
                log("Cider went away")
            was_alive = alive
            time.sleep(2)

    def encode_loop(self):
        """Capture the sink's monitor with parec, encode to MP3 with ffmpeg.
        parec hands ffmpeg plain raw audio, so ffmpeg doesn't need PulseAudio
        support compiled in. Restarts the pair if either exits."""
        while self.running:
            rec_cmd = ["parec", "-d", f"{self.sink}.monitor", "--format=s16le",
                       "--rate=44100", "--channels=2", "--latency-msec=20", "--raw"]
            enc_cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error",
                       "-f", "s16le", "-ar", "44100", "-ac", "2", "-i", "pipe:0",
                       "-c:a", "libmp3lame", "-b:a", self.bitrate,
                       "-f", "mp3", "-flush_packets", "1", "pipe:1"]
            try:
                self.parec = subprocess.Popen(rec_cmd, stdout=subprocess.PIPE)
                self.ffmpeg = subprocess.Popen(enc_cmd, stdin=self.parec.stdout, stdout=subprocess.PIPE)
                self.parec.stdout.close()  # ffmpeg owns the pipe now
                log(f"encoder started ({self.bitrate} MP3)")
                fd = self.ffmpeg.stdout.fileno()
                while self.running:
                    chunk = os.read(fd, 4096)
                    if not chunk:
                        break
                    broadcaster.publish(chunk)
            except Exception as e:
                log("encoder error:", e)
            for proc in (self.ffmpeg, self.parec):
                if proc and proc.poll() is None:
                    proc.terminate()
            if self.running:
                log("encoder stopped; restarting in 2s")
                time.sleep(2)

    def start(self):
        self.ensure_sink()
        threading.Thread(target=self.route_loop, daemon=True).start()
        threading.Thread(target=self.encode_loop, daemon=True).start()

    def stop(self):
        self.running = False
        for proc in (self.ffmpeg, self.parec):
            if proc and proc.poll() is None:
                proc.terminate()
        if self.module_id:
            # Unloading the sink sends Cider back to the normal speakers automatically.
            subprocess.run(["pactl", "unload-module", self.module_id], capture_output=True)
            log("removed virtual sink")


# --------------------------------------------------------------------------
# HTTP API
# --------------------------------------------------------------------------
STATE = {"generation": 0}   # bumps on skip/seek/play so clients know to flush their buffer
STATE_LOCK = threading.Lock()


def bump():
    with STATE_LOCK:
        STATE["generation"] += 1
        return STATE["generation"]


def now_playing():
    try:
        kbps = int(str(CFG["bitrate"]).lower().rstrip("k"))
    except ValueError:
        kbps = 0
    out = {"cider": False, "playing": False, "track": None,
           "generation": STATE["generation"], "listeners": broadcaster.count(), "kbps": kbps}
    if not cider.alive():
        return out
    out["cider"] = True
    try:
        out["playing"] = bool(cider.get("/playback/is-playing").get("is_playing"))
    except Exception:
        pass
    try:
        info = cider.get("/playback/now-playing").get("info") or {}
        if info.get("name"):
            out["track"] = {
                "id": (info.get("playParams") or {}).get("id"),
                "name": info.get("name"),
                "artist": info.get("artistName"),
                "album": info.get("albumName"),
                "duration_ms": info.get("durationInMillis"),
                "position_s": info.get("currentPlaybackTime"),
                "artwork": bridge_art(art_url((info.get("artwork") or {}).get("url"))),
            }
    except Exception:
        pass
    for key, path in (("shuffle", "/playback/shuffle-mode"), ("repeat", "/playback/repeat-mode")):
        try:
            out[key] = cider.get(path).get("value")
        except Exception:
            pass
    return out


SIMPLE_COMMANDS = {
    "play": "/playback/play",
    "pause": "/playback/pause",
    "playpause": "/playback/playpause",
    "next": "/playback/next",
    "previous": "/playback/previous",
    "shuffle": "/playback/toggle-shuffle",
    "repeat": "/playback/toggle-repeat",
}
FLUSH_ON = {"next", "previous"}

LIBRARY = {
    "playlists": "/v1/me/library/playlists",
    "albums": "/v1/me/library/albums",
    "songs": "/v1/me/library/songs",
    "artists": "/v1/me/library/artists",
}


ITEM_FIELDS = ("id", "type", "href", "name", "artist", "album", "duration_ms", "track_count", "artwork")


def clean(v):
    if v is None:
        return ""
    if isinstance(v, bool):
        return "1" if v else "0"
    return str(v).replace("\t", " ").replace("\r", " ").replace("\n", " ")


def to_text(obj):
    """Plain-text form of a response, for MiniAmp (easy to read in C, no JSON parser).
    Lists:   I<TAB>id<TAB>type<TAB>href<TAB>name<TAB>artist<TAB>album<TAB>duration_ms<TAB>track_count<TAB>artwork
             N<TAB>next_offset           (only when there is more)
    Search:  S<TAB>songs / S<TAB>albums / S<TAB>playlists, each followed by its I lines
    Others:  key=value lines; nested track fields become track_name=..., etc."""
    def item_line(it):
        return "I\t" + "\t".join(clean(it.get(k)) for k in ITEM_FIELDS)
    lines = []
    if "items" in obj:
        lines += [item_line(it) for it in obj["items"]]
        if obj.get("next_offset") is not None:
            lines.append(f"N\t{obj['next_offset']}")
    elif all(k in obj for k in ("songs", "albums", "playlists")):
        for k in ("songs", "albums", "playlists"):
            lines.append(f"S\t{k}")
            lines += [item_line(it) for it in obj[k]]
    else:
        for k, v in obj.items():
            if isinstance(v, dict):
                lines += [f"{k}_{k2}={clean(v2)}" for k2, v2 in v.items()]
            else:
                lines.append(f"{k}={clean(v)}")
    return "\n".join(lines) + "\n"


class Handler(BaseHTTPRequestHandler):
    server_version = "CiderBridge/0.2"
    text_mode = False

    def log_message(self, fmt, *args):
        pass  # keep the journal quiet; errors are logged explicitly

    # ---- helpers
    def send_json(self, obj, code=200):
        if self.text_mode:
            body = to_text(obj).encode()
            ctype = "text/plain; charset=utf-8"
        else:
            body = json.dumps(obj).encode()
            ctype = "application/json"
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def body_json(self):
        n = int(self.headers.get("Content-Length") or 0)
        if not n:
            return {}
        try:
            return json.loads(self.rfile.read(n))
        except Exception:
            return {}

    def fail(self, e):
        if isinstance(e, urllib.error.HTTPError) and e.code in (401, 403):
            return self.send_json({"error": "Cider rejected the API token — check cider_token in config.json"}, 502)
        if isinstance(e, urllib.error.URLError):
            return self.send_json({"error": "Cider isn't reachable — is it running?"}, 503)
        log("error:", repr(e))
        return self.send_json({"error": str(e)}, 500)

    # ---- routes
    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        p, q = url.path, urllib.parse.parse_qs(url.query)
        self.text_mode = q.get("fmt", [""])[0] == "text"
        try:
            if p in ("/", "/index.html"):
                return self.serve_file("test_page.html", "text/html; charset=utf-8")
            if p in ("/stream.mp3", "/stream"):
                return self.stream()
            if p == "/api/status":
                return self.send_json(now_playing())
            if p == "/api/art":
                return self.art(q.get("u", [""])[0])
            if p == "/api/recent":
                return self.send_json(simplify_list(cider.amapi("/v1/me/recent/played?limit=25")))
            if p == "/api/search":
                term = q.get("q", [""])[0].strip()
                if not term:
                    return self.send_json({"error": "missing q"}, 400)
                path = (f"/v1/catalog/{CFG['storefront']}/search?types=songs,albums,playlists&limit=15&term="
                        + urllib.parse.quote(term))
                res = cider.amapi(path).get("results", {})
                return self.send_json({k: [simplify(i) for i in (res.get(k) or {}).get("data", [])]
                                       for k in ("songs", "albums", "playlists")})
            if p.startswith("/api/library/"):
                return self.library(p[len("/api/library/"):], q)
            return self.send_json({"error": "not found"}, 404)
        except Exception as e:
            return self.fail(e)

    def do_POST(self):
        url = urllib.parse.urlparse(self.path)
        p = url.path
        self.text_mode = urllib.parse.parse_qs(url.query).get("fmt", [""])[0] == "text"
        body = self.body_json()
        try:
            name = p[len("/api/"):] if p.startswith("/api/") else ""
            if name in SIMPLE_COMMANDS:
                cider.post(SIMPLE_COMMANDS[name])
                gen = bump() if name in FLUSH_ON else STATE["generation"]
                return self.send_json({"ok": True, "generation": gen})
            if name == "seek":
                cider.post("/playback/seek", {"position": float(body.get("seconds", 0))})
                return self.send_json({"ok": True, "generation": bump()})
            if name == "play_item":
                # MiniAmp sends back the "href" (preferred) or "type"+"id" from a list item.
                if body.get("href"):
                    cider.post("/playback/play-item-href", {"href": body["href"]})
                elif body.get("type") and body.get("id"):
                    cider.post("/playback/play-item", {"type": body["type"], "id": str(body["id"])})
                else:
                    return self.send_json({"error": "need href, or type and id"}, 400)
                return self.send_json({"ok": True, "generation": bump()})
            return self.send_json({"error": "not found"}, 404)
        except Exception as e:
            return self.fail(e)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    # ---- route bodies
    def library(self, rest, q):
        offset = int(q.get("offset", ["0"])[0])
        parts = rest.strip("/").split("/")
        kind = parts[0]
        if kind not in LIBRARY:
            return self.send_json({"error": f"unknown library section '{kind}'"}, 404)
        if len(parts) == 1:  # e.g. /api/library/playlists
            path = f"{LIBRARY[kind]}?limit=50&offset={offset}"
        elif len(parts) == 3 and parts[2] == "tracks" and kind in ("playlists", "albums"):
            item_id = urllib.parse.quote(parts[1])
            path = f"{LIBRARY[kind]}/{item_id}/tracks?limit=100&offset={offset}"
        else:
            return self.send_json({"error": "bad library path"}, 404)
        return self.send_json(simplify_list(cider.amapi(path)))

    def art(self, u):
        host = urllib.parse.urlparse(u).hostname or ""
        if not host.endswith("mzstatic.com"):  # only proxy Apple artwork, never arbitrary URLs
            return self.send_json({"error": "artwork host not allowed"}, 400)
        with urllib.request.urlopen(u, timeout=8) as r:
            data, ctype = r.read(), r.headers.get("Content-Type", "image/jpeg")
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "max-age=86400")
        self.end_headers()
        self.wfile.write(data)

    def serve_file(self, name, ctype):
        with open(os.path.join(HERE, name), "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def stream(self):
        q = broadcaster.add()
        log(f"listener connected from {self.client_address[0]} ({broadcaster.count()} total)")
        try:
            self.send_response(200)
            self.send_header("Content-Type", "audio/mpeg")
            self.send_header("Cache-Control", "no-cache, no-store")
            self.send_header("Connection", "close")
            self.end_headers()
            while True:
                try:
                    chunk = q.get(timeout=10)
                except queue.Empty:
                    continue  # encoder restarting; keep the connection open
                self.wfile.write(chunk)
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            broadcaster.remove(q)
            log(f"listener disconnected ({broadcaster.count()} left)")


def discovery_loop(port):
    """Answers MiniAmp's broadcast so it can find the bridge without an IP address.
    MiniAmp sends "MINIAMP_DISCOVER" to UDP port 8780; we reply "CIDER_BRIDGE <http port>"."""
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(("", port))
    except OSError as e:
        log(f"discovery disabled (UDP {port}: {e})")
        return
    log(f"discovery listening on UDP {port}")
    while True:
        try:
            data, addr = s.recvfrom(256)
            if data.strip() == b"MINIAMP_DISCOVER":
                s.sendto(f"CIDER_BRIDGE {CFG['listen_port']}".encode(), addr)
                log(f"answered discovery from {addr[0]}")
        except Exception as e:
            log("discovery error:", e)
            time.sleep(1)


def main():
    no_audio = "--no-audio" in sys.argv  # API-only mode, for testing without PipeWire
    threading.Thread(target=discovery_loop, args=(CFG["listen_port"],), daemon=True).start()
    audio = None
    if "--test-tone" in sys.argv:
        # developer mode: stream a generated tone instead of Cider (no PipeWire needed)
        def tone():
            cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-re", "-f", "lavfi",
                   "-i", "sine=frequency=440:sample_rate=44100", "-ac", "2",
                   "-c:a", "libmp3lame", "-b:a", CFG["bitrate"], "-f", "mp3", "-flush_packets", "1", "pipe:1"]
            p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
            fd = p.stdout.fileno()
            while True:
                chunk = os.read(fd, 4096)
                if not chunk:
                    break
                broadcaster.publish(chunk)
        threading.Thread(target=tone, daemon=True).start()
        no_audio = True
    if not no_audio:
        audio = Audio(CFG)
        audio.start()
    if not CFG["cider_token"]:
        log("note: no cider_token set — fine only if Cider's API auth is turned off")

    server = ThreadingHTTPServer((CFG["listen_host"], CFG["listen_port"]), Handler)
    server.daemon_threads = True

    def shutdown(*_):
        log("shutting down")
        if audio:
            audio.stop()
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)
    log(f"Cider Bridge listening on http://{CFG['listen_host']}:{CFG['listen_port']}")
    server.serve_forever()


if __name__ == "__main__":
    main()
