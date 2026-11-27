// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ContextualObject.hpp"
#include <QHash>
#include <algorithm>
#include <vector>

namespace pipelang::qt {
class ContextObject final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool current READ current NOTIFY contextChanged)
    Q_PROPERTY(pipelang::qt::InvocationOutcome contextStatus READ contextStatus NOTIFY contextChanged)
public:
    static ContextObject* capture(const std::shared_ptr<::pipelang::model::ContextOwner>& owner, QObject* root) {
        ::pipelang::model::validate(owner);
        if (!root || root->thread() != QThread::currentThread())
            throw ::pipelang::model::ExecutionFailure("qt.wrong_owner");
        const auto lease = owner->capture();
        auto& values = captures();
        std::erase_if(values, [](const auto& value) { return !value; });
        for (const auto& value : values) {
            if (value->current() && value->root_ == root && value->owner_ == owner && value->lease_.sameIdentity(lease))
                return value.data();
        }
        auto* result = new ContextObject(owner, lease, root);
        if (result->current()) {
            result->setParent(root);
            QQmlEngine::setObjectOwnership(result, QQmlEngine::CppOwnership);
        } else {
            QQmlEngine::setObjectOwnership(result, QQmlEngine::JavaScriptOwnership);
        }
        values.push_back(result);
        return result;
    }

    static void invalidateSnapshot(ContextObject* value) {
        if (value)
            value->retire();
    }

    ~ContextObject() override {
        if (!onOwner())
            std::terminate();
        // Publication-owner destruction can notify descendants before QObject
        // clears its weak references. Do not expose partially destroyed caches.
        retired_ = true;
        subscription_.reset();
    }

    bool current() const {
        requireOwner();
        return !retired_ && root_ && lease_.is_current()
            && publicationLifetime_->capture().is_current();
    }
    InvocationOutcome contextStatus() const {
        return current() ? InvocationOutcome::success()
            : InvocationOutcome::cancelled(QStringLiteral("context.stale"));
    }

    Q_INVOKABLE QObject* model(QObject* prototype) {
        return bindModel(prototype, false);
    }

    Q_INVOKABLE QObject* controller(QObject* prototype) {
        return bindModel(prototype, true);
    }

private:
    QObject* bindModel(QObject* prototype, bool shared) {
        requireOwner();
        if (!current())
            return nullptr;
        auto* source = qobject_cast<ContextualObject*>(prototype);
        if (!source || source->thread() != thread())
            return nullptr;
        const auto origin = source->prototypeIdentity();
        std::erase_if(models_, [](const auto& value) { return !value.receiver; });
        for (const auto& value : models_) {
            if (value.origin == origin && value.shared == shared && value.receiver && value.receiver->current())
                return value.receiver.data();
        }
        ContextBinding binding;
        binding.owner = owner_;
        binding.lease = lease_;
        binding.permit = publicationLifetime_->capture().permit();
        binding.root = root_;
        try {
            auto* receiver = shared ? source->bindShared(std::move(binding)) : source->bindFresh(std::move(binding));
            if (!current() || !receiver->current())
                return nullptr;
            receiver->setParent(this);
            QQmlEngine::setObjectOwnership(receiver, QQmlEngine::CppOwnership);
            models_.push_back({origin, receiver, shared});
            return receiver;
        } catch (const ::pipelang::model::StaleContextCancelled&) {
            return nullptr;
        } catch (const ::pipelang::model::ExecutionFailure&) {
            return nullptr;
        }
    }

public:
    Q_INVOKABLE QObject* scope(const QString& slot, const QString& key) {
        try {
            return scopeCurrent(slot, key);
        } catch (const ::pipelang::model::OperationCancelled&) {
            return nullptr;
        } catch (const ::pipelang::model::ExecutionFailure&) {
            return nullptr;
        }
    }

private:
    QObject* scopeCurrent(const QString& slot, const QString& key) {
        requireOwner();
        if (!current() || slot.isEmpty() || slot.size() > 96 || key.size() > 1024 * 1024)
            return nullptr;
        auto found = scopes_.find(slot);
        if (found != scopes_.end() && found.value()->key == key && found.value()->snapshot && found.value()->snapshot->current())
            return found.value()->snapshot.data();
        std::shared_ptr<Scope> value;
        if (found == scopes_.end()) {
            if (scopes_.size() == 64)
                return nullptr;
            value = std::make_shared<Scope>();
            value->owner = std::make_shared<::pipelang::model::ContextOwner>(publicationLifetime_->capture().permit());
            scopes_.insert(slot, value);
        } else {
            // A repeated key after navigation obtains a new incarnation. The
            // previous snapshot is never refreshed or made authoritative again.
            value = found.value();
            QPointer<ContextObject> self(this);
            try {
                value->owner->replace();
            } catch (const ::pipelang::model::ExecutionFailure&) {
                return nullptr;
            }
            if (!self || !current())
                return nullptr;
        }
        value->key = key;
        value->snapshot = capture(value->owner, root_);
        return value->snapshot.data();
    }

Q_SIGNALS:
    void contextChanged();

private:
    struct BoundModel {
        std::shared_ptr<PrototypeIdentity> origin;
        QPointer<ContextualObject> receiver;
        bool shared = false;
    };
    struct Scope {
        QString key;
        std::shared_ptr<::pipelang::model::ContextOwner> owner;
        QPointer<ContextObject> snapshot;
    };

    ContextObject(std::shared_ptr<::pipelang::model::ContextOwner> owner, ::pipelang::model::ContextLease lease, QObject* root)
        : owner_(std::move(owner)), lease_(std::move(lease)),
          publicationLifetime_(std::make_unique<::pipelang::model::ContextOwner>(lease_.permit())),
          root_(root), thread_(QThread::currentThread()) {
        QObject::connect(root, &QObject::destroyed, this, [this] {
            const QPointer<ContextObject> self(this);
            const auto owner = owner_;
            try {
                owner->invalidate();
            } catch (const ::pipelang::model::ExecutionFailure&) {
                // It can already be revoking when destruction is reentrant.
            }
            if (!self)
                return;
            self->retire();
            if (self)
                self->deleteLater();
        });
        QPointer<ContextObject> snapshot(this);
        subscription_ = lease_.observeRevocation([snapshot] {
            if (snapshot)
                snapshot->retire();
        });
    }

    static std::vector<QPointer<ContextObject>>& captures() {
        static thread_local std::vector<QPointer<ContextObject>> values;
        return values;
    }

    bool onOwner() const noexcept { return QThread::currentThread() == thread_ && thread() == thread_; }
    void requireOwner() const {
        if (!onOwner())
            throw ::pipelang::model::ExecutionFailure("qt.wrong_owner");
    }

    void retire() {
        requireOwner();
        if (retired_)
            return;
        retired_ = true;
        subscription_.reset();
        const QPointer<ContextObject> self(this);
        // Closing publishes revocation synchronously. A projection callback
        // may destroy this snapshot and its root, so active lifetime state and
        // caches must be owned by this stack frame before notifying receivers.
        const auto publicationLifetime = std::move(publicationLifetime_);
        auto models = std::move(models_);
        auto scopes = std::move(scopes_);
        if (publicationLifetime->capture().is_current())
            publicationLifetime->close();
        if (!self)
            return;
        models.clear();
        scopes.clear();
        if (!self)
            return;
        setParent(nullptr);
        QQmlEngine::setObjectOwnership(this, QQmlEngine::JavaScriptOwnership);
        Q_EMIT contextChanged();
    }

    std::shared_ptr<::pipelang::model::ContextOwner> owner_;
    const ::pipelang::model::ContextLease lease_;
    std::unique_ptr<::pipelang::model::ContextOwner> publicationLifetime_;
    QPointer<QObject> root_;
    QThread* const thread_;
    ::pipelang::model::RevocationSubscription subscription_;
    std::vector<BoundModel> models_;
    QHash<QString, std::shared_ptr<Scope>> scopes_;
    bool retired_ = false;
};
}
