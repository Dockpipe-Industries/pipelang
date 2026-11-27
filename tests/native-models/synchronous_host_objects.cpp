// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "SyncHostObjects.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using namespace pipelang::model;
using namespace sync_host_objects;
using IntResult = Result<std::int64_t>;

namespace {
class NativeWorld final : public World {
public:
    IntResult Read(std::int64_t number) override {
        assert(std::this_thread::get_id() == owner);
        ++effects;
        if (callback)
            callback();
        if (throwing)
            throw std::runtime_error("object host fault");
        if (decline)
            return IntResult::Failure("object host declined");
        return IntResult::Success(number);
    }
    std::thread::id owner = std::this_thread::get_id();
    int effects = 0;
    bool throwing = false;
    bool decline = false;
    std::function<void()> callback;
};
}

int main() {
    auto world = std::make_shared<NativeWorld>();
    App model(world);
    world->callback = [&] {
        bool refused = false;
        try {
            (void)model.Calls();
        } catch (const ExecutionFailure& error) {
            refused = true;
            assert(error.what() == std::string("model reentry during a synchronous host callback"));
        }
        assert(refused);
    };
    assert(model.Run().value() == 1 && model.Calls() == 1);
    world->decline = true;
    assert(model.Run().is_failure() && model.Calls() == 2);
    world->decline = false;
    world->throwing = true;
    const auto failed = model.Run();
    assert(failed.is_failure() && failed.error() == "object host fault");
    assert(model.Calls() == 2 && world->effects == 3);
    world->throwing = false;
    assert(model.Run().value() == 3 && model.Calls() == 3);
}
