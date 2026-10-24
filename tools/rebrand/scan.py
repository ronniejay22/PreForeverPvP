"""Finds every occurrence of "centurion" (any case) that the rebrand has to
decide about, and writes them to an inventory. Read-only.

Sources:
  repo     tracked text files (git ls-files), except the SQL dumps
  db       every text column of the auth, world and characters databases
           (the dumps under centurion/sql are what these were loaded from)
  addons   files inside centurion/patches/addons.zip
  mpq      names and text files inside each patch zip's MPQ, plus the
           strings of every DBC in them
  dbc      strings of the server's DBCs (centurion/dbc)

usage: python -I tools/rebrand/scan.py [--out D:\\WoWServer\\rebrand\\inventory.json]
"""
from __future__ import annotations

import argparse
import configparser
import io
import json
import re
import struct
import subprocess
import sys
import zipfile
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
from mpq import MPQ  # noqa: E402

WORD = re.compile(rb"centurion", re.I)
TEXT_EXT = {".lua", ".xml", ".toc", ".txt", ".wtf", ".html", ".htm", ".json", ".md"}
MYSQL = r"C:\Program Files\MySQL\MySQL Server 8.4\bin\mysql.exe"


def context(data: bytes, start: int, end: int, width: int = 60) -> str:
    a = max(0, start - width)
    b = min(len(data), end + width)
    return data[a:b].decode("utf-8", "replace").replace("\r", " ").replace("\n", " ⏎ ")


def scan_text(data: bytes, where: dict) -> list[dict]:
    hits = []
    for m in WORD.finditer(data):
        line = data.count(b"\n", 0, m.start()) + 1
        hits.append({**where, "line": line, "match": m.group().decode(), "context": context(data, m.start(), m.end())})
    return hits


def dbc_strings(data: bytes) -> list[tuple[int, str]]:
    """(offset, string) for every string in a WDBC string block that matches."""
    if data[:4] != b"WDBC":
        return []
    records, fields, record_size, string_size = struct.unpack_from("<4I", data, 4)
    block = data[20 + records * record_size: 20 + records * record_size + string_size]
    out = []
    for m in WORD.finditer(block):
        s = block.rfind(b"\0", 0, m.start()) + 1
        e = block.find(b"\0", m.end())
        out.append((s, block[s:e].decode("utf-8", "replace")))
    return sorted(set(out))


def scan_repo() -> list[dict]:
    files = subprocess.run(["git", "-C", str(REPO), "ls-files", "-z"], capture_output=True, check=True).stdout.split(b"\0")
    hits = []
    for f in files:
        if not f:
            continue
        rel = f.decode("utf-8")
        if rel.startswith("centurion/sql/") or rel.startswith("centurion/patches/") or rel.startswith("centurion/dbc/"):
            continue  # covered by the db, mpq and dbc sources
        if rel.startswith("tools/rebrand/"):
            continue
        path = REPO / rel
        try:
            data = path.read_bytes()
        except OSError:
            continue
        if b"\0" in data[:8000]:
            if WORD.search(path.name.encode()):
                hits.append({"source": "repo", "file": rel, "line": 0, "match": "", "context": "(binary file name)"})
            continue
        hits += scan_text(data, {"source": "repo", "file": rel})
        if WORD.search(rel.encode()) and not WORD.search(data):
            hits.append({"source": "repo", "file": rel, "line": 0, "match": "", "context": "(file name)"})
    return hits


def mysql(sql: str) -> list[list[str]]:
    cnf = r"D:\WoWServer\secrets\mysql-root.cnf"
    out = subprocess.run([MYSQL, f"--defaults-extra-file={cnf}", "--default-character-set=utf8mb4", "-N", "-B", "-e", sql],
                         capture_output=True, check=True).stdout.decode("utf-8", "replace")
    return [row.split("\t") for row in out.splitlines()]


def scan_db() -> list[dict]:
    # base tables only: views just show base-table data (and some views in the
    # snapshot refer to things that don't exist here)
    cols = mysql("SELECT c.table_schema, c.table_name, c.column_name FROM information_schema.columns c "
                 "JOIN information_schema.tables t ON t.table_schema = c.table_schema AND t.table_name = c.table_name "
                 "WHERE c.table_schema IN ('auth','world','characters') AND t.table_type = 'BASE TABLE' "
                 "AND c.data_type IN ('char','varchar','text','tinytext','mediumtext','longtext')")
    tables: dict[tuple[str, str], list[str]] = {}
    for schema, table, column in cols:
        tables.setdefault((schema, table), []).append(column)
    keys = {}
    for schema, table, column in mysql("SELECT table_schema, table_name, column_name FROM information_schema.key_column_usage "
                                       "WHERE constraint_name='PRIMARY' AND table_schema IN ('auth','world','characters') "
                                       "ORDER BY ordinal_position"):
        keys.setdefault((schema, table), []).append(column)
    hits = []
    for (schema, table), columns in sorted(tables.items()):
        pk = keys.get((schema, table), [])
        where = " OR ".join(f"`{c}` LIKE '%centurion%'" for c in columns)
        select = ", ".join([f"`{c}`" for c in pk] + [f"`{c}`" for c in columns]) if pk else ", ".join(f"`{c}`" for c in columns)
        # MySQL -B escapes tabs/newlines inside values, so rows split cleanly
        try:
            rows = mysql(f"SELECT {select} FROM `{schema}`.`{table}` WHERE {where}")
        except subprocess.CalledProcessError as e:
            print(f"skipped {schema}.{table}: {e.stderr.decode('utf-8', 'replace').strip()}", file=sys.stderr)
            continue
        for row in rows:
            ids, values = row[:len(pk)], row[len(pk):]
            for c, v in zip(columns, values):
                if WORD.search(v.encode("utf-8", "replace")):
                    hits.append({"source": "db", "file": f"{schema}.{table}", "key": dict(zip(pk, ids)),
                                 "column": c, "context": v[:160]})
    return hits


def scan_zip_texts(zbytes: bytes, label: str) -> list[dict]:
    hits = []
    with zipfile.ZipFile(io.BytesIO(zbytes)) as z:
        for n in z.namelist():
            if WORD.search(n.encode()):
                hits.append({"source": "addons", "file": f"{label}:{n}", "line": 0, "match": "", "context": "(path)"})
            if Path(n).suffix.lower() in TEXT_EXT:
                hits += scan_text(z.read(n), {"source": "addons", "file": f"{label}:{n}"})
    return hits


def scan_mpqs(patches: Path, work: Path) -> list[dict]:
    hits = []
    work.mkdir(parents=True, exist_ok=True)
    for zpath in sorted(patches.glob("*.zip")):
        if zpath.stem in ("addons", "client-tweaks"):
            continue
        with zipfile.ZipFile(zpath) as z:
            (member,) = z.namelist()
            mpq_path = work / member
            if not mpq_path.exists() or mpq_path.stat().st_size != z.getinfo(member).file_size:
                with z.open(member) as src, open(mpq_path, "wb") as out:
                    while chunk := src.read(1 << 20):
                        out.write(chunk)
        label = f"{zpath.stem}:{member}"
        with MPQ(mpq_path) as a:
            for n in a.names():
                if WORD.search(n.encode("latin-1")):
                    hits.append({"source": "mpq", "file": f"{label}:{n}", "line": 0, "match": "", "context": "(path)"})
                ext = Path(n.replace("\\", "/")).suffix.lower()
                if ext in TEXT_EXT:
                    hits += scan_text(a.read(n), {"source": "mpq", "file": f"{label}:{n}"})
                elif ext == ".dbc":
                    for off, s in dbc_strings(a.read(n)):
                        hits.append({"source": "mpq", "file": f"{label}:{n}", "offset": off, "context": s[:200]})
    return hits


def scan_server_dbcs() -> list[dict]:
    hits = []
    for f in sorted((REPO / "centurion" / "dbc").glob("*.dbc")):
        for off, s in dbc_strings(f.read_bytes()):
            hits.append({"source": "dbc", "file": f"centurion/dbc/{f.name}", "offset": off, "context": s[:200]})
    return hits


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=r"D:\WoWServer\rebrand\inventory.json")
    ap.add_argument("--patches", default=str(REPO / "centurion" / "patches"))
    ap.add_argument("--work", default=r"D:\WoWServer\rebrand\mpq")
    args = ap.parse_args()

    patches = Path(args.patches)
    inventory = {
        "repo": scan_repo(),
        "db": scan_db(),
        "addons": scan_zip_texts((patches / "addons.zip").read_bytes(), "addons.zip"),
        "mpq": scan_mpqs(patches, Path(args.work)),
        "dbc": scan_server_dbcs(),
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(inventory, indent=1, ensure_ascii=False), encoding="utf-8")

    for source, hits in inventory.items():
        files = Counter(h["file"] for h in hits)
        print(f"{source:7} {len(hits):6} hits in {len(files):5} places")
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
