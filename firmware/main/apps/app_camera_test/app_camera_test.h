/*
 * SEP-0008: camera preview app — live view of what the GC0308 sees.
 */
#pragma once
#include <mooncake.h>

#include <cstdint>

class AppCameraTest : public mooncake::AppAbility {
public:
    AppCameraTest();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    void* _rgb565      = nullptr;  // PSRAM frame buffer (w*h*2)
    void* _img_dsc     = nullptr;  // lv_image_dsc_t
    void* _img         = nullptr;  // lv_obj_t*
    void* _fps_label   = nullptr;  // lv_obj_t*
    uint32_t _last_ms  = 0;
    uint32_t _fps_tick_ms = 0;
    uint32_t _fps_count  = 0;
};
