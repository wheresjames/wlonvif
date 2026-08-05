#include <wlonvif/wlonvif.hpp>

int main()
{
    auto runtime = wlonvif::Runtime::create();
    if (!runtime)
    {
        return 1;
    }
    return runtime.value()->shutdown() ? 0 : 1;
}
