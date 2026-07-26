#!/usr/bin/env python3
"""
Claude Usage HTTP Server for ESP32 display.
Reads OAuth token from macOS Keychain, makes a lightweight API call,
parses rate-limit headers, and serves usage JSON on port 8266.

Usage: python3 server.py
"""

import json
import subprocess
import time
import urllib.request
import urllib.error
from http.server import HTTPServer, BaseHTTPRequestHandler

PORT = 8266
CACHE_TTL = 180  # 60s polling tripped the endpoint's rate limit (429) — 3 min is safe
API_URL = "https://api.anthropic.com/api/oauth/usage"

_cache = {"data": None, "ts": 0}


def get_oauth_token():
    raw = subprocess.check_output(
        ["security", "find-generic-password", "-s", "Claude Code-credentials", "-w"],
        text=True,
    ).strip()
    cred = json.loads(raw)
    return cred["claudeAiOauth"]["accessToken"]


def _iso_to_ts(iso):
    from datetime import datetime
    try:
        return int(datetime.fromisoformat(iso).timestamp())
    except (ValueError, TypeError):
        return 0


def fetch_usage(token):
    # GET /api/oauth/usage — same endpoint Claude Code's status line uses.
    # Zero token cost (the old approach burned a 1-token haiku call per refresh).
    req = urllib.request.Request(
        API_URL,
        headers={
            "Authorization": f"Bearer {token}",
            "anthropic-beta": "oauth-2025-04-20",
        },
    )
    with urllib.request.urlopen(req, timeout=15) as resp:
        data = json.loads(resp.read())

    now = int(time.time())
    session_pct = 0
    session_reset = 0
    weekly_pct = 0
    weekly_reset = 0
    fable_pct = -1  # -1 = no scoped bucket; firmware skips the bar
    fable_label = ""
    worst_severity = "normal"

    for lim in data.get("limits", []):
        pct = round(lim.get("percent") or 0)
        reset = _iso_to_ts(lim.get("resets_at"))
        kind = lim.get("kind")
        if kind == "session":
            session_pct, session_reset = pct, reset
        elif kind == "weekly_all":
            weekly_pct, weekly_reset = pct, reset
        elif kind == "weekly_scoped":
            scope = lim.get("scope") or {}
            model = (scope.get("model") or {}).get("display_name") or "Scoped"
            fable_pct, fable_label = pct, model
        sev = lim.get("severity") or "normal"
        if sev != "normal":
            worst_severity = sev

    return {
        "session_pct": session_pct,
        "session_reset_ts": session_reset,
        "session_reset_min": max(0, (session_reset - now) // 60),
        "weekly_pct": weekly_pct,
        "weekly_reset_ts": weekly_reset,
        "weekly_reset_day": time.strftime("%a %H:%M", time.localtime(weekly_reset)),
        "fable_pct": fable_pct,
        "fable_label": fable_label,
        "status": "allowed" if worst_severity == "normal" else worst_severity,
        "ts": now,
    }


def get_usage():
    now = time.time()
    if _cache["data"] and (now - _cache["ts"]) < CACHE_TTL:
        cached = _cache["data"].copy()
        cached["session_reset_min"] = max(0, (cached["session_reset_ts"] - int(now)) // 60)
        cached["cached"] = True
        return cached

    try:
        token = get_oauth_token()
        data = fetch_usage(token)
        data["cached"] = False
        _cache["data"] = data
        _cache["ts"] = now
        return data
    except Exception as e:
        # Upstream failed (e.g. 429): serve last good data instead of an error blob
        if _cache["data"]:
            stale = _cache["data"].copy()
            stale["session_reset_min"] = max(0, (stale["session_reset_ts"] - int(now)) // 60)
            stale["cached"] = True
            stale["stale"] = True
            return stale
        return {"error": str(e), "ts": int(now)}


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path in ("/", "/usage"):
            data = get_usage()
            body = json.dumps(data).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.write(body) if hasattr(self, 'write') else self.wfile.write(body)
        else:
            self.send_error(404)

    def log_message(self, fmt, *args):
        ts = time.strftime("%H:%M:%S")
        print(f"[{ts}] {fmt % args}")


if __name__ == "__main__":
    print(f"Claude Usage Server starting on port {PORT}...")
    try:
        data = get_usage()
        if "error" not in data:
            print(f"  Session: {data['session_pct']}% (resets in {data['session_reset_min']}m)")
            print(f"  Weekly:  {data['weekly_pct']}% (resets {data['weekly_reset_day']})")
        else:
            print(f"  Warning: {data['error']}")
    except Exception as e:
        print(f"  Initial fetch failed: {e}")

    server = HTTPServer(("0.0.0.0", PORT), Handler)
    print(f"Serving on http://0.0.0.0:{PORT}/usage")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.")
        server.server_close()
