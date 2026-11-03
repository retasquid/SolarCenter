#!/usr/bin/env python3
"""Minifie légèrement et compresse en gzip les sources de web/ vers littlefs_image/www/.

La minification reste prudente (aucun analyseur JS) : commentaires et indentation
supprimés, retours à la ligne conservés pour ne pas dépendre de l'insertion
automatique des points-virgules.
"""

import gzip
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "web"
OUT = ROOT / "littlefs_image" / "www"
BUDGET = 100 * 1024  # octets compressés (CLAUDE.md §11.1)
EXTS = {".html", ".css", ".js", ".svg"}


def minify_css(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"\s+", " ", text)
    text = re.sub(r"\s*([{}:;,>])\s*", r"\1", text)
    return text.replace(";}", "}").strip()


def minify_html(text: str) -> str:
    text = re.sub(r"<!--.*?-->", "", text, flags=re.S)
    lines = (line.strip() for line in text.splitlines())
    return "\n".join(line for line in lines if line)


def minify_js(text: str) -> str:
    out = []
    for line in text.splitlines():
        line = line.strip()
        if line and not line.startswith("//"):
            out.append(line)
    return "\n".join(out)


MINIFIERS = {".css": minify_css, ".html": minify_html, ".svg": minify_html, ".js": minify_js}


def main() -> int:
    if not SRC.is_dir():
        print(f"erreur : {SRC} introuvable", file=sys.stderr)
        return 1
    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob("*"):
        if old.is_file():
            old.unlink()

    total_raw = total_gz = 0
    for src in sorted(SRC.iterdir()):
        if not src.is_file() or src.suffix not in EXTS:
            continue
        raw = src.read_text(encoding="utf-8")
        mini = MINIFIERS[src.suffix](raw).encode("utf-8")
        gz = gzip.compress(mini, compresslevel=9, mtime=0)
        (OUT / (src.name + ".gz")).write_bytes(gz)
        total_raw += len(raw.encode("utf-8"))
        total_gz += len(gz)
        print(f"  {src.name:<14} {len(raw.encode('utf-8')):>7} o -> {len(mini):>7} o -> {len(gz):>6} o gzip")

    print(f"total : {total_raw} o source, {total_gz} o compressés (budget {BUDGET} o)")
    if total_gz > BUDGET:
        print("erreur : budget de 100 Ko dépassé", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
