# Red Alert 实时 LLM 战术控制

[English version](LLM_TACTICS_EN.md) · [中文项目说明](../README_CN.md)

这次实现把现有 AI 的战术层接到 LLM API 的 function calling。
`llm.example.ini` 提供 Cline 的 Chat Completions 接口和
`cline-free/deepseek-v4.1-flash` 示例，实际地址、认证和模型以本地 `llm.ini` 为准；
也支持 OpenAI Responses 协议。
游戏每个模拟秒导出局势，Python 桥接程序异步请求模型，模型调用
`submit_tactical_plan`，C++ 在游戏逻辑线程验证并执行指令。执行反馈随下一份局势返回模型。
HTTP 请求只在 Python 进程运行；DLL 不等待网络，也不持有 API key。

未安装 `llm.ini` 时 LLM 控制关闭；安装配置后由 `[bridge] enabled` 决定是否开启。
逐局详细日志默认开启。LLM 控制当前范围是本地单人遭遇战中的一个电脑阵营，三个战斗组：
`ground`、`naval`、`air`。战役、真人阵营和有两个或以上初始真人参与者的
GlyphX 对战均不接收外部战术。多人局中的真人离开也不会解除这个限制。
生产、采矿、扩张、武器选择、路径、编队、近距离交火和紧急防御继续使用原生控制器。

## 正常进入游戏：无需 PowerShell 或脚本

安装后的 mod 数据目录包含：

```text
<启用的 AIBoost mod>/Data/
    RedAlert.dll
    RedAlert.pdb
    LLMBridge.exe
    llm.ini
    log/
```

`llm.ini` 中保留已有的 `[llm]`、`[headers]`，增加自动桥接设置：

```ini
[bridge]
enabled = true
auto_start = true
channel = v1
house = -1
mode = real
```

从 Steam 正常启动《Command & Conquer Remastered Collection》，选择 **Red Alert**，
在 Mods 中启用 AI-Boost，进入单人遭遇战并加入电脑对手。
DLL 开始运行允许 LLM 控制的单人游戏逻辑后，在后台读取同目录配置；
先检测所选通道上的健康桥接，存在则连接，否则直接启动同目录的 `LLMBridge.exe`。
不会打开 PowerShell 或控制台窗口，不依赖 Python 安装，也不需要特殊游戏启动参数。
模组更新后先退出并重新启动游戏，确保新 DLL 已装入。

检测、配置读取与 EXE 启动由 DLL 的独立后台线程处理；HTTP 请求仍在独立桥接
进程的工作线程运行，游戏逻辑线程只处理共享内存和经过校验的指令。
启动不放在 `DllMain` 中，网络等待期间本地 AI 继续运行。
启动失败会写日志，最多每 30 秒重试一次；同通道互斥锁防止出现两个桥接。
DLL 关闭或进入不允许控制的会话时通知自动启动的桥接结束；游戏异常退出时
桥接通过进程句柄检测退出。若当时已有 API 请求，会先停止发布并等请求结束或超时。
手动运行的既有桥接由用户管理，DLL 不会关闭它。

`enabled = false` 关闭 LLM；`auto_start = false` 只连接已经运行的桥接。
`mode = mock` 可在同样的自然启动流程中离线演练 hold 指令，不请求 API。
这些桥接设置约每秒重新读取；API 地址、认证或模型修改后需退出游戏再重新进入。
`house = -1` 选择最先出现的有效电脑阵营；指定数字使用引擎 house ID，
不保证与 UI 槽位编号一致。多个本地实例使用不同 `channel`。

DLL 自动模式只认实际加载 DLL 同目录的 `llm.ini`，与工作目录无关。
仓库根目录的 `llm.ini` 作为首次安装的配置来源；安装后调整游戏配置应编辑
mod 的 `Data/llm.ini`。安装器默认保留已经存在的目标配置。

启动状态与错误记录在 `Data/log/bridge_<时间戳>_p<PID>.log`，
逐局 JSONL 记录 `llm_bridge_started`、`llm_bridge_connected`、
`llm_bridge_executable_missing`、`llm_bridge_start_failed` 和退出/配置事件。
完整 API 传入、回应和实际战术执行反馈继续写入同一逐局 JSONL。

## 构建与安装（开发者）

构建需要 Windows、Python 3.9 或以上、Visual Studio C++ 工具链与 Windows SDK。
DLL 构建为 Win32；桥接 EXE 可使用 64 位 Python 打包。
桥接运行代码只依赖 Python 标准库，打包使用
[PyInstaller 的单文件模式](https://pyinstaller.org/en/stable/usage.html#what-to-generate)。
构建工具安装在仓库 `build/llm-packager`，不修改系统 Python；EXE 不内嵌配置和认证。

```powershell
./SCRIPTS/Build-RedAlert.ps1
python ./SCRIPTS/build_llm_bridge.py --install-dependencies
./SCRIPTS/Install-RedAlert.ps1 -ModDirectory 'D:\SteamLibrary\steamapps\workshop\content\1213210\2221741447\AIBoost'
```

输出为 `build/redalert/RedAlert.dll`、匹配的 PDB 和 `LLMBridge.exe`。
安装脚本校验目标 RA mod，备份被替换的文件，校验安装后的文件并在失败时恢复。
首次安装会复制仓库已配置的 `llm.ini`；已有目标配置默认保留，
明确需要覆盖时才使用 `-ReplaceLLMConfig`，也可用 `-LLMConfig` 指定来源。
未提供配置时复制模板，真实模式需要先填认证。游戏运行时安装器停止，不改文件。

原有手动启动方式仍可用于开发联调：

```powershell
py -3 ./SCRIPTS/llm_bridge.py --mock --mock-action attack_target --log ./build/llm/mock.jsonl
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ./SCRIPTS/Start-LLMGame.ps1 -GameExecutable 'D:\SteamLibrary\steamapps\common\CnCRemastered\ClientLauncherG.exe'
```

默认 `--mock` 生成 hold 命令；`--mock-action attack_target` 尝试用每组 75% 的
可用战力进攻最近的兼容候选目标。原生安全检查仍可能拒绝该进攻。
手动脚本给进程设置 `AIBOOST_LLM=1`；显式 `AIBOOST_LLM=0` 仍能覆盖 INI 关闭控制。
脚本使用官方 `ClientLauncherG.exe`，由它启动客户端和 `InstanceServerG.exe`。
直接运行 `ClientG.exe` 可能在加载遭遇战时等待缺失的模拟服务。
上述 `ExecutionPolicy` 仅作用于本次进程；正常 Steam 启动没有执行策略要求。

## 连接真实 API

自动模式使用 DLL 同目录的 `llm.ini` 保存实际地址、完整 Authorization、模型和请求头。
开发脚本默认使用仓库根目录的 `llm.ini`。
已经生成可编辑的本地文件，Git 忽略它和常见备份；可发布的模板是
`llm.example.ini`。不要把实际配置或 token 发到聊天、提交到仓库或加入局势日志。

填入完整 Authorization 值，并在同一文件指定实际服务和模型，例如：

```ini
[llm]
protocol = chat_completions
api_url = https://api.openai.com/api/v1/chat/completions
authorization = Bearer <token>
model = gpt
reasoning_effort = xhigh
strict = false
tool_choice = forced
parallel_tool_calls = false
max_output_tokens = 4096
timeout = 60
plan_ttl_seconds = 30
interval = 8
```

`authorization` 不加引号；填整个请求头值，而非只填 token。
`[headers]` 预置示例中的 `User-Agent`、`HTTP-Referer`、`X-Title` 和各个 `X-*`
版本头，可以在同一文件修改。`%`、`#`、`;` 在值中按原文读取，不作插值或行内注释。
HTTP header 值必须是单行 ASCII，禁止在 `[headers]` 再覆盖 Authorization 或 Content-Type。

先只检查配置，命令不会请求网络，也不会显示认证内容：

```powershell
py -3 ./SCRIPTS/llm_bridge.py --check-config
```

开始真实游戏桥接：

```powershell
py -3 ./SCRIPTS/llm_bridge.py --config ./llm.ini --log ./build/llm/live.jsonl
```

脚本默认配置文件路径以仓库位置为准，打包 EXE 默认读取 EXE 同目录的文件；工作目录变化不会找错配置。`--config` 可指定另一个
本地文件。`--model`、`--api-url`、`--protocol`、`--interval`、`--timeout`、`--plan-ttl-seconds` 和
`--strict`/`--no-strict` 可覆盖非密钥项。认证不提供命令行参数。

[Cline 官方文档](https://github.com/cline/cline/blob/main/docs/api/chat-completions.mdx)
说明其接口使用 Chat Completions 的 `tools[].function` 和 `message.tool_calls` 格式。
桥接发送 `stream: false`、`reasoning: {"effort":"xhigh"}`、指定函数的
`tool_choice`（默认 `forced`）和 `parallel_tool_calls: false`，解析真正的 tool call。
同时接受 Cline 实际返回的 `{"success":true,"data":{...}}` 封装，先检查成功状态，
再验证内部 Chat Completions 响应；失败封装不会被当作工具调用。
普通文本里的 JSON 不当作函数调用接受，截断、拒绝、多调用、无效参数均失败。
部分兼容服务使用 `finish_reason: stop` 收尾；只有同时存在完整合法的 tool_calls 才接受。

`strict = false` 是默认兼容设置，表示不要求服务端的严格生成扩展；Python 和 C++
的格式、实例和战术校验始终启用。支持严格生成的服务可以设为 `strict = true`。
若服务只支持 `tool_choice: required`，可在 INI 把 `tool_choice` 改成 `required`；
仍只提供一个函数并要求一次调用，测试不会退化成纯文本 JSON。
如果模型只支持自动选工具，可用 `tool_choice = auto`。实测
`cline-free/muse-spark-1.3-contributor` 的上游会拒绝指定函数和 `required`，
使用 `auto` 后三项真实联调均通过。此模式下模型可能只回复文字，桥接仍会
拒绝该回复；只有一次合法的 `submit_tactical_plan` 调用才会进入 C++，
其余错误回复不会产生游戏指令。本地 `llm.ini` 已按该模型设为 `auto`。
请求禁止重定向，以免认证头被转发到其他地址。

切换到 Responses 时，在 INI 修改 `protocol = responses`、完整
`api_url = https://api.openai.com/v1/responses`、实际模型与认证；`strict = true`
可启用 [OpenAI 严格 function calling](https://developers.openai.com/api/docs/guides/function-calling?api-mode=responses)。
该模式发送 `store: false`；这只是 Responses 存储选项。
只在此原始 OpenAI 地址且 INI 认证留空时兼容 `OPENAI_API_KEY`，不会把这个环境密钥
自动发送到 Cline。模型优先从 INI 或 `--model` 读取。

## 真实 Function Call 测试

不用启动游戏，先完成真实模型 → 函数参数 → C++ 战术控制器的三项联调：

```powershell
py -3 ./SCRIPTS/test_llm_live.py --config ./llm.ini --wait-config
```

`--wait-config` 默认最多等 600 秒，在配置缺失、未填认证、仍是示例 token 或未保存
完整配置时不发送请求；保存有效配置后才开始。也可以先填好 INI，然后不加等待参数运行。
真实请求只发送构造的测试战场，测试进程不控制已安装游戏。

测试自动编译实际 `AITACTICS.CPP` 对应的 C++ fixture，并依次验证：

1. `hold`：正确的单次函数调用令地面组待命。
2. `attack_target`：用目标实例 ID 下达进攻，8 辆坦克按 75% 比例投入 6 辆。
3. `defend_area`：使用 `[35,80]` 格坐标增援，检查原生移动目的地。

每项都检查真实 `tool_calls`、函数名、完整参数及封套，然后编码并交给 C++，
检查 `accepted` 反馈和 `active_plan`。只有三项都通过才退出 0，并把
`build/llm/live-test.json` 的 `passed` 设为 true。退出 1 表示有失败；报告包含
阶段、有限错误原因、请求耗时、参数和执行反馈，不保存 Authorization、请求头、
INI 内容或原始 API 响应。401/403/404 立即停止该项，不反复尝试无效认证。

默认每项最多 3 次格式验证尝试，可以用 `--attempts 1` 禁止重试，用
`--case attack_target` 只测一项，或 `--skip-build` 使用已编译且与当前源码匹配的 fixture。
等待和网络阶段支持 Ctrl+C。测试会发起真实 API 调用，消耗对应账户的额度。
服务端是否支持该模型、工具参数和当前认证，以实测报告为准。
这种通过证明 API/函数/原生战术接口闭环成立，不代表模型所有后续决策都能通过，
也不替代安装后真实对局的延迟、效果和 DLL 加载测试。

本次使用实际安装的 Muse 配置完成协议 2 的三项测试，每项第一次调用即通过，C++ 均反馈
`accepted`。请求耗时分别为：待命 2.343 秒、目标进攻 9.078 秒、区域增援 4.141 秒。
实际结果见本机 `build/llm/network-ttl-live-test.json`，其中 `real_api`、`completed`、`passed`
和各项 `real_function_call_passed`、`cpp_execution_passed` 均为 true。
模板中的 DeepSeek 默认模型未包含在本次真实测试中；切换模型后应重新运行测试。

实际游戏桥接默认两次请求至少间隔 8 个现实秒，同时只有一个网络请求。
`ground`、`naval`、`air` 三组的可用单位都为零时暂停新请求，部队重新可用时
自动恢复。快照和桥接心跳继续更新；暂停期间不发空部队 API 请求。
已经发出的请求若返回时仍无可用部队，其回复会被丢弃。

协议 2 将网络等待和执行期限分开：`timeout = 60` 是现实时间的网络等待预算，
允许设置 1–120 秒；响应正文读取也使用剩余总预算，不能靠持续分段发送延长等待。
`plan_ttl_seconds = 30` 是 DLL 接受计划后最长的模拟执行时间，允许设置 1–30 秒。
模型返回 `valid_for_ticks`，范围为 1 到配置的秒数乘以 15；DLL 在完整校验成功时
设置到期帧为当前模拟帧加该时长。默认最长 450 tick，即 30 个模拟秒。
网络等待不消耗这个执行期限，因此快速游戏中的慢回复不会仅因旧快照帧 TTL 耗尽
而被丢弃。DLL 与 EXE 必须一起更新；协议 1 的绝对到期指令会被拒绝。

回复仍须属于原对局、原阵营和真实采样，且不能重复应用。桥接拒绝超出网络预算、
快照已停止更新或已切换对局的回复。DLL 的采样历史按现实时间保留，最长 125 秒
（120 秒最大网络预算加 5 秒采样新鲜度窗口），同时最多 1024 份，避免快速游戏中
旧的 32 份历史在网络回复前被淘汰。对象存活、创建代数、当前兵力、当前路径、
武器与战力安全检查仍在接受时重新执行；不会招募采样之后新建的替代单位。
这是一层秒级战术决策，逐帧寻路、交火与防御由游戏处理。
关闭桥接或 Ctrl+C 后，心跳立即过期，游戏交回原生 AI；API 超时、拒绝或格式
错误时，已有有效计划可以继续到其 TTL，之后自动恢复原生控制。

## 输入数据

每份 JSON 包含：

| 字段 | 含义 |
| --- | --- |
| `protocol_version` | 当前为 `2` |
| `match_id` | 16 位十六进制对局标识，新局及读档都会更新 |
| `snapshot_seq`、`sim_frame` | 局势序号与采样模拟帧 |
| `controlled_house_id` | 受控电脑阵营的引擎编号 |
| `visibility_mode` | 当前为 `omniscient`，来自引擎对象的全知 AI 调试信息 |
| `ticks_per_second`、`map_bounds` | 时间标尺与有效绝对地图格坐标范围 |
| `self` | 资金、电力、耗电和基地格坐标 |
| `groups` | 三个可用战斗组的数量、估计战力、平均血量与单位明细 |
| `target_candidates` | 有兼容武器和可达接近点的敌方目标，以及局部地面/防空威胁 |
| `active_plan` | 当前仍有效的战术、目标/坐标、剩余可用成员数和 TTL，供模型保持计划连续性 |
| `last_plan_results` | 最近接受、拒绝、撤退、过期及防御抢占等执行反馈 |

`groups[].units` 包含实例 ID、INI 类型名、格坐标、当前/最大生命、弹药和原生
任务编号。只导出可用武装部队，脚本队伍、矿车、MCV、正在补弹的飞机与已分配
的紧急防御单位不供模型招募。每组最多 256 个单位。
候选目标每份最多 96 个，按距离基地排序；可能包含敌军、采矿单位与建筑。
未列出的敌人仍参与原生局部威胁计算。`power` 是造价乘剩余血量比例的估计，
不是伤害值或胜率。坐标是绝对地图格，不是屏幕像素。

当前输出是结构化局势，并不输出地形截图/战场图片；可以用日志里的单位与目标
格坐标继续绘制局势图。当前也没有把人类战争迷雾下的可见单位作为数据边界；
不能把这套全知调试数据用于声称公平视野的 AI 对比。

## Function call 输出协议

完整工具定义见 [submit_tactical_plan.tool.json](llm/submit_tactical_plan.tool.json)，
代码中的唯一生成源是 `SCRIPTS/llm_bridge.py` 的 `function_tool()`。
该文件展示 Responses 的平铺工具定义；Chat Completions 请求会把名称、说明和
参数 schema 包在 `tools[].function` 中，参数约束相同。
用以下命令检查当前定义：

```powershell
py -3 ./SCRIPTS/llm_bridge.py --show-schema
```

实际请求把当前 match、序号和 house 写入 schema 的 enum，减少模型填错封套。
同时限制为有兵力的组、当前候选目标和合法执行时长；没有可用兵力时暂停 API 请求。
所有属性必须存在，object 禁止额外属性；不适用的目标/坐标用 `null`。
下面的数字只用于展示格式，执行时必须来自当前采样：

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

| 动作 | 参数与行为 |
| --- | --- |
| `hold` | 目标和坐标均为 null；暂停该组战略进攻，保留近距离自卫 |
| `attack_target` | 指定本份候选中的实例 ID；由原生编队进攻这个目标 |
| `defend_area` | 目标为 null，坐标为 `[x,y]`；向地点增援并交火 |
| `retreat_to` | 目标为 null，坐标为 `[x,y]`；移动撤退，不主动追击 |
| `harass_economy` | 指定采矿单位、矿厂或电厂候选 ID |
| `raid_power` | 仅限 `air`，目标必须是电厂候选 |

一个计划最多三个动作，每组最多一个。计划替换上一份计划，省略的组恢复原生
控制，`orders: []` 主动交还所有组。`commit_percent` 为 10–100 的近似战力比例，
整支单位不能拆分；`hold` 对当前可用组全体生效，不按该比例保留进攻单位。
`withdraw_avg_hp_percent` 为 0–90，0 关闭模型的血量阈值，原生安全判断继续生效。
低血量时原生控制器撤回该次进攻的集结地点，避免海军被命令前往陆地基地。
区域坐标被占用时，可以改用周围两格内可达地点。

Python 先验证 function arguments，再编码固定上界的小端数据包。C++ 再次验证：
对局/阵营、真实采样历史及现实时间上限、序号单调递增、执行 TTL、对象存活和归属、实例代数、
可用兵力、武器、路径及原生进攻安全。任意一条指令不成立时整份计划拒绝，
不会先执行半份。实例 ID 格式为 `引擎TARGET:创建代数`，防止编号复用。
前一份采样后新建的单位不会被该采样的指令招募。
LLM 组控制期间，原生随机/数量优势进攻不会替换其目标，也不会自行添加新兵力。
紧急防御可以优先使用模型成员，剩余成员继续计划或反馈中断。

局势心跳是现实时间，命令 TTL 是模拟帧，二者不混用。
共享内存使用 `Local\AIBoostLLM-v1`，64 字节头部、256 KiB 局势槽、512 字节命令槽，
末尾增加 4 KiB 的 UTF-8 逐局日志路径槽。内部 mapping 版本为 2，JSON 与命令协议仍为 1；
seqlock 防止读取半写入内容。单桥接互斥锁与游戏 PID 认领防止多进程交叉控制。
上一次游戏异常退出后，Windows 确认其进程已结束才允许新游戏接替认领。
DLL 不增添导出接口，不改变游戏对象及保存文件的结构布局。
更新时需要同时使用本次构建的 DLL 和桥接 EXE（开发方式为 Python 脚本），并重启游戏与桥接进程。

## 逐局详细日志

安装本次构建的 `RedAlert.dll` 后，每局开始运行游戏逻辑时自动创建日志，
不需要启用 LLM，也不需要给桥接程序传入 `--log`。
目录从实际加载的 DLL 路径确定：

```text
<RedAlert.dll 所在目录>/log/
    2026-10-07_21-30-15-123_p1234_0000000100000001.jsonl
```

文件名使用本机对局开始时间，精确到毫秒，附加游戏 PID 和对局 ID，避免重名。
文件采用 UTF-8 JSONL，每行一个完整 JSON 对象，便于 Python、日志工具或后续分析程序逐行读取。
进入下一局或读档后创建新文件，`match_start.data.previous_log` 指向上一段日志；
胜负通过 `game_over` / `multiplayer_game_over` 记录，换局和正常关闭 DLL 时写入
`match_end` 并排空后台写入队列。单个玩家结束比赛后仍可继续记录观战期间的战场变化。

每行的公共字段为：

| 字段 | 含义 |
| --- | --- |
| `log_version` | 日志结构版本，目前为 1 |
| `timestamp` | 事件产生时的 UTC 时间，精确到毫秒 |
| `source` / `pid` | `dll` 或 `llm_bridge`，以及产生事件的进程 PID |
| `seq` | 当前来源的递增编号；桥接编号在每次请求中独立计数 |
| `match_id` | 同一局的关联 ID，与 LLM 局势和指令中的对局 ID 相同 |
| `sim_frame` | 模拟帧；DLL 事件使用当时帧，桥接事件使用该请求基于的采样帧 |
| `house_id` | 引擎阵营编号，全局事件为 -1 |
| `event` / `data` | 事件类型与结构化详细数据 |
| `snapshot_seq` / `request_id` | 桥接事件附加的采样序号和本次 API 请求 ID |

同一文件包括全场快照、所有电脑阵营的任务评估和实际下达指令，以及启用 LLM 时
被控制阵营的模型交互。`world_snapshot` 每个模拟秒记录一次（15 tick），包含阵营经济、
电力、生产选择和所有当前活动的车辆、步兵、舰船、飞机、建筑；单位字段包括类型、
格坐标、血量、造价、弹药、任务、排队任务、攻击目标、移动目标及对象创建代数。
快照是供对局结束后分析的完整引擎状态，LLM 接收的局势仍由原有可见性和控制规则筛选。

| 事件 | 记录内容 |
| --- | --- |
| `match_start` / `match_end` / `game_over` / `multiplayer_game_over` | 场景、地图范围、会话类型、DLL 路径、对局关联与胜负/结束原因 |
| `world_snapshot` | 全场阵营和单位状态，含玩家与电脑单位 |
| `house_evaluation` / `production_*` / `strategy_selection` | 电脑阵营每次 AI 评估的前后状态、五类生产选择与战略选择；包括继续等待或未选中生产项 |
| `economy_scan` / `mcv_deployment_plan` | 收入、矿区、矿厂、生产与仓储目标、扩张需求、MCV 目标和路径摘要 |
| `storage_decision` / `essential_recovery` | 仓储候选的收入速度、军队与补充目标、预留资金、空余容量、预测需求及建筑总量剩余空间；必要军事设施的恢复选择 |
| `harvester_*` | 采矿目标、矿厂选择、队列/距离评分和受攻击后的退避判断 |
| `tactics_evaluation` / `defense_decision` / `strike_*` / `all_out_roll` | 兵力评估、防守、候选目标与路线安全、进攻启动/拒绝/取消以及随机突击判断 |
| `strike_batch_ready` | 新组成的推进批次：目标、总成员数、批次人数和战力、其余追赶成员数、集合点及强制进攻标记 |
| `strike_cancel` | 每次活动进攻取消的明确原因，以及存活/到位人数和战力、当前批次人数、目标防御与敌方响应战力、阶段、原集合点、当前路点、路线步数、等待 tick、保留的前线格及是否保留成功 |
| `mission_evaluation` / `order_mission` / `order_target` / `order_destination` | 每次实际任务状态机评估的前后状态，以及 AI 下达的任务、攻击与移动指令 |
| `building_sell_requested` / `building_repair_requested` / `special_weapon_evaluation` | 卖建筑、修理请求和超级武器评估 |
| `llm_snapshot` | DLL 实际发布给模型桥接的完整结构化局势 |
| `llm_request` | API 地址、模型、协议和完整请求体，含提示词、局势、函数 schema 与生成选项 |
| `llm_response` / `llm_http_error` / `llm_network_error` | API 原始返回内容（保留服务封装、reasoning、tool_calls 和 usage）或 HTTP/连接错误 |
| `llm_plan_validated` / `llm_validation_failed` | 函数参数通过本地校验后的计划，或校验失败原因 |
| `llm_plan_submitted` / `llm_plan_discarded` | 写入共享内存的计划，或因超时、换局等丢弃的计划 |
| `llm_command_received` / `llm_plan_rejected` / `llm_plan_applied` / `llm_execution_result` | DLL 收到的命令数据包、校验拒绝、接受后的完整计划和后续执行结果 |

具体事件名和字段以实现为准，核心代码为 `REDALERT/AILOG.CPP`、
`REDALERT/AILOGENGINE.CPP` 和 `SCRIPTS/llm_audit.py`。
任务评估与指令日志覆盖原生 AI 的实际调度；内部路径搜索的每个节点、每个候选评分
和每次武器碰撞计算不逐条展开，战术和采矿控制器会记录专门的决策摘要。

`strike_cancel.data.reason` 的主要取值：

| 原因 | 含义 |
| --- | --- |
| `rally_timeout` / `advance_timeout` | 集结或推进路点连续 40 个模拟秒没有新增集结进展，也没有成功推进 |
| `target_destroyed_or_missing` | 原目标死亡、消失或已不在有效活动状态，且没有成功改选目标 |
| `target_changed_owner` / `target_owner_no_longer_hostile` | 原目标归属变化，或其阵营已不属于可攻击的敌人 |
| `no_available_members` | 进攻成员已没有可控制且存活的战斗单位 |
| `nearby_enemy_superior` | 当前位置附近敌方战力超过普通进攻的继续作战条件 |
| `unreachable_target` / `route_rebuild_failed` | 目标已没有可达接近点，或重建后的剩余路线为空 |
| `unsafe_route` | 剩余路线不满足普通进攻的安全条件 |
| `insufficient_target_strength` / `insufficient_engagement_strength` | 目标防御或可响应敌军超过普通进攻/交战的战力条件 |
| `economy_not_ready` | 没有可用采集能力且资源尚未枯竭，暂停进攻 |
| `model_takeover` / `withdrawing_low_health` | 模型接管替换当前进攻，或模型的血量阈值触发撤退 |
| 其他模型执行状态，如 `superseded`、`expired` | 模型指令释放时透传具体执行状态作为取消原因 |

`progress_wait_ticks / 15` 是距最近一次集结或推进进展的模拟秒数；格坐标可用
`x = frontline_cell % 128`、`y = frontline_cell // 128` 解码。`frontline_cell = -1`
表示没有可保留的存活成员。取消后成员就地警戒，预备队逻辑保留这些单位的位置；
重新进攻优先使用仍有成员驻守的前线位置。明确的撤退指令和基地防守调度仍可移动单位。
`strike_started` 的 `resumed_frontline` / `frontline_cell` 可核对重启是否沿用了前线。

使用 `match_id` 关联整局，使用 `house_id` 分析阵营，使用 `snapshot_seq` 和
`request_id` 串起一次模型请求、回复与桥接校验，再用计划中的采样序号关联 DLL
执行反馈。两个来源共用 Windows 文件锁，保证并发追加不会交错成损坏的 JSON 行；
写入顺序可能受后台排队影响，分析时应同时参考模拟帧和事件时间。

认证请求头、其他 HTTP 请求头和 INI 原文不写入日志；已配置的 Authorization/token
及返回数据中的常见密钥字段会替换为 `[redacted]`。API 回复读取上限保持为 1 MiB，
超过上限的回复会标记 `exceeds_limit` 并拒绝执行，日志保存读取到的部分。
`--log` 仍可额外指定一份简要局势/计划日志，每条 JSONL 记录增加 `timestamp`，
表示写入该条记录时的 UTC 现实时间，精确到毫秒；`kind` 和 `data` 字段保持原有含义。
桥接控制台和自动启动的 `bridge_*.log` 状态消息也带同样的时间前缀，例如
`[2026-10-09T16:30:12.345+00:00] Submitted plan for snapshot 7`。
现实时间与快照中的 `sim_frame` 分别用于定位 API/进程事件和游戏进度。
逐局完整日志不依赖 `--log` 参数，其原有 UTC `timestamp` 继续记录每次 LLM 请求、响应和执行反馈。

`test_llm_live.py` 的 JSON 报告还记录每种测试及每次 API 尝试的 `started_at` / `completed_at`，
以及整份报告的完成时间；失败的 API 尝试同样有完成时间，原有 `elapsed_seconds` 继续表示请求耗时。

原生日志通过后台线程分批写入，每个现实秒刷新，正常换局/关闭时排空队列。
队列上限为 16 MiB；磁盘持续跟不上时会等待可用空间以保留记录。
游戏异常终止时最后尚未写入的队列可能丢失。DLL 目录需要创建 `log` 和写文件的权限；
失败时输出调试信息，桥接追加失败时输出简短错误，原生 AI 继续运行。
日志不会自动清理，长时间比赛可能产生较大文件，分析完成后可自行归档。
临时关闭日志可在启动游戏前设置进程环境变量 `AIBOOST_LOG=0`，移除该变量后恢复默认开启。

## 验证与实际边界

```powershell
py -3 ./SCRIPTS/test_redalert_ai.py
py -3 ./SCRIPTS/test_llm_bridge.py
py -3 ./SCRIPTS/test_llm_autostart.py
./SCRIPTS/Build-RedAlert.ps1
```

离线测试包含原有 AI 回归、真实战术控制器的导出/回包/执行反馈、严格函数格式、
API 请求形状、跨对局/过期/对象复用，以及独立 Python 与 C++ 进程的 Windows
共享内存通信。31 项桥接测试还验证了 INI 桥接参数、DLL 所在目录建日志、时间戳命名、换局关联、
5,000 条原生记录与 100 条较大回复并发追加、UTF-8/旧编码文本、完整 API 数据和认证脱敏。
不需要 API key 或游戏资源。测试里的 API 响应是模拟数据，
并不证明真实网络可达、某个模型的决策质量或已安装游戏能够正确加载 DLL。
真实对局需要装入构建的 mod，配置密钥与模型，然后检查日志里是否依次出现
snapshot、plan 与 accepted/拒绝反馈。仅出现 Submitted plan 表示桥接已经写入，
执行成功要以之后 snapshot 的 `last_plan_results` 为准。

自动启动测试另外运行实际 Win32 C++ 桥接代码和打包后的 EXE，覆盖无环境变量的
INI 自动连接、配置关闭、缺 EXE 时回退、后台启动与命令回传、正常关闭和游戏异常退出，
以及中文/空格目录。它使用 mock 模式，不请求 API；需要先构建桥接 EXE。
