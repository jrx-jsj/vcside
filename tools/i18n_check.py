#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Juan Sánchez Jurado
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


def unescape(literal):
    return bytes(literal, "utf-8").decode("unicode_escape").encode("latin-1").decode("utf-8")


def source_strings():
    found = {}
    for folder, _, files in os.walk(SRC):
        for file_name in files:
            if not file_name.endswith((".cpp", ".h", ".mm")):
                continue
            path = os.path.join(folder, file_name)
            text = open(path, encoding="utf-8").read()
            for match in CALL.finditer(text):
                found.setdefault(unescape(match.group(1)), os.path.relpath(path, ROOT))
            for match in TERNARY.finditer(text):
                for branch in (1, 2):
                    found.setdefault(unescape(match.group(branch)), os.path.relpath(path, ROOT))
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
    for file_name in sorted(os.listdir(I18N)):
        if not file_name.endswith(".json"):
            continue
        path = os.path.join(I18N, file_name)
        catalog = json.load(open(path, encoding="utf-8"))
        translations = catalog.get("strings", {})
        missing = [text for text in strings if not translations.get(text)]
        unused = [text for text in translations if text not in strings]
        bad = [text for text, translation in translations.items()
               if translation and set(PLACEHOLDER.findall(text)) != set(PLACEHOLDER.findall(translation))]
        done = len(strings) - len(missing)
        print(f"{file_name}: {done}/{len(strings)} translated, {len(missing)} missing, {len(unused)} unused, {len(bad)} with wrong placeholders")
        for text in bad:
            print(f"  placeholders differ: {text!r} -> {translations[text]!r}")
        if not update:
            for text in unused:
                print(f"  unused: {text!r}")
            if unused:
                status = 1
        if bad:
            status = 1
        if update:
            catalog["total"] = len(strings)
            catalog["strings"] = {text: translations.get(text, "") for text in sorted(strings, key=str.lower)}
            with open(path, "w", encoding="utf-8") as out:
                json.dump(catalog, out, ensure_ascii=False, indent=2)
                out.write("\n")
    return status


if __name__ == "__main__":
    sys.exit(main())
