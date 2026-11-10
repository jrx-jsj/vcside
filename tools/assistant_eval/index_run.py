#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Juan Sánchez Jurado
"""Measures the project index: keywords alone, meaning alone, and both (docs/design-assistant.md).

For each question the answer is a place in the project (file and line range). A question counts as found
when one of the top 3 chunks overlaps one of its places (recall@3), and as first-hit when the best chunk does.
The embedding model runs in the bundled helper, as it does inside VCSIDE.

  python3 tools/assistant_eval/index_run.py --helper <llama-server> --index <vcside_index> --project <dir> \
      --questions <questions.json> --model <embedding.gguf> --pooling last --query-prefix "..." [--min 0.45]

questions.json: [{"q": "where is the joystick read", "at": [["main.asm", 120, 180]]}, ...]
"""
import argparse, json, os, subprocess, tempfile, time, urllib.request


def free_port():
    import socket
    s = socket.socket(); s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]; s.close(); return port


def overlaps(hit, places):
    return any(hit["file"] == f and hit["first"] <= last and hit["last"] >= first for f, first, last in places)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--helper", required=True)
    parser.add_argument("--index", required=True)
    parser.add_argument("--project", required=True)
    parser.add_argument("--questions", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--pooling", default="mean")
    parser.add_argument("--query-prefix", default="")
    parser.add_argument("--doc-prefix", default="")
    parser.add_argument("--min", default="0.45")
    parser.add_argument("--show", action="store_true")
    args = parser.parse_args()
    questions = json.load(open(args.questions))
    port = free_port()
    server = subprocess.Popen([args.helper, "-m", args.model, "--host", "127.0.0.1", "--port", str(port), "--embedding", "--pooling", args.pooling,
                               "-c", "2048", "-b", "2048", "-ub", "2048", "-np", "1", "--no-webui", "--no-warmup"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(180):
            try:
                if urllib.request.urlopen(f"http://127.0.0.1:{port}/health", timeout=2).status == 200:
                    break
            except Exception:
                time.sleep(1)
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
            f.write("\n".join(item["q"] for item in questions) + "\n")
            queries = f.name
        started = time.time()
        out = subprocess.run([args.index, args.project, queries, "--url", f"http://127.0.0.1:{port}/v1", "--model", "m", "--query-prefix", args.query_prefix,
                              "--doc-prefix", args.doc_prefix, "--min", args.min], capture_output=True, text=True, check=True)
        print(out.stderr.strip(), f"(indexing and answering took {time.time() - started:.1f}s)")
        rows = [json.loads(line) for line in out.stdout.splitlines()]
    finally:
        server.terminate(); server.wait()
    totals = {"keywords": [0, 0, 0], "meaning": [0, 0, 0], "both": [0, 0, 0]}
    for item, row in zip(questions, rows):
        marks = []
        for kind in ("keywords", "meaning", "both"):
            hits = row.get(kind, [])
            found = any(overlaps(h, item["at"]) for h in hits)
            first = bool(hits) and overlaps(hits[0], item["at"])
            noise = not hits
            totals[kind][0] += found; totals[kind][1] += first; totals[kind][2] += noise
            marks.append(("F" if found else "-") + ("1" if first else "-"))
        print(f"{marks[0]} {marks[1]} {marks[2]}   {item['q']}")
        if args.show:
            for kind in ("keywords", "meaning", "both"):
                print("     ", kind, [(h["file"], h["first"], h["last"]) for h in row.get(kind, [])])
    n = len(questions)
    print(f"\n{'':10} found@3  first  nothing")
    for kind, (found, first, noise) in totals.items():
        print(f"{kind:10} {found:3}/{n}   {first:3}/{n}   {noise:3}/{n}")


if __name__ == "__main__":
    main()
