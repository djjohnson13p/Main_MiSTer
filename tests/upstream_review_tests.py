#!/usr/bin/env python3
"""Exercise upstream review against synthetic Git history; never access the network."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/upstream_review.py"
SPEC = importlib.util.spec_from_file_location("upstream_review", SCRIPT)
review = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(review)


class UpstreamReviewTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self.git("init", "-b", "master")
        self.git("config", "user.name", "Fixture")
        self.git("config", "user.email", "fixture@example.invalid")
        self.git("config", "commit.gpgsign", "false")
        self.git("config", "core.autocrlf", "false")
        self.save("common.txt", "baseline\n")
        self.save("same.txt", "before\n")
        self.base = self.make_commit("baseline")
        self.git("remote", "add", "upstream", review.OFFICIAL_URL)
        self.git("update-ref", "refs/remotes/upstream/master", self.base)
        self.git("symbolic-ref", "refs/remotes/upstream/HEAD", "refs/remotes/upstream/master")
        self.git("checkout", "-b", "independent/main")
        self.ledger = self.repo / ".git/upstream-review.json"

    def git(self, *arguments):
        return subprocess.check_output(["git", "--no-pager", "-C", str(self.repo), *arguments], stderr=subprocess.PIPE).decode().strip()

    def save(self, name, contents):
        (self.repo / name).write_text(contents, encoding="utf-8")

    def make_commit(self, message):
        self.git("add", ".")
        self.git("commit", "-m", message)
        return self.git("rev-parse", "HEAD")

    def upstream_commit(self, contents, message="official change"):
        self.git("checkout", "master")
        for name, value in contents.items():
            self.save(name, value)
        tip = self.make_commit(message)
        self.git("update-ref", "refs/remotes/upstream/master", tip)
        self.git("checkout", "independent/main")
        return tip

    def invoke(self, *arguments):
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = review.main(["--repo", str(self.repo), *arguments])
        return code, json.loads(stdout.getvalue()) if stdout.getvalue() else None, stderr.getvalue()

    def acknowledge(self, tip):
        with mock.patch.object(review, "fetch_upstream") as fetch:
            code, report, error = self.invoke("--fetch", "--ack", tip, "--note", "Synthetic changes inspected")
        self.assertEqual((code, error), (0, ""))
        fetch.assert_called_once_with(self.repo, "master")
        return report

    def test_unacknowledged_report_leaves_source_and_ledger_untouched(self):
        self.save("common.txt", "fork\n")
        fork = self.make_commit("fork change")
        code, report, error = self.invoke()
        self.assertEqual((code, error), (0, ""))
        self.assertEqual(report["status"], "unacknowledged_baseline")
        self.assertIsNone(report["last_reviewed_upstream"])
        self.assertEqual(report["unreviewed_commits"], [])
        self.assertEqual(report["fork_only_commit_count"], 1)
        self.assertFalse(self.ledger.exists())
        self.assertEqual(self.git("rev-parse", "HEAD"), fork)
        self.assertEqual(self.git("status", "--porcelain"), "")

    def test_divergent_history_and_overlap_since_acknowledged_checkpoint(self):
        self.acknowledge(self.base)
        self.save("common.txt", "fork\n")
        fork = self.make_commit("fork change")
        upstream = self.upstream_commit({"common.txt": "official\n", "new.txt": "new\n"})
        ledger_before = self.ledger.read_bytes()
        code, report, error = self.invoke("--output", str(self.root / "report.json"))
        self.assertEqual((code, error), (0, ""))
        self.assertEqual(report["status"], "upstream_changed")
        self.assertEqual(report["merge_base"], self.base)
        self.assertEqual((report["fork_only_commit_count"], report["upstream_only_commit_count"]), (1, 1))
        self.assertEqual([item["sha"] for item in report["unreviewed_commits"]], [upstream])
        self.assertEqual(report["potentially_overlapping_paths"], ["common.txt"])
        self.assertEqual(report["upstream_changed_paths"], ["common.txt", "new.txt"])
        self.assertEqual(json.loads((self.root / "report.json").read_text()), report)
        self.assertEqual(self.ledger.read_bytes(), ledger_before)
        self.assertEqual(self.git("rev-parse", "HEAD"), fork)

    def test_ack_guards_and_unchanged_reports_never_advance_ledger(self):
        for arguments in (("--ack", self.base, "--note", "reviewed"),
                          ("--fetch", "--ack", self.base, "--note", " "),
                          ("--fetch", "--ack", self.base[:7], "--note", "reviewed"),
                          ("--fetch", "--ack", "0" * 40, "--note", "reviewed")):
            with mock.patch.object(review, "fetch_upstream"):
                code, _, error = self.invoke(*arguments)
            self.assertEqual(code, 2)
            self.assertTrue(error)
            self.assertFalse(self.ledger.exists())
        self.acknowledge(self.base)
        ledger_before = self.ledger.read_bytes()
        for _ in range(2):
            code, report, error = self.invoke()
            self.assertEqual((code, error), (0, ""))
            self.assertEqual(report["status"], "unchanged")
            self.assertFalse(report["review_required"])
            self.assertEqual(self.ledger.read_bytes(), ledger_before)

    def test_patch_equivalence_is_reported_without_a_correctness_verdict(self):
        self.acknowledge(self.base)
        self.save("same.txt", "after\n")
        fork = self.make_commit("fork implementation")
        upstream = self.upstream_commit({"same.txt": "after\n"}, "upstream implementation")
        _, report, _ = self.invoke()
        self.assertEqual(report["patch_equivalent_upstream_commits"], [upstream])
        self.assertEqual(report["patch_equivalent_fork_commits"], [fork])
        self.assertIn("do not establish correctness", report["evidence_limit"])

    def test_upstream_rewind_is_visible_and_never_silently_acknowledged(self):
        upstream = self.upstream_commit({"new.txt": "new\n"})
        self.acknowledge(upstream)
        ledger_before = self.ledger.read_bytes()
        self.git("update-ref", "refs/remotes/upstream/master", self.base)
        _, report, _ = self.invoke()
        self.assertEqual(report["status"], "upstream_history_rewritten")
        self.assertEqual([item["sha"] for item in report["removed_reviewed_commits"]], [upstream])
        self.assertEqual(self.ledger.read_bytes(), ledger_before)

    def test_unexpected_effective_remote_and_worktree_output_are_refused(self):
        self.git("config", "url.https://unexpected.invalid/.insteadOf", "https://github.com/")
        with mock.patch.object(review, "fetch_upstream") as fetch:
            code, _, error = self.invoke("--fetch")
        self.assertEqual(code, 2)
        self.assertIn("Refusing upstream", error)
        fetch.assert_not_called()
        self.git("config", "--unset", "url.https://unexpected.invalid/.insteadOf")
        code, _, _ = self.invoke("--output", str(self.repo / "common.txt"))
        self.assertEqual(code, 2)
        self.assertEqual((self.repo / "common.txt").read_text(), "baseline\n")

    def test_git_metadata_cannot_be_overwritten_by_requested_reports(self):
        for name in ("HEAD", "config", "objects/blocked.json"):
            target = self.repo / ".git" / name
            before = target.read_bytes() if target.exists() else None
            code, _, error = self.invoke("--output", str(target))
            self.assertEqual(code, 2)
            self.assertIn("Refusing Git metadata write", error)
            self.assertEqual(target.read_bytes() if target.exists() else None, before)
        permitted = self.repo / ".git/upstream-review/report.json"
        code, report, error = self.invoke("--output", str(permitted))
        self.assertEqual((code, error), (0, ""))
        self.assertEqual(json.loads(permitted.read_text()), report)

    def test_nested_repo_path_still_protects_the_entire_source_worktree(self):
        nested = self.repo / "nested"
        nested.mkdir()
        output = self.repo / "common.txt"
        code, _, error = self.invoke("--repo", str(nested), "--output", str(output))
        self.assertEqual(code, 2)
        self.assertIn("outside the source worktree", error)
        self.assertEqual(output.read_text(), "baseline\n")


if __name__ == "__main__":
    unittest.main()
