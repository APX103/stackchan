# StackChan 局域网 MCP Agent 设计与实现方案

- 版本：v1（待评审）
- 日期：2026-10-27
- 基线分支：`feature/parallel-agent`（基于官方 `m5stack/StackChan` @ `eff0d6b`）
- 参考实现（已废弃，只读）：`backup/agent-mods-20261027`（含上一轮 mic/speaker WebSocket 流的可用代码与全部踩坑记录）
- 面向读者：评审人 + 负责实现的 AI Agent

---

## 0. 给实现者的硬性约束（先读这一节）

1. **禁止修改 `firmware/xiaozhi-esp32/` 内任何文件。** 它是 vendored 第三方仓库（78/xiaozhi-esp32 v2.2.4），只允许 include 其头文件、链接其代码（当作库用）。上一轮实现 patch 了它，导致 `fetch_repos.py` 在新克隆上会静默丢补丁，这是本次重写要根治的问题。
2. **对官方文件的修改仅限 §13 列出的 5 处**，其余一律新增文件。
3. **每个里程碑以 §14 的验收测试通过为唯一完成标准**，先编译、再烧录、再跑脚本验收，并把结果记录到 `firmware/docs/mcp_agent_progress.md`。
4. 开工前先执行 §14 的 M-1 基线清理（当前 vendored 子仓是脏的混合状态，必须先还原）。
5. 遇到与本文档冲突的代码事实，以代码为准，先停下记录到 progress 文件再继续。

---

## 1. 背景与目标

### 1.1 现状（为什么不优雅）

官方 `AI.AGENT` App 的 MCP 能力走云端隧道：

- 设备通过 WebSocket/MQTT 连到 `CONFIG_OTA_URL`（`https://api.tenclass.net/xiaozhi/ota/`）配出的服务器；
- 云端下发的 `{"type":"mcp","payload":…}` 消息由 `Application::OnIncomingJson` 转入 `McpServer::ParseMessage`（`firmware/xiaozhi-esp32/main/application.cc:564-568`，v2.2.4）；
- 回包沿同一条 WebSocket 上行（`application.cc:1069-1075`）；
- 协议只有设备激活/APP 绑定完成后才建立（`application.cc:165+`、`ota.cc:167-185`）。

后果：内网 MCP 客户端无法直连设备；音频/图像数据出公网；云端不可用则 MCP 全废。

### 1.2 目标

新增一个启动器 App **MCP.AGENT**：打开后设备在内网自托管一个标准 MCP 服务（Streamable HTTP 传输），局域网内任意 MCP 客户端（Python `mcp` SDK、Claude Desktop（经 mcp-proxy）、Cursor 等）可直接调用机器人的全部能力：**屏幕显示、收音、放音、双舵机运动、摄像头拍照、LED、提醒、设备状态**。

能力原则上 **⊇ 官方云 MCP**（除 §6.2 中刻意排除的危险/无意义项）。

### 1.3 非目标

- 不修改官方 `AI.AGENT`、`APP.CENTER` 等任何现有 App 的行为（两个 Agent 共存安装、互斥运行，见 §4.4）；
- 不做 TLS（理由见 §11）；
- 不做多客户端并发音频（每条流单客户端）；
- 不做云中继兼容；
- 不做红外（硬件在但官方固件零驱动，缺引脚定义，列为可选 M6+，见 §16）。

---

## 2. 官方云 MCP 工具全集（作为对齐基准）

vendored xiaozhi v2.2.4 `McpServer::AddCommonTools()`（`application.cc:97-99` 调用）注册：

| 工具 | 参数 | 行为 |
|---|---|---|
| `self.get_device_status` | 无 | `board.GetDeviceStatusJson()`（扬声器/屏幕/电池/网络） |
| `self.audio_speaker.set_volume` | `volume` int 0–100 | `codec->SetOutputVolume` |
| `self.screen.set_brightness` | `brightness` int 0–100 | `backlight->SetBrightness` |
| `self.screen.set_theme` | `theme` string | LVGL 主题切换（StackChan 桌面框架不用该主题体系） |
| `self.camera.take_photo` | `question` string | 拍照后**必然上传云端视觉端点**做理解（`camera->Explain`），无 explain_url 则抛错 |

`AddUserOnlyTools()`（仅 `tools/list` 带 `withUserTools:true` 时列出）：`self.get_system_info`、`self.reboot`、`self.upgrade_firmware(url)`、`self.screen.get_info`、`self.screen.snapshot(url,quality)`（需 `CONFIG_LV_USE_SNAPSHOT`，已开）、`self.screen.preview_image(url)`、`self.assets.set_download_url(url)`。

StackChan 板级工具（官方 `firmware/main/hal/hal_mcp.cpp`，由 `Hal::init()` → `xiaozhi_mcp_init()` 注册）：`self.robot.get_head_angles`、`self.robot.set_head_angles(yaw,pitch,speed)`、`self.robot.set_led_color(r,g,b)`、`self.robot.create_reminder/get_reminders/stop_reminder`。

---

## 3. 硬件与系统能力盘点（附证据）

| 能力 | 事实 | 证据 |
|---|---|---|
| SoC | ESP32-S3 @240MHz，16MB Flash，8MB PSRAM(quad/80MHz) | `firmware/README.md`、`sdkconfig.defaults` |
| 屏幕 | ILI9342 2.0" 320×240 RGB565 触摸，背光 AXP2101 DLDO1 | `stackchan.cc:38-54,121-143`、`config.h:30-31` |
| UI | LVGL 9.4.0 + esp_lvgl_port ~2.7.0，LVGL 任务 prio3/core1，全局锁 `LvglLockGuard`（30s 超时） | `idf_component.yml`、`stackchan_display.cc:105-127,288-298` |
| 麦克风 | ES7210，24kHz 立体声交织（slot0=mic，slot1=回采参考），硬件增益实测无效（峰值~241） | `cores3_audio_codec.cc:12-17,190-211` |
| 喇叭 | AW88298，24kHz 单声道输出；**I2S 总线时钟由输出侧驱动**（不开输出则麦克风读全零） | `cores3_audio_codec.cc:93-183` |
| 舵机 | Feetech SCSCL ×2，UART1@1M；yaw(id1) ±128°，pitch(id2) 3°..87°；`move/moveWithSpeed/rotate/lookAtNormalized/lookAtPoint/goHome`；20ms 周期更新任务在 LVGL 锁内 | `hal_servo.cpp:331-361`、`servo.h:17-182`、`hal.cpp:145-177` |
| LED | 12×RGB 霓虹（PY32 扩展器），`setColor(r,g,b)` 带动画 | `neon_light.h:20-70`、`hal_io_expander.cpp:51` |
| 摄像头 | GC0308 DVP，320×240 YUYV@20fps；**传感器不出 JPEG**，软件 `image_to_jpeg` 编码（~500ms、8KB SRAM）；`Capture()` 带快门音效+预览，`StreamCaptures()` 为精简路径 | `stackchan_camera.cc:103,395-852,854-968,1040-1070` |
| 红外 | 机体硬件存在（README），**固件无任何驱动/引脚定义** | `README.md:15`（全文 grep 无 infrared/rmt/NEC 驱动） |
| 分区 | ota_0/ota_1 各 4.94MB；官方固件 3.76MB，**余量 28%** | `partitions.csv`、构建日志 |
| Opus | `espressif/esp_audio_codec ~2.4.1` 提供 `esp_opus_enc/dec` 包装 API（原生 `opus_encode` 未导出）；官方参数：16kHz 单声道 60ms 帧（960 样本/1920 字节 PCM） | `esp_opus_enc.h:120-237`、`audio_service.cc:62-90` |
| httpd | IDF `esp_http_server` 可用；WebSocket 需 `CONFIG_HTTPD_WS_SUPPORT=y`（**官方 defaults 未设**，本方案补）；默认 socket 上限 7 | `sdkconfig`、otto-robot 板参考 `boards/otto-robot/websocket_control_server.cc:87-109` |

内存特征：`SPIRAM_MALLOC_ALWAYSINTERNAL=512`、`RESERVE_INTERNAL=65536`——小分配走内部 SRAM，**内部 RAM 是稀缺资源**（httpd 栈只能给 6KB）；大块（JPEG 缓冲、LVGL 缓存 2MB）走 PSRAM。

---

## 4. 总体架构

### 4.1 形态与生命周期

```
开机 → mooncake 启动器（官方，不动）
         ├─ AI.AGENT（官方，不动）      ← 云端 Agent
         ├─ APP.CENTER / AVATAR / …（官方，不动）
         └─ MCP.AGENT（新增）           ← 打开即成为内网 MCP Server
                onOpen:  画状态页(被动Avatar) → 确保Wi-Fi(§10) → 启动 httpd + 5端点
                onRunning: 状态页实时更新(IP/客户端数/最近事件)
                onClose:  停流 → 停httpd → 清任务 → 回启动器（兜底方案见§10.3）
```

### 4.2 服务组成

单个 `esp_http_server` 实例（默认端口 **8080**，Kconfig 可改）挂载全部端点：

| 端点 | 类型 | 用途 |
|---|---|---|
| `POST /mcp` | JSON | MCP Streamable HTTP（无状态 JSON-RPC 2.0，§5） |
| `GET /` | JSON | 服务状态（名称/版本/IP/端口/uptime/heap/已连客户端） |
| `GET /photo.jpg` | 二进制 | 提供最近一次拍照的 JPEG（§8） |
| `WS /mic` | WebSocket | 麦克风音频流（§7.1-7.2） |
| `WS /speaker` | WebSocket | 扬声器播放流（§7.3） |

### 4.3 关键决策 A：独立实现 MCP 协议层，不复用也不修改 vendored `McpServer`

上一轮实现 patch 了 vendored `mcp_server.cc`（`BeginHttpCapture`/`SetToolCallDispatcher` 等 11 文件 +249 行），带来四个结构性问题：

1. 补丁只存在于脏工作区，`fetch_repos.py:33-40` 在干净克隆上 `git apply --check` 失败会**静默跳过**——构建不可复现；
2. `SetToolCallDispatcher` 是全局静态钩子，与 xiaozhi 主循环的 `Application::Schedule` 互斥；
3. `AddCommonTools()` 双调用会重复注册（需补丁内加幂等守卫）；
4. 回复通道单槽位，并发 HTTP 请求会互相偷回包。

因此本次**自己写一个 ~400 行的独立 MCP 层**（JSON-RPC 解析 + 工具注册表 + schema 生成），工具体直接调用官方公开 API（`GetHAL()` / `GetStackChan()` / `GetBoard()`，见 §6）。vendored 代码只被 include 和链接，永不修改。代价是重写工具体（每个 5-30 行，全部有官方现成实现可对照）。

### 4.4 关键决策 B：与官方 AI.AGENT 的关系——共存安装、互斥运行

- MCP.AGENT 运行期间**绝不启动** xiaozhi `Application`（不 `startXiaozhi`），所以不存在 McpServer 单例、dispatcher、音频服务的竞争；
- 官方 AI.AGENT 打开时会销毁 mooncake 并独占一切（`main.cpp:63-96`）——两个 App 由启动器框架天然互斥，"不影响现有 Agent 能力"由此保证；
- 音频/摄像头归当前前台 App 所有：MCP.AGENT 关闭后一切资源释放干净，AI.AGENT 照常工作。

### 4.5 任务模型（新增 5 个任务）

| 任务 | 栈 | 优先级/核 | 职责 |
|---|---|---|---|
| httpd（内置） | 6144 | 默认 | 处理 /mcp、/、/photo.jpg、WS 握手 |
| `lan_mcp_wait` | 4096 | 3 | Wi-Fi 等待/自愈轮询（§10.1） |
| `mcp_tool` | 8192 | 3/core1 | 串行执行 tools/call 工具体（队列深度 4） |
| `mic_ws` | 24576 | 4/core1 | 采集+Opus 编码+推流（**8K 栈首帧即栈溢出重启，实测结论**） |
| `spk_ws` | 24576 | 4/core1 | 收流+Opus 解码+播放 |

`tools/call` 流程：httpd 任务解析 JSON → 构造 `std::function` 投递 `mcp_tool` 队列 → 二值信号量等待，**30s 超时**（舵机慢动作/拍照 legitimately 慢）→ 超时返回 `-32603`，迟到的工具结果丢弃并记日志。所有工具串行执行（设备是单执行体，串行化反而正确）。

---

## 5. MCP 传输协议设计（Streamable HTTP，无状态）

### 5.1 语义

- 只支持 `POST /mcp` + JSON 响应（`Content-Type: application/json`）；**不支持** SSE 会话（`GET /mcp`、`DELETE /mcp` 返回 405 + 说明 body）；无 `Mcp-Session-Id`。Python `mcp` SDK / mcp-proxy / Cursor 的无状态模式均兼容此形态。
- `initialize`：回显客户端 `protocolVersion`（支持 `2024-11-05` / `2025-03-26` / `2025-06-18`，缺省 `2025-03-26`）；`serverInfo = {name:"stackchan-mcp", version:"1.0.0"}`；`capabilities = {tools:{listChanged:false}}`；**忽略** 客户端 `capabilities.vision`（云端专属，不适用）。
- `ping` → `{}`；`notifications/*` → HTTP 202 空 body；未知方法 → `-32601`。
- JSON-RPC `id` 原生支持 string/number（独立实现的优势，无需上一轮的数字重映射 hack）。
- `tools/list`：支持游标分页（`nextCursor`），**每页 ≤8 个工具或 ≤3KB**——内部 RAM 紧张时大响应会 EAGAIN 中断发送（上一轮实测）。
- Body 上限 32KB（httpd 栈预算）；超限 413。

### 5.2 认证（可选）

Kconfig `MCP_AGENT_TOKEN`（默认空=不启用）。启用时 `/mcp` 校验 `Authorization: Bearer <token>`（常时比较），失败 401；`/mic` `/speaker` 握手校验查询参数 `?token=`。`/` 与 `/photo.jpg` 不设防（只读状态/媒体）。

### 5.3 wire 示例

请求：
```json
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"demo","version":"0"}}}
```
响应：
```json
{"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-03-26","capabilities":{"tools":{"listChanged":false}},"serverInfo":{"name":"stackchan-mcp","version":"1.0.0"}}}
```
`tools/call`（转头看左上并亮红灯）：
```json
{"jsonrpc":"2.0","id":"req-42","method":"tools/call","params":{"name":"self.robot.set_head_angles","arguments":{"yaw":-45,"pitch":60,"speed":300}}}
```
响应：
```json
{"jsonrpc":"2.0","id":"req-42","result":{"content":[{"type":"text","text":"ok"}]}}
```

---

## 6. 工具集规范

### 6.1 总表（19 个，3 组态）

命名空间沿用官方 `self.*`，官方同名工具**参数与语义保持一致**（客户端提示词可无缝复用）。

**对齐官方（13）：**

| 工具 | 输入 | 行为/返回 | 实现要点 |
|---|---|---|---|
| `self.get_device_status` | 无 | 扬声器/屏幕/电池/网络 JSON | `GetBoard()->GetDeviceStatusJson()`（vendored 公开 API） |
| `self.get_system_info` | 无 | 版本/MAC/内存 JSON | `GetBoard()->GetSystemInfoJson()`（官方为 user-only，只读无害，默认开放） |
| `self.audio_speaker.set_volume` | `volume` 0–100 | true | `GetBoard()->GetAudioCodec()->SetOutputVolume()`，官方同款 |
| `self.screen.get_info` | 无 | `{width:320,height:240,monochrome:false}` | 常量即可 |
| `self.screen.set_brightness` | `brightness` 0–100 | true | `GetBoard()->GetBacklight()->SetBrightness(b,true)` |
| `self.robot.get_head_angles` | 无 | `{yaw,pitch}` 度 | `LvglLockGuard` + `GetStackChan().motion()`（官方 `hal_mcp.cpp:28-40` 同款） |
| `self.robot.set_head_angles` | `yaw`[-128,128] `pitch`[0,90]（实际夹到 3..87） `speed`[100,1000] 默认150 | ok | `moveWithSpeed(angle*10, speed)`，官方 `hal_mcp.cpp:43-70` 同款 |
| `self.robot.set_led_color` | `red,green,blue` 0–168 | ok | 双侧霓虹 `setColor`，官方同款 |
| `self.robot.create_reminder` | `duration_s`[1,86400] `message` 默认"Time's up!" `repeat` bool | `{id}` | 复用官方 `apps/common/reminder/reminder.*` 的 `ReminderManager` |
| `self.robot.get_reminders` | 无 | 活跃提醒列表 | 同上 |
| `self.robot.stop_reminder` | `id` | ok | 同上 |
| `self.camera.take_photo` | `quality`[1,100] 默认 20；`embed` bool 默认 false | `{url,width,height,size_bytes,quality}`；`embed=true` 时附 base64 ImageContent | 本地语义，见 §8（官方同名工具必传云端，本版不依赖外网） |
| `self.reboot` | 无（**默认不注册**） | true | Kconfig `MCP_AGENT_ENABLE_OWNER_TOOLS=y` 才注册；执行经 `mcp_tool` 延迟 1s 重启 |

**新增（6）：**

| 工具 | 输入 | 行为 |
|---|---|---|
| `self.display.set_emotion` | `emotion` ∈ `neutral/happy/laughing/angry/sad/crying/sleepy/doubtful` | `GetHAL().setEmotion?` → `StackChanAvatarDisplay` 表情（`stackchan_display.cc:321-372` 枚举）；非法值报错（不静默回 neutral） |
| `self.display.show_message` | `text`(≤200字) `duration_ms` 默认 10000，0=常驻 | Avatar 气泡 `SetChatMessage("assistant", text)`；duration 到期 `ClearChatMessages()`（LVGL timer） |
| `self.display.clear_message` | 无 | 清气泡 |
| `self.microphone.stream` | 无 | `{url:"ws://<ip>:8080/mic",codec:"opus",sample_rate:16000,channels:1,frame_ms:60}`；实际启停由 WS 会话控制帧完成（§7.1） |
| `self.speaker.stream` | 无 | 同上，`/speaker` 端点（§7.3） |
| `self.robot.look_at` | `x,y` ∈ [-1,1] 归一化 | `motion().lookAtNormalized(x,y)`（现成 API，云控没有的顺手能力） |

**不提供（与官方的差异，须在工具 description 或文档说明）：**

| 官方工具 | 不提供原因 |
|---|---|
| `self.upgrade_firmware` | 危险操作，LAN 明文暴露不合适；已有 APP.CENTER OTA 路径 |
| `self.screen.set_theme` | StackChan 桌面不用 xiaozhi 主题体系，无意义 |
| `self.screen.snapshot` / `self.screen.preview_image` | 可选 M6（`CONFIG_LV_USE_SNAPSHOT` 已开，技术可行）；首版不做以控范围 |
| `self.assets.set_download_url` | 云端资产分发设施，与 LAN 无关 |
| 官方 `take_photo` 的 `question` 参数 | 云端视觉理解参数；LAN 版无云端，忽略该参数（传入不报错） |

### 6.2 错误约定

工具执行失败返回 MCP `isError:true` + text 内容为错误描述；参数校验失败（超范围/类型错）返回 `-32602`；工具抛 C++ 异常捕获后转 `isError`，**不允许异常逃逸炸掉 mcp_tool 任务**。

---

## 7. 音频流设计（WebSocket）

上一轮 `/mic` 已真机验证可用（增益/槽位/1006 断连等问题都有实测结论），代码在 `backup/agent-mods-20261027:firmware/main/hal/mic_ws_stream.cc`，**逻辑直接移植、逐条复验**；`/speaker` 只编译通过、未真机验证，M4 预留调试预算。

### 7.1 `/mic` 协议

```
client → WS 连接 ws://<ip>:8080/mic
client → text  {"type":"start"}
server → text  {"type":"hello","sample_rate":16000,"channels":1,"frame_ms":60,"codec":"opus"}
server → binary×N   每个 binary 帧 = 1 个自包含 Opus 包（16kHz/mono/60ms）
client → text  {"type":"stop"}   （或直接断开）
```

- **单客户端**：`client_fd_` 原子 CAS 占用；第二个连接回 `{"type":"error","message":"busy"}` 并关闭；
- `stop` 释放占用即结束（不保证排空余帧）；
- 控制帧之外的未知 text / 二进制帧 → error 帧并忽略；
- 若启用 token，握手需 `?token=`。

### 7.2 采集管线（精确配方，全部为实测结论）

1. `EnableInput(true)` + **`EnableOutput(true)`**（I2S 总线时钟由输出侧驱动，不开输出 ES7210 读回全零——只借时钟不播声音）+ `Start()`；
2. `InputData(buf)` 阻塞读 60ms 块 = **1440 交织样本**（24kHz，[mic, ref, mic, ref…]）；
3. 取偶数 slot（mic），丢弃 ref slot（实测恒为 −2）；
4. **×16 数字增益**（+24dB，带饱和；硬件增益路径在该 codec 栈上实测无效，原始峰值~241）；
5. `esp_ae_rate_cvt` 24k→16k（complexity 2 / SPEED / 单声道）；
6. 累积到 960 样本（1920 字节）→ `esp_opus_enc_process`（参数与官方一致：VBR/DTX/complexity 0）；
7. 编码结果拷入 `shared_ptr<vector>`，经 **`httpd_queue_work` + `httpd_ws_send_frame_async`** 发送（非 httpd 任务发 WS 帧的唯一合法路径）；发送失败或 httpd 队列连续 10 次满 → 释放客户端（对端消失的唯一可靠感知）。

**已知的坑（必须照办）：**
- WS 接收必须**完整排水**（≤4KB 帧整体读完再丢）；固定小 buffer 收大帧会 `ESP_ERR_INVALID_SIZE` 且 payload 不消费 → 会话 1006 横死（`ws_util.h` 有现成实现）；
- WS handler 内**不要判断 `req->method`**（每帧都会进 handler，判错回 error 会把纯 HTTP 响应写进流里破坏协议）；
- `handle_ws_control_frames = false`；
- 编码任务栈 **≥24KB**（8K 首帧栈溢出直接系统 abort）。

### 7.3 `/speaker` 协议与播放管线

- 控制帧对称：client `{"type":"start"}` → server hello（同参数）→ client 发 binary Opus 包（**16kHz/mono/60ms，发送端自行配速 ~16.7 帧/秒**）→ `{"type":"stop"}`；
- 播放任务：`deque<shared_ptr<vector>>` + condition_variable 交接（FreeRTOS queue 的 memcpy 会破坏 shared_ptr 引用计数）；**backlog 上限 8 包（~0.5s）**，超出丢最旧并回 `{"type":"error","message":"speaker backlog, frame dropped"}`；
- 解码器**首包惰性打开**（空闲省 ~30KB）；`esp_opus_dec` 直接解到 24kHz（解码器内部重采样）→ `codec->OutputData(pcm)`；
- `EnableOutput(true)`/`Start()` 幂等覆盖；
- **无 AEC**：播放会串进 `/mic`（文档必须提醒客户端，避免自激）。

### 7.4 音频互斥

`/mic` 与 `/speaker` 各自单客户端，但**允许同一客户端同时开两条**（全双工）。MCP.AGENT App 运行期间音频归它所有；App 关闭即停两条流（§10.3）。

---

## 8. 摄像头设计

- 工具体走**精简采集路径 `StreamCaptures()`**（单帧、无快门音效、无预览，`stackchan_camera.cc:854-968`）——刻意避开 `Capture()` 的快门音效：`app_play_sound` 在 xiaozhi 未运行时会永久阻塞（上一轮实测整机挂死，见 §13 的 hal_bridge 修复）；
- JPEG 编码复用官方 `image_to_jpeg`（`hal_ws_avatar.cpp:426-453` 是现成模板：质量回退 {20,10,5} 适配负载大小）；
- 编码结果存 **PSRAM 缓冲**（互斥锁保护）+ 元数据（宽高/大小/质量/时间戳）；`take_photo` 返回 `{"url":"http://<ip>:8080/photo.jpg",…}`——URL 方式避免在 JSON-RPC 响应里塞几十 KB base64 挤爆内部 RAM；`embed=true` 时才附 base64 ImageContent（≤~40KB，客户端按需选用）；
- `GET /photo.jpg`：`Content-Type: image/jpeg`、`Cache-Control: no-store`，返回最近缓冲；无照片时 404；
- 拍照在 `mcp_tool` 任务串行执行（天然互斥）；首次拍照 ~0.5-1s（自动曝光收敛 + 编码），工具 description 写明预期时延。

---

## 9. App UI 与显示工具

状态页（App 前台界面）：

- 复用 StackChan Avatar，只挂**被动修饰器**（Breath/Blink/Swinging——不挂 idle motion/IMU，舵机只响应用户命令，与官方 AI.AGENT 待机的区别要肉眼可辨）；
- 大字显示 `http://<ip>:8080/mcp`（IP 变化时刷新）；token 启用时显示 `token: ****<末4位>`；
- 两行状态：当前各端点客户端计数、最近事件行（最近一次工具调用/错误，20 字符滚动）；
- 底部提示「BACK 退出并停止服务」。

显示工具实现注意：`set_emotion`/`show_message`/avatar 更新全部在 `LvglLockGuard` 内；`show_message` 的定时清除用 LVGL timer（勿自建任务）。

---

## 10. 网络与生命周期

### 10.1 启动

1. `onOpen`：画状态页 → 查 `WifiManager::IsConnected()`；
2. 已连：直接 `lan_service_start()`；未连：`GetHAL().startNetwork()`（复用官方 65s 阻塞流程，超时→错误页 + 重试按钮；进入配网模式则调 `requestAppSetup()` 走官方 BLE App 配网）；
3. `lan_mcp_wait` 任务每 5s 轮询：Wi-Fi 掉了不杀服务（httpd 存活，客户端自然断开），重连后自愈（启动幂等：已启动则跳过）。

### 10.2 稳定性

Wi-Fi 重连、客户端暴力断开、httpd 队列打满——三个场景都不能崩、不能泄漏（fd/任务/内存）。验收含 10 次连续开关 App 循环（§14 M5）。

### 10.3 退出

`onClose`：停两条流 → `httpd_stop` → 删 `mcp_tool`/waiter 任务 → 音频 `EnableInput/EnableOutput(false)`。**首选优雅关闭直接回启动器**（上一轮用 `requestWarmReboot(2)` 整机重启是兜底，索引还写死）；若实现中发现优雅关闭有残留状态，退回 warm reboot 但**索引必须运行时动态计算**（遍历已安装 App 求本 App 下标），禁止硬编码。

---

## 11. 安全

- 威胁模型：家用/实验局域网。不做 TLS 的理由：证书签发/部署成本远超收益，且 MCP over LAN 的主流客户端也多走明文；敏感性由可选 token 兜底；
- 默认**不注册** owner-only 工具（reboot 等）；`upgrade_firmware` 永不暴露；
- token 见 §5.2；日志不得打印 token/完整 URL 带 token 的形式。

---

## 12. 内存/性能/体积预算

| 项 | 预算 | 说明 |
|---|---|---|
| 新增任务栈合计 | ~67KB | httpd 6K + waiter 4K + mcp_tool 8K + mic 24K + spk 24K（栈从内部 RAM） |
| PSRAM 新增峰值 | ~250KB | JPEG 缓冲 + Opus 句柄/缓冲 |
| 固件体积 | +30~80KB | 官方已用 72%，余 1.4MB，充裕 |
| httpd sockets | 7 上限 | /mcp×N + /mic + /speaker + /photo，同时 2 个 MCP 客户端 + 2 流在预算内；`lru_purge_enable`、独立 `ctrl_port` |
| tools/list | 分页 ≤3KB/页 | 19 工具 schema 约 5KB，分 3 页 |

如构建后发现内部 RAM 不足：`sdkconfig.defaults.local` 加 `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`（让 LWIP/WiFi 缓冲走 PSRAM，上一轮已验证有效）。

---

## 13. 文件规划

### 13.1 新增（全部在 `firmware/main/` 下）

```
apps/app_mcp_agent/app_mcp_agent.h/.cpp        # App：状态页 + 生命周期编排
hal/mcp_lan/mcp_jsonrpc.h/.cpp                 # JSON-RPC 2.0 解析/回包 + Streamable HTTP 语义
hal/mcp_lan/mcp_registry.h/.cpp                # 工具注册表 + inputSchema 生成 + tools/list 分页
hal/mcp_lan/mcp_tools_device.cpp               # device/volume/brightness/info 工具
hal/mcp_lan/mcp_tools_robot.cpp                # 舵机/LED/look_at/reminders 工具
hal/mcp_lan/mcp_tools_display.cpp              # emotion/message 工具
hal/mcp_lan/mcp_tools_camera.cpp               # take_photo + /photo.jpg handler
hal/mcp_lan/lan_service.h/.cpp                 # 启停编排：网络等待、httpd、任务、幂等/自愈
hal/mic_ws_stream.h/.cc                        # 从 backup 分支移植 + 复验
hal/speaker_ws_stream.h/.cc                    # 从 backup 分支移植 + bring-up
hal/ws_util.h                                  # 从 backup 分支原样移植
assets/assets_bin/icon_mcp_agent.bin           # 图标（见下）
docs/mcp_agent_progress.md                     # 实现进度/验收记录（实现者维护）
docs/README_MCP.md                             # 面向使用者的接入说明（M5 产出）
```

参考实现读取方式（不 checkout）：
```
git show backup/agent-mods-20261027:firmware/main/hal/mic_ws_stream.cc
git show backup/agent-mods-20261027:firmware/main/hal/speaker_ws_stream.cc
git show backup/agent-mods-20261027:firmware/main/hal/ws_util.h
```

### 13.2 图标资产管线

1. 取 `firmware/main/assets/assets_bin/icon_ai_agent.bin`，读文件头确认格式与尺寸（`.bin` = LVGL 二进制图：头 + RGB565 像素，`assets.cpp:28-39` 解析）；**新图标按同格式同尺寸生成**（小脚本：PIL 转 RGB565 + 克隆现有文件头改宽高，写进 progress 文件）；
2. 放入 `assets/assets_bin/icon_mcp_agent.bin`——`build_default_assets.py` 自动打包进 assets 分区，`idf.py flash` 会连 assets 一起刷（**图标改动必须刷 assets 分区，只刷 app 不生效**）；
3. App 构造函数：`setAppInfo().name = "MCP.AGENT"`（与 `AI.AGENT` 命名风格一致；如评审想叫 "MCP Agent" 改一个字符串即可）、`icon` 指向 `assets::get_image("icon_mcp_agent.bin")`、`userData` = 主题色（建议 0x3366CC）。

### 13.3 修改官方文件（仅此 6 处，每处一行意图）

| 文件 | 修改 | 意图 |
|---|---|---|
| `firmware/main/apps/apps.h` | +1 include | 注册新 App |
| `firmware/main/main.cpp` | install 列表 +1 行（紧跟 AppAiAgent 之后） | 启动器出现图标；**不加任何其他逻辑**（上一轮在此混入开机日志/强制启动器等改动，本次不要） |
| `firmware/main/CMakeLists.txt` | PRIV_REQUIRES + `esp_http_server` | 链接 httpd |
| `firmware/main/Kconfig.projbuild` | +`MCP_AGENT_ENABLE` / `MCP_AGENT_PORT`(默认8080) / `MCP_AGENT_TOKEN`(默认空) / `MCP_AGENT_ENABLE_OWNER_TOOLS`(默认n) | 配置项，风格对齐现有 `STACKCHAN_SERVER_URL`（`Kconfig.projbuild:1-12`） |
| `firmware/sdkconfig.defaults` | +`CONFIG_HTTPD_WS_SUPPORT=y` | WS 支持（官方 defaults 未设，特性必须自含；不依赖 `.local`） |
| `firmware/main/hal/board/hal_bridge.cc` | `app_play_sound()` 开头加 `if (!is_xiaozhi_ready()) return;` | **修官方隐藏 bug**：xiaozhi 未运行时播音（如快门音效）会永久阻塞整机（上一轮实测）；一行防御，无行为变化 |

### 13.4 明确不动

`firmware/xiaozhi-esp32/`（全部）、`firmware/patches/`、mooncake 组件、官方 8 个 App、`hal_mcp.cpp`（官方云工具注册，随 AI.AGENT 原样保留）、启动器视图。

---

## 14. 里程碑与验收（每步先编译→烧录→跑验收→记录）

**M-1 基线清理（开工第一步）**
- `git -C firmware/xiaozhi-esp32 checkout -- . && git -C firmware/xiaozhi-esp32 clean -fd`（脏修改已在 `backup/agent-mods-20261027:firmware/patches/xiaozhi-esp32_agent_mods_backup_20261027.patch` 留底）；
- 重跑 `python fetch_repos.py` 让官方补丁干净应用；删 `firmware/sdkconfig` + `idf.py fullclean`；
- 验收：官方原版固件 `idf.py build flash monitor` 启动到启动器、AI.AGENT 可正常进（不插网也行，能进配网即算过）。

**M0 骨架**：App + 图标 + 状态页 + 网络编排 + httpd + `GET /`。
- 验收：启动器出现 MCP.AGENT 图标；进入后显示 IP；PC `curl http://<ip>:8080/` 返回状态 JSON；BACK 退出回启动器。

**M1 MCP 核心 + 基础工具**：JSON-RPC 层 + registry + `device/volume/brightness/servo/led` 工具。
- 验收：`python scripts/mcp_smoke.py`（M1 随代码提交，基于官方 `mcp` SDK StreamableHTTP 客户端）全部断言通过；MCP Inspector（`npx @modelcontextprotocol/inspector`）能看到 19 个工具并逐个调用成功。

**M2 显示 + 提醒 + 摄像头**：`display.*`、`reminder.*`、`take_photo` + `/photo.jpg`。
- 验收：smoke 脚本扩展用例；肉眼确认表情/气泡变化；浏览器打开 `/photo.jpg` 看到 320×240 图像；拍两次照片内容不同（确认非缓存）。

**M3 麦克风流**：移植 `/mic`。
- 验收：smoke 脚本连 WS 收 10s Opus → 存文件 → `ffmpeg` 解码出 wav，波形/听感有人声；第二客户端收到 busy；中途断开 3 次不崩、可重连。

**M4 扬声器流（预算 2-3 天调试，上一轮未真机验证）**：移植 + bring-up `/speaker`。
- 验收：PC 用 ffmpeg 把 wav 转成 16k/mono/60ms Opus 裸流按 16.7fps 发送，机器人扬声器出声、清晰无爆音；连发 1000 帧不断流；停止后 `/mic` 仍可用（资源释放干净）。

**M5 加固 + 文档**：token、10 次开关 App 循环、断网自愈（路由器关 30s 再开）、`README_MCP.md`（含 §15 客户端示例）、可选：mDNS（`stackchan-mcp.local`）、owner 工具开关、IR 预研（阻塞在硬件信息，见 §16）。
- 验收：循环/断网场景无崩溃无泄漏（`get_system_info` 的 free_heap 10 次循环后回落到基线 ±10%）；文档评审通过。

---

## 15. 客户端接入指南（写进 README_MCP.md 的素材）

Python（官方 SDK）：
```python
from mcp import ClientSession
from mcp.client.streamable_http import streamablehttp_client
async with streamablehttp_client("http://192.168.x.x:8080/mcp") as (r, w, _):
    async with ClientSession(r, w) as s:
        await s.initialize()
        tools = await s.list_tools()
        await s.call_tool("self.robot.set_head_angles", {"yaw": -45, "pitch": 60})
```

Claude Desktop（经 mcp-proxy，stdio→HTTP 桥）：
```json
{"mcpServers": {"stackchan": {"command": "mcp-proxy", "args": ["http://192.168.x.x:8080/mcp"]}}}
```

扬声器流测试编码参数：
```
ffmpeg -i in.wav -ac 1 -ar 16000 -c:a libopus -application voip -frame_duration 60 -f data out.opus
```
（按帧切分后以 ~16.7 帧/秒发送；`-f data` 出的是长度前缀裸包，脚本去掉前缀取负载。）

---

## 16. 风险与未决问题（需要评审人拍板/提供信息）

| # | 事项 | 状态 |
|---|---|---|
| 1 | **红外**：机体有红外收发硬件（README 明示），但官方固件零驱动、零引脚定义（grep 无 rmt/NEC/infrared 驱动代码）。要做必须先拿到原理图/引脚号（需用户提供），再写 RMT 收发 + `self.ir.send/receive` 工具。 | 阻塞，列为可选 M6+ |
| 2 | `/speaker` 端到端未验证（上一轮仅编译通过），M4 预算已留足；若配速/背压策略需调整，以 M4 实测为准回填本文档。 | 实现期风险 |
| 3 | 内部 RAM 余量：新增 67KB 任务栈为静态推算，M0 起每次验收记录 `get_system_info` 实测值，不足则启用 `SPIRAM_TRY_ALLOCATE_WIFI_LWIP`。 | 监控项 |
| 4 | vendored xiaozhi 未来升级（>v2.2.4）可能动 `Board`/camera 公开 API——本方案不 patch 它，升级风险被限制在 include 的头文件层面。 | 低 |
| 5 | App 命名 `MCP.AGENT`（对齐现有风格）vs `MCP Agent`（用户原话）——一个字符串的事，默认前者。 | 待确认 |
| 6 | 优雅关闭 vs warm-reboot 退出（§10.3）：默认先做优雅关闭，实测有残留再退回 reboot（索引动态计算）。 | 待确认 |

---

## 17. 附录

### A. 上一轮实现的经验教训（已吸收进本设计的条目）

1006 断连（帧排水）、handler 不判 method、BCLK 需开输出、×16 数字增益、24KB Opus 栈、httpd 6KB 栈、32KB body 上限、3KB 分页、单客户端 CAS、async 发送失败即释放、backlog 8 丢帧、`app_play_sound` 挂死、user-only 工具不外露、幂等启动/自愈、AddCommonTools 双注册（本次因不复用 vendored McpServer 而天然消失）。出处：`backup/agent-mods-20261027` 的 `ws_util.h`/`mic_ws_stream.cc`/`speaker_ws_stream.cc`/`http_mcp_server.cc` 注释与 `firmware/patches/xiaozhi-esp32_agent_mods_backup_20261027.patch`。

### B. 证据文件索引（本文引用的主要位置）

`hal_mcp.cpp`（官方云工具）、`xiaozhi-esp32/main/mcp_server.cc`（AddCommonTools/AddUserOnlyTools）、`application.cc:564-568`（云路由）、`hal_app_center.cpp:18`（App 列表源）、`ota.c:127-189`（OTA 机制）、`stackchan_camera.cc`、`hal_ws_avatar.cpp:426-453`（JPEG 模板）、`cores3_audio_codec.cc:93-232`（音频/时钟耦合）、`audio_service.cc:62-90`（Opus 参数）、`stackchan_display.cc:321-372`（表情）、`ability.h:242-287` + `main.cpp:34-41`（App 框架）、`assets.cpp:24-63`（图标管线）、`fetch_repos.py:33-40`（补丁静默跳过）、`otto-robot/websocket_control_server.cc`（httpd WS 参考）。
