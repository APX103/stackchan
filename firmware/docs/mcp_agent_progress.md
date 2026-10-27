# MCP Agent 实现进度（feature/mcp-agent）

按 `docs/mcp_agent_design.md` 的里程碑推进。每项标注状态：✅ 完成 / ⏳ 待真机验证 / ❌ 未做。

## M-1 基线清理 — ✅（已验证）

- vendored `firmware/xiaozhi-esp32/` 还原至 v2.2.4 原样，官方补丁
  `patches/xiaozhi-esp32.patch` 以 `--ignore-whitespace` 干净应用（9 文件，
  与官方补丁范围一致）。上一轮的子仓改动已留底在
  `backup/agent-mods-20261027:firmware/patches/xiaozhi-esp32_agent_mods_backup_20261027.patch`。
- 基线（官方代码+补丁，未含新功能）在本机 ESP-IDF 5.5.5 构建通过。
- 注：`fetch_repos.py` 在本机 CRLF 环境会因 `git apply --check` 失败而
  **静默跳过**官方补丁；重新克隆后需手动
  `git -C xiaozhi-esp32 apply --ignore-whitespace <绝对路径补丁>`。

## 代码实现 — ✅ 编译通过，⏳ 全部待真机验证

最终文件布局（相对设计文档 §13 有小调整：工具合并为单文件）：

- 新增 `main/hal/mcp_lan/`：`device_access`（硬件门面，全特性唯一允许
  接触 xiaozhi 头文件的文件）、`mcp_registry`、`mcp_jsonrpc`、`lan_service`、
  `mcp_photo`、`mcp_tools`（19 工具）
- 新增 `main/hal/`：`mic_ws_stream`、`speaker_ws_stream`、`ws_util.h`
  （自 backup 分支移植：token 校验、干净退出、去掉了 200 块原始诊断扫描）
- 新增 `main/apps/app_mcp_agent/`（状态页：服务 URL / 统计 / 最近事件）
- 新增 `main/assets/assets_bin/icon_mcp_agent.bin`（RGB565A8 188×150，
  头部与官方图标逐字节一致；由脚本生成——见本文件附录）
- 新增 `firmware/scripts/mcp_smoke.py`（零依赖冒烟测试）、
  `firmware/scripts/ws_audio_test.py`（mic 录音 / speaker 放音）
- 修改官方文件 6 处（均为设计文档 §13.3 清单内）：`apps.h`、`main.cpp`
  （条件安装）、`CMakeLists.txt`（+esp_http_server/esp_timer）、
  `Kconfig.projbuild`（MCP Agent 菜单）、`sdkconfig.defaults`（+HTTPD_WS）、
  `hal_bridge.cc`（app_play_sound 防挂死守卫）

关键实现决策（与设计文档的差异，均已按用户拍板）：

1. **红外砍掉**（用户确认）。
2. App 名 `MCP.AGENT`（用户授权自定）。
3. 退出走**优雅关闭**（不 warm reboot；流任务带 exited 标志，最坏兜底
   强删并告警）。
4. 用户追加约束：**不调用小智服务层**。MCP 层完全独立实现；`device_access`
  单点封装硬件获取（摄像头/电池/背光走本仓库 hal_bridge，仅音频编解码器
  单例经 vendored Board 获取——该获取方法定义在本仓库 stackchan.cc）。
  云端工具体（hal_mcp.cpp 的舵机/LED/提醒）按"抄过来"原则复制到
  `mcp_tools.cpp` 并重绑注册表。
5. `self.reboot` 由 Kconfig 门控（默认 n），重启在 HTTP 应答发出后延迟执行。

## 待真机验收清单（按设计文档 §14）

- [ ] M0：启动器出现 MCP.AGENT 图标（新图标需刷 assets 分区）；进入后
      显示 IP；`curl http://<ip>:8080/` 返回状态 JSON；退出回启动器。
- [ ] M1：`python scripts/mcp_smoke.py <ip>` 全绿；MCP Inspector 可见 19 工具。
- [ ] M2：肉眼确认表情/气泡；浏览器开 /photo.jpg 为 320×240 JPEG，
      连拍两张内容不同。
- [ ] M3：`ws_audio_test.py mic` 录 10s → ffmpeg 解码 → 有人声；第二客户端
      busy；断开 3 次不崩。
- [ ] M4：speaker 放音清晰、1000 帧不断流、停完 /mic 仍可用。
      **风险点：speaker 端到端从未真机验证过**（前一轮仅编译通过）。
- [ ] M5：token 模式、连续开关 App ×10、断网 30s 自愈、
      `get_device_status` 的 free_heap 回落基线 ±10%。

## 附录：图标再生成

```bash
python -  <<'EOF'   # 结构：12B 头(<IHHHH: 0x19|cf0x14|flags, w,h,stride,0) + RGB565 面 + A8 面
EOF
# 实际脚本见 git 历史本次提交信息；格式结论来自
# managed_components/lvgl__lvgl/src/misc/lv_color.h:156 (RGB565A8=0x14)
```
