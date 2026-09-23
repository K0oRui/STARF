#!/usr/bin/env python3
"""Bidirectional save converter between gbe_fork (Goldberg) and STAR.

Layouts:
  GBE:  <gbe_root>/<appid>/remote/...          (sanitized names, subdirs kept)
        <gbe_root>/<appid>/achievements.json   ({name: {earned, earned_time}})
        <gbe_root>/<appid>/stats/<lowername>   (raw 4-byte LE int/float each)
        <gbe_root>/<appid>/leaderboard/...     (raw binary, no STAR equivalent)
  STAR: <star_root>/<appid>/<steamid>/remote/...  (subdirs kept, illegal -> _)
        <star_root>/<appid>/<steamid>/achievements.json ({name: {achieved, unlock_time}})
        <star_root>/<appid>/<steamid>/stats.json        ({name: {type, value}})
        <star_root>/<appid>/<steamid>/playtime.json     (STAR-only, left alone)

Default roots: %APPDATA%/GSE Saves and %APPDATA%/STAR (XDG on Linux).
"""

import argparse
import json
import math
import os
import shutil
import struct
import sys
from pathlib import Path

# --- GBE filename codec (mirrors sanitize_file_name/desanitize_file_name) ---
_GBE_ENCODE = [
    ("|", ".V_SLASH."),
    (":", ".COLON."),
    ("*", ".ASTERISK."),
    ('"', ".QUOTE."),
    ("?", ".Q_MARK."),
    ("%", ".PERCENT."),
]
_GBE_DECODE = [
    (".SLASH.", "/"),
    (".B_SLASH.", "\\"),
    (".F_SLASH.", "/"),
    (".V_SLASH.", "|"),
    (".COLON.", ":"),
    (".ASTERISK.", "*"),
    (".QUOTE.", '"'),
    (".Q_MARK.", "?"),
    (".PERCENT.", "%"),
]


def gbe_desanitize(name: str) -> str:
    for old, new in _GBE_DECODE:
        name = name.replace(old, new)
    return name


def gbe_logical_to_disk(remote_dir: Path, logical: str) -> Path:
    # Split subdirs first so separators survive on any platform.
    parts = []
    for p in logical.replace("\\", "/").split("/"):
        if p in ("", "."):
            continue
        for old, new in _GBE_ENCODE:
            p = p.replace(old, new)
        parts.append(p)
    return remote_dir.joinpath(*parts)


# --- STAR path codec (mirrors Storage::remote_path) ---
_STAR_ILLEGAL = set(':<>"|?*') | {chr(c) for c in range(0x20)}


def star_disk_path(remote_dir: Path, logical: str) -> Path:
    parts: list[str] = []
    for raw in logical.replace("/", "\\").split("\\"):
        if raw in ("", "."):
            continue
        if raw == "..":
            if parts:
                parts.pop()
            continue
        clean = "".join("_" if c in _STAR_ILLEGAL else c for c in raw)
        # A bare drive letter ("C") stays a plain folder, keeping C: vs D: apart.
        if clean:
            parts.append(clean)
    return remote_dir.joinpath(*parts)


# --- path resolution ---
def default_root(name: str) -> Path:
    if os.name == "nt":
        base = Path(os.environ.get("APPDATA", "."))
    else:
        base = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local" / "share"))
    return base / name


def resolve_gbe_dir(args) -> Path:
    if args.gbe_dir:
        return Path(args.gbe_dir)
    return Path(args.gbe_root or default_root("GSE Saves")) / str(args.appid)


def resolve_star_dir(args) -> Path:
    if args.star_dir:
        return Path(args.star_dir)
    root = Path(args.star_root or default_root("STAR")) / str(args.appid)
    if args.steam_id:
        return root / str(args.steam_id)
    if root.is_dir():
        subs = [p for p in root.iterdir() if p.is_dir()]
        if len(subs) == 1:
            return subs[0]
    sys.exit(f"error: --steam-id required (could not pick one under {root})")


# --- remote files ---
# src is a source path, or raw bytes to write verbatim (stats payloads).
def copy_remote_files(
    files, overwrite: bool, dry_run: bool, verb: str
) -> tuple[int, int]:
    copied = skipped = 0
    for src, dst in files:
        if dst.exists() and not overwrite:
            print(f"  skip (exists): {dst}")
            skipped += 1
            continue
        detail = src if isinstance(src, Path) else str(len(src)) + " bytes"
        print(f"  {verb}: {detail} -> {dst}")
        if not dry_run:
            dst.parent.mkdir(parents=True, exist_ok=True)
            if isinstance(src, bytes):
                dst.write_bytes(src)
            else:
                shutil.copy2(src, dst)
        copied += 1
    return copied, skipped


def collect_files(root: Path) -> list[Path]:
    if not root.is_dir():
        return []
    return [p for p in sorted(root.rglob("*")) if p.is_file()]


# --- achievements ---
def load_json(path: Path) -> dict:
    if path.is_file():
        try:
            data = json.loads(path.read_text(encoding="utf-8-sig"))
            return data if isinstance(data, dict) else {}
        except (json.JSONDecodeError, OSError) as e:
            print(f"  warn: could not parse {path}: {e}")
    return {}


# Either spelling reads ("earned"/"earned_time" vs "achieved"/"unlock_time"):
# each direction only ever writes its own, so preference order never matters.
def ach_tuple(entry: dict) -> tuple[bool, int]:
    if not isinstance(entry, dict):
        return False, 0
    unlocked = bool(entry.get("earned", entry.get("achieved", False)))
    t = entry.get("earned_time", entry.get("unlock_time", 0))
    try:
        t = int(t)
    except (TypeError, ValueError):
        t = 0
    return unlocked, t


def merge_achievements(
    old: dict, new_items: dict[str, tuple[bool, int]], true_key: str, time_key: str
) -> dict:
    merged = dict(old)
    for name in new_items:
        unlocked, t = new_items[name]
        o_unlocked, o_t = ach_tuple(merged[name]) if name in merged else (False, 0)
        u = unlocked or o_unlocked
        merged[name] = {true_key: u, time_key: max(t, o_t) if u else 0}
    return merged


def merge_achievement_files(
    src_path: Path,
    old: dict,
    true_key: str,
    time_key: str,
    src_label: str,
    dst_label: str,
) -> dict:
    src = load_json(src_path)
    merged = merge_achievements(
        old, {n: ach_tuple(e) for n, e in src.items()}, true_key, time_key
    )
    unlocked = sum(
        1 for v in merged.values() if isinstance(v, dict) and v.get(true_key)
    )
    print(
        f"achievements: {len(src)} in {src_label}, {len(old)} in {dst_label} "
        f"-> {len(merged)} merged ({unlocked} unlocked)"
    )
    return merged


def write_json(path: Path, data: dict) -> None:
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


# --- stats ---
def load_gbe_stat_types(settings_dir: str | None) -> dict[str, str]:
    types: dict[str, str] = {}
    if settings_dir:
        try:
            defs = json.loads(
                (Path(settings_dir) / "stats.json").read_text(encoding="utf-8-sig")
            )
            for d in defs:
                if isinstance(d, dict) and d.get("name"):
                    types[str(d["name"]).lower()] = str(d.get("type", "int")).lower()
        except (OSError, json.JSONDecodeError) as e:
            print(f"  warn: could not read stats.json in {settings_dir}: {e}")
    return types


def guess_float_stat(raw: bytes) -> bool:
    """Heuristic for GBE raw stats when no steam_settings/stats.json is given.

    Small ints decode as float denormals (~1e-45), so anything with |f| < 1e-6
    is treated as int; sane-magnitude finite floats as float; huge/NaN as int.
    Every guess is reported so it can be checked against steam_settings.
    """
    i = struct.unpack("<i", raw)[0]
    f = struct.unpack("<f", raw)[0]
    if i == 0:
        return False
    if not math.isfinite(f) or abs(f) < 1e-6 or abs(f) > 1e9:
        return False
    return True


def convert_gbe_stats(gbe_app: Path, types: dict[str, str]) -> tuple[dict, list[str]]:
    stats_dir = gbe_app / "stats"
    out: dict = {}
    notes: list[str] = []
    if stats_dir.is_dir():
        for p in sorted(stats_dir.iterdir()):
            if not p.is_file():
                continue
            raw = p.read_bytes()
            if len(raw) != 4:
                notes.append(f"skip stat '{p.name}': {len(raw)} bytes (expected 4)")
                continue
            t = types.get(p.name.lower(), "")
            if t not in ("float", "avgrate", "int"):
                if guess_float_stat(raw):
                    t = "float"
                    notes.append(
                        f"stat '{p.name}' assumed float (no definition; pass --gbe-settings to fix)"
                    )
                else:
                    t = "int"
            fmt = "<f>" if t in ("float", "avgrate") else "<i>"
            tag = "float" if t in ("float", "avgrate") else "int"
            out[p.name] = {"type": tag, "value": struct.unpack(fmt, raw)[0]}
    return out, notes


def convert_star_stats(star_stats: dict) -> tuple[list[tuple[str, bytes]], list[str]]:
    files: list[tuple[str, bytes]] = []
    notes: list[str] = []
    for name, entry in star_stats.items():
        if not isinstance(entry, dict):
            notes.append(f"skip stat '{name}': not an object")
            continue
        t = str(entry.get("type", "int")).lower()
        if t not in ("float", "int"):
            notes.append(f"stat '{name}': unknown type '{t}', wrote as int")
            t = "int"
        try:
            v = (
                float(entry.get("value", 0.0))
                if t == "float"
                else int(entry.get("value", 0))
            )
            files.append(
                (str(name).lower(), struct.pack("<f" if t == "float" else "<i", v))
            )
        except (TypeError, ValueError, struct.error) as e:
            notes.append(f"skip stat '{name}': bad value ({e})")
    return files, notes


# --- directions ---
def cmd_gbe2star(args) -> int:
    gbe_app = resolve_gbe_dir(args)
    star_app = resolve_star_dir(args)
    if not gbe_app.is_dir():
        sys.exit(f"error: GBE app dir not found: {gbe_app}")
    print(f"GBE : {gbe_app}\nSTAR: {star_app}")

    # remote
    gbe_remote = gbe_app / "remote"
    pairs = [
        (
            src,
            star_disk_path(
                star_app / "remote",
                gbe_desanitize(src.relative_to(gbe_remote).as_posix()),
            ),
        )
        for src in collect_files(gbe_remote)
    ]
    print(f"remote files: {len(pairs)}")
    copied, skipped = copy_remote_files(pairs, args.overwrite, args.dry_run, "copy")

    # achievements (union merge, unlocked wins)
    old = load_json(star_app / "achievements.json")
    merged = merge_achievement_files(
        gbe_app / "achievements.json", old, "achieved", "unlock_time", "GBE", "STAR"
    )

    # stats
    types = load_gbe_stat_types(args.gbe_settings)
    if types:
        print(f"stat type definitions: {len(types)}")
    new_stats, notes = convert_gbe_stats(gbe_app, types)
    old_stats = load_json(star_app / "stats.json")
    merged_stats = {**old_stats, **new_stats}  # source wins per key
    print(
        f"stats: {len(new_stats)} from GBE, {len(old_stats)} in STAR -> {len(merged_stats)} merged"
    )
    for n in notes:
        print(f"  note: {n}")

    # leaderboards have no STAR equivalent: carry them verbatim for round-trips
    lb_src = gbe_app / "leaderboard"
    lb_pairs = [
        (p, star_app / "leaderboard" / p.relative_to(lb_src))
        for p in collect_files(lb_src)
    ]
    if lb_pairs:
        print(f"leaderboard blobs (passthrough): {len(lb_pairs)}")
    lb_copied, lb_skipped = copy_remote_files(
        lb_pairs, args.overwrite, args.dry_run, "carry"
    )

    if not args.dry_run:
        star_app.mkdir(parents=True, exist_ok=True)
        write_json(star_app / "achievements.json", merged)
        write_json(star_app / "stats.json", merged_stats)
    print(
        f"done: remote {copied} copied/{skipped} skipped, leaderboard {lb_copied}/{lb_skipped}, "
        f"achievements+stats {'previewed' if args.dry_run else 'written'}"
    )
    return 0


def cmd_star2gbe(args) -> int:
    gbe_app = resolve_gbe_dir(args)
    star_app = resolve_star_dir(args)
    if not star_app.is_dir():
        sys.exit(f"error: STAR app dir not found: {star_app}")
    print(f"STAR: {star_app}\nGBE : {gbe_app}")

    star_remote = star_app / "remote"
    pairs = [
        (
            src,
            gbe_logical_to_disk(
                gbe_app / "remote", src.relative_to(star_remote).as_posix()
            ),
        )
        for src in collect_files(star_remote)
    ]
    print(
        f'remote files: {len(pairs)} (note: STAR folds <>:"|?* to _, GBE keeps them encoded - '
        f"names with those chars won't round-trip)"
    )
    copied, skipped = copy_remote_files(pairs, args.overwrite, args.dry_run, "copy")

    old = load_json(gbe_app / "achievements.json")
    merged = merge_achievement_files(
        star_app / "achievements.json", old, "earned", "earned_time", "STAR", "GBE"
    )

    star_stats = load_json(star_app / "stats.json")
    stat_files, notes = convert_star_stats(star_stats)
    for n in notes:
        print(f"  note: {n}")
    print(f"stats: {len(stat_files)} from STAR")
    stats_pairs = [(payload, gbe_app / "stats" / name) for name, payload in stat_files]
    s_copied, s_skipped = copy_remote_files(
        stats_pairs, args.overwrite, args.dry_run, "write"
    )

    lb_src = star_app / "leaderboard"
    lb_pairs = [
        (p, gbe_app / "leaderboard" / p.relative_to(lb_src))
        for p in collect_files(lb_src)
    ]
    if lb_pairs:
        print(f"leaderboard blobs (restore): {len(lb_pairs)}")
    lb_copied, lb_skipped = copy_remote_files(
        lb_pairs, args.overwrite, args.dry_run, "carry"
    )

    if (star_app / "playtime.json").is_file():
        print("  note: playtime.json has no GBE equivalent, left behind")
    if not args.dry_run:
        gbe_app.mkdir(parents=True, exist_ok=True)
        write_json(gbe_app / "achievements.json", merged)
    print(
        f"done: remote {copied} copied/{skipped} skipped, stats {s_copied}/{s_skipped}, "
        f"leaderboard {lb_copied}/{lb_skipped}, achievements {'previewed' if args.dry_run else 'written'}"
    )
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Convert saves between gbe_fork and STAR (both directions)."
    )
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("--appid", required=True, help="Steam app ID")
        p.add_argument(
            "--gbe-root", help="GBE saves root (default: %%APPDATA%%/GSE Saves)"
        )
        p.add_argument(
            "--gbe-dir", help="GBE <appid> dir directly (overrides --gbe-root/--appid)"
        )
        p.add_argument("--star-root", help="STAR root (default: %%APPDATA%%/STAR)")
        p.add_argument("--star-dir", help="STAR <appid>/<steamid> dir directly")
        p.add_argument(
            "--steam-id", help="STAR user ID (auto-detected if exactly one exists)"
        )
        p.add_argument(
            "--overwrite",
            action="store_true",
            help="replace existing remote/stat files",
        )
        p.add_argument(
            "--dry-run", action="store_true", help="preview only, write nothing"
        )

    p1 = sub.add_parser("gbe2star", help="GBE -> STAR")
    common(p1)
    p1.add_argument(
        "--gbe-settings", help="GBE steam_settings dir with stats.json for stat types"
    )

    p2 = sub.add_parser("star2gbe", help="STAR -> GBE")
    common(p2)

    args = ap.parse_args()
    return cmd_gbe2star(args) if args.cmd == "gbe2star" else cmd_star2gbe(args)


if __name__ == "__main__":
    raise SystemExit(main())
