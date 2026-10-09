"""INI-configured function-call controller for the opt-in Red Alert AI bridge.

Uses only the Python standard library. Network requests run on a worker while
the main loop maintains the shared-memory heartbeat and reads fresh snapshots.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import ctypes
import json
import mmap
import os
from pathlib import Path
import re
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from llm_config import APIConfig, BridgeConfig, ConfigError, DEFAULT_CONFIG, load_bridge_config, load_config, validate_config
from llm_audit import MatchAudit, log_status, utc_timestamp

PROTOCOL_VERSION = 2
MAPPING_MAGIC = 0x314D4C41
MAPPING_VERSION = 2
PLAN_MAGIC = 0x314E4C50
HEADER_BYTES = 64
SNAPSHOT_BYTES = 262144
COMMAND_BYTES = 512
LOG_PATH_BYTES = 4096
LOG_PATH_OFFSET = HEADER_BYTES + SNAPSHOT_BYTES + COMMAND_BYTES
MAPPING_BYTES = LOG_PATH_OFFSET + LOG_PATH_BYTES
MAX_PLAN_TICKS = 450
SIM_TICKS_PER_SECOND = 15
GROUPS = ("ground", "naval", "air")
ACTIONS = ("hold", "attack_target", "defend_area", "retreat_to", "harass_economy", "raid_power")
TARGET_ACTIONS = {"attack_target", "harass_economy", "raid_power"}
PLAN_KEYS = {"protocol_version", "match_id", "based_on_snapshot_seq", "controlled_house_id", "valid_for_ticks", "orders"}
ORDER_KEYS = {"action", "group_id", "target_id", "destination_cell", "commit_percent", "withdraw_avg_hp_percent"}

SYSTEM_PROMPT = """You are the tactical commander of one computer-controlled army in
Command & Conquer Red Alert. Call submit_tactical_plan exactly once using only
the supplied snapshot. Target IDs name live object instances; use only IDs in
target_candidates, and only groups listed in reachable_by. Cell coordinates are
absolute map cells, not pixels. Only choose cells inside map_bounds. The native
AI checks paths, weapons, defense needs and local safety and may reject a plan.
Power estimates are price weighted by remaining health, not win probabilities.
commit_percent is an approximate share of eligible combat power; the remaining
units stay in reserve. withdraw_avg_hp_percent is the participating group's
average health threshold. Mandatory emergency defenders are never recruited.
Hold controls all eligible units in its group; commit_percent applies to the
other actions.
Use hold to avoid new attacks, defend_area to reinforce a location, retreat_to
to withdraw, attack_target for a specific enemy, harass_economy for a listed
harvester/refinery/power target, and raid_power only for an air group and a power
plant. Target actions require target_id and null destination_cell. Area actions
require null target_id and [x,y]. Hold requires both to be null. Submit at most
one order per group. This plan replaces the previous plan; omitted groups return
to native control. An empty orders array returns all groups to native control.
Prefer stable plans, avoid unnecessary reversals, and consider active_plan and
last_plan_results. Do not assign orders to groups with no available units.
Set valid_for_ticks within the supplied schema's limits (at most 450 ticks, or
30 simulation seconds). It starts when the DLL accepts the plan, independently
of the network wait budget. The game continues during the request, and the DLL
rechecks live targets, object generations, current routes and force safety.
The snapshot is omniscient AI-debug data; it is not a human player's view.
"""


class BridgeError(ValueError):
    pass


def _unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise BridgeError("duplicate JSON key")
        result[key] = value
    return result


def parse_json(text):
    def reject_constant(_):
        raise BridgeError("non-finite JSON number")
    return json.loads(text, object_pairs_hook=_unique_pairs, parse_constant=reject_constant)


def _integer(value, minimum, maximum):
    return type(value) is int and minimum <= value <= maximum


def validate_snapshot(snapshot):
    if (not isinstance(snapshot, dict) or type(snapshot.get("protocol_version")) is not int
            or snapshot["protocol_version"] != PROTOCOL_VERSION):
        raise BridgeError("unsupported snapshot version")
    if not isinstance(snapshot.get("match_id"), str) or not re.fullmatch(r"[0-9a-f]{16}", snapshot["match_id"]):
        raise BridgeError("invalid match identity")
    for key, low in (("snapshot_seq", 1), ("sim_frame", 0), ("controlled_house_id", 0)):
        if not _integer(snapshot.get(key), low, 2147483647):
            raise BridgeError("invalid snapshot envelope")
    if snapshot.get("visibility_mode") != "omniscient":
        raise BridgeError("this bridge requires omniscient AI-debug snapshots")
    if type(snapshot.get("ticks_per_second")) is not int or snapshot["ticks_per_second"] != SIM_TICKS_PER_SECOND:
        raise BridgeError("unsupported simulation tick rate")
    if not isinstance(snapshot.get("groups"), list) or not isinstance(snapshot.get("target_candidates"), list):
        raise BridgeError("missing tactical data")
    bounds = snapshot.get("map_bounds")
    if (not isinstance(bounds, dict) or set(bounds) != {"x", "y", "width", "height"}
            or not all(_integer(bounds[key], 0 if key in {"x", "y"} else 1, 128) for key in bounds)
            or bounds["x"] + bounds["width"] > 128 or bounds["y"] + bounds["height"] > 128):
        raise BridgeError("invalid map bounds")
    if len(snapshot["groups"]) != 3 or len(snapshot["target_candidates"]) > 96:
        raise BridgeError("tactical data exceeds limits")
    seen = set()
    for group in snapshot["groups"]:
        if (not isinstance(group, dict) or not isinstance(group.get("id"), str)
                or group["id"] not in GROUPS or group["id"] in seen
                or not _integer(group.get("count"), 0, 256)
                or not isinstance(group.get("units"), list) or len(group["units"]) != group["count"]):
            raise BridgeError("invalid combat group")
        seen.add(group["id"])
    seen = set()
    for target in snapshot["target_candidates"]:
        if (not isinstance(target, dict) or not isinstance(target.get("id"), str)
                or not re.fullmatch(r"[1-9][0-9]*:[1-9][0-9]*", target["id"])
                or target["id"] in seen or not isinstance(target.get("category"), str)
                or target["category"] not in {"power", "refinery", "harvester", "building", "military"}
                or not isinstance(target.get("reachable_by"), list) or not target["reachable_by"]
                or not all(isinstance(group, str) and group in GROUPS for group in target["reachable_by"])):
            raise BridgeError("invalid target candidate")
        seen.add(target["id"])
    return snapshot


def has_combat_groups(snapshot):
    return any(group["count"] > 0 for group in snapshot["groups"])


def plan_tick_limit(config):
    return config.plan_ttl_seconds * SIM_TICKS_PER_SECOND


def function_tool(snapshot=None, max_plan_ticks=MAX_PLAN_TICKS):
    envelope = {
        "protocol_version": {"type": "integer", "enum": [PROTOCOL_VERSION]},
        "match_id": {"type": "string"},
        "based_on_snapshot_seq": {"type": "integer"},
        "controlled_house_id": {"type": "integer"},
        "valid_for_ticks": {"type": "integer", "minimum": 1, "maximum": max_plan_ticks,
                            "description": "Simulation ticks from DLL acceptance, independent of API latency."},
        "orders": {
            "type": "array", "maxItems": 3,
            "items": {
                "type": "object", "additionalProperties": False,
                "properties": {
                    "action": {"type": "string", "enum": list(ACTIONS)},
                    "group_id": {"type": "string", "enum": list(GROUPS)},
                    "target_id": {"type": ["string", "null"]},
                    "destination_cell": {"type": ["array", "null"], "items": {"type": "integer"}, "minItems": 2, "maxItems": 2},
                    "commit_percent": {"type": "integer", "minimum": 10, "maximum": 100},
                    "withdraw_avg_hp_percent": {"type": "integer", "minimum": 0, "maximum": 90},
                },
                "required": sorted(ORDER_KEYS),
            },
        },
    }
    if snapshot is not None:
        envelope["match_id"]["enum"] = [snapshot["match_id"]]
        envelope["based_on_snapshot_seq"]["enum"] = [snapshot["snapshot_seq"]]
        envelope["controlled_house_id"]["enum"] = [snapshot["controlled_house_id"]]
        groups = [group["id"] for group in snapshot["groups"] if group["count"]]
        if groups:
            envelope["orders"]["items"]["properties"]["group_id"]["enum"] = groups
        else:
            envelope["orders"]["maxItems"] = 0
        envelope["orders"]["items"]["properties"]["target_id"]["enum"] = [
            target["id"] for target in snapshot["target_candidates"]] + [None]
    return {
        "type": "function", "name": "submit_tactical_plan", "strict": True,
        "description": "Propose a bounded tactical plan; the game validates and executes it.",
        "parameters": {"type": "object", "properties": envelope, "required": sorted(PLAN_KEYS), "additionalProperties": False},
    }


def validate_plan(plan, snapshot, max_plan_ticks=MAX_PLAN_TICKS):
    validate_snapshot(snapshot)
    if not isinstance(plan, dict) or set(plan) != PLAN_KEYS:
        raise BridgeError("invalid plan fields")
    if type(plan["protocol_version"]) is not int or plan["protocol_version"] != PROTOCOL_VERSION:
        raise BridgeError("invalid plan version")
    for key, snapshot_key in (("match_id", "match_id"), ("based_on_snapshot_seq", "snapshot_seq"), ("controlled_house_id", "controlled_house_id")):
        if type(plan[key]) is not type(snapshot[snapshot_key]) or plan[key] != snapshot[snapshot_key]:
            raise BridgeError("plan does not match the requested snapshot")
    if not _integer(plan["valid_for_ticks"], 1, max_plan_ticks):
        raise BridgeError("invalid plan lifetime")
    orders = plan["orders"]
    if not isinstance(orders, list) or len(orders) > 3:
        raise BridgeError("too many orders")
    available = {group["id"]: group for group in snapshot["groups"]}
    targets = {target["id"]: target for target in snapshot["target_candidates"]}
    used = set()
    bounds = snapshot["map_bounds"]
    for order in orders:
        if not isinstance(order, dict) or set(order) != ORDER_KEYS:
            raise BridgeError("invalid order fields")
        group, action = order["group_id"], order["action"]
        if not isinstance(group, str) or group not in GROUPS or group not in available or group in used:
            raise BridgeError("invalid or duplicated group")
        if not available[group]["count"]:
            raise BridgeError("group has no available combat units")
        used.add(group)
        if not isinstance(action, str) or action not in ACTIONS:
            raise BridgeError("unknown tactical action")
        if not _integer(order["commit_percent"], 10, 100) or not _integer(order["withdraw_avg_hp_percent"], 0, 90):
            raise BridgeError("invalid tactical percentages")
        target_id, cell = order["target_id"], order["destination_cell"]
        if action in TARGET_ACTIONS:
            if not isinstance(target_id, str) or target_id not in targets or cell is not None:
                raise BridgeError("invalid target reference")
            target = targets[target_id]
            if group not in target["reachable_by"]:
                raise BridgeError("target is not reachable by this group")
            if action == "raid_power" and (group != "air" or target["category"] != "power"):
                raise BridgeError("invalid power raid")
            if action == "harass_economy" and target["category"] not in {"power", "refinery", "harvester"}:
                raise BridgeError("invalid economic target")
            if not re.fullmatch(r"[1-9][0-9]*:[1-9][0-9]*", target_id):
                raise BridgeError("invalid instance identity")
        elif target_id is not None:
            raise BridgeError("unexpected target reference")
        elif action == "hold":
            if cell is not None:
                raise BridgeError("unexpected hold destination")
        elif (not isinstance(cell, list) or len(cell) != 2 or
              not all(_integer(value, 0, 127) for value in cell) or
              not bounds["x"] <= cell[0] < bounds["x"] + bounds["width"] or
              not bounds["y"] <= cell[1] < bounds["y"] + bounds["height"]):
            raise BridgeError("invalid area destination")
    return plan


def encode_plan(plan, snapshot):
    validate_plan(plan, snapshot)
    match = int(plan["match_id"], 16)
    packet = struct.pack("<IIIIiiii", PLAN_MAGIC, PROTOCOL_VERSION, match & 0xFFFFFFFF,
                         match >> 32, plan["based_on_snapshot_seq"], plan["controlled_house_id"],
                         plan["valid_for_ticks"], len(plan["orders"]))
    for order in plan["orders"]:
        target, generation = (int(value) for value in order["target_id"].split(":")) if order["target_id"] else (0, 0)
        if target > 0xFFFFFFFF or generation > 0xFFFFFFFF:
            raise BridgeError("instance identity exceeds wire bounds")
        x, y = order["destination_cell"] or (-1, -1)
        packet += struct.pack("<iiIIiiii", GROUPS.index(order["group_id"]), ACTIONS.index(order["action"]),
                              target, generation, x, y, order["commit_percent"], order["withdraw_avg_hp_percent"])
    return packet


def extract_plan(response, snapshot):
    if not isinstance(response, dict) or response.get("status") != "completed":
        raise BridgeError("API response is incomplete or failed")
    output = response.get("output", [])
    if not isinstance(output, list):
        raise BridgeError("invalid API output")
    calls = [item for item in output if isinstance(item, dict) and item.get("type") == "function_call"]
    if len(calls) != 1 or calls[0].get("name") != "submit_tactical_plan" or calls[0].get("status") not in {None, "completed"}:
        raise BridgeError("expected one completed submit_tactical_plan function call")
    arguments = calls[0].get("arguments")
    if not isinstance(arguments, str) or len(arguments) > 16384:
        raise BridgeError("invalid function arguments")
    return validate_plan(parse_json(arguments), snapshot)


def unwrap_chat_response(response):
    if isinstance(response, dict) and "success" in response:
        if response["success"] is not True or response.get("error") is not None:
            raise BridgeError("Chat Completions response contains an API error")
        response = response.get("data")
    if not isinstance(response, dict) or "error" in response:
        raise BridgeError("Chat Completions response contains an API error")
    return response


def extract_chat_plan(response, snapshot):
    response = unwrap_chat_response(response)
    choices = response.get("choices")
    if not isinstance(choices, list) or len(choices) != 1 or not isinstance(choices[0], dict):
        raise BridgeError("expected exactly one Chat Completions choice")
    choice = choices[0]
    if choice.get("finish_reason") not in ("tool_calls", "stop"):
        raise BridgeError("model did not complete a tool call (plain text, refusal or token limit)")
    message = choice.get("message")
    if not isinstance(message, dict) or message.get("refusal"):
        raise BridgeError("model refused the function call")
    calls = message.get("tool_calls")
    if not isinstance(calls, list) or len(calls) != 1 or not isinstance(calls[0], dict):
        raise BridgeError("expected exactly one submit_tactical_plan tool call")
    call = calls[0]
    function = call.get("function")
    if (call.get("type") != "function" or not isinstance(call.get("id"), str) or not call["id"]
            or not isinstance(function, dict) or function.get("name") != "submit_tactical_plan"):
        raise BridgeError("invalid submit_tactical_plan tool envelope")
    arguments = function.get("arguments")
    if not isinstance(arguments, str) or len(arguments) > 16384:
        raise BridgeError("invalid function arguments")
    return validate_plan(parse_json(arguments), snapshot)


def build_request(snapshot, config, instructions=SYSTEM_PROMPT):
    validate_config(config)
    validate_snapshot(snapshot)
    tool = function_tool(snapshot, plan_tick_limit(config))
    content = json.dumps(snapshot, separators=(",", ":"), ensure_ascii=True)
    if config.protocol == "chat_completions":
        function = {key: value for key, value in tool.items() if key != "type"}
        if not config.strict:
            function.pop("strict")
        request_body = {
            "model": config.model, "stream": False, "max_tokens": config.max_output_tokens,
            "messages": [{"role": "system", "content": instructions}, {"role": "user", "content": content}],
            "tools": [{"type": "function", "function": function}],
            "tool_choice": ({"type": "function", "function": {"name": "submit_tactical_plan"}}
                            if config.tool_choice == "forced" else config.tool_choice),
            "parallel_tool_calls": False,
        }
    else:
        tool["strict"] = config.strict
        request_body = {
            "model": config.model, "store": False, "max_output_tokens": config.max_output_tokens,
            "instructions": instructions, "input": content, "tools": [tool],
            "tool_choice": ({"type": "function", "name": "submit_tactical_plan"}
                            if config.tool_choice == "forced" else config.tool_choice),
            "parallel_tool_calls": False,
        }
    if config.reasoning_effort:
        request_body["reasoning"] = {"effort": config.reasoning_effort}
    headers = dict(config.headers, Authorization=config.authorization, **{"Content-Type": "application/json"})
    return urllib.request.Request(config.api_url, data=json.dumps(request_body).encode("utf-8"), headers=headers)


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, new_url):
        raise BridgeError("API redirect refused; configure the final HTTPS endpoint in llm.ini")


def open_api(request, timeout):
    # urllib's default redirect handler can forward Authorization to another
    # host. A configured endpoint must answer directly.
    return urllib.request.build_opener(_NoRedirect()).open(request, timeout=timeout)


def read_api_response(response, deadline):
    payload = bytearray()
    read = getattr(response, "read1", response.read)
    while len(payload) <= 1048576:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise BridgeError("API response exceeded network wait budget")
        # urllib's HTTPResponse wraps a socket in a buffered reader. Bound each
        # read by the remaining total budget, including a slowly streamed body.
        socket = getattr(getattr(getattr(response, "fp", None), "raw", None), "_sock", None)
        if socket is not None:
            socket.settimeout(remaining)
        chunk = read(min(65536, 1048577 - len(payload)))
        if time.monotonic() > deadline:
            raise BridgeError("API response exceeded network wait budget")
        if not chunk:
            break
        payload.extend(chunk)
    return bytes(payload)


def request_plan(snapshot, model=None, api_key=None, base_url=None, timeout=None,
                 *, config=None, instructions=SYSTEM_PROMPT, metadata=None, audit=None):
    # Preserve the original Responses helper's positional interface.
    if config is None:
        config = APIConfig(protocol="responses", api_url=(base_url or "https://api.openai.com/v1").rstrip("/") + "/responses",
                           model=model or "", authorization="Bearer " + (api_key or ""), strict=True,
                           reasoning_effort="", max_output_tokens=1536, timeout=timeout or 20)
    validate_snapshot(snapshot)
    if not has_combat_groups(snapshot):
        raise BridgeError("no available combat groups; API request paused")
    request = build_request(snapshot, config, instructions)
    if audit:
        audit.record("llm_request", {"protocol": config.protocol, "endpoint": config.api_url,
            "model": config.model, "network_wait_seconds": config.timeout,
            "plan_ttl_seconds": config.plan_ttl_seconds, "request_body": parse_json(request.data)})
    started_at = time.monotonic()
    try:
        with open_api(request, config.timeout) as response:
            payload = read_api_response(response, started_at + config.timeout)
        if audit:
            try:
                body = parse_json(payload)
            except (ValueError, UnicodeError):
                body = {"body_text": payload.decode("utf-8", errors="replace")}
            audit.record("llm_response", {"response_body": body, "exceeds_limit": len(payload) > 1048576})
        if len(payload) > 1048576:
            raise BridgeError("API response exceeds limit")
        if time.monotonic() - started_at > config.timeout:
            raise BridgeError("API response exceeded network wait budget")
        response = parse_json(payload)
        if config.protocol == "chat_completions":
            response = unwrap_chat_response(response)
        plan = extract_chat_plan(response, snapshot) if config.protocol == "chat_completions" else extract_plan(response, snapshot)
        validate_plan(plan, snapshot, plan_tick_limit(config))
        if audit:
            audit.record("llm_plan_validated", {"plan": plan})
        if metadata is not None:
            metadata["function_name"] = "submit_tactical_plan"
            metadata["tool_call_count"] = 1
            usage = response.get("usage", {})
            if isinstance(usage, dict):
                metadata["usage"] = {name: value for name, value in usage.items()
                                     if name in {"prompt_tokens", "completion_tokens", "total_tokens", "input_tokens", "output_tokens"}
                                     and _integer(value, 0, 2147483647)}
        return plan
    except urllib.error.HTTPError as error:
        if audit:
            payload = error.read(1048576)
            try:
                body = parse_json(payload)
            except (ValueError, UnicodeError):
                body = {"body_text": payload.decode("utf-8", errors="replace")}
            audit.record("llm_http_error", {"http_status": error.code, "response_body": body})
        # The console only gets a status; full bodies go through audit redaction.
        raise BridgeError("API HTTP status " + str(error.code)) from None
    except (urllib.error.URLError, TimeoutError, OSError):
        if audit:
            audit.record("llm_network_error", {"error": "API connection failed or timed out"})
        raise BridgeError("API connection failed or timed out") from None
    except (ValueError, TypeError, KeyError, UnicodeError) as error:
        if audit:
            audit.record("llm_validation_failed", {"error": str(error) if isinstance(error, BridgeError) else "invalid response data"})
        raise


def mock_plan(snapshot, action="hold", max_plan_ticks=MAX_PLAN_TICKS):
    orders = []
    for group in snapshot["groups"]:
        if not group["count"]:
            continue
        target = next((entry for entry in snapshot["target_candidates"] if group["id"] in entry["reachable_by"]), None)
        attack = action == "attack_target" and target is not None
        orders.append({"action": "attack_target" if attack else "hold", "group_id": group["id"],
                       "target_id": target["id"] if attack else None, "destination_cell": None,
                       "commit_percent": 75, "withdraw_avg_hp_percent": 35})
    return {"protocol_version": PROTOCOL_VERSION, "match_id": snapshot["match_id"],
            "based_on_snapshot_seq": snapshot["snapshot_seq"], "controlled_house_id": snapshot["controlled_house_id"],
            "valid_for_ticks": max_plan_ticks, "orders": orders}


def submission_rejection(plan, requested, latest, *, elapsed, snapshot_age, config):
    validate_plan(plan, requested, plan_tick_limit(config))
    if elapsed < 0 or elapsed > config.timeout:
        return "network_wait_budget_exceeded"
    if (latest is None or latest["match_id"] != requested["match_id"]
            or latest["controlled_house_id"] != requested["controlled_house_id"]):
        return "match_or_house_changed"
    if (snapshot_age >= 5 or snapshot_age < 0 or latest["sim_frame"] < requested["sim_frame"]
            or latest["snapshot_seq"] < requested["snapshot_seq"]):
        return "game_snapshot_stale"
    if not has_combat_groups(latest):
        return "no_available_combat_groups"
    return None


class SharedMemory:
    def __init__(self, channel="v1", house=-1):
        if sys.platform != "win32":
            raise BridgeError("live shared-memory control requires Windows")
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,63}", channel):
            raise BridgeError("invalid channel name")
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel.CreateMutexW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_wchar_p]
        self.kernel.CreateMutexW.restype = ctypes.c_void_p
        self.kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        self.kernel.GetTickCount.restype = ctypes.c_uint32
        self.name = "Local\\AIBoostLLM-" + channel
        ctypes.set_last_error(0)
        self.mutex = self.kernel.CreateMutexW(None, False, self.name + "-bridge")
        if not self.mutex or ctypes.get_last_error() == 183:
            if self.mutex:
                self.kernel.CloseHandle(self.mutex)
            raise BridgeError("another bridge owns this channel")
        try:
            self.memory = mmap.mmap(-1, MAPPING_BYTES, tagname=self.name, access=mmap.ACCESS_WRITE)
            magic, version = struct.unpack_from("<II", self.memory, 0)
            if magic != MAPPING_MAGIC or version != MAPPING_VERSION:
                self.memory[:] = bytes(MAPPING_BYTES)
                struct.pack_into("<II", self.memory, 0, MAPPING_MAGIC, MAPPING_VERSION)
            struct.pack_into("<i", self.memory, 24, house)
            self.last_snapshot_version = 0
            self.heartbeat()
        except Exception:
            if hasattr(self, "memory"):
                self.memory.close()
            self.kernel.CloseHandle(self.mutex)
            raise

    def heartbeat(self):
        struct.pack_into("<I", self.memory, 28, self.kernel.GetTickCount())

    def read_snapshot(self):
        sequence, length = struct.unpack_from("<II", self.memory, 8)
        if not sequence or sequence & 1 or sequence == self.last_snapshot_version:
            return None
        if not 0 < length <= SNAPSHOT_BYTES:
            return None
        payload = self.memory[HEADER_BYTES:HEADER_BYTES + length]
        if struct.unpack_from("<I", self.memory, 8)[0] != sequence:
            return None
        self.last_snapshot_version = sequence
        return validate_snapshot(parse_json(payload))

    def write_plan(self, plan, snapshot):
        packet = encode_plan(plan, snapshot)
        sequence = struct.unpack_from("<I", self.memory, 16)[0]
        odd = ((sequence + 1) | 1) & 0xFFFFFFFF
        struct.pack_into("<I", self.memory, 16, odd)
        offset = HEADER_BYTES + SNAPSHOT_BYTES
        self.memory[offset:offset + len(packet)] = packet
        struct.pack_into("<I", self.memory, 20, len(packet))
        struct.pack_into("<I", self.memory, 16, (odd + 1) & 0xFFFFFFFF)

    def audit_for(self, snapshot, config):
        sequence, length, low, high = struct.unpack_from("<IIII", self.memory, 36)
        if not sequence or sequence & 1 or not 0 < length < LOG_PATH_BYTES:
            return None
        if snapshot["match_id"] != format(high, "08x") + format(low, "08x"):
            return None
        raw = self.memory[LOG_PATH_OFFSET:LOG_PATH_OFFSET + length]
        if struct.unpack_from("<I", self.memory, 36)[0] != sequence:
            return None
        try:
            path = Path(raw.decode("utf-8"))
            if not path.is_absolute() or path.parent.name != "log" or path.suffix != ".jsonl" or not path.is_file():
                return None
            return MatchAudit(path, snapshot, config)
        except (UnicodeError, OSError, ValueError):
            return None

    def close(self):
        # Expire the heartbeat immediately even while the DLL retains a mapping.
        struct.pack_into("<I", self.memory, 28, (self.kernel.GetTickCount() - 6000) & 0xFFFFFFFF)
        self.memory.close()
        self.kernel.CloseHandle(self.mutex)


class GameLifetime:
    """Stop an automatically started bridge when its DLL host exits/unloads."""
    def __init__(self, parent_pid=None, stop_event=None):
        self.handles = []
        self.kernel = None
        if parent_pid is None and stop_event is None:
            return
        if sys.platform != "win32":
            raise BridgeError("game lifetime monitoring requires Windows")
        if parent_pid is not None and not 0 < parent_pid < 2**32:
            raise BridgeError("invalid parent process ID")
        if stop_event is not None and not re.fullmatch(r"Local\\AIBoostLLM-stop-[0-9]+", stop_event):
            raise BridgeError("invalid game stop event")
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
        self.kernel.OpenProcess.restype = ctypes.c_void_p
        self.kernel.OpenEventW.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_wchar_p]
        self.kernel.OpenEventW.restype = ctypes.c_void_p
        self.kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        self.kernel.WaitForSingleObject.restype = ctypes.c_uint32
        self.kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        try:
            for handle in ([self.kernel.OpenProcess(0x100000, False, parent_pid)] if parent_pid else []):
                if not handle:
                    raise BridgeError("game process is unavailable")
                self.handles.append(handle)
            if stop_event:
                handle = self.kernel.OpenEventW(0x100000, False, stop_event)
                if not handle:
                    raise BridgeError("game stop event is unavailable")
                self.handles.append(handle)
        except Exception:
            self.close()
            raise

    def stopped(self):
        return any(self.kernel.WaitForSingleObject(handle, 0) != 258 for handle in self.handles)

    def close(self):
        for handle in self.handles:
            self.kernel.CloseHandle(handle)
        self.handles.clear()


def write_summary_log(log, kind, data):
    log.write(json.dumps({"timestamp": utc_timestamp(), "kind": kind, "data": data}) + "\n")
    log.flush()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG, help="Local llm.ini path; beside a packaged EXE, or at the repository root.")
    parser.add_argument("--model", help="Override the INI model ID.")
    parser.add_argument("--protocol", choices=("chat_completions", "responses"), help="Override the INI API protocol.")
    parser.add_argument("--api-url", help="Override the complete HTTPS endpoint URL.")
    parser.add_argument("--base-url", help="Legacy API base URL; append the selected protocol's endpoint.")
    parser.add_argument("--house", type=int, help="Override INI house; -1 selects the first eligible AI.")
    parser.add_argument("--channel", help="Override INI shared-memory channel.")
    parser.add_argument("--parent-pid", type=int, help="Automatically exit when this game process ends.")
    parser.add_argument("--stop-event", help="DLL-owned Windows event for graceful bridge shutdown.")
    parser.add_argument("--runtime-log", type=Path, help="Safe startup/status log for automatic background launch.")
    parser.add_argument("--interval", type=float, help="Override minimum wall-clock seconds between requests.")
    parser.add_argument("--timeout", type=float, help="Override HTTP timeout in wall-clock seconds.")
    parser.add_argument("--plan-ttl-seconds", type=int, help="Override maximum plan lifetime after DLL acceptance, in simulation seconds (1-30).")
    parser.add_argument("--strict", action=argparse.BooleanOptionalAction, default=None, help="Override server-side strict schema mode.")
    parser.add_argument("--check-config", action="store_true", help="Validate the INI without sending a request; never print authorization.")
    parser.add_argument("--mock", action="store_true", help="Exercise the control loop without API calls.")
    parser.add_argument("--mock-action", choices=("hold", "attack_target"), default="hold")
    parser.add_argument("--log", type=Path, help="Optional timestamped JSONL log of snapshots, plans, and execution feedback.")
    parser.add_argument("--show-schema", action="store_true")
    args = parser.parse_args()
    if args.runtime_log:
        args.runtime_log.parent.mkdir(parents=True, exist_ok=True)
        sys.stdout = sys.stderr = args.runtime_log.open("a", encoding="utf-8", buffering=1)
    if args.show_schema:
        print(json.dumps(function_tool(), indent=2))
        return
    if args.api_url and args.base_url:
        parser.error("[" + utc_timestamp() + "] choose api-url or base-url")
    try:
        runtime = load_bridge_config(args.config) if args.config.exists() else BridgeConfig()
        args.mock = args.mock or runtime.mode == "mock"
        args.channel = runtime.channel if args.channel is None else args.channel
        args.house = runtime.house if args.house is None else args.house
        if not -1 <= args.house <= 255:
            raise ConfigError("house is outside the supported range")
        config = (APIConfig() if args.mock and not args.config.exists()
                  else load_config(args.config, require_auth=False))
        protocol = args.protocol or config.protocol
        endpoint = args.api_url
        if args.base_url:
            endpoint = args.base_url.rstrip("/") + ("/chat/completions" if protocol == "chat_completions" else "/responses")
        from dataclasses import replace
        overrides = {"protocol": protocol, "api_url": endpoint, "model": args.model,
                     "timeout": args.timeout, "plan_ttl_seconds": args.plan_ttl_seconds,
                     "interval": args.interval, "strict": args.strict}
        config = replace(config, **{key: value for key, value in overrides.items() if value is not None})
        validate_config(config, require_auth=not args.mock or args.check_config)
    except ConfigError as error:
        parser.error("[" + utc_timestamp() + "] " + str(error))
    if args.check_config:
        log_status("llm.ini is valid; authorization is configured and has not been displayed.")
        return
    log = None
    if args.log:
        args.log.parent.mkdir(parents=True, exist_ok=True)
        log = args.log.open("a", encoding="utf-8")
    channel = SharedMemory(args.channel, args.house)
    try:
        lifetime = GameLifetime(args.parent_pid, args.stop_event)
    except Exception:
        channel.close()
        if log:
            log.close()
        raise
    executor = ThreadPoolExecutor(max_workers=1)
    pending = None
    audit = None
    latest = requested = None
    next_request = 0.0
    last_requested = None
    last_update = 0.0
    request_started = 0.0
    combat_ready = None
    combat_context = None
    log_status("LLM bridge ready; waiting for an opted-in single-player skirmish.")
    try:
        while not lifetime.stopped():
            channel.heartbeat()
            now = time.monotonic()
            try:
                snapshot = channel.read_snapshot()
            except (BridgeError, json.JSONDecodeError, UnicodeDecodeError):
                snapshot = None
                log_status("Rejected malformed game snapshot.", file=sys.stderr)
            if snapshot is not None:
                latest, last_update = snapshot, now
                context = (latest["match_id"], latest["controlled_house_id"])
                if context != combat_context:
                    combat_context, combat_ready = context, None
                ready = has_combat_groups(latest)
                if ready != combat_ready:
                    combat_ready = ready
                    status = "resumed" if ready else "paused"
                    log_status("LLM requests " + status + ": " + ("combat groups available." if ready else "no available combat groups."))
                    status_audit = channel.audit_for(latest, config)
                    if status_audit:
                        status_audit.record("llm_requests_" + status,
                                            {"reason": "combat_groups_available" if ready else "no_available_combat_groups"})
                if log:
                    write_summary_log(log, "snapshot", latest)
            if pending is not None and pending.done():
                try:
                    plan = pending.result()
                    reason = submission_rejection(plan, requested, latest, elapsed=now - request_started,
                                                  snapshot_age=now - last_update, config=config)
                    if reason is None:
                        channel.write_plan(plan, requested)
                        if audit:
                            audit.record("llm_plan_submitted", {"plan": plan, "current_sim_frame": latest["sim_frame"],
                                "network_elapsed_seconds": now - request_started})
                        log_status("Submitted plan for snapshot " + str(requested["snapshot_seq"]))
                        if log:
                            write_summary_log(log, "plan", plan)
                    else:
                        log_status("Discarded API plan: " + reason + ".")
                        if audit:
                            audit.record("llm_plan_discarded", {"reason": reason, "plan": plan,
                                "network_elapsed_seconds": now - request_started})
                except (BridgeError, ValueError, KeyError, TypeError) as error:
                    reason = ": " + str(error) if isinstance(error, BridgeError) else ": invalid response data"
                    log_status("Plan rejected or API unavailable" + reason + "; existing plans expire normally.",
                               file=sys.stderr)
                    next_request = max(next_request, now + config.interval)
                pending = None
                audit = None
            identity = (latest["match_id"], latest["snapshot_seq"]) if latest else None
            if (pending is None and latest is not None and has_combat_groups(latest)
                    and now >= next_request and now - last_update < 5 and identity != last_requested):
                requested = latest
                last_requested = identity
                next_request = now + config.interval
                request_started = now
                audit = channel.audit_for(requested, config)
                if args.mock:
                    if audit:
                        audit.record("llm_mock_request", {"action": args.mock_action})
                    pending = executor.submit(mock_plan, requested, args.mock_action, plan_tick_limit(config))
                else:
                    pending = executor.submit(request_plan, requested, config=config, audit=audit)
            time.sleep(0.05)
    except KeyboardInterrupt:
        log_status("Bridge stopped; native AI will resume.")
    finally:
        channel.close()
        lifetime.close()
        executor.shutdown(wait=True, cancel_futures=True)
        if log:
            log.close()
        log_status("Bridge stopped; native AI will resume.")


if __name__ == "__main__":
    try:
        main()
    except (BridgeError, ConfigError) as error:
        log_status(str(error), file=sys.stderr)
        sys.exit(1)
    except Exception:
        # A hidden bootstrap must leave a diagnostic without dumping INI data
        # or an unexpected exception's potentially credential-bearing message.
        log_status("Bridge failed unexpectedly; inspect the per-match log and configuration.", file=sys.stderr)
        sys.exit(1)
