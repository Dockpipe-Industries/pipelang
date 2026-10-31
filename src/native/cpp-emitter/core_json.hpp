#pragma once

#include <nlohmann/json.hpp>
#include <cstddef>
#include <string>

namespace pipelang::native {

using Json = nlohmann::ordered_json;
inline constexpr std::size_t byteLimit = 16 * 1024 * 1024;

const Json& member(const Json& object, const std::string& name);
std::string textMember(const Json& object, const std::string& name);
int positionMember(const Json& object, const std::string& name);
std::string identityKey(const Json& identity);
Json canonicalIdentity(const Json& identity);
Json canonicalType(const Json& type);
std::string typeKey(const Json& type);
std::string typeName(const Json& type);
Json parseEmitterEnvelope(const std::string& input);
Json pilotProgram(const Json& request);
Json parseRequest(const std::string& input);

}
