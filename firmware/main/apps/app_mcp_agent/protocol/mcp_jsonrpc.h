/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <esp_http_server.h>

#include <string>

#include "mcp_registry.h"

namespace mcp_lan {

class LanService;

// SEP 服务化:把原 JSON-RPC /mcp 的工具面收敛为 REST 控制端点。
// 每条路由把请求体 JSON 作为工具参数,走与旧 tools/call 完全相同的
// submit_tool 异步执行路径;响应统一 {"ok":bool,...}。
// 路由表(与 sep/docs/miwave-agent-robot-设计方案.md §3.1 对应):
//   POST   /display/emotion    self.display.set_emotion
//   POST   /display/message    self.display.show_message
//   DELETE /display/message    self.display.clear_message
//   GET    /screen             self.screen.get_info
//   POST   /screen/brightness  self.screen.set_brightness
//   GET    /robot/head         self.robot.get_head_angles
//   POST   /robot/head         set_head_angles | look_at | go_home(按 body 形状分派)
//   POST   /led                self.robot.set_led_color
//   POST   /speaker/volume     self.audio_speaker.set_volume
//   POST   /speaker/play       self.audio_speaker.play_opus
//   POST   /camera/photo       self.camera.take_photo
//   POST   /reboot             self.reboot
bool register_rest_endpoints(httpd_handle_t server, LanService& service);

// REST 响应构造(executor 异步完成时共用)。
std::string make_rest_result(const ToolOutput& output);
std::string make_rest_error(const char* message);

}  // namespace mcp_lan
