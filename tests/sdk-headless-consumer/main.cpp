#include "Model.hpp"
#include <cmath>
#include <type_traits>

int main()
{
    headless_fixture::ContactModel model;
    static_assert(std::is_same_v<decltype(model.Half(0.0f)), float>);
    if (model.Half(1.5f) != 0.75f)
        return 1;
    if (!std::signbit(model.Half(-0.0f)))
        return 2;
    return 0;
}
