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


def is_private_subject(subject):
    return bool(_PRIVATE_RE.search(subject))


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
    body = "\n".join(f"- {l}" for l in changelog_lines) if changelog_lines else "- (none)"
    content = f"**{subject} {version}** — {label}\n{ts}\n{body}"
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
