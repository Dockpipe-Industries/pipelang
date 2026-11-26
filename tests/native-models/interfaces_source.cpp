// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "InterfaceInheritance.hpp"
#include "InterfaceModules.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <thread>

int main() {
    interface_inheritance::App model;
    assert(model.Identities().value());
    assert(model.Values().value() == 72);
    assert(model.ExactChoice().value() == 103);
    assert(model.DerivedChoice().value() == 303);
    assert(model.BaseChoice().value() == 203);
    assert(model.TransitiveArguments().value() == 6);
    assert(model.MutateThroughBase(7).value() == 7);
    const auto before = model.snapshot();
    const auto failed = model.FailInheritedMutation();
    assert(failed.is_failure() && failed.error() == "inherited mutation fault");
    assert(model.snapshot() == before && model.Read().value() == 7);
    auto task = model.WaitThroughBase();
    assert(!task.completed() && model.pending() == 1);
    assert(model.MutateThroughBase(9).value() == 9);
    assert(model.ReleaseSignal().is_success());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!task.completed()) {
        model.poll();
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    assert(task.result().is_success() && task.result().value() == 9);
    assert(model.Read().value() == 9 && model.Identities().value());

    interface_modules::App modular;
    assert(modular.Read().value() == 31);
}
