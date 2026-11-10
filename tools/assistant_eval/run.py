#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Juan Sánchez Jurado
"""Measures how much the knowledge base helps a local model (docs/design-assistant.md, phase 6).

Every question is asked twice to the same model, once as it is and once with the entries the
assistant would add from the knowledge base. An answer passes when it matches all the regular
expressions in 'must' (the key facts, not the wording). Nothing here replaces reading the answers:
use --show to print them.

  python3 tools/assistant_eval/run.py --helper <llama-server> --model <file.gguf> --ground <vcside_ground> [--show]
"""
import argparse, json, os, re, subprocess, sys, time, urllib.request

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def free_port():
    import socket
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def ask(port, system, user):
    body = {"stream": False, "temperature": 0, "max_tokens": 400, "chat_template_kwargs": {"enable_thinking": False},
            "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}]}
    request = urllib.request.Request(f"http://127.0.0.1:{port}/v1/chat/completions", json.dumps(body).encode(), {"Content-Type": "application/json"})
    return json.load(urllib.request.urlopen(request, timeout=300))["choices"][0]["message"]["content"]


def passes(answer, must):
    return all(re.search(pattern, answer, re.I | re.S) for pattern in must)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--helper", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--ground", required=True, help="the vcside_ground program")
    parser.add_argument("--questions", default=os.path.join(ROOT, "tools", "assistant_eval", "questions.json"))
    parser.add_argument("--data", default=os.path.join(ROOT, "data"))
    parser.add_argument("--show", action="store_true")
    parser.add_argument("--out", help="write the results as JSON")
    args = parser.parse_args()

    questions = json.load(open(args.questions))["questions"]
    system = open(os.path.join(args.data, "assistant", "system.txt")).read()
    port = free_port()
    server = subprocess.Popen([args.helper, "-m", args.model, "--host", "127.0.0.1", "--port", str(port), "-c", "8192", "-np", "1", "--no-webui", "--no-warmup"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(180):
            try:
                if urllib.request.urlopen(f"http://127.0.0.1:{port}/health", timeout=2).status == 200:
                    break
            except Exception:
                time.sleep(1)
        rows = []
        for item in questions:
            grounding = json.loads(subprocess.check_output([args.ground, args.data, item["q"]]))
            plain = ask(port, system, item["q"])
            grounded = ask(port, system, item["q"] + ("\n\n" + grounding["block"] if grounding["block"] else ""))
            row = {"id": item["id"], "kind": item["kind"], "entries": grounding["entries"],
                   "plain": passes(plain, item["must"]), "grounded": passes(grounded, item["must"]),
                   "plain_answer": plain, "grounded_answer": grounded}
            rows.append(row)
            mark = lambda ok: "ok " if ok else "-- "
            print(f"{mark(row['plain'])}{mark(row['grounded'])}{item['kind']:8} {item['id']:22} {','.join(grounding['entries']) or '(nothing found)'}", flush=True)
            if args.show:
                print("   plain   :", plain.replace("\n", " ")[:300])
                print("   grounded:", grounded.replace("\n", " ")[:300])
    finally:
        server.terminate()
        server.wait()
    print("\nplain  grounded")
    for kind in ("name", "concept", None):
        subset = [r for r in rows if kind is None or r["kind"] == kind]
        if subset:
            print(f"{kind or 'all':8} {sum(r['plain'] for r in subset):2}/{len(subset)}  {sum(r['grounded'] for r in subset):2}/{len(subset)}")
    found = [r for r in rows if r["entries"]]
    print(f"questions where the knowledge base found something: {len(found)}/{len(rows)}")
    if args.out:
        json.dump(rows, open(args.out, "w"), indent=2, ensure_ascii=False)


if __name__ == "__main__":
    sys.exit(main())
