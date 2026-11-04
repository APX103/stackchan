# AI Agent “VAD 失效”静态调研记录

- 日期：2026-10-28
- 分支：`feature/mcp-agent`
- 分析基线：`67d34ebf2e434eacd712ce24db6222138b2cec22`
- 对比范围：`main...HEAD`
- 现象来源：用户反馈 AI Agent 无法做 VAD；本地未复现。
- 调研方式：仅静态审阅源码、配置和分支差异；未编译、未烧录、未运行测试、未采集日志、未做设备调试。
- 版本边界：本文以已提交的上述 HEAD 为准，不纳入写报告时工作区中其他未提交修改。
- 本文目的：保存调研结果，防止遗忘；**不归因、不写解决方案、不表示任何疑点已经得到运行时验证。**

## 1. 当前结论

目前没有找到“本分支直接删除或关闭 AI Agent VAD”的代码或配置。

可以确认：

1. AI Agent、AudioService、AFE/VAD 和 codec 实现文件没有被本分支直接修改。
2. 当前配置仍启用 AFE 和 WebRTC VAD。
3. 新增的 `audio_codec_warmup()` 只是提前构造同一个 codec 单例，后续 AI Agent 会复用；它不是一条直接禁用 VAD 的路径。
4. 没进入 MCP.AGENT / AUDIO.TEST 时，它们的主要运行任务不会在后台持续运行。Mooncake 安装 App 不等于启动 App。
5. 如果打开过这些新页面，它们会操作与 AI Agent 共用的 codec；退出时存在状态遗留和强删任务跳过清理的风险。但这些状态与“VAD 失效”之间尚未建立运行时因果关系。
6. 当前不能把 VAD 问题唯一归因于 warmup、MCP、AUDIO.TEST、IDF 升级、LVGL 配置或任何一个 App。

## 2. 先明确 App 生命周期：导航页不会运行所有 App

这一点是后续判断“没打开的新 App 是否占用计算资源”的基础。

### 2.1 安装不等于运行

`app_main()` 中会安装多个 App：

- `firmware/main/main.cpp:42-52`

`installApp()` 的意义是把 App 注册进 Mooncake，不代表每个 App 的 `onOpen()`、`onRunning()` 或硬件任务都同时启动。

因此，启动器显示多个图标，不代表这些 Application 都在 FreeRTOS 后台并行执行各自的主要工作。

### 2.2 新 App 的任务启动时机

- MCP.AGENT 的 HTTP server 和 `mcp_tool` 任务：在 `AppMcpAgent::onOpen()` 调用 `service_start()` 后创建。  
  `firmware/main/apps/app_mcp_agent/app_mcp_agent.cpp:106-115`  
  `firmware/main/hal/mcp_lan/lan_service.cpp:314-372`
- MCP 麦克风任务：不是仅进入页面就创建，而是在 WebSocket 客户端发送 `start` 后按需创建。  
  `firmware/main/hal/mic_ws_stream.cpp:124-142`  
  `firmware/main/hal/mic_ws_stream.cpp:401-427`
- MCP 扬声器任务：同样在 WebSocket 客户端 `start` 后按需创建。  
  `firmware/main/hal/speaker_ws_stream.cpp:190-213`  
  `firmware/main/hal/speaker_ws_stream.cpp:268-289`
- AUDIO.TEST 的音频任务：进入该 App 的 `onOpen()` 后创建。  
  `firmware/main/apps/app_audio_test/app_audio_test.cpp:150-180`

所以，如果从未进入这些 App，就不存在它们的 HTTP、VAD 无关 MCP 工具、音频流或 FFT 循环在后台持续运行的问题。

### 2.3 仍然存在的非后台型开销

“不运行 App 任务”不等于“分支完全没有全局变化”。以下两项即使不进入 App 也存在：

- App 安装时的一次性构造和少量对象内存。
- AUDIO.TEST 的全局 FFT 对象 `kFft`：两张各 256 个 float 的表，约 2 KB 静态数据，并在进程启动时做一次初始化计算；不是后台持续执行 FFT。  
  `firmware/main/apps/app_audio_test/app_audio_test.cpp:71-123`

## 3. 本分支没有直接修改的 AI/VAD 文件

对比 `main...HEAD`，以下范围没有差异：

- `firmware/main/apps/app_ai_agent/`
- `firmware/xiaozhi-esp32/main/application.cc`
- `firmware/xiaozhi-esp32/main/audio/`
- `firmware/main/hal/board/cores3_audio_codec.cc`
- `firmware/main/hal/board/stackchan.cc`

当前工作区配置仍包含：

- 未设置 `CONFIG_WAKE_WORD_DISABLED`：`firmware/sdkconfig:758`
- 使用 AFE 接口：`firmware/sdkconfig:805`
- 使用 WebRTC VAD：`firmware/sdkconfig:808`
- 配置 StackChan 唤醒词：`firmware/sdkconfig:848`

MCP 的新增 Kconfig 没有覆盖这些选项：

- `firmware/main/Kconfig.projbuild:14-47`

所以目前没有看到“因为启用 MCP，编译器把 AI VAD 关闭或删除”的路径。

## 4. AI Agent 的音频与 VAD 启动链路

### 4.1 AI 启动

1. 打开 AI.AGENT 时，App 设置小智启动请求：  
   `firmware/main/apps/app_ai_agent/app_ai_agent.cpp:36-45`
2. 主循环处理请求，退出 Mooncake 并进入 `startXiaozhi()`：  
   `firmware/main/main.cpp:57-75`
3. `Application::Initialize()` 获取 codec，初始化并启动 AudioService：  
   `firmware/main/hal/board/hal_bridge.cc:112-120`  
   `firmware/xiaozhi-esp32/main/application.cc:62-100`
4. `AudioService::Initialize()` 调用 `codec_->Start()`，构造 Opus、resampler 和 AFE 组件：  
   `firmware/xiaozhi-esp32/main/audio/audio_service.cc:62-123`
5. `AudioService::Start()` 创建 `audio_input`、`audio_output` 和 `opus_codec` 任务：  
   `firmware/xiaozhi-esp32/main/audio/audio_service.cc:125-167`

### 4.2 输入数据路径

`AudioInputTask` 只有在 audio-testing、wake-word 或 audio-processor 至少一个事件位有效时才读取 codec：

- `firmware/xiaozhi-esp32/main/audio/audio_service.cc:230-269`

读取后，数据被喂给对应消费者：

- `firmware/xiaozhi-esp32/main/audio/audio_service.cc:268-284`

本板 codec 输入为 24 kHz，AI 侧目标输入为 16 kHz；`ReadAudioData()` 负责按需启用输入并经过 resampler：

- `firmware/xiaozhi-esp32/main/audio/audio_service.cc:184-227`

### 4.3 Wake Word 与 Listening VAD 不是同一个状态

- Idle 状态主要启用唤醒词检测，并关闭 voice processing：  
  `firmware/xiaozhi-esp32/main/application.cc:849-857`
- 进入 Listening 后才启用 voice processing 和 AFE processor：  
  `firmware/xiaozhi-esp32/main/application.cc:863-878`
- 当前配置在 Listening 时不继续做唤醒词检测：  
  `firmware/xiaozhi-esp32/main/application.cc:880-886`  
  `firmware/sdkconfig:762`

因此，“没有唤醒”与“进入 Listening 后没有 VAD”是不同现象，不能混用。

### 4.4 VAD callback 的语义

AFE processor 在 `VAD_SILENCE` / `VAD_SPEECH` 状态变化时触发回调：

- `firmware/xiaozhi-esp32/main/audio/processors/afe_audio_processor.cc:155-164`

Application 使用该事件更新 `voice_detected_` 并处理 LED：

- `firmware/xiaozhi-esp32/main/audio/audio_service.cc:101-110`
- `firmware/xiaozhi-esp32/main/application.cc:233-238`

处理后的音频送往编码器并不完全依赖本地 VAD transition callback：

- `firmware/xiaozhi-esp32/main/audio/processors/afe_audio_processor.cc:166-185`

所以“没有观察到 VAD callback”不一定等于“麦克风数据没有编码或发送”，后续定位时需要先区分这两类现象。

## 5. 对新增 codec warmup 的评估

### 5.1 修改内容

只要 `CONFIG_MCP_AGENT_ENABLE=y`，`app_main()` 就会在 HAL 初始化后、AI 启动前调用 warmup：

- `firmware/main/main.cpp:23-33`
- 当前配置确实启用：`firmware/sdkconfig:608-611`

warmup 只获取 codec 指针并打印日志：

- `firmware/main/hal/mcp_lan/device_access.cpp:22-35`

### 5.2 它创建的是同一个单例

Board 的 `GetAudioCodec()` 使用函数内静态对象：

- `firmware/main/hal/board/stackchan.cc:701-708`

所以 warmup 后，AI Agent 得到的是同一个 codec，不会再构造第二份。

### 5.3 构造与逻辑启用是两件事

codec 构造函数会创建接口、input/output device，并初始化、启用底层 I²S TX/RX channel：

- `firmware/main/hal/board/cores3_audio_codec.cc:9-76`
- `firmware/main/hal/board/cores3_audio_codec.cc:93-107`
- `firmware/main/hal/board/cores3_audio_codec.cc:178-182`

但 warmup 没有调用逻辑上的 `EnableInput()`、`EnableOutput()` 或 `Start()`。逻辑输入输出由以下接口另行打开：

- `firmware/main/hal/board/cores3_audio_codec.cc:190-231`

`CoreS3AudioCodec` 没有覆写 `Start()`；基类 `Start()` 主要处理音量，不负责开启 VAD 或输入：

- `firmware/xiaozhi-esp32/main/audio/audio_codec.cc:29-38`

**静态结论：** warmup 改变的是构造、底层 I²S 初始化和内存分配的时间点，不是直接禁用 AI 输入或 VAD。它是否通过内存/初始化时序间接影响 AFE，目前没有运行时证据。

## 6. 只有打开过新 App 才涉及的共享 codec 状态

以下内容不适用于“fresh boot 后从未进入新 App，直接运行 AI”的场景。

### 6.1 MCP mic-only 正常退出可能留下 output enabled

MCP 麦克风启动时同时启用 input 和 output：

- `firmware/main/hal/mic_ws_stream.cpp:164-184`

正常 capture 清理只关闭 input：

- `firmware/main/hal/mic_ws_stream.cpp:358-368`

output 的关闭依赖 speaker 任务退出，并且要求 mic client 已不活跃：

- `firmware/main/hal/speaker_ws_stream.cpp:123-135`

如果从未创建 playback task，`SpeakerWsStream::Stop()` 没有独立的 codec output 关闭动作：

- `firmware/main/hal/speaker_ws_stream.cpp:292-318`

因此，mic-only 会话正常退出后，逻辑 output 可能仍为 enabled。

这只是确定的状态遗留；它没有直接证明 AI 的 input 或 VAD 会失效。AI 的 `ReadAudioData()` 仍会在 input flag 为 false 时重新启用输入。

### 6.2 强删任务可能跳过清理

以下路径都可能在等待约 1.5 秒后强制删除任务：

- MCP mic：`firmware/main/hal/mic_ws_stream.cpp:430-448`
- MCP speaker：`firmware/main/hal/speaker_ws_stream.cpp:292-310`
- AUDIO.TEST：`firmware/main/apps/app_audio_test/app_audio_test.cpp:387-399`

AUDIO.TEST 的 input/output 清理同样位于任务正常退出路径：

- `firmware/main/apps/app_audio_test/app_audio_test.cpp:591-595`

强删发生的位置不同，可能留下不同的 codec flag、设备状态、encoder/resampler/decoder 状态。静态代码不能统一推断为“输入一定被关闭”或“输入一定保持可用”。

### 6.3 AI 切换时的卸载不是额外清理保证

MCP 正常页面关闭会调用 `AppMcpAgent::onClose()`：

- `firmware/main/apps/app_mcp_agent/app_mcp_agent.cpp:136-143`

但最终切换到 Xiaozhi 时使用的 `uninstallAllApps()` 本身只是卸载 Mooncake manager，不会逐个走 App 的 `StateGoClose/onClose()`：

- `firmware/components/mooncake/src/mooncake.cpp:86-89`
- `firmware/components/mooncake/src/ability/app_ability.cpp:20-54`

所以 AI 切换过程本身不能被视为对 MCP 音频资源的兜底清理。

## 7. 与 VAD 现象有关的其他静态事实

本节多数是既有代码中的错误或边界处理，不表示都是本分支引入。

### 7.1 Codec 读取失败会被上层当成成功

`CoreS3AudioCodec::Read()` 调用读取接口时使用 `ESP_ERROR_CHECK_WITHOUT_ABORT`，但无论读取结果如何都返回 `samples`：

- `firmware/main/hal/board/cores3_audio_codec.cc:234-239`

基类因此会把 `samples > 0` 当成有效读取：

- `firmware/xiaozhi-esp32/main/audio/audio_codec.cc:21-27`

这意味着读取错误可能被掩盖。至于此时 buffer 是零、旧数据还是有效数据，静态上无法确定。

### 7.2 Resampler 初始化失败后仍继续运行

AudioService 在 resampler 创建失败时记录错误但继续：

- `firmware/xiaozhi-esp32/main/audio/audio_service.cc:86-93`

本板为 24 kHz 输入；如果 resampler 为空，`ReadAudioData()` 会把未重采样的数据按目标 16 kHz 流处理：

- `firmware/xiaozhi-esp32/main/audio/audio_service.cc:191-213`

错误采样率是否就是当前 VAD 不检测的原因，没有运行时证据。

### 7.3 音频/AFE 任务创建结果没有完整检查

- AudioService 输入、输出、编码任务：`firmware/xiaozhi-esp32/main/audio/audio_service.cc:125-167`
- AFE `audio_communication` 任务：`firmware/xiaozhi-esp32/main/audio/processors/afe_audio_processor.cc:67-75`

如果任务创建失败，可能分别表现为没有 codec 读取或没有 AFE/VAD 处理。

### 7.4 VAD 启用依赖 Listening 和 processor 状态

- 未进入 Listening 时，不应期待 Listening VAD 正常工作：`firmware/xiaozhi-esp32/main/application.cc:863-878`
- `EnableVoiceProcessing(false)` 会停止 processor 并清除对应输入事件位：`firmware/xiaozhi-esp32/main/audio/audio_service.cc:579-603`
- processor 停止时，`Feed()` 数据会被丢弃：`firmware/xiaozhi-esp32/main/audio/processors/afe_audio_processor.cc:91-107`
- AFE fetch 返回空或失败时，该轮没有 VAD/output 处理：`firmware/xiaozhi-esp32/main/audio/processors/afe_audio_processor.cc:135-153`
- 进入 Listening 的某些路径会先等待播放队列清空；如果队列不排空，后续 VAD 启用不会执行：`firmware/xiaozhi-esp32/main/application.cc:867-878`

### 7.5 一个既有 callback 状态边界

AFE processor 的 `Stop()` 没有重置 `is_speaking_`。如果上次在 speech 状态停止，重新启动后需要先有 silence 结果复位，之后的 speech rising edge 才会再次触发对应 callback：

- `firmware/xiaozhi-esp32/main/audio/processors/afe_audio_processor.cc:109-121`
- `firmware/xiaozhi-esp32/main/audio/processors/afe_audio_processor.cc:155-163`

这会影响 transition callback，不等于 AFE 永久停止取数。

### 7.6 新音频页面“有电平”不能证明 AI VAD 输入一定正常

MCP mic 路径对样本使用额外数字增益，而 AI AudioService 输入路径没有相同的软件增益：

- MCP：`firmware/main/hal/mic_ws_stream.cpp:288-297`
- AI：`firmware/xiaozhi-esp32/main/audio/audio_service.cc:184-227`

所以新页面里看到明显波形或听到声音，与 AI AFE 获得的电平、采样率和数据内容是否正常，是不同问题。

## 8. 其他全局修改的记录

以下修改不属于某个 App 的后台任务，但会随固件全局存在。当前都没有静态证据证明它们导致 VAD 失效。

1. **IDF 版本记录变化**  
   `firmware/dependencies.lock` 从 5.5.4 变为 5.5.5，`build_agent.ps1` 也固定使用 5.5.5。`idf_component.yml` 和 AFE/VAD 源文件没有对应差异。是否涉及运行时驱动变化，未在本机做版本 A/B。

2. **LVGL 默认刷新周期变化**  
   分支设置 `CONFIG_LV_DEF_REFR_PERIOD=16`：`firmware/sdkconfig.defaults:87-89`。LVGL Kconfig 原默认值为 33 ms：`firmware/managed_components/lvgl__lvgl/Kconfig:113-117`。这是全局 UI/动画配置，不表示所有 App 在后台运行；对 VAD 的 CPU 时序影响未量化。

3. **AUDIO.TEST 静态 FFT 对象**  
   约 2 KB 静态表和启动时一次初始化：`firmware/main/apps/app_audio_test/app_audio_test.cpp:71-123`。它不是后台 FFT 任务；是否影响紧张的内部 RAM 分配，没有运行时证据。

4. **声音播放 guard**  
   `hal_bridge::app_play_sound()` 在 Xiaozhi 未 ready 时直接返回：`firmware/main/hal/board/hal_bridge.cc:147-160`。调用点主要是通知音和相机快门音，没有发现它直接控制 VAD。

5. **舵机修改**  
   本分支的舵机初始化、诊断和模式修改未进入音频/VAD 调用链，目前没有建立关联。

## 9. 按场景保存当前判断

### 场景 A：fresh boot，从未进入新 App，直接运行 AI 就无 VAD

可以排除“新 App 的 onOpen 任务正在后台抢占资源”这一解释，因为这些任务根本没有启动。

仍然存在的分支差异只有全局 warmup、安装/静态对象、LVGL/IDF 等。当前没有静态证据证明其中任何一项必然关闭 VAD。VAD 状态门控、任务创建、resampler、PCM 读取和采样率仍未得到运行时数据。

### 场景 B：进入过 MCP.AGENT / AUDIO.TEST，再回 AI 后无 VAD

新 App 与 AI 共用 codec，且存在正常清理不完整、强删跳过清理、AI 卸载不兜底等事实，因此共享状态残留是合理疑点。

但正常残留 output enabled 本身并不能解释 input/VAD 失效；强删究竟留下了什么状态，也取决于实际中断位置。所以目前仍不能把场景 B 直接归因为“MCP 把 AI VAD 关了”。

### 场景 C：AFE 在运行，但观察不到 VAD transition

需要与“完全没进入 Listening / 没有读麦 / 没有 AFE fetch”区分。callback 状态、输入数据、采样率、音量和 `is_speaking_` 边界都可能影响观察结果；音频编码发送并不完全依赖本地 VAD callback。

## 10. 目前尚未确定、以后定位时不要混淆的问题

- 用户看到的是完全无 VAD callback、无法进入 Listening、无法唤醒，还是音频本身无效？
- 问题在 fresh boot 直接 AI 时出现，还是只有使用过新 App 后出现？
- codec 输入是否实际打开，读取是否报错，PCM 是有效数据、零还是旧数据？
- 24 kHz 到 16 kHz 的 resampler 是否成功创建并参与处理？
- `audio_input`、`audio_communication` 等任务是否真实创建并运行？
- AFE 是否收到数据，是否产生 speech/silence 状态，只是 transition callback 没触发，还是 fetch 本身停止？
- 是否存在内存不足、任务创建失败或初始化时序差异？
- IDF 5.5.4/5.5.5 和全局 LVGL 配置是否与现象有关？目前没有 A/B 证据。

**记录截止：当前仅完成静态审阅，没有确认根因，没有提出或实施修复。**

---

## 11. 2026-10-28 后续运行时复核（与上述静态记录分开）

以下结果来自后续临时测试固件。生产源码已恢复；`PcmDiag` 和自动导航只用于测试，没有留在正式固件。

### 11.1 已通过的数据链路

1. **fresh boot → AI**：`Audio detection task started`，42 个 PCM 窗口，mic 峰值 14,156，无看门狗或复位证据。日志：`ai_fresh_current.log`（临时诊断目录）。
2. **MCP 关闭 → AI**：初次复现了第二次 `StartNetwork()` 只报告 `Station already active`、Application 停在 Starting 的问题。原因是已连接 Station 不会再次发 `Connected`。在“曾打开 MCP 且明确进入 AI”的析构交接中，通过公共 `StopStation()` 让 AI 重新建立 Station 后，检测任务启动，取得 33 个 PCM 窗口，mic 峰值 22,420。正常 Home 不断网，fresh 直接 AI 不执行该重置。日志：`ai_after_mcp_wifi_handoff.log`。
3. **fresh boot → AUDIO.TEST 频谱 → 关闭 → AI**：通过真实 LVGL card 事件进入 SPECTRUM；关闭前先出现 input/output `false`，再到 `on close`，没有强删任务。缩短测试导航时序后，AI 检测任务启动，取得 73 个 PCM 窗口，mic 峰值 16,930。日志：`audio_regression_full.log`。

这些结果证明上述三条路径的 AI 输入、重采样和 WakeNet/AFE 检测任务能够工作。AUDIO.TEST 后 AI 启动时仍有两条 `i2s_channel_disable: the channel has not been enabled yet` 错误日志，但后续 PCM 持续有效；未为消除日志去改公共 HAL。

### 11.2 仍未验证或仍阻塞

- **真实唤醒词、触摸进入 Listening、Listening VAD transition、实际 mic 音质和 speaker 放音**：需要现场人员，不以 PCM 峰值替代。
- **MCP 活动 mic 流成功后再进 AI**：尚未验证。22 KB mic 栈与 Codec DMA 的连续内部 RAM 预算冲突；mic-first 曾触发 DMA 分配失败复位，后已改为先构造 Codec、后申请任务栈，使失败变成明确错误且服务不复位。该保护通过不代表活动 mic 路径通过。
- 20 ms/16 KB mic 候选发生 core dump 确认的栈溢出，已撤回；恢复 60 ms/22 KB。不得把缩帧写成内存问题已解决。
- fresh/MCP/AUDIO.TEST 的短时数据链路通过，不等于所有长时间运行、云端连接、唤醒和触摸场景都已回归。

### 11.3 当前结论边界

原静态报告中“安装不等于运行”仍成立；全局 warmup 和静态 FFT 常驻已移除。后续问题还涉及 App 退出时的 Codec/任务清理和重复网络启动，而不是单纯 VAD 参数错误。生产 `firmware/main/hal` 与线上 `main` 一致，没有用私有 HAL 修改掩盖这些路径。
