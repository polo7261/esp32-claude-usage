#!/usr/bin/env python3
"""
Claude Usage HTTP Server for ESP32 display.
Reads OAuth token from macOS Keychain, makes a lightweight API call,
parses rate-limit headers, and serves usage JSON on port 8266.

Usage: BIND_HOST=<lan-ip> python3 server.py   (defaults to loopback only)
"""

import ipaddress
import json
import os
import ssl
import subprocess
import time
import urllib.request
import urllib.error
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

PORT = 8266
CACHE_TTL = 180  # 60s polling tripped the endpoint's rate limit (429) — 3 min is safe
API_URL = "https://api.anthropic.com/api/oauth/usage"

# This endpoint has no authentication. Binding 0.0.0.0 on a machine that holds a
# public IP (and macOS ships with its firewall off) publishes it to the internet,
# so default to loopback and make the LAN bind an explicit, deliberate choice.
BIND_HOST = os.environ.get("BIND_HOST", "127.0.0.1")
ALLOW_PUBLIC_BIND = os.environ.get("ALLOW_PUBLIC_BIND") == "1"

# Layout preview. Model-scoped weekly caps are a Max-plan feature, so on Pro
# the API returns null for every seven_day_<model> bucket and there is nothing
# real to draw. Set FAKE_MODELS=1 (or a comma-separated name list) to inject
# synthetic bars; the payload carries demo=true so the display labels them.
FAKE_MODELS = os.environ.get("FAKE_MODELS", "")

_cache = {"data": None, "ts": 0}


def private_ipv4_addresses():
    """Private IPv4 addresses on this machine, offered as BIND_HOST candidates."""
    try:
        out = subprocess.check_output(["ifconfig"], text=True)
    except (OSError, subprocess.CalledProcessError):
        return []

    found = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0] == "inet":
            try:
                addr = ipaddress.ip_address(parts[1])
            except ValueError:
                continue
            if addr.is_private and not addr.is_loopback:
                found.append(str(addr))
    return found


def is_loopback_host(host):
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return host == "localhost"


def check_bind_host(host):
    """Reject a publicly reachable bind unless the operator opted in explicitly."""
    if host in ("0.0.0.0", "::"):
        addr = None  # every interface, including any public one
    else:
        try:
            addr = ipaddress.ip_address(host)
        except ValueError:
            return  # hostname — leave resolution to the socket layer

    if ALLOW_PUBLIC_BIND or (addr is not None and (addr.is_private or addr.is_loopback)):
        return

    where = "every interface, public ones included" if addr is None else "a public address"
    candidates = ", ".join(private_ipv4_addresses()) or "(none found)"
    raise SystemExit(
        f"Refusing to bind {host} — that is {where}, and this endpoint has no auth.\n"
        f"  Bind a LAN address instead:  BIND_HOST=<ip> python3 server.py\n"
        f"  Candidates on this machine:  {candidates}\n"
        f"  To override anyway:          ALLOW_PUBLIC_BIND=1 python3 server.py"
    )


def build_ssl_context():
    """Default trust store, topped up from certifi when it turns out to be empty.

    Homebrew's Python links against an OpenSSL whose CA directory is often left
    unpopulated, which fails every HTTPS call with CERTIFICATE_VERIFY_FAILED.
    certifi is additive here — verification stays on either way.
    """
    ctx = ssl.create_default_context()
    if ctx.cert_store_stats()["x509_ca"] > 0:
        return ctx

    try:
        import certifi
    except ImportError:
        print("  Warning: no CA certificates found and certifi is not installed;")
        print("           HTTPS will fail. Fix: brew install ca-certificates")
        return ctx

    ctx.load_verify_locations(certifi.where())
    return ctx


_SSL_CONTEXT = build_ssl_context()


CCUSAGE_BIN = os.environ.get("CCUSAGE_BIN", "ccusage")
CCUSAGE_TTL = 120  # it rescans every transcript on disk — not free, cache it
_cc_cache = {"data": None, "ts": 0}


def pretty_model(name):
    """'claude-haiku-4-5-20251001' -> 'Haiku 4.5'. Non-Claude names pass through."""
    # ccusage tags third-party agents as "[openclaw] deepseek-v4-pro" — drop the
    # tag, not the model, or two entries truncate to the same 14-char label.
    if name.startswith("["):
        name = name.split("]", 1)[-1].strip()
    if not name.startswith("claude-"):
        return name[:14]
    parts = name[len("claude-"):].split("-")
    if parts and len(parts[-1]) == 8 and parts[-1].isdigit():
        parts = parts[:-1]  # drop the release date
    version = ".".join(p for p in parts[1:] if p.isdigit())
    return f"{parts[0].capitalize()} {version}".strip()


def run_ccusage(args):
    out = subprocess.run([CCUSAGE_BIN] + args, capture_output=True, text=True, timeout=90)
    if out.returncode != 0:
        raise RuntimeError((out.stderr or "ccusage failed").strip()[:120])
    return json.loads(out.stdout)


def _fmt_tokens(n):
    if n >= 1_000_000:
        return f"{n / 1_000_000:.1f}M"
    if n >= 1_000:
        return f"{n / 1_000:.0f}K"
    return str(n)


def token_usage():
    """Per-model token counts from Claude Code's local transcripts, via ccusage.

    Deliberately a second, independent source. The usage API above answers
    "how much of the plan limit is gone"; this answers "what was actually
    consumed, split by model" — which the API never reports, and which works
    on Pro where every model-scoped bucket comes back null.

    The dollar figures are list-price valuations of those tokens, NOT what a
    subscription is billed. Anything rendering them must say so.
    """
    now = time.time()
    if _cc_cache["data"] and (now - _cc_cache["ts"]) < CCUSAGE_TTL:
        return _cc_cache["data"]

    try:
        blocks = run_ccusage(["blocks", "--json", "--active"]).get("blocks", [])
        active = blocks[0] if blocks and blocks[0].get("isActive") else {}
        weeks = run_ccusage(["weekly", "--json"]).get("weekly", [])
        week = weeks[-1] if weeks else {}
    except Exception as e:
        out = {"cc_ok": False, "cc_error": str(e), "tok_models": []}
        _cc_cache.update(data=out, ts=now)
        return out

    week_tok = week.get("totalTokens") or 0
    models = []
    for mb in sorted(week.get("modelBreakdowns", []), key=lambda m: -(m.get("cost") or 0)):
        tok = sum(
            mb.get(k) or 0
            for k in ("inputTokens", "outputTokens", "cacheCreationTokens", "cacheReadTokens")
        )
        models.append({
            "name": pretty_model(mb.get("modelName") or "?"),
            # Share of the week's tokens — the only 0-100 figure available here,
            # since local accounting has no limit to be a percentage of.
            "pct": round(100 * tok / week_tok) if week_tok else 0,
            "tok": _fmt_tokens(tok),
            "cost": round(mb.get("cost") or 0, 2),
        })

    out = {
        "cc_ok": True,
        "tok_active": _fmt_tokens(active.get("totalTokens") or 0),
        "cost_active": round(active.get("costUSD") or 0, 2),
        "burn_hr": round((active.get("burnRate") or {}).get("costPerHour") or 0, 1),
        "tok_week": _fmt_tokens(week_tok),
        "cost_week": round(week.get("totalCost") or 0, 2),
        "tok_models": models[:4],
    }
    _cc_cache.update(data=out, ts=now)
    return out


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


_MODEL_LABELS = {
    "opus": "Opus",
    "sonnet": "Sonnet",
    "haiku": "Haiku",
    "fable": "Fable",
    "cowork": "Cowork",
    "omelette": "Omelette",
    "oauth_apps": "OAuth Apps",
}


def _model_entry(name, pct, reset_ts):
    return {
        "name": name,
        "pct": round(pct or 0),
        "reset_day": time.strftime("%a %H:%M", time.localtime(reset_ts)) if reset_ts else "",
    }


def scoped_models(data):
    """Every model-scoped 7-day bucket this account actually has.

    Two sources overlap. limits[] carries a proper display_name but only lists
    buckets the server considers active; the top-level seven_day_<model> keys
    cover the rest and are null when the plan has no such cap. limits[] wins.
    """
    models = []
    seen = set()

    for lim in data.get("limits", []):
        if lim.get("kind") != "weekly_scoped":
            continue
        scope = lim.get("scope") or {}
        name = (scope.get("model") or {}).get("display_name") or "Scoped"
        models.append(_model_entry(name, lim.get("percent"), _iso_to_ts(lim.get("resets_at"))))
        seen.add(name.lower())

    for key, bucket in data.items():
        # "seven_day" itself is the all-model weekly limit — the trailing
        # underscore in the prefix is what keeps it out of the model list.
        if not key.startswith("seven_day_") or not isinstance(bucket, dict):
            continue
        label = _MODEL_LABELS.get(
            key[len("seven_day_"):], key[len("seven_day_"):].replace("_", " ").title()
        )
        if label.lower() in seen:
            continue
        models.append(_model_entry(label, bucket.get("utilization"), _iso_to_ts(bucket.get("resets_at"))))
        seen.add(label.lower())

    return models


def demo_models(reset_ts):
    """Synthetic buckets for FAKE_MODELS — layout and colour preview only.

    Percentages are spread across the firmware's three colour bands (accent /
    yellow at 75 / red at 90) so one glance checks the whole palette.
    """
    names = [n.strip() for n in FAKE_MODELS.split(",") if n.strip() and n.strip() != "1"]
    if not names:
        names = ["Opus 5", "Fable 5", "Sonnet 5"]
    percents = [32, 78, 96]
    return [
        _model_entry(name, percents[i % len(percents)], reset_ts)
        for i, name in enumerate(names)
    ]


def spend_summary(data):
    """Extra-usage credits. The API exposes no token counts anywhere, so this
    dollar figure is the only consumption number available to display."""
    spend = data.get("spend") or {}
    used = spend.get("used") or {}
    limit = spend.get("limit") or {}

    def amount(box):
        minor = box.get("amount_minor")
        return None if minor is None else minor / (10 ** (box.get("exponent") or 0))

    return {
        "spend_enabled": bool(spend.get("enabled")),
        "spend_pct": round(spend.get("percent") or 0),
        "spend_used": amount(used),
        "spend_limit": amount(limit),
        "spend_cur": used.get("currency") or limit.get("currency") or "",
    }


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
    with urllib.request.urlopen(req, timeout=15, context=_SSL_CONTEXT) as resp:
        data = json.loads(resp.read())

    now = int(time.time())
    session_pct = 0
    session_reset = 0
    weekly_pct = 0
    weekly_reset = 0
    worst_severity = "normal"

    for lim in data.get("limits", []):
        pct = round(lim.get("percent") or 0)
        reset = _iso_to_ts(lim.get("resets_at"))
        kind = lim.get("kind")
        if kind == "session":
            session_pct, session_reset = pct, reset
        elif kind == "weekly_all":
            weekly_pct, weekly_reset = pct, reset
        sev = lim.get("severity") or "normal"
        if sev != "normal":
            worst_severity = sev

    models = scoped_models(data)
    demo = bool(FAKE_MODELS) and not models
    if demo:
        models = demo_models(weekly_reset)

    out = {
        "demo": demo,
        "session_pct": session_pct,
        "session_reset_ts": session_reset,
        "session_reset_min": max(0, (session_reset - now) // 60),
        "weekly_pct": weekly_pct,
        "weekly_reset_ts": weekly_reset,
        "weekly_reset_day": time.strftime("%a %H:%M", time.localtime(weekly_reset)),
        "models": models,
        # Kept for firmware built before models[] existed: first model, or -1.
        "fable_pct": models[0]["pct"] if models else -1,
        "fable_label": models[0]["name"] if models else "",
        "status": "allowed" if worst_severity == "normal" else worst_severity,
        "ts": now,
    }
    out.update(spend_summary(data))
    return out


def get_usage():
    """Plan limits (API) plus local token accounting (ccusage), merged.

    Two independent sources with independent caches — a ccusage failure must
    not cost us the limit percentages, and vice versa.
    """
    data = api_usage()
    data.update(token_usage())
    return data


def api_usage():
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
        # Copy: the caller merges ccusage fields in, which must not land
        # in the cached API payload and go stale there.
        return data.copy()
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
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_error(404)

    def log_message(self, fmt, *args):
        ts = time.strftime("%H:%M:%S")
        print(f"[{ts}] {fmt % args}")


if __name__ == "__main__":
    check_bind_host(BIND_HOST)

    print(f"Claude Usage Server starting on port {PORT}...")
    try:
        data = get_usage()
        if "error" not in data:
            print(f"  Session: {data['session_pct']}% (resets in {data['session_reset_min']}m)")
            print(f"  Weekly:  {data['weekly_pct']}% (resets {data['weekly_reset_day']})")
            for m in data["models"]:
                print(f"  {m['name']} (7d): {m['pct']}%")
            if data["spend_enabled"]:
                print(f"  Credits: {data['spend_pct']}% "
                      f"({data['spend_used']:.2f}/{data['spend_limit']:.2f} {data['spend_cur']})")
            if data.get("cc_ok"):
                print(f"  Tokens:  {data['tok_active']} this 5h "
                      f"(${data['burn_hr']}/hr) | {data['tok_week']} this week")
                for m in data["tok_models"]:
                    print(f"    {m['name']:<12} {m['pct']:>3}%  {m['tok']:>7}  ${m['cost']}")
            else:
                print(f"  ccusage unavailable: {data.get('cc_error')}")
        else:
            print(f"  Warning: {data['error']}")
    except Exception as e:
        print(f"  Initial fetch failed: {e}")

    # Threaded: a single ESP32 stalling mid-request would otherwise wedge the server.
    server = ThreadingHTTPServer((BIND_HOST, PORT), Handler)
    print(f"Serving on http://{BIND_HOST}:{PORT}/usage")
    if is_loopback_host(BIND_HOST):
        candidates = ", ".join(private_ipv4_addresses()) or "(none found)"
        print("  Loopback only — the ESP32 cannot reach this.")
        print("  For the display, rerun as: BIND_HOST=<ip> python3 server.py")
        print(f"  LAN addresses on this machine: {candidates}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.")
        server.server_close()
