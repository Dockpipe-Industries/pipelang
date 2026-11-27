// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ContextBinding.hpp"
#include <QQmlEngine>
#include <QHash>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <type_traits>
#include <utility>

namespace pipelang::qt {
class ContextualObject : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString correlation READ correlation CONSTANT)
    Q_PROPERTY(bool current READ current NOTIFY contextChanged)
    Q_PROPERTY(bool operationPending READ operationPending NOTIFY contextChanged)
    Q_PROPERTY(pipelang::qt::InvocationOutcome contextStatus READ contextStatus NOTIFY contextChanged)
public:
    ~ContextualObject() override {
        if (!onOwner())
            std::terminate();
        subscription_.reset();
        pendingSubscription_.reset();
        if (binding_.registeredOperation && binding_.operation && binding_.operation->is_current())
            (void)binding_.operation->complete();
        notifyOperationRetired();
    }

    QString correlation() const { requireOwner(); return correlation_; }
    bool current() const { requireOwner(); return !retired_ && binding_.permit.is_current(); }
    bool operationPending() const {
        if (!current())
            return false;
        return binding_.operation ? binding_.operation->is_current()
            : pendingTicket_ && pendingTicket_->is_current();
    }
    InvocationOutcome contextStatus() const {
        requireOwner();
        return current() ? status_ : InvocationOutcome::cancelled(QStringLiteral("context.stale"));
    }

    std::shared_ptr<PrototypeIdentity> prototypeIdentity() const {
        requireOwner();
        return binding_.origin;
    }

    // ContextObject owns admission. Callers cannot substitute an arbitrary
    // token: the binding contains an actual owner lease and captured permit.
    ContextualObject* bindFresh(ContextBinding binding) const {
        requireOwner();
        binding.origin = binding_.origin;
        binding.permit.requireCurrent();
        ::pipelang::model::InvocationContextScope invocation(binding.permit);
        auto* receiver = freshReceiver(std::move(binding));
        QQmlEngine::setObjectOwnership(receiver, QQmlEngine::JavaScriptOwnership);
        return receiver;
    }

    ContextualObject* bindShared(ContextBinding binding) const {
        requireOwner();
        if (!current())
            throw ::pipelang::model::StaleContextCancelled();
        binding.origin = binding_.origin;
        binding.permit.requireCurrent();
        ::pipelang::model::InvocationContextScope invocation(binding.permit);
        auto* receiver = sharedReceiver(std::move(binding));
        QQmlEngine::setObjectOwnership(receiver, QQmlEngine::JavaScriptOwnership);
        return receiver;
    }

    Q_INVOKABLE QObject* latest() { return guardedBegin(::pipelang::model::OperationPolicy::LatestWins); }
    Q_INVOKABLE QObject* exclusive() { return guardedBegin(::pipelang::model::OperationPolicy::RejectBusy); }

    Q_INVOKABLE QObject* forOperation(QObject* target) {
        requireOwner();
        auto* receiver = qobject_cast<ContextualObject*>(target);
        if (!current() || !binding_.operation || !receiver || !receiver->current())
            return nullptr;
        if (!binding_.root || binding_.root != receiver->binding_.root ||
            binding_.permit.rootIdentity() != receiver->binding_.permit.rootIdentity() ||
            receiver->modelInvoking())
            return nullptr;
        try {
            auto binding = receiver->binding_;
            binding.operation = binding_.operation;
            binding.permit = ::pipelang::model::InvocationPermit::combine(binding_.permit, binding.permit);
            binding.correlation = correlation_;
            binding.registeredOperation = false;
            binding.supervisedRoot = false;
            auto* alias = receiver->sharedReceiver(std::move(binding));
            QQmlEngine::setObjectOwnership(alias, QQmlEngine::JavaScriptOwnership);
            return alias;
        } catch (const ::pipelang::model::OperationCancelled&) {
            return nullptr;
        } catch (const ::pipelang::model::ExecutionFailure&) {
            return nullptr;
        }
    }

    Q_INVOKABLE QObject* captureTarget(const QString& slot, QObject* target) {
        requireOwner();
        if (!current() || !binding_.operation || slot.isEmpty() || slot.size() > 96)
            return nullptr;
        const auto found = targets_.constFind(slot);
        if (found != targets_.cend()) {
            if (found->source != target || !found->alias || !found->alias->current() ||
                found->alias->binding_.supervisedRoot)
                return nullptr;
            return found->alias.data();
        }
        if (targets_.size() == 64)
            return nullptr;
        auto* alias = qobject_cast<ContextualObject*>(forOperation(target));
        if (!alias)
            return nullptr;
        alias->setParent(this);
        QQmlEngine::setObjectOwnership(alias, QQmlEngine::CppOwnership);
        targets_.insert(slot, CapturedTarget{target, alias});
        return alias;
    }

    // Capture admission to the permanent root owner, not a DTO publication
    // exemption. A synchronous root command may retire its own child contexts;
    // its captured alias still rejects stale entry and never refreshes a permit.
    // Ordinary operation/DTO aliases do not inherit this handoff.
    Q_INVOKABLE QObject* captureSupervisor(const QString& slot, QObject* rootPrototype) {
        requireOwner();
        if (!current() || !binding_.operation || slot.isEmpty() || slot.size() > 96)
            return nullptr;
        auto* receiver = qobject_cast<ContextualObject*>(rootPrototype);
        if (!receiver || !receiver->onOwner() || !receiver->current() ||
            rootPrototype != binding_.root.data() || receiver->binding_.permit.is_bound() ||
            receiver->binding_.operation || receiver->binding_.origin->prototype != receiver ||
            receiver->modelInvoking())
            return nullptr;
        const auto found = targets_.constFind(slot);
        if (found != targets_.cend()) {
            if (found->source != rootPrototype || !found->alias || !found->alias->current() ||
                !found->alias->binding_.supervisedRoot)
                return nullptr;
            return found->alias.data();
        }
        if (targets_.size() == 64)
            return nullptr;
        const QPointer<ContextualObject> self(this);
        try {
            auto binding = receiver->binding_;
            binding.owner = binding_.owner;
            binding.lease = binding_.lease;
            binding.permit = binding_.permit;
            binding.operation = binding_.operation;
            binding.lane = binding_.lane;
            binding.root = binding_.root;
            binding.correlation = correlation_;
            binding.registeredOperation = false;
            binding.supervisedRoot = true;
            auto* alias = receiver->sharedReceiver(std::move(binding));
            if (!self || !alias->current()) {
                alias->deleteLater();
                return nullptr;
            }
            alias->setParent(this);
            QQmlEngine::setObjectOwnership(alias, QQmlEngine::CppOwnership);
            targets_.insert(slot, CapturedTarget{rootPrototype, alias});
            return alias;
        } catch (const ::pipelang::model::OperationCancelled&) {
            return nullptr;
        } catch (const ::pipelang::model::ExecutionFailure&) {
            return nullptr;
        }
    }

    Q_INVOKABLE QObject* target(const QString& slot) const {
        requireOwner();
        if (!current() || !binding_.operation)
            return nullptr;
        const auto found = targets_.constFind(slot);
        if (found == targets_.cend() || !found->alias || !found->alias->current())
            return nullptr;
        return found->alias.data();
    }

    Q_INVOKABLE QObject* resolveOperation(const QString& correlation) {
        requireOwner();
        auto& operations = binding_.origin->operations;
        std::erase_if(operations, [](const auto& receiver) {
            return !receiver || !receiver->operationPending();
        });
        for (const auto& receiver : operations) {
            if (receiver->correlation_ == correlation)
                return receiver.data();
        }
        return nullptr;
    }

    Q_INVOKABLE InvocationOutcome finishOperation() {
        requireOwner();
        if (!current() || !binding_.operation)
            return InvocationOutcome::cancelled(QStringLiteral("context.stale_operation"));
        const auto ticket = *binding_.operation;
        const auto result = ticket.complete();
        if (result.is_success())
            return InvocationOutcome::success();
        return InvocationOutcome::cancelled(QStringLiteral("context.stale_operation"));
    }

    void refreshProjection() {
        requireOwner();
        if (current())
            projectCurrent();
    }

Q_SIGNALS:
    void contextChanged();
    void operationRetired(QString correlation);

protected:
    explicit ContextualObject(ContextBinding binding = {}, QObject* parent = nullptr)
        : QObject(parent), owner_(QThread::currentThread()), binding_(std::move(binding)) {
        if (!binding_.origin) {
            binding_.origin = std::make_shared<PrototypeIdentity>();
            binding_.origin->prototype = this;
        }
        if (binding_.operation)
            correlation_ = binding_.correlation.isEmpty()
                ? QUuid::createUuid().toString(QUuid::WithoutBraces) : binding_.correlation;
        qRegisterMetaType<InvocationOutcome>();
    }

    bool onOwner() const noexcept {
        return QThread::currentThread() == owner_ && thread() == owner_;
    }

    void requireOwner() const {
        if (!onOwner())
            throw ::pipelang::model::ExecutionFailure("qt.wrong_owner");
    }

    const ContextBinding& contextBinding() const noexcept { return binding_; }
    QObject* contextRoot() const noexcept {
        return binding_.root ? binding_.root.data() : const_cast<ContextualObject*>(this);
    }

    void deactivateBinding() noexcept {
        // QObject weak references remain live during derived member teardown.
        // Reentrant callbacks must fail admission before model storage dies.
        retired_ = true;
        subscription_.reset();
        pendingSubscription_.reset();
    }

    // Called after generated storage and projection members are initialized.
    void activateBinding() {
        requireOwner();
        if (binding_.root) {
            QObject::connect(binding_.root.data(), &QObject::destroyed, this, [this] {
                const QPointer<ContextualObject> self(this);
                retire();
                if (self)
                    self->deleteLater();
            });
        }
        if (binding_.permit.is_bound()) {
            QPointer<ContextualObject> receiver(this);
            subscription_ = binding_.permit.observeRevocation([receiver] {
                if (receiver)
                    receiver->retire();
            });
        }
        if (!binding_.permit.is_current())
            retire();
    }

    void retire() {
        requireOwner();
        if (retired_)
            return;
        retired_ = true;
        const QPointer<ContextualObject> self(this);
        subscription_.reset();
        pendingSubscription_.reset();
        pendingTicket_.reset();
        targets_.clear();
        status_ = InvocationOutcome::cancelled(QStringLiteral("context.stale"));
        releaseModel();
        if (!self)
            return;
        clearProjection();
        if (!self)
            return;
        // The binding keeps only revocable identity, never model data. Keeping
        // its owner until QObject destruction also avoids destructor reentry in
        // the middle of that owner's revocation notification.
        setParent(nullptr);
        QQmlEngine::setObjectOwnership(this, QQmlEngine::JavaScriptOwnership);
        notifyOperationRetired();
        if (!self)
            return;
        Q_EMIT contextChanged();
    }

    virtual ContextualObject* freshReceiver(ContextBinding binding) const = 0;
    virtual ContextualObject* sharedReceiver(ContextBinding binding) const = 0;
    virtual bool modelInvoking() const noexcept = 0;
    virtual void projectCurrent() = 0;
    virtual void clearProjection() = 0;
    virtual void releaseModel() = 0;

private:
    QObject* guardedBegin(::pipelang::model::OperationPolicy policy) {
        if (!onOwner())
            return nullptr;
        try {
            return beginOperation(policy);
        } catch (const ::pipelang::model::OperationCancelled&) {
            status_ = InvocationOutcome::cancelled(QStringLiteral("context.stale"));
        } catch (const ::pipelang::model::ExecutionFailure& error) {
            status_ = InvocationOutcome::failure(QString::fromUtf8(error.what()));
        }
        Q_EMIT contextChanged();
        return nullptr;
    }

    QObject* beginOperation(::pipelang::model::OperationPolicy policy) {
        requireOwner();
        if (!current()) {
            status_ = InvocationOutcome::cancelled(QStringLiteral("context.stale"));
            Q_EMIT contextChanged();
            return nullptr;
        }
        if (binding_.operation) {
            status_ = InvocationOutcome::failure(QStringLiteral("context.already_operation_bound"));
            Q_EMIT contextChanged();
            return nullptr;
        }
        if (modelInvoking()) {
            status_ = InvocationOutcome::failure(QStringLiteral("qt.reentry"));
            Q_EMIT contextChanged();
            return nullptr;
        }
        if (!binding_.owner) {
            // Unbound native constructors retain their ordinary behavior. A
            // private root lane is needed only if the prototype starts an op.
            binding_.owner = std::make_shared<::pipelang::model::ContextOwner>();
            binding_.lease = binding_.owner->capture();
        }
        if (!binding_.lane)
            binding_.lane = std::make_shared<::pipelang::model::OperationLane>(*binding_.owner);
        const QPointer<ContextualObject> self(this);
        const auto lane = binding_.lane;
        const auto admitted = lane->begin(binding_.lease, policy);
        if (!self)
            return nullptr;
        if (!current())
            return nullptr;
        if (!admitted.is_success()) {
            status_ = admitted.is_cancelled()
                ? InvocationOutcome::cancelled(QStringLiteral("context.stale"))
                : InvocationOutcome::failure(QString::fromStdString(admitted.error()));
            Q_EMIT contextChanged();
            return nullptr;
        }
        pendingTicket_ = admitted.value();
        QPointer<ContextualObject> pendingOwner(this);
        pendingSubscription_ = admitted.value().observeRevocation([pendingOwner] {
            if (pendingOwner) {
                pendingOwner->pendingTicket_.reset();
                Q_EMIT pendingOwner->contextChanged();
            }
        });
        auto binding = binding_;
        binding.operation = admitted.value();
        binding.permit = ::pipelang::model::InvocationPermit::combine(admitted.value().permit(), binding_.permit);
        binding.correlation.clear();
        binding.registeredOperation = true;
        auto* receiver = sharedReceiver(std::move(binding));
        if (!receiver->current()) {
            receiver->deleteLater();
            return nullptr;
        }
        receiver->setParent(binding_.origin->prototype.data());
        QQmlEngine::setObjectOwnership(receiver, QQmlEngine::CppOwnership);
        auto& operations = binding_.origin->operations;
        std::erase_if(operations, [](const auto& value) {
            return !value || !value->operationPending();
        });
        operations.push_back(receiver);
        status_ = InvocationOutcome::success();
        Q_EMIT contextChanged();
        return receiver;
    }

    void notifyOperationRetired() {
        if (!binding_.registeredOperation || retirementNotified_)
            return;
        retirementNotified_ = true;
        std::erase_if(binding_.origin->operations, [](const auto& receiver) {
            return !receiver || !receiver->operationPending();
        });
        if (binding_.origin->prototype)
            Q_EMIT binding_.origin->prototype->operationRetired(correlation_);
    }

    struct CapturedTarget {
        QPointer<QObject> source;
        QPointer<ContextualObject> alias;
    };

    QHash<QString, CapturedTarget> targets_;
    QThread* const owner_;
    ContextBinding binding_;
    ::pipelang::model::RevocationSubscription subscription_;
    ::pipelang::model::RevocationSubscription pendingSubscription_;
    std::optional<::pipelang::model::OperationTicket> pendingTicket_;
    QString correlation_;
    InvocationOutcome status_ = InvocationOutcome::success();
    bool retired_ = false;
    bool retirementNotified_ = false;
};

// Each operation receiver shares one actual model and one reentry guard with
// its context receiver. Observers are weak and pruned on every publication.
template<class Model>
struct ContextModelGroup {
    explicit ContextModelGroup(std::shared_ptr<Model> value) : model(std::move(value)) {
        if (!model)
            throw ::pipelang::model::ExecutionFailure("qt.null_model");
    }

    void publish() {
        std::erase_if(observers, [](const auto& receiver) { return !receiver || !receiver->current(); });
        const auto receivers = observers;
        for (const auto& receiver : receivers) {
            if (receiver && receiver->current())
                receiver->refreshProjection();
        }
    }

    std::shared_ptr<Model> model;
    bool invoking = false;
    std::vector<QPointer<ContextualObject>> observers;
};

template<class Model>
std::shared_ptr<Model> freshContextModel() {
    if constexpr (std::is_default_constructible_v<Model>)
        return std::make_shared<Model>();
    else
        throw ::pipelang::model::ExecutionFailure("qt.context_model_requires_default_constructor");
}
}
