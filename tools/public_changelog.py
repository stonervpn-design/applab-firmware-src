#!/usr/bin/env python3
"""
Post a build changelog to the webhook when a webflasher is deployed.

To keep the feed uncluttered, the commits since the last deploy are split into
TWO separate messages on the SAME webhook, in order:

  1. "Public build …"   — the public-facing commits (sent FIRST)
  2. "Private update …" — the private-only commits (sent AFTER)

A commit is "private" if its subject carries a (private…) / [private…] marker or
the phrase "private-only" (case-insensitive). Public commits that merely mention
"private" outside a marker (e.g. "fix private key parsing") stay public. If a
deploy has only public (or only private) commits, only that one message is sent.

SECURITY: the webhook URL is read from git-ignored tools/.public_webhook_url, so
the secret never lands in the public source repo.

Usage:
  python tools/public_changelog.py --board m5stick     # default
  python tools/public_changelog.py --board cardputer
  python tools/public_changelog.py --board t_embed
  python tools/public_changelog.py --board m5stick --test   # labeled test, no marker change
"""
import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
URL_FILE = os.path.join(HERE, ".public_webhook_url")  # git-ignored secret

# A commit subject is "private" if "private" appears in a ()/[] marker, or the
# phrase "private-only" appears. Targeted so incidental mentions of the word
# "private" in a public commit are not swept into the private message.
_PRIVATE_RE = re.compile(r"[\(\[]\s*private|private[-\s]only", re.IGNORECASE)

# CAR MATERIAL in commit SUBJECTS must never reach the PUBLIC changelog feed — the
# owner's "no car material on public" guardrail applies to every public output, not
# just the firmware binary (this classifier is why the binary guardrail's cousin
# lives here). Any subject matching a car cipher/feature/brand/decoder keyword is
# routed to the PRIVATE message instead. Deliberately broad and fail-safe: it is
# harmless to keep a borderline commit off the public feed, but a leak is not. If a
# genuinely public commit is ever hidden by this, tag it in the future — do NOT
# loosen these keywords. Word-boundaried so "car" won't match "card", etc.
_CAR_RE = re.compile(
    r"\b("
    r"vag|aut[-_ ]?64|proto[-_ ]?pirate|proto_pirate|"
    r"car|kia|hyundai|audi|bmw|ford|renault|nissan|toyota|mazda|fiat|peuge?ot|"
    r"citro[e\xEB]n|chevrolet|chrysler|jeep|subaru|mitsubishi|opel|volvo|porsche|"
    r"lexus|ldv|ducato|land[-_ ]?rover|scher[-_ ]?khan|star[-_ ]?line|starline|"
    r"sheriff|fob|rke|immobil\w*|dst[-_ ]?40|dst[-_ ]?80|megamos|hitag\d*|tpms|"
    r"keefob|maruti|volkswagen|skoda|seat"
    r")\b",
    re.IGNORECASE,
)


def is_car_subject(subject):
    return bool(_CAR_RE.search(subject))


def is_private_subject(subject):
    # Private feed catches BOTH explicitly-private commits AND anything car-related,
    # so car development can never be announced on the public channel.
    return bool(_PRIVATE_RE.search(subject)) or is_car_subject(subject)


BOARDS = {
    "m5stick": {
        "label": "M5Stick S3 webflasher",
        "tag": "m5stick_s3",
        "manifest": os.path.join(ROOT, "webflash", "manifest.json"),
        "marker": os.path.join(HERE, ".last_public_deploy_m5stick"),
    },
    "cardputer": {
        "label": "Cardputer ADV webflasher",
        "tag": "cardputer_adv",
        "manifest": os.path.join(ROOT, "webflash_cardputer_adv", "manifest.json"),
        "marker": os.path.join(HERE, ".last_public_deploy_cardputer"),
    },
    "t_embed": {
        "label": "T-Embed CC1101+ webflasher",
        "tag": "t_embed",
        "manifest": os.path.join(ROOT, "webflash_lilygo", "manifest.json"),
        "marker": os.path.join(HERE, ".last_public_deploy_t_embed"),
    },
}


def sh(args):
    try:
        return subprocess.run(
            args, cwd=ROOT, capture_output=True, text=True, timeout=30
        ).stdout.strip()
    except Exception:
        return ""


def read_url():
    if not os.path.exists(URL_FILE):
        sys.exit("no webhook configured: create tools/.public_webhook_url with the URL")
    return open(URL_FILE, encoding="utf-8").read().strip()


def read_version(manifest):
    try:
        with open(manifest, encoding="utf-8") as fh:
            return json.load(fh).get("version", "?")
    except Exception:
        return "?"


def build_changelog(marker):
    """Return (head_sha, public_lines, private_lines) since the last deploy."""
    head = sh(["git", "rev-parse", "HEAD"]) or "?"
    last = ""
    if os.path.exists(marker):
        last = open(marker, encoding="utf-8").read().strip()
    rng = f"{last}..HEAD" if last else "-15"  # first run: last 15 commits
    raw = sh(["git", "log", "--no-merges", "--pretty=%s", rng])
    lines = [l for l in raw.splitlines() if l.strip()]
    public = [l for l in lines if not is_private_subject(l)]
    private = [l for l in lines if is_private_subject(l)]
    return head, public, private


def post(url, kind, board_tag, label, version, subject, changelog_lines, ts, test):
    # Cap the message so a big backlog can't blow the webhook's content limit.
    # A stale deploy marker let ~195 commits pile up -> a 14 KB body -> HTTP 400
    # (a shorter 2.5 KB message went through, so the ceiling is a few KB; 1800 is a
    # safe margin under the common 2000-char cap). Keep the NEWEST commits (git log is
    # newest-first) and note how many older ones were omitted.
    MAX_CONTENT = 1800
    header = f"**{subject} {version}** — {label}\n{ts}\n"
    lines = list(changelog_lines) if changelog_lines else ["(none)"]
    kept, used, omitted = [], len(header), 0
    for i, l in enumerate(lines):
        item = f"- {l}\n"
        if used + len(item) > MAX_CONTENT and kept:
            omitted = len(lines) - i
            break
        kept.append(l)
        used += len(item)
    body = "\n".join(f"- {l}" for l in kept)
    if omitted:
        # Wording matters: this is the MESSAGE being trimmed to fit the webhook, NOT
        # features dropped from the build. An earlier "(+N commits omitted)" phrasing
        # read like N features were missing and caused a false alarm.
        body += (f"\n- …(only the {len(kept)} newest commits shown to fit; "
                 f"{omitted} older ones are still in the build — full list in git log)")
    content = header + body
    payload = {
        "content": content,  # Discord-style webhooks read `content`
        "text": content,     # some webhooks read `text`
        "kind": kind,        # "public" | "private"
        "version": version,
        "board": board_tag,
        "changelog": body,
        "timestamp": ts,
        "test": test,
    }
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=data,
        headers={
            "Content-Type": "application/json",
            # Cloudflare (in front of nexoria.chat) 403s the default
            # python-urllib User-Agent, so send a normal one.
            "User-Agent": "applab-firmware-changelog/1.0",
        },
    )
    with urllib.request.urlopen(req, timeout=15) as resp:
        return resp.status, resp.read().decode("utf-8", "replace")[:200]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", default="m5stick")
    ap.add_argument("--test", action="store_true")
    a = ap.parse_args()
    if a.board not in BOARDS:
        sys.exit(f"unknown board '{a.board}' (allowed: {', '.join(BOARDS)})")
    cfg = BOARDS[a.board]

    url = read_url()
    version = read_version(cfg["manifest"])
    head, public_lines, private_lines = build_changelog(cfg["marker"])

    if not public_lines and not private_lines and not a.test:
        print(f"[{a.board}] no new commits since last deploy - not posting.")
        return 0

    ts = datetime.datetime.now().isoformat(timespec="seconds")
    ok = True

    # HARD GUARD (defense in depth): never let a car-material commit reach the PUBLIC
    # post, even if is_private_subject() is later weakened. A public output leaking car
    # development is the exact failure this whole split exists to prevent.
    _leaked = [l for l in public_lines if is_car_subject(l)]
    if _leaked:
        print(f"[{a.board}] GUARD: dropped {len(_leaked)} car commit(s) from the PUBLIC "
              f"message (routing them private). Fix is_private_subject if this fires.")
        public_lines = [l for l in public_lines if not is_car_subject(l)]
        private_lines = private_lines + _leaked

    # 1) PUBLIC message first.
    if public_lines or a.test:
        try:
            status, resp = post(
                url, "public", cfg["tag"], cfg["label"], version,
                "Public build" + (" (test)" if a.test else ""),
                public_lines or ["(no public changes)"], ts, a.test)
            print(f"[{a.board}] public POST -> HTTP {status}  {resp}")
            ok = ok and 200 <= status < 300
        except Exception as exc:
            print(f"[{a.board}] public POST failed: {exc}")
            ok = False

    # 2) PRIVATE message AFTER (only if there are private commits).
    if private_lines:
        try:
            status, resp = post(
                url, "private", cfg["tag"], cfg["label"], version,
                "Private update" + (" (test)" if a.test else ""),
                private_lines, ts, a.test)
            print(f"[{a.board}] private POST -> HTTP {status}  {resp}")
            ok = ok and 200 <= status < 300
        except Exception as exc:
            print(f"[{a.board}] private POST failed: {exc}")
            ok = False

    if ok and not a.test:
        with open(cfg["marker"], "w", encoding="utf-8") as fh:
            fh.write(head)
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
