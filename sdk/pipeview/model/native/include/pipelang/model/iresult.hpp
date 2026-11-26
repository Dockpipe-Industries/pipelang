// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "collection.hpp"
#include <memory>
#include <utility>

namespace pipelang::model {
// An owning, read-only result interface. Only the interface conversion allocates;
// the existing Result<T,E> representation and its constexpr contract are unchanged.
template<class E>
class IResult {
    struct Contract {
        virtual ~Contract() = default;
        virtual Outcome outcome() const noexcept = 0;
        virtual const E& error() const noexcept = 0;
        virtual void validatePayload() const = 0;
        virtual std::size_t bytes() const noexcept = 0;
        virtual std::size_t elements() const noexcept = 0;
    };

    template<class T>
    struct Box final : Contract {
        explicit Box(Result<T,E> value, std::size_t bytes, std::size_t elements)
            : value_(std::move(value)), bytes_(bytes), elements_(elements) {}
        Outcome outcome() const noexcept override { return value_.outcome(); }
        const E& error() const noexcept override { return value_.error(); }
        void validatePayload() const override {
            using model::validate;
            validate(value_);
        }
        std::size_t bytes() const noexcept override { return bytes_; }
        std::size_t elements() const noexcept override { return elements_; }
        Result<T,E> value_;
        std::size_t bytes_;
        std::size_t elements_;
    };

public:
    IResult() = default;

    template<class T>
    static IResult From(Result<T,E> value) {
        using model::validate;
        using model::ownedBytes;
        using model::ownedElements;
        validate(value);
        const auto bytes = addOwnedBytes(sizeof(IResult),
            addOwnedBytes(sizeof(Box<T>), ownedBytes(value)));
        const auto elements = boundedOwnedSum(1, ownedElements(value), maximumListElements);
        if (bytes > maximumListBytes || elements > maximumListElements)
            throw ExecutionFailure("result interface exceeds owned value limits");
        return IResult(std::make_shared<const Box<T>>(std::move(value), bytes, elements));
    }

    Outcome outcome() const { return require().outcome(); }
    bool is_success() const { return outcome() == Outcome::Success; }
    bool is_failure() const { return outcome() == Outcome::Failure; }
    bool is_cancelled() const { return outcome() == Outcome::Cancelled; }
    const E& error() const { return require().error(); }
    void validate() const { require().validatePayload(); }
    std::size_t bytes() const { return require().bytes(); }
    std::size_t elements() const { return require().elements(); }
    // Interface copies refer to the same immutable result object. Converting a
    // typed Result creates a new object; it does not compare only status/error.
    bool operator==(const IResult&) const = default;

private:
    explicit IResult(std::shared_ptr<const Contract> value) : value_(std::move(value)) {}
    const Contract& require() const {
        if (!value_)
            throw ExecutionFailure("result interface is empty");
        return *value_;
    }
    std::shared_ptr<const Contract> value_;
};

template<class E>
void validate(const IResult<E>& result) { result.validate(); }

template<class E>
std::size_t ownedBytes(const IResult<E>& result) { return result.bytes(); }

template<class E>
std::size_t ownedElements(const IResult<E>& result) { return result.elements(); }
}
