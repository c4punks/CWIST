#!/usr/bin/env python3
"""Read-only endpoint telemetry for a supervisor-owned, unreaped process group.

Not an atomic sample or an ownership verifier. RSS sums double-count shared pages.
Only allowlisted status fields and anonymous-fd backend counts leave this module.
"""
import argparse
import json
import os
from pathlib import Path
import re
import time


COUNTERS = ("voluntary_ctxt_switches", "nonvoluntary_ctxt_switches")
BACKENDS = ("io_uring", "eventpoll", "eventfd")


def _stat(text):
    prefix, fields = text.rsplit(") ", 1)
    fields = fields.split()
    return int(prefix.split(" (", 1)[0]), int(fields[2]), int(fields[19])


def snapshot(pgid, proc_root=Path("/proc")):
    """Return JSON-safe diagnostics; proc_root injection is for synthetic tests."""
    if type(pgid) is not int or pgid <= 0:
        raise ValueError("pgid must be a positive integer")
    root, errors = Path(proc_root), []

    def attempt(label, operation):
        try:
            return operation()
        except (OSError, ValueError, IndexError, UnicodeError):
            errors.append(label + ": unavailable")
            return None

    def identity(path):
        value = _stat((path / "stat").read_text())
        if value[0] != int(path.name) or value[2] < 0:
            raise ValueError("invalid identity")
        return value

    def entries(path, label):
        return attempt(label, lambda: sorted(
            (p for p in path.iterdir() if re.fullmatch(r"[0-9]+", p.name)),
            key=lambda p: int(p.name)))

    def members():
        found = {}
        for path in entries(root, "process enumeration") or []:
            try:
                value = identity(path)
            except (FileNotFoundError, ProcessLookupError):
                continue  # Not yet known to belong to our group.
            except (OSError, ValueError, IndexError, UnicodeError):
                errors.append("process identity: unavailable")
                continue
            if value[1] == pgid:
                found[path.name] = value
        return found

    def status(path, label):
        text = attempt(label, lambda: path.read_text())
        return dict(line.split(":", 1) for line in (text or "").splitlines() if ":" in line)

    def field(values, key, label, pattern=r"[0-9]+"):
        value = values.get(key, "").strip()
        if not re.fullmatch(pattern, value):
            errors.append(label + ": missing or invalid " + key)
            return None
        return value

    def number(values, key, label):
        value = field(values, key, label)
        return int(value) if value is not None else None

    enabled = attempt("schedstats setting", lambda: (
        root / "sys/kernel/sched_schedstats").read_text().strip())
    if enabled not in ("0", "1"):
        errors.append("schedstats setting: unknown")
    enabled = {"0": False, "1": True}.get(enabled)
    initial = members()
    leader = initial.get(str(pgid))
    if leader is None:
        errors.append("leader: missing")
    processes = {}
    for pid, ident in initial.items():
        path, label = root / pid, "pid " + pid
        values = status(path / "status", label)
        rss = field(values, "VmRSS", label, r"[0-9]+\s+kB")
        tasks = {}
        paths = entries(path / "task", label + " tasks")
        for task_path in paths or []:
            tid, task_label = task_path.name, label + " tid " + task_path.name
            task_ident = attempt(task_label + " identity", lambda: identity(task_path))
            if task_ident is not None and task_ident[1] != pgid:
                errors.append(task_label + " group changed")
            values = status(task_path / "status", task_label)
            task = {"tid": int(tid), "starttime": task_ident[2] if task_ident else None,
                    "cpus_allowed_list": field(values, "Cpus_allowed_list", task_label,
                                               r"[0-9]+(?:-[0-9]+)?(?:,[0-9]+(?:-[0-9]+)?)*"),
                    **{key: number(values, key, task_label) for key in COUNTERS},
                    "runtime_ns": None, "wait_ns": None}
            sched = attempt(task_label + " schedstat", lambda: [
                int(n) for n in (task_path / "schedstat").read_text().split()])
            if sched is not None and len(sched) >= 2 and min(sched) >= 0:
                task.update(runtime_ns=sched[0], wait_ns=sched[1] if enabled else None)
            elif sched is not None:
                errors.append(task_label + " schedstat: invalid")
            if attempt(task_label + " identity recheck", lambda: identity(task_path)) != task_ident:
                errors.append(task_label + " identity changed")
            tasks[tid] = task
        final_tasks = entries(path / "task", label + " tasks recheck")
        if not paths or final_tasks != paths:
            errors.append(label + " task set missing or changed")
        counts = dict.fromkeys(BACKENDS, 0)
        fds = entries(path / "fd", label + " fds")
        for fd in fds or []:
            target = attempt(label + " fd read", lambda: os.readlink(fd))
            if target is None:
                counts = dict.fromkeys(BACKENDS, None)
                break
            for backend in BACKENDS:
                if target in ("anon_inode:[" + backend + "]", "anon_inode:" + backend):
                    counts[backend] += 1
        if fds is None:
            counts = dict.fromkeys(BACKENDS, None)
        if attempt(label + " identity recheck", lambda: identity(path)) != ident:
            errors.append(label + " identity changed")
        processes[pid] = {"pid": int(pid), "starttime": ident[2], "tasks": tasks,
                          "rss_kb": int(rss.split()[0]) if rss else None, "fd_counts": counts}
    if members() != initial:
        errors.append("process membership or identity changed")
    rss_values = [p["rss_kb"] for p in processes.values()]
    return {"schema_version": 1, "pgid": pgid, "leader_starttime": leader[2] if leader else None,
            "captured_monotonic_ns": time.monotonic_ns(), "schedstats_enabled": enabled,
            "processes": processes, "rss_kb": sum(rss_values) if rss_values and not errors else None,
            "rss_semantics": "sum of process VmRSS kB; shared pages double-counted",
            "complete": not errors, "errors": errors}


def summarize(before, after):
    """Strict endpoint deltas; membership/reuse/decreases invalidate all totals."""
    result = {"schema_version": 1, "complete": False, "errors": [], "pgid": None,
              "leader_starttime": None, "elapsed_ns": None,
              "context_switch_delta": None, "runqueue_wait_ns": None, "runtime_ns": None}

    def integer(value):
        if type(value) is not int or value < 0:
            raise ValueError("invalid numeric field")
        return value

    try:
        for sample in (before, after):
            if sample["complete"] is not True or sample["errors"]:
                raise ValueError("incomplete endpoint")
            integer(sample["leader_starttime"])
            if integer(sample["pgid"]) == 0 or not sample["processes"]:
                raise ValueError("invalid group")
            if sample["processes"][str(sample["pgid"])]["starttime"] != sample["leader_starttime"]:
                raise ValueError("leader mismatch")
        if any(before[key] != after[key] for key in ("pgid", "leader_starttime")):
            raise ValueError("leader changed")
        if before["processes"].keys() != after["processes"].keys():
            raise ValueError("process set changed")
        totals = dict.fromkeys((*COUNTERS, "runtime_ns", "wait_ns"), 0)
        wait_enabled = before["schedstats_enabled"] is True and after["schedstats_enabled"] is True
        for pid, process in before["processes"].items():
            other = after["processes"][pid]
            if integer(process["starttime"]) != integer(other["starttime"]):
                raise ValueError("process reused")
            if not process["tasks"] or process["tasks"].keys() != other["tasks"].keys():
                raise ValueError("task set changed")
            for tid, task in process["tasks"].items():
                end = other["tasks"][tid]
                if integer(task["starttime"]) != integer(end["starttime"]):
                    raise ValueError("task reused")
                for key in totals:
                    if key == "wait_ns" and not wait_enabled:
                        continue
                    delta = integer(end[key]) - integer(task[key])
                    if delta < 0:
                        raise ValueError("counter decreased")
                    totals[key] += delta
        elapsed = integer(after["captured_monotonic_ns"]) - integer(before["captured_monotonic_ns"])
        if elapsed <= 0:
            raise ValueError("measurement duration must be positive")
        result.update(complete=True, pgid=before["pgid"], leader_starttime=before["leader_starttime"],
                      elapsed_ns=elapsed, context_switch_delta=sum(totals[k] for k in COUNTERS),
                      runqueue_wait_ns=totals["wait_ns"] if wait_enabled else None,
                      runtime_ns=totals["runtime_ns"])
    except (KeyError, TypeError, ValueError, AttributeError):
        result["errors"].append("incomplete, invalid, or non-comparable endpoints")
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    snap = commands.add_parser("snapshot")
    snap.add_argument("--pgid", type=int, required=True)
    summary = commands.add_parser("summarize")
    summary.add_argument("before", type=Path)
    summary.add_argument("after", type=Path)
    args = parser.parse_args(argv)
    if args.command == "snapshot":
        if args.pgid <= 0:
            parser.error("pgid must be a positive integer")
        result = snapshot(args.pgid)
    else:
        try:
            def reject_constant(value):
                raise ValueError("non-standard JSON")
            result = summarize(*(json.loads(p.read_text(), parse_constant=reject_constant)
                                 for p in (args.before, args.after)))
        except (OSError, ValueError, UnicodeError):
            result = summarize(None, None)
    print(json.dumps(result, allow_nan=False, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
