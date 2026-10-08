"""Offline function-call, wire transport, and real tactical-controller checks.

No API key or game assets are required. On Windows this compiles and exercises
the DLL's actual shared-memory transport with MSVC. Run test_redalert_ai.py first
to include the compiled tactical-controller integration fixture.
"""
from pathlib import Path
import copy
from dataclasses import replace
import io
import json
import mmap
import os
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import uuid

import llm_bridge as bridge
from llm_audit import MatchAudit
from llm_config import APIConfig, ConfigError, load_bridge_config, load_config
import test_llm_live as live
from test_redalert_ai import msvc_environment

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "build/ai-tests"


def snapshot():
    return {
        "protocol_version": 1, "match_id": "12345678abcdef01", "snapshot_seq": 7,
        "sim_frame": 901, "controlled_house_id": 4, "visibility_mode": "omniscient",
        "ticks_per_second": 15, "map_bounds": {"x": 1, "y": 1, "width": 126, "height": 126},
        "self": {"credits": 5000, "power": 300, "drain": 200, "base_cell": [20, 40]},
        "groups": [
            {"id": "ground", "count": 8, "units": [{"id": str(i + 1) + ":1"} for i in range(8)]},
            {"id": "naval", "count": 0, "units": []},
            {"id": "air", "count": 1, "units": [{"id": "9:1"}]},
        ],
        "target_candidates": [
            {"id": "16777224:2", "category": "power", "reachable_by": ["ground", "air"]},
            {"id": "16777225:1", "category": "military", "reachable_by": ["ground"]},
        ],
        "last_plan_results": [],
    }


def api_response(plan):
    return {"status": "completed", "output": [
        {"type": "reasoning", "summary": []},
        {"type": "function_call", "name": "submit_tactical_plan", "status": "completed",
         "call_id": "test_call", "arguments": json.dumps(plan)},
    ]}


def chat_response(plan):
    return {"choices": [{"index": 0, "finish_reason": "tool_calls", "message": {
        "role": "assistant", "content": None, "reasoning": "ignored by the tactical protocol",
        "tool_calls": [{"id": "test_call", "type": "function", "function": {
            "name": "submit_tactical_plan", "arguments": json.dumps(plan)}}],
    }}], "usage": {"prompt_tokens": 100, "completion_tokens": 50}}


class IniConfiguration(unittest.TestCase):
    def setUp(self):
        OUTPUT.mkdir(parents=True, exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(dir=OUTPUT)
        self.path = Path(self.directory.name) / "llm.ini"

    def tearDown(self):
        self.assertEqual(Path(self.directory.name).resolve().parent, OUTPUT.resolve())
        self.directory.cleanup()

    def write(self, content):
        self.path.write_text(content, encoding="utf-8")

    def test_user_defaults_full_authorization_and_literal_token(self):
        example = (ROOT / "llm.example.ini").read_text(encoding="utf-8")
        self.write(example.replace("authorization =\n", "authorization = Bearer test-only-%#;literal\n"))
        config = load_config(self.path)
        self.assertEqual(config.api_url, "https://api.cline.bot/api/v1/chat/completions")
        self.assertEqual(config.model, "cline-free/deepseek-v4.1-flash")
        self.assertEqual(config.authorization, "Bearer test-only-%#;literal")
        self.assertEqual(config.headers["User-Agent"], "Cline/3.72.0")
        self.assertEqual(config.reasoning_effort, "xhigh")
        self.assertNotIn("test-only", repr(config))

    def test_missing_placeholder_and_incomplete_credentials_do_not_send_requests(self):
        for auth in ("", "Bearer", "Bearer xxxxx", "Bearer YOUR_API_KEY"):
            self.write("[llm]\nauthorization = " + auth + "\n")
            with self.subTest(auth=auth), self.assertRaises(ConfigError):
                load_config(self.path)
            with patch.object(bridge, "open_api") as transport:
                with self.assertRaises(ConfigError):
                    live.wait_for_config(self.path, 0)
                transport.assert_not_called()

    def test_waits_for_saved_configuration_before_returning(self):
        self.write("[llm]\nauthorization =\n")
        def finish_configuration(_):
            self.write("[llm]\nauthorization = Bearer test-only-saved\n")
        with patch.object(live.time, "sleep", side_effect=finish_configuration) as sleep, \
                patch.object(bridge, "open_api") as transport, patch("sys.stdout", new=io.StringIO()) as output:
            config = live.wait_for_config(self.path, 10)
            self.assertEqual(config.authorization, "Bearer test-only-saved")
            sleep.assert_called_once_with(1)
            transport.assert_not_called()
            self.assertNotIn("test-only-saved", output.getvalue())

    def test_bad_ini_errors_do_not_echo_secret_source_lines(self):
        contents = [
            "[llm]\nauthorization = Bearer secret-test-only\nauthorization = second-secret\n",
            "[llm]\nauthorization = Bearer secret-test-only\napi_url = https://user:secret-test-only@example.invalid/v1\n",
            "[llm]\nauthorization = Bearer secret-test-only\napi_url = https://[broken/v1\n",
            "[llm]\nauthorization = Bearer secret-test-only\n interval = broken\n",
            "[llm]\nauthorization = Bearer secret-test-only\n[headers]\nAuthorization = leaked-token\n",
            "[llm]\nauthorization = Bearer secret-test-only\n[headers]\nX-Test = first\nx-test = second\n",
        ]
        for content in contents:
            self.write(content)
            with self.subTest(content=content):
                try:
                    load_config(self.path)
                except ConfigError as error:
                    self.assertNotIn("secret-test-only", str(error))
                    self.assertNotIn("leaked-token", str(error))
                else:
                    self.fail("malformed configuration was accepted")

    def test_openai_key_is_not_forwarded_to_cline_and_overrides_work(self):
        self.write("[llm]\nauthorization =\n")
        with patch.dict(os.environ, OPENAI_API_KEY="test-only-env-key"):
            with self.assertRaises(ConfigError):
                load_config(self.path)
            self.write("[llm]\nprotocol = responses\napi_url = https://api.openai.com/v1/responses\n"
                       "model = test-model\nreasoning_effort =\nstrict = true\n")
            config = load_config(self.path, timeout=25, interval=2)
            self.assertEqual(config.authorization, "Bearer test-only-env-key")
            self.assertEqual(config.timeout, 25)
            self.assertEqual(config.interval, 2)

    def test_bridge_options_and_invalid_values_do_not_expose_authorization(self):
        self.write("[llm]\nauthorization = Bearer local-test-secret\n"
                   "[bridge]\nenabled = true\nauto_start = false\nchannel = test_2\nhouse = 4\nmode = mock\n")
        runtime = load_bridge_config(self.path)
        self.assertTrue(runtime.enabled)
        self.assertFalse(runtime.auto_start)
        self.assertEqual((runtime.channel, runtime.house, runtime.mode), ("test_2", 4, "mock"))
        load_config(self.path)
        for bad in ("mode = script", "enabled = secret-invalid", "house = 256", "channel = ../bad", "command = secret-invalid"):
            self.write("[llm]\nauthorization = Bearer local-test-secret\n[bridge]\n" + bad + "\n")
            with self.assertRaises(ConfigError) as failure:
                load_config(self.path)
            self.assertNotIn("secret", str(failure.exception))


class ChatCompletions(unittest.TestCase):
    def setUp(self):
        self.snapshot = snapshot()
        self.plan = bridge.mock_plan(self.snapshot, "attack_target")
        self.config = APIConfig(api_url="https://api.cline.bot/api/v1/chat/completions", model="cline-free/deepseek-v4.1-flash", authorization="Bearer test-only", headers={"User-Agent": "Cline/3.72.0"})

    def test_cline_request_body_headers_and_exact_tool_format(self):
        request = bridge.build_request(self.snapshot, self.config)
        body = json.loads(request.data)
        self.assertEqual(request.full_url, self.config.api_url)
        self.assertEqual(request.get_header("Authorization"), "Bearer test-only")
        self.assertEqual(request.get_header("User-agent"), "Cline/3.72.0")
        self.assertFalse(body["stream"])
        self.assertEqual(body["model"], "cline-free/deepseek-v4.1-flash")
        self.assertEqual(body["reasoning"], {"effort": "xhigh"})
        self.assertEqual(body["tool_choice"], {"type": "function", "function": {"name": "submit_tactical_plan"}})
        self.assertFalse(body["parallel_tool_calls"])
        self.assertNotIn("strict", body["tools"][0]["function"])
        self.assertEqual(json.loads(body["messages"][1]["content"]), self.snapshot)
        strict = json.loads(bridge.build_request(self.snapshot, replace(self.config, strict=True)).data)
        self.assertTrue(strict["tools"][0]["function"]["strict"])
        required = json.loads(bridge.build_request(self.snapshot, replace(self.config, tool_choice="required")).data)
        self.assertEqual(required["tool_choice"], "required")
        auto = json.loads(bridge.build_request(self.snapshot, replace(self.config, tool_choice="auto")).data)
        self.assertEqual(auto["tool_choice"], "auto")
        self.assertEqual(auto["tools"], body["tools"])

    def test_chat_tool_calls_are_parsed_and_locally_validated(self):
        self.assertEqual(bridge.extract_chat_plan(chat_response(self.plan), self.snapshot), self.plan)
        response = chat_response(self.plan)
        response["choices"][0]["finish_reason"] = "stop"
        self.assertEqual(bridge.extract_chat_plan(response, self.snapshot), self.plan)
        invalid = copy.deepcopy(self.plan)
        invalid["orders"][0]["target_id"] = "16777224:1"
        with self.assertRaises(bridge.BridgeError):
            bridge.extract_chat_plan(chat_response(invalid), self.snapshot)

    def test_plain_json_text_refusals_multiple_calls_and_truncation_fail(self):
        invalid = []
        plain = chat_response(self.plan)
        plain["choices"][0]["message"].pop("tool_calls")
        plain["choices"][0]["message"]["content"] = json.dumps(self.plan)
        plain["choices"][0]["finish_reason"] = "stop"
        invalid.append(plain)
        refusal = chat_response(self.plan)
        refusal["choices"][0]["message"]["refusal"] = "refused"
        invalid.append(refusal)
        for reason in ("length", "content_filter", None):
            response = chat_response(self.plan)
            response["choices"][0]["finish_reason"] = reason
            invalid.append(response)
        repeated = chat_response(self.plan)
        repeated["choices"][0]["message"]["tool_calls"] *= 2
        invalid.append(repeated)
        wrong = chat_response(self.plan)
        wrong["choices"][0]["message"]["tool_calls"][0]["function"]["name"] = "move_anything"
        invalid.append(wrong)
        for response in invalid:
            with self.subTest(response=response), self.assertRaises(bridge.BridgeError):
                bridge.extract_chat_plan(response, self.snapshot)

    def test_real_request_adapter_records_only_safe_metadata(self):
        metadata = {}
        with patch.object(bridge, "open_api", return_value=io.BytesIO(json.dumps(chat_response(self.plan)).encode("utf-8"))):
            result = bridge.request_plan(self.snapshot, config=self.config, metadata=metadata)
        self.assertEqual(result, self.plan)
        self.assertEqual(metadata["tool_call_count"], 1)
        self.assertEqual(metadata["usage"], {"prompt_tokens": 100, "completion_tokens": 50})
        self.assertNotIn("test-only", json.dumps(metadata))

    def test_cline_success_data_envelope_preserves_tool_call_and_usage(self):
        response = {"success": True, "data": chat_response(self.plan)}
        self.assertEqual(bridge.extract_chat_plan(response, self.snapshot), self.plan)
        metadata = {}
        with patch.object(bridge, "open_api", return_value=io.BytesIO(json.dumps(response).encode("utf-8"))):
            result = bridge.request_plan(self.snapshot, config=replace(self.config, tool_choice="auto"), metadata=metadata)
        self.assertEqual(result, self.plan)
        self.assertEqual(metadata["usage"]["prompt_tokens"], 100)

    def test_cline_failed_or_malformed_envelopes_and_auto_text_are_rejected(self):
        plain = chat_response(self.plan)
        plain["choices"][0]["message"].pop("tool_calls")
        plain["choices"][0]["message"]["content"] = json.dumps(self.plan)
        plain["choices"][0]["finish_reason"] = "stop"
        responses = [
            {"success": False, "error": "test-only-secret", "data": chat_response(self.plan)},
            {"success": True, "error": "test-only-secret", "data": chat_response(self.plan)},
            {"success": 1, "data": chat_response(self.plan)},
            {"success": True, "data": None},
            {"success": True, "data": plain},
        ]
        for response in responses:
            with self.subTest(response=response):
                with self.assertRaises(bridge.BridgeError) as raised:
                    bridge.extract_chat_plan(response, self.snapshot)
                self.assertNotIn("test-only-secret", str(raised.exception))

    def test_authorization_is_not_forwarded_on_redirect(self):
        request = bridge.build_request(self.snapshot, self.config)
        with self.assertRaisesRegex(bridge.BridgeError, "redirect refused"):
            bridge._NoRedirect().redirect_request(request, None, 302, "redirect", {}, "https://other.invalid")


class LiveTestHarness(unittest.TestCase):
    @unittest.skipUnless((OUTPUT / "tactics_test.exe").is_file(), "compile the tactical fixture first")
    def test_three_live_test_paths_use_function_calls_and_real_cpp_execution(self):
        def fake_request(exported, *, config, instructions, metadata):
            marker = "do not add a rationale or other orders:\n"
            desired = json.loads(instructions.split(marker, 1)[1])
            metadata.update(function_name="submit_tactical_plan", tool_call_count=1)
            return bridge.extract_chat_plan(chat_response(desired), exported)
        for action in live.CASES:
            with self.subTest(action=action):
                result = live.run_case(APIConfig(authorization="Bearer test-only"), action,
                                       OUTPUT / "tactics_test.exe", requester=fake_request)
                self.assertTrue(result["passed"], result)
                self.assertTrue(result["cpp_execution_passed"])
                self.assertNotIn("test-only", json.dumps(result))

    @unittest.skipUnless((OUTPUT / "tactics_test.exe").is_file(), "compile the tactical fixture first")
    def test_live_test_failure_is_not_marked_as_passed(self):
        def failed_request(*args, **kwargs):
            raise bridge.BridgeError("API HTTP status 401")
        result = live.run_case(APIConfig(authorization="Bearer test-only"), "hold",
                               OUTPUT / "tactics_test.exe", requester=failed_request)
        self.assertFalse(result["passed"])
        self.assertEqual(len(result["attempts"]), 1)
        self.assertNotIn("cpp_execution_passed", result)


class FunctionCalls(unittest.TestCase):
    def setUp(self):
        self.snapshot = snapshot()
        self.plan = bridge.mock_plan(self.snapshot, "attack_target")

    def test_strict_schema_and_snapshot_constraints(self):
        tool = bridge.function_tool(self.snapshot)
        self.assertTrue(tool["strict"])
        self.assertEqual(tool["parameters"]["properties"]["match_id"]["enum"], [self.snapshot["match_id"]])
        order = tool["parameters"]["properties"]["orders"]["items"]["properties"]
        self.assertEqual(order["group_id"]["enum"], ["ground", "air"])
        self.assertEqual(order["target_id"]["enum"], ["16777224:2", "16777225:1", None])
        for item in (tool["parameters"], tool["parameters"]["properties"]["orders"]["items"]):
            self.assertFalse(item["additionalProperties"])
            self.assertEqual(set(item["required"]), set(item["properties"]))
        schema_file = ROOT / "docs/llm/submit_tactical_plan.tool.json"
        self.assertEqual(json.loads(schema_file.read_text(encoding="utf-8")), bridge.function_tool())

    def test_valid_call_wire_and_null_fields(self):
        plan = bridge.extract_plan(api_response(self.plan), self.snapshot)
        packet = bridge.encode_plan(plan, self.snapshot)
        self.assertEqual(len(packet), 96)
        self.assertEqual(struct.unpack_from("<IIIIiiii", packet),
                         (bridge.PLAN_MAGIC, 1, 0xABCDEF01, 0x12345678, 7, 4, 1351, 2))
        self.assertEqual(struct.unpack_from("<iiIIiiii", packet, 32),
                         (0, 1, 16777224, 2, -1, -1, 75, 35))
        self.plan["orders"] = []
        self.assertEqual(len(bridge.encode_plan(self.plan, self.snapshot)), 32)

    def test_refusal_plain_text_incomplete_and_multiple_calls(self):
        invalid = [
            {"status": "incomplete", "output": api_response(self.plan)["output"]},
            {"status": "completed", "output": [{"type": "message", "content": [{"type": "refusal"}]}]},
            {"status": "completed", "output": [{"type": "message", "content": [{"type": "output_text", "text": json.dumps(self.plan)}]}]},
        ]
        repeated = api_response(self.plan)
        repeated["output"].append(copy.deepcopy(repeated["output"][-1]))
        invalid.append(repeated)
        wrong = api_response(self.plan)
        wrong["output"][-1]["name"] = "move_anything"
        invalid.append(wrong)
        for response in invalid:
            with self.subTest(response=response), self.assertRaises(bridge.BridgeError):
                bridge.extract_plan(response, self.snapshot)

    def test_duplicate_json_fields_and_nonfinite_numbers(self):
        for text in ('{"orders":[],"orders":[]}', '{"x":NaN}', '{"x":Infinity}'):
            with self.subTest(text=text), self.assertRaises(bridge.BridgeError):
                bridge.parse_json(text)
        response = api_response(self.plan)
        response["output"][-1]["arguments"] = "x" * 16385
        with self.assertRaises(bridge.BridgeError):
            bridge.extract_plan(response, self.snapshot)

    def test_envelope_staleness_and_strict_fields(self):
        changes = {
            "protocol_version": [True, 2], "match_id": ["0000000000000000"],
            "based_on_snapshot_seq": [6, True], "controlled_house_id": [3, True],
            "expires_at_frame": [901, 1352, True], "orders": [None, self.plan["orders"] * 2],
            "unexpected": [1],
        }
        for key, values in changes.items():
            for value in values:
                candidate = copy.deepcopy(self.plan)
                candidate[key] = value
                with self.subTest(key=key, value=value), self.assertRaises(bridge.BridgeError):
                    bridge.validate_plan(candidate, self.snapshot)

    def test_invalid_targets_groups_percentages_and_destinations(self):
        changes = {
            "target_id": ["16777224:1", "16777226:1", None],
            "group_id": ["human", "naval", []], "action": ["sell", []],
            "commit_percent": [0, 101, True], "withdraw_avg_hp_percent": [-1, 91, True],
            "destination_cell": [[40, 80]], "unexpected": [1],
        }
        for key, values in changes.items():
            for value in values:
                candidate = copy.deepcopy(self.plan)
                candidate["orders"][0][key] = value
                with self.subTest(key=key, value=value), self.assertRaises(bridge.BridgeError):
                    bridge.validate_plan(candidate, self.snapshot)
        self.plan["orders"][0].update(action="defend_area", target_id=None, destination_cell=[40, 80])
        bridge.validate_plan(self.plan, self.snapshot)
        for cell in ([0, 1], [127, 1], [128, 10], [10, True], [10], None):
            candidate = copy.deepcopy(self.plan)
            candidate["orders"][0]["destination_cell"] = cell
            with self.subTest(cell=cell), self.assertRaises(bridge.BridgeError):
                bridge.validate_plan(candidate, self.snapshot)

    def test_category_gates_and_duplicate_groups(self):
        self.plan["orders"][0]["action"] = "raid_power"
        with self.assertRaises(bridge.BridgeError):
            bridge.validate_plan(self.plan, self.snapshot)
        self.plan["orders"][0].update(action="harass_economy", target_id="16777225:1")
        with self.assertRaises(bridge.BridgeError):
            bridge.validate_plan(self.plan, self.snapshot)
        self.plan = bridge.mock_plan(self.snapshot)
        self.plan["orders"][1]["group_id"] = "ground"
        with self.assertRaises(bridge.BridgeError):
            bridge.validate_plan(self.plan, self.snapshot)

    def test_malformed_snapshot_does_not_enter_worker(self):
        invalid = []
        for key, value in (("protocol_version", True), ("sim_frame", -1), ("groups", []), ("map_bounds", {})):
            candidate = snapshot()
            candidate[key] = value
            invalid.append(candidate)
        candidate = snapshot()
        candidate["groups"][0]["count"] = 300
        invalid.append(candidate)
        candidate = snapshot()
        candidate["target_candidates"][0]["category"] = []
        invalid.append(candidate)
        for candidate in invalid:
            with self.subTest(candidate=candidate), self.assertRaises(bridge.BridgeError):
                bridge.validate_snapshot(candidate)

    def test_request_uses_forced_responses_function_call(self):
        def fake_urlopen(request, timeout):
            body = json.loads(request.data)
            self.assertEqual(request.full_url, "https://example.invalid/v1/responses")
            self.assertEqual(timeout, 20)
            self.assertEqual(body["tool_choice"], {"type": "function", "name": "submit_tactical_plan"})
            self.assertFalse(body["parallel_tool_calls"])
            self.assertFalse(body["store"])
            self.assertEqual(json.loads(body["input"]), self.snapshot)
            return io.BytesIO(json.dumps(api_response(self.plan)).encode("utf-8"))
        with patch.object(bridge, "open_api", fake_urlopen):
            self.assertEqual(bridge.request_plan(self.snapshot, "test-model", "test-only", "https://example.invalid/v1", 20), self.plan)

    def test_http_failure_does_not_expose_response_or_key(self):
        error = bridge.urllib.error.HTTPError("https://example.invalid", 401, "secret-response", {}, io.BytesIO(b"secret-response"))
        with patch.object(bridge, "open_api", side_effect=error):
            with self.assertRaisesRegex(bridge.BridgeError, "^API HTTP status 401$"):
                bridge.request_plan(self.snapshot, "test-model", "test-only", "https://example.invalid/v1", 20)

    @unittest.skipUnless((OUTPUT / "tactics_test.exe").is_file(), "run test_redalert_ai.py --test tactics_test first")
    def test_actual_controller_snapshot_function_call_and_execution_feedback(self):
        child = subprocess.Popen([str(OUTPUT / "tactics_test.exe"), "--llm-pipe"], cwd=OUTPUT,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            exported = bridge.validate_snapshot(bridge.parse_json(child.stdout.readline()))
            self.assertEqual(exported["groups"][0]["units"][0]["type"], 'tank"\\\n')
            plan = bridge.mock_plan(exported, "attack_target")
            parsed = bridge.extract_plan(api_response(plan), exported)
            child.stdin.write(bridge.encode_plan(parsed, exported).hex() + "\n")
            child.stdin.flush()
            output, errors = child.communicate(timeout=10)
            self.assertEqual(child.returncode, 0, errors)
            feedback = bridge.validate_snapshot(bridge.parse_json(output.strip()))
            self.assertIn({"snapshot_seq": exported["snapshot_seq"], "group_id": "ground", "status": "accepted"},
                          feedback["last_plan_results"])
            self.assertEqual(feedback["active_plan"][0]["action"], "attack_target")
            self.assertEqual(feedback["active_plan"][0]["member_count"], 6)
        finally:
            if child.poll() is None:
                child.kill()
                child.communicate()


@unittest.skipUnless(sys.platform == "win32", "Windows shared memory integration")
class NativeTransport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUTPUT.mkdir(parents=True, exist_ok=True)
        environment = msvc_environment()
        cls.executable = OUTPUT / "llm_transport_test.exe"
        subprocess.run([
            environment["AI_TEST_COMPILER"], "/nologo", "/EHsc", "/W4", "/WX", "/Od", "/MT",
            "/I" + str(ROOT / "REDALERT"), "/Fo" + str(OUTPUT) + os.sep, "/Fe" + str(cls.executable),
            str(ROOT / "tests/llm_transport_test.cpp"), str(ROOT / "REDALERT/LLMBRIDGE.CPP"),
            str(ROOT / "REDALERT/AILOG.CPP"),
        ], cwd=OUTPUT, env=environment, check=True)

    def test_shared_memory_gates_seqlock_and_native_packet_validation(self):
        exported = snapshot()
        path = OUTPUT / "llm-snapshot.json"
        path.write_text(json.dumps(exported), encoding="ascii")
        name = "test_" + uuid.uuid4().hex
        channel = bridge.SharedMemory(name, 4)
        environment = dict(os.environ, AIBOOST_LLM="1", AIBOOST_LLM_CHANNEL=name)
        child = subprocess.Popen([str(self.executable), str(path)], cwd=OUTPUT, env=environment,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        def command(text):
            child.stdin.write(text + "\n")
            child.stdin.flush()
            return child.stdout.readline().strip()
        def packet(data, declared_length=None, stable=True):
            version = struct.unpack_from("<I", channel.memory, 16)[0]
            odd = (version + 1) | 1
            struct.pack_into("<I", channel.memory, 16, odd)
            offset = bridge.HEADER_BYTES + bridge.SNAPSHOT_BYTES
            channel.memory[offset:offset + len(data)] = data
            struct.pack_into("<i", channel.memory, 20, len(data) if declared_length is None else declared_length)
            if stable:
                struct.pack_into("<I", channel.memory, 16, odd + 1)
        try:
            self.assertEqual(child.stdout.readline().strip(), "ready")
            self.assertEqual(channel.read_snapshot(), exported)
            self.assertIsNone(channel.read_snapshot())
            self.assertEqual(struct.unpack_from("<I", channel.memory, 32)[0], child.pid)
            with self.assertRaisesRegex(bridge.BridgeError, "another bridge"):
                bridge.SharedMemory(name, 4)
            plan = bridge.mock_plan(exported, "attack_target")
            channel.write_plan(plan, exported)
            self.assertEqual(command("receive"), "7:2:16777224:2")
            self.assertEqual(command("receive"), "none")
            binary = bridge.encode_plan(plan, exported)
            packet(binary, stable=False)
            self.assertEqual(command("receive"), "none")
            packet(binary[:-1])
            self.assertEqual(command("receive"), "invalid")
            packet(binary, declared_length=bridge.COMMAND_BYTES + 1)
            self.assertEqual(command("receive"), "invalid")
            packet(binary, declared_length=-1)
            self.assertEqual(command("receive"), "invalid")
            malformed = bytearray(binary)
            struct.pack_into("<I", malformed, 4, 2)
            packet(malformed)
            self.assertEqual(command("receive"), "invalid")
            malformed = bytearray(binary)
            struct.pack_into("<i", malformed, 64, 0)  # Duplicate ground group.
            packet(malformed)
            self.assertEqual(command("receive"), "invalid")
            struct.pack_into("<I", channel.memory, 32, os.getpid())
            self.assertEqual(command("allowed"), "blocked")
            struct.pack_into("<I", channel.memory, 32, child.pid)
            former = subprocess.Popen([sys.executable, "-c", "pass"])
            former.wait(timeout=10)
            struct.pack_into("<I", channel.memory, 32, former.pid)
            self.assertEqual(command("allowed"), "allowed")
            self.assertEqual(struct.unpack_from("<I", channel.memory, 32)[0], child.pid)
            struct.pack_into("<I", channel.memory, 28, (channel.kernel.GetTickCount() - 6000) & 0xFFFFFFFF)
            self.assertEqual(command("allowed"), "blocked")
            channel.heartbeat()
            self.assertEqual(command("allowed"), "allowed")
            self.assertEqual(command("offline"), "offline")
            self.assertEqual(struct.unpack_from("<I", channel.memory, 32)[0], 0)
            self.assertEqual(command("allowed"), "blocked")
            child.stdin.write("quit\n")
            child.stdin.flush()
            _, errors = child.communicate(timeout=10)
            self.assertEqual(child.returncode, 0, errors)
        finally:
            if child.poll() is None:
                child.kill()
                child.communicate()
            channel.close()

    def test_real_mock_bridge_process_reads_native_snapshot_and_sends_plan(self):
        exported = snapshot()
        path = OUTPUT / "llm-cli-snapshot.json"
        path.write_text(json.dumps(exported), encoding="ascii")
        name = "cli_" + uuid.uuid4().hex
        log = OUTPUT / (name + ".jsonl")
        memory = mmap.mmap(-1, bridge.MAPPING_BYTES, tagname="Local\\AIBoostLLM-" + name, access=mmap.ACCESS_WRITE)
        worker = subprocess.Popen([
            sys.executable, str(ROOT / "SCRIPTS/llm_bridge.py"), "--mock", "--mock-action", "attack_target",
            "--channel", name, "--house", "4", "--interval", "1", "--log", str(log),
        ], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        native = None
        def wait_until(predicate):
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if predicate():
                    return
                if worker.poll() is not None:
                    output, errors = worker.communicate()
                    self.fail("bridge exited: " + output + errors)
                time.sleep(0.02)
            self.fail("bridge did not produce data before the deadline")
        try:
            wait_until(lambda: struct.unpack_from("<I", memory, 28)[0] != 0)
            environment = dict(os.environ, AIBOOST_LLM="1", AIBOOST_LLM_CHANNEL=name)
            native = subprocess.Popen([str(self.executable), str(path)], cwd=OUTPUT, env=environment,
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.assertEqual(native.stdout.readline().strip(), "ready")
            wait_until(lambda: struct.unpack_from("<I", memory, 16)[0] > 0
                       and not struct.unpack_from("<I", memory, 16)[0] & 1)
            native.stdin.write("receive\nquit\n")
            native.stdin.flush()
            output, errors = native.communicate(timeout=10)
            self.assertEqual(native.returncode, 0, errors)
            self.assertEqual(output.strip(), "7:2:16777224:2")
            wait_until(lambda: log.exists() and '"kind": "plan"' in log.read_text(encoding="utf-8"))
            records = [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()]
            self.assertEqual(records[0]["kind"], "snapshot")
            self.assertEqual(records[0]["data"], exported)
            self.assertTrue(any(record["kind"] == "plan" for record in records))
        finally:
            if native is not None and native.poll() is None:
                native.kill()
                native.communicate()
            worker.terminate()
            worker.communicate(timeout=10)
            memory.close()


@unittest.skipUnless(sys.platform == "win32", "Windows DLL match logging")
class NativeMatchLogs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.output = OUTPUT / "log-fixture"
        cls.output.mkdir(parents=True, exist_ok=True)
        cls.executable = cls.output / "ai_log_test.exe"
        environment = msvc_environment()
        subprocess.run([
            environment["AI_TEST_COMPILER"], "/nologo", "/EHsc", "/W4", "/WX", "/Od", "/MT",
            "/I" + str(ROOT / "REDALERT"), "/Fo" + str(cls.output) + os.sep, "/Fe" + str(cls.executable),
            str(ROOT / "tests/ai_log_test.cpp"), str(ROOT / "REDALERT/AILOG.CPP"), str(ROOT / "REDALERT/LLMBRIDGE.CPP"),
        ], cwd=cls.output, env=environment, check=True)

    def setUp(self):
        self.channel_name = "log_test_" + uuid.uuid4().hex
        self.channel = bridge.SharedMemory(self.channel_name, 4)
        environment = dict(os.environ, AIBOOST_LLM="1", AIBOOST_LLM_CHANNEL=self.channel_name)
        environment.pop("AIBOOST_LOG", None)  # Logging must work by default.
        self.child = subprocess.Popen([str(self.executable)], cwd=ROOT, env=environment,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8")
        self.path = Path(self.child.stdout.readline().strip())
        self.match = self.child.stdout.readline().strip()
        self.assertTrue(self.path.is_file(), self.path)
        self.assertEqual(self.path.parent, self.output / "log")

    def tearDown(self):
        if self.child.poll() is None:
            self.child.kill()
            self.child.communicate(timeout=10)
        self.channel.close()

    def command(self, line):
        self.child.stdin.write(line + "\n")
        self.child.stdin.flush()
        return self.child.stdout.readline().strip()

    def stop(self):
        self.child.stdin.write("quit\n")
        self.child.stdin.flush()
        self.child.communicate(timeout=15)
        self.assertEqual(self.child.returncode, 0)

    def publish(self):
        exported = snapshot()
        exported["match_id"] = self.match
        self.channel.heartbeat()
        self.assertEqual(self.command("publish " + json.dumps(exported, separators=(",", ":"))), "published")
        self.assertEqual(self.channel.read_snapshot(), exported)
        audit = self.channel.audit_for(exported, APIConfig(authorization="Bearer dummy-secret-for-audit"))
        self.assertIsInstance(audit, MatchAudit)
        self.assertEqual(audit.path, self.path)
        return exported, audit

    def rows(self, path=None):
        return [bridge.parse_json(line) for line in (path or self.path).read_text(encoding="utf-8").splitlines()]

    def test_default_module_directory_timestamp_and_match_rotation(self):
        initial_path, initial_match = self.path, self.match
        self.assertRegex(initial_path.name, r"^\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}-\d{3}_p\d+_[0-9a-f]{16}\.jsonl$")
        self.child.stdin.write("rotate\n")
        self.child.stdin.flush()
        self.path = Path(self.child.stdout.readline().strip())
        self.match = self.child.stdout.readline().strip()
        self.assertNotEqual(initial_path, self.path)
        self.assertNotEqual(initial_match, self.match)
        self.stop()
        first, second = self.rows(initial_path), self.rows()
        self.assertEqual([row["event"] for row in first], ["match_start", "match_end"])
        self.assertEqual([row["event"] for row in second], ["match_start", "match_end"])
        self.assertTrue(all(row["match_id"] == initial_match for row in first))
        self.assertTrue(all(row["match_id"] == self.match for row in second))
        self.assertEqual(Path(first[0]["data"]["dll_path"]), self.executable)
        self.assertEqual(first[0]["data"]["previous_log"], "")
        self.assertEqual(Path(second[0]["data"]["previous_log"]), initial_path)

    def test_cpp_and_bridge_append_complete_records_to_the_same_match_log(self):
        exported, audit = self.publish()
        self.assertEqual(self.command("record"), "recorded")
        for index in range(100):
            self.channel.heartbeat()
            audit.record("llm_response", {"index": index, "response_body": {"reasoning": "x" * 16000}})
        self.stop()
        rows = self.rows()
        native = [row for row in rows if row["event"] == "native_fixture_decision"]
        responses = [row for row in rows if row["event"] == "llm_response"]
        self.assertEqual([row["data"]["index"] for row in native], list(range(5000)))
        self.assertEqual([row["data"]["index"] for row in responses], list(range(100)))
        self.assertEqual(native[0]["data"]["quoted"], 'line\nquote"slash\\')
        self.assertEqual(native[0]["data"]["utf8"], "测试")
        self.assertIsInstance(native[0]["data"]["legacy_non_ascii"], str)
        self.assertTrue(all(row["match_id"] == exported["match_id"] for row in rows))
        self.assertTrue(all(row["source"] == "llm_bridge" for row in responses))
        self.assertTrue(all(row["request_id"] == audit.request_id for row in responses))
        self.assertEqual(rows[-1]["event"], "match_end")

    def test_full_api_audit_validation_errors_and_auth_redaction(self):
        exported, audit = self.publish()
        config = APIConfig(authorization="Bearer dummy-secret-for-audit")
        plan = bridge.mock_plan(exported, "attack_target")
        response = chat_response(plan)
        response["choices"][0]["message"]["reasoning"] = "Keep full reasoning, hide dummy-secret-for-audit"
        response["diagnostic"] = {"authorization": "Bearer dummy-secret-for-audit", "token": "another-secret"}
        with patch.object(bridge, "open_api", return_value=io.BytesIO(json.dumps(response).encode("utf-8"))):
            self.assertEqual(bridge.request_plan(exported, config=config, audit=audit), plan)
        invalid = chat_response(plan)
        invalid["choices"][0]["message"].pop("tool_calls")
        with patch.object(bridge, "open_api", return_value=io.BytesIO(json.dumps(invalid).encode("utf-8"))):
            with self.assertRaises(bridge.BridgeError):
                bridge.request_plan(exported, config=config, audit=audit)
        error = bridge.urllib.error.HTTPError(config.api_url, 403, "denied", {},
            io.BytesIO(json.dumps({"error": "dummy-secret-for-audit", "authorization": "another-secret"}).encode("utf-8")))
        with patch.object(bridge, "open_api", side_effect=error):
            with self.assertRaisesRegex(bridge.BridgeError, "403"):
                bridge.request_plan(exported, config=config, audit=audit)
        self.stop()
        text = self.path.read_text(encoding="utf-8")
        self.assertNotIn("dummy-secret-for-audit", text)
        self.assertNotIn("another-secret", text)
        rows = self.rows()
        request = next(row for row in rows if row["event"] == "llm_request")
        self.assertEqual(request["request_id"], audit.request_id)
        self.assertTrue(all(row["request_id"] == audit.request_id for row in rows if row["source"] == "llm_bridge"))
        self.assertEqual(request["data"]["request_body"]["tools"][0]["function"]["name"], "submit_tactical_plan")
        received = next(row for row in rows if row["event"] == "llm_response")
        self.assertIn("Keep full reasoning", received["data"]["response_body"]["choices"][0]["message"]["reasoning"])
        self.assertIn("llm_plan_validated", [row["event"] for row in rows])
        self.assertIn("llm_validation_failed", [row["event"] for row in rows])
        self.assertIn("llm_http_error", [row["event"] for row in rows])


if __name__ == "__main__":
    unittest.main(verbosity=2)
