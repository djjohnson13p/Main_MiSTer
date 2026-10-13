#!/usr/bin/env python3
"""Report official MiSTer upstream changes without merging or changing source files."""
import argparse
import datetime
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


OFFICIAL_URL = "https://github.com/MiSTer-devel/Main_MiSTer.git"
OFFICIAL_URLS = {OFFICIAL_URL, OFFICIAL_URL.removesuffix(".git")}
REPOSITORY = "MiSTer-devel/Main_MiSTer"
SHA = re.compile(r"[0-9a-f]{40}\Z")


class ReviewError(Exception):
    pass


def git(repo, *arguments, allow_missing=False):
    env = dict(os.environ, GIT_TERMINAL_PROMPT="0")
    try:
        result = subprocess.run(
            ["git", "--no-pager", "-C", str(repo), *arguments],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, timeout=60,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise ReviewError("Git command failed or timed out") from exc
    if result.returncode and not (allow_missing and result.returncode == 1):
        message = result.stderr.decode("utf-8", errors="replace").strip()
        raise ReviewError(message or "Git command failed")
    return result.stdout.decode("utf-8", errors="replace")


def verify_upstream(repo):
    # Check configured and effective URLs; get-url expands insteadOf rewrites.
    configured = git(repo, "config", "--get-all", "remote.upstream.url", allow_missing=True).splitlines()
    effective = git(repo, "remote", "get-url", "--all", "upstream").splitlines()
    if len(configured) != 1 or len(effective) != 1 or any(
        url not in OFFICIAL_URLS for url in configured + effective
    ):
        raise ReviewError("Refusing upstream: its configured/effective URL must be the official MiSTer repository")
    symbolic = git(repo, "symbolic-ref", "-q", "refs/remotes/upstream/HEAD", allow_missing=True).strip()
    prefix = "refs/remotes/upstream/"
    if symbolic and (not symbolic.startswith(prefix) or symbolic == prefix + "HEAD"):
        raise ReviewError("Unexpected upstream default-branch reference")
    branch = symbolic[len(prefix):] if symbolic else "master"
    git(repo, "check-ref-format", "refs/heads/" + branch)
    return branch, prefix + branch


def fetch_upstream(repo, branch):
    # Explicit refspec updates only this official remote-tracking branch.
    git(
        repo, "-c", "protocol.file.allow=never", "-c", "protocol.ext.allow=never",
        "fetch", "--no-tags", "--no-recurse-submodules", "--no-auto-maintenance",
        "upstream", "+refs/heads/" + branch + ":refs/remotes/upstream/" + branch,
    )


def commit(repo, revision):
    return git(repo, "rev-parse", "--verify", revision + "^{commit}").strip()


def paths(repo, *arguments):
    return sorted(set(filter(None, git(repo, *arguments).split("\0"))))


def commits(repo, start, end):
    result = []
    for oid in git(repo, "rev-list", "--reverse", start + ".." + end).splitlines():
        subject, date = git(repo, "show", "-s", "--format=%s%x00%aI", oid).strip().split("\0", 1)
        changed = paths(repo, "diff-tree", "--root", "-m", "--no-commit-id", "--name-only", "-r", "-z", oid)
        result.append({"sha": oid, "subject": subject, "author_date": date, "changed_paths": changed})
    return result


def load_ledger(path, repo, branch):
    if not path.exists():
        return None
    try:
        ledger = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise ReviewError("Review ledger is unreadable or invalid JSON") from exc
    if not isinstance(ledger, dict) or ledger.get("schema_version") != 1 or ledger.get("repository") != REPOSITORY:
        raise ReviewError("Review ledger has an unexpected schema or repository")
    if ledger.get("upstream_branch") != branch or not SHA.fullmatch(str(ledger.get("last_reviewed_upstream", ""))):
        raise ReviewError("Review ledger has an unexpected branch or checkpoint")
    if not isinstance(ledger.get("reviews"), list):
        raise ReviewError("Review ledger is missing its acknowledgement history")
    commit(repo, ledger["last_reviewed_upstream"])
    return ledger


def build_report(repo, branch, upstream_ref, ledger, fetched):
    fork = commit(repo, "HEAD")
    upstream = commit(repo, upstream_ref)
    common = git(repo, "merge-base", fork, upstream).strip()
    checkpoint = ledger["last_reviewed_upstream"] if ledger else None
    start = checkpoint or common
    changes = commits(repo, start, upstream)
    removed = commits(repo, upstream, checkpoint) if checkpoint else []
    rewritten = bool(removed)
    upstream_paths = sorted({path for change in changes for path in change["changed_paths"]})
    fork_paths = paths(repo, "diff", "--name-only", "-z", common, fork)
    ahead, behind = map(int, git(repo, "rev-list", "--left-right", "--count", fork + "..." + upstream).split())
    unreviewed = {change["sha"] for change in changes}
    equivalent_upstream = [line.split()[1] for line in git(repo, "cherry", fork, upstream).splitlines()
                           if line.startswith("- ") and line.split()[1] in unreviewed]
    equivalent_fork = [line.split()[1] for line in git(repo, "cherry", upstream, fork).splitlines()
                       if line.startswith("- ")]
    status = "upstream_history_rewritten" if rewritten else (
        "unacknowledged_baseline" if not ledger else "upstream_changed" if changes else "unchanged"
    )
    return {
        "schema_version": 1,
        "generated_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "repository": REPOSITORY,
        "upstream_url": OFFICIAL_URL,
        "upstream_branch": branch,
        "fetch_performed": fetched,
        "fork_head": fork,
        "upstream_head": upstream,
        "merge_base": common,
        "last_reviewed_upstream": checkpoint,
        "review_range_base": start,
        "review_range_basis": "explicit acknowledgement" if ledger else "merge-base; no review acknowledged",
        "status": status,
        "review_required": status != "unchanged",
        "fork_only_commit_count": ahead,
        "upstream_only_commit_count": behind,
        "unreviewed_commits": changes,
        "upstream_history_rewritten": rewritten,
        "removed_reviewed_commits": removed,
        "upstream_changed_paths": paths(repo, "diff", "--name-only", "-z", start, upstream),
        "upstream_paths_touched": upstream_paths,
        "fork_changed_paths": fork_paths,
        "potentially_overlapping_paths": sorted(set(upstream_paths) & set(fork_paths)),
        "patch_equivalent_upstream_commits": equivalent_upstream,
        "patch_equivalent_fork_commits": equivalent_fork,
        "evidence_limit": "Path overlap and git cherry patch equivalence route review; they do not establish correctness or which implementation is better.",
    }


def private_or_external_path(repo, path, ledger=False):
    path = path.resolve()
    git_dir = Path(git(repo, "rev-parse", "--absolute-git-dir").strip()).resolve()
    common_dir = Path(git(repo, "rev-parse", "--git-common-dir").strip())
    if not common_dir.is_absolute():
        common_dir = repo / common_dir
    if path.is_relative_to(git_dir) or path.is_relative_to(common_dir.resolve()):
        allowed = (ledger and path == git_dir / "upstream-review.json") or (
            path.is_relative_to(git_dir / "upstream-review") and path.suffix == ".json"
        )
        if not allowed:
            raise ReviewError("Refusing Git metadata write: use the default review ledger or JSON files under upstream-review/")
    elif path.is_relative_to(repo):
        raise ReviewError("Report and ledger files must be outside the source worktree or inside Git metadata")
    return path


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent, delete=False) as output:
            temporary = Path(output.name)
            json.dump(value, output, indent=2)
            output.write("\n")
        os.replace(temporary, path)
    finally:
        if temporary and temporary.exists():
            temporary.unlink()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--fetch", action="store_true", help="Fetch the verified official upstream branch before reporting")
    parser.add_argument("--ledger", type=Path, help="Private/external review ledger; default is Git-private upstream-review.json")
    parser.add_argument("--output", type=Path, help="Write a JSON report outside the worktree or inside Git metadata")
    parser.add_argument("--ack", metavar="EXPECTED_SHA", help="Explicitly acknowledge review of this exact full upstream SHA; requires --fetch and --note")
    parser.add_argument("--note", help="Nonempty review decision/evidence note for --ack")
    args = parser.parse_args(argv)
    try:
        if args.ack and (not args.fetch or not args.note or not args.note.strip() or not SHA.fullmatch(args.ack)):
            raise ReviewError("Acknowledgement requires --fetch, a nonempty --note, and an exact 40-hex upstream SHA")
        if args.note is not None and not args.ack:
            raise ReviewError("--note is only valid with --ack")
        repo = Path(git(args.repo.resolve(), "rev-parse", "--show-toplevel").strip()).resolve()
        branch, upstream_ref = verify_upstream(repo)
        default_ledger = Path(git(repo, "rev-parse", "--git-path", "upstream-review.json").strip())
        if not default_ledger.is_absolute():
            default_ledger = repo / default_ledger
        ledger_path = private_or_external_path(repo, args.ledger or default_ledger, ledger=True)
        output_path = private_or_external_path(repo, args.output) if args.output else None
        if output_path == ledger_path:
            raise ReviewError("Report output and review ledger must be different files")
        if args.fetch:
            fetch_upstream(repo, branch)
        ledger = load_ledger(ledger_path, repo, branch)
        report = build_report(repo, branch, upstream_ref, ledger, args.fetch)
        report["ledger_path"] = str(ledger_path)
        if args.ack:
            if args.ack != report["upstream_head"] or args.ack != commit(repo, upstream_ref):
                raise ReviewError("Acknowledgement rejected: expected SHA does not match the fetched upstream tip")
            review = {"upstream_sha": args.ack, "acknowledged_at_utc": report["generated_at_utc"], "note": args.note.strip()}
            updated = {
                "schema_version": 1, "repository": REPOSITORY, "upstream_branch": branch,
                "last_reviewed_upstream": args.ack, "reviews": (ledger["reviews"] if ledger else []) + [review],
            }
            write_json(ledger_path, updated)
            report["acknowledgement"] = review
        if output_path:
            write_json(output_path, report)
        print(json.dumps(report, indent=2))
        return 0
    except (ReviewError, OSError) as exc:
        print("upstream review: " + str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
