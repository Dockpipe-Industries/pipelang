// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include <pipelang/model/extended_collections.hpp>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdlib>
#include <new>
#include <string>

// Deterministic allocation-failure injection is limited to one mutation at a
// time. All fixture setup, observations and assertions run with injection off.
#if defined(__GNUC__) || defined(__clang__)
#define PIPELANG_TEST_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define PIPELANG_TEST_NOINLINE __declspec(noinline)
#else
#define PIPELANG_TEST_NOINLINE
#endif

static std::int64_t allocationsUntilFailure = -1;
PIPELANG_TEST_NOINLINE void* operator new(std::size_t size) {
    if (allocationsUntilFailure == 0)
        throw std::bad_alloc();
    if (allocationsUntilFailure > 0)
        --allocationsUntilFailure;
    if (void* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc();
}
PIPELANG_TEST_NOINLINE void operator delete(void* memory) noexcept { std::free(memory); }
PIPELANG_TEST_NOINLINE void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
PIPELANG_TEST_NOINLINE void* operator new[](std::size_t size) { return ::operator new(size); }
PIPELANG_TEST_NOINLINE void operator delete[](void* memory) noexcept { ::operator delete(memory); }
PIPELANG_TEST_NOINLINE void operator delete[](void* memory, std::size_t) noexcept { ::operator delete(memory); }

using namespace pipelang::model;
using Integer = std::int64_t;

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

namespace fixture {
struct CopyFailure {};
struct Fragile {
    static inline bool failCopy = false;
    Integer number = 0;
    explicit Fragile(Integer value = 0) : number(value) {}
    Fragile(const Fragile& other) : number(other.number) {
        if (failCopy)
            throw CopyFailure{};
    }
    Fragile(Fragile&&) noexcept = default;
    Fragile& operator=(const Fragile&) = default;
    Fragile& operator=(Fragile&&) noexcept = default;
    bool operator==(const Fragile&) const = default;
};

struct Nested {
    Optional<Result<Error,std::string>> error;
    Queue<Integer> queue;
    bool operator==(const Nested&) const = default;
};
void validate(const Nested& value) {
    using pipelang::model::validate;
    validate(value.error);
    validate(value.queue);
}
std::size_t ownedBytes(const Nested& value) {
    using pipelang::model::ownedBytes;
    return addOwnedBytes(addOwnedBytes(sizeof(value), ownedBytes(value.error)), ownedBytes(value.queue));
}
std::size_t ownedElements(const Nested& value) {
    using pipelang::model::ownedElements;
    return boundedOwnedSum(ownedElements(value.error), ownedElements(value.queue), maximumListElements);
}
}

void arraysAndCopies() {
    auto values = Array<std::string>::Create(3, "initial");
    auto copy = values;
    values.set(1, "changed");
    assert(copy.at(1) == "initial");
    assert(values.values() == (std::vector<std::string>{"initial", "changed", "initial"}));
    auto detached = values.values();
    detached[0] = "detached";
    assert(values.at(0) == "initial");
    expectFailure([&] { values.at(-1); });
    expectFailure([&] { values.at(3); });
    expectFailure([&] { values.set(-1, "bad"); });
    assert(values.count() == 3);
    expectFailure([] { Array<Integer>::Create(-1, 0); });
    expectFailure([] { Array<Integer>::Create(4097, 0); });
    assert(Array<Integer>::Create(0, 1).count() == 0);
    assert(Array<bool>::Create(2, true).at(1));
    const auto before = values;
    expectFailure([&] { values.set(1, std::string(maximumListBytes, 'x')); });
    assert(values == before);
}

void dictionariesAndSets() {
    auto values = Dictionary<std::string,Integer>::Empty();
    values.add("one", 1);
    values.add("two", 2);
    values.set("one", 10);
    values.set("three", 3);
    assert(values.count() == 3 && values.at("one") == 10);
    assert(values.keys().at(0) == "one");
    assert(values.mappedValues().at(1) == 2);
    assert(values.entries().at(2) == (KeyValuePair<std::string,Integer>{"three", 3}));
    assert(values.tryGet("two").value() == 2);
    assert(!values.tryGet("missing").has_value());
    expectFailure([&] { values.at("missing"); });
    const auto before = values;
    expectFailure([&] { values.add("two", 99); });
    assert(values == before && values.values() == before.values());
    auto snapshot = values.values();
    snapshot[0].value = 999;
    assert(values.at("one") == 10);
    assert(values.remove("two"));
    assert(!values.remove("two"));
    values.add("two", 20);
    assert(values.keys().at(2) == "two");
    assert(before.at("two") == 2);
    auto reordered = Dictionary<std::string,Integer>::Empty();
    reordered.add("two", 20);
    reordered.add("three", 3);
    reordered.add("one", 10);
    assert(reordered == values);

    auto set = HashSet<std::string>::Empty();
    assert(set.add("first"));
    assert(set.add("second"));
    assert(!set.add("first"));
    assert(set.contains("second") && set.count() == 2);
    auto setCopy = set;
    assert(set.remove("first"));
    assert(!set.remove("first"));
    assert(set.add("first"));
    assert(set.items().at(0) == "second" && set.items().at(1) == "first");
    assert(set == setCopy);
    set.clear();
    assert(set.count() == 0 && setCopy.count() == 2);
    enum class Key { First, Second };
    auto enums = Dictionary<Key,Integer>::Empty();
    enums.add(Key::First, 4);
    assert(enums.at(Key::First) == 4);
    auto booleans = HashSet<bool>::Empty();
    assert(booleans.add(false) && booleans.add(true));
    assert(!booleans.add(false));
    const std::string malformed("\xff", 1);
    expectFailure([&] { values.containsKey(malformed); });
    expectFailure([&] { set.add(malformed); });
}

void queuesAndStacks() {
    auto queue = Queue<std::string>::Empty();
    assert(!queue.tryDequeue().has_value());
    expectFailure([&] { queue.dequeue(); });
    expectFailure([&] { queue.peek(); });
    queue.enqueue("one");
    queue.enqueue("two");
    auto copy = queue;
    assert(queue.dequeue() == "one");
    queue.enqueue("three");
    assert(queue.peek() == "two");
    assert(queue.values() == (std::vector<std::string>{"two", "three"}));
    assert(copy.values() == (std::vector<std::string>{"one", "two"}));
    assert(queue.tryDequeue().value() == "two");
    queue.clear();
    assert(queue.count() == 0);
    queue.enqueue("reused");
    assert(queue.dequeue() == "reused");

    auto stack = Stack<Integer>::Empty();
    assert(!stack.tryPop().has_value());
    expectFailure([&] { stack.peek(); });
    expectFailure([&] { stack.pop(); });
    stack.push(1);
    stack.push(2);
    auto stackCopy = stack;
    assert(stack.values() == (std::vector<Integer>{2, 1}));
    assert(stack.peek() == 2 && stack.pop() == 2);
    assert(stack.tryPop().value() == 1);
    assert(stackCopy.count() == 2 && stackCopy.peek() == 2);
    stack.clear();
    assert(stack.count() == 0);

    // Interleave enough operations to exercise prefix compaction and reuse.
    auto rotating = Queue<Integer>::Empty();
    for (Integer index = 0; index < 100; ++index)
        rotating.enqueue(index);
    for (Integer index = 0; index < 1000; ++index) {
        assert(rotating.dequeue() == index);
        rotating.enqueue(index + 100);
    }
    assert(rotating.count() == 100 && rotating.peek() == 1000);
    auto transferred = std::move(rotating);
    assert(transferred.count() == 100 && transferred.peek() == 1000);
    assert(rotating.count() == 0);
    rotating.enqueue(7);
    assert(rotating.dequeue() == 7);
}

void limitsAndRecursiveOwnership() {
    auto dictionary = Dictionary<Integer,Integer>::Empty();
    auto set = HashSet<Integer>::Empty();
    auto queue = Queue<Integer>::Empty();
    auto stack = Stack<Integer>::Empty();
    for (Integer index = 0; index < 4096; ++index) {
        dictionary.add(index, index);
        assert(set.add(index));
        queue.enqueue(index);
        stack.push(index);
    }
    expectFailure([&] { dictionary.add(4096, 0); });
    expectFailure([&] { set.add(4096); });
    expectFailure([&] { queue.enqueue(4096); });
    expectFailure([&] { stack.push(4096); });
    assert(dictionary.count() == 4096 && set.count() == 4096);
    assert(queue.count() == 4096 && stack.count() == 4096);
    dictionary.set(1, 8); // Replacement does not consume another element.
    assert(dictionary.at(1) == 8);
    assert(dictionary.remove(2));
    dictionary.add(4096, 9);
    assert(dictionary.count() == 4096 && dictionary.at(4096) == 9);
    auto outer = List<Queue<Integer>>::Empty();
    expectFailure([&] { outer.add(queue); }); // Outer element plus 4096 nested.
    assert(outer.count() == 0);

    auto errors = Dictionary<std::string,Optional<Error>>::Empty();
    errors.add("first", Optional<Error>::Some(makeError(std::string(700000, 'a'))));
    const auto before = errors;
    expectFailure([&] {
        errors.add("second", Optional<Error>::Some(makeError(std::string(500000, 'b'))));
    });
    assert(errors == before && errors.count() == 1);
    expectFailure([&] {
        errors.set("first", Optional<Error>::Some(makeError(std::string(maximumListBytes, 'c'))));
    });
    assert(errors == before);
    expectFailure([] {
        Array<Optional<Error>>::Create(2, Optional<Error>::Some(makeError(std::string(700000, 'x'))));
    });
    fixture::Nested nested{
        Optional<Result<Error,std::string>>::Some(
            Result<Error,std::string>::Success(makeError(std::string(700000, 'n')))),
        Queue<Integer>::Empty()};
    nested.queue.enqueue(3);
    auto records = Stack<fixture::Nested>::Empty();
    records.push(nested);
    expectFailure([&] { records.push(nested); });
    assert(records.count() == 1);
    nested.error = Optional<Result<Error,std::string>>::Some(
        Result<Error,std::string>::Success(Error{std::string("\xff", 1)}));
    expectFailure([&] { records.push(nested); });
    assert(records.count() == 1);
    auto wrapped = Optional<Dictionary<std::string,Optional<Error>>>::Some(errors);
    validate(wrapped);
    assert(ownedBytes(wrapped) > 700000);
    auto pairs = List<KeyValuePair<std::string,Error>>::Empty();
    pairs.add({"key", makeError(std::string(700000, 'p'))});
    expectFailure([&] { pairs.add({"other", makeError(std::string(500000, 'q'))}); });
}

void failedCopiesAndAllocations() {
    auto queue = Queue<fixture::Fragile>::Empty();
    queue.enqueue(fixture::Fragile(7));
    const auto bytes = ownedBytes(queue);
    fixture::Fragile::failCopy = true;
    bool rejected = false;
    try {
        queue.dequeue();
    } catch (const fixture::CopyFailure&) {
        rejected = true;
    }
    fixture::Fragile::failCopy = false;
    assert(rejected && queue.count() == 1 && queue.peek().number == 7);
    assert(ownedBytes(queue) == bytes);

    std::size_t failures = 0;
    std::size_t successes = 0;
    for (Integer failurePoint = 0; failurePoint < 12; ++failurePoint) {
        auto values = Dictionary<std::string,std::string>::Empty();
        values.add("first", "baseline");
        const auto expected = values.values();
        const auto previousBytes = ownedBytes(values);
        std::string key(200, 'k');
        std::string value(200, 'v');
        allocationsUntilFailure = failurePoint;
        bool allocationFailed = false;
        try {
            values.add(std::move(key), std::move(value));
        } catch (const std::bad_alloc&) {
            allocationFailed = true;
        }
        allocationsUntilFailure = -1;
        if (allocationFailed) {
            ++failures;
            assert(values.values() == expected);
            assert(values.count() == 1 && values.at("first") == "baseline");
            assert(ownedBytes(values) == previousBytes);
            assert(!values.containsKey(std::string(200, 'k')));
        } else {
            ++successes;
            assert(values.count() == 2);
            assert(values.at(std::string(200, 'k')) == std::string(200, 'v'));
        }
        values.set("first", "still consistent");
        assert(values.at("first") == "still consistent");
    }
    assert(failures != 0 && successes != 0);
}

int main() {
    arraysAndCopies();
    dictionariesAndSets();
    queuesAndStacks();
    limitsAndRecursiveOwnership();
    failedCopiesAndAllocations();
}
