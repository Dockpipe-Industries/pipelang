#include <cstdio>
#include <limits>

extern "C" int pipelang_fixture_add(int first, int second);

int main() {
    if (pipelang_fixture_add(20, 22) != 42)
        return 1;
    if (pipelang_fixture_add(-10, 10) != 0)
        return 2;
    if (pipelang_fixture_add(std::numeric_limits<int>::max(), 1) != std::numeric_limits<int>::min())
        return 3;
    std::puts("NativeAOT UnmanagedCallersOnly C export linked and executed");
    return 0;
}
