#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Juan Sánchez Jurado
"""Measures the assistant's guided procedure with a model server.

  python3 tools/assistant_eval/guided_run.py --url http://127.0.0.1:8795/v1 [--osito DIR] [--runs N] [--only ID]

Each task of guided_tasks.json is a request on a fresh copy of a project: the two DASM templates (made with vcside_new) and,
with --osito, a copy of a real game. A task passes when the procedure ends in a proposal, the project assembles with it and the
lines it adds have what the task needs (regular expressions on the code, comments left out) and nothing it must not have.
Prints a line per run and the totals; --json FILE keeps every report.
"""
import argparse, json, os, re, shutil, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

def code_of(text):
    return "\n".join(line.split(";", 1)[0] for line in text.splitlines())

def make_projects(work, osito):
    new = os.path.join(ROOT, "build", "vcside_new")
    subprocess.run([new, os.path.join(work, "empty"), "empty", "dasm", "NTSC", "-"], check=True, capture_output=True)
    subprocess.run([new, os.path.join(work, "game"), "game", "dasm", "NTSC", "game"], check=True, capture_output=True)
    if osito:
        shutil.copytree(osito, os.path.join(work, "osito"), ignore=shutil.ignore_patterns(".git", ".vcsai", "build", "bin", ".claude"))

def project_file(folder):
    for name in sorted(os.listdir(folder)):
        if name.endswith(".vcsproj"):
            return os.path.join(folder, name)
    return None

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", required=True)
    parser.add_argument("--model", default="model")
    parser.add_argument("--osito")
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--only")
    parser.add_argument("--json")
    args = parser.parse_args()
    tasks = json.load(open(os.path.join(HERE, "guided_tasks.json")))
    if args.only:
        tasks = [t for t in tasks if t["id"] == args.only]
    tool = os.path.join(ROOT, "build", "vcside_guided")
    reports, passed, total, seconds = [], 0, 0, 0.0
    for run in range(args.runs):
        for task in tasks:
            if task["project"] == "osito" and not args.osito:
                continue
            with tempfile.TemporaryDirectory(prefix="vcside-eval-") as work:
                make_projects(work, args.osito if task["project"] == "osito" else None)
                project = project_file(os.path.join(work, task["project"]))
                started = time.time()
                done = subprocess.run([tool, project, task["file"], task["request"], "--url", args.url, "--model", args.model, "--kb", os.path.join(ROOT, "data", "kb")],
                                      capture_output=True, text=True, cwd=ROOT)
                took = time.time() - started
                try:
                    report = json.loads(done.stdout)
                except ValueError:
                    report = {"outcome": "crash", "message": (done.stderr or done.stdout)[-300:]}
                added = code_of(report.get("added", ""))
                missing = [p for p in task["must"] if not re.search(p, added)]
                unwanted = [p for p in task["must_not"] if re.search(p, added)]
                ok = report.get("outcome") == "proposal" and report.get("assembles") is True and not missing and not unwanted
                why = "" if ok else (report.get("outcome", "?") if report.get("outcome") != "proposal" else
                                     ("does not assemble" if report.get("assembles") is not True else "missing " + ", ".join(missing) if missing else "has " + ", ".join(unwanted)))
                print(f"{'PASS' if ok else 'FAIL'}  {task['id']:<14} {took:5.1f}s  tries {report.get('attempts', '-')}  {report.get('routine', '-'):<16} {why}", flush=True)
                report.update({"task": task["id"], "pass": ok, "why": why, "took": took, "run": run})
                reports.append(report)
                passed += ok
                total += 1
                seconds += took
    print(f"\n{passed}/{total} passed, {seconds / max(1, total):.1f}s per task")
    if args.json:
        json.dump(reports, open(args.json, "w"), indent=1)
    return 0 if passed == total else 1

if __name__ == "__main__":
    sys.exit(main())
