// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "FoundationObjects.constants.hpp"
#include "FoundationObjects.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <functional>
#include <memory>
#include <string>

using namespace pipelang::model;
using namespace foundation_objects;

namespace {
struct Queries final : RotationQueries {
    int mode = 0;
    std::function<void()> reenter;
    Result<Vector, std::string> Rotate(Vector value) const override {
        if (reenter)
            reenter();
        if (mode == 1)
            return Result<Vector, std::string>::Failure("rotation unavailable");
        if (mode == 2)
            return Result<Vector, std::string>::Cancelled();
        return Result<Vector, std::string>::Success({-value.y, value.x});
    }
    Result<Vector, std::string> Height(Vector value) override {
        return Result<Vector, std::string>::Success(value);
    }
};
}

int main() {
    auto queries = std::make_shared<Queries>();
    App model(queries);
    static_assert(App::EyeHeight == 168.0f);
    assert(model.ReadHeight() == App::EyeHeight);
    assert(model.Rotate({3.0f, 4.0f}).value() == (Vector{-4.0f, 3.0f}));
    queries->mode = 1;
    assert(model.Rotate({}).is_failure());
    queries->mode = 2;
    assert(model.Rotate({}).is_cancelled());
    queries->mode = 0;
    bool rejected = false;
    queries->reenter = [&] {
        try {
            (void)model.ReadHeight();
        } catch (const ExecutionFailure&) {
            rejected = true;
        }
    };
    assert(model.Rotate({}).is_success() && rejected);
}
