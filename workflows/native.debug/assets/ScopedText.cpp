// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/native_host.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <thread>

namespace model = pipelang::model;
namespace native = pipelang::native_app_v19;

extern "C" const native::HostedModel proof_root;

void require(bool valid, const char* diagnostic)
{
    if (!valid)
        throw std::runtime_error(diagnostic);
}

native::HostedValue names(std::string previous, std::string latest)
{
    const std::vector<std::uint8_t> bytes(latest.size(), latest.empty() ? 0 : static_cast<std::uint8_t>(latest.front()));
    return native::HostedValue::fromRecord("NativeProof.ScopedText.Names", {
        {"previous", native::HostedValue::fromText(std::move(previous))},
        {"latest", native::HostedValue::fromText(std::move(latest))},
        {"bytes", native::HostedValue::fromBytes(bytes)}
    });
}

void check(const native::HostedValueResult& result, const std::string& previous, const std::string& latest, bool withAppend)
{
    if (result.is_failure())
        throw std::runtime_error(result.error());
    require(result.is_success(), "native text observation did not complete");
    require(result.value().field("label").text() == latest, "native receiver lost its text field");
    const auto aliases = result.value().field("aliases");
    require(aliases.field("previous").text() == previous && aliases.field("latest").text() == latest,
            "native receiver lost nested text or an earlier constructor borrow");

    std::vector<std::uint8_t> expected(latest.size(), latest.empty() ? 0 : static_cast<std::uint8_t>(latest.front()));
    const auto nested = aliases.field("bytes").bytes();
    require(std::ranges::equal(nested, expected), "nested receiver buffer changed");
    if (withAppend)
        expected.push_back(255);
    const auto payload = result.value().field("payload").bytes();
    require(std::ranges::equal(payload, expected), "native receiver lost its byte field or mutated its earlier borrow");
}

int main()
{
    try {
        constexpr std::string_view service = "NativeProof.ScopedText.Consumer";
        const auto root = native::HostedInstance::bind(proof_root, {});
        require(root.is_success(), "could not initialize native text graph");
        const auto build = [&](const std::string& previous, const std::string& latest) {
            const std::array<native::HostedConstructorArgument, 1> arguments{names(previous, latest)};
            auto instance = root.value().constructService(service, arguments);
            if (instance.is_failure())
                throw std::runtime_error(instance.error());
            require(instance.is_success(), "could not construct native text receiver");
            return std::move(instance).value();
        };

        const std::string previous = std::string(8000, 'p') + " é";
        const std::string latest = std::string(8000, 'l') + " 中";
        const std::string nextPrevious = std::string(8000, 'n') + " ø";
        const std::string nextLatest = std::string(8000, 'u') + " 新";
        const auto instance = build(previous, latest);
        model::TaskScope tasks;
        const std::array<native::HostedCallArgument, 1> firstArgs{instance};
        check(root.value().startMethod(tasks, "Read", firstArgs).result(), previous, latest, true);

        const auto group = tasks.createGroup();
        const std::array<native::HostedCallArgument, 2> holdArgs{instance, model::CancellationToken{}};
        const auto held = root.value().tryStartMethod(tasks, group, "Hold", holdArgs);
        require(held.is_success() && !held.value().completed(), "text reader did not suspend after observing the receiver");
        const auto writer = tasks.createGroup();
        const std::array<native::HostedCallArgument, 2> updateArgs{instance, names(nextPrevious, nextLatest)};
        const auto changed = root.value().tryStartMethod(tasks, writer, "Update", updateArgs);
        require(changed.is_success() && changed.value().result().is_success(), "sibling could not replace receiver text");
        check(root.value().startMethod(tasks, "Read", firstArgs).result(), nextPrevious, nextLatest, false);

        const auto released = root.value().tryStartMethod(tasks, writer, "Release", {});
        require(released.is_success() && released.value().result().is_success(), "sibling could not release parked text reader");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!held.value().completed() && std::chrono::steady_clock::now() < deadline) {
            tasks.poll();
            std::this_thread::yield();
        }
        require(held.value().completed(), "text reader did not resume");
        check(held.value().result(), previous, latest, true);

        const auto empty = build("", "");
        const std::array<native::HostedCallArgument, 1> emptyArgs{empty};
        check(root.value().startMethod(tasks, "Read", emptyArgs).result(), "", "", true);

        for (int iteration = 0; iteration < 1800; ++iteration) {
            const auto scoped = build(previous, latest);
            const std::array<native::HostedCallArgument, 1> arguments{scoped};
            check(root.value().startMethod(tasks, "Read", arguments).result(), previous, latest, true);
            require(root.value().retireService(scoped).is_success(), "could not retire a scoped text receiver");
        }
        std::cout << "Scoped receiver text and bytes survived construction, suspension and independent reassignment.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
