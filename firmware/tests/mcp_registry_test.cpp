#include <cJSON.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "apps/app_mcp_agent/protocol/mcp_registry.h"

namespace {

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

mcp_lan::Tool make_tool(const char* name, int value = 0)
{
    return {
        name,
        "test tool",
        R"({"value":{"type":"integer","minimum":0,"maximum":10}})",
        [value](const cJSON*) { return mcp_lan::ToolOutput{false, std::to_string(value), {}}; },
    };
}

}  // namespace

int main()
{
    using mcp_lan::Registry;
    using mcp_lan::ToolOutput;
    Registry registry;
    registry.add({});
    expect(registry.size() == 0, "malformed registration is rejected");

    const char* names[] = {"tool0", "tool1", "tool2", "tool3", "tool4",
                           "tool5", "tool6", "tool7", "tool8"};
    for (int i = 0; i < 9; ++i) {
        registry.add(make_tool(names[i], i));
    }
    expect(registry.size() == 9, "nine tools registered");
    expect(registry.find("missing") == nullptr, "missing tool lookup");

    registry.add(make_tool("tool0", 42));
    expect(registry.size() == 9, "duplicate registration replaces in place");
    ToolOutput output;
    std::string error;
    expect(registry.call("tool0", nullptr, &output, &error) && output.text == "42",
           "replacement tool executes");

    expect(!registry.call("missing", nullptr, &output, &error), "unknown call is rejected");
    expect(error.find("missing") != std::string::npos, "unknown call has useful error");

    int next = -1;
    cJSON* page = registry.build_tools_page(0, &next);
    expect(cJSON_GetArraySize(page) == 8 && next == 8, "first page is bounded");
    cJSON* first = cJSON_GetArrayItem(page, 0);
    cJSON* schema = cJSON_GetObjectItemCaseSensitive(first, "inputSchema");
    cJSON* properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    cJSON* value = cJSON_GetObjectItemCaseSensitive(properties, "value");
    expect(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(value, "maximum")),
           "input schema properties are preserved");
    cJSON_Delete(page);

    page = registry.build_tools_page(8, &next);
    expect(cJSON_GetArraySize(page) == 1 && next == -1, "last page has no cursor");
    cJSON_Delete(page);
    page = registry.build_tools_page(-1, &next);
    expect(cJSON_GetArraySize(page) == 0 && next == -1, "negative cursor is empty");
    cJSON_Delete(page);
    page = registry.build_tools_page(99, &next);
    expect(cJSON_GetArraySize(page) == 0 && next == -1, "past-end cursor is empty");
    cJSON_Delete(page);

    registry.add({
        "throwing",
        "throws",
        "not valid json",
        [](const cJSON*) -> ToolOutput { throw std::runtime_error("boom"); },
    });
    expect(registry.call("throwing", nullptr, &output, &error), "throwing tool exists");
    expect(output.is_error && output.text.find("boom") != std::string::npos,
           "tool exception becomes an error result");
    page = registry.build_tools_page(9, &next);
    expect(cJSON_GetArraySize(page) == 1, "schema fallback tool remains listed");
    first = cJSON_GetArrayItem(page, 0);
    schema = cJSON_GetObjectItemCaseSensitive(first, "inputSchema");
    properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    expect(cJSON_IsObject(properties) && cJSON_GetArraySize(properties) == 0,
           "invalid schema falls back to empty properties");
    cJSON_Delete(page);

    std::cout << "mcp_registry_test: all tests passed\n";
    return 0;
}
