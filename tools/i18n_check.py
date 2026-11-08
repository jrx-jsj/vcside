#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 VCSIDE contributors
"""Checks the interface catalogs in data/i18n against the strings in src/.

Strings are collected from T("..."), Ts("..."), F("...") and N_("...") calls,
including the two branches of `cond ? "a" : "b"` inside them.

  python3 tools/i18n_check.py            report missing / unused strings per catalog
  python3 tools/i18n_check.py --update   add missing keys (empty = falls back to English),
                                         drop unused ones, sort, refresh "total"
  python3 tools/i18n_check.py --new pt-BR "Português (Brasil)"   start a new catalog

Exit code 1 if a catalog has unused keys or a translation breaks the {n} placeholders.
"""
import json, os, re, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SRC = os.path.join(ROOT, "src")
I18N = os.path.join(ROOT, "data", "i18n")

LIT = r'"((?:[^"\\]|\\.)*)"'
CALL = re.compile(r'\b(?:T|Ts|F|N_)\(\s*' + LIT)
TERNARY = re.compile(r'\b(?:T|Ts|F|N_)\(\s*[^"();]*\?\s*' + LIT + r'\s*:\s*' + LIT)
PLACEHOLDER = re.compile(r'\{(\d+)\}')


def unescape(s):
    return bytes(s, "utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8")


def source_strings():
    found = {}
    for d, _, files in os.walk(SRC):
        for f in files:
            if not f.endswith((".cpp", ".h")):
                continue
            path = os.path.join(d, f)
            text = open(path, encoding="utf-8").read()
            for m in CALL.finditer(text):
                found.setdefault(unescape(m.group(1)), os.path.relpath(path, ROOT))
            for m in TERNARY.finditer(text):
                for g in (1, 2):
                    found.setdefault(unescape(m.group(g)), os.path.relpath(path, ROOT))
    found.pop("", None)
    return found


def main():
    strings = source_strings()
    if len(sys.argv) >= 4 and sys.argv[1] == "--new":
        code, name = sys.argv[2], sys.argv[3]
        path = os.path.join(I18N, code + ".json")
        if os.path.exists(path):
            sys.exit(f"{path} already exists")
        json.dump({"language": code, "name": name, "total": len(strings), "strings": {}}, open(path, "w"), ensure_ascii=False, indent=2)
        sys.argv = [sys.argv[0], "--update"]
    update = "--update" in sys.argv
    status = 0
    for f in sorted(os.listdir(I18N)):
        if not f.endswith(".json"):
            continue
        path = os.path.join(I18N, f)
        cat = json.load(open(path, encoding="utf-8"))
        tr = cat.get("strings", {})
        missing = [s for s in strings if not tr.get(s)]
        unused = [s for s in tr if s not in strings]
        bad = [s for s, t in tr.items() if t and set(PLACEHOLDER.findall(s)) != set(PLACEHOLDER.findall(t))]
        done = len(strings) - len(missing)
        print(f"{f}: {done}/{len(strings)} translated, {len(missing)} missing, {len(unused)} unused, {len(bad)} with wrong placeholders")
        for s in bad:
            print(f"  placeholders differ: {s!r} -> {tr[s]!r}")
        if not update:
            for s in unused:
                print(f"  unused: {s!r}")
            if unused:
                status = 1
        if bad:
            status = 1
        if update:
            cat["total"] = len(strings)
            cat["strings"] = {s: tr.get(s, "") for s in sorted(strings, key=str.lower)}
            with open(path, "w", encoding="utf-8") as out:
                json.dump(cat, out, ensure_ascii=False, indent=2)
                out.write("\n")
    return status


if __name__ == "__main__":
    sys.exit(main())
