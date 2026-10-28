# MCP Agent 静态代码审查与修复交接报告

- 日期：2026-10-28
- 分支：`feature/mcp-agent`
- 审查开始基线：`90805c8`
- 报告写入前复核版本：`67d34ebf2e434eacd712ce24db6222138b2cec22`
- 版本差异：`90805c8..67d34eb` 只增加舵机模式初始化和 MCP.AGENT 摸头事件观测，没有修复本报告列出的缺陷。除特别说明外，本文行号按 `67d34eb`。
- 审查方式：仅静态阅读源码、调用链、构建测试配置和既有测试脚本；未编译、未烧录、未连接设备、未运行 smoke、未做运行时调试。
- 文档目的：把审查结论交给后续实现 Agent。后续修改前仍应对照当时的 HEAD 复核代码和行号，不要机械套用本文。

> 本文中的“确定缺陷”是指代码中的错误所有权、阻塞、同步或生命周期路径可以确定存在；不表示已经通过运行时证据证明用户现场每一次卡死都由该项触发。

## 1. 结论摘要

MCP Agent 存在多条足以解释“调用某个 API 后 MCP 服务卡住、页面无法返回，甚至整机像死机一样”的代码路径：

1. 工具超时后的参数所有权错误会造成 use-after-free / double-free，并伴随信号量泄漏。
2. HTTP 工具调用虽然是独立任务执行，HTTP handler 仍同步等待最多 30 秒；按 Home 后又会在 Mooncake 主任务里同步等待 HTTP 服务停止。
3. 工具任务和音频任务退出超时后会被强制删除；如果任务持有 LVGL 或其他锁，可能把锁永久遗留给已不存在的任务。
4. 相机路径存在无超时的线程 join 和 `portMAX_DELAY` 视频帧等待，能够永久占用唯一的工具执行任务。
5. 页面退出后仍有 LVGL 定时器、modifier 和共享回调可能访问已经销毁的 avatar。
6. 提醒、照片、音频、socket 会话等共享状态还有多组并发与生命周期问题。

用户的方向“这是阻塞/并发问题”是对的，但“框架没有使用 FreeRTOS，需要引入 FreeRTOS”不符合当前代码事实。当前已经有：

- `mcp_tool` 工具执行任务：`firmware/main/hal/mcp_lan/lan_service.cpp:322-335`
- HTTP server 自己的任务：`firmware/main/hal/mcp_lan/lan_service.cpp:337-343`
- 麦克风任务：`firmware/main/hal/mic_ws_stream.cpp:401-427`
- 扬声器任务：`firmware/main/hal/speaker_ws_stream.cpp:268-289`
- Mooncake / StackChan / LVGL 等既有任务与锁。

核心问题不是任务数量不足，而是：任务间同步等待、不可取消的驱动调用、超时后的对象所有权、强制任务退出、共享状态保护和页面生命周期没有形成一致设计。简单再包一层 `xTaskCreate` 或把 30 秒改成更久，不能修复这些问题。

## 2. 当前执行模型与卡死调用链

### 2.1 工具调用模型

`tools/call` 的路径是：

1. HTTP handler 解析 JSON 并复制参数：`firmware/main/hal/mcp_lan/mcp_jsonrpc.cpp:167-191`
2. `service_execute()` 把 job 放入队列：`firmware/main/hal/mcp_lan/lan_service.cpp:177-204`
3. 唯一的 `mcp_tool` 任务串行执行 job：`firmware/main/hal/mcp_lan/lan_service.cpp:79-103`
4. HTTP handler 同时阻塞在二进制信号量上，最多等待 30 秒：`firmware/main/hal/mcp_lan/lan_service.cpp:206`、`firmware/main/hal/mcp_lan/mcp_jsonrpc.cpp:24-27`

所以“工具不在 HTTP 任务上直接运行”是真的，但“HTTP 请求是非阻塞的”是假的。等待期间，同一个 HTTP server 无法继续正常处理状态、照片、WebSocket handler 和排队到 HTTP 任务上的发送工作。

### 2.2 Home 返回模型

1. Home 事件在 App running 更新中轮询：`firmware/main/apps/app_mcp_agent/app_mcp_agent.cpp:124-132`
2. Home 回调调用 `close()`：`firmware/main/apps/app_mcp_agent/app_mcp_agent.cpp:76-78`
3. Mooncake 后续在 App 状态更新中执行 `onClose()`；App 的 `onClose()` 直接调用 `service_stop()`：`firmware/main/apps/app_mcp_agent/app_mcp_agent.cpp:138-145`
4. `service_stop()` 先停音频和 HTTP，再处理工具任务：`firmware/main/hal/mcp_lan/lan_service.cpp:375-403`
5. 当前 IDF 5.5.5 的 `httpd_stop()` 会无超时等待 HTTP 线程进入 `THREAD_STOPPED`：`D:/Espressif/frameworks/esp-idf-v5.5.5/components/esp_http_server/src/httpd_main.c:556-585`

因此，如果 HTTP 线程仍在等待工具结果，按 Home 的 Mooncake 主任务也会被拖住。这不是单纯的“网络服务无响应”，而是页面退出路径本身包含阻塞。

### 2.3 超时不能取消底层工作

`service_execute()` 的接口注释已经明确说明：超时返回后，body 仍可能继续执行。

- 合同：`firmware/main/hal/mcp_lan/lan_service.h:43-45`
- 实现：`firmware/main/hal/mcp_lan/lan_service.cpp:177-213`

这意味着 30 秒只是 HTTP 等待上限，不是工具执行上限，更不是底层硬件操作的取消机制。后续设计必须保持这个区别清晰，不能把“HTTP 已超时”当作“job 从未开始”或“job 已取消”。

## 3. 确定缺陷：最高优先级

### P0-1：工具超时导致参数 use-after-free / double-free，并泄漏信号量

**证据**

- `args_copy` 是裸 `cJSON*`，被捕获进执行任务 lambda；任务结束时调用 `cJSON_Delete(args_copy)`：`firmware/main/hal/mcp_lan/mcp_jsonrpc.cpp:182-191`
- `service_execute()` 在信号量等待超时时返回 `false`，但不会取消已排队或已运行的 job：`firmware/main/hal/mcp_lan/lan_service.cpp:177-213`
- 调用方看到任意 `false` 都立即再次删除 `args_copy`，注释却假设“executor never picked it up”：`firmware/main/hal/mcp_lan/mcp_jsonrpc.cpp:192-194`

**触发与后果**

任一工具运行超过 30 秒，或者因为前一个工具卡住而排队超过 30 秒，都可能触发。HTTP 侧释放后，执行任务仍可能读取该 JSON；之后还会再次释放。结果包括堆损坏、断言、异常重启或看似随机的死机。

每次超时还会有意不删除二进制信号量：`firmware/main/hal/mcp_lan/lan_service.cpp:206-212`。重复超时会持续泄漏资源。

**修复要求**

- job 的参数、结果、完成通知必须有唯一、明确的共享所有权；HTTP 等待方超时后不得再释放仍归 job 使用的对象。
- 区分“未入队”“已取消”“已排队”“执行中”“已完成”“等待超时”，不能用一个 `false` 覆盖所有状态。
- 完成通知的销毁也必须由明确的一方负责，不能靠“每次超时泄漏一点”维持安全。
- 增加可控的长任务测试，覆盖超时后晚完成、超时后取消、退出应用时仍在执行等情况。

## 4. 确定缺陷：阻塞与退出

### P1-1：HTTP handler 同步等待工具，Home 又同步等待 HTTP 停止

**证据**

- HTTP 等待工具信号量：`firmware/main/hal/mcp_lan/lan_service.cpp:177-213`
- 工具超时固定为 30 秒：`firmware/main/hal/mcp_lan/mcp_jsonrpc.cpp:24-27`
- App 在 `onClose()` 同步停服务：`firmware/main/apps/app_mcp_agent/app_mcp_agent.cpp:138-145`
- `service_stop()` 先执行 `httpd_stop()`，后通知工具任务停止：`firmware/main/hal/mcp_lan/lan_service.cpp:375-403`
- IDF 停止 HTTP 时无超时等待：`D:/Espressif/frameworks/esp-idf-v5.5.5/components/esp_http_server/src/httpd_main.c:556-585`

**影响**

- 一个正常但较慢的工具会阻塞同 HTTP server 的其他工作。
- 工具卡住时，后续请求不断经历 30 秒等待并触发 P0-1。
- API 执行中按 Home，页面返回会被同一条等待链拖住。

**修复要求**

- Mooncake 主更新任务不得执行无界或秒级的同步服务拆除。
- HTTP 请求模型与工具执行模型需要明确的异步完成、取消、背压和关闭协议；不能仅靠增加队列长度。
- 关闭时先阻止新工作，再让活动工作按可取消协议退出，最后回收资源。
- 不建议只把 `httpd_stop()` 与 `ex.running = false` 调换顺序就宣布完成；底层工具若仍永久阻塞，仅换顺序不能解决。

### P1-2：工具和音频任务被强制删除，可能永久遗留锁和驱动状态

**证据**

- 工具任务最多等 15 × 100 ms，随后无论 `ex.exited` 是否为真都删除：`firmware/main/hal/mcp_lan/lan_service.cpp:390-403`
- 工具实际执行点：`firmware/main/hal/mcp_lan/lan_service.cpp:79-103`
- 多个工具通过 RAII 持有 `LvglLockGuard`，例如：`firmware/main/hal/mcp_lan/mcp_tools.cpp:262-270`、`firmware/main/hal/mcp_lan/mcp_tools.cpp:350-362`
- 锁只在析构中释放：`firmware/main/hal/hal.h:326-336`
- 麦克风强删路径：`firmware/main/hal/mic_ws_stream.cpp:430-447`
- 扬声器强删路径：`firmware/main/hal/speaker_ws_stream.cpp:292-310`

**触发与后果**

服务关闭时，如果任务仍在执行超过 1.5 秒，会在任意 C++ 语句中被删除。FreeRTOS 不会为强删任务展开 C++ 栈。若任务当时持有 LVGL、codec 或其他互斥锁，析构不会运行，后续任务可能永久等待该锁；硬件和驱动状态也可能停留在半完成状态。

**修复要求**

- 不要把强删忙碌任务当作正常退出机制。
- 所有可阻塞操作需要可取消或至少有明确的有限等待与错误退出路径。
- 退出协议必须保证锁、内存、codec、camera、socket 和任务句柄都能回收。
- 如果保留最后的强制兜底，必须明确证明当时不可能持锁或访问随后被销毁的对象；当前代码没有这种证明。

### P1-3：相机工具存在真正无界的 join 和视频帧等待

**证据**

- MCP 工具进入同步拍照：`firmware/main/hal/mcp_lan/mcp_tools.cpp:445-455`
- `mcp_photo::capture()` 调用 `StreamCaptures()` 并同步编码 JPEG：`firmware/main/hal/mcp_lan/mcp_photo.cpp:53-103`
- `StreamCaptures()` 对既有编码线程执行无超时 `join()`：`firmware/main/hal/board/stackchan_camera.cc:854-858`
- 随后执行 `VIDIOC_DQBUF`：`firmware/main/hal/board/stackchan_camera.cc:864-870`
- 当前 video 组件把 DQBUF 等待设为 `portMAX_DELAY`：`firmware/managed_components/espressif__esp_video/src/esp_video_ioctl.c:175-192`
- 既有编码线程还可能在队列满时无限等待：`firmware/main/hal/board/stackchan_camera.cc:1041-1068`

**触发与后果**

相机不出帧、视频 buffer 通知异常或前一个编码线程卡住时，唯一的 `mcp_tool` 可以永久阻塞。之后所有工具都串行排在它后面。HTTP 超时只释放 HTTP 等待方，不会停止相机操作，并会进一步触发 P0-1。

**修复要求**

- 为 camera 的取帧、编码退出和队列等待设计有限时间、取消和错误传播。
- 不能通过强删 `mcp_tool` 来“解决”底层永久等待。
- 需要故障注入测试：无帧、编码线程不退出、连续拍照、拍照时退出 App、拍照时另一个 camera 消费者运行。

### P1-4：进入页面时网络启动也可阻塞 Home 最长约 65 秒

**证据**

- `onOpen()` 在 Mooncake 状态进入正常运行前同步调用 `startNetwork()`：`firmware/main/apps/app_mcp_agent/app_mcp_agent.cpp:89-104`
- 网络启动包含长等待和配网重启路径：`firmware/main/hal/hal_network.cpp:46-126`
- Home 轮询位于 `onRunning()`，网络等待期间不会正常执行：`firmware/main/apps/app_mcp_agent/app_mcp_agent.cpp:117-132`

**影响**

这是“刚进页面就无法返回”的另一条独立路径，不需要先调用 API。它与服务关闭阻塞是不同问题，应分别验收。

**修复要求**

网络连接和加载状态应可取消，不能长期占用 Mooncake 主更新任务；离线和等待期间仍应处理 Home。

## 5. 确定缺陷：对象生命周期与共享数据

### P1-5：`show_message` 定时器在页面退出后访问已销毁 avatar

**证据**

- 全局 `g_speech_timer` 和无空指针检查的 callback：`firmware/main/hal/mcp_lan/mcp_tools.cpp:274-281`
- `show_message` 接受最长 600000 ms，并创建单次 LVGL 定时器：`firmware/main/hal/mcp_lan/mcp_tools.cpp:283-310`
- `onClose()` 没有取消该定时器，而是停止服务后销毁 avatar：`firmware/main/apps/app_mcp_agent/app_mcp_agent.cpp:138-157`
- `StackChan::avatar()` 直接解引用 `_avatar`：`firmware/main/stackchan/stackchan.h:69-77`

**触发与后果**

调用 `self.display.show_message` 后，在定时器到期前退出 MCP.AGENT。定时器之后仍会运行，可能空指针崩溃；若另一个 avatar 页面已经打开，旧 callback 还可能修改新页面的 avatar。

**修复要求**

- App 退出必须取消属于本 App 的定时器；callback 也必须有有效的生命周期保护。
- 处理“旧页面定时器晚到”与“新页面已经复用全局 avatar”两种情况。
- 验收不能只等默认 10 秒，应使用短的可控测试时长重复进入/退出。

### P1-6：提醒管理器被多任务无锁读写

**证据**

- `_reminders` 的创建、删除、遍历、擦除和复制都没有互斥：`firmware/main/apps/common/reminder/reminder.cpp:35-83`
- MCP 工具在 `mcp_tool` 上创建、查询和停止提醒：`firmware/main/hal/mcp_lan/mcp_tools.cpp:404-440`
- MCP.AGENT 的 `onRunning()` 周期调用更新：`firmware/main/apps/app_mcp_agent/app_mcp_agent.cpp:117-123`
- StackChan 更新任务也每 20 ms 调用更新：`firmware/main/hal/hal.cpp:145-154`

**触发与后果**

API 与到期更新并发时，会发生 C++ 数据竞争；`push_back` 引起扩容或 `erase` 时，另一个任务正在使用的迭代器、元素和 `std::string` 可能失效。超过最初 reserve 的 5 个提醒会更容易触发扩容，但少于 5 个也并不构成线程安全。

**修复要求**

- 选择明确的单任务所有权，或给所有访问建立同一同步协议；不能只保护其中一两个入口。
- 若在回调中允许再次增删提醒，需要特别检查重入和死锁，不要简单拿着互斥锁 emit。
- 这是共享代码，修改时必须核对 AI.AGENT 和其他现有提醒用户的行为。

### P1-7：`GET /photo.jpg` 使用已解锁、可能被下一次拍照释放的裸指针

**证据**

- `mcp_photo::get()` 只在取指针和长度时短暂持有 mutex：`firmware/main/hal/mcp_lan/mcp_photo.cpp:106-116`
- HTTP handler 随后在未持锁的情况下执行可能阻塞的发送：`firmware/main/hal/mcp_lan/lan_service.cpp:160-172`
- 下一次成功拍照会在锁内释放并替换旧 buffer：`firmware/main/hal/mcp_lan/mcp_photo.cpp:95-102`
- 头文件也明确说明指针只保留下一次 capture 前有效：`firmware/main/hal/mcp_lan/mcp_photo.h:19-21`

**触发与后果**

照片发送与后续拍照完成并发时，`httpd_resp_send()` 可能继续读取已释放 PSRAM，造成图片损坏、内存异常或堆相关故障。

**修复要求**

使用有所有权的不可变快照或等价生命周期方案，使发送期间 buffer 保证存活。不要简单改为“拿着全局照片锁做慢 socket send”，否则会把相机和 HTTP 阻塞进一步耦合。

### P1-8：LVGL 锁获取失败后仍继续访问并无条件解锁

**证据**

- `LvglLockGuard` 构造函数不保存获取结果，析构总是 unlock：`firmware/main/hal/hal.h:326-336`
- 具体显示实现等待 30000 ms，失败只打印日志：`firmware/main/hal/board/stackchan_display.cc:288-297`
- 底层锁 API 会返回失败，但当前包装层把该状态丢掉。

**触发与后果**

LVGL 锁被占用至少 30 秒时，MCP 工具会继续在未持锁状态下访问 avatar、motion 或其他受保护对象，最后还会尝试释放自己未获得的锁。它与 30 秒 MCP 超时叠加，会进一步扩大竞态和 P0-1 的触发窗口。

**修复要求**

- 锁 guard 必须能表示“未获得锁”，未获得时不得执行受保护操作，也不得 unlock。
- 这是共享 guard，不能只改一个 MCP 调用点来掩盖问题；修改共享实现前要审计所有既有用户和返回策略。

## 6. 其他确定问题

### P2-1：modifier 随重复打开累积，HeadPet 可在 avatar 销毁后运行

- 每次 `onOpen()` 添加 Breath、Blink、HeadPet 三个 modifier：`firmware/main/apps/app_mcp_agent/app_mcp_agent.cpp:60-68`
- `onClose()` 只 `resetAvatar()`，没有移除这些 modifier：`firmware/main/apps/app_mcp_agent/app_mcp_agent.cpp:138-157`
- `resetAvatar()` 只重置 `_avatar`，与 `clearModifiers()` 是不同操作：`firmware/main/stackchan/stackchan.h:45-57`、`firmware/main/stackchan/stackchan.h:109-124`
- HeadPet 的 swipe/restore 会直接调用 `stackchan.avatar()`：`firmware/main/stackchan/modifiers/head_pet.h:69-77`、`firmware/main/stackchan/modifiers/head_pet.h:97-104`

后果是重复进入退出后 modifier 数量、CPU 和内存占用增加；退出后仍有摸头事件或延迟恢复时，可能访问空 avatar。当前版本额外添加的 `_pet_conn` 遥测回调断开，不等于移除 HeadPetModifier 自己注册的信号连接。

修复时建议明确每个 modifier 的 App 所有权并按 ID 清理，避免不加区分地清掉其他 App 仍需要的 modifier。

### P2-2：Speaker 异常断连留下永久 client claim；Mic 部分初始化失败也有类似风险

- Speaker 接收/传输失败直接返回，没有清除 `client_fd_`：`firmware/main/hal/speaker_ws_stream.cpp:150-159`
- 只有显式 stop、hello 失败、任务创建失败或整个服务 Stop 等路径会释放：`firmware/main/hal/speaker_ws_stream.cpp:190-228`、`firmware/main/hal/speaker_ws_stream.cpp:292-299`
- 播放任务空闲退出依赖 `client_fd_ < 0`：`firmware/main/hal/speaker_ws_stream.cpp:63-76`
- Mic 的部分早期初始化失败路径也可能保留 claim：`firmware/main/hal/mic_ws_stream.cpp:167-172`、`firmware/main/hal/mic_ws_stream.cpp:201-205`、`firmware/main/hal/mic_ws_stream.cpp:223-230`

客户端发送 start 后直接断电、断网或关闭 socket，后续客户端可能持续收到 `busy`，播放任务也不能按无客户端逻辑退出。应补充 socket close/session 生命周期处理，并覆盖 FD 复用情况。

### P2-3：HTTP server 启动失败会泄漏工具任务和内部 RAM 栈

- 工具任务先创建：`firmware/main/hal/mcp_lan/lan_service.cpp:325-335`
- `httpd_start()` 失败仅设置 `ex.running = false` 后返回，没有 notify、join 或 delete：`firmware/main/hal/mcp_lan/lan_service.cpp:337-347`
- 工具任务退出循环后会 park：`firmware/main/hal/mcp_lan/lan_service.cpp:79-103`
- 重试可能覆盖 `ex.task`，丢失旧任务句柄。

每次此类失败可泄漏一个任务及其 8 KB 内部栈。启动、注册失败、正常停止应尽量收敛到同一套可重入清理逻辑。

### P2-4：`set_volume` 绕过 codec 控制互斥锁

- `self.audio_speaker.set_volume` 直接调用 HAL：`firmware/main/hal/mcp_lan/mcp_tools.cpp:201-209`
- facade 注释要求所有 codec CONTROL 调用使用 `codec_mutex`，并记录过交错 I2C 导致 `ESP_ERROR_CHECK` abort 的历史：`firmware/main/hal/mcp_lan/device_access.h:28-35`
- Mic/Speaker 初始化使用该锁，但 set_volume 没有使用。

缺少锁是确定的；具体某次交错是否导致硬件错误取决于时序。smoke 恰好没有覆盖这个工具。修复时应统一从 facade 约束 codec 控制调用，避免只在一个新路径再次复制规则。

### P2-5：Bearer token 比较在长度不同的时候会越过字符串结尾读取

`token_matches()` 的循环以两个字符串都到达结尾为结束条件，但每轮同时读取 `presented[i]` 和 `expected[i]`。当两者长度不同，较短一方越过 `\0` 后仍会继续被读取：`firmware/main/hal/mcp_lan/mcp_jsonrpc.cpp:47-62`。

默认空 token 不启用该路径；启用鉴权后，这是明确的内存安全问题。需要重新设计长度安全的比较，并测试不同长度、空值、超长 header 和非 ASCII 输入。

### P2-6：WebSocket token 查询参数解析会拒绝合法的前置 token

`service_ws_authorized()` 对 `token=` 后面的值直接 `strlen()` 到 URI 末尾：`firmware/main/hal/mcp_lan/lan_service.cpp:259-279`。因此 `?token=abc&other=1` 会把 `abc&other=1` 当作 token，而不是 `abc`。应统一解析 query 参数边界，并覆盖 token 位于第一个、中间、最后一个参数的情况。

### P2-7：HTTP/WebSocket 单次的 socket timeout 不等于完整消息有截止时间

- HTTP body 循环只检查是否收满：`firmware/main/hal/mcp_lan/mcp_jsonrpc.cpp:232-247`
- WebSocket frame 读取：`firmware/main/hal/ws_util.h:32-75`
- 服务从 `HTTPD_DEFAULT_CONFIG()` 开始，没有建立应用层完整消息 deadline：`firmware/main/hal/mcp_lan/lan_service.cpp:337-343`

IDF 默认的单次收发等待有限，但客户端只要不断在单次超时前提供少量数据，就可以让完整 body/frame 的总耗时没有上限，并长期占用共享 HTTP 任务。需要总时长限制、慢客户端测试和明确的失败响应/关闭行为。

### P2-8：执行队列静默丢弃 job，没有完成或取消等待者

`service_execute()` 在队列超过 4 个时直接 `pop_front()`：`firmware/main/hal/mcp_lan/lan_service.cpp:192-202`。被丢弃 job 的调用方收不到完成通知，只能等待自己的 30 秒超时，随后进入超时泄漏路径。`service_stop()` 的 `jobs.clear()` 同样没有逐个完成/取消等待者：`firmware/main/hal/mcp_lan/lan_service.cpp:390-395`。

当前单 HTTP handler 模型降低了大队列出现的概率，但这个通用执行接口的所有权和背压语义仍然是错的。后续异步化时必须先修复，否则更容易暴露。

## 7. 条件性风险：需要专项复核，不能当成已复现事实

以下问题有静态依据，但是否在目标设备形成具体故障，需要更完整的并发时序或故障注入验证。它们不应替代前述确定缺陷的优先级。

1. **camera frame 跨消费者生命周期**  
   `mcp_photo.cpp:60-78` 读取 `camera->GetFrameData()` 时没有相机级所有权保护；另一个 camera 消费者可能再次 `StreamCaptures()` 并释放/替换 frame：`firmware/main/hal/board/stackchan_camera.cc:873-880`。另一个实际消费者见 `firmware/main/hal/hal_ws_avatar.cpp:417-449`。需要明确 camera frame 的共享所有权，而不仅是照片 JPEG store 的锁。

2. **Mic 排队发送只保存整数 FD，存在 FD 复用 ABA 风险**  
   `firmware/main/hal/mic_ws_stream.cpp:41-59`、`firmware/main/hal/mic_ws_stream.cpp:339-350`。旧 session 的 queued work 若在新 session 复用同 FD 后执行，可能发错数据或释放新 session。建议使用 session/generation 身份，而不是只比较 FD。

3. **任务和 server 句柄跨任务访问缺少统一同步**  
   `capture_task_`、`playback_task_`、`server_` 等在 HTTP、capture/playback、status 和 Stop 路径中读写，例如 `firmware/main/hal/mic_ws_stream.cpp:401-449`、`firmware/main/hal/speaker_ws_stream.cpp:260-310`。需要核对 Start/Stop/status/Ensure 并发时是否存在删除后使用或重复删除。

4. **服务状态标志不是统一的同步对象**  
   `g_running` 和 `Executor::running` 的部分访问在锁外，部分在锁内：`firmware/main/hal/mcp_lan/lan_service.cpp:76-103`、`firmware/main/hal/mcp_lan/lan_service.cpp:177-204`、`firmware/main/hal/mcp_lan/lan_service.cpp:293-317`。从 C++ 内存模型看需要统一原子或锁协议，不能依赖“当前 Xtensa 上通常能看见”。

5. **低内存路径缺少系统性检查**  
   例如 `status_handler()` 未检查 `cJSON_PrintUnformatted()` 结果便把 `printed` 交给 `strlen()`：`firmware/main/hal/mcp_lan/lan_service.cpp:128-156`。本设备内部 RAM 紧张是既有设计约束，建议把 OOM 作为错误路径测试，而不是只测正常堆余量。

## 8. 现有测试覆盖结论

### 8.1 不是“完全没有测试”，但不是完整自动化验收

现有文件：

- `firmware/scripts/mcp_smoke.py`：依赖真实设备 IP 的顺序 smoke。
- `firmware/scripts/ws_audio_test.py`：WebSocket 音频辅助脚本。
- `firmware/tests/motion_math_test.cpp`：与 MCP 无关的运动数学测试。
- `firmware/tests/CMakeLists.txt:7-18`：CTest 只注册 motion math，没有 MCP 测试。

静态比较 smoke 的默认工具清单与实际 `call_tool` 调用：默认 19 个工具，只调用了 17 个正常工具，另有一个故意的不存在工具错误测试。

未调用的默认工具：

- `self.audio_speaker.set_volume`
- `self.screen.set_brightness`

主要缺口：

- 没有工具超过 30 秒、晚完成、取消或永久阻塞测试。
- 没有 API 执行中按 Home、关闭中重进、连续进入退出测试。
- 没有并发 create/list/stop reminder 与到期更新测试。
- 没有拍照和照片下载并发、无帧、编码卡住测试。
- 没有长 `show_message` 后退出、旧 timer 晚到测试。
- 没有 WebSocket 异常断开、FD 复用、第二客户端 busy 后恢复测试。
- 没有 auth 错误 token、不同长度、WS query 参数边界测试。
- 没有慢 HTTP body、慢 WebSocket frame、send backpressure 测试。
- 没有 `camera.take_photo` 的 `embed:true` 分支测试。
- 没有 service_start 各阶段失败后的资源回收与重试测试。
- 没有 OOM、锁获取失败、codec 并发控制故障注入。

`ws_audio_test.py` 的 mic 路径只打印帧数，没有断言帧数必须大于零；speaker 路径主要验证发送流程，不能单独证明设备实际发声。现有“smoke 全绿”不能覆盖本报告的并发和生命周期问题。

## 9. 建议的修复顺序与架构约束

不要一次性大范围改写所有模块。以下每一批都应带对应测试，并核对共享代码对 AI.AGENT、Avatar、音频、提醒和 camera 现有功能的影响。

### 第一批：先消除内存破坏和直接卡死链

处理 P0-1、P1-1、P1-2、P1-3。它们相互关联，设计时要一起考虑，但提交仍应尽量小且可验证。

必须建立以下不变量：

- job 入队后，参数和完成对象的生命周期不依赖 HTTP 等待者是否超时。
- HTTP 超时与工具取消是不同状态，不能混为一谈。
- Mooncake 主任务不做无界停止等待。
- 不靠强删任务释放锁或结束硬件调用。
- camera 和其他驱动调用有有限等待与退出路径。

### 第二批：修复页面和共享对象生命周期

处理 P1-5、P1-6、P1-7、P1-8、P2-1。

必须建立以下不变量：

- App 退出后，没有属于旧 App 的 timer、modifier 或 callback 再访问旧 avatar。
- reminder、photo 等共享数据只有一个一致的同步/所有权协议。
- 未获得 LVGL 锁时绝不操作受保护对象，也绝不 unlock。
- 清理本 App 资源时不误删其他 App 的资源。

### 第三批：补齐音频、启动失败和网络健壮性

处理 P2-2 至 P2-8，并复核第 7 节条件性风险。

重点是会话身份、各错误路径统一清理、完整消息 deadline、codec 控制锁和鉴权边界。

### 不建议采用的“修复”

- 只把 30 秒改成 60 秒或更久。
- 只增加队列长度。
- 只在现有调用外再创建一个 FreeRTOS task。
- 只调换 `service_stop()` 中几行顺序。
- 超时后继续由 HTTP 和 worker 双方分别释放同一份参数。
- 通过强删任务来掩盖底层不可取消等待。
- 在全局照片锁内执行慢 socket send。
- 没有审计共享调用方就直接改变通用 reminder、LVGL、camera 行为。

## 10. 后续实现 Agent 的最低验收清单

### A. 工具执行与超时

- [ ] 可控工具运行超过 30 秒，HTTP 超时后晚完成，不发生 UAF、double-free、信号量或堆泄漏。
- [ ] 工具排队超时、取消、服务关闭时仍有明确完成状态，没有静默丢失等待者。
- [ ] 一个工具卡住时，系统行为符合明确的背压/取消设计，而不是每个后续请求都再卡 30 秒。

### B. 页面导航与生命周期

- [ ] API 执行中按 Home，可以在约定的有限时间内返回；Mooncake 主任务不等待无界 teardown。
- [ ] 离线进入和 Wi-Fi 等待期间可以取消并返回。
- [ ] 连续打开/关闭 MCP.AGENT，任务数、modifier 数、heap、FD、timer 数量回到稳定基线。
- [ ] `show_message` 后退出，旧 timer 到期不崩溃、不修改新页面。
- [ ] 退出前后注入 HeadPet 事件，不访问空 avatar，不累积 modifier。

### C. Camera 与 Photo

- [ ] 无帧、编码线程卡住、连续拍照、拍照中退出都有有限时间错误路径。
- [ ] 拍照和 `/photo.jpg` 下载并发，不发生释放后读取，图片内容属于某个完整快照。
- [ ] MCP camera 与既有 avatar camera 消费者并发时，frame 生命周期安全。
- [ ] `embed:true` 和 URL 两种返回都覆盖。

### D. Reminder、LVGL 与共享状态

- [ ] 并发 create/list/stop 和到期更新，包含超过 5 个提醒迫使 vector 扩容，无数据竞争和崩溃。
- [ ] LVGL 锁获取失败被显式处理，未持锁时不访问、不解锁。
- [ ] AI.AGENT 的提醒、显示、avatar 行为无回归。

### E. Audio 与 WebSocket

- [ ] 19 个默认工具逐一覆盖，特别补上 set_volume、set_brightness 和各自参数边界。
- [ ] set_volume 与 mic/speaker 初始化和关闭并发，codec 控制不交错。
- [ ] start 后直接断开、stop 断开、异常网络断开、FD 复用、第二客户端重连，claim 最终都能正确释放。
- [ ] mic 必须断言收到有效帧；speaker 除了发送成功，还要保留真机实际放音验收。

### F. HTTP、鉴权与资源回收

- [ ] 慢 body、慢 frame、发送 backpressure 都有完整消息 deadline，不能长期独占 HTTP 任务。
- [ ] token 长度不同、错误 token、超长 header、WS token 位于不同 query 位置都通过测试。
- [ ] `httpd_start`、URI 注册、mic/speaker Start 等各阶段失败后重试，不泄漏任务、栈或 socket。
- [ ] 重复超时、异常断开和启动失败后的 heap/task/FD 数量稳定。

## 11. 交接完成标准

后续 Agent 不应以“编译通过”或“原 smoke 全绿”作为唯一完成标准。完成时至少需要：

1. 说明每个已修问题的新所有权、取消和退出协议，而不仅是列出改动。
2. 提供对应自动化测试和真机验证结果；未执行的项目明确标注，不能把静态推断写成已验证。
3. 验证 Home 返回、重复进入退出、资源基线和既有 AI.AGENT 无回归。
4. 更新 `firmware/docs/mcp_agent_progress.md`，记录测试版本、命令、结果和仍存在的限制。
5. 若某项因硬件状态无法验证，明确写出缺什么设备操作或故障注入条件，不把风险关闭为“应该没问题”。

---

## 12. 2026-10-28 修复与验证状态（后续追加）

本节不改变前述静态审查的历史结论。状态分为“代码已修且有对应证据”“仍阻塞”“未验证”；不把代码修改本身等同于完整验收。

### 12.1 已有对应修复和证据

- **P0-1**：`AsyncToolJob` 共享拥有参数、ID、结果和 async request；HTTP 超时不再释放 worker 仍使用的参数，响应只完成一次。可控长工具/重复超时故障注入仍未完成。
- **P1-1**：工具请求使用 `httpd_req_async_handler_begin()`；Home 使用 request/stopped/reclaim，HTTPD 停止等待不在 Mooncake。单次关闭重进及扩展 smoke 通过，10 轮导航另行记录。
- **P1-2**：MCP 和 AUDIO.TEST 正常退出不再强删任务；WithCaps 句柄在退出后由 owner 回收。底层驱动永久等待不能靠此机制取消。
- **P1-4**：离线网络启动不再调用原 65 秒阻塞流程，HTTPD 只在连接已建立后创建。物理离线导航仍待人工验收。
- **P1-5**：speech timer 由 App 持有并在关闭时取消；使用 `lv_timer_get_user_data()`，不再依赖旧全局 timer。
- **P1-6**：提醒为 App-local 单 UI 上下文 store；host 测试覆盖超过 5 个、一次性、重复、停止和清空。
- **P1-7**：JPEG 使用不可变 shared snapshot；URL 和 embed smoke 通过，关闭时清理保留照片。相机 frame 本身的风险不因 JPEG 快照消失。
- **P2-1**：Breath/Blink/HeadPet 按 ID 移除，HeadPet 遥测连接断开。
- **P2-2**：socket-close 释放两路音频 claim，mic 带 session 防 FD 复用；同时修复了 `close_fn` 未实际 `close(fd)` 导致的 accept(23)。
- **P2-3**：任务创建失败、HTTPD/endpoint 启动失败进入统一停止路径；启动故障注入仍未完成。
- **P2-4**：`set_volume` 纳入 codec 控制锁，smoke 25/75 通过。
- **P2-5 / P2-6**：Bearer 比较长度安全，WS query 按 `&` 边界解析。启用 token 的专项测试未执行。
- **P2-8**：等待队列有界，满载返回 503，不再静默 pop/drop。
- **UI 附加问题**：`onOpen()` IP hint 刷新纳入 LVGL 锁后，原先的 `set_emotion` UI 超时和看门狗路径消失，扩展 smoke 全通过。UI action 异常/超时取消由 host 测试覆盖。

### 12.2 仍然阻塞或未关闭

- **P1-3 相机不可取消**：未改 HAL，join/DQBUF/底层永久等待无法保证取消，不能计入已修。
- **P1-8 LVGL 锁失败**：共享 guard 未改变，仍不能表达未取得锁；未做锁失败下的安全保证。
- **P2-7 完整 WS frame deadline**：HTTP body 已有 10 秒总 deadline，WS 慢帧总 deadline 未实现。
- **条件性风险**：跨 camera 消费者 frame 所有权、任务/server 句柄的全部并发时序、OOM 和锁失败注入仍需专项验证。
- **音频内部 RAM（新运行时证据）**：Codec 构造后 free internal 约 25 KB、largest 16 KB，22 KB mic 栈无法创建；mic-first 的 DMA 断言已改为可返回错误。20 ms/16 KB 候选栈溢出已撤回。mic/speaker/全双工不能记为已验证。

### 12.3 AI 与其他 App 回归

- fresh、MCP 后、AUDIO.TEST 后三条 AI 数据链路均取得检测任务和有效 PCM；详见 `ai_agent_vad_static_review.md` 第 11 节。
- MCP→AI 的重复 `StartNetwork()` 无 `Connected` 事件问题已通过 App 内 Station 交接修复。正常 Home 不断网。
- 唤醒词、触摸 Listening、实际放音、舵机安全和其他 App 肉眼导航仍需现场人员，不在“已通过”范围内。

### 12.4 后续验收原则

继续保留长任务、慢客户端、并发/背压、故障注入和资源计数门槛。任何“smoke 全绿”都只代表对应顺序工具调用，不替代上述未决项。10 轮导航和最终生产烧录的实测结果写入 `mcp_agent_progress.md`。
