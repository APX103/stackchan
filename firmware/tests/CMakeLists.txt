cmake_minimum_required(VERSION 3.16)

project(stackchan_firmware_tests LANGUAGES C CXX)

enable_testing()

add_executable(motion_math_test
    motion_math_test.cpp
    ../main/stackchan/motion/motion_math.cpp
)

target_include_directories(motion_math_test PRIVATE
    ../main
)

target_compile_features(motion_math_test PRIVATE cxx_std_17)

add_test(NAME motion_math_test COMMAND $<TARGET_FILE:motion_math_test>)

add_executable(mcp_reminder_store_test
    mcp_reminder_store_test.cpp
    ../main/apps/app_mcp_agent/service/reminder_store.cpp
)
target_include_directories(mcp_reminder_store_test PRIVATE ../main)
target_compile_features(mcp_reminder_store_test PRIVATE cxx_std_17)
add_test(NAME mcp_reminder_store_test COMMAND $<TARGET_FILE:mcp_reminder_store_test>)

add_executable(mcp_ui_pending_test mcp_ui_pending_test.cpp)
target_include_directories(mcp_ui_pending_test PRIVATE ../main)
target_compile_features(mcp_ui_pending_test PRIVATE cxx_std_17)
add_test(NAME mcp_ui_pending_test COMMAND $<TARGET_FILE:mcp_ui_pending_test>)

# The registry itself is host-testable with IDF's real cJSON and a no-op log
# shim. Set STACKCHAN_IDF_PATH (or IDF_PATH) to enable this test on a host.
set(STACKCHAN_IDF_PATH "$ENV{IDF_PATH}" CACHE PATH "ESP-IDF path for host-side cJSON tests")
if(EXISTS "${STACKCHAN_IDF_PATH}/components/json/cJSON/cJSON.c")
    add_executable(mcp_registry_test
        mcp_registry_test.cpp
        ../main/apps/app_mcp_agent/protocol/mcp_registry.cpp
        "${STACKCHAN_IDF_PATH}/components/json/cJSON/cJSON.c"
    )
    target_include_directories(mcp_registry_test PRIVATE
        stubs
        ../main
        "${STACKCHAN_IDF_PATH}/components/json/cJSON"
    )
    target_compile_features(mcp_registry_test PRIVATE cxx_std_17)
    add_test(NAME mcp_registry_test COMMAND $<TARGET_FILE:mcp_registry_test>)
else()
    message(STATUS "mcp_registry_test disabled: set STACKCHAN_IDF_PATH to an ESP-IDF tree")
endif()
