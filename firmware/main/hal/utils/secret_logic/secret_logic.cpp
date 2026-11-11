/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "secret_logic.h"
#include <sdkconfig.h>

// SEP-0002: optional runtime override (NVS), see hal/sep/sep_automation.h
#include "hal/sep/sep_automation.h"

namespace secret_logic {

__attribute__((weak)) std::string get_server_url()
{
    // SEP-0002: NVS override wins over the build-time default
    const auto override_url = sep::serverUrlOverride();
    if (!override_url.empty()) return override_url;
#ifdef CONFIG_STACKCHAN_SERVER_URL
    return CONFIG_STACKCHAN_SERVER_URL;
#else
    return "http://localhost:3000";
#endif
}

__attribute__((weak)) std::string generate_auth_token()
{
    return "hi-stack-chan";
}

__attribute__((weak)) std::string generate_handshake_token(std::string_view data)
{
    return "hi-stack-chan";
}

}  // namespace secret_logic
