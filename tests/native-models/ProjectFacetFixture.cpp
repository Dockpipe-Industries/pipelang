// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ProjectFacetFixture.hpp"
#include "../../src/native/support/support.hpp"

using pipelang::tools::atomicWrite;

void writeProjectFacetSources(const std::filesystem::path& root) {
    std::filesystem::create_directories(root / "src/facets");
    atomicWrite(root / "src/facets/Provider.pipe", R"pipe(module Facets.Provider;
public Interface Provider {
    Result<int, string> Resolve(int input);
    Task<Result<int, string>> ResolveLater(int input);
    pure Result<float32, string> Scalar(int index);
}
)pipe");
    atomicWrite(root / "src/facets/Policy.pipe", R"pipe(module Facets.Policy;
import Facets.Provider;
public Record NormalPoint { public float32 x; public float32 y; public float32 z; }
public Record NormalInput {
    public int index;
    public int fault;
    public NormalPoint first;
    public NormalPoint second;
    public NormalPoint third;
}
public model Class Policy {
    private const int PrivateOffset = 3;
    public const int Offset = Policy.PrivateOffset;
    public int accepted;
    private Provider provider;
    private int secret = 5;
    private Policy? peer = null;

    public Policy(Provider host) {
        provider = host;
    }

    public Policy(Provider host, bool makePeer) {
        provider = host;
        if (makePeer)
            peer = new Policy(host);
    }

    private pure int PrivateRead() => secret;
    public pure int PeerValue() => peer.Value.secret + peer.Value.PrivateRead();

    private pure int Shadow() {
        int Policy = 4;
        return Policy;
    }

    private pure int Shift(int input) => input + Policy.PrivateOffset + Shadow() - 4;

    private pure Result<T, string> Forward<T, U>(Result<U, string> outcome) {
        if (outcome.IsCancelled)
            return Result<T, string>.Cancelled(outcome.Error);
        return Result<T, string>.Failure(outcome.Error);
    }

    public pure int Read() => accepted;
    public pure float32 Sum(float32 first, float32 second) => first + second;
    public pure float32 Difference(float32 first, float32 second) => first - second;
    public pure float32 Product(float32 first, float32 second) => first * second;
    public pure float32 Quotient(float32 first, float32 second) => first / second;
    public pure float32 Composite(float32 first, float32 second, float32 third) => first * second + third;
    public float32 Observe() => provider.Scalar(1).Value + provider.Scalar(2).Value;


    private pure NormalPoint SubtractPoints(NormalPoint first, NormalPoint second)
        => new NormalPoint { x = first.x - second.x, y = first.y - second.y, z = first.z - second.z };

    private pure NormalPoint CrossPoints(NormalPoint first, NormalPoint second)
        => new NormalPoint {
            x = first.y * second.z - first.z * second.y,
            y = first.z * second.x - first.x * second.z,
            z = first.x * second.y - first.y * second.x
        };

    private pure NormalPoint NormalizePoint(NormalPoint value) {
        float32 length = float32.Hypot(value.x, value.y, value.z);
        if (length <= 0.000001f)
            return new NormalPoint {};
        return new NormalPoint { x = value.x / length, y = value.y / length, z = value.z / length };
    }

    private pure NormalPoint FaceNormal(NormalInput input) {
        if (input.fault == 1) {
            int delay = 0;
            while (delay < 200)
                delay = delay + 1;
            throw Error("first indexed normal fault");
        }
        if (input.fault == 2)
            throw Error("later indexed normal fault");
        return NormalizePoint(CrossPoints(SubtractPoints(input.second, input.first),
            SubtractPoints(input.third, input.first)));
    }

    public List<NormalPoint> SerialNormals(List<NormalInput> inputs) {
        List<NormalPoint> values = List<NormalPoint>.Empty();
        foreach (NormalInput input in inputs)
            values.Add(FaceNormal(input));
        return values;
    }

    public List<NormalPoint> ParallelNormals(List<NormalInput> inputs, CancellationToken cancellation) {
        accepted = 99;
        ConcurrentDictionary<int, NormalPoint> completed = ConcurrentDictionary<int, NormalPoint>.Empty();
        parallel foreach (NormalInput input in inputs) {
            completed.Set(input.index, FaceNormal(input));
        }
        List<NormalPoint> values = List<NormalPoint>.Empty();
        int index = 0;
        while (index < inputs.Count) {
            values.Add(completed.At(index));
            index = index + 1;
        }
        accepted = 100;
        return values;
    }

    public Result<int, string> Change(int input) {
        Result<int, string> outcome = provider.Resolve(Shift(input));
        if (!outcome.IsSuccess)
            return Forward<int, int>(outcome);
        accepted = outcome.Value;
        return outcome;
    }

    public async Task<Result<int, string>> ChangeLater(int input) {
        Result<int, string> outcome = await provider.ResolveLater(Shift(input));
        if (!outcome.IsSuccess)
            return Forward<int, int>(outcome);
        accepted = outcome.Value;
        return outcome;
    }

    public Result<void, string> MutateThenThrow(int input) {
        accepted = input;
        throw Error("facet rollback");
    }
}
)pipe");
    atomicWrite(root / "src/facets/Derived.pipe", R"pipe(module Facets.Derived;
import Domain.Item;
public Class Derived : Item {
    public Derived(int initial) : base(initial) {}
    public pure int ReadProtected() => Item.ProtectedOffset;
}
)pipe");
    atomicWrite(root / "src/facets/Group.pipe", R"pipe(module Facets.Group;
import Facets.Provider;
import Facets.Policy;
import Facets.Derived;
import Domain.Counter;
import Domain.Item;
import Domain.Contract;
public model Class Group {
    private Policy policy;
    private Counter counter = new Counter(17);
    private Readable derived = new Derived(29);
    private Derived concreteDerived = new Derived(37);
    private Item item = new Item(31);

    public Group(Provider host) {
        policy = new Policy(host, true);
    }

    public pure int Read() => policy.Read();
    public pure int CounterRead() => counter.Read();
    public pure int DerivedRead() => derived.Read();
    public pure int ProtectedRead() => concreteDerived.ReadProtected();
    public pure int PeerValue() => policy.PeerValue();
    public pure Item ExportReference() => item;
    public pure Policy ExportPolicy() => policy;
    public pure float32 ReadPolicy(Policy value, float32 first, float32 second) => value.Sum(first, second);
    public pure float32 Sum(float32 first, float32 second) => policy.Sum(first, second);
    public pure float32 Difference(float32 first, float32 second) => policy.Difference(first, second);
    public pure float32 Product(float32 first, float32 second) => policy.Product(first, second);
    public pure float32 Quotient(float32 first, float32 second) => policy.Quotient(first, second);
    public pure float32 Composite(float32 first, float32 second, float32 third) => policy.Composite(first, second, third);
    public float32 Observe() => policy.Observe();

    public List<NormalPoint> ParallelNormals(List<NormalInput> inputs, CancellationToken cancellation)
        => policy.ParallelNormals(inputs, cancellation);

    public void SetCounter(int next) { counter.Set(next); }
    public Result<int, string> Change(int input) => policy.Change(input);
    public async Task<Result<int, string>> ChangeLater(int input) {
        Result<int, string> outcome = await policy.ChangeLater(input);
        return outcome;
    }
    public Result<void, string> MutateThenThrow(int input) => policy.MutateThenThrow(input);
}
)pipe");
}

void writeProjectFacetConsumer(const std::filesystem::path& root) {
    atomicWrite(root / "facet-consumer.hpp", R"cpp(#pragma once
#include <atomic>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <vector>
#include <pipelang/model/binary32_operations.hpp>
#include <pipelang/model/context.hpp>
#include <chrono>
#include <thread>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace facet_test {
using project_fixture::source::Facets::Policy::Policy;
using project_fixture::source::Facets::Provider::Provider;
using project_fixture::source::Facets::Group::Group;
using project_fixture::source::Domain::Item::Item;
using IntResult = pipelang::model::Result<std::int64_t, std::string>;

class NativeProvider final : public Provider {
public:
    IntResult Resolve(std::int64_t input) override {
        ++calls;
        if (outcome == pipelang::model::Outcome::Cancelled)
            return IntResult::Cancelled("provider cancelled");
        if (outcome == pipelang::model::Outcome::Failure)
            return IntResult::Failure("provider failed");
        return IntResult::Success(input);
    }

    IntResult ResolveLater(std::int64_t input) override {
        return Resolve(input);
    }

    pipelang::model::Result<float, std::string> Scalar(std::int64_t index) const override {
        scalarCalls.push_back(index);
        if (stopAt == index) {
            if (scalarCancelled)
                throw pipelang::model::OperationCancelled("scalar cancelled");
            throw std::runtime_error("scalar failed");
        }
        return pipelang::model::Result<float, std::string>::Success(index == 1 ? 1.25f : 2.5f);
    }

    mutable std::vector<std::int64_t> scalarCalls;
    std::int64_t stopAt = 0;
    bool scalarCancelled = false;
    pipelang::model::Outcome outcome = pipelang::model::Outcome::Success;
    std::atomic<int> calls{0};
};

inline void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template<class Owner, class Task>
void finish(Owner& owner, const Task& task) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!task.completed()) {
        owner.poll();
        require(std::chrono::steady_clock::now() < deadline, "project facet task exceeded its deadline");
        std::this_thread::yield();
    }
}

inline void sameBits(float actual, float expected) {
    require((std::isnan(actual) && std::isnan(expected)) ||
            std::bit_cast<std::uint32_t>(actual) == std::bit_cast<std::uint32_t>(expected),
            "project facet binary32 bits changed");
}

inline void verifyBinary32(Policy& native, Group& group, NativeProvider& provider) {
    using namespace pipelang::model;
    const std::array<float, 9> values{0.f, -0.f, 1.f, -1.f,
        std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::min(),
        std::numeric_limits<float>::max(), std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN()};
    for (float first : values) {
        for (float second : values) {
            volatile float sum = first + second;
            volatile float difference = first - second;
            volatile float product = first * second;
            volatile float quotient = first / second;
            sameBits(native.Sum(first, second), sum);
            sameBits(native.Difference(first, second), difference);
            sameBits(native.Product(first, second), product);
            sameBits(native.Quotient(first, second), quotient);
            sameBits(group.Sum(first, second), sum);
            sameBits(group.Difference(first, second), difference);
            sameBits(group.Product(first, second), product);
            sameBits(group.Quotient(first, second), quotient);
        }
    }
    // Multiplication rounds to one before subtraction. A contracted operation
    // would return a nonzero result for this exact binary32 triplet.
    const float first = 0x1.000002p0f;
    const float second = 0x1.fffffep-1f;
    require(std::fma(first, second, -1.f) != 0.f, "rounding witness cannot distinguish contraction");
    sameBits(native.Composite(first, second, -1.f), 0.f);
    sameBits(group.Composite(first, second, -1.f), 0.f);
    const Policy::Reference reference = group.ExportPolicy();
    sameBits(group.ReadPolicy(reference, 1.25f, 2.5f), 3.75f);

    for (bool nested : {false, true}) {
        provider.scalarCalls.clear();
        const auto success = nested ? group.Observe() : native.Observe();
        require(success.is_success() && success.value() == 3.75f &&
                provider.scalarCalls == std::vector<std::int64_t>{1, 2},
                "binary32 host operand order/count changed");
        for (std::int64_t stop : {1, 2}) {
            for (bool cancelled : {false, true}) {
                provider.scalarCalls.clear();
                provider.stopAt = stop;
                provider.scalarCancelled = cancelled;
                const auto result = nested ? group.Observe() : native.Observe();
                const std::vector<std::int64_t> expected = stop == 1
                    ? std::vector<std::int64_t>{1} : std::vector<std::int64_t>{1, 2};
                require(!result.is_success() && result.is_cancelled() == cancelled &&
                        result.error() == (cancelled ? "scalar cancelled" : "scalar failed") &&
                        provider.scalarCalls == expected,
                        "binary32 abrupt host completion evaluated another operand");
            }
        }
        provider.stopAt = 0;
    }
    // Independently exercise aggregate sequencing when budget admission itself
    // fails before the callback in either operand. The failed step remains visible.
    for (std::size_t admitted : {0U, 1U}) {
        auto budget = EvaluationBudget::forInitialization(1);
        if (admitted == 0)
            budget.step();
        std::vector<int> observations;
        const auto operand = [&](int index) {
            budget.step();
            observations.push_back(index);
            return float(index);
        };
        bool refused = false;
        try {
            (void)addBinary32(Binary32Operands{operand(1), operand(2)});
        } catch (const EvaluationLimitFailure&) {
            refused = true;
        }
        require(refused && observations == (admitted == 0 ? std::vector<int>{} : std::vector<int>{1}),
                "binary32 budget refusal lost first-stop ordering");
    }
}

inline void verifyParallelFacetNormals() {
    using project_fixture::source::Facets::Policy::NormalInput;
    using project_fixture::source::Facets::Policy::NormalPoint;
    using pipelang::model::List;
    auto inputs = List<NormalInput>::Empty();
    for (std::int64_t index = 0; index < 64; ++index) {
        const float scale = static_cast<float>(index % 7 + 1);
        inputs.add(NormalInput{index, 0,
            NormalPoint{0.f, 0.f, 0.f}, NormalPoint{scale, 0.f, 0.f},
            NormalPoint{0.f, index % 2 == 0 ? scale : -scale, 0.f}});
    }
    // Degenerate normals and signed zero use the same canonical value helpers.
    inputs.add(NormalInput{64, 0, NormalPoint{-0.f, 0.f, -0.f},
        NormalPoint{0.f, 0.f, 0.f}, NormalPoint{0.f, 0.f, 0.f}});
    for (std::size_t degree : {1U, 4U}) {
        auto host = std::make_shared<NativeProvider>();
        Policy native(host, true, {}, {degree});
        Group group(host, {}, {degree});
        const auto serial = native.SerialNormals(inputs);
        const auto direct = native.ParallelNormals(inputs, {});
        const auto nested = group.ParallelNormals(inputs, {});
        require(serial.is_success() && direct.is_success() && nested.is_success(),
                "stateless project facet parallel normal calculation failed");
        const auto& expected = serial.value().values();
        require(expected.size() == 65 && direct.value().values().size() == expected.size() &&
                nested.value().values().size() == expected.size(), "parallel normal publication lost source order/count");
        for (std::size_t index = 0; index < expected.size(); ++index) {
            const auto& first = direct.value().values()[index];
            const auto& second = nested.value().values()[index];
            for (auto pair : {std::pair{first.x, expected[index].x}, std::pair{first.y, expected[index].y},
                              std::pair{first.z, expected[index].z}, std::pair{second.x, expected[index].x},
                              std::pair{second.y, expected[index].y}, std::pair{second.z, expected[index].z}})
                require(std::bit_cast<std::uint32_t>(pair.first) == std::bit_cast<std::uint32_t>(pair.second),
                        "parallel native/source helper changed exact normal bits");
            if (index < 64)
                require(first.x == 0.f && first.y == 0.f && first.z == (index % 2 == 0 ? 1.f : -1.f),
                        "parallel normal independent axis oracle failed");
            else
                require(first.x == 0.f && first.y == 0.f && first.z == 0.f,
                        "parallel normal independent degenerate oracle failed");
        }
        pipelang::model::CancellationSource cancellation;
        cancellation.cancel();
        require(native.ParallelNormals(inputs, cancellation.token()).is_success() &&
                group.ParallelNormals(inputs, cancellation.token()).is_success() &&
                native.Read() == 100 && group.Read() == 100,
                "parallel helper inferred cancellation from an unused authored token");
        {
            pipelang::model::ContextOwner authority;
            pipelang::model::InvocationContextScope invocation(authority.capture().permit());
            authority.invalidate();
            require(native.ParallelNormals(inputs, {}).is_cancelled() &&
                    group.ParallelNormals(inputs, {}).is_cancelled(),
                    "parallel helper lost invocation-context cancellation");
        }
        require(native.Read() == 100 && group.Read() == 100,
                "cancelled parallel helper committed owner mutation");
        auto faults = List<NormalInput>::Empty();
        auto firstFault = inputs.values()[0];
        firstFault.fault = 1;
        auto laterFault = inputs.values()[1];
        laterFault.fault = 2;
        faults.add(firstFault);
        faults.add(laterFault);
        for (int repeat = 0; repeat < 4; ++repeat) {
            const auto directFault = native.ParallelNormals(faults, {});
            const auto nestedFault = group.ParallelNormals(faults, {});
            require(directFault.is_failure() && nestedFault.is_failure() &&
                    directFault.error() == "first indexed normal fault" &&
                    nestedFault.error() == directFault.error() && native.Read() == 100 && group.Read() == 100,
                    "parallel helper fault changed first-index selection or rollback");
        }
        require(native.ParallelNormals(inputs, {}).is_success() && group.ParallelNormals(inputs, {}).is_success(),
                "parallel helper failure/cancellation poisoned later owner execution");
        require(host->calls.load() == 0 && host->scalarCalls.empty(),
                "stateless worker helper reached a host service");
    }
}

inline void verify() {
    verifyParallelFacetNormals();
    static_assert(std::is_same_v<decltype(std::declval<const Group&>().ExportReference()), Item::Reference>);
    static_assert(!std::is_same_v<Item, Item::Reference>);
    auto provider = std::make_shared<NativeProvider>();
    Policy native(provider, true);
    Group group(provider);
    verifyBinary32(native, group, *provider);
    const auto exported = group.ExportReference();
    exported.require();
    const auto before = native.snapshot();
    require(native.Change(4).value() == 7 && group.Change(10).value() == 13,
            "native/source host helper results differ");
    require(native.Read() == 7 && group.Read() == 13 && before.accepted == 0,
            "native/source instances or immutable snapshots share mutation");
    require(group.CounterRead() == 17 && group.DerivedRead() == 29 && group.ProtectedRead() == 1 &&
            native.PeerValue() == 10 && group.PeerValue() == 10,
            "nested model objects or marked-base inheritance failed");
    group.SetCounter(19);
    require(group.CounterRead() == 19 && Policy::Offset == 3,
            "source mutation or native constant export failed");

    for (const auto outcome : {pipelang::model::Outcome::Cancelled, pipelang::model::Outcome::Failure}) {
        provider->outcome = outcome;
        const auto direct = native.Change(100);
        const auto nested = group.Change(100);
        require(direct.outcome() == outcome && nested.outcome() == outcome && direct.error() == nested.error(),
                "native/source cancellation or failure details changed");
        require(native.Read() == 7 && group.Read() == 13,
                "declined host result mutated accepted state");
    }
    const auto directFault = native.MutateThenThrow(200);
    const auto nestedFault = group.MutateThenThrow(200);
    require(directFault.is_failure() && nestedFault.is_failure() &&
            directFault.error() == "facet rollback" && nestedFault.error() == "facet rollback",
            "native/source typed execution failure differs");
    require(native.snapshot().accepted == 7 && group.Read() == 13,
            "native/source transaction failed to roll back");
    provider->outcome = pipelang::model::Outcome::Success;
    auto directTask = native.ChangeLater(20);
    finish(native, directTask);
    auto nestedTask = group.ChangeLater(30);
    finish(group, nestedTask);
    require(directTask.result().value() == 23 && nestedTask.result().value() == 33 &&
            native.Read() == 23 && group.Read() == 33,
            "async native/source transaction publication differs");
    provider->outcome = pipelang::model::Outcome::Cancelled;
    auto cancelled = group.ChangeLater(100);
    finish(group, cancelled);
    require(cancelled.result().is_cancelled() && cancelled.result().error() == "provider cancelled" &&
            group.Read() == 33 && native.pending() == 0 && group.pending() == 0,
            "async source cancellation lost its outcome, state or lifetime");
    require(provider->calls.load() == 9, "host service lifetime or call count changed");
}
}
)cpp");
}
