#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

#include "apps/app_mcp_agent/service/ui_pending.h"

namespace {

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main()
{
    using mcp_lan::UiJobResult;
    using mcp_lan::UiPending;

    auto completed = std::make_shared<UiPending>();
    bool called = false;
    std::thread worker([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        expect(completed->run([&] { called = true; }) == UiJobResult::Completed,
               "normal UI action completes");
    });
    expect(completed->wait(1000), "wait observes completion");
    worker.join();
    expect(called, "normal action ran");

    auto cancelled = std::make_shared<UiPending>();
    cancelled->cancel();
    called = false;
    expect(cancelled->run([&] { called = true; }) == UiJobResult::Cancelled,
           "cancelled action reports cancellation");
    expect(!called && !cancelled->wait(0), "cancelled action neither runs nor succeeds");

    auto late = std::make_shared<UiPending>();
    called = false;
    expect(!late->wait(5), "short wait times out");
    expect(late->run([&] { called = true; }) == UiJobResult::Cancelled,
           "late execution observes timeout cancellation");
    expect(!called, "timed-out callback is not executed");

    auto failed = std::make_shared<UiPending>();
    expect(failed->run([] { throw std::runtime_error("test"); }) == UiJobResult::Failed,
           "exception is contained");
    expect(!failed->wait(0), "failed action is not reported as successful");

    auto async_timeout = std::make_shared<UiPending>();
    called = false;
    std::thread waiter([&] { expect(!async_timeout->wait(10), "concurrent wait times out"); });
    waiter.join();
    expect(async_timeout->run([&] { called = true; }) == UiJobResult::Cancelled,
           "concurrent late completion is cancelled");
    expect(!called, "concurrent late callback stays cancelled");

    std::cout << "mcp_ui_pending_test: all tests passed\n";
    return 0;
}
