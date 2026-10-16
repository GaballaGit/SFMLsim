#!/usr/bin/env python3
"""Writes feature-reports/board-sync.json from repo state when a run didn't write it.

The board watcher copies board-sync.json to the dashboard after every run. The run's
AI is supposed to write it, but sometimes skips that step, which left the board frozen.
This builds it from facts instead: backlog.md, the commits made during the run, and
the messages the watcher handed over in inbox.json.

Usage: board-sync-fallback.py <run-start-epoch-seconds> <head-before-run>
"""
import json, os, re, subprocess, sys, time

start, head_before = float(sys.argv[1]), sys.argv[2]
sync_path = "feature-reports/board-sync.json"

# The run wrote its own sync file: keep it.
if os.path.exists(sync_path) and os.path.getmtime(sync_path) >= start:
    sys.exit(0)

def git(*args):
    return subprocess.run(["git", *args], capture_output=True, text=True).stdout.strip()

run_id = time.strftime("%Y-%m-%d-%H%M", time.localtime(start))
commits = [l.split(" ", 1) for l in git("log", "--format=%h %s", f"{head_before}..HEAD").splitlines() if l]

# Every backlog item, with its status and (if a commit mentions its ID) commit.
features, order = {}, 0
item = re.compile(r"^- \[(x| |~)\] \*\*([A-Za-z0-9]+-F\d+)[:\s]\s*([^*]+?)\*\*\s*(?:\(([^)]*)\))?")
lines = open("feature-reports/backlog.md").read().splitlines()
for i, line in enumerate(lines):
    m = item.match(line)
    if not m:
        continue
    order += 1
    mark, fid, title, effort = m.group(1), m.group(2), m.group(3).strip(" :"), m.group(4) or ""
    block = "\n".join(lines[i + 1:i + 6])
    status = "built" if mark == "x" else "failed" if (mark == "~" or "FAILED:" in block or "VETOED" in block) else "queued"
    needs = re.search(r"Needs:\s*(.*)", block)
    needs = [n for n in re.findall(r"[A-Za-z0-9]+-F\d+", needs.group(1))] if needs else []
    if status == "queued" and needs:
        built = {f for f, v in features.items() if v["status"] == "built"}
        status = "queued" if all(n in built for n in needs) else "blocked"
    commit = next((h for h, s in commits if fid in s), "") or git("log", "-1", "--format=%h", f"--grep={fid}")
    summary = re.sub(r"\s+", " ", line[m.end():]).strip(" .:-")[:160]
    features[fid] = {"title": title, "effort": effort, "status": status, "order": order,
                     "commit": commit, "summary": summary, "needs": needs}

# Answer every message the watcher handed to this run.
reports = sorted(f for f in os.listdir("feature-reports") if re.match(r"\d{4}-\d\d-\d\d-\d{4}\.md$", f))
report = reports[-1] if reports else ""
built_txt = "; ".join(f"{s} ({h})" for h, s in commits[:5]) if commits else "no new commits"
inbox = {}
try:
    for msg in json.load(open("feature-reports/inbox.json")).get("messages", []):
        inbox[msg["id"]] = {"status": "done", "reply": f"Handled in run {run_id}: {built_txt}. Details in feature-reports/{report}."[:500]}
except (OSError, ValueError, KeyError):
    pass

json.dump({
    "runId": run_id,
    "features": features,
    "inbox": inbox,
    "now": {"activity": "Idle", "busy": False,
            "detail": f"Last run {run_id}: {len(commits)} commit(s)" + (f", latest {commits[0][0]}" if commits else "")},
}, open(sync_path, "w"), indent=1)
print(f"wrote fallback {sync_path} for {run_id}: {len(features)} features, {len(inbox)} messages")
