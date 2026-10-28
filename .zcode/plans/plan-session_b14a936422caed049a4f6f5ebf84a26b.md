# MCP Agent 独立化、AI 麦克风回归与完整验证计划

## 先说明：目前仍不知道什么

以下信息没有从用户回答或运行时证据中取得，不能当作已知事实：

1. AI 麦克风故障是 fresh boot 直接 AI 就发生，还是使用 MCP.AGENT / AUDIO.TEST 后才发生。
2. 同一块硬件刷官方 `main` 后，唤醒词和触摸后的语音输入是否正常。
3. 当前可用 COM 口、设备连接状态、Wi-Fi/AI 服务是否可用，以及最后是否有人能现场说话、触摸和确认声音。
4. 相机功能与“整个 `firmware/main/hal` 必须和 `main` 完全一致”的冲突如何处理。官方相机无限等待和帧所有权问题就在 HAL 内，严格不改 HAL 时，不能诚实承诺把相机工具的所有审查项修完。

因此不会直接开始生产代码修改，更不会预先承诺“全部测试已通过”。第一阶段必须先取得基线证据；没有设备或必要现场验收时，会明确停在哪个验证门槛。

## 已确认的约束和实施边界

- 以本次要求为准，覆盖旧设计文档中允许修改 HAL、把 MCP 放在 `hal/` 的内容。
- `firmware/main/hal` 整棵树最终与 `main` 内容一致：既有文件手工恢复，新增 MCP 文件移出，**不使用 `git revert`、`git checkout` 或提交历史操作**。
- 删除 `servo_heal.cpp/.h`、开机调用和 EPROM 修复逻辑；运动工具只走官方公开 motion/HAL API。
- 保留当前未提交的 `mcp_tools.cpp` 修正（删除失效的 servo debug 依赖），在迁移时一并保留，不覆盖别人的工作。
- MCP 的实现集中到自己的 App 目录；不调用 `app_ai_agent` 或其他具体 App 的内部实现，不依赖 `apps/common` 中的共享可变服务。外部只保留必要的注册、构建、Kconfig 和资产集成。
- 没有证据前不调整 codec 增益、采样率、AFE 配置或任务栈，避免把“听不见”误当成“调大音量就能修”。

## 阶段 0：先诊断，暂不改生产代码

1. 记录当前 HEAD、未提交修改、实际 `sdkconfig/.local`、工具链和设备信息。当前生成的 ELF/bin 已不能作为验收依据，后续必须 clean rebuild，不能刷旧二进制。
2. 用同一 IDF 5.5.5 分别构建官方 `main` 基线和当前分支，排除工具链变量；使用 full flash，确保 assets 和唤醒词资源一起写入。
3. 从复位开始采集串口日志，并启用项目已有的 AI UDP PCM tap。该 tap 位于 AI 重采样之后、wake/AFE 之前，正好能区分公共输入与下游问题，无需修改共享 HAL。
4. 对两种固件、相同语音分别记录三组场景：
   - fresh boot → 直接 AI：唤醒词 + 触摸后说话；
   - fresh boot → MCP 音频 → 退出 → AI；
   - fresh boot → AUDIO.TEST → 退出 → AI。
5. 保存并分析：codec read 错误、input/output 状态、resampler 初始化、音频/AFE 任务、UDP 包长和节奏、每声道 peak/RMS、零/恒定样本比例，以及 AI 是否进入 Listening。
6. 只有证据明确后才选择修改点：
   - AI PCM 无效：继续定位公共输入、时钟、重采样或任务；
   - AI PCM 有效：停止猜 codec，转向 wake/AFE/Opus/服务链路；
   - fresh AI 正常而新 App 后失败：重点处理新 App 的共享状态；
   - 官方 main 也失败：先报告基线问题；若必须修改被冻结的 HAL/公共 AI 代码才能修，停止该部分并重新确认边界。

## 阶段 1：恢复 HAL 并建立真正的 App 边界

诊断门槛完成后才修改代码：

1. 手工恢复 `hal_bridge.cc`、`SCSCL.h` 等所有 main-tracked HAL 差异；删除 `servo_heal`。
2. 把 `hal/mcp_lan`、`mic_ws_stream`、`speaker_ws_stream`、`ws_util` 等 MCP 实现迁入 `apps/app_mcp_agent/`，按 UI、service、protocol、tools、audio、platform 分模块，保持命名和依赖方向清晰。
3. 删除 `main.cpp` 的 servo heal 和 codec warmup 开机副作用；`main.cpp` 只保留 App 安装。MCP 与 AUDIO.TEST 使用各自独立配置，不再由 MCP 开关顺带安装 AUDIO.TEST。
4. 移除为 AUDIO.TEST 添加的全局 LVGL 16 ms 覆盖；FFT 表改为 App 所有、按需初始化。AUDIO.TEST 自己完成对称音频清理，不依赖 MCP 代码。
5. 更新设计文档，使文件布局和生命周期说明符合新边界。
6. 每次此阶段构建前后都验证：
   - `git diff --exit-code main -- firmware/main/hal`
   - HAL 下没有未跟踪新增文件
   - 其他具体 App 和 vendored Xiaozhi 没有被 MCP 反向依赖。

## 阶段 2：重做 MCP 生命周期与并发所有权

1. `AppMcpAgent` 持有明确的服务实例，不再由多个全局静态对象分别拥有 executor、photo、timer 和 session。
2. 使用 IDF 已有 `httpd_req_async_handler_begin/complete`：
   - HTTP handler 快速交付出工具请求；
   - 每个请求独立拥有参数、结果和 async request；
   - 响应只允许完成一次；
   - 超时只结束响应等待，不提前释放仍在执行的参数；
   - 队列有界，满载显式拒绝，不静默 drop；
   - HTTP body/frame 有完整消息 deadline。
3. 工具执行保持可读的单 worker 串行模型，UI/avatar/motion 操作通过 App 自有 UI 命令队列在 Mooncake 上下文执行，避免跨线程直接操作 LVGL。
4. Home 只发起异步 closing；`onRunning` 轮询完成后再 `close()`。`onClose()` 只做有界 UI 清理，不执行 `httpd_stop` 或等待硬件。
5. 所有任务协作退出，不把强删作为正常回收方式。每个启动失败路径复用统一清理逻辑。
6. App 内处理：提醒单任务所有权、immutable photo snapshot、timer 取消、modifier ID 逐个移除、token 长度安全比较、WS query 解析、session/generation 身份、异常断连释放、speaker/mic 对称 codec 清理。
7. 相机子项暂不擅自选择例外：严格 HAL 零改动时，现有官方 camera API 无法保证取消和帧生命周期。该子项在用户未确定边界前保持为明确阻塞项，不能计入“全部修完”。

## 阶段 3：按证据修复 AI 麦克风并隔离其他 App

1. 若证据指向 MCP 开机 warmup、全局配置或新 App 残留，通过阶段 1/2 的隔离和对称清理消除，并重复阶段 0 的相同 PCM/语音对照。
2. 若证据指向 AI 主链路，只修改证据对应的层级；不借 MCP 之名改 AI App，也不调无关 VAD 参数。
3. 如果根因必须修改官方 HAL 或 vendored AI 公共实现，而当前约束不允许，保留诊断结果并停止该部分，不用 App 内 hack 掩盖。
4. 最终验证 AI 的两种输入：本地“Hi StackChan”唤醒，以及触摸进入 Listening 后的语音；二者都必须验证，不能用其中一个替代另一个。

## 阶段 4：测试与验收

### 自动化

- 扩展 host CTest：executor 超时/晚完成/取消/背压、JSON-RPC、registry、token/query、photo 所有权、reminder 并发、session 生命周期和启动失败清理。
- 测试钩子只存在于 App/test 范围且默认关闭，用于长任务、无帧、锁失败、OOM、task-create 失败和 codec 交错；不把测试逻辑塞进公共 HAL。
- 扩展设备 LAN 测试：全部默认工具及参数边界、`embed:true`、慢客户端、并发、异常 WS 断开、重连、认证和资源计数。
- mic 必须收到非零且可解码的有效音频；speaker 的网络发送成功只记为网络通过，不冒充实际放音通过。

### 真机回归

- MCP：进入、Home、慢调用中 Home、离线返回、连续开关、长消息退出、并发提醒、拍照/下载、音频异常断开和重复重连。
- AI：fresh 直接 AI、MCP 后 AI、AUDIO.TEST 后 AI；比较串口和 PCM，再验证唤醒与触摸语音。
- 其他 App：至少验证 launcher、AI.AGENT、Avatar/camera、Setup 音频和导航无回归。
- 舵机测试需要设备稳定、头部无阻挡并有人监督；语音唤醒、实际 mic 音质和 speaker 放音需要现场人员，缺失时只能标记“未验收”，不能写“全部通过”。

## 最终交付与完成定义

- MCP 代码集中在独立 App 目录，删除后只留下最小注册/构建集成，重新加入即可构建；无开机音频/舵机副作用。
- `firmware/main/hal` 与 `main` 内容一致，无 `servo_heal`，未使用 Git 历史回滚。
- 两份审查文档中的每个项目标记为已修复、已验证、阻塞或不适用，并附证据；设计/进度文档同步更新。
- 所有已实现的 host/device 测试通过，AI 和其他 App 完成回归。相机边界、硬件或现场语音条件未解决时，明确报告未完成项，绝不把静态检查或部分测试写成“全部通过”。
- 不提交、不推送 Git。