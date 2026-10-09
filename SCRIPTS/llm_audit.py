"""Append credential-filtered LLM events to the DLL's per-match JSONL file."""
from datetime import datetime, timezone
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import threading
import sys
import uuid

SENSITIVE_KEYS = {"authorization", "api_key", "apikey", "api-key", "access_token", "refresh_token", "client_secret", "token"}


def utc_timestamp():
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def log_status(message, *, file=None):
    print("[" + utc_timestamp() + "] " + message, file=file, flush=True)


class Overlapped(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_size_t), ("InternalHigh", ctypes.c_size_t),
                ("Offset", wintypes.DWORD), ("OffsetHigh", wintypes.DWORD), ("hEvent", wintypes.HANDLE)]


class MatchAudit:
    def __init__(self, path, snapshot, config):
        self.path = Path(path)
        self.snapshot = snapshot
        self.sequence = 0
        self.request_id = uuid.uuid4().hex
        self.lock = threading.Lock()
        self.warned = False
        self.secrets = [config.authorization]
        parts = config.authorization.split(" ", 1)
        if len(parts) == 2:
            self.secrets.append(parts[1])
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                           ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        self.kernel.CreateFileW.restype = wintypes.HANDLE
        self.kernel.LockFileEx.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.POINTER(Overlapped)]
        self.kernel.UnlockFileEx.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.POINTER(Overlapped)]
        self.kernel.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
        self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]

    def sanitize(self, value):
        if isinstance(value, dict):
            return {key: ("[redacted]" if key.lower() in SENSITIVE_KEYS else self.sanitize(item)) for key, item in value.items()}
        if isinstance(value, list):
            return [self.sanitize(item) for item in value]
        if isinstance(value, str):
            for secret in sorted(self.secrets, key=len, reverse=True):
                if secret:
                    value = value.replace(secret, "[redacted]")
        return value

    def record(self, event, data):
        with self.lock:
            self.sequence += 1
            row = {"log_version": 1, "timestamp": utc_timestamp(),
                   "source": "llm_bridge", "pid": os.getpid(), "seq": self.sequence,
                   "match_id": self.snapshot["match_id"], "sim_frame": self.snapshot["sim_frame"],
                   "house_id": self.snapshot["controlled_house_id"], "snapshot_seq": self.snapshot["snapshot_seq"],
                   "request_id": self.request_id, "event": event, "data": self.sanitize(data)}
            text = (json.dumps(row, separators=(",", ":"), ensure_ascii=True) + "\n").encode("utf-8")
            # OPEN_EXISTING: only append to the log already created by this DLL.
            handle = self.kernel.CreateFileW(str(self.path), 0x40000000, 3, None, 3, 0x80, None)
            if handle == ctypes.c_void_p(-1).value:
                self.failure()
                return
            overlap = Overlapped()
            locked = False
            try:
                locked = bool(self.kernel.LockFileEx(handle, 2, 0, 1, 0, ctypes.byref(overlap)))
                written = wintypes.DWORD()
                append = Overlapped(); append.Offset = 0xffffffff; append.OffsetHigh = 0xffffffff
                if not locked or not self.kernel.WriteFile(handle, text, len(text), ctypes.byref(written), ctypes.byref(append)) or written.value != len(text):
                    self.failure()
            finally:
                if locked:
                    self.kernel.UnlockFileEx(handle, 0, 1, 0, ctypes.byref(overlap))
                self.kernel.CloseHandle(handle)

    def failure(self):
        if not self.warned:
            log_status("Could not append LLM data to the DLL match log; check log directory permissions and disk space.", file=sys.stderr)
            self.warned = True
