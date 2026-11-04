# MCP Agent 独立 App 设计

- 版本：v2.0
- 日期：2026-10-28
- 实现分支：`feature/mcp-agent`
- 实测结果：只记录在 `mcp_agent_progress.md`，本文不把设计目标当作验收结果。

## 1. 硬约束

1. MCP.AGENT 是独立 Application。实现集中在 `apps/app_mcp_agent/`，不得调用 AI.AGENT、Avatar 或其他具体 App 的内部实现。
2. 可以调用公共 HAL/Board/StackChan API；`firmware/main/hal/` 整棵树必须与线上 `main` 一致。没有 `servo_heal`、开机舵机修复、开机 codec warmup 或 MCP 私有 HAL 补丁。
3. 不修改 vendored 小智应用服务。硬件单例获取集中在 App 的 `platform/device_access.*`。
4. 安装 App 不等于运行 App。未进入 MCP.AGENT 时，不创建 HTTP、工具或音频任务，也不启动小智服务。
5. MCP 与 AUDIO.TEST 使用独立 Kconfig。AUDIO.TEST 不依赖 MCP 代码，也不由 MCP 开关顺带安装。
6. Home 返回不得等待 HTTPD、硬件或工具任务。正常退出不强删忙碌任务。
7. 不用“编译通过”“smoke 全绿”代替并发、生命周期、AI 语音和实际听音验收。

## 2. 文件边界

```text
firmware/main/apps/app_mcp_agent/
  app_mcp_agent.cpp/.h          # Mooncake 生命周期、Avatar、UI 队列、提醒
  service/
    lan_service.cpp/.h          # App-owned 服务、executor、HTTPD、超时
    reminder_store.cpp/.h       # 只在 UI 上下文访问的提醒存储
    ui_pending.h                # UI 完成、异常、超时取消和晚执行防护
  protocol/
    mcp_jsonrpc.cpp/.h          # JSON-RPC / Streamable HTTP
    mcp_registry.cpp/.h         # 工具注册、调用、分页
    ws_util.h                   # WebSocket 帧接收与有界排水
  tools/
    mcp_tools.cpp               # 19 个默认工具
    mcp_photo.cpp/.h            # 不可变 JPEG 快照
  audio/
    mic_ws_stream.cpp/.h        # 麦克风、重采样、Opus
    speaker_ws_stream.cpp/.h    # Opus 解码和播放
    codec_bus.cpp/.h            # mic/speaker 共享输出时钟计数
  platform/
    device_access.cpp/.h        # 公共 Board/HAL 适配
```

删除该目录后，只保留下列最小集成点：`apps.h` 声明、条件 `installApp`、CMake 源过滤、Kconfig 和图标资产。其他 App 不包含 MCP 内部头文件。

## 3. 生命周期

### 3.1 打开

`onOpen()` 创建 Avatar、App-owned `LanService` 和 UI hooks。Wi-Fi 未连接时，通过 Board 的异步网络启动接口发起连接，不运行原 `Hal::startNetwork()` 的长阻塞等待；离线状态仍处理 Home。连接成功后由 `onRunning()` 启动 HTTPD。

被动 Avatar 只添加本 App 的 Breath/Blink/HeadPet modifier，并记录 ID。显示工具不直接跨线程调用 LVGL，而是投递给 Mooncake UI 上下文。

### 3.2 Home 与直接 closeApp

Home 回调只设置 `_closing` 并调用 `request_stop()`：

1. 停止接收新工具，唤醒 executor。
2. executor 取消排队响应，停止 HTTPD 和流；HTTPD 的停止等待发生在 executor，不在 Mooncake。
3. mic/speaker 协作退出并释放 Codec。
4. `stopped()` 同时满足 HTTPD 已停止、executor 已退出、两路音频任务已退出。
5. `onRunning()` 观察到停止后 `reclaim()`，再进入 Mooncake close；`onClose()` 只做有界 UI 清理。

直接 `closeApp()` 可能让 Mooncake 先进入 Sleeping，因此 `onSleeping()` 也检查停止完成并执行同样的延迟清理。重复打开时，上一代尚未停止就不重复初始化。

`uninstallAllApps()` 会直接析构 manager，并不逐个调用 `onClose()`。App 析构函数因此也执行停止和清理，避免进入 AI 后残留访问旧 App 的回调。这个最终桌面卸载路径会等待清理完成；不可取消的相机调用仍是第 9 节的明确边界。

如果 MCP 曾经打开且当前明确要进入 AI，析构还会通过公共 `StopStation()` 重建网络交接：vendored `StartNetwork()` 在 Station 已活跃时不会再次发 `Connected`，否则 Application 会停在 Starting。正常 Home 不断网；从未打开 MCP 的 fresh AI 不执行该重置。

### 3.3 关闭清理内容

- 按 ID 移除本 App 的三个 modifier，断开本 App 的 HeadPet 遥测连接。
- 取消 App-owned speech timer 和未执行 UI jobs。
- 清理提醒、Avatar、Home/Status UI。
- 回收 WithCaps 任务句柄和栈；任务本身只协作退出，不在运行中自删或强删。
- 清除保留的最近照片；已经发出的不可变快照由其持有者继续存活。

## 4. HTTP 与工具执行

单个 `esp_http_server` 提供：

- `GET /`：状态和资源指标；
- `POST /mcp`：无状态 JSON-RPC；
- `GET /photo.jpg`：最近照片；
- `WS /mic`、`WS /speaker`：音频流。

`tools/call` 使用 `httpd_req_async_handler_begin()` 创建独立请求所有权。HTTP handler 复制参数和 ID 后立即返回，工具在单个 `mcp_tool` worker 中串行执行。队列最多 4 个等待 job，另有 1 个活动 job；满载立即返回 503，不静默丢任务。

`AsyncToolJob` 由 `shared_ptr` 持有参数、ID、结果、async request 和完成状态。30 秒只限制 HTTP 响应等待，不声称取消底层工具。超时后 worker 仍可安全晚完成，但不会再次发送响应或提前释放参数。队列取消、超时、正常完成都只有一个响应完成方。

当前 lwIP 为 10 socket，HTTPD 自身需要 3 个，因此客户端上限保持 7。无状态 JSON/照片响应带 `Connection: close`。注册 `close_fn` 后，IDF 不再自动关闭 fd；回调必须同时释放音频会话并调用 `close(fd)`。

HTTP body 上限 32 KB，完整接收 deadline 为 10 秒。WebSocket 单帧上限 4 KB；完整 WS 帧总 deadline 尚未实现，不能用单次 socket timeout 冒充。

## 5. UI、提醒与照片

UI action 由 worker 投递，Mooncake 每轮最多消费 4 个。`UiPending` 使用共享所有权：

- 正常完成才返回成功；
- action 抛异常会被隔离并返回失败；
- 等待超时只取消本次等待，晚到的 action 不再执行；
- App 关闭时取消尚未执行的 action。

提醒 store 属于 App，创建、查询、停止、到期和清空都只在 UI 上下文执行，不与 worker 或 StackChan 共享可变 vector。

照片使用 `shared_ptr<const PhotoBytes>` 不可变快照，字节位于 PSRAM。下一次拍照不会释放 HTTP 正在发送的旧快照。照片 handler 持有自己的 snapshot，不在全局照片锁内做慢 socket send。

## 6. 音频与 socket 会话

- mic 和 speaker 都只在客户端 `start` 后懒创建任务；先构造公共 Codec，再申请大任务栈，避免栈先占用后导致 DMA 分配断言复位。内存不足时返回明确错误，服务继续运行。
- mic 使用 22 KB 内部栈，speaker 使用 16 KB；使用 WithCaps 字节单位，完成退出后由 owner 回收。20 ms/16 KB mic 候选已发生实测栈溢出并撤回，不能用缩帧代替内存验收。
- mic：24 kHz 交织输入 → 取 mic slot → 饱和数字增益 → 重采样 16 kHz → Opus 60 ms。
- speaker：有界 8 包队列，超限明确报告 backlog；解码到 Codec 输出速率。
- `codec_bus` 对共享输出时钟计数，最后一个退出者才能 `EnableOutput(false)`，避免 mic/speaker 互相关闭。
- mic 发送 job 带 FD + session/generation，旧会话事件不得影响复用 FD 的新会话。
- HTTPD socket-close 回调覆盖对端不再发送帧的异常断连；断连后立即开始协作清理。
- 旧任务尚未退出时，新 start 明确返回 `restarting`，客户端短暂重试；不启动第二份相同任务。

网络播放成功只说明数据路径工作，不等于实际放音验收；Opus 解码出非零 PCM 也不等于语音唤醒或听感验收。

## 7. AUDIO.TEST 与 AI 隔离

AUDIO.TEST 有独立目录、Kconfig 和 Codec 适配。FFT 表只在频谱模式懒分配，不再开机常驻。Home、直接 close 和最终析构都让音频任务协作退出，完成 input/output 清理后才回收任务，不在 1.5 秒后强删。

生产代码不为 MCP 修改 AI.AGENT、AudioService、AFE/VAD 或公共 HAL。AI 验收必须分别覆盖：

1. fresh boot → AI；
2. MCP 音频/关闭 → AI；
3. AUDIO.TEST 频谱/关闭 → AI。

检测任务日志和 PCM 指标只证明部分链路；唤醒词、触摸后 Listening、实际 mic 音质和 speaker 放音需要现场人员。

## 8. 自动化测试层次

- Host CTest：运动数学、提醒 store、UI 完成/异常/超时取消、注册表/分页/异常隔离。注册表测试使用 IDF 的真实 cJSON 和 host 日志 shim。
- `scripts/mcp_smoke.py`：全部 19 个默认工具、音量、背光、URL/内联照片和典型参数错误。
- `scripts/mcp_stream_test.py`：mic 有效包、第二客户端 busy、三次异常断连重连、speaker 背压、双路并行；捕获文件封装为 Ogg/Opus，可再用 FFmpeg 解码。
- 临时真机导航测试：连续开关、直接 close、卸载到 AI。测试入口必须在烧录后恢复，不能把自动导航留在生产 `main.cpp`。

## 9. 尚未关闭的边界

1. **音频内部 RAM 预算**：首次构造 Codec 后实测 free internal 约 25 KB、largest block 16 KB，22 KB mic 栈无法创建。20 ms/16 KB 候选已发生 core dump 确认的栈溢出并撤回；mic、speaker 和全双工不能记为已验收，需另行设计安全的内存预算。
2. **相机不可取消**：官方 `StreamCaptures()` 的 join、`VIDIOC_DQBUF` 和底层等待在 HAL/驱动内。严格 HAL 零修改时，不能保证无帧或编码卡死时取消工具；也不能保证这种情况下的最终 AI 卸载不被等待。
3. **共享 LVGL guard**：公共 guard 不能表达锁获取失败。未改共享 HAL 的约束下，P1-8 仍是开放项；不能宣称所有锁失败路径已修复。
4. **完整 WS frame deadline**、慢帧故障注入、OOM/任务创建失败/锁失败注入尚未全部实现。
5. **跨 camera 消费者**：MCP JPEG 快照安全不等于官方 camera frame 在多消费者间有共享所有权；Mooncake 前台互斥降低但不能形式化消除该风险。
6. token 错误/边界、慢客户端、资源计数和启动失败的专项测试需要独立记录结果。
7. 语音唤醒、触摸、实际听音、舵机安全及其他 App 的肉眼导航不能以日志或网络测试替代。

这些项目必须保持“阻塞/未验证”，不得写入“全部修复”或“全部通过”。
