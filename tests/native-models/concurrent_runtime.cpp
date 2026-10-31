// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/concurrent_collections.hpp>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <atomic>
#include <barrier>
#include <cassert>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

// Injection is thread local and enabled only by owner-thread failure probes.
// Worker tests run with normal allocation and a fixed eight-thread ceiling.
#if defined(__GNUC__) || defined(__clang__)
#define PIPELANG_TEST_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define PIPELANG_TEST_NOINLINE __declspec(noinline)
#else
#define PIPELANG_TEST_NOINLINE
#endif

thread_local int allocationCountdown = -1;

PIPELANG_TEST_NOINLINE void* operator new(std::size_t size) {
    if (allocationCountdown == 0)
        throw std::bad_alloc();
    if (allocationCountdown > 0)
        --allocationCountdown;
    if (void* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc();
}

PIPELANG_TEST_NOINLINE void operator delete(void* memory) noexcept {
    std::free(memory);
}

PIPELANG_TEST_NOINLINE void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

PIPELANG_TEST_NOINLINE void* operator new[](std::size_t size) {
    return ::operator new(size);
}

PIPELANG_TEST_NOINLINE void operator delete[](void* memory) noexcept {
    ::operator delete(memory);
}

PIPELANG_TEST_NOINLINE void operator delete[](void* memory, std::size_t) noexcept {
    ::operator delete(memory);
}

using namespace pipelang::model;

namespace {

struct ThrowingCopy {
    int value = 0;
    static inline bool refuseCopy = false;
    static inline bool refuseComparison = false;

    explicit ThrowingCopy(int initial = 0) : value(initial) {
    }

    ThrowingCopy(const ThrowingCopy& other) : value(other.value) {
        if (refuseCopy)
            throw std::runtime_error("injected payload copy failure");
    }

    ThrowingCopy(ThrowingCopy&&) noexcept = default;
    ThrowingCopy& operator=(const ThrowingCopy&) = default;
    ThrowingCopy& operator=(ThrowingCopy&&) noexcept = default;

    bool operator==(const ThrowingCopy& other) const {
        if (refuseComparison)
            throw std::runtime_error("injected comparison failure");
        return value == other.value;
    }
};

template<class Function>
void expectFailure(Function function) {
    bool failed = false;
    try {
        function();
    } catch (const ExecutionFailure&) {
        failed = true;
    }
    assert(failed);
}

template<class Function>
void expectCopyFailure(Function function) {
    ThrowingCopy::refuseCopy = true;
    bool failed = false;
    try {
        function();
    } catch (const std::runtime_error&) {
        failed = true;
    }
    ThrowingCopy::refuseCopy = false;
    assert(failed);
}

void sharingAndSnapshots() {
    ConcurrentDictionary<std::string, std::string> dictionary;
    auto alias = dictionary;
    assert(alias.tryAdd("first", "one"));
    assert(!dictionary.tryAdd("first", "ignored"));
    dictionary.set("second", "two");
    assert(alias.getOrAdd("first", "other") == "one");
    assert(alias.getOrAdd("third", "three") == "three");
    assert(!alias.tryUpdate("first", "bad", "mismatch"));
    assert(alias.tryUpdate("first", "updated", "one"));
    auto entries = dictionary.entries();
    auto keys = dictionary.keys();
    auto values = dictionary.mappedValues();
    assert(entries.at(0).key == "first" && entries.at(0).value == "updated");
    assert(keys.at(1) == "second" && values.at(2) == "three");
    assert(alias.tryRemove("second").value() == "two");
    assert(!dictionary.tryRemove("absent").has_value());
    assert(!dictionary.tryGet("second").has_value());
    assert(!dictionary.containsKey("second"));
    alias.clear();
    assert(dictionary.count() == 0);
    assert(entries.count() == 3 && entries.at(1).value == "two");
    expectFailure([&] { dictionary.at("missing"); });

    ConcurrentQueue<std::string> queue;
    auto queueAlias = queue;
    queue.enqueue("first");
    queueAlias.enqueue("second");
    auto queued = queue.items();
    auto queuedVector = queue.values();
    assert(queueAlias.tryPeek().value() == "first");
    assert(queue.tryDequeue().value() == "first");
    assert(queueAlias.tryDequeue().value() == "second");
    assert(!queue.tryDequeue().has_value() && !queue.tryPeek().has_value());
    queue.enqueue("changed");
    queueAlias.clear();
    assert(queue.count() == 0 && queued.at(0) == "first");
    assert(queuedVector[1] == "second");

    ConcurrentStack<std::string> stack;
    auto stackAlias = stack;
    stack.push("bottom");
    stackAlias.push("top");
    auto stacked = stack.items();
    assert(stacked.at(0) == "top" && stack.values()[1] == "bottom");
    assert(stackAlias.tryPeek().value() == "top");
    assert(stack.tryPop().value() == "top");
    assert(stackAlias.tryPop().value() == "bottom");
    assert(!stack.tryPop().has_value() && !stack.tryPeek().has_value());
    stackAlias.clear();
    assert(stacked.count() == 2);

    auto movedDictionary = std::move(alias);
    auto movedQueue = std::move(queueAlias);
    auto movedStack = std::move(stackAlias);
    validate(movedDictionary);
    validate(movedQueue);
    validate(movedStack);
    expectFailure([&] { validate(alias); });
    expectFailure([&] { queueAlias.count(); });
    expectFailure([&] { stackAlias.push("invalid"); });
}

template<class Push, class Pop>
void producersAndConsumers(Push push, Pop pop) {
    constexpr int producers = 4;
    constexpr int consumers = 4;
    constexpr int perProducer = 32;
    constexpr int total = producers * perProducer;
    std::array<std::atomic<int>, total> seen{};
    std::atomic<int> finishedProducers{0};
    std::atomic<int> removed{0};
    std::barrier start(producers + consumers);
    std::array<std::jthread, producers + consumers> workers;
    for (int producer = 0; producer < producers; ++producer) {
        workers[producer] = std::jthread([&, producer] {
            start.arrive_and_wait();
            for (int index = 0; index < perProducer; ++index)
                push(producer * perProducer + index);
            finishedProducers.fetch_add(1);
        });
    }
    for (int consumer = 0; consumer < consumers; ++consumer) {
        workers[producers + consumer] = std::jthread([&] {
            start.arrive_and_wait();
            for (;;) {
                const auto item = pop();
                if (item.has_value()) {
                    assert(item.value() >= 0 && item.value() < total);
                    assert(seen[static_cast<std::size_t>(item.value())].fetch_add(1) == 0);
                    removed.fetch_add(1);
                } else if (finishedProducers.load() == producers && removed.load() == total) {
                    return;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (auto& worker : workers)
        worker.join();
    assert(removed.load() == total);
    for (const auto& count : seen)
        assert(count.load() == 1);
}

void concurrentOperations() {
    ConcurrentQueue<std::int64_t> queue;
    producersAndConsumers([queue](int value) { queue.enqueue(value); },
                          [queue] { return queue.tryDequeue(); });
    assert(queue.count() == 0);
    ConcurrentStack<std::int64_t> stack;
    producersAndConsumers([stack](int value) { stack.push(value); },
                          [stack] { return stack.tryPop(); });
    assert(stack.count() == 0);

    ConcurrentDictionary<std::int64_t, std::int64_t> dictionary;
    std::barrier start(4);
    std::array<std::int64_t, 4> winners{};
    std::array<std::jthread, 4> workers;
    for (std::size_t index = 0; index < workers.size(); ++index) {
        workers[index] = std::jthread([&, index] {
            start.arrive_and_wait();
            winners[index] = dictionary.getOrAdd(7, static_cast<std::int64_t>(index));
        });
    }
    for (auto& worker : workers)
        worker.join();
    assert(dictionary.count() == 1);
    for (auto winner : winners)
        assert(winner == dictionary.at(7));

    dictionary.set(7, 0);
    for (auto& worker : workers) {
        worker = std::jthread([&] {
            for (int update = 0; update < 100; ++update) {
                for (;;) {
                    const auto previous = dictionary.at(7);
                    if (dictionary.tryUpdate(7, previous + 1, previous))
                        break;
                }
            }
        });
    }
    for (auto& worker : workers)
        worker.join();
    assert(dictionary.at(7) == 400);

    std::atomic<int> removals{0};
    for (auto& worker : workers) {
        worker = std::jthread([&] {
            if (dictionary.tryRemove(7).has_value())
                removals.fetch_add(1);
        });
    }
    for (auto& worker : workers)
        worker.join();
    assert(removals.load() == 1 && dictionary.count() == 0);
}

void ownershipLimits() {
    ConcurrentQueue<std::int64_t> queue;
    ConcurrentStack<std::int64_t> stack;
    ConcurrentDictionary<std::int64_t, std::int64_t> dictionary;
    for (std::size_t index = 0; index < maximumListElements; ++index) {
        queue.enqueue(static_cast<std::int64_t>(index));
        stack.push(static_cast<std::int64_t>(index));
        assert(dictionary.tryAdd(static_cast<std::int64_t>(index), 0));
    }
    expectFailure([&] { queue.enqueue(9); });
    expectFailure([&] { stack.push(9); });
    expectFailure([&] { dictionary.tryAdd(-1, 9); });
    assert(queue.count() == static_cast<std::int64_t>(maximumListElements));
    assert(stack.count() == queue.count() && dictionary.count() == queue.count());
    ConcurrentQueue<std::string> textQueue;
    ConcurrentStack<std::string> textStack;
    ConcurrentDictionary<std::string, std::string> textDictionary;
    const std::string oversized(maximumListBytes, 'x');
    expectFailure([&] { textQueue.enqueue(oversized); });
    expectFailure([&] { textStack.push(oversized); });
    expectFailure([&] { textDictionary.set("key", oversized); });
    assert(textQueue.count() == 0 && textStack.count() == 0 && textDictionary.count() == 0);

    auto nested = List<std::int64_t>::Empty();
    for (int index = 0; index < 2048; ++index)
        nested.add(index);
    ConcurrentQueue<List<std::int64_t>> nestedQueue;
    ConcurrentStack<List<std::int64_t>> nestedStack;
    ConcurrentDictionary<std::int64_t, List<std::int64_t>> nestedDictionary;
    nestedQueue.enqueue(nested);
    nestedStack.push(nested);
    nestedDictionary.set(1, nested);
    expectFailure([&] { nestedQueue.enqueue(nested); });
    expectFailure([&] { nestedStack.push(nested); });
    expectFailure([&] { nestedDictionary.set(2, nested); });
    assert(nestedQueue.count() == 1 && nestedStack.count() == 1 && nestedDictionary.count() == 1);
}

void throwingPayloads() {
    ConcurrentDictionary<std::int64_t, ThrowingCopy> dictionary;
    dictionary.set(1, ThrowingCopy(7));
    expectCopyFailure([&] { dictionary.tryRemove(1); });
    assert(dictionary.at(1).value == 7);
    expectCopyFailure([&] { dictionary.getOrAdd(2, ThrowingCopy(8)); });
    assert(!dictionary.containsKey(2));
    expectCopyFailure([&] { dictionary.values(); });
    assert(dictionary.count() == 1);
    ThrowingCopy::refuseComparison = true;
    bool comparisonFailed = false;
    try {
        dictionary.tryUpdate(1, ThrowingCopy(8), ThrowingCopy(7));
    } catch (const std::runtime_error&) {
        comparisonFailed = true;
    }
    ThrowingCopy::refuseComparison = false;
    assert(comparisonFailed && dictionary.at(1).value == 7);

    ConcurrentQueue<ThrowingCopy> queue;
    queue.enqueue(ThrowingCopy(3));
    expectCopyFailure([&] { queue.tryDequeue(); });
    assert(queue.count() == 1 && queue.tryPeek().value().value == 3);
    ConcurrentStack<ThrowingCopy> stack;
    stack.push(ThrowingCopy(4));
    expectCopyFailure([&] { stack.tryPop(); });
    assert(stack.count() == 1 && stack.tryPeek().value().value == 4);
}

void allocationFailures() {
    int failures = 0;
    for (int limit = 0; limit < 16; ++limit) {
        ConcurrentDictionary<std::int64_t, std::string> dictionary;
        dictionary.set(1, "before");
        bool failed = false;
        allocationCountdown = limit;
        try {
            dictionary.getOrAdd(2, std::string(80, 'v'));
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        allocationCountdown = -1;
        assert(dictionary.at(1) == "before");
        assert(dictionary.count() == (failed ? 1 : 2));
        failures += failed ? 1 : 0;

        ConcurrentQueue<std::string> queue;
        queue.enqueue("before");
        failed = false;
        allocationCountdown = limit;
        try {
            queue.enqueue(std::string(80, 'v'));
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        allocationCountdown = -1;
        assert(queue.tryPeek().value() == "before");
        assert(queue.count() == (failed ? 1 : 2));
        failures += failed ? 1 : 0;
    }
    assert(failures > 0);

    ConcurrentDictionary<std::int64_t, std::string> dictionary;
    ConcurrentQueue<std::string> queue;
    ConcurrentStack<std::string> stack;
    const std::string payload(80, 'p');
    dictionary.set(1, payload);
    queue.enqueue(payload);
    stack.push(payload);
    for (int operation = 0; operation < 3; ++operation) {
        bool failed = false;
        allocationCountdown = 0;
        try {
            if (operation == 0) {
                dictionary.tryRemove(1);
            } else if (operation == 1) {
                queue.tryDequeue();
            } else {
                stack.tryPop();
            }
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        allocationCountdown = -1;
        assert(failed);
        assert(dictionary.count() == 1 && queue.count() == 1 && stack.count() == 1);
    }
}

}

int main() {
    sharingAndSnapshots();
    concurrentOperations();
    ownershipLimits();
    throwingPayloads();
    allocationFailures();
}
