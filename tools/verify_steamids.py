#!/usr/bin/env python3
"""Verify SteamID64s have a public custom avatar and are not VAC-banned.

Usage:
  python3 tools/verify_steamids.py --ids 7656119... 7656119...
  python3 tools/verify_steamids.py --json lang/zh-CN.json
  python3 tools/verify_steamids.py --harvest --need 50
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

USER_AGENT = "CS2-Bot-Identity/0.1.5 steamid-verify (local tool)"
DEFAULT_AVATAR_HASHES = {
    "fef49e7fa7e1997310d705b2a6158ff8dc1cdfeb",
    "b5bd56c1aa4644a082a76f40c4f30920ef869ead",
}
PRO_NAME_RE = re.compile(
    r"(s1mple|zywoo|m0nesy|monesy|\bniko\b|donk|sh1ro|device|kennys|\bsimple\b|"
    r"slouken|pierre-loup|scottlu|gabe|valve)",
    re.I,
)
STEAMID64_RE = re.compile(r"<steamID64>(\d{17})</steamID64>")
AVATAR_RE = re.compile(r"<avatarIcon><!\[CDATA\[(.*?)\]\]></avatarIcon>")
VAC_RE = re.compile(r"<vacBanned>(\d)</vacBanned>")
NAME_RE = re.compile(r"<steamID><!\[CDATA\[(.*?)\]\]></steamID>")
PRIVACY_RE = re.compile(r"<privacyState>(\w+)</privacyState>")
HASH_RE = re.compile(r"/([0-9a-f]{40})\.")


def fetch(url: str, timeout: float = 20.0) -> str:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read().decode("utf-8", errors="replace")


def harvest_group_ids(group: str, pages: int) -> list[int]:
    ids: list[int] = []
    seen: set[int] = set()
    for page in range(1, pages + 1):
        url = f"https://steamcommunity.com/groups/{group}/memberslistxml/?xml=1&p={page}"
        try:
            xml = fetch(url)
        except (urllib.error.URLError, TimeoutError) as exc:
            print(f"harvest fail group={group} page={page}: {exc}", file=sys.stderr)
            break
        found = 0
        for match in STEAMID64_RE.finditer(xml):
            sid = int(match.group(1))
            if sid in seen:
                continue
            seen.add(sid)
            ids.append(sid)
            found += 1
        print(f"harvest group={group} page={page} new={found} total={len(ids)}", file=sys.stderr)
        if found == 0:
            break
        time.sleep(0.4)
    return ids


def verify_one(steamid: int) -> dict:
    url = f"https://steamcommunity.com/profiles/{steamid}/?xml=1"
    out = {
        "steamid": steamid,
        "ok": False,
        "reason": "",
        "name": "",
        "vac": None,
        "privacy": "",
        "avatar_hash": "",
    }
    try:
        xml = fetch(url)
    except (urllib.error.URLError, TimeoutError) as exc:
        out["reason"] = f"fetch:{exc}"
        return out
    if "Failed loading profile data" in xml or "<profile>" not in xml:
        out["reason"] = "no-profile"
        return out
    name_m = NAME_RE.search(xml)
    vac_m = VAC_RE.search(xml)
    priv_m = PRIVACY_RE.search(xml)
    av_m = AVATAR_RE.search(xml)
    out["name"] = name_m.group(1) if name_m else ""
    out["vac"] = int(vac_m.group(1)) if vac_m else None
    out["privacy"] = priv_m.group(1) if priv_m else ""
    avatar = av_m.group(1) if av_m else ""
    hash_m = HASH_RE.search(avatar)
    out["avatar_hash"] = hash_m.group(1) if hash_m else ""
    if out["vac"] == 1:
        out["reason"] = "vac"
        return out
    if not out["avatar_hash"]:
        out["reason"] = "no-avatar"
        return out
    if out["avatar_hash"] in DEFAULT_AVATAR_HASHES:
        out["reason"] = "default-avatar"
        return out
    if PRO_NAME_RE.search(out["name"] or ""):
        out["reason"] = "pro-name"
        return out
    if out["privacy"] != "public":
        out["reason"] = f"privacy:{out['privacy'] or 'unknown'}"
        return out
    if "☭" in (out["name"] or "") or "TRADING" in (out["name"] or "").upper():
        out["reason"] = "skip-name"
        return out
    out["ok"] = True
    out["reason"] = "ok"
    return out


def load_json_ids(path: Path) -> list[int]:
    data = json.loads(path.read_text(encoding="utf-8"))
    bots = data.get("bots", data)
    ids = []
    for _key, bot in bots.items():
        ids.append(int(bot["steamid"]))
    return ids


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ids", nargs="*", type=int, default=[])
    parser.add_argument("--json", type=Path)
    parser.add_argument("--harvest", action="store_true")
    parser.add_argument("--need", type=int, default=50)
    parser.add_argument(
        "--groups",
        nargs="*",
        default=["counterstrike", "SteamUniverse", "Valve"],
    )
    parser.add_argument("--pages", type=int, default=3)
    parser.add_argument("--sleep", type=float, default=0.25)
    args = parser.parse_args()

    candidates: list[int] = []
    if args.json:
        candidates.extend(load_json_ids(args.json))
    candidates.extend(args.ids)
    if args.harvest:
        for group in args.groups:
            candidates.extend(harvest_group_ids(group, args.pages))

    seen: set[int] = set()
    unique: list[int] = []
    for sid in candidates:
        if sid in seen:
            continue
        seen.add(sid)
        unique.append(sid)

    accepted: list[dict] = []
    rejected: list[dict] = []
    for sid in unique:
        if len(accepted) >= args.need and args.harvest:
            break
        row = verify_one(sid)
        time.sleep(args.sleep)
        if row["ok"]:
            accepted.append(row)
            print(
                f"OK  {row['steamid']}  vac={row['vac']}  {row['privacy']:8}  {row['name']!r}",
                file=sys.stderr,
            )
        else:
            rejected.append(row)
            print(f"NO  {row['steamid']}  {row['reason']}", file=sys.stderr)

    print(json.dumps({"accepted": accepted, "rejected_count": len(rejected)}, ensure_ascii=False, indent=2))
    if args.harvest and len(accepted) < args.need:
        print(f"need {args.need}, got {len(accepted)}", file=sys.stderr)
        return 1
    if not args.harvest:
        bad = [r for r in rejected]
        if bad:
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
