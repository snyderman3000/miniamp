# Cider Bridge

Streams Apple Music from Cider on a Linux PC (PipeWire) to MiniAmp on the Miyoo Mini Plus, and gives MiniAmp a simple API for browsing and control.

```
 Pop!_OS                                             Miyoo Mini Plus
 ┌───────────┐  audio   ┌──────────────┐  MP3 stream  ┌─────────┐
 │  Cider    │ ───────► │ cider_bridge │ ───────────► │ MiniAmp │ ─► headphones
 │ (logged   │ ◄─────── │  :8780       │ ◄─────────── │         │
 │  in)      │  RPC API └──────────────┘  JSON API    └─────────┘
 └───────────┘ :10767 (localhost only)
```

## How it works

- **Audio routing.** The bridge creates a virtual PipeWire output called `Cider_Bridge` and moves Cider's audio into it every couple of seconds. Your PC speakers stay quiet, and other apps still play normally. When the bridge stops, the virtual output is removed and Cider falls back to your speakers.
- **Encoding.** ffmpeg records that virtual output and encodes it to 192k MP3. Every listener connects to the live point. A slow listener drops old audio instead of falling behind.
- **Control.** Cider's API only listens on localhost and needs a token. The bridge holds the token, listens on your LAN, and translates simple calls from MiniAmp.
- **Discovery.** The bridge answers a `MINIAMP_DISCOVER` broadcast on UDP port 8780 with `CIDER_BRIDGE <port>`, so MiniAmp finds it without an IP address. If you run a firewall (Pop!_OS doesn't by default), allow TCP and UDP 8780: `sudo ufw allow 8780`.

## Setup

1. Install Cider 2 (cider.sh) and sign in to Apple Music.
2. In Cider, open Settings → Connectivity → Manage External Application Access, then generate a token.
3. Run:
   ```bash
   cd bridge
   chmod +x install.sh
   ./install.sh
   ```
   Paste the token when asked.
4. Turn off automatic suspend in Pop!_OS power settings. If the PC sleeps, the music stops.
5. Open `http://<pc-ip>:8780/` on your phone, tap **Listen here**, then pick something under Playlists.

Useful commands:
```bash
journalctl --user -u cider-bridge -f       # live logs
systemctl --user restart cider-bridge      # after editing config.json
systemctl --user disable --now cider-bridge
```

Config is at `~/.local/share/cider-bridge/config.json`.

## API (for MiniAmp)

All responses are JSON unless noted. Add `?fmt=text` to get the plain-text form MiniAmp uses
(`key=value` lines for status and commands; tab-separated `I` lines for lists, plus `N <offset>`
when there's another page, and `S <section>` headers in search results).
Base URL: `http://<pc-ip>:8780`

| Method | Path | Notes |
|---|---|---|
| GET | `/stream.mp3` | Continuous MP3 audio (never ends). |
| GET | `/api/status` | `cider`, `playing`, `track{name,artist,album,duration_ms,position_s,artwork}`, `shuffle`, `repeat`, `generation`, `listeners` |
| POST | `/api/play` `/api/pause` `/api/playpause` | |
| POST | `/api/next` `/api/previous` | Bumps `generation`. |
| POST | `/api/shuffle` `/api/repeat` | Toggles. |
| POST | `/api/seek` | `{"seconds": 42}`. Bumps `generation`. |
| POST | `/api/play_item` | `{"href": ...}` from any list item, or `{"type","id"}`. Bumps `generation`. |
| GET | `/api/library/{playlists,albums,songs,artists}?offset=0` | `{items:[...], next_offset?}` |
| GET | `/api/library/playlists/<id>/tracks` | Same for `albums/<id>/tracks`. |
| GET | `/api/library/recently-added` | New in your library (pages of 25). |
| GET | `/api/recent` | Recently played (pages of 10). |
| GET | `/api/search?q=...` | Catalog search: `{songs, albums, artists, playlists, sections}` |
| GET | `/api/charts/{songs,albums,playlists}` | Apple Music Top Charts for your storefront. |
| GET | `/api/recommendations` | Your "Made for You" shelves: `{sections:[{title, items}]}` |
| GET | `/api/catalog/{albums,playlists}/<id>/tracks` | Tracks of a catalog album or playlist. |
| GET | `/api/catalog/artists/<id>/albums` | An artist's albums. |
| GET | `/api/art?u=...` | JPEG artwork over plain http. Item `artwork` fields already point here. |

Each list item looks like this:
`{id, type, href, name, artist, album, duration_ms, track_count, artwork}`.

**Flushing on skip.** MiniAmp keeps a short audio buffer, so after a skip it would otherwise play a second or two of the old track. Every command that changes the track returns a new `generation`, which also shows up in `/api/status`. When MiniAmp sees it change, it drops its buffer and reconnects to `/stream.mp3`. That reconnect lands on the live point, so the new track starts right away.

## Testing without Cider

`python3 cider_bridge.py --test-tone` streams a 440 Hz tone instead of Cider's audio, and
`--no-audio` runs the API only.

## Known limits

- Playing a playlist starts at the top. There is no "start at track 7" yet.
- LAN only, with no auth on the bridge. Keep it on your home network.
- Apple's API has no official "new releases" list, so What's new uses Top Charts, your
  recommendations and Apple's New Music Daily playlist.
- Streamed audio is for your own listening. Don't share the bridge publicly.
