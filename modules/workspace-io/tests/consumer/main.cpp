#include <pipelang/workspace/WorkspaceIo.hpp>
int main() {
    return pipelang::workspace::Hashing::text("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" ? 0 : 1;
}
