/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <sdkconfig.h>
#include "app_launcher/app_launcher.h"
#include "app_ai_agent/app_ai_agent.h"
#if CONFIG_MCP_AGENT_ENABLE
#include "app_mcp_agent/app_mcp_agent.h"
#endif
#if CONFIG_AUDIO_TEST_ENABLE
#include "app_audio_test/app_audio_test.h"
#endif
#include "app_avatar/app_avatar.h"
#include "app_setup/app_setup.h"
#include "app_espnow_ctrl/app_espnow_ctrl.h"
#include "app_app_center/app_app_center.h"
#include "app_ezdata/app_ezdata.h"
#include "app_dance/app_dance.h"
