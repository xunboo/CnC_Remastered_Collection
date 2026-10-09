"""Test the configured real model's tool calls and C++ tactical execution.

Makes real API requests only after llm.ini has valid authorization. It uses a
controlled combat fixture, not an installed game, and saves a secret-free JSON
report. --wait-config waits for the user to finish the INI file first.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time

import llm_bridge as bridge
from llm_audit import log_status, utc_timestamp
from llm_config import ConfigError, DEFAULT_CONFIG, load_config, validate_config

ROOT = Path(__file__).resolve().parents[1]
CASES = ("hold", "attack_target", "defend_area")


def expected_plan(snapshot, action, config=None):
    target = None
    if action == "attack_target":
        target = next((candidate["id"] for candidate in snapshot["target_candidates"]
                       if candidate["category"] == "power" and "ground" in candidate["reachable_by"]), None)
        if target is None:
            raise bridge.BridgeError("test fixture has no compatible power target")
    return {
        "protocol_version": bridge.PROTOCOL_VERSION, "match_id": snapshot["match_id"],
        "based_on_snapshot_seq": snapshot["snapshot_seq"],
        "controlled_house_id": snapshot["controlled_house_id"],
        "valid_for_ticks": bridge.plan_tick_limit(config) if config is not None else bridge.MAX_PLAN_TICKS,
        "orders": [{"action": action, "group_id": "ground", "target_id": target,
                    "destination_cell": [35, 80] if action == "defend_area" else None,
                    "commit_percent": 100 if action == "hold" else 75,
                    "withdraw_avg_hp_percent": 35}],
    }


def test_instructions(expected):
    return bridge.SYSTEM_PROMPT + "\nThis is an interface verification scenario in a controlled world. " \
        "Call submit_tactical_plan exactly once. Use exactly the following arguments, including null values; " \
        "do not add a rationale or other orders:\n" + json.dumps(expected, separators=(",", ":"))


def _line_reader(stream, destination):
    destination.put(stream.readline())


def read_snapshot(child, timeout=10):
    lines = queue.Queue()
    threading.Thread(target=_line_reader, args=(child.stdout, lines), daemon=True).start()
    try:
        line = lines.get(timeout=timeout)
    except queue.Empty:
        raise bridge.BridgeError("native fixture snapshot timed out") from None
    if not line:
        raise bridge.BridgeError("native fixture did not export a snapshot")
    return bridge.validate_snapshot(bridge.parse_json(line))


def run_case(config, action, executable, attempts=3, requester=bridge.request_plan):
    child = subprocess.Popen([str(executable), "--llm-pipe", action], cwd=executable.parent,
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True)
    result = {"case": action, "started_at": utc_timestamp(), "passed": False, "attempts": []}
    try:
        snapshot = read_snapshot(child)
        expected = expected_plan(snapshot, action, config)
        instructions = test_instructions(expected)
        accepted = None
        for attempt in range(1, attempts + 1):
            start = time.monotonic()
            entry = {"attempt": attempt, "started_at": utc_timestamp()}
            metadata = {}
            try:
                plan = requester(snapshot, config=config, instructions=instructions, metadata=metadata)
                bridge.validate_plan(plan, snapshot)
                if plan != expected:
                    raise bridge.BridgeError("tool arguments do not match this verification scenario")
                entry.update(passed=True, **metadata)
                accepted = plan
            except (bridge.BridgeError, ConfigError, ValueError, TypeError, KeyError) as error:
                # These messages are generated locally; never copy API bodies,
                # Request objects, headers, INI source or authorization here.
                reason = str(error) if isinstance(error, (bridge.BridgeError, ConfigError)) else "invalid response data"
                entry.update(passed=False, error=reason)
                instructions += "\nThe preceding call failed local validation: " + reason + ". Follow the exact arguments above."
            entry["elapsed_seconds"] = round(time.monotonic() - start, 3)
            entry["completed_at"] = utc_timestamp()
            result["attempts"].append(entry)
            if accepted is not None:
                break
            if entry.get("error", "").startswith(("API HTTP status 401", "API HTTP status 403", "API HTTP status 404")):
                break
        if accepted is None:
            result["error"] = "real function call did not pass validation"
            return result
        child.stdin.write(bridge.encode_plan(accepted, snapshot).hex() + "\n")
        child.stdin.flush()
        output, _ = child.communicate(timeout=10)
        if child.returncode != 0:
            raise bridge.BridgeError("native tactical controller rejected the real model plan")
        feedback = bridge.validate_snapshot(bridge.parse_json(output.strip()))
        if not any(item.get("snapshot_seq") == snapshot["snapshot_seq"] and item.get("group_id") == "ground"
                   and item.get("status") == "accepted" for item in feedback["last_plan_results"]):
            raise bridge.BridgeError("native execution did not acknowledge the function call")
        if not any(item.get("action") == action and item.get("group_id") == "ground" for item in feedback["active_plan"]):
            raise bridge.BridgeError("native active plan does not match the requested action")
        result.update(passed=True, real_function_call_passed=True, cpp_execution_passed=True,
                      plan=accepted, execution_feedback=feedback["last_plan_results"])
        return result
    except (bridge.BridgeError, ValueError, TypeError, KeyError, subprocess.TimeoutExpired):
        result["error"] = "fixture snapshot, packet or execution feedback failed"
        return result
    finally:
        if child.poll() is None:
            child.kill()
            child.communicate(timeout=10)
        result["completed_at"] = utc_timestamp()


def wait_for_config(path, wait_seconds, timeout_override=None):
    deadline = time.monotonic() + wait_seconds
    announced = False
    while True:
        try:
            return load_config(path, timeout=timeout_override)
        except ConfigError:
            if wait_seconds <= 0:
                raise
            if not announced:
                log_status("Waiting for a complete llm.ini; no API requests have been sent.")
                announced = True
            if time.monotonic() >= deadline:
                raise ConfigError("configuration wait timed out; save llm.ini and run the test again") from None
            time.sleep(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--wait-config", action="store_true", help="Wait for valid saved credentials before testing.")
    parser.add_argument("--wait-seconds", type=float, default=600)
    parser.add_argument("--attempts", type=int, default=3, help="Maximum validation attempts per case (1-5).")
    parser.add_argument("--timeout", type=float, help="Override the INI HTTP timeout (1-120 seconds).")
    parser.add_argument("--case", choices=CASES, action="append", help="Select cases; defaults to all three.")
    parser.add_argument("--skip-build", action="store_true", help="Use the existing tactics_test.exe fixture.")
    parser.add_argument("--report", type=Path, default=ROOT / "build/llm/live-test.json")
    args = parser.parse_args()
    if not 1 <= args.attempts <= 5 or not 1 <= args.wait_seconds <= 86400:
        parser.error("attempts or wait-seconds is outside the supported range")
    if args.timeout is not None and not 1 <= args.timeout <= 120:
        parser.error("timeout must be between 1 and 120 seconds")
    config = wait_for_config(args.config, args.wait_seconds if args.wait_config else 0, args.timeout)
    validate_config(config)
    log_status("Configuration is ready; credentials will not be printed or saved in the report.")
    if not args.skip_build:
        subprocess.run([sys.executable, str(ROOT / "SCRIPTS/test_redalert_ai.py"), "--test", "tactics_test"],
                       cwd=ROOT, check=True, timeout=60)
    executable = ROOT / "build/ai-tests/tactics_test.exe"
    if not executable.is_file():
        raise ConfigError("native fixture is missing; run without --skip-build")
    report = {"started_at": utc_timestamp(), "protocol": config.protocol,
              "requested_model": config.model, "real_api": True, "controlled_fixture": True, "cases": []}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    for action in args.case or CASES:
        log_status("Testing real function call and C++ execution: " + action)
        result = run_case(config, action, executable, args.attempts)
        report["cases"].append(result)
        report["passed"] = False
        report["completed"] = False
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        log_status(("PASS: " if result["passed"] else "FAIL: ") + action)
        if not result["passed"]:
            if result["attempts"]:
                log_status(result["attempts"][-1].get("error", result.get("error", "verification failed")))
            break
    report["completed"] = len(report["cases"]) == len(args.case or CASES)
    report["passed"] = report["completed"] and all(case["passed"] for case in report["cases"])
    report["completed_at"] = utc_timestamp()
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    log_status("Secret-free test report: " + str(args.report))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ConfigError, bridge.BridgeError) as error:
        log_status(str(error), file=sys.stderr)
        sys.exit(1)
    except (OSError, subprocess.SubprocessError):
        log_status("Native fixture could not be built or started.", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        log_status("Real LLM test stopped.", file=sys.stderr)
        sys.exit(130)
