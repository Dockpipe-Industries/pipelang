// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include <pipelang/model/native_host.hpp>
#include <pipelang/di/CleanupStatus.hpp>

namespace pipelang::native_app_v19 {

class HostedComposition {
    struct Binding;
    struct Scope;
    struct ScopeLease;

public:
    struct Input {
        std::string_view service;
        HostedConstructorArgument argument;
    };

    class Service {
    public:
        bool sameObject(const Service& other) const;

    private:
        friend class HostedComposition;
        Service(const std::shared_ptr<Scope>& owner, std::size_t node, HostedValue value);

        std::weak_ptr<Scope> owner_;
        std::size_t node_;
        HostedValue value_;
    };

    class CleanupSession {
    public:
        model::Result<di::CleanupStatus> status() const;
        model::Task<model::Result<void>> task() const;

    private:
        friend class HostedComposition;
        CleanupSession(const std::shared_ptr<Binding>& binding, std::shared_ptr<Scope> scope);

        std::weak_ptr<Binding> binding_;
        std::shared_ptr<Scope> scope_;
    };

    using Argument = std::variant<HostedCallArgument, Service>;

    static model::Result<HostedComposition> bind(const HostedModel& descriptor,
                                                  std::span<const HostedConstructorArgument> modelArguments,
                                                  std::span<const Input> singletonInputs);
    model::Result<HostedComposition> createScope(std::span<const Input> inputs) const;
    model::Result<Service> resolve(std::string_view service) const;
    model::Result<Service> resolve(std::string_view service, model::CancellationToken cancellation) const;
    model::Result<model::Task<HostedValueResult>> tryStartMethod(
        std::string_view method, std::span<const Argument> arguments,
        model::CancellationToken cancellation = {}) const;
    model::Result<model::Task<HostedValueResult>> tryStartServiceMethod(
        const Service& service, std::string_view method, std::span<const Argument> arguments,
        model::CancellationToken cancellation = {}) const;
    model::Result<std::size_t> poll() const;
    model::Result<di::CleanupStatus> cleanupStatus() const;
    model::Result<CleanupSession> observeCleanup() const;
    model::Result<void> close() const;
    model::Result<model::Task<model::Result<void>>> closeAsync() const;
    model::Result<std::size_t> pending() const;

private:
    HostedComposition(std::shared_ptr<Binding> binding, std::shared_ptr<Scope> scope);

    model::Result<void> requireOpen() const;
    model::Result<void> requireAccessible(const Service& service) const;
    model::Result<model::Task<HostedValueResult>> tryStart(
        const Service* service, std::string_view method, std::span<const Argument> arguments,
        model::CancellationToken cancellation) const;

    std::shared_ptr<Binding> binding_;
    std::shared_ptr<Scope> scope_;
    std::shared_ptr<ScopeLease> lease_;
};

}
