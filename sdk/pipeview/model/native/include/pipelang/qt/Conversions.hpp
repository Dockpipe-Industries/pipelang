// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ActionReport.hpp"
#include <pipelang/model/runtime.hpp>
#include <QStringList>
#include <limits>

namespace pipelang::qt {
inline std::string fromQt(const QString& value) {
    if (value.size() > 1024 * 1024)
        throw model::ExecutionFailure("qt.invalid_text");
    for (qsizetype index = 0; index < value.size(); ++index) {
        const auto character = value.at(index);
        if (character.isHighSurrogate()) {
            if (++index == value.size() || !value.at(index).isLowSurrogate())
                throw model::ExecutionFailure("qt.invalid_text");
        } else if (character.isLowSurrogate()) {
            throw model::ExecutionFailure("qt.invalid_text");
        }
    }
    const auto encoded = value.toUtf8();
    std::string result(encoded.constData(), static_cast<std::size_t>(encoded.size()));
    model::validate(result);
    return result;
}
inline bool fromQt(bool value) { return value; }
inline model::List<std::string> fromQt(const QStringList& values) {
    if (values.size() > 4096)
        throw model::ExecutionFailure("qt.invalid_list");
    auto result = model::List<std::string>::Empty();
    for (const auto& value : values)
        result.add(fromQt(value));
    return result;
}
inline QString toQt(const std::string& value) {
    model::validate(value);
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}
inline bool toQt(bool value) { return value; }
inline QStringList toQt(const model::List<std::string>& values) {
    QStringList result;
    for (const auto& value : values.values())
        result.append(toQt(value));
    return result;
}
inline QString safeCode(const std::string& value) {
    if (value.empty() || value.size() > 96)
        return QStringLiteral("qt.source_failure");
    for (const auto character : value) {
        if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
                character == '.' || character == '_'))
            return QStringLiteral("qt.source_failure");
    }
    return toQt(value);
}
inline InvocationOutcome outcome(bool value) { return InvocationOutcome::success(value); }
inline InvocationOutcome outcome(const std::string& value) { return InvocationOutcome::success(toQt(value)); }
template<class T>
InvocationOutcome outcome(const model::Result<T, std::string>& result) {
    if (result.is_cancelled())
        return InvocationOutcome::cancelled(safeCode(result.error()));
    if (!result.is_success())
        return InvocationOutcome::failure(safeCode(result.error()));
    if constexpr (std::is_void_v<T>)
        return InvocationOutcome::success();
    else
        return outcome(result.value());
}
}
