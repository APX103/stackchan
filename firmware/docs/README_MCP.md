# MCP Agent 使用说明（局域网 MCP Server）

MCP.AGENT 是 StackChan 启动器里的一个独立应用：打开它，机器人就在局域网里
变成一台 MCP 服务器，内网任何 MCP 客户端都能直接控制它——不经过小智云端、
不需要外网。

## 快速开始

1. 构建并烧录（含 assets 分区，图标在 assets 里）：
   ```powershell
   powershell -ExecutionPolicy Bypass -File build_agent.ps1 flash
   ```
2. 机器人进配网（首次用官方 App 蓝牙配网），在启动器里打开 **MCP.AGENT**。
   屏幕上会显示服务地址，例如 `http://192.168.1.23:8080/mcp`。
3. 在同网段的电脑上冒烟测试（无第三方依赖）：
   ```bash
   python scripts/mcp_smoke.py 192.168.1.23
   ```

## 服务端点

| 端点 | 说明 |
|---|---|
| `POST /mcp` | MCP Streamable HTTP（无状态 JSON-RPC 2.0） |
| `GET /` | 服务状态 JSON |
| `GET /photo.jpg` | 最近一次 `self.camera.take_photo` 的 JPEG |
| `WS /mic` | 麦克风流：发 `{"type":"start"}` → 收二进制 Opus 包（16k/mono/60ms）→ `{"type":"stop"}` |
| `WS /speaker` | 扬声器流：发 `{"type":"start"}` → 发二进制 Opus 包（同参数，~16.7 包/秒）→ `{"type":"stop"}` |

音频流一次各允许一个客户端；播放会串进麦克风（无回声消除）。

## 工具一览（19 个）

- 设备：`self.get_device_status`、`self.get_system_info`
- 音频：`self.audio_speaker.set_volume`、`self.microphone.stream`、`self.speaker.stream`
- 屏幕：`self.screen.get_info`、`self.screen.set_brightness`
- 表情/文字：`self.display.set_emotion`、`self.display.show_message`、`self.display.clear_message`
- 头部（双舵机）：`self.robot.get_head_angles`、`self.robot.set_head_angles`、`self.robot.look_at`、`self.robot.go_home`
- 灯：`self.robot.set_led_color`
- 提醒：`self.robot.create_reminder`、`self.robot.get_reminders`、`self.robot.stop_reminder`
- 摄像头：`self.camera.take_photo`（本地出图，返回 `/photo.jpg` URL，可 `embed:true` 内联 base64）
- `self.reboot`：默认不注册（Kconfig `MCP_AGENT_ENABLE_OWNER_TOOLS=y` 打开）

官方云 MCP 的工具名与参数语义保持一致（`upgrade_firmware`/`set_theme` 等
危险或无意义项除外），客户端提示词可无缝复用。

## 客户端接入

Python（官方 MCP SDK）：
```python
from mcp import ClientSession
from mcp.client.streamable_http import streamablehttp_client

async with streamablehttp_client("http://192.168.1.23:8080/mcp") as (r, w, _):
    async with ClientSession(r, w) as s:
        await s.initialize()
        tools = await s.list_tools()
        await s.call_tool("self.robot.set_head_angles", {"yaw": -45, "pitch": 60})
```

Claude Desktop（经 mcp-proxy）：
```json
{"mcpServers": {"stackchan": {"command": "mcp-proxy", "args": ["http://192.168.1.23:8080/mcp"]}}}
```

Cursor：Settings → MCP → Add HTTP MCP server → `http://192.168.1.23:8080/mcp`。

音频流测试：
```bash
pip install websocket-client
python scripts/ws_audio_test.py mic 192.168.1.23            # 录 10 秒
ffmpeg -i in.wav -ac 1 -ar 16000 -c:a libopus -application voip -frame_duration 60 -f data play.opus
python scripts/ws_audio_test.py speaker 192.168.1.23 play.opus  # 放音
```

## 配置（menuconfig → MCP Agent）

| 选项 | 默认 | 说明 |
|---|---|---|
| `MCP_AGENT_ENABLE` | y | 安装 MCP.AGENT 应用 |
| `MCP_AGENT_PORT` | 8080 | HTTP/WS 端口 |
| `MCP_AGENT_TOKEN` | 空 | 非空时 /mcp 要 `Authorization: Bearer`，WS 要 `?token=` |
| `MCP_AGENT_ENABLE_OWNER_TOOLS` | n | 暴露 self.reboot |

## 与官方 AI.AGENT 的关系

共存安装、互斥运行：打开 MCP.AGENT 不会启动小智应用；退出 MCP.AGENT 后
所有端口、任务、音频占用全部释放，官方 AI.AGENT 行为完全不变。
实现上不修改 `firmware/xiaozhi-esp32/`（vendored 小智仓库）的任何文件，
也不调用其应用层服务；唯一的接触点是 `hal/mcp_lan/device_access.cpp`
里获取音频编解码器单例（该单例与获取方法都定义在本仓库）。
