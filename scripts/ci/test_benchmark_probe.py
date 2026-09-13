#!/usr/bin/env python3
"""Synthetic contract reconstruction, not recovered historical test bytes."""
import contextlib
import copy
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import benchmark_probe as probe


class ProbeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.put("sys/kernel/sched_schedstats", "1")
        for pid, group, tids in ((100, 100, (100, 101)), (200, 100, (200,)),
                                 (999, 999, (999,))):
            self.put(f"{pid}/stat", self.stat(pid, group, pid + 400))
            self.put(f"{pid}/status", "VmRSS: 12 kB\nName: PRIVATE_STATUS\n")
            (self.root / str(pid) / "fd").mkdir()
            for tid in tids:
                self.put(f"{pid}/task/{tid}/stat", self.stat(tid, group, tid + 400))
                self.put(f"{pid}/task/{tid}/status", "Cpus_allowed_list: 0-2,4\n"
                         "voluntary_ctxt_switches: 10\nnonvoluntary_ctxt_switches: 20\n")
                self.put(f"{pid}/task/{tid}/schedstat", "1000 2000 3")

    @staticmethod
    def stat(pid, group, start):
        # Linux stat fields 3..22; comm deliberately contains a false ') ' delimiter.
        return f"{pid} (PRIVATE_COMM ) worker) " + " ".join(
            ["S", "1", str(group)] + ["0"] * 16 + [str(start)])

    def put(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    def snap(self):
        with mock.patch.object(probe.time, "monotonic_ns", return_value=1000):
            return probe.snapshot(100, self.root)

    def task(self, sample, pid="100", tid="101"):
        return sample["processes"][pid]["tasks"][tid]

    def endpoints(self):
        before = self.snap()
        self.assertTrue(before["complete"], before["errors"])
        after = copy.deepcopy(before)
        after["captured_monotonic_ns"] += 100
        self.assertTrue(probe.summarize(before, after)["complete"])
        return before, after

    def incomplete(self, sample):
        self.assertIs(sample["complete"], False)
        self.assertTrue(sample["errors"])
        self.assertIsNone(sample["rss_kb"])
        self.assertNotIn("PRIVATE", json.dumps(sample, allow_nan=False))

    def invalid_summary(self, before, after):
        result = probe.summarize(before, after)
        self.assertIs(result["complete"], False)
        self.assertTrue(result["errors"])
        for key in ("pgid", "leader_starttime", "elapsed_ns", "context_switch_delta",
                    "runqueue_wait_ns", "runtime_ns"):
            self.assertIsNone(result[key], key)
        self.assertNotIn("PRIVATE", json.dumps(result, allow_nan=False))

    def changed_read(self, relative, replacement, after=1):
        original, calls = Path.read_text, 0
        def read(path, *args, **kwargs):
            nonlocal calls
            if path == self.root / relative:
                calls += 1
                if calls > after:
                    if isinstance(replacement, Exception):
                        raise replacement
                    return replacement
            return original(path, *args, **kwargs)
        return mock.patch.object(Path, "read_text", read)

    def test_snapshot_schema_group_filter_and_tricky_comm(self):
        sample = self.snap()
        self.assertTrue(sample["complete"], sample["errors"])
        self.assertEqual((sample["schema_version"], sample["pgid"],
                          sample["leader_starttime"], sample["captured_monotonic_ns"]),
                         (1, 100, 500, 1000))
        self.assertEqual(set(sample["processes"]), {"100", "200"})
        self.assertEqual(set(sample["processes"]["100"]["tasks"]), {"100", "101"})
        self.assertEqual(sample["rss_kb"], 24)
        self.assertIn("double-count", sample["rss_semantics"])
        self.assertEqual(self.task(sample)["cpus_allowed_list"], "0-2,4")
        self.assertEqual(self.task(sample)["voluntary_ctxt_switches"], 10)
        self.assertEqual(self.task(sample)["nonvoluntary_ctxt_switches"], 20)
        self.assertEqual(self.task(sample)["starttime"], 501)

    def test_invalid_pgid_rejected(self):
        for value in (0, -1, True, False, "100", 100.0, None):
            with self.subTest(value=value), self.assertRaises(ValueError):
                probe.snapshot(value, self.root)

    def test_missing_leader_is_incomplete(self):
        (self.root / "100/stat").unlink()
        sample = self.snap()
        self.incomplete(sample)
        self.assertIsNone(sample["leader_starttime"])
        self.assertIn("200", sample["processes"])

    def test_unrelated_transient_entry_is_skipped(self):
        (self.root / "888").mkdir()
        self.assertTrue(self.snap()["complete"])

    def test_known_member_disappearance_is_not_silently_skipped(self):
        with self.changed_read("200/stat", FileNotFoundError("PRIVATE path")):
            self.incomplete(self.snap())

    def test_leader_reuse_during_snapshot_is_incomplete(self):
        with self.changed_read("100/stat", self.stat(100, 100, 9999)):
            self.incomplete(self.snap())

    def test_task_disappearance_is_incomplete(self):
        with self.changed_read("100/task/101/stat", ProcessLookupError("PRIVATE")):
            self.incomplete(self.snap())
        with self.changed_read("100/task/101/stat", FileNotFoundError("PRIVATE"), after=0):
            sample = self.snap()
            self.incomplete(sample)
            self.assertIsNone(self.task(sample)["starttime"])

    def test_task_group_mismatch_is_incomplete(self):
        for identity in (self.stat(101, 999, 501), self.stat(102, 100, 501)):
            self.put("100/task/101/stat", identity)
            self.incomplete(self.snap())

    def test_missing_counter_is_null_and_incomplete(self):
        self.put("100/task/101/status", "Cpus_allowed_list: 0\n")
        sample = self.snap()
        self.incomplete(sample)
        for key in probe.COUNTERS:
            self.assertIsNone(self.task(sample)[key])

    def test_negative_snapshot_counters_are_null(self):
        self.put("100/task/101/status", "Cpus_allowed_list: 0\n"
                 "voluntary_ctxt_switches: -1\nnonvoluntary_ctxt_switches: -2\n")
        sample = self.snap()
        self.incomplete(sample)
        for key in probe.COUNTERS:
            self.assertIsNone(self.task(sample)[key])

    def test_permission_failure_is_diagnostic_without_exception_text(self):
        for path in ("100/status", "100/stat", "100/task/101/status"):
            with self.subTest(path=path), self.changed_read(
                    path, PermissionError("PRIVATE credential /secret"), after=0):
                self.incomplete(self.snap())

    def test_fd_backend_counts_do_not_leak_targets(self):
        targets = ("anon_inode:[io_uring]", "anon_inode:io_uring", "anon_inode:[eventpoll]",
                   "anon_inode:[eventfd]", "/PRIVATE/path", "socket:[PRIVATE]",
                   "anon_inode:[PRIVATE]", "anon_inode:[eventfd]PRIVATE")
        for fd, target in enumerate(targets):
            (self.root / "100/fd" / str(fd)).symlink_to(target)
        sample = self.snap()
        self.assertTrue(sample["complete"])
        self.assertEqual(sample["processes"]["100"]["fd_counts"],
                         {"io_uring": 2, "eventpoll": 1, "eventfd": 1})
        self.assertNotIn("PRIVATE", json.dumps(sample))

    def test_fd_permission_failure_produces_null_counts(self):
        (self.root / "100/fd/0").symlink_to("anon_inode:[eventfd]")
        with mock.patch.object(probe.os, "readlink", side_effect=PermissionError("PRIVATE")):
            sample = self.snap()
        self.incomplete(sample)
        self.assertEqual(sample["processes"]["100"]["fd_counts"],
                         {"io_uring": None, "eventpoll": None, "eventfd": None})

    def test_only_allowlisted_files_are_read(self):
        original, seen = Path.read_text, set()
        for name in ("environ", "cmdline", "net/tcp"):
            self.put("100/" + name, "PRIVATE")
        def read(path, *args, **kwargs):
            seen.add(path.name)
            self.assertIn(path.name, {"stat", "status", "schedstat", "sched_schedstats"})
            self.assertTrue(path.is_relative_to(self.root))
            return original(path, *args, **kwargs)
        with mock.patch.object(Path, "read_text", read):
            self.assertTrue(self.snap()["complete"])
        self.assertEqual(seen, {"stat", "status", "schedstat", "sched_schedstats"})

    def test_missing_optional_schedstat_is_null_not_zero(self):
        (self.root / "100/task/101/schedstat").unlink()
        sample = self.snap()
        self.incomplete(sample)
        self.assertIsNone(self.task(sample)["runtime_ns"])
        self.assertIsNone(self.task(sample)["wait_ns"])

    def test_disabled_schedstats_wait_is_unknown_not_zero(self):
        self.put("sys/kernel/sched_schedstats", "0")
        sample = self.snap()
        self.assertTrue(sample["complete"])
        self.assertIs(sample["schedstats_enabled"], False)
        self.assertIsNone(self.task(sample)["wait_ns"])
        self.assertEqual(self.task(sample)["runtime_ns"], 1000)

    def test_summary_disabled_wait_is_null_context_switches_still_known(self):
        self.put("sys/kernel/sched_schedstats", "0")
        before, after = self.endpoints()
        self.task(after)["voluntary_ctxt_switches"] += 7
        result = probe.summarize(before, after)
        self.assertTrue(result["complete"])
        self.assertEqual(result["context_switch_delta"], 7)
        self.assertIsNone(result["runqueue_wait_ns"])

    def test_valid_summary_and_true_zero(self):
        before, after = self.endpoints()
        result = probe.summarize(before, after)
        self.assertEqual([result[k] for k in ("elapsed_ns", "context_switch_delta",
                                             "runtime_ns", "runqueue_wait_ns")], [100, 0, 0, 0])
        self.assertEqual((result["pgid"], result["leader_starttime"]), (100, 500))

    def test_summary_sums_every_thread_once(self):
        before, after = self.endpoints()
        for increment, (pid, tid) in enumerate((("100", "100"), ("100", "101"),
                                               ("200", "200")), 1):
            for key, scale in (("voluntary_ctxt_switches", 1), ("nonvoluntary_ctxt_switches", 2),
                               ("runtime_ns", 10), ("wait_ns", 100)):
                self.task(after, pid, tid)[key] += increment * scale
        result = probe.summarize(before, after)
        self.assertTrue(result["complete"])
        self.assertEqual([result[k] for k in ("context_switch_delta", "runtime_ns",
                                             "runqueue_wait_ns")], [18, 60, 600])

    def test_summary_rejects_changed_identity_task_set_and_negative_counters(self):
        changes = [((), "pgid", 200), ((), "leader_starttime", 501), ((), "complete", False),
                   ((), "errors", ["PRIVATE"]), (("processes",), "200", None),
                   (("processes", "200"), "starttime", 900),
                   (("processes", "100", "tasks"), "101", None),
                   (("processes", "100", "tasks", "101"), "starttime", 999)]
        changes += [(("processes", "100", "tasks", "101"), key, value)
                    for key in (*probe.COUNTERS, "runtime_ns", "wait_ns")
                    for value in (None, -1, 0, True, "PRIVATE")]
        for path, key, value in changes:
            with self.subTest(path=path, key=key, value=value):
                before, after = self.endpoints()
                target = after
                for part in path:
                    target = target[part]
                if value is None:
                    del target[key]
                else:
                    target[key] = value
                self.invalid_summary(before, after)
        self.invalid_summary(None, {"PRIVATE": True})

    def test_zero_duration_summary_is_not_a_measurement(self):
        for duration in (0, -1):
            before, after = self.endpoints()
            after["captured_monotonic_ns"] = before["captured_monotonic_ns"] + duration
            self.invalid_summary(before, after)

    def test_cli_json_and_fixed_proc_root(self):
        output = io.StringIO()
        with mock.patch.object(probe, "snapshot", return_value=self.snap()) as snap:
            with contextlib.redirect_stdout(output):
                self.assertEqual(probe.main(["snapshot", "--pgid", "100"]), 0)
            snap.assert_called_once_with(100)
        self.assertTrue(json.loads(output.getvalue())["complete"])
        for args in (["snapshot", "--pgid", "0"], ["snapshot", "--pgid", "abc"],
                     ["snapshot", "--pgid", "100", "--proc-root", str(self.root)]):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                probe.main(args)
            self.assertEqual(error.exception.code, 2)

    def test_invalid_summary_json_is_diagnostic_and_does_not_echo_input(self):
        before, after = self.endpoints()
        first = self.put("before.json", json.dumps(before))
        second = self.put("after.json", json.dumps(after))
        for text in (json.dumps(before), "PRIVATE invalid", "NaN", "Infinity", "-Infinity"):
            first.write_text(text)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(probe.main(["summarize", str(first), str(second)]), 0)
            def reject(value):
                self.fail("nonfinite JSON: " + value)
            result = json.loads(output.getvalue(), parse_constant=reject)
            self.assertEqual(result["complete"], text == json.dumps(before))
            self.assertNotIn("PRIVATE", output.getvalue())
        first.unlink()
        with contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(probe.main(["summarize", str(first), str(second)]), 0)
        self.assertFalse(json.loads(output.getvalue())["complete"])


if __name__ == "__main__":
    unittest.main()
