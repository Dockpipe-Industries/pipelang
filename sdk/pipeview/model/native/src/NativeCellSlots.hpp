// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <pipelang/model/object.hpp>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace pipelang::native_app_v19 {

template<class Cell>
class NativeCellSlots {
public:
    std::size_t checkpoint() const noexcept { return cells_.size(); }

    void restore(std::size_t checkpoint) noexcept
    {
        while (cells_.size() > checkpoint) {
            if (!cells_.back())
                --available_;
            cells_.pop_back();
        }
    }

    std::size_t retain(Cell cell, const char* exhausted)
    {
        if (available_ != 0) {
            const auto entry = std::find(cells_.begin(), cells_.end(), std::nullopt);
            entry->emplace(std::move(cell));
            --available_;
            return static_cast<std::size_t>(entry - cells_.begin());
        }
        if (cells_.size() == model::ObjectDomain::maximumCells)
            throw model::ExecutionFailure(exhausted);
        cells_.emplace_back(std::move(cell));
        return cells_.size() - 1;
    }

    const Cell* find(std::size_t index) const noexcept
    {
        if (index >= cells_.size() || !cells_[index])
            return nullptr;
        return &*cells_[index];
    }

    void retire(std::size_t index) noexcept
    {
        if (index >= cells_.size() || !cells_[index])
            return;
        cells_[index].reset();
        ++available_;
    }

private:
    std::vector<std::optional<Cell>> cells_;
    std::size_t available_ = 0;
};

}
