// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <QObject>
#include <QString>
#include <QVariant>
#include <utility>

namespace pipelang::qt {
class InvocationOutcome {
    Q_GADGET
    Q_PROPERTY(Kind outcome READ outcome)
    Q_PROPERTY(QString code READ code)
    Q_PROPERTY(QVariant value READ value)
public:
    enum Kind { Success, Cancelled, Failure };
    Q_ENUM(Kind)
    InvocationOutcome() = default;
    static InvocationOutcome success(QVariant value = {}) {
        return InvocationOutcome(Success, {}, std::move(value));
    }
    static InvocationOutcome cancelled(QString code) {
        return InvocationOutcome(Cancelled, std::move(code), {});
    }
    static InvocationOutcome failure(QString code) {
        return InvocationOutcome(Failure, std::move(code), {});
    }
    Kind outcome() const { return outcome_; }
    QString code() const { return code_; }
    QVariant value() const { return value_; }
private:
    InvocationOutcome(Kind outcome, QString code, QVariant value)
        : outcome_(outcome), code_(std::move(code)), value_(std::move(value)) {}
    Kind outcome_ = Failure;
    QString code_ = QStringLiteral("qt.uninitialized");
    QVariant value_;
};
}
Q_DECLARE_METATYPE(pipelang::qt::InvocationOutcome)
