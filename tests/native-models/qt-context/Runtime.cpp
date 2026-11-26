// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ContextStateQt.hpp"
#include <QCoreApplication>
#include <QEvent>
#include <QJSValue>
#include <QQmlEngine>
#include <iostream>
#include <memory>
#include <vector>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

namespace {
using Adapter = qt_context_binding::ContextStateQt;
using Model = qt_context_fixture::ContextState;
using pipelang::model::ContextOwner;
using pipelang::qt::ContextObject;
using pipelang::qt::ContextualObject;
using pipelang::qt::InvocationOutcome;

Adapter* adapter(QObject* value) {
    auto* result = qobject_cast<Adapter*>(value);
    assert(result);
    return result;
}

ContextObject* context(QObject* value) {
    auto* result = qobject_cast<ContextObject*>(value);
    assert(result);
    return result;
}

void success(const pipelang::qt::ActionReport& report) {
    assert(report.operation().outcome() == InvocationOutcome::Success);
    assert(report.projection().outcome() == InvocationOutcome::Success);
}

void evaluate(QQmlEngine& engine, const QString& source) {
    const auto result = engine.evaluate(source);
    if (result.isError())
        std::cerr << result.toString().toStdString() << '\n';
    assert(!result.isError() && result.toBool());
}

void runtimeAndGarbageCollection(QQmlEngine& engine) {
    QObject root;
    Adapter readPrototype(std::make_unique<Model>(), &root);
    Adapter pagePrototype(std::make_unique<Model>(), &root);
    Adapter foreignPrototype(std::make_unique<Model>(), &root);
    const auto owner = std::make_shared<ContextOwner>();
    auto* snapshot = ContextObject::capture(owner, &root);
    assert(snapshot == ContextObject::capture(owner, &root));
    assert(snapshot->parent() == &root);

    auto* read = adapter(snapshot->model(&readPrototype));
    auto* page = adapter(snapshot->model(&pagePrototype));
    assert(read == snapshot->model(&readPrototype));
    assert(read->parent() == snapshot && page->parent() == snapshot);
    success(page->Set(QStringLiteral("private value")));
    assert(pagePrototype.pl_get_Text().isEmpty());

    // Context snapshots and their current models are natively retained. A
    // wrapper GC must not recreate current state or lose a live operation.
    engine.globalObject().setProperty(QStringLiteral("ctx"), engine.newQObject(snapshot));
    engine.globalObject().setProperty(QStringLiteral("prototype"), engine.newQObject(&pagePrototype));
    evaluate(engine, QStringLiteral("ctx.model(prototype).Text === 'private value'"));
    engine.collectGarbage();
    assert(snapshot->model(&pagePrototype) == page);

    std::vector<QString> retired;
    QObject::connect(&readPrototype, &ContextualObject::operationRetired, &root,
        [&](const QString& correlation) { retired.push_back(correlation); });
    auto* first = adapter(read->latest());
    const auto firstTag = first->correlation();
    assert(!firstTag.isEmpty() && first->parent() == &readPrototype);
    assert(read->operationPending());
    assert(!read->exclusive() && read->contextStatus().code() == QStringLiteral("context.busy"));
    engine.collectGarbage();
    assert(readPrototype.resolveOperation(firstTag) == first);
    assert(!foreignPrototype.resolveOperation(firstTag));

    auto* replacement = adapter(read->latest());
    assert(replacement->correlation() != firstTag);
    assert(!first->current() && first->pl_get_Text().isEmpty() && !first->parent());
    assert(retired.size() == 1 && retired[0] == firstTag);
    assert(!readPrototype.resolveOperation(firstTag));
    assert(first->finishOperation().outcome() == InvocationOutcome::Cancelled);

    auto* decode = adapter(replacement->captureTarget(QStringLiteral("page"), page));
    assert(decode->correlation() == replacement->correlation() && decode->parent() == replacement);
    assert(replacement->target(QStringLiteral("page")) == decode);
    assert(replacement->captureTarget(QStringLiteral("page"), page) == decode);
    assert(!replacement->captureTarget(QStringLiteral("page"), read));
    success(decode->Begin());
    success(decode->Append(QStringLiteral("one")));
    success(decode->Append(QStringLiteral("two")));
    success(decode->Commit(QStringLiteral("complete")));
    assert(page->pl_get_Rows() == QStringList({QStringLiteral("one"), QStringLiteral("two")}));
    assert(page->pl_get_Text() == QStringLiteral("complete"));
    assert(decode->current() && replacement->current());
    const auto completedTag = replacement->correlation();
    assert(replacement->finishOperation().outcome() == InvocationOutcome::Success);
    assert(!replacement->current() && !decode->current() && !read->operationPending());
    assert(!readPrototype.resolveOperation(completedTag) && retired.size() == 2);
    assert(page->current() && page->pl_get_Text() == QStringLiteral("complete"));

    auto* profile = adapter(snapshot->controller(&pagePrototype));
    success(profile->Set(QStringLiteral("controller state")));
    assert(pagePrototype.pl_get_Text() == QStringLiteral("controller state"));

    auto* entityA = context(snapshot->scope(QStringLiteral("entity"), QStringLiteral("A")));
    auto* oldPageA = adapter(entityA->model(&pagePrototype));
    auto* oldReadA = adapter(entityA->model(&readPrototype));
    success(oldPageA->Set(QStringLiteral("old A data")));
    auto* oldOperationA = adapter(oldReadA->latest());
    const auto oldATag = oldOperationA->correlation();
    auto* oldAliasA = adapter(oldOperationA->captureTarget(QStringLiteral("page"), oldPageA));
    auto* entityB = context(snapshot->scope(QStringLiteral("entity"), QStringLiteral("B")));
    assert(!entityA->current() && entityB->current());
    assert(!oldPageA->current() && oldPageA->pl_get_Text().isEmpty());
    assert(!oldAliasA->current() && !readPrototype.resolveOperation(oldATag));
    auto* entityANext = context(snapshot->scope(QStringLiteral("entity"), QStringLiteral("A")));
    auto* nextPageA = adapter(entityANext->model(&pagePrototype));
    assert(entityANext != entityA && nextPageA != oldPageA && nextPageA->pl_get_Text().isEmpty());
    assert(!oldOperationA->forOperation(nextPageA));
    assert(!oldOperationA->target(QStringLiteral("page")));
    assert(!oldOperationA->captureTarget(QStringLiteral("page"), nextPageA));
    assert(oldAliasA->Append(QStringLiteral("late chunk")).operation().outcome() == InvocationOutcome::Cancelled);
    assert(nextPageA->pl_get_Rows().isEmpty());

    // A response pipeline cannot borrow an unrelated lifetime root.
    const auto foreignOwner = std::make_shared<ContextOwner>();
    auto* foreignContext = ContextObject::capture(foreignOwner, &root);
    auto* foreignPage = adapter(foreignContext->model(&foreignPrototype));
    auto* active = adapter(read->latest());
    assert(!active->forOperation(foreignPage));

    engine.globalObject().setProperty(QStringLiteral("oldPage"), engine.newQObject(page));
    engine.globalObject().setProperty(QStringLiteral("oldContext"), engine.newQObject(snapshot));
    owner->replace();
    assert(!snapshot->current() && !snapshot->parent());
    assert(!page->current() && page->pl_get_Text().isEmpty() && page->pl_get_Rows().isEmpty());
    assert(!active->current() && !readPrototype.resolveOperation(active->correlation()));
    evaluate(engine, QStringLiteral("!oldContext.current && !oldPage.current && oldPage.Text === '' && oldPage.Rows.length === 0"));
    assert(pagePrototype.pl_get_Text() == QStringLiteral("controller state"));
    auto* current = ContextObject::capture(owner, &root);
    auto* newPage = adapter(current->model(&pagePrototype));
    assert(newPage->current() && newPage->pl_get_Text().isEmpty());
    engine.globalObject().deleteProperty(QStringLiteral("ctx"));
    engine.globalObject().deleteProperty(QStringLiteral("prototype"));
    engine.globalObject().deleteProperty(QStringLiteral("oldPage"));
    engine.globalObject().deleteProperty(QStringLiteral("oldContext"));
    engine.collectGarbage();
}

void reentryAndRevocation(QQmlEngine&) {
    QObject root;
    Adapter prototype(std::make_unique<Model>(), &root);
    const auto owner = std::make_shared<ContextOwner>();
    auto* snapshot = ContextObject::capture(owner, &root);
    auto* page = adapter(snapshot->model(&prototype));
    auto* operation = adapter(page->latest());
    bool called = false;
    QObject::connect(page, &Adapter::projectionChanged, &root, [&] {
        if (called)
            return;
        called = true;
        const auto query = page->Confirm(QStringLiteral("committed before notification"));
        assert(query.operation().outcome() == InvocationOutcome::Success);
        assert(query.operation().value().toBool());
        assert(query.projection().outcome() == InvocationOutcome::Success);
        const auto nested = operation->Set(QStringLiteral("reentrant value"));
        assert(nested.operation().code() == QStringLiteral("qt.reentry"));
        owner->invalidate();
        assert(!page->current() && page->pl_get_Text().isEmpty());
    });
    const auto report = operation->Set(QStringLiteral("committed before notification"));
    assert(called);
    // Publication finished before projection notification. An authoritative
    // successful operation stays successful when its UI projection expires.
    assert(report.operation().outcome() == InvocationOutcome::Success);
    assert(report.projection().outcome() == InvocationOutcome::Cancelled);
    assert(!operation->current() && !page->current());
}

void projectionFailureDoesNotRefreshOldAuthority() {
    QObject root;
    Adapter controller(std::make_unique<Model>(), &root);
    Adapter pagePrototype(std::make_unique<Model>(), &root);
    auto* original = context(controller.pl_get_Lifetime());
    auto* oldPage = adapter(original->model(&pagePrototype));
    success(oldPage->Set(QStringLiteral("private projection")));
    auto* oldOperation = adapter(oldPage->latest());
    const auto oldCorrelation = oldOperation->correlation();
    const auto failed = controller.FailProjection();
    assert(failed.operation().outcome() == InvocationOutcome::Success);
    assert(failed.projection().outcome() == InvocationOutcome::Failure);
    assert(!original->current() && !oldPage->current() && !oldOperation->current());
    assert(oldPage->pl_get_Text().isEmpty() && !pagePrototype.resolveOperation(oldCorrelation));
    success(controller.RecoverProjection());
    auto* replacement = context(controller.pl_get_Lifetime());
    auto* nextPage = adapter(replacement->model(&pagePrototype));
    assert(replacement != original && replacement->current());
    assert(nextPage->current() && nextPage->pl_get_Text().isEmpty());
    assert(oldOperation->FailProjection().operation().outcome() == InvocationOutcome::Cancelled);
    assert(nextPage->current() && replacement->current());
    assert(!oldOperation->forOperation(nextPage));
}

void supervisorCommandsKeepAdmissionWithoutSelfRevocationRollback() {
    QObject observer;
    Adapter root(std::make_unique<Model>(), &observer);
    Adapter requestPrototype(std::make_unique<Model>(), &observer);
    success(root.Set(QStringLiteral("before handoff")));
    auto* snapshot = context(root.pl_get_Lifetime());
    auto* strictController = adapter(snapshot->controller(&root));
    auto* strict = adapter(strictController->latest());
    const auto refused = strict->ReplaceAndSetLifetime(QStringLiteral("must roll back"));
    assert(refused.operation().outcome() == InvocationOutcome::Cancelled);
    assert(root.pl_get_Text() == QStringLiteral("before handoff"));

    snapshot = context(root.pl_get_Lifetime());
    auto* requester = adapter(snapshot->model(&requestPrototype));
    auto* operation = adapter(requester->latest());
    assert(!operation->captureSupervisor(QStringLiteral("other"), &requestPrototype));
    assert(!operation->captureSupervisor(QStringLiteral("bound"), requester));
    auto* supervisor = adapter(operation->captureSupervisor(QStringLiteral("root"), &root));
    assert(supervisor == operation->target(QStringLiteral("root")));
    assert(supervisor == operation->captureSupervisor(QStringLiteral("root"), &root));
    assert(!operation->captureTarget(QStringLiteral("root"), &root));
    const auto committed = supervisor->ReplaceAndSetLifetime(QStringLiteral("committed root command"));
    assert(committed.operation().outcome() == InvocationOutcome::Success);
    assert(committed.projection().outcome() == InvocationOutcome::Cancelled);
    assert(root.pl_get_Text() == QStringLiteral("committed root command"));
    assert(!operation->current() && !supervisor->current());
    assert(supervisor->Set(QStringLiteral("stale root command")).operation().outcome() == InvocationOutcome::Cancelled);
    assert(root.pl_get_Text() == QStringLiteral("committed root command"));

    // A strict DTO alias of a supervisor remains strict; copying its receiver
    // must never copy the explicit root-command publication handoff.
    snapshot = context(root.pl_get_Lifetime());
    requester = adapter(snapshot->model(&requestPrototype));
    operation = adapter(requester->latest());
    supervisor = adapter(operation->captureSupervisor(QStringLiteral("root"), &root));
    auto* downgraded = adapter(operation->forOperation(supervisor));
    const auto strictResult = downgraded->ReplaceAndSetLifetime(QStringLiteral("strict alias must roll back"));
    assert(strictResult.operation().outcome() == InvocationOutcome::Cancelled);
    assert(root.pl_get_Text() == QStringLiteral("committed root command"));
}

void rootCanBeDestroyedDuringRevocation(bool retireSnapshot) {
    QObject observer;
    auto root = std::make_unique<QObject>();
    auto* prototype = new Adapter(std::make_unique<Model>(), root.get());
    const auto owner = std::make_shared<ContextOwner>();
    QPointer<ContextObject> snapshot = ContextObject::capture(owner, root.get());
    QPointer<Adapter> receiver = adapter(snapshot->model(prototype));
    success(receiver->Set(QStringLiteral("private before destruction")));
    QPointer<Adapter> operation = adapter(receiver->latest());
    QPointer<Adapter> origin = prototype;
    int notifications = 0;
    auto* sender = retireSnapshot ? receiver.data() : operation.data();
    QObject::connect(sender, &Adapter::projectionChanged, &observer, [&] {
        ++notifications;
        if (root)
            root.reset();
    });
    if (retireSnapshot) {
        ContextObject::invalidateSnapshot(snapshot.data());
    } else {
        const auto completed = operation->finishOperation();
        assert(completed.outcome() == InvocationOutcome::Success);
    }
    assert(notifications == 1 && !root && !origin);
    assert(!owner->capture().is_current());
    if (receiver)
        assert(!receiver->current() && receiver->pl_get_Text().isEmpty());
    if (operation)
        assert(!operation->current() && operation->pl_get_Text().isEmpty());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    assert(!snapshot && !receiver && !operation);
}

void generatedDestructionRejectsReentryBeforeQObjectTeardown() {
    QObject observer;
    auto root = std::make_unique<Adapter>(std::make_unique<Model>());
    Adapter childPrototype(std::make_unique<Model>(), &observer);
    QPointer<Adapter> destroyingRoot = root.get();
    auto* snapshot = context(root->pl_get_Lifetime());
    QPointer<Adapter> child = adapter(snapshot->model(&childPrototype));
    success(child->Set(QStringLiteral("private descendant")));
    int notifications = 0;
    QObject::connect(child.data(), &Adapter::projectionChanged, &observer, [&] {
        ++notifications;
        // Model member destruction precedes QObject base destruction, so the
        // weak reference is intentionally still live at this callback boundary.
        assert(destroyingRoot && !destroyingRoot->current());
        const auto rejected = destroyingRoot->Set(QStringLiteral("must not reach model storage"));
        assert(rejected.operation().outcome() == InvocationOutcome::Cancelled);
        assert(destroyingRoot->pl_get_Text().isEmpty());
    });
    root.reset();
    assert(notifications == 1 && !destroyingRoot);
    if (child)
        assert(!child->current() && child->pl_get_Text().isEmpty());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    assert(!child);
}

void snapshotDestructionRejectsCacheReentry() {
    QObject root;
    QObject observer;
    Adapter prototype(std::make_unique<Model>(), &observer);
    const auto owner = std::make_shared<ContextOwner>();
    QPointer<ContextObject> snapshot = ContextObject::capture(owner, &root);
    QPointer<Adapter> receiver = adapter(snapshot->model(&prototype));
    int notifications = 0;
    QObject::connect(receiver.data(), &Adapter::projectionChanged, &observer, [&] {
        ++notifications;
        assert(snapshot && !snapshot->current());
        assert(!snapshot->model(&prototype));
        assert(!snapshot->scope(QStringLiteral("destroying"), QStringLiteral("scope")));
    });
    delete snapshot.data();
    assert(notifications == 1 && !snapshot);
    if (receiver)
        assert(!receiver->current());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void sourceLifetimeAndRootCleanup() {
    std::shared_ptr<ContextOwner> retained;
    {
        Model model;
        retained = model.Lifetime();
        assert(retained->capture().is_current());
    }
    assert(!retained->capture().is_current());

    const auto owner = std::make_shared<ContextOwner>();
    QPointer<ContextObject> snapshot;
    QPointer<Adapter> receiver;
    {
        QObject root;
        Adapter prototype(std::make_unique<Model>(), &root);
        snapshot = ContextObject::capture(owner, &root);
        receiver = adapter(snapshot->model(&prototype));
        success(receiver->Set(QStringLiteral("root private data")));
    }
    assert(!owner->capture().is_current());
    if (receiver)
        assert(!receiver->current() && receiver->pl_get_Text().isEmpty());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    assert(!snapshot && !receiver);
}
}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QQmlEngine engine;
    runtimeAndGarbageCollection(engine);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    reentryAndRevocation(engine);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    projectionFailureDoesNotRefreshOldAuthority();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    supervisorCommandsKeepAdmissionWithoutSelfRevocationRollback();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    rootCanBeDestroyedDuringRevocation(true);
    rootCanBeDestroyedDuringRevocation(false);
    generatedDestructionRejectsReentryBeforeQObjectTeardown();
    snapshotDestructionRejectsCacheReentry();
    sourceLifetimeAndRootCleanup();
    engine.collectGarbage();
    std::cout << "Qt context runtime assertions passed\n";
}
