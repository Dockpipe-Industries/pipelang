#include "core_json.hpp"
#include "emitter.hpp"
#include "float64_profile.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

int main() {
    try {
        std::string input;
        std::array<char, 8192> buffer{};
        while (std::cin) {
            std::cin.read(buffer.data(), buffer.size());
            const auto count = static_cast<std::size_t>(std::cin.gcount());
            if (count > pipelang::native::byteLimit - input.size())
                throw std::runtime_error("emitter input limit");
            input.append(buffer.data(), count);
        }
        if (!std::cin.eof())
            throw std::runtime_error("failed reading emitter input");
        const auto envelope = pipelang::native::parseEmitterEnvelope(input);
        const auto contract = pipelang::native::textMember(envelope, "contract");
        pipelang::native::Json generated;
        if (contract == pipelang::native::float64RequestContract) {
            generated = pipelang::native::generateFloat64Comparison(envelope);
        } else {
            const auto program = pipelang::native::pilotProgram(envelope);
            pipelang::native::Emitter emitter;
            generated = emitter.generate(program);
        }
        const auto response = generated.dump();
        // No response is written until parsing, emission and JSON serialization
        // have all completed successfully.
        std::cout << response << '\n';
        if (!std::cout)
            throw std::runtime_error("failed writing emitter response");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pipelang-cpp-emitter: " << error.what() << '\n';
        return 2;
    }
}
