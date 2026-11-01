#!/usr/bin/env bash
# Installs Cider Bridge as a systemd *user* service on Pop!_OS.
# A user service (not system-wide) is used because it needs to talk to your
# desktop session's PipeWire, which only exists while you're logged in.
set -euo pipefail

DEST="$HOME/.local/share/cider-bridge"
UNIT_DIR="$HOME/.config/systemd/user"

echo "==> Checking dependencies"
missing=()
command -v python3 >/dev/null || missing+=(python3)
command -v ffmpeg  >/dev/null || missing+=(ffmpeg)
{ command -v pactl >/dev/null && command -v parec >/dev/null; } || missing+=(pulseaudio-utils)
if ((${#missing[@]})); then
  echo "Installing: ${missing[*]}"
  sudo apt install -y "${missing[@]}"
fi
if ! pactl -f json info >/dev/null 2>&1; then
  echo "!! 'pactl -f json' failed. Is PipeWire running in this session?"; exit 1
fi
# Capture the list first: piping straight into "grep -q" can make ffmpeg exit
# early on a broken pipe, which pipefail would misread as a failed check.
ENCODERS=$(ffmpeg -hide_banner -encoders 2>/dev/null || true)
if ! grep -q libmp3lame <<<"$ENCODERS"; then
  echo "!! This ffmpeg build has no MP3 encoder (libmp3lame)."
  echo "   Try: sudo apt install ffmpeg libavcodec-extra"; exit 1
fi

echo "==> Copying files to $DEST"
mkdir -p "$DEST"
cp cider_bridge.py test_page.html "$DEST/"
if [ ! -f "$DEST/config.json" ]; then
  cp config.example.json "$DEST/config.json"
  echo "   Created $DEST/config.json"
fi

TOKEN=$(python3 -c "import json;print(json.load(open('$DEST/config.json')).get('cider_token',''))")
if [ -z "$TOKEN" ]; then
  echo
  echo "   Paste your Cider API token (Cider > Settings > Connectivity >"
  echo "   Manage External Application Access). Leave blank to skip."
  read -r -p "   Token: " TOKEN
  if [ -n "$TOKEN" ]; then
    python3 - "$DEST/config.json" "$TOKEN" <<'PY'
import json, sys
p, t = sys.argv[1], sys.argv[2]
c = json.load(open(p)); c["cider_token"] = t
json.dump(c, open(p, "w"), indent=2)
PY
  fi
fi

echo "==> Installing user service"
mkdir -p "$UNIT_DIR"
cp cider-bridge.service "$UNIT_DIR/"
systemctl --user daemon-reload
systemctl --user enable cider-bridge.service
systemctl --user restart cider-bridge.service   # picks up new files when updating

IP=$(hostname -I | awk '{print $1}')
echo
echo "Done. Cider Bridge is running."
echo "  Test page:  http://$IP:8780/   (open it on your phone)"
echo "  Logs:       journalctl --user -u cider-bridge -f"
