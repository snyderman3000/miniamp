"""
Internet radio for Cider Bridge.

Stations come from Radio Browser (https://www.radio-browser.info), a free,
community-run directory. Whatever format a station uses (MP3 or AAC, http or
https, Icecast/Shoutcast or HLS), ffmpeg turns it into the same MP3 stream
MiniAmp already plays for Apple Music. For Icecast/Shoutcast stations the
bridge also reads the song titles the station sends (ICY metadata).

Standard library only (plus the ffmpeg the bridge already needs).
"""
import json
import os
import random
import socket
import subprocess
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

USER_AGENT = "MiniAmp-CiderBridge/0.4"
RB_HOST = os.environ.get("RADIO_BROWSER_HOST", "")   # override for testing, e.g. "http://127.0.0.1:8790"
RB_FALLBACK = ["de1.api.radio-browser.info", "de2.api.radio-browser.info", "fi1.api.radio-browser.info"]

# Shown under Picks. Each is looked up in Radio Browser by name (the most
# listened-to match wins), so stream addresses stay current.
PICKS = [
    ("KEXP", "KEXP 90.3"),
    ("WXPN", "WXPN"),            # Philadelphia; home of World Cafe
    ("WFUV", "WFUV"),
    ("KCRW Eclectic 24", "KCRW Eclectic"),
    ("WWOZ", "WWOZ"),
    ("SomaFM Groove Salad", "Groove Salad"),
    ("WKNO", "WKNO"),
    ("WEVL", "WEVL"),
]

GENRES = ["indie", "rock", "jazz", "classical", "blues", "country", "folk", "electronic", "ambient",
          "hip hop", "soul", "oldies", "80s", "public radio", "news", "talk"]

ALLOWED_SCHEMES = ("http", "https")
FFMPEG_PROTOCOLS = "http,https,tcp,tls,crypto,httpproxy"


def _log(*a):
    print(time.strftime("%H:%M:%S"), "[radio]", *a, flush=True)


# --------------------------------------------------------------------------
# Radio Browser
# --------------------------------------------------------------------------
class RadioBrowser:
    def __init__(self):
        self.servers = []
        self.lock = threading.Lock()

    def _pick_servers(self):
        """Radio Browser asks clients to look up all.api.radio-browser.info and pick a random server."""
        names = []
        try:
            for info in socket.getaddrinfo("all.api.radio-browser.info", 443, proto=socket.IPPROTO_TCP):
                ip = info[4][0]
                try:
                    name = socket.gethostbyaddr(ip)[0]
                    if name.endswith("radio-browser.info") and name not in names:
                        names.append(name)
                except OSError:
                    pass
        except OSError as e:
            _log("server lookup failed:", e)
        random.shuffle(names)
        for n in RB_FALLBACK:
            if n not in names:
                names.append(n)
        return names

    def get(self, path, params=None):
        q = ("?" + urllib.parse.urlencode(params)) if params else ""
        if RB_HOST:
            bases = [RB_HOST]
        else:
            with self.lock:
                if not self.servers:
                    self.servers = self._pick_servers()
                servers = list(self.servers)
            bases = ["https://" + s for s in servers]
        last = None
        for i, base in enumerate(bases[:3]):
            try:
                req = urllib.request.Request(base + path + q, headers={"User-Agent": USER_AGENT})
                with urllib.request.urlopen(req, timeout=8) as r:
                    data = json.loads(r.read() or b"null")
                if i and not RB_HOST:   # remember the server that worked
                    with self.lock:
                        self.servers.remove(bases[i][8:])
                        self.servers.insert(0, bases[i][8:])
                return data
            except Exception as e:
                last = e
                _log(f"{base}{path} failed: {e}")
        raise RuntimeError(f"Radio Browser isn't answering ({last})")

    def click(self, uuid):
        """Radio Browser asks apps to report plays; it's how its popularity lists work."""
        def go():
            try:
                self.get("/json/url/" + urllib.parse.quote(uuid))
            except Exception:
                pass
        threading.Thread(target=go, daemon=True).start()


# --------------------------------------------------------------------------
# Stations
# --------------------------------------------------------------------------
def location(st):
    parts = [st.get("state") or "", st.get("countrycode") or st.get("country") or ""]
    return ", ".join(p for p in parts if p)


def describe(st):
    tags = [t.strip() for t in (st.get("tags") or "").split(",") if t.strip()][:2]
    bits = [location(st)] + ([", ".join(tags)] if tags else [])
    return " · ".join(b for b in bits if b)


def fmt(st):
    codec = (st.get("codec") or "").upper().replace("UNKNOWN", "")
    if st.get("hls") in (1, True, "1") and "HLS" not in codec:
        codec = (codec + " HLS").strip()
    br = st.get("bitrate") or 0
    return f"{codec} {br}k".strip() if br else codec


class Radio:
    def __init__(self, cfg, data_dir):
        self.cfg = cfg
        self.rb = RadioBrowser()
        self.cache = {}                     # uuid -> station dict
        self.cache_lock = threading.Lock()
        self.fav_path = os.path.join(data_dir, "radio_favorites.json")
        self.favs = self._load_favs()
        self.picks = None
        self.picks_at = 0
        self.session = None                 # the station playing now
        self.session_lock = threading.Lock()

    # ---- favorites (kept on the PC, next to the bridge's config)
    def _load_favs(self):
        try:
            with open(self.fav_path) as f:
                favs = json.load(f)
            return [s for s in favs if isinstance(s, dict) and s.get("stationuuid")]
        except (OSError, ValueError):
            return []

    def _save_favs(self):
        tmp = self.fav_path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(self.favs, f, indent=1)
        os.replace(tmp, self.fav_path)

    def is_fav(self, uuid):
        return any(s["stationuuid"] == uuid for s in self.favs)

    def toggle_fav(self, uuid):
        if self.is_fav(uuid):
            self.favs = [s for s in self.favs if s["stationuuid"] != uuid]
            on = False
        else:
            st = self.station(uuid)
            if not st:
                raise RuntimeError("unknown station")
            keep = {k: st.get(k) for k in ("stationuuid", "name", "url", "url_resolved", "favicon", "tags",
                                          "country", "countrycode", "state", "codec", "bitrate", "hls", "homepage")}
            self.favs.append(keep)
            on = True
        self._save_favs()
        return on

    # ---- lookups
    def _remember(self, stations):
        with self.cache_lock:
            for st in stations:
                if st.get("stationuuid"):
                    self.cache[st["stationuuid"]] = st

    def station(self, uuid):
        with self.cache_lock:
            st = self.cache.get(uuid)
        if st:
            return st
        for s in self.favs:
            if s["stationuuid"] == uuid:
                return s
        res = self.rb.get("/json/stations/byuuid/" + urllib.parse.quote(uuid))
        if res:
            self._remember(res)
            return res[0]
        return None

    def _search(self, **params):
        base = {"hidebroken": "true", "order": "clickcount", "reverse": "true", "limit": 50}
        base.update(params)
        res = self.rb.get("/json/stations/search", base) or []
        # stations are often listed more than once; keep the first of each name + stream
        seen, out = set(), []
        for st in res:
            key = ((st.get("name") or "").strip().lower(), st.get("url_resolved") or st.get("url"))
            if key in seen or not st.get("name"):
                continue
            seen.add(key)
            out.append(st)
        self._remember(out)
        return out

    def search(self, term, offset=0):
        return self._search(name=term, offset=offset)

    def popular(self, offset=0):
        return self._search(offset=offset)

    def by_tag(self, tag, offset=0):
        return self._search(tag=tag, tagExact="true", offset=offset)

    def get_picks(self):
        if self.picks is not None and time.time() - self.picks_at < 6 * 3600:
            return self.picks
        found = [None] * len(PICKS)

        def look(i, term):
            try:
                res = self._search(name=term, limit=5)
                if res:
                    found[i] = res[0]
            except Exception as e:
                _log(f"pick '{term}' failed: {e}")
        threads = [threading.Thread(target=look, args=(i, t)) for i, (_, t) in enumerate(PICKS)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(15)
        picks = [s for s in found if s]
        if picks:
            self.picks, self.picks_at = picks, time.time()
        return picks

    def item(self, st):
        """A station in the list format MiniAmp reads."""
        uuid = st["stationuuid"]
        return {
            "id": uuid,
            "type": "radio-fav" if self.is_fav(uuid) else "radio",
            "href": "",
            "name": (st.get("name") or "").strip(),
            "artist": describe(st),
            "album": fmt(st),
            "duration_ms": 0,
            "track_count": 0,
            "artwork": f"/api/radio/logo/{urllib.parse.quote(uuid)}" if st.get("favicon") else None,
        }

    def items(self, stations, offset=None):
        out = {"items": [self.item(s) for s in stations]}
        if offset is not None and len(stations) >= 40:
            out["next_offset"] = offset + 50
        return out

    # ---- logo: any image format, any size -> small JPEG for the handheld
    def logo(self, uuid):
        st = self.station(uuid)
        url = (st or {}).get("favicon") or ""
        if urllib.parse.urlparse(url).scheme not in ALLOWED_SCHEMES:
            return None
        try:
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(req, timeout=8) as r:
                raw = r.read(2 << 20)
        except Exception as e:
            _log(f"logo for {uuid} failed: {e}")
            return None
        size = self.cfg.get("artwork_size", 160)
        p = subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", "pipe:0", "-frames:v", "1",
             "-vf", f"scale={size}:{size}:force_original_aspect_ratio=decrease,"
                    f"pad={size}:{size}:(ow-iw)/2:(oh-ih)/2:color=white",
             "-f", "mjpeg", "pipe:1"],
            input=raw, capture_output=True, timeout=15)
        return p.stdout if p.returncode == 0 and p.stdout else None

    # ---- playing
    def status(self):
        with self.session_lock:
            s = self.session
        if not s:
            return {"active": 0}
        return s.status()

    def open_stream(self, uuid):
        st = self.station(uuid)
        if not st:
            raise RuntimeError("unknown station")
        sess = Session(st, self.cfg["bitrate"])
        with self.session_lock:
            old, self.session = self.session, sess
        if old:
            old.stop()
        sess.start()
        self.rb.click(uuid)
        return sess


class Session:
    """One station being fetched and turned into MP3 for one listener."""

    def __init__(self, st, bitrate):
        self.st = st
        self.bitrate = bitrate
        self.url = st.get("url_resolved") or st.get("url") or ""
        self.title = ""
        self.error = ""
        self.started = time.time()
        self.stopped = False
        self.ffmpeg = None
        self.src = None
        self.icy = False

    def status(self):
        st = self.st
        return {"active": 0 if self.stopped else 1, "uuid": st["stationuuid"], "name": (st.get("name") or "").strip(),
                "title": self.title, "where": describe(st), "format": fmt(st), "error": self.error,
                "titles": 1 if self.icy else 0, "seconds": int(time.time() - self.started),
                "artwork": f"/api/radio/logo/{urllib.parse.quote(st['stationuuid'])}" if st.get("favicon") else ""}

    def _encoder(self, src_args):
        cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", *src_args,
               "-vn", "-ac", "2", "-ar", "44100", "-c:a", "libmp3lame", "-b:a", self.bitrate,
               "-f", "mp3", "-flush_packets", "1", "pipe:1"]
        return cmd

    def start(self):
        if urllib.parse.urlparse(self.url).scheme not in ALLOWED_SCHEMES:
            self.error = "This station's address isn't a web stream"
            raise RuntimeError(self.error)
        hls = self.st.get("hls") in (1, True, "1") or ".m3u8" in self.url.lower()
        if not hls:
            try:
                self._start_icy()
                return
            except urllib.error.HTTPError as e:
                # the station's server answered and said no: ffmpeg won't do better
                self.error = f"The station isn't available right now ({e.code})"
                raise RuntimeError(self.error)
            except Exception as e:
                _log(f"reading {self.url} directly failed ({e}); letting ffmpeg fetch it")
        self._start_direct(hls)

    def _start_direct(self, hls):
        """ffmpeg fetches the station itself (HLS, or servers Python can't talk to). No song titles."""
        # HLS: ffmpeg would otherwise download the last few segments at once;
        # -re keeps it at real time so the handheld isn't flooded
        src = (["-re"] if hls else []) + ["-protocol_whitelist", FFMPEG_PROTOCOLS, "-user_agent", USER_AGENT,
               "-reconnect", "1", "-reconnect_streamed", "1", "-reconnect_delay_max", "5",
               "-i", self.url]
        self.ffmpeg = subprocess.Popen(self._encoder(src), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE)
        _log(f"playing {self.st.get('name')!r} through ffmpeg")

    def _start_icy(self):
        """Python fetches an Icecast/Shoutcast stream, asking for song titles, and feeds the audio to ffmpeg."""
        req = urllib.request.Request(self.url, headers={"User-Agent": USER_AGENT, "Icy-MetaData": "1"})
        self.src = urllib.request.urlopen(req, timeout=10)
        ctype = (self.src.headers.get("Content-Type") or "").lower()
        if "mpegurl" in ctype or "x-scpls" in ctype or "text/html" in ctype:
            self.src.close()
            raise RuntimeError(f"not an audio stream ({ctype})")
        try:
            metaint = int(self.src.headers.get("icy-metaint") or 0)
        except ValueError:
            metaint = 0
        self.icy = metaint > 0
        self.ffmpeg = subprocess.Popen(self._encoder(["-i", "pipe:0"]), stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        threading.Thread(target=self._feed, args=(metaint,), daemon=True).start()
        _log(f"playing {self.st.get('name')!r} ({ctype or 'unknown type'}{', with song titles' if self.icy else ''})")

    def _read(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.src.read(n - len(buf))
            if not chunk:
                raise EOFError
            buf += chunk
        return buf

    def _feed(self, metaint):
        try:
            while not self.stopped:
                if metaint:
                    self.ffmpeg.stdin.write(self._read(metaint))
                    n = self._read(1)[0] * 16
                    if n:
                        self._title(self._read(n))
                else:
                    chunk = self.src.read(4096)
                    if not chunk:
                        break
                    self.ffmpeg.stdin.write(chunk)
        except (EOFError, BrokenPipeError, OSError, ValueError) as e:
            if not self.stopped:
                _log(f"station stream ended: {e!r}")
                self.error = "The station stopped sending"
        finally:
            try:
                self.ffmpeg.stdin.close()
            except Exception:
                pass

    def _title(self, raw):
        text = raw.rstrip(b"\0").decode("utf-8", "replace")
        if "StreamTitle='" not in text:
            return
        t = text.split("StreamTitle='", 1)[1]
        end = t.find("';")
        t = (t[:end] if end >= 0 else t.rstrip("'")).strip()
        if t != self.title:
            self.title = t
            _log(f"now playing: {t}")

    def chunks(self, first_timeout=20):
        """Yields MP3 data. The first chunk is waited for so a dead station can be reported."""
        fd = self.ffmpeg.stdout.fileno()
        import select
        r, _, _ = select.select([fd], [], [], first_timeout)
        if not r:
            self.error = self.error or "The station isn't answering"
            return
        while not self.stopped:
            chunk = os.read(fd, 4096)
            if not chunk:
                if not self.error:
                    self.error = ("The station isn't answering" if time.time() - self.started < 5
                                  else "The station stopped sending")
                break
            yield chunk

    def stop(self):
        self.stopped = True
        for closer in (lambda: self.src and self.src.close(),
                       lambda: self.ffmpeg and self.ffmpeg.poll() is None and self.ffmpeg.kill()):
            try:
                closer()
            except Exception:
                pass
