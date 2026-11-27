// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#ifdef PIPELANG_CONTEXT_CPP_REFERENCE
#include "Model.hpp"
#else
#include <pipelang/model/native_host.hpp>
#endif

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

using pipelang::model::ContextOwner;
using pipelang::model::InvocationContextScope;

#ifndef PIPELANG_CONTEXT_CPP_REFERENCE
using namespace pipelang::native_app_v19;
extern "C" const HostedModel proof_hosted_context_owner;
#endif

void require(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string(message));
}

class Session {
public:
    Session() = default;

    auto lifetime()
    {
#ifdef PIPELANG_CONTEXT_CPP_REFERENCE
        return instance_.Lifetime();
#else
        auto result = instance_.startContextOwner(scope_).result();
        require(result.is_success(), result.is_success() ? "" : result.error());
        return result.value();
#endif
    }

    auto replace()
    {
#ifdef PIPELANG_CONTEXT_CPP_REFERENCE
        return instance_.Replace();
#else
        return invoke("Replace");
#endif
    }

    auto invalidate()
    {
#ifdef PIPELANG_CONTEXT_CPP_REFERENCE
        return instance_.Invalidate();
#else
        return invoke("Invalidate");
#endif
    }

    auto invalidateThenFail()
    {
#ifdef PIPELANG_CONTEXT_CPP_REFERENCE
        return instance_.InvalidateThenFail();
#else
        return invoke("InvalidateThenFail");
#endif
    }

    auto close()
    {
#ifdef PIPELANG_CONTEXT_CPP_REFERENCE
        return instance_.Close();
#else
        return invoke("Close");
#endif
    }

    std::int64_t changes()
    {
#ifdef PIPELANG_CONTEXT_CPP_REFERENCE
        return instance_.snapshot().changes;
#else
        auto result = invoke("Changes");
        require(result.is_success(), result.is_success() ? "" : result.error());
        return result.value().integer();
#endif
    }

    auto createChild()
    {
#ifdef PIPELANG_CONTEXT_CPP_REFERENCE
        return instance_.CreateChild();
#else
        return invoke("CreateChild");
#endif
    }

    auto dropChild()
    {
#ifdef PIPELANG_CONTEXT_CPP_REFERENCE
        return instance_.DropChild();
#else
        return invoke("DropChild");
#endif
    }

    auto createIncompleteChild()
    {
#ifdef PIPELANG_CONTEXT_CPP_REFERENCE
        return instance_.CreateIncompleteChild();
#else
        return invoke("CreateIncompleteChild");
#endif
    }

    auto childLifetime()
    {
#ifdef PIPELANG_CONTEXT_CPP_REFERENCE
        return instance_.ChildLifetime();
#else
        auto result = instance_.startContextOwner(scope_, "ChildLifetime").result();
        require(result.is_success(), result.is_success() ? "" : result.error());
        return result.value();
#endif
    }

    void verifyLocalOperations()
    {
        for (std::int64_t scenario : {0, 1}) {
#ifdef PIPELANG_CONTEXT_CPP_REFERENCE
            const auto result = instance_.Exercise(scenario);
            require(result.is_success() && result.value() == 42, "local context operations changed");
#else
            const std::array<HostedCallArgument, 1> arguments{scenario};
            const auto result = instance_.startMethod(scope_, "Exercise", arguments).result();
            require(result.is_success() && result.value().integer() == 42,
                    result.is_success() ? "local context operations changed" : result.error());
#endif
        }
    }

#ifndef PIPELANG_CONTEXT_CPP_REFERENCE
    void verifyTypedBoundary()
    {
        auto wrongGetter = instance_.startContextOwner(scope_, "Changes").result();
        require(wrongGetter.is_failure(), "typed context getter accepted an integer method");
        auto genericGetter = invoke("Lifetime");
        require(genericGetter.is_failure(), "generic host projection exported a context owner");
        require(genericGetter.error().find("startContextOwner") != std::string::npos, "missing context getter guidance");

        bool refused = false;
        std::thread other([&] {
            pipelang::model::TaskScope otherScope;
            const auto result = instance_.startContextOwner(otherScope).result();
            refused = result.is_failure() && result.error() == "context.wrong_owner_thread";
        });
        other.join();
        require(refused, "context getter transferred ownership across threads");
    }
#endif

private:
#ifdef PIPELANG_CONTEXT_CPP_REFERENCE
    reference::source::NativeProof::HostedContextOwner::ContextOwnerModel instance_;
#else
    HostedValueResult invoke(std::string_view name)
    {
        return instance_.startMethod(scope_, name, {}).result();
    }

    static HostedInstance bind()
    {
        auto result = HostedInstance::bind(proof_hosted_context_owner, {});
        require(result.is_success(), result.is_success() ? "" : result.error());
        return std::move(result).value();
    }

    HostedInstance instance_ = bind();
    pipelang::model::TaskScope scope_;
#endif
};

int main()
{
    try {
        Session revoking;
        const auto lease = revoking.lifetime()->capture();
        {
            const InvocationContextScope invocation(lease.permit());
            const auto refused = revoking.invalidate();
            require(refused.is_cancelled() && refused.error() == "context.stale",
                    "invalidated context published its active segment");
        }
        require(!lease.is_current(), "publication rollback restored revoked authority");
        require(revoking.changes() == 0, "revoked segment committed its receiver writes");

        Session first;
        Session second;
        const auto owner = first.lifetime();
        require(owner && owner == first.lifetime(), "getter changed context identity");
        require(owner != second.lifetime(), "independent receivers shared a context owner");
        const auto initial = owner->capture();
        require(initial.is_current(), "initial context is stale");
        require(first.replace().is_success(), "replacement failed");
        require(!initial.is_current() && initial.cancellation().is_cancelled(), "replacement kept old authority");
        const auto replacement = owner->capture();
        require(replacement.is_current(), "replacement context is stale");
        require(first.invalidate().is_success(), "invalidation failed");
        require(!replacement.is_current() && !owner->capture().is_current(), "invalidation kept authority");
        require(first.changes() == 2, "successful changes were lost");
        require(first.replace().is_success(), "replacement after invalidation failed");
        const auto next = owner->capture();
        require(next.is_current() && !initial.is_current(), "replacement revived old authority");

        const auto failed = first.invalidateThenFail();
        require(failed.is_failure() && failed.error() == "source failure", "source failure changed");
        require(!next.is_current() && !owner->capture().is_current(), "rollback restored revoked authority");
        require(first.changes() == 3, "rollback committed scalar state");
        require(first.close().is_success() && first.close().is_success(), "close was not idempotent");
        const auto closed = first.replace();
        require(closed.is_failure() && closed.error() == "context.closed", "closed context allowed replacement");
        require(second.lifetime()->capture().is_current(), "operation affected an independent receiver");

        std::shared_ptr<ContextOwner> retained;
        {
            Session temporary;
            retained = temporary.lifetime();
            require(retained->capture().is_current(), "temporary receiver context is stale");
        }
        require(!retained->capture().is_current(), "host retention extended source lifetime");

        second.verifyLocalOperations();
        const auto firstChild = second.createChild();
        require(firstChild.is_success(), firstChild.is_success() ? "" : firstChild.error());
        const auto retainedChild = second.childLifetime();
        require(retainedChild && retainedChild->capture().is_current(), "dynamic context is stale");
        for (int count = 0; count < 4200; ++count) {
            const auto incomplete = second.createIncompleteChild();
            require(incomplete.is_failure() && incomplete.error() == "context.null_owner",
                    incomplete.is_success() ? "constructor accepted an uninitialized context owner" : incomplete.error());
            require(second.dropChild().is_success(), "dynamic receiver cleanup failed");
            const auto created = second.createChild();
            require(created.is_success(), created.is_success() ? "" : created.error());
        }
        require(second.childLifetime() != retainedChild, "new receiver reused a retained context identity");

        ContextOwner parent;
        std::unique_ptr<Session> child;
        {
            InvocationContextScope invocation(parent.capture().permit());
            child = std::make_unique<Session>();
        }
        const auto childOwner = child->lifetime();
        require(childOwner->capture().is_current(), "child context is stale");
        parent.invalidate();
        require(!childOwner->capture().is_current(), "parent revocation did not reach child");
        require(child->replace().is_success(), "child replacement after parent revocation failed");
        require(!childOwner->capture().is_current(), "child replacement restored revoked parent authority");

#ifndef PIPELANG_CONTEXT_CPP_REFERENCE
        first.verifyTypedBoundary();
#endif
        std::cout << "Context ownership preserves identity, rollback revocation, and parent lifetime.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
