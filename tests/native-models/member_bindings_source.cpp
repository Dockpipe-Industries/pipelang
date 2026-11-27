// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "Model.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
#ifdef PIPELANG_BINDING_PROJECT
using App = member_bindings::source::Binding::App;
using BindingHost = member_bindings::source::Binding::BindingHost;
using Point = member_bindings::source::Binding::Point;
using FacetProbe = member_bindings::source::Binding::FacetProbe;
#else
using member_bindings::App;
using member_bindings::BindingHost;
using member_bindings::Point;
#endif
using IntResult = pipelang::model::Result<std::int64_t>;
std::size_t assertions = 0;

void require(bool condition, const char* message) {
    ++assertions;
    if (!condition)
        throw std::runtime_error(message);
}

class Host final : public BindingHost {
public:
    std::int64_t Read() const override {
        return 37;
    }

    IntResult Step(std::int64_t value) override {
        return IntResult::Success(value + 2);
    }
};

}

int main() {
    try {
        App app(std::make_shared<Host>(), 91, 41);
        require(app.snapshot().value == 91, "explicit root field bound to constructor parameter");
        require(app.snapshot().initialized == 11, "root initializer captured the constructor parameter");
        require(app.ItemInitial() == 7, "object initializer captured the constructor parameter");
        require(app.snapshot().fromArgument == 41, "native root initializer lost its constructor input");
        require(app.ItemInput() == 42, "object initializer lost its constructor input");
        require(app.ItemParameter(3) == 94, "object parameter and explicit field were not distinguished");
        require(app.InheritedInitial() == 92, "derived initializer captured parameter instead of initialized base field");
        require(app.InheritedParameter(3) == 95, "inherited field and parameter binding changed");
        require(app.Parameter(3) == 94, "root parameter and explicit field were not distinguished");
        require(app.ConstantParameter(19) == 38, "a constant captured a lexical parameter");
        require(app.NamedTypeParameter(Point{23}) == 23, "a type-qualified constant captured a lexical receiver");
        require(app.ReadHost() == 37, "same-name host injection did not preserve the service boundary");
        require(app.Set(55).is_success(), "explicit root member mutation failed");
        require(app.snapshot().value == 55 && app.snapshot().point.count == 55,
            "explicit root/record assignment mutated a parameter or wrong state");
        require(app.First() == 55, "explicit root collection member did not retain owned mutation");
        require(app.ExplicitCall(8) == 9, "explicit root helper call lost its receiver");
        require(app.GenericCall(17) == 17, "explicit root generic helper binding failed");
        auto inputs = pipelang::model::List<std::int64_t>::Empty();
        inputs.add(2);
        inputs.add(3);
        require(app.Workers(inputs).value() == 10, "stateless worker parameter bound to the shadowed field");
        auto task = app.Forward(17);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!task.completed()) {
            app.poll();
            require(std::chrono::steady_clock::now() < deadline, "explicit async member call did not finish");
            std::this_thread::yield();
        }
        require(task.result().is_success() && task.result().value() == 19,
            "async parameter/member binding did not survive suspension");
        require(app.snapshot().value == 55, "async parameter unexpectedly wrote the shadowed field");
#ifdef PIPELANG_BINDING_PROJECT
        FacetProbe facet(std::make_shared<Host>(), 81);
        require(facet.Initial() == 11 && facet.Parameter(3) == 84,
            "project source-object facet diverged from native owner field binding");
        require(facet.GenericCall(17) == 17, "project source facet lost explicit generic member binding");
        require(facet.Workers(inputs).value() == 10, "project source facet lost worker parameter binding");
        auto facetTask = facet.Forward(17);
        const auto facetDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!facetTask.completed()) {
            facet.poll();
            require(std::chrono::steady_clock::now() < facetDeadline, "source facet async binding did not finish");
            std::this_thread::yield();
        }
        require(facetTask.result().is_success() && facetTask.result().value() == 19,
            "source facet async member binding diverged after suspension");
#endif
        std::cout << "member bindings: " << assertions << " runtime assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
