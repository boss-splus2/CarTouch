#!/usr/bin/env python3
"""Collect the logs of every finished job in the current workflow run.

Runs in the last CI job (`logs`, `if: always()`), after all other jobs have
finished, so the run always ends with a downloadable "logs" artifact next to
"cartouch-firmware" and "ci-report" - whether the build passed or failed.

Output folder (default: run-logs/):
  run-summary.txt   one line per job and per step with its result
  jobs.json         the raw job/step data returned by the GitHub API
  NN_<job>.log      the full log text of each finished job

The log of this collector job itself cannot be included (it is still running
while the files are written); it is listed in the summary as "in progress".

Needs: the GitHub CLI (`gh`, pre-installed on GitHub runners), GH_TOKEN with
`actions: read`, and the standard GITHUB_REPOSITORY / GITHUB_RUN_ID /
GITHUB_RUN_ATTEMPT variables.
Exit code is always 0: a problem while collecting logs must never turn a
green build red. Problems are written into run-summary.txt instead.
"""
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
# □□□□□□□□□□ Configuration
# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

GH = os.environ.get("GH_BIN", "gh")
RETRIES = 3
RETRY_DELAY_S = int(os.environ.get("CT_LOG_RETRY_DELAY", "5"))
TIMEOUT_S = 120

# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
# □□□□□□□□□□ GitHub API access
# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

def gh_api(path, *extra):
    """Return (ok, text) for `gh api <path>`; never raises."""
    try:
        res = subprocess.run([GH, "api", path, *extra], capture_output=True,
                             timeout=TIMEOUT_S)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return False, f"{type(exc).__name__}: {exc}"
    text = res.stdout.decode("utf-8", errors="replace")
    if res.returncode != 0:
        err = res.stderr.decode("utf-8", errors="replace").strip()
        return False, err or text or f"gh exited with {res.returncode}"
    return True, text

def gh_api_retry(path, *extra):
    """Same as gh_api, but retries (logs can be a few seconds late)."""
    ok, text = False, ""
    for attempt in range(1, RETRIES + 1):
        ok, text = gh_api(path, *extra)
        if ok and text.strip():
            return True, text
        if attempt < RETRIES:
            time.sleep(RETRY_DELAY_S)
    return ok, text

def list_jobs(repo, run_id, attempt):
    """All jobs of this run attempt, as a list of dicts."""
    ok, text = gh_api_retry(f"repos/{repo}/actions/runs/{run_id}/attempts/{attempt}/jobs",
                            "--paginate", "--jq", ".jobs[]")
    if not ok:
        return None, text
    jobs = []
    for line in text.splitlines():
        line = line.strip()
        if line:
            jobs.append(json.loads(line))
    return jobs, ""

# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
# □□□□□□□□□□ Output
# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

def safe_name(text):
    """File-name safe version of a job name."""
    return re.sub(r"[^A-Za-z0-9._-]+", "_", text).strip("_") or "job"

def step_lines(job):
    lines = []
    for step in job.get("steps") or []:
        lines.append(f"    {step.get('number', '?'):>3}. "
                     f"{(step.get('conclusion') or step.get('status') or '?'):<10} "
                     f"{step.get('name', '')}")
    return lines

def collect(out_dir, repo, run_id, attempt):
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    summary = [f"Repository : {repo}",
               f"Run        : {run_id} (attempt {attempt})",
               f"Collected  : {time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime())}",
               ""]

    jobs, err = list_jobs(repo, run_id, attempt)
    if jobs is None:
        summary += ["Could not list the jobs of this run:", err]
        (out / "run-summary.txt").write_text("\n".join(summary) + "\n", encoding="utf-8")
        return 0

    (out / "jobs.json").write_text(json.dumps(jobs, indent=2), encoding="utf-8")
    jobs.sort(key=lambda j: j.get("started_at") or "")
    saved = 0
    for index, job in enumerate(jobs, start=1):
        name = job.get("name", f"job-{job.get('id')}")
        status = job.get("status")
        conclusion = job.get("conclusion") or status
        summary.append(f"[{conclusion}] {name}")
        summary += step_lines(job)

        if status != "completed":
            summary.append("    (log not included: job is still running - "
                           "this is the log collector itself)")
        elif conclusion == "skipped":
            summary.append("    (job was skipped, it has no log)")
        else:
            ok, text = gh_api_retry(f"repos/{repo}/actions/jobs/{job['id']}/logs")
            if ok and text.strip():
                fname = f"{index:02d}_{safe_name(name)}.log"
                (out / fname).write_text(text, encoding="utf-8")
                summary.append(f"    log: {fname} ({len(text)} chars)")
                saved += 1
            else:
                summary.append(f"    (log could not be downloaded: {text.strip()[:300]})")
        summary.append("")

    summary.append(f"{saved} job log(s) saved.")
    (out / "run-summary.txt").write_text("\n".join(summary) + "\n", encoding="utf-8")
    return saved

# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
# □□□□□□□□□□ Entry point
# ■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■

def main(argv):
    out_dir = argv[1] if len(argv) > 1 else "run-logs"
    repo = os.environ.get("GITHUB_REPOSITORY", "")
    run_id = os.environ.get("GITHUB_RUN_ID", "")
    attempt = os.environ.get("GITHUB_RUN_ATTEMPT", "1")
    if not repo or not run_id:
        Path(out_dir).mkdir(parents=True, exist_ok=True)
        Path(out_dir, "run-summary.txt").write_text(
            "GITHUB_REPOSITORY / GITHUB_RUN_ID not set: nothing collected.\n", encoding="utf-8")
        print("GITHUB_REPOSITORY / GITHUB_RUN_ID not set", file=sys.stderr)
        return 0
    try:
        saved = collect(out_dir, repo, run_id, attempt)
        print(f"Collected {saved} job log(s) into {out_dir}/")
    except Exception as exc:  # noqa: BLE001 - must never fail the build
        Path(out_dir).mkdir(parents=True, exist_ok=True)
        Path(out_dir, "collector-error.txt").write_text(f"{type(exc).__name__}: {exc}\n",
                                                        encoding="utf-8")
        print(f"::warning::log collection failed: {exc}")
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
