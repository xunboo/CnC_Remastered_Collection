"""Exercise native INI detection and the packaged EXE without API calls."""
from pathlib import Path
import ctypes
from datetime import datetime, timezone
import json
import mmap
import os
import shutil
import struct
import subprocess
import tempfile
import time
import unittest
import uuid

import llm_bridge as bridge
from test_llm_bridge import snapshot
from test_redalert_ai import msvc_environment

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "build/ai-tests/autostart"
PACKAGE = ROOT / "build/redalert/LLMBridge.exe"


class AutomaticBridge(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not PACKAGE.is_file():
            raise RuntimeError("Build the portable LLMBridge.exe before running automatic-start tests")
        OUTPUT.mkdir(parents=True, exist_ok=True)
        environment = msvc_environment()
        cls.executable = OUTPUT / "llm_autostart_test.exe"
        subprocess.run([environment["AI_TEST_COMPILER"], "/nologo", "/EHsc", "/W4", "/WX", "/Od", "/MT",
                        "/I" + str(ROOT / "REDALERT"), "/Fo" + str(OUTPUT) + os.sep, "/Fe" + str(cls.executable),
                        str(ROOT / "tests/llm_autostart_test.cpp"), str(ROOT / "REDALERT/LLMBRIDGE.CPP"),
                        str(ROOT / "REDALERT/AILOG.CPP")], cwd=OUTPUT, env=environment, check=True)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="自动 bridge ", dir=OUTPUT)
        self.directory = Path(self.temp.name)
        self.ini = self.directory / "llm.ini"
        self.name = "auto_" + uuid.uuid4().hex
        shutil.copy2(self.executable, self.directory / self.executable.name)
        self.child = None
        self.memory = None
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
        self.kernel.OpenProcess.restype = ctypes.c_void_p
        self.kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        self.kernel.CloseHandle.argtypes = [ctypes.c_void_p]

    def configure(self, enabled=True, auto_start=True, house=4):
        self.ini.write_text("[bridge]\nenabled = " + str(enabled).lower() + "\nauto_start = " + str(auto_start).lower()
                            + "\nchannel = " + self.name + "\nhouse = " + str(house) + "\nmode = mock\n"
                            "[llm]\nauthorization = Bearer autostart-test-only\ninterval = 1\ntimeout = 1\n", encoding="utf-8")

    def start(self):
        environment = {key: value for key, value in os.environ.items()
                       if key.upper() not in {"AIBOOST_LLM", "AIBOOST_LLM_CHANNEL", "AIBOOST_LOG", "OPENAI_API_KEY"}}
        self.child = subprocess.Popen([str(self.directory / self.executable.name)], cwd=ROOT, env=environment,
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                      text=True, encoding="utf-8")
        self.assertEqual(int(self.child.stdout.readline()), self.child.pid)
        self.log = Path(self.child.stdout.readline().strip())
        self.match = self.child.stdout.readline().strip()
        self.assertEqual(self.log.parent, self.directory / "log")

    def command(self, command):
        self.child.stdin.write(command + "\n")
        self.child.stdin.flush()
        value = self.child.stdout.readline().strip()
        self.assertTrue(value, "native fixture exited before returning its result")
        return value

    def until(self, predicate, seconds=20):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(0.05)
        self.fail("automatic bridge did not reach the expected state before the deadline")

    def exited(self, pid):
        handle = self.kernel.OpenProcess(0x100000, False, pid)
        if not handle:
            return ctypes.get_last_error() == 87
        try:
            return self.kernel.WaitForSingleObject(handle, 0) == 0
        finally:
            self.kernel.CloseHandle(handle)

    def records(self):
        result = []
        for line in self.log.read_text(encoding="utf-8").splitlines():
            try:
                result.append(json.loads(line))
            except json.JSONDecodeError:
                continue  # The active logger may still be appending its final line.
        return result

    def boot_pid(self):
        return next((row["data"]["bridge_pid"] for row in self.records() if row["event"] == "llm_bridge_started"), None)

    def quit(self):
        self.child.stdin.write("quit\n")
        self.child.stdin.flush()
        output, errors = self.child.communicate(timeout=10)
        self.assertEqual(self.child.returncode, 0, output + errors)

    def tearDown(self):
        if self.child is not None and self.child.poll() is None:
            self.child.kill()  # Only this test fixture; backend observes its exit.
            self.child.communicate(timeout=10)
        if self.memory is not None:
            self.memory.close()
        # Wait for packaged children to release copied executable and log files.
        deadline = time.monotonic() + 10
        while True:
            try:
                self.temp.cleanup()
                break
            except PermissionError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.1)

    def test_existing_bridge_connects_using_ini_and_releases_on_disable(self):
        self.configure(auto_start=False)
        channel = bridge.SharedMemory(self.name, 4)
        try:
            self.start()
            self.assertEqual(self.command("allowed"), "blocked")
            self.assertEqual(self.command("local"), "local")
            self.until(lambda: (channel.heartbeat() or self.command("allowed")) == "allowed")
            self.assertEqual(struct.unpack_from("<I", channel.memory, 32)[0], self.child.pid)
            self.configure(enabled=False, auto_start=False)
            self.until(lambda: (channel.heartbeat() or self.command("allowed")) == "blocked")
            self.assertEqual(struct.unpack_from("<I", channel.memory, 32)[0], 0)
            self.quit()
            self.assertFalse(any(row["event"] == "llm_bridge_started" for row in self.records()))
        finally:
            channel.close()

    def test_packaged_exe_starts_without_shell_and_returns_plan_then_stops(self):
        self.configure()
        shutil.copy2(PACKAGE, self.directory / "LLMBridge.exe")
        checked = subprocess.run([str(self.directory / "LLMBridge.exe"), "--check-config"], cwd=ROOT,
                                 capture_output=True, text=True, encoding="utf-8", timeout=15)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        self.assertNotIn("autostart-test-only", checked.stdout + checked.stderr)
        self.start()
        self.assertEqual(self.command("allowed"), "blocked")
        self.assertFalse(any(self.directory.glob("log/bridge_*.log")))
        self.assertEqual(self.command("local"), "local")
        self.until(lambda: self.command("allowed") == "allowed")
        exported = snapshot()
        exported["match_id"] = self.match
        path = OUTPUT / (self.name + ".json")
        path.write_text(json.dumps(exported), encoding="ascii")
        self.assertEqual(self.command("publish " + str(path)), "published")
        received = []
        self.until(lambda: (received.append(self.command("receive")) or received[-1]) not in {"none"})
        self.assertEqual(received[-1], "7:2:0")
        self.until(lambda: self.boot_pid() is not None)
        pid = self.boot_pid()
        self.assertFalse(self.exited(pid))
        self.quit()
        self.until(lambda: self.exited(pid), 10)
        events = {row["event"] for row in self.records()}
        self.assertTrue({"llm_bridge_started", "llm_bridge_connected", "llm_mock_request",
                         "llm_plan_submitted", "llm_command_received"} <= events)
        self.assertNotIn("llm_request", events)
        status = next(self.directory.glob("log/bridge_*.log")).read_text(encoding="utf-8")
        self.assertIn("LLM bridge ready", status)
        self.assertIn("Bridge stopped", status)
        for line in status.splitlines():
            self.assertTrue(line.startswith("["), line)
            self.assertEqual(datetime.fromisoformat(line[1:line.index("]")]).tzinfo, timezone.utc)
        self.assertNotIn("authorization", status)

    def test_missing_portable_exe_keeps_native_control_and_logs_failure(self):
        self.configure()
        self.start()
        self.assertEqual(self.command("local"), "local")
        self.until(lambda: (self.command("allowed") == "blocked") and any(
            row["event"] == "llm_bridge_executable_missing" for row in self.records()))
        self.quit()
        self.assertTrue(any(row["data"].get("windows_error_or_exit_code") == 2 for row in self.records()))

    def test_packaged_bridge_exits_when_game_process_dies(self):
        self.configure()
        shutil.copy2(PACKAGE, self.directory / "LLMBridge.exe")
        self.start()
        self.command("local")
        self.until(lambda: self.command("allowed") == "allowed")
        self.until(lambda: self.boot_pid() is not None)
        pid = self.boot_pid()
        self.child.kill()
        self.child.communicate(timeout=10)
        self.until(lambda: self.exited(pid), 10)

    def test_ini_house_filters_a_manual_auto_select_bridge(self):
        self.configure(auto_start=False, house=3)
        channel = bridge.SharedMemory(self.name, -1)
        try:
            self.start()
            self.command("local")
            self.until(lambda: (channel.heartbeat() or self.command("allowed")) == "blocked"
                       and struct.unpack_from("<I", channel.memory, 32)[0] == self.child.pid)
            self.configure(auto_start=False, house=4)
            self.until(lambda: (channel.heartbeat() or self.command("allowed")) == "allowed")
            self.quit()
        finally:
            channel.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
