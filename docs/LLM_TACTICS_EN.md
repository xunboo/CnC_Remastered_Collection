# Red Alert Real-Time LLM Tactical Control

[Chinese version](LLM_TACTICS_CN.md)

The LLM bridge connects the existing AI's tactical layer to an API that supports
function calling. [llm.example.ini](../llm.example.ini) provides a Cline Chat
Completions example using `cline-free/deepseek-v4.1-flash`. The actual endpoint,
authorization, model, and request settings come from your local `llm.ini`.
The bridge also supports the OpenAI Responses protocol.

The game publishes a tactical snapshot every simulation second. The Python
bridge requests a model plan asynchronously; the model calls
`submit_tactical_plan`, and C++ validates and executes the plan on the game
logic thread. Execution feedback is included in subsequent snapshots.
HTTP requests run only in the Python process. The DLL does not wait for the
network or hold the API key.

LLM control is disabled when `llm.ini` is absent. Once the file is installed,
`[bridge] enabled` controls whether the feature is active. Detailed per-match
logging is enabled by default independently of LLM control.

External tactical control is limited to one computer-controlled house in a
local single-player skirmish, using the `ground`, `naval`, and `air` combat
groups. Campaigns, human-controlled houses, and GlyphX matches with two or more
initial human participants do not accept external tactics. A human leaving a
multiplayer match does not remove this restriction.

Production, mining, expansion, weapon selection, pathfinding, formation movement,
close-range combat, and emergency defense remain under native control. The
current function interface does not include building purchases or production
queue management.

## Normal Game Launch

The installed mod's data directory contains:

```text
<enabled AIBoost mod>/Data/
    RedAlert.dll
    RedAlert.pdb
    LLMBridge.exe
    llm.ini
    log/
```

Keep your existing `[llm]` and `[headers]` sections in `llm.ini`, and add these
automatic bridge settings:

```ini
[bridge]
enabled = true
auto_start = true
channel = v1
house = -1
mode = real
```

Launch **Command & Conquer Remastered Collection** through Steam, select
**Red Alert**, enable AI-Boost in Mods, and start a single-player skirmish with
a computer opponent. When the DLL begins running an eligible session, it reads
the configuration beside the loaded DLL in the background. It first checks for
a healthy bridge on the selected channel. If one is already running, it connects;
otherwise, it starts `LLMBridge.exe` from the same directory.

Normal play requires no PowerShell window, console window, Python installation,
special launch arguments, or environment variables. After updating the mod,
exit and restart the game so that it loads the new DLL.

Bridge detection, configuration reads, and EXE startup run on a dedicated DLL
background thread. HTTP requests run on a worker thread in the separate bridge
process. The game logic thread only handles shared memory and validated commands.
Startup does not run inside `DllMain`; native AI continues while the network
request is pending.

Startup failures are logged and retried at most once every 30 seconds. A
per-channel mutex prevents duplicate bridges. When the DLL closes or the session
becomes ineligible, it asks an automatically started bridge to stop. If the game
crashes, the bridge detects termination through the game process handle. An
in-flight API request stops publishing results and is allowed to finish or time
out. A bridge that was already running manually remains user-managed; the DLL
does not close it.

- `enabled = false` disables LLM control.
- `auto_start = false` connects only to an already running bridge.
- `mode = mock` exercises `hold` plans offline through the same automatic
  startup flow, without an API request.
- `house = -1` selects the first eligible computer-controlled house. A numeric
  value selects an engine house ID, which may differ from the game's UI slot.
- Use different `channel` values for multiple local instances.

Bridge settings are reread approximately once per second. After changing the
API endpoint, authorization, or model, exit and restart the game and bridge.

Automatic mode reads only the `llm.ini` beside the DLL that is actually loaded;
the working directory does not affect this path. The repository's root
`llm.ini` is the configuration source for the first installation. After
installation, edit the mod's `Data/llm.ini` to change game settings. The installer
preserves an existing destination configuration by default.

Startup status and errors are written to
`Data/log/bridge_<timestamp>_p<PID>.log`. The per-match JSONL log records
`llm_bridge_started`, `llm_bridge_connected`,
`llm_bridge_executable_missing`, `llm_bridge_start_failed`, and related
configuration and exit events. Full API requests, responses, and tactical
execution feedback are also written to that match's JSONL file.

## Building and Installing

Building requires Windows, Python 3.9 or later, the Visual Studio C++ toolchain,
and the Windows SDK. The DLL is built for Win32; the bridge EXE can be packaged
with 64-bit Python. The bridge runtime uses only the Python standard library.
Packaging uses [PyInstaller's single-file mode](https://pyinstaller.org/en/stable/usage.html#what-to-generate).
Build dependencies are installed in `build/llm-packager` rather than the system
Python environment. The EXE does not embed configuration or credentials.

Run these commands from the repository root:

```powershell
./SCRIPTS/Build-RedAlert.ps1
python ./SCRIPTS/build_llm_bridge.py --install-dependencies
./SCRIPTS/Install-RedAlert.ps1 -ModDirectory 'D:\SteamLibrary\steamapps\workshop\content\1213210\2221741447\AIBoost'
```

The output is `build/redalert/RedAlert.dll`, its matching PDB, and
`build/redalert/LLMBridge.exe`. The installer validates the target Red Alert mod,
backs up replaced files, verifies the installed files, and restores the backup
if installation fails. It stops without changing files while the game is running.

On the first installation, the installer copies the repository's configured
`llm.ini`. An existing destination configuration is preserved unless you
explicitly use `-ReplaceLLMConfig`. Use `-LLMConfig` to select another source.
If no configured file is supplied, the installer copies the example template;
real API mode then requires authorization to be filled in.

Manual startup remains available for development:

```powershell
py -3 ./SCRIPTS/llm_bridge.py --mock --mock-action attack_target --log ./build/llm/mock.jsonl
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ./SCRIPTS/Start-LLMGame.ps1 -GameExecutable 'D:\SteamLibrary\steamapps\common\CnCRemastered\ClientLauncherG.exe'
```

`--mock` produces `hold` orders by default. With `--mock-action attack_target`,
the bridge attempts to commit 75% of each group's available combat power to its
nearest compatible candidate. Native safety checks can still reject an attack.

The manual launcher sets `AIBOOST_LLM=1` for the game process. An explicit
`AIBOOST_LLM=0` still overrides the INI and disables control. The script uses
the official `ClientLauncherG.exe`, which starts the client and
`InstanceServerG.exe`. Launching `ClientG.exe` directly can leave a skirmish
waiting for the missing simulation service. `ExecutionPolicy` in this command
applies only to that PowerShell process; normal Steam startup has no execution
policy requirement.

## Connecting a Real API

Automatic mode uses the `llm.ini` beside the DLL for the actual endpoint, full
Authorization value, model, and headers. Development scripts use the
repository's root `llm.ini` by default. The file and common backups are ignored
by Git; [llm.example.ini](../llm.example.ini) is the distributable template.
Keep the actual configuration and token out of chat, commits, and snapshot data.

Fill in the complete Authorization header value and specify your provider and
model. This example follows the repository's Cline template:

```ini
[llm]
protocol = chat_completions
api_url = https://api.cline.bot/api/v1/chat/completions
authorization = Bearer <token>
model = cline-free/deepseek-v4.1-flash
reasoning_effort = xhigh
strict = false
tool_choice = forced
parallel_tool_calls = false
max_output_tokens = 4096
timeout = 60
plan_ttl_seconds = 30
interval = 8
```

Do not quote `authorization`; enter the whole header value, not just the token.
The example `[headers]` section includes `User-Agent`, `HTTP-Referer`, `X-Title`,
and `X-*` version headers, all editable in the same file. `%`, `#`, and `;` are
read literally inside values, without interpolation or inline-comment parsing.
HTTP header values must be single-line ASCII. Reserved headers such as
Authorization and Content-Type cannot be overridden in `[headers]`.

Validate the configuration without making a network request or displaying
authorization:

```powershell
py -3 ./SCRIPTS/llm_bridge.py --check-config
```

Start the real bridge manually:

```powershell
py -3 ./SCRIPTS/llm_bridge.py --config ./llm.ini --log ./build/llm/live.jsonl
```

The script's default configuration path is based on the repository location;
the packaged EXE defaults to its own directory. Changing the working directory
does not change either default. `--config` selects another local file.
`--model`, `--api-url`, `--protocol`, `--interval`, `--timeout`,
`--plan-ttl-seconds`, and `--strict`/`--no-strict` override non-secret settings.
There is no command-line argument for authorization.

The Chat Completions request uses the `tools[].function` and
`message.tool_calls` shapes described in the
[Cline API reference](https://github.com/cline/cline/blob/main/docs/api/chat-completions.mdx).
The bridge sends `stream: false`, the configured reasoning effort, a
`tool_choice` derived from the INI, and `parallel_tool_calls: false`. It parses
an actual tool call. Cline's `{"success":true,"data":{...}}` response wrapper
is supported: the bridge checks the success status before validating the inner
Chat Completions response. An error wrapper does not become a game command.

Plain-text JSON is not accepted as a function call. Truncated responses,
refusals, multiple calls, and invalid arguments fail validation. Some compatible
providers finish with `finish_reason: stop`; these are accepted only when a
complete, valid tool call is also present.

`strict = false` is the default compatibility setting and omits the provider's
strict-generation extension. Python and C++ validation remain enabled in all
modes. Providers that support strict generation can use `strict = true`.

`tool_choice = forced` requests the named function. If a provider supports only
the `required` form, set `tool_choice = required`. Both modes expose a single
function and require one call. For models that accept only automatic tool
selection, use `tool_choice = auto`. A previous local test of
`cline-free/muse-spark-1.3-contributor` rejected forced and required selection but
passed all three live integration cases with auto selection. Auto selection may
still produce plain text; only one valid `submit_tactical_plan` call can reach
C++. Set these options for the model you actually use. HTTP redirects are
disabled so that authorization is not forwarded to a different endpoint.

For Responses, set `protocol = responses`, use the full endpoint
`api_url = https://api.openai.com/v1/responses`, and configure your actual model
and authorization. `strict = true` enables the strict-generation option; see
[OpenAI's function-calling documentation](https://developers.openai.com/api/docs/guides/function-calling?api-mode=responses).
This mode sends `store: false`, which is a Responses storage setting.

If authorization is empty, the bridge supports `OPENAI_API_KEY` as a fallback
only for the Responses protocol on the `api.openai.com` host. It does not
automatically send that environment key to Cline or another provider. The model
comes from the INI or `--model`.

## Testing Real Function Calls

Test the actual model-to-function-to-C++ tactical path before starting the game:

```powershell
py -3 ./SCRIPTS/test_llm_live.py --config ./llm.ini --wait-config
```

`--wait-config` waits up to 600 seconds by default. No request is sent while the
configuration is missing, authorization is empty or still a placeholder, or a
complete valid configuration has not been saved. You can also fill the INI
first and run without this option. Real requests contain constructed test
battlefields; the test process does not control the installed game.

The test builds a C++ fixture using the actual tactical controller in
`AITACTICS.CPP`, then checks:

1. `hold`: one correct function call places the ground group on hold.
2. `attack_target`: an instance ID selects the target; eight tanks with a 75%
   commitment recruit six tanks.
3. `defend_area`: `[35,80]` reinforces a map cell and validates native movement
   destinations.

Each case validates real tool calls, the function name, complete arguments, and
the plan envelope, then encodes the packet and passes it to C++. It checks the
`accepted` result and `active_plan`. The default three-case run exits with code
0 and sets `passed: true` in `build/llm/live-test.json` only if all cases pass.
Exit code 1 indicates a failure. With `--case`, all selected cases must pass.

The report includes the stage, a bounded error explanation, request duration,
arguments, and execution feedback. It does not save Authorization, headers,
INI contents, or raw API responses. HTTP 401, 403, and 404 stop the affected
case immediately rather than repeatedly retrying invalid authorization or a
missing endpoint.

Each case allows up to three attempts by default. Use `--attempts 1` to disable
retries, `--case attack_target` to select one case, or `--skip-build` to use an
existing fixture that matches the current source. Configuration waiting and
network requests support Ctrl+C. Live testing makes real API requests and uses
the configured account's allowance.

A passing run proves that the API, function arguments, and native tactical
interface work together for those test cases. It does not guarantee that every
future model decision will be accepted, or replace an installed-game check of
latency, outcomes, and DLL loading. Provider support, model compatibility, and
authorization should be assessed from the actual test report.

One recorded protocol-2 run with the Muse configuration passed all three cases
on the first attempt, with `accepted` from C++. Request times were 2.343 seconds
for hold, 9.078 seconds for target attack, and 4.141 seconds for area defense.
The local report is `build/llm/network-ttl-live-test.json`; `real_api`,
`completed`, `passed`, `real_function_call_passed`, and `cpp_execution_passed`
are true. The template's DeepSeek model was not part of that run. Rerun the
test after changing the model.

## Request Scheduling and Plan Lifetime

The game bridge starts requests at least eight wall-clock seconds apart by
default, with at most one network request in flight. New requests pause when
all three eligible combat groups are empty and resume when troops become
available. Snapshots and bridge heartbeats continue while requests are paused.
If an in-flight request returns while no eligible troops are available, its
reply is discarded.

Protocol 2 separates network waiting from plan execution:

| Setting | Clock and behavior |
| --- | --- |
| `timeout = 60` | Wall-clock network budget; allowed range is 1–120 seconds. Response-body reads share the remaining budget, so incremental delivery cannot extend it indefinitely. |
| `plan_ttl_seconds = 30` | Maximum simulation lifetime after the DLL accepts the plan; allowed range is 1–30 simulation seconds. |
| `valid_for_ticks` | Returned by the model, from 1 to the configured TTL multiplied by 15. The default maximum is 450 ticks. |

After complete validation, the DLL sets expiry to the current simulation frame
plus `valid_for_ticks`. Network waiting does not consume the execution TTL.
A slow reply in a fast game is therefore not discarded solely because an
absolute deadline based on an old snapshot frame has passed. Update the DLL
and EXE together; protocol-1 commands using absolute expiry are rejected.

A reply must still refer to the same match, house, and actual exported snapshot,
and cannot be applied twice. The bridge discards replies that exceed the network
budget, refer to a stalled snapshot stream, or belong to a different match.
The DLL retains snapshot history for up to 125 wall-clock seconds: the maximum
120-second network budget plus the five-second snapshot-freshness window. The
history is also limited to 1,024 snapshots, avoiding the earlier small-history
problem in fast games.

At acceptance, the DLL rechecks live targets, object generations, current
eligible forces, weapons, paths, and local safety. Units created after the
referenced snapshot cannot replace its original members. The LLM supplies
decisions on a seconds scale; the native game handles movement, combat, and
defense on individual frames.

Closing the bridge or pressing Ctrl+C expires its heartbeat immediately and
returns control to native AI. After an API timeout, refusal, or invalid response,
an existing valid plan can continue until its TTL expires, then native control
resumes.

## Input Data

Each tactical snapshot contains:

| Field | Meaning |
| --- | --- |
| `protocol_version` | Currently `2`. |
| `match_id` | A 16-digit hexadecimal match identity. A new match or loaded save creates a new identity. |
| `snapshot_seq`, `sim_frame` | Snapshot sequence and sampled simulation frame. |
| `controlled_house_id` | Engine ID of the controlled computer house. |
| `visibility_mode` | Currently `omniscient`: AI-debug data from engine objects. |
| `ticks_per_second`, `map_bounds` | Simulation time scale and valid absolute map-cell bounds. |
| `self` | Credits, power, drain, and the base cell. |
| `groups` | Counts, estimated combat power, average health, and unit details for the three eligible combat groups. |
| `target_candidates` | Enemy targets with compatible weapons and reachable approach cells, including local ground and anti-air threats. |
| `active_plan` | Still-active actions, targets or coordinates, remaining eligible members, and TTL, helping the model maintain plan continuity. |
| `last_plan_results` | Recent acceptance, rejection, withdrawal, expiry, and defense-preemption results. |

`groups[].units` includes the instance ID, INI type name, map cell, current and
maximum health, ammunition, and native mission number. Only eligible armed
combat units are exported. Scripted teams, harvesters, MCVs, aircraft rearming,
and units already assigned to emergency defense cannot be recruited by the model.
Each group has at most 256 units.

Each snapshot lists at most 96 target candidates, sorted by distance from the
base. Candidates can include enemy combat units, economic units, and buildings.
Enemies omitted from this list still contribute to native local threat
calculations. `power` estimates purchase cost weighted by remaining health; it
is not damage or a probability of victory. Coordinates are absolute map cells,
not screen pixels.

The current interface exports structured data, not terrain screenshots or
battlefield images. Unit and target cells in the logs can be used to plot a
battlefield view. The data is not restricted to units visible through a human
player's fog of war; an evaluation using these omniscient debug snapshots must
describe that visibility model.

## Function-Call Output Protocol

The complete static tool definition is in
[submit_tactical_plan.tool.json](llm/submit_tactical_plan.tool.json).
The generation source is `function_tool()` in
[SCRIPTS/llm_bridge.py](../SCRIPTS/llm_bridge.py). The JSON file shows the flat
Responses tool definition. Chat Completions wraps the name, description, and
parameter schema inside `tools[].function`, with the same argument constraints.

Inspect the current definition with:

```powershell
py -3 ./SCRIPTS/llm_bridge.py --show-schema
```

Actual requests constrain the current match, sequence, and house with schema
enums. They also restrict groups to those with available troops, targets to the
current candidate IDs, and lifetime to the configured limit. Requests pause
when there are no eligible troops. Every property is required; objects reject
extra properties. Use `null` for an inapplicable target or destination.

The values below illustrate the format. An executable plan must use identities
from its actual snapshot:

```json
{
  "protocol_version": 2,
  "match_id": "12345678abcdef01",
  "based_on_snapshot_seq": 7,
  "controlled_house_id": 12,
  "valid_for_ticks": 450,
  "orders": [
    {
      "action": "attack_target",
      "group_id": "ground",
      "target_id": "16777224:2",
      "destination_cell": null,
      "commit_percent": 75,
      "withdraw_avg_hp_percent": 35
    }
  ]
}
```

| Action | Arguments and behavior |
| --- | --- |
| `hold` | Both target and destination are `null`. Suspend strategic attacks for the group while retaining close-range self-defense. |
| `attack_target` | Use an instance ID from this snapshot's candidates. The native formation attacks that target. |
| `defend_area` | Target is `null`; destination is `[x,y]`. Reinforce the location and engage enemies. |
| `retreat_to` | Target is `null`; destination is `[x,y]`. Withdraw without actively pursuing enemies. |
| `harass_economy` | Select a candidate harvester, refinery, or power plant by instance ID. |
| `raid_power` | Available only to `air`; the selected candidate must be a power plant. |

A plan contains at most three orders, with at most one per group. A plan replaces
the previous plan; omitted groups return to native control. `orders: []`
explicitly returns all groups to native control.

`commit_percent` is an approximate share of eligible combat power from 10 to
100; individual units cannot be split. `hold` controls every eligible unit in
its group, rather than recruiting only the specified fraction.
`withdraw_avg_hp_percent` ranges from 0 to 90. Zero disables the model's health
threshold, while native safety checks remain active. When the threshold triggers,
the controller withdraws to the strike's rally location so that naval units are
not sent to a land base. An occupied area destination can be adjusted to a
reachable cell within two cells of the requested location.

Python validates the function arguments, then encodes a bounded little-endian
packet. C++ validates the match and house, actual snapshot history and its
wall-clock age, increasing sequence number, execution TTL, target life and owner,
object generation, eligible members, weapons, routes, and native attack safety.
If any order is invalid, the whole plan is rejected before orders are installed.

Instance IDs have the form `engine TARGET:creation generation`, preventing a
reused engine handle from selecting a replacement object. Units created after
the referenced snapshot cannot be recruited by that snapshot's plan. While an
LLM directive controls a group, native random or numerical-advantage attacks do
not replace its target or add newly produced members. Emergency defense can
preempt model members; the remaining members continue or report an interruption.

Heartbeats use wall-clock time; command TTL uses simulation frames. The shared
memory channel defaults to `Local\AIBoostLLM-v1`, with a 64-byte header, a
256 KiB snapshot slot, a 512-byte command slot, and a trailing 4 KiB UTF-8 match-log
path slot. **The shared-memory mapping version and the JSON/command protocol
version are both 2.** The `v1` in the default channel name is not a protocol
version declaration.

A seqlock prevents partially written reads. A single-bridge mutex and game-PID
claim prevent multiple processes from controlling the same channel. After an
abnormal game exit, Windows must confirm that the previous process has ended
before a new game can take over the claim. The implementation does not add DLL
exports or alter serialized game-object layouts. Update the DLL and packaged
EXE together, or use the matching Python bridge source, and restart both processes.

## Detailed Per-Match Logs

With the current `RedAlert.dll` installed, each match creates a log when game
logic begins. Logging does not require LLM control or the bridge's `--log`
argument. The directory is resolved from the DLL that was actually loaded:

```text
<directory containing RedAlert.dll>/log/
    2026-10-07_21-30-15-123_p1234_0000000100000001.jsonl
```

The filename contains the match's local start time to milliseconds, the game PID,
and the match identity to avoid collisions. Files are UTF-8 JSONL with one
complete JSON object per line, suitable for streaming analysis.

A new match or loaded save creates a new file.
`match_start.data.previous_log` identifies the previous segment. Results are
recorded through `game_over` or `multiplayer_game_over`. Switching matches or
closing the DLL normally writes `match_end` and drains the background queue.
Recording can continue during observation after an individual player finishes.

Common record fields are:

| Field | Meaning |
| --- | --- |
| `log_version` | Log structure version; currently 1. This is separate from tactical protocol version 2. |
| `timestamp` | Event creation time in UTC, to milliseconds. |
| `source`, `pid` | `dll` or `llm_bridge`, and the originating process PID. |
| `seq` | Increasing source sequence. Bridge sequences restart for each API request. |
| `match_id` | Correlation ID shared with the tactical snapshots and plans. |
| `sim_frame` | DLL events use the current simulation frame; bridge events use the sampled frame on which that request was based. |
| `house_id` | Engine house ID; global events use -1. |
| `event`, `data` | Event type and structured details. |
| `snapshot_seq`, `request_id` | Additional bridge fields identifying the snapshot and API request. |

The same file contains full-world snapshots, native task evaluations and orders
for computer houses, and model interactions for the house under LLM control.
`world_snapshot` runs every simulation second, or 15 ticks. It includes house
economy, power, production choices, and all active vehicles, infantry, ships,
aircraft, and buildings. Object fields include type, cell, health, cost,
ammunition, mission, queued mission, attack target, movement destination, and
creation generation.

World snapshots are full engine state for post-match analysis. Tactical snapshots
sent to the model remain filtered by the bridge's eligibility and visibility
rules.

| Event | Recorded data |
| --- | --- |
| `match_start`, `match_end`, `game_over`, `multiplayer_game_over` | Scenario, map bounds, session type, DLL path, match linkage, and result or termination context. |
| `world_snapshot` | Full house and object state, including player and computer units. |
| `house_evaluation`, `production_*`, `strategy_selection` | Before/after AI state, choices across five production categories, and strategy selections, including waiting or no selection. |
| `economy_scan`, `mcv_deployment_plan` | Income, ore fields, refineries, production/storage goals, expansion needs, MCV destinations, and route summaries. |
| `storage_decision`, `essential_recovery` | Storage income rate, army and replenishment goals, reserved funds, capacity, forecast demand, silo goals/limits, growth budget and cash ceiling; recovery selections for essential military facilities. |
| `harvester_*` | Ore targets, refinery selection, queue/distance scores, and retreat decisions under attack. |
| `tactics_evaluation`, `defense_decision`, `strike_*`, `all_out_roll` | Force assessment, defense, candidate targets and route safety, attack start/rejection/cancellation, and random assault decisions. |
| `strike_batch_ready` | A new advancing batch: target, total members, batch count and power, trailing members, rally cell, and forced-attack flag. |
| `strike_cancel` | Cancellation reason; live and ready counts/power; batch size; target defense and response power; phase, original rally cell, current waypoint, route progress, wait ticks, retained frontline cell, and retention status. |
| `mission_evaluation`, `order_mission`, `order_target`, `order_destination` | Before/after mission-state evaluation and actual mission, attack-target, and movement orders issued by AI. |
| `building_sell_requested`, `building_repair_requested`, `special_weapon_evaluation` | Building sale/repair requests and superweapon evaluation. |
| `llm_snapshot` | The complete structured tactical snapshot actually published by the DLL. |
| `llm_request` | Endpoint, model, protocol, and full request body, including instructions, snapshot, function schema, and generation options. |
| `llm_response`, `llm_http_error`, `llm_network_error` | Raw API response data, retaining provider wrappers, reasoning, tool calls, and usage, or HTTP/connection errors. |
| `llm_plan_validated`, `llm_validation_failed` | A locally validated plan or the validation failure reason. |
| `llm_plan_submitted`, `llm_plan_discarded` | A plan written to shared memory or discarded because of timeout, match change, or another bridge condition. |
| `llm_command_received`, `llm_plan_rejected`, `llm_plan_applied`, `llm_execution_result` | Received packet, rejection details, the accepted plan, and subsequent execution status. |
| `llm_bridge_*` | Bridge configuration, startup, connection, missing executable, failure, and lifecycle events. |
| `llm_requests_paused`, `llm_requests_resumed` | Requests paused or resumed as eligible combat groups become empty or available. |

Exact event names and fields are defined by the implementation, principally
[REDALERT/AILOG.CPP](../REDALERT/AILOG.CPP),
[REDALERT/AILOGENGINE.CPP](../REDALERT/AILOGENGINE.CPP), and
[SCRIPTS/llm_audit.py](../SCRIPTS/llm_audit.py). Mission and order records follow
the native AI's actual scheduling. Individual path-search nodes, every candidate
score, and each weapon collision are not expanded into separate records;
tactical and harvesting controllers provide decision summaries.

### Attack Cancellation Reasons

The main values of `strike_cancel.data.reason` are:

| Reason | Meaning |
| --- | --- |
| `rally_timeout`, `advance_timeout` | No further gathering progress or successful advance for 40 simulation seconds at a rally or waypoint. |
| `target_destroyed_or_missing` | The original target is dead, missing, or inactive, and retargeting did not succeed. |
| `target_changed_owner`, `target_owner_no_longer_hostile` | Target ownership changed, or the owner is no longer an attackable opponent. |
| `no_available_members` | No living, controllable combat members remain. |
| `nearby_enemy_superior` | Nearby enemy strength exceeds the continuation condition for an ordinary attack. |
| `unreachable_target`, `route_rebuild_failed` | No reachable approach remains, or the rebuilt remaining route is empty. |
| `unsafe_route` | The remaining route fails the ordinary attack's safety requirements. |
| `insufficient_target_strength`, `insufficient_engagement_strength` | Target defense or responding enemy strength exceeds ordinary attack/engagement requirements. |
| `economy_not_ready` | No eligible harvesting capability is available while resources remain, so attacks pause. |
| `model_takeover`, `withdrawing_low_health` | A model plan replaces the current strike, or its health threshold triggers withdrawal. |
| Other model statuses, such as `superseded` or `expired` | Releasing a model directive passes its execution status through as the cancellation reason. |

`progress_wait_ticks / 15` gives simulation seconds since the last gathering or
advance progress. Decode a retained cell with `x = frontline_cell % 128` and
`y = frontline_cell // 128`. `frontline_cell = -1` means no surviving members
could retain a frontline position.

After cancellation, members guard their current positions. The reserve logic
preserves those positions, and a new strike prefers a frontline still held by
survivors. Explicit retreat orders and emergency base defense can still move
these units. `strike_started.resumed_frontline` and `frontline_cell` show whether
a restarted strike reused the front.

### Correlating Requests and Actual Execution

Use `match_id` to group the match and `house_id` to select a house.
`snapshot_seq` and `request_id` connect one model request, its response, and
bridge validation. The plan's snapshot sequence connects those records to DLL
execution feedback.

`llm_plan_submitted` means the bridge wrote a plan to shared memory.
`llm_command_received` means the DLL read a packet. `llm_plan_applied` and an
`accepted` result mean the native controller accepted the plan. Subsequent
`llm_execution_result`, `last_plan_results`, and native unit orders show what
happened after acceptance; acceptance alone does not prove that an attack reached
or destroyed its target.

Both writers use a shared Windows file lock so concurrent appends do not
interleave JSON lines. Background queuing can affect append order. Analyze
simulation frames and event timestamps together rather than treating file
order as a single global timeline. Bridge events retain the request's sampled
frame even when the response arrives later.

### Credentials, Timestamps, and Storage

Authorization and other HTTP headers, and the original INI contents, are not
logged. Configured Authorization/token values and common secret fields in
returned data are replaced with `[redacted]`. API replies are read up to 1 MiB.
Oversized replies are marked `exceeds_limit` and rejected; the log keeps the
portion that was read.

`--log` optionally creates an additional compact snapshot/plan log. Each JSONL
record includes a millisecond UTC wall-clock `timestamp` for its write time;
the existing `kind` and `data` fields retain their meanings. Console messages
and automatically started `bridge_*.log` status records also have a time prefix:

```text
[2026-10-09T16:30:12.345+00:00] Submitted plan for snapshot 7
```

Wall-clock timestamps identify API/process events; `sim_frame` identifies game
progress. The full per-match log does not depend on `--log`; its UTC timestamps
continue to cover each LLM request, response, and execution result.

`test_llm_live.py` reports `started_at` and `completed_at` for each case and API
attempt, plus the report's completion time. Failed attempts also have a
completion timestamp. `elapsed_seconds` continues to represent request duration.

Native records are written in batches on a background thread and flushed every
wall-clock second. Normal match transitions and shutdown drain the queue.
The queue limit is 16 MiB; if disk writes cannot keep up, producers wait for
space so records are retained. An abnormal game exit can lose the final queued
records that have not yet been written.

The DLL directory must allow creation of `log` and log-file writes. Native log
failures emit debug output; bridge append failures emit a brief error, and native
AI continues. Logs are not automatically cleaned up. Long matches can generate
large files; archive them after analysis as needed.

To disable logging temporarily, set the process environment variable
`AIBOOST_LOG=0` before starting the game. Remove it to restore the default.

## Verification and Practical Limits

Run the offline checks and build from the repository root:

```powershell
py -3 ./SCRIPTS/test_redalert_ai.py
py -3 ./SCRIPTS/test_llm_bridge.py
py -3 ./SCRIPTS/test_llm_autostart.py
./SCRIPTS/Build-RedAlert.ps1
```

The offline suite covers AI regressions, snapshots and plan execution through
the actual tactical controller, strict function formats, API request shapes,
match changes, expiry, object-handle reuse, and Windows shared-memory exchange
between independent Python and C++ processes.

Bridge checks also cover INI options, logs beside the loaded DLL, timestamped
filenames, match linkage, concurrent appends of 5,000 native records and 100
larger replies, UTF-8 and legacy text, full API data, and credential redaction.
They do not require an API key or game assets. Mock API responses do not establish
real network access, model decision quality, or successful loading of the mod by
the installed game.

For an installed-game check, load the built mod, configure the endpoint, model,
and authorization, and examine the snapshot, plan, acceptance/rejection, and
execution records. `Submitted plan` alone confirms only bridge submission.
Review `llm_plan_applied`, `last_plan_results`, subsequent execution statuses,
and actual unit orders to assess in-game behavior.

Automatic startup tests additionally exercise the real Win32 bridge code and
packaged EXE. They cover INI-based startup without environment variables,
configuration disablement, fallback when the EXE is missing, background startup
and command return, normal shutdown, abnormal game exit, and paths containing
Unicode characters or spaces. These tests use mock mode and make no API requests;
build the bridge EXE before running them.
