"""Choose an AI-Boost JSONL log and compare army size and available credits.

Python 3.10+; pip install matplotlib
GUI:    python SCRIPTS/plot_ai_log.py
Export: python SCRIPTS/plot_ai_log.py --input match.jsonl --export output/match

Only world snapshots are decoded, so the multi-gigabyte event log is never
loaded into memory. Army counts describe deployed mobile units, including
support units but excluding HARV/MCV. Buildings, dead/inactive objects and
objects in limbo (including passengers and unfinished production) are excluded.
"""
from __future__ import annotations

import argparse
from bisect import bisect_left
import csv
from dataclasses import dataclass, field
import importlib.util
import json
import math
import os
from pathlib import Path, PureWindowsPath
import queue
import statistics
import sys
import threading
import time
from typing import Callable


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_LOG_DIRECTORY = Path(
    r"C:\Program Files (x86)\Steam\steamapps\workshop\content\1213210\2221741447\AIBoost\Data\log"
)
# RTTIType in REDALERT/DEFINES.H. House counters omit ships and include objects
# still in the production queue; count the deployed objects instead.
MOBILE_TYPES = {28: "vehicles", 13: "infantry", 1: "aircraft", 30: "vessels"}
NEUTRAL_HOUSES = {10, 11}
METRICS = {
    "army": "Army size (excluding harvesters/MCVs)",
    "mobile": "All mobile units (including harvesters/MCVs)",
    "vehicles": "Vehicles (excluding harvesters/MCVs)",
    "infantry": "Infantry",
    "aircraft": "Aircraft",
    "vessels": "Ships",
    "harvesters": "Harvesters",
    "mcvs": "MCVs",
}


class LogError(ValueError):
    pass


class LoadCancelled(Exception):
    pass


@dataclass(slots=True)
class Counts:
    vehicles: int = 0
    infantry: int = 0
    aircraft: int = 0
    vessels: int = 0
    harvesters: int = 0
    mcvs: int = 0

    @property
    def army(self) -> int:
        return self.vehicles + self.infantry + self.aircraft + self.vessels

    @property
    def mobile(self) -> int:
        return self.army + self.harvesters + self.mcvs


@dataclass(slots=True)
class HousePoint:
    credits: int | None
    counts: Counts | None

    def value(self, metric: str) -> float:
        value = self.credits if metric == "credits" else (
            getattr(self.counts, metric) if self.counts is not None else None
        )
        return float(value) if value is not None else math.nan


@dataclass(slots=True)
class Snapshot:
    frame: int
    timestamp: str
    houses: dict[int, HousePoint]


@dataclass(slots=True)
class HouseInfo:
    house_id: int
    human: bool = False
    populated: bool = False
    enemy: int = -1

    @property
    def selectable(self) -> bool:
        return self.human or (self.populated and self.house_id not in NEUTRAL_HOUSES)

    @property
    def label(self) -> str:
        return f"{'Player' if self.human else 'AI'} · House {self.house_id}"


@dataclass(slots=True)
class Match:
    match_id: str
    source: Path
    ticks_per_second: float = 15.0
    scenario: str = ""
    snapshots: list[Snapshot] = field(default_factory=list)
    houses: dict[int, HouseInfo] = field(default_factory=dict)
    duplicate_frames: int = 0
    missing_objects: int = 0
    missing_credits: int = 0

    @property
    def participants(self) -> list[int]:
        return sorted(hid for hid, info in self.houses.items() if info.selectable)

    @property
    def interval(self) -> float:
        differences = [b.frame - a.frame for a, b in zip(self.snapshots, self.snapshots[1:])]
        return statistics.median(differences) if differences else self.ticks_per_second

    @property
    def gaps(self) -> int:
        limit = max(self.interval * 3, self.ticks_per_second * 3)
        return sum(b.frame - a.frame > limit for a, b in zip(self.snapshots, self.snapshots[1:]))

    def default_pair(self) -> tuple[int, int]:
        ids = self.participants
        if len(ids) < 2:
            raise LogError("This log has fewer than two eligible houses to compare.")
        ai = next((hid for hid in ids if not self.houses[hid].human), ids[0])
        enemy = self.houses[ai].enemy
        opponent = enemy if enemy in ids and enemy != ai else next(
            (hid for hid in ids if hid != ai and self.houses[hid].human),
            next(hid for hid in ids if hid != ai),
        )
        return ai, opponent

    def series(self, house_id: int, metric: str) -> tuple[list[float], list[float]]:
        """Use actual frames; break the curve across missing snapshot intervals."""
        x, y = [], []
        interval = self.interval
        limit = max(interval * 3, self.ticks_per_second * 3)
        previous = None
        for snap in self.snapshots:
            if previous is not None and snap.frame - previous > limit:
                x.append((previous + interval) / self.ticks_per_second / 60)
                y.append(math.nan)
            point = snap.houses.get(house_id)
            x.append(snap.frame / self.ticks_per_second / 60)
            y.append(point.value(metric) if point else math.nan)
            previous = snap.frame
        return x, y


@dataclass(slots=True)
class LogData:
    source: Path
    matches: list[Match]
    lines: int
    invalid_records: int
    invalid_objects: int
    changed_during_read: bool
    elapsed: float

    def notices(self, match: Match) -> str:
        notes = []
        if self.invalid_records:
            notes.append(f"Skipped {self.invalid_records} malformed snapshot/match-start records")
        if self.invalid_objects:
            notes.append(f"Skipped {self.invalid_objects} invalid objects")
        if match.missing_objects:
            notes.append(f"{match.missing_objects} snapshots have no unit data")
        if match.missing_credits:
            notes.append(f"{match.missing_credits} house samples have no credit data")
        if match.duplicate_frames:
            notes.append(f"Merged {match.duplicate_frames} duplicate-frame snapshots")
        if match.gaps:
            notes.append(f"{match.gaps} snapshot gaps are shown as breaks in the curves")
        if match.snapshots[0].frame > 0:
            notes.append("The log starts partway through the match")
        if self.changed_during_read:
            notes.append("The file changed while loading; charts use its initial contents. Reopen the file to refresh")
        return "; ".join(notes)


def integer(value) -> int | None:
    return value if isinstance(value, int) and not isinstance(value, bool) else None


def read_log(path: str | Path, progress: Callable[[float, int], None] | None = None,
             cancelled: threading.Event | None = None) -> LogData:
    """Stream the file up to its initial size; retain compact snapshot totals only."""
    source = Path(path).resolve()
    size = source.stat().st_size
    started = time.monotonic()
    matches: dict[str, Match] = {}
    lines = invalid_records = invalid_objects = consumed = 0
    last_progress = 0.0
    with source.open("rb", buffering=8 * 1024 * 1024) as stream:
        while consumed < size:
            if cancelled is not None and cancelled.is_set():
                raise LoadCancelled()
            line = stream.readline(size - consumed)
            if not line:
                break
            consumed += len(line)
            lines += 1
            if lines % 8192 == 0:
                now = time.monotonic()
                if progress and now - last_progress >= 0.15:
                    progress(consumed / max(size, 1), lines)
                    last_progress = now
            # Most lines are per-object/per-tick decisions. Avoid JSON-decoding
            # these millions of irrelevant records, even with spaced JSON.
            if b'"world_snapshot"' not in line and b'"match_start"' not in line:
                continue
            try:
                row = json.loads(line)
            except (ValueError, UnicodeError):
                invalid_records += 1
                continue
            if not isinstance(row, dict) or row.get("event") not in {"match_start", "world_snapshot"}:
                continue
            data = row.get("data")
            frame = integer(row.get("sim_frame"))
            if not isinstance(data, dict) or frame is None or frame < 0:
                invalid_records += 1
                continue
            match_id = str(row.get("match_id") or "unknown")
            match = matches.setdefault(match_id, Match(match_id, source))
            tps = data.get("ticks_per_second")
            if isinstance(tps, (float, int)) and not isinstance(tps, bool) and math.isfinite(tps) and tps > 0:
                match.ticks_per_second = float(tps)
            match.scenario = str(data.get("scenario") or match.scenario)
            if row["event"] == "match_start":
                continue
            raw_houses = data.get("houses")
            if not isinstance(raw_houses, list):
                invalid_records += 1
                continue
            houses: dict[int, HousePoint] = {}
            raw_objects = data.get("objects")
            has_objects = isinstance(raw_objects, list)
            if not has_objects:
                match.missing_objects += 1
            for house in raw_houses:
                if not isinstance(house, dict):
                    continue
                hid = integer(house.get("house_id"))
                if hid is None or hid < 0:
                    continue
                credits = integer(house.get("credits"))
                if credits is None:
                    match.missing_credits += 1
                info = match.houses.setdefault(hid, HouseInfo(hid))
                info.human |= house.get("human") is True
                info.populated |= any((integer(house.get(key)) or 0) > 0
                                      for key in ("credits", "buildings", "units", "infantry", "aircraft"))
                enemy = integer(house.get("enemy_house"))
                if enemy is not None and enemy >= 0:
                    info.enemy = enemy
                houses[hid] = HousePoint(credits, Counts() if has_objects else None)
            if not houses:
                invalid_records += 1
                continue
            for obj in raw_objects if has_objects else []:
                if not isinstance(obj, dict):
                    invalid_objects += 1
                    continue
                if obj.get("active") is False or obj.get("limbo") is True:
                    continue
                kind = MOBILE_TYPES.get(integer(obj.get("rtti")))
                if kind is None:
                    continue
                hid, health = integer(obj.get("house_id")), integer(obj.get("health"))
                if hid not in houses or health is None:
                    invalid_objects += 1
                    continue
                if health <= 0:
                    continue
                counts = houses[hid].counts
                type_name = str(obj.get("type", "")).upper()
                if kind == "vehicles" and type_name == "HARV":
                    counts.harvesters += 1
                elif kind == "vehicles" and type_name == "MCV":
                    counts.mcvs += 1
                else:
                    setattr(counts, kind, getattr(counts, kind) + 1)
                match.houses[hid].populated = True
            match.snapshots.append(Snapshot(frame, str(row.get("timestamp", "")), houses))
    usable = []
    for match in matches.values():
        if not match.snapshots:
            continue
        by_frame = {snap.frame: snap for snap in match.snapshots}
        match.duplicate_frames = len(match.snapshots) - len(by_frame)
        match.snapshots = [by_frame[frame] for frame in sorted(by_frame)]
        usable.append(match)
    if not usable:
        raise LogError("No valid world_snapshot records found. Select an AI-Boost match .jsonl log "
                       "instead of a bridge log. Enable AIBOOST_LOG when recording the match.")
    if progress:
        progress(1.0, lines)
    return LogData(source, usable, lines, invalid_records, invalid_objects,
                   source.stat().st_size != size, time.monotonic() - started)


def load_plotting(gui: bool = False):
    # The Windows launcher keeps optional dependencies in the ignored build
    # directory rather than changing the user's or Codex's Python installation.
    local = ROOT / "build/log-viewer-deps"
    if importlib.util.find_spec("matplotlib") is None and local.is_dir():
        sys.path.insert(0, str(local))
    os.environ.setdefault("MPLCONFIGDIR", str(ROOT / "build/log-viewer-cache"))
    try:
        import matplotlib
    except ImportError as exc:
        raise RuntimeError("Matplotlib is required. Run python -m pip install matplotlib, "
                           "or use SCRIPTS/Run-AILogViewer.ps1 to install the dependencies.") from exc
    matplotlib.use("TkAgg" if gui else "Agg")
    matplotlib.rcParams.update({
        "font.family": "sans-serif", "font.sans-serif": ["Microsoft YaHei", "SimHei", "DejaVu Sans"],
        "axes.unicode_minus": False, "font.size": 10,
    })
    return matplotlib


def check_pair(match: Match, ai: int, opponent: int):
    if ai == opponent:
        raise LogError("Select two different houses.")
    for hid in (ai, opponent):
        if hid not in match.houses:
            raise LogError(f"House {hid} is not present in this match.")


def draw_comparison(figure, match: Match, ai: int, opponent: int, metric: str = "army"):
    from matplotlib.ticker import FuncFormatter, MaxNLocator

    check_pair(match, ai, opponent)
    figure.clear()
    figure.set_facecolor("white")
    axes = figure.subplots(2, 1, sharex=True)
    first, last = (snap.frame / match.ticks_per_second / 60
                   for snap in (match.snapshots[0], match.snapshots[-1]))
    for axis, key, title, ylabel in zip(
        axes, (metric, "credits"), (METRICS[metric], "Available credits"), ("Unit count", "Credits")
    ):
        for hid, color in ((ai, "#2563eb"), (opponent, "#ea580c")):
            x, y = match.series(hid, key)
            axis.plot(x, y, color=color, linewidth=1.45,
                      drawstyle="default" if key == "credits" else "steps-post",
                      label=match.houses[hid].label)
        axis.set_title(title, loc="left", fontsize=12, fontweight="bold", pad=12)
        axis.set_ylabel(ylabel)
        axis.set_facecolor("#f8fafc")
        axis.grid(True, axis="y", color="#cbd5e1", alpha=0.6, linewidth=0.7)
        axis.grid(True, axis="x", color="#e2e8f0", linewidth=0.5)
        for side in ("top", "right"):
            axis.spines[side].set_visible(False)
        axis.spines["left"].set_color("#94a3b8")
        axis.spines["bottom"].set_color("#94a3b8")
        axis.set_ylim(bottom=0, top=max(axis.get_ylim()[1], 1))
        axis.set_xlim(first, last if last > first else first + 1 / 60)
        axis.legend(loc="lower right", bbox_to_anchor=(1, 1.015), ncol=2,
                    frameon=False, borderaxespad=0, columnspacing=1.6)
        axis.yaxis.set_major_locator(MaxNLocator(nbins=6, integer=True))
    axes[1].yaxis.set_major_formatter(FuncFormatter(lambda value, _: f"{value:,.0f}"))
    axes[1].set_xlabel("Game time (minutes)")
    figure.suptitle("AI vs opponent: army / credits", x=0.075, ha="left", fontsize=17, fontweight="bold", y=0.977)
    figure.text(0.075, 0.927,
                f"{first:.2f}–{last:.2f} minutes   ·   {len(match.snapshots):,} snapshots   ·   "
                f"{match.ticks_per_second:g} ticks/second   ·   Match {match.match_id}", color="#475569", fontsize=9)
    figure.text(0.075, 0.040, "Counts include deployed, living mobile units; buildings and objects in limbo are excluded. "
                "Credits show available funds at each snapshot.", color="#475569", fontsize=8.5)
    figure.text(0.075, 0.016, f"Log: {match.source.name}", color="#64748b", fontsize=8)
    figure.subplots_adjust(left=0.075, right=0.975, top=0.87, bottom=0.115, hspace=0.30)
    return axes


def export_csv(match: Match, houses: tuple[int, ...], path: str | Path):
    """UTF-8 BOM for Windows Excel; missing data stays blank instead of zero."""
    columns = ["match_id", "sim_frame", "game_minutes", "timestamp", "house_id", "human",
               "army", "mobile", "vehicles", "infantry", "aircraft", "vessels", "harvesters", "mcvs", "credits"]
    destination = Path(path)
    if destination.resolve() == match.source:
        raise LogError("The export path must not overwrite the input log.")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        for snap in match.snapshots:
            for hid in houses:
                point = snap.houses.get(hid)
                row = {"match_id": match.match_id, "sim_frame": snap.frame,
                       "game_minutes": f"{snap.frame / match.ticks_per_second / 60:.6f}",
                       "timestamp": snap.timestamp, "house_id": hid, "human": int(match.houses[hid].human)}
                for key in columns[6:]:
                    value = point.value(key) if point else math.nan
                    row[key] = int(value) if math.isfinite(value) else ""
                writer.writerow(row)


class LogViewer:
    def __init__(self, initial_path: str | None = None):
        import tkinter as tk
        from tkinter import ttk
        from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg, NavigationToolbar2Tk
        from matplotlib.figure import Figure

        self.root = tk.Tk()
        self.root.title("AI-Boost Log Comparison")
        self.root.geometry("1280x960")
        self.root.minsize(900, 690)
        self.data: LogData | None = None
        self.events: queue.Queue = queue.Queue()
        self.cancelled = threading.Event()
        self.loading = False
        self.match_names: list[str] = []
        self.house_names: dict[str, int] = {}
        self.status = tk.StringVar(value="Select an AI-Boost .jsonl log.")
        self.hover = tk.StringVar(value="Hover over a curve to inspect values. Use the toolbar to zoom, pan, or restore the full match view.")
        self.file_name = tk.StringVar(value="No log selected")
        top = ttk.Frame(self.root, padding=(12, 10))
        top.pack(fill="x")
        self.open_button = ttk.Button(top, text="Select log file…", command=self.choose_file)
        self.open_button.pack(side="left")
        ttk.Label(top, textvariable=self.file_name).pack(side="left", padx=12)
        self.cancel_button = ttk.Button(top, text="Cancel loading", command=self.cancelled.set, state="disabled")
        self.cancel_button.pack(side="right")
        controls = ttk.Frame(self.root, padding=(12, 2))
        controls.pack(fill="x")
        self.match_box = self.make_box(controls, "Match", 23, self.change_match)
        self.ai_box = self.make_box(controls, "AI / Side A", 17, self.redraw)
        self.opponent_box = self.make_box(controls, "Opponent / Side B", 17, self.redraw)
        self.metric_box = self.make_box(controls, "Metric", 43, self.redraw)
        self.metric_box["values"] = list(METRICS.values())
        self.metric_box.current(0)
        buttons = ttk.Frame(self.root, padding=(12, 7))
        buttons.pack(fill="x")
        self.image_button = ttk.Button(buttons, text="Save image…", command=self.save_image, state="disabled")
        self.image_button.pack(side="left")
        self.csv_button = ttk.Button(buttons, text="Export CSV…", command=self.save_csv, state="disabled")
        self.csv_button.pack(side="left", padx=8)
        self.progress = ttk.Progressbar(buttons, length=210, maximum=100)
        self.progress.pack(side="right")
        ttk.Label(self.root, textvariable=self.status, padding=(12, 0), wraplength=1200).pack(fill="x")
        self.figure = Figure(figsize=(12.8, 8.2), dpi=100)
        self.canvas = FigureCanvasTkAgg(self.figure, master=self.root)
        self.canvas.get_tk_widget().pack(fill="both", expand=True, padx=6, pady=6)
        self.toolbar = NavigationToolbar2Tk(self.canvas, self.root, pack_toolbar=False)
        self.toolbar.pack(fill="x", padx=6)
        ttk.Label(self.root, textvariable=self.hover, padding=(12, 6), wraplength=1200).pack(fill="x")
        self.canvas.mpl_connect("motion_notify_event", self.show_hover)
        self.root.protocol("WM_DELETE_WINDOW", self.close)
        self.root.after(100, self.poll)
        if initial_path:
            self.root.after(150, lambda: self.open_file(initial_path))
        else:
            self.root.after(150, self.choose_file)

    def make_box(self, parent, label, width, callback):
        from tkinter import ttk
        ttk.Label(parent, text=label).pack(side="left", padx=(0, 5))
        box = ttk.Combobox(parent, width=width, state="disabled")
        box.pack(side="left", padx=(0, 12))
        box.bind("<<ComboboxSelected>>", lambda _: callback())
        return box

    @property
    def match(self) -> Match:
        return self.data.matches[self.match_box.current()]

    def selection(self) -> tuple[int, int, str]:
        ai, opponent = self.house_names[self.ai_box.get()], self.house_names[self.opponent_box.get()]
        metric = next(key for key, label in METRICS.items() if label == self.metric_box.get())
        check_pair(self.match, ai, opponent)
        return ai, opponent, metric

    def choose_file(self):
        from tkinter import filedialog
        initial = self.data.source.parent if self.data else DEFAULT_LOG_DIRECTORY
        name = filedialog.askopenfilename(parent=self.root, title="Select an AI-Boost match log",
                                         initialdir=str(initial if initial.is_dir() else ROOT),
                                         filetypes=[("AI logs", "*.jsonl *.log"), ("All files", "*.*")])
        if name:
            self.open_file(name)

    def set_loading(self, loading: bool):
        self.loading = loading
        self.open_button["state"] = "disabled" if loading else "normal"
        self.cancel_button["state"] = "normal" if loading else "disabled"
        ready = self.data is not None and not loading
        for box in (self.match_box, self.ai_box, self.opponent_box, self.metric_box):
            box["state"] = "readonly" if ready else "disabled"
        for button in (self.image_button, self.csv_button):
            button["state"] = "normal" if ready else "disabled"

    def open_file(self, path: str):
        if self.loading:
            return
        self.cancelled.clear()
        self.set_loading(True)
        self.progress["value"] = 0
        self.status.set(f"Loading {Path(path).name}…")

        def worker():
            try:
                data = read_log(path, lambda fraction, lines: self.events.put(("progress", (fraction, lines))),
                                self.cancelled)
                self.events.put(("loaded", data))
            except LoadCancelled:
                self.events.put(("cancelled", None))
            except Exception as exc:
                self.events.put(("error", str(exc)))

        threading.Thread(target=worker, daemon=True).start()

    def poll(self):
        from tkinter import messagebox
        try:
            while True:
                event, value = self.events.get_nowait()
                if event == "progress":
                    fraction, lines = value
                    self.progress["value"] = fraction * 100
                    self.status.set(f"Loading: {fraction:.0%} · Scanned {lines:,} lines")
                elif event == "loaded":
                    self.data = value
                    self.file_name.set(str(value.source))
                    self.match_names = [f"{i + 1}. {m.match_id}" for i, m in enumerate(value.matches)]
                    self.match_box["values"] = self.match_names
                    self.match_box.current(0)
                    self.set_loading(False)
                    self.change_match()
                elif event == "cancelled":
                    self.set_loading(False)
                    self.status.set("Loading cancelled. Select a file to try again.")
                elif event == "error":
                    self.set_loading(False)
                    self.status.set(value)
                    messagebox.showerror("Load failed", value, parent=self.root)
        except queue.Empty:
            pass
        self.root.after(100, self.poll)

    def change_match(self):
        from tkinter import messagebox
        self.house_names = {self.match.houses[hid].label: hid for hid in self.match.participants}
        for box in (self.ai_box, self.opponent_box):
            box.set("")
            box["values"] = list(self.house_names)
        try:
            ai, opponent = self.match.default_pair()
            self.ai_box.set(self.match.houses[ai].label)
            self.opponent_box.set(self.match.houses[opponent].label)
            self.redraw()
        except LogError as exc:
            self.figure.clear()
            self.canvas.draw_idle()
            self.image_button["state"] = self.csv_button["state"] = "disabled"
            self.status.set(str(exc))
            messagebox.showerror("Cannot compare houses", str(exc), parent=self.root)

    def redraw(self):
        if self.data is None or not self.ai_box.get() or not self.opponent_box.get():
            return
        try:
            ai, opponent, metric = self.selection()
        except LogError as exc:
            self.status.set(str(exc))
            self.image_button["state"] = self.csv_button["state"] = "disabled"
            return
        draw_comparison(self.figure, self.match, ai, opponent, metric)
        self.toolbar.update()
        self.canvas.draw_idle()
        self.hover.set("Hover over a curve to inspect values. Use the toolbar to zoom, pan, or restore the full match view.")
        self.image_button["state"] = self.csv_button["state"] = "normal"
        notice = self.data.notices(self.match)
        self.status.set(f"Loaded {self.data.lines:,} lines and {len(self.match.snapshots):,} snapshots "
                        f"in {self.data.elapsed:.1f} seconds." + (" " + notice if notice else ""))

    def show_hover(self, event):
        if self.data is None or event.inaxes is None or event.xdata is None or self.loading:
            return
        try:
            ai, opponent, metric = self.selection()
        except (LogError, KeyError):
            return
        samples = self.match.snapshots
        frames = [snap.frame for snap in samples]
        frame = event.xdata * self.match.ticks_per_second * 60
        index = min(bisect_left(frames, frame), len(samples) - 1)
        if index and abs(frames[index - 1] - frame) < abs(frames[index] - frame):
            index -= 1
        snap = samples[index]
        seconds = snap.frame / self.match.ticks_per_second
        parts = [f"Sample {int(seconds // 60):02d}:{int(seconds % 60):02d} · Frame {snap.frame}"]
        for hid in (ai, opponent):
            point = snap.houses.get(hid)
            amount = point.value(metric) if point else math.nan
            credits = point.value("credits") if point else math.nan
            text = lambda number: f"{number:,.0f}" if math.isfinite(number) else "No data"
            parts.append(f"{self.match.houses[hid].label}: Units {text(amount)} / Credits {text(credits)}")
        if abs(snap.frame - frame) > max(self.match.interval, self.match.ticks_per_second * 2):
            parts.append("No snapshot at the cursor; showing the nearest recorded sample")
        self.hover.set("    |    ".join(parts))

    def output_path(self, extension: str) -> str:
        from tkinter import filedialog
        directory = ROOT / "build/log-plots"
        directory.mkdir(parents=True, exist_ok=True)
        ai, opponent, metric = self.selection()
        return filedialog.asksaveasfilename(
            parent=self.root, initialdir=str(directory),
            initialfile=f"{self.data.source.stem}_{self.match.match_id}_h{ai}_vs_h{opponent}_{metric}{extension}",
            defaultextension=extension,
            filetypes=[("CSV data", "*.csv")] if extension == ".csv" else
            [("PNG image", "*.png"), ("SVG vector image", "*.svg"), ("PDF", "*.pdf")],
        )

    def save_image(self):
        from tkinter import messagebox
        try:
            destination = self.output_path(".png")
            if destination:
                if Path(destination).resolve() == self.data.source:
                    raise LogError("The export path must not overwrite the input log.")
                self.figure.savefig(destination, dpi=160, facecolor="white")
                self.status.set(f"Image saved: {destination}")
        except Exception as exc:
            messagebox.showerror("Save failed", str(exc), parent=self.root)

    def save_csv(self):
        from tkinter import messagebox
        try:
            destination = self.output_path(".csv")
            if destination:
                ai, opponent, _ = self.selection()
                export_csv(self.match, (ai, opponent), destination)
                self.status.set(f"CSV saved: {destination}")
        except Exception as exc:
            messagebox.showerror("Export failed", str(exc), parent=self.root)

    def close(self):
        self.cancelled.set()
        self.root.destroy()

    def run(self):
        self.root.mainloop()


def main(argv: list[str] | None = None) -> int:
    # Keep UTF-8 output readable for Unicode paths and PowerShell redirection.
    for stream in (sys.stdout, sys.stderr):
        if stream is not None and hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description="Select an AI-Boost log and plot army size and credits for both sides.")
    parser.add_argument("--input", help="Log file; opens a file picker when omitted")
    parser.add_argument("--export", metavar="PREFIX", help="Export PREFIX.png and PREFIX.csv without opening the GUI")
    parser.add_argument("--match", dest="match_id", help="Select a match_id from a file containing multiple matches")
    parser.add_argument("--ai", type=int, help="House ID for side A; defaults to the AI")
    parser.add_argument("--opponent", type=int, help="House ID for side B; defaults to the player or recorded enemy")
    parser.add_argument("--metric", choices=METRICS, default="army", help="Unit-count metric; defaults to army")
    args = parser.parse_args(argv)
    if args.export and not args.input:
        parser.error("--export requires --input")
    if not args.export and (args.match_id is not None or args.ai is not None or args.opponent is not None
                            or args.metric != "army"):
        parser.error("--match / --ai / --opponent / --metric require --export; use the dropdowns in the GUI")
    try:
        load_plotting(gui=not bool(args.export))
        if not args.export:
            LogViewer(args.input).run()
            return 0
        from matplotlib.backends.backend_agg import FigureCanvasAgg
        from matplotlib.figure import Figure
        data = read_log(args.input)
        if args.match_id:
            match = next((m for m in data.matches if m.match_id == args.match_id), None)
            if match is None:
                raise LogError(f"Match {args.match_id} not found. Available matches: " + ", ".join(m.match_id for m in data.matches))
        elif len(data.matches) != 1:
            raise LogError("The file contains multiple matches. Use --match to select one: " +
                           ", ".join(m.match_id for m in data.matches))
        else:
            match = data.matches[0]
        ai, opponent = match.default_pair()
        ai = args.ai if args.ai is not None else ai
        opponent = args.opponent if args.opponent is not None else opponent
        check_pair(match, ai, opponent)
        prefix = Path(args.export).resolve()
        prefix.parent.mkdir(parents=True, exist_ok=True)
        image_path = prefix.parent / (prefix.name + ".png")
        csv_path = prefix.parent / (prefix.name + ".csv")
        if data.source in (image_path, csv_path):
            raise LogError("The export path must not overwrite the input log.")
        figure = Figure(figsize=(12.8, 8.8), dpi=100)
        FigureCanvasAgg(figure)
        draw_comparison(figure, match, ai, opponent, args.metric)
        figure.savefig(image_path, dpi=160, facecolor="white")
        export_csv(match, (ai, opponent), csv_path)
        scenario = PureWindowsPath(match.scenario).name
        print(f"Match {match.match_id} ({scenario}); {data.lines:,} lines, {len(match.snapshots):,} snapshots, "
              f"loaded in {data.elapsed:.2f} seconds; {match.houses[ai].label} vs {match.houses[opponent].label}")
        notice = data.notices(match)
        if notice:
            print(notice)
        print(f"Image: {image_path}\nData: {csv_path}")
        return 0
    except (OSError, LogError, RuntimeError, ImportError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        if not args.export:
            try:
                import tkinter as tk
                from tkinter import messagebox
                root = tk.Tk()
                root.withdraw()
                messagebox.showerror("AI-Boost Log Viewer", str(exc), parent=root)
                root.destroy()
            except Exception:
                pass
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
