// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ByteBufferObjects.hpp"
#include "ByteBufferProject.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

template<class Model>
void verifyOwnership(Model& model) {
    const pipelang::model::ByteBuffer bytes(std::vector<std::uint8_t>{0, 255, 128});
    assert(model.Set(bytes).is_success());
    auto snapshot = model.Read();
    snapshot.add(9);
    assert(model.Read() == bytes);
    assert(model.Append(bytes).is_success());
    assert(model.Length() == 6 && model.Packet() == model.Read());
    const auto before = model.Read();
    assert(model.Fail(256).is_failure());
    assert(model.Read() == before && model.Packet() == before);
    const pipelang::model::ByteBuffer body(std::vector<std::uint8_t>(16384, 255));
    assert(model.Set(body).is_success());
    assert(model.Length() == 16384 && model.Packet() == body);
}

int main() {
    byte_buffer_objects::App objectModel;
    verifyOwnership(objectModel);
    byte_buffer_project::source::Binary::App projectModel;
    verifyOwnership(projectModel);
}
