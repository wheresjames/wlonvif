#include <chrono>
#include <iostream>
#include <thread>
int main()
{
    const char frame[] = {char(0xff), char(0xd8), 'f', 'a', 'k', 'e', '-', 'j', 'p', 'e', 'g',
                          char(0xff), char(0xd9)};
    for (int index = 0; index < 4; ++index)
    {
        std::cout.write(frame, sizeof(frame));
        std::cout.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}
