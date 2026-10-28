#include <cassert>
#include <string>
#include <vector>

#include "apps/app_mcp_agent/service/reminder_store.h"

int main()
{
    mcp_lan::ReminderStore store;
    const int once = store.create(1000, 100, "once", false);
    const int repeat = store.create(1050, 50, "repeat", true);
    for (int i = 0; i < 6; ++i) {
        store.create(2000 + i, 1000, "bulk", false);
    }
    assert(store.active().size() == 8);

    size_t fired = 0;
    store.tick(1049, [&](const mcp_lan::ReminderInfo&) { ++fired; });
    assert(fired == 0);

    store.tick(1099, [&](const mcp_lan::ReminderInfo&) { ++fired; });
    assert(fired == 0);

    store.tick(1100, [&](const mcp_lan::ReminderInfo& info) {
        ++fired;
        assert(info.id == once || info.id == repeat);
    });
    assert(fired == 2);
    assert(store.active().size() == 7);

    store.tick(1150, [&](const mcp_lan::ReminderInfo& info) {
        ++fired;
        assert(info.id == repeat);
    });
    assert(fired == 3);
    assert(store.active().size() == 7);

    store.stop(once);
    store.stop(repeat);
    assert(store.active().size() == 6);
    store.clear();
    assert(store.active().empty());
    return 0;
}
