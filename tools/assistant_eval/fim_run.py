#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Juan Sánchez Jurado
"""Would a small fill-in-the-middle model be worth shipping for completion? (docs/design-assistant.md, "AI completion")

  build/llm-helper/bin/llama-server -m <a FIM model .gguf> --port 8796 -c 8192 &
  python3 tools/assistant_eval/fim_run.py --url http://127.0.0.1:8796 [--cases 120] [--extra more.asm] [--json out.json]

The test set is made from the DASM templates: an instruction line is hidden and the model is asked for it, given the text before
and after. Two places for the cursor: the start of the line ("line") and after the mnemonic ("operand"). Each case is asked twice:
with the file alone, and with what the knowledge base says about the registers named nearby and the recipe the nearest comment
matches (as the extra context an editor would add).

What is counted: suggestions identical to the hidden line; suggestions that pass the filter the editor would apply before showing
one (a 6502 mnemonic, operands that exist in the file or the reference, no store to a read-only register); and, of the ones that
pass, how many are right: that last number is what a person would live with.
"""
import argparse, glob, json, os, random, re, sys, time, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
KB = os.path.join(ROOT, "data", "kb")

def load_kb():
    cpu = json.load(open(os.path.join(KB, "6502.json")))
    mnemonics = {name.lower() for name, entry in cpu["instructions"].items() if not entry.get("illegal")}
    registers = {}
    for name in ("tia.json", "riot.json"):
        for reg in json.load(open(os.path.join(KB, name)))["registers"]:
            registers[reg["name"]] = reg
    techniques = [t for t in json.load(open(os.path.join(KB, "techniques.json")))["techniques"]
                  if not t["id"].startswith(("7800-", "tinyelf-"))]
    return mnemonics, registers, techniques

def code_of(line):
    return line.split(";", 1)[0]

def symbols_of(lines):
    names = set()
    for line in lines:
        code = code_of(line)
        match = re.match(r"^([A-Za-z_.][\w.]*):?", code)
        if match:
            names.add(match.group(1))
        match = re.match(r"^\s+([A-Za-z_][\w.]*)\s+(=|equ|EQU|ds|DS)\b", code)
        if match:
            names.add(match.group(1))
    return names

def kb_context(lines, at, registers, techniques):
    """What an editor would add: the registers named around the cursor and the recipe of the nearest comment."""
    extra = []
    nearby = " ".join(lines[max(0, at - 16):at] + lines[at + 1:at + 9])
    named = [name for name in registers if re.search(r"\b" + re.escape(name) + r"\b", nearby)]
    if named:
        text = "; Atari 2600 registers used here (R = read only, W = write only):\n"
        for name in named[:10]:
            reg = registers[name]
            text += f"; {name} {reg['address']} {reg['access']}: {reg['summary']}\n"
        extra.append({"filename": "reference/registers.asm", "text": text})
    comment = ""
    for back in range(at - 1, max(-1, at - 8), -1):
        if ";" in lines[back]:
            comment = lines[back].split(";", 1)[1].lower()
            break
    words = set(re.findall(r"[a-z]{4,}", comment))
    best, score = None, 0
    for technique in techniques:
        hits = len(words & {k.lower() for k in technique.get("keywords", [])})
        if hits > score and technique.get("example"):
            best, score = technique, hits
    if best and score >= 2:
        extra.append({"filename": "reference/" + best["id"] + ".asm", "text": "; " + best["title"] + "\n" + best["example"] + "\n"})
    return extra

def infill(url, prefix, suffix, extra):
    body = {"input_prefix": prefix, "input_suffix": suffix, "input_extra": extra, "n_predict": 32, "temperature": 0.1, "stop": ["\n"],
            "t_max_predict_ms": 2000, "cache_prompt": True}
    request = urllib.request.Request(url.rstrip("/") + "/infill", data=json.dumps(body).encode(), headers={"Content-Type": "application/json"})
    started = time.time()
    with urllib.request.urlopen(request, timeout=60) as reply:
        content = json.loads(reply.read()).get("content", "")
    return content.split("\n")[0], time.time() - started

def normal(line):
    return " ".join(code_of(line).split()).lower()

def passes_filter(line, mnemonics, registers, known):
    """The filter the editor would apply before showing a suggestion."""
    words = code_of(line).split()
    if not words or words[0].lower() not in mnemonics:
        return False
    mnemonic, operand = words[0].lower(), " ".join(words[1:])
    for name in re.findall(r"[A-Za-z_.][\w.]*", re.sub(r"\$[0-9A-Fa-f]+|%[01]+|'.'", " ", operand)):
        if name.upper() in ("A", "X", "Y"):
            continue
        if name in registers:
            access = registers[name]["access"]
            if mnemonic in ("sta", "stx", "sty") and access == "R":
                return False
            if mnemonic in ("lda", "ldx", "ldy", "bit", "cmp", "cpx", "cpy", "and", "ora", "eor", "adc", "sbc") and access == "W":
                return False
            continue
        if name not in known:
            return False
    return True

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", required=True)
    parser.add_argument("--cases", type=int, default=120)
    parser.add_argument("--extra", action="append", default=[])
    parser.add_argument("--seed", type=int, default=2600)
    parser.add_argument("--json")
    args = parser.parse_args()
    mnemonics, registers, techniques = load_kb()
    files = sorted(glob.glob(os.path.join(ROOT, "data", "templates", "dasm*", "main.asm"))) + args.extra
    candidates = []
    for path in files:
        lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
        known = symbols_of(lines)
        for index, line in enumerate(lines):
            words = code_of(line).split()
            if index > 20 and line[:1] in (" ", "\t") and words and words[0].lower() in mnemonics:
                candidates.append((path, index, lines, known))
    random.Random(args.seed).shuffle(candidates)
    candidates = candidates[:args.cases]
    results = []
    totals = {}
    for path, index, lines, known in candidates:
        hidden = lines[index]
        indent = re.match(r"\s*", hidden).group(0)
        mnemonic = code_of(hidden).split()[0]
        before = "\n".join(lines[max(0, index - 256):index]) + "\n"
        after = "\n" + "\n".join(lines[index + 1:index + 65])
        extra = kb_context(lines, index, registers, techniques)
        for place, typed in (("line", indent), ("operand", indent + mnemonic + " ")):
            if place == "operand" and len(code_of(hidden).split()) < 2:
                continue  # an instruction with no operand: nothing to complete after it
            for label, context in (("file", []), ("file+kb", extra)):
                try:
                    suggestion, seconds = infill(args.url, before + typed, after, context)
                except Exception as error:  # noqa: BLE001
                    print("server error:", error, file=sys.stderr)
                    return 1
                whole = typed + suggestion
                exact = normal(whole) == normal(hidden)
                same_mnemonic = bool(code_of(whole).split()) and code_of(whole).split()[0].lower() == mnemonic.lower()
                shown = bool(suggestion.strip()) and passes_filter(whole, mnemonics, registers, known)
                key = (place, label)
                total = totals.setdefault(key, {"n": 0, "exact": 0, "mnemonic": 0, "shown": 0, "shown_exact": 0, "seconds": 0.0})
                total["n"] += 1
                total["exact"] += exact
                total["mnemonic"] += same_mnemonic
                total["shown"] += shown
                total["shown_exact"] += shown and exact
                total["seconds"] += seconds
                results.append({"file": os.path.relpath(path, ROOT), "line": index + 1, "place": place, "context": label, "hidden": hidden.strip(),
                                "suggestion": suggestion.strip(), "exact": exact, "shown": shown, "seconds": seconds})
    print(f"{'cursor':<8} {'context':<8} {'cases':>5} {'exact':>7} {'mnemonic':>9} {'shown':>7} {'right of shown':>15} {'s/each':>7}")
    for (place, label), total in sorted(totals.items()):
        n = max(1, total["n"])
        print(f"{place:<8} {label:<8} {total['n']:>5} {100 * total['exact'] / n:>6.0f}% {100 * total['mnemonic'] / n:>8.0f}% {100 * total['shown'] / n:>6.0f}% "
              f"{100 * total['shown_exact'] / max(1, total['shown']):>14.0f}% {total['seconds'] / n:>7.2f}")
    if args.json:
        json.dump(results, open(args.json, "w"), indent=1)
    return 0

if __name__ == "__main__":
    sys.exit(main())
