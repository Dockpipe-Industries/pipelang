#include <value-generated/StandaloneConstants.hpp>
#include "Model.hpp"
#include <value-generated/Model.hpp>

#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected independently supplied counter value");
        const std::string_view input(argv[1]);
        int expected = 0;
        const auto parsed = std::from_chars(input.data(), input.data() + input.size(), expected);
        require(parsed.ec == std::errc{} && parsed.ptr == input.data() + input.size(), "invalid expected value");
        source_set_fixture::App app;
        source_set_fixture::Monitor monitor;
        source_set_values::ValueModel values;
        static_assert(source_set_values::ValueModel::Gates == 18);
        static_assert(source_set_values::constants::ValueData::CourseDimensions::Gates == 18);
        require(values.ReadSpacing() == source_set_values::constants::ValueData::CourseDimensions::GateSpacing,
                "imported constant identity or closed float32 expression differs");
        require(values.Read() == 42, "v2 closed value modules did not compile through the installed SDK");
        require(app.Read() == expected, "inherited/interface dispatch did not use the source dependency");
        require(monitor.Read() == expected + 6, "second exported root did not initialize independently");
        require(app.Increment().is_success(), "source object mutation failed");
        require(app.Read() == expected + 1 && monitor.Read() == expected + 6,
                "mutation escaped its exported root's object ownership");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Source-set installed consumer failed: " << error.what() << '\n';
        return 1;
    }
}
