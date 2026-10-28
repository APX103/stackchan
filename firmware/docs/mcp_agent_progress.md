# MCP Agent 实现与验证进度

- 日期：2026-10-28
- 分支：`feature/mcp-agent`
- 本文只记录当前工作区的证据，不沿用旧提交“全部通过”的结论。
- 未提交、未推送 Git。生产源码中的自动导航和 `PcmDiag` 均在测试后恢复。

## 1. 已落实的架构边界

- MCP 实现集中在 `main/apps/app_mcp_agent/`，分 UI/service/protocol/tools/audio/platform；不调用其他具体 App 的内部实现。
- `main/hal` 整棵树与线上 `main` 一致。`servo_heal.cpp/.h`、开机舵机修复、开机 codec warmup 和 MCP 私有 HAL 修改已删除。
- MCP 与 AUDIO.TEST 独立 Kconfig、独立源码过滤。未打开 App 时不创建其 HTTP、工具或音频任务。
- AUDIO.TEST FFT 改为进入频谱模式后懒分配；MCP 与 AUDIO.TEST 的任务正常退出均改为协作退出和 owner 回收。
- `main.cpp` 仅保留条件安装。每次临时导航/AI 测试后都恢复生产备份，并做文本/二进制比较。

## 2. 主要修复

1. 工具请求改为 IDF 原生异步 request；job 参数、ID、结果和响应具有共享所有权。队列满载明确返回 503，超时不再由 HTTP 和 worker 双方释放同一参数。
2. Home 使用 request/stopped/reclaim 分阶段关闭；直接 `closeApp()` 由 Sleeping 状态完成延迟清理。最终卸载到 AI 时，App 析构也执行清理。
3. UI action 在 Mooncake 上下文执行，支持完成、异常、超时取消和晚执行防护。`onOpen()` 的 IP hint 刷新纳入 LVGL 锁，消除了实测看门狗路径。
4. 提醒改为 App-local 单 UI 上下文 store；speech timer、modifier ID、HeadPet 回调和照片快照都有明确清理/所有权。
5. HTTPD `close_fn` 同时释放音频会话并调用实际 `close(fd)`。这修复了连续请求后 `httpd_accept_conn: error in accept (23)` 的 fd 泄漏。
6. mic/speaker 先构造公共 Codec，再申请任务栈。避免先占大栈后导致 Codec DMA 断言复位；分配失败现在返回错误，服务保持可用。
7. MCP 曾经打开且进入 AI 时，通过公共 `StopStation()` 完成网络交接，避免 AI 因已活跃 Station 不再发 `Connected` 而停在 Starting。正常 Home 不断网，fresh AI 不受影响。
8. `set_volume` 纳入 codec 控制锁；mic 发送带 FD + session，socket-close 覆盖无后续帧的异常断连。

## 3. 已通过的检查

| 检查 | 结果与证据 |
|---|---|
| Host CTest | 4/4：运动数学、提醒 store、UI 完成/异常/超时取消、注册表/分页/异常隔离。使用真实 IDF cJSON。 |
| 扩展设备 smoke | 28 项通过：19 个默认工具、音量、背光、显示、提醒、URL 照片、内联照片及典型错误。 |
| 单次关闭重进 | 串口有停止/关闭/重开，后续扩展 smoke 全通过；该轮无看门狗或复位。 |
| fresh boot → AI | 检测任务启动；42 个 PCM 窗口，mic 峰值 14,156。 |
| MCP 关闭 → AI | Station 交接修复后检测任务启动；33 个 PCM 窗口，mic 峰值 22,420。 |
| AUDIO.TEST 频谱 → AI | input/output 先关闭再 `on close`；AI 检测任务启动，73 个 PCM 窗口，mic 峰值 16,930。 |
| mic 分配失败保护 | 返回 `no memory for capture task`；GET `/` 继续响应，uptime 不复位。 |
| 最终生产构建与烧录 | ESP-IDF 5.5.5 构建、full flash（含 assets）通过。硬复位后正常进入 Launcher；MCP/AUDIO.TEST 只有 `on create`，没有自动 `on open`。 |

## 4. 音频内存阻塞（不能写成音频通过）

逐组内存探针定位到第一次音量/Codec 构造，而非拍照：

- smoke 前：free internal 44,143，largest block 31,744；
- smoke 后：free internal 25,103，largest block 16,384；
- 拍照和 embed 后几乎没有继续下降。

结果：

1. 22 KB mic 栈无法在 Codec 构造后分配，服务返回明确错误。
2. mic-first 曾先占 22 KB 栈，再因 Codec DMA 分配失败复位；调整构造顺序后，该复位路径已变成可返回的错误，但活动 mic 功能仍未验证。
3. 20 ms/16 KB 候选在首包后复位，core dump 明确为 `mic_ws` stack overflow；已撤回并恢复 60 ms/22 KB。speaker 保持 16 KB。
4. mic 有效流、全双工、完整异常断连测试及活动 mic 后 AI，均不能记为通过。单路 speaker 音阶播放的设备输出验证见第 10 节；实际听感仍需现场确认。

下一步必须做安全的任务/Codec 内存预算设计。不能继续盲目缩栈、使用 PSRAM 栈，或未经评估修改全局内存配置。

## 5. 10 轮导航压力测试

**短周期通过**：同一固件完成 10 次 open、10 次 close 和 `done`，期间执行状态轮询；28 个 HTTP 样本，free SRAM 41,803..44,039，largest block 保持 31,744。无看门狗、异常复位或 `accept(23)`。完整 smoke 已在长保持窗口单独通过。本轮不运行音频流。

**长稳未验收**：长周期版本曾在第 5 轮附近重新启动，但保存的 core 与该镜像 SHA 不匹配，无法诚实归因；短周期通过不能替代长时间运行验证。

## 6. 其他未关闭项

1. **相机取消**：官方 `StreamCaptures()` 的 join、`VIDIOC_DQBUF` 和底层等待在 HAL/驱动内。严格 HAL 零修改时，无法保证无帧/编码卡死时取消工具或最终 AI 卸载不等待。URL/embed 正常拍照通过不等于故障路径修复。
2. **共享 LVGL guard**：公共 guard 不能表达锁获取失败；未改共享 HAL，锁失败路径仍未关闭。
3. **长任务与故障注入**：超时后晚完成/取消缺少可控长工具真机测试；OOM、任务创建失败、锁失败、无帧和慢客户端尚未全部覆盖。
4. **协议边界**：完整 WS frame 总 deadline 未实现；慢 frame、慢 body、发送背压和队列并发需要专项结果。
5. **鉴权**：token 长度、错误 token、超长 header、WS query 位置尚未做启用配置的专项测试。
6. **资源基线**：连续开关后的任务数、FD、timer、modifier 和 heap 还需要完整计数；单看服务可访问不够。
7. **现场验收**：唤醒词、触摸进入 Listening、实际 mic 音质、speaker 放音、舵机安全、Avatar/Setup/导航肉眼回归均未完成。

## 7. 测试产物

临时日志、core dump、Ogg/WAV 和生产备份位于本机临时 `stackchan-vad` 目录，不属于生产源码。可重复脚本：

- `firmware/scripts/mcp_smoke.py`
- `firmware/scripts/mcp_stream_test.py`（当前受音频内存阻塞，不能作为通过项）
- `firmware/tests/`（host CTest）

详细架构见 `mcp_agent_design.md`；原始缺陷逐项状态见 `mcp_agent_static_review.md`；AI 运行时复核见 `ai_agent_vad_static_review.md` 第 11 节。

## 8. 用户要求的舵机演示

在 MCP.AGENT 打开后，执行了 8 组动作：居中、左右 yaw ±35°、pitch 25°/45°/65°、左右斜向 yaw ±30°，速度 300–500，最后 `go_home`。控制器动画角度均在 5°量化容差内，最终 yaw=-1、pitch=3；串口无看门狗、复位或舵机错误日志。首次 3°阈值过严导致脚本停止，已回中后以 5°容差重跑完整序列通过。

该结果证明 MCP 工具调用和控制器运动动画正常，**不等于远程听到了实际舵机声音或验证了物理角度**；声音与机械状态仍需现场人员确认。演示后已恢复正式 Launcher 固件。

## 9. 舵机无声音的根因与总线复测

用户现场确认初轮演示完全无声。复查发现，迁移到独立 App 时漏掉了已有的 `motion.setTorqueEnabled(true)` 修复：舵机释放扭矩时会忽略位置写入，但 HTTP 和动画仍能返回成功。因此初轮结果不能作为物理运动验收。

已在 App 的 `set_head_angles`、`look_at`、`go_home` 三条路径补回公共扭矩使能 API，未修改 HAL。复测遥测显示：初始 yaw/pitch torque=0；动作后 torque=1、moving=1，位置随各组动作变化，没有 `invalid current pos` 回退，也无看门狗或复位。该证据来自舵机总线状态，不是 HTTP 200 或动画值。临时 `ServoDiag` 已移除，正式 Launcher 已恢复。

实际声音是否满足用户听感仍由现场人员判断；文档不把总线成功写成“我听到了”。

## 10. 单路 speaker 音阶播放验证

通过 MCP `self.audio_speaker.set_volume` 设置 50% 音量，使用 `/speaker` 发送电脑合成的 C5、D5、E5、F5、G5（哆来咪发嗦）16 kHz 单声道、60 ms Opus 包，共 36 包、约 2.16 秒。

设备日志确认：`Set output volume to 50`、`playback task ready (opus -> 24000 Hz)`、`first opus packet decoded and played`、`playback task stopped (36 packets played)`，随后 `Set output enable to false`。speaker 栈余量 7,952 字节，停止后 `spk_active=false`。

这证明 MCP speaker 的 WebSocket、Opus 解码和 Codec 输出路径可用，不是单纯网络发送成功。**实际音阶听感仍由设备旁用户判断，不冒充远程听音。**测试后已恢复正式 Launcher，音量保留 50%。mic 采集及全双工仍受第 4 节内存问题限制。
