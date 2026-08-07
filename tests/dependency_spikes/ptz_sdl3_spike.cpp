#include <SDL3/SDL.h>

#include <iostream>

int main()
{
    if (!SDL_Init(SDL_INIT_EVENTS | SDL_INIT_GAMEPAD))
    {
        std::cerr << "SDL initialization failed: " << SDL_GetError() << '\n';
        return 1;
    }
    int count = 0;
    SDL_JoystickID* gamepads = SDL_GetGamepads(&count);
    std::cout << "SDL " << SDL_GetRevision() << "; gamepads=" << count << '\n';
    for (int i = 0; i < count; ++i)
    {
        std::cout << gamepads[i] << " " << SDL_GetGamepadNameForID(gamepads[i]) << '\n';
    }
    SDL_free(gamepads);
    SDL_Event pushed{};
    pushed.type = SDL_EVENT_USER;
    if (!SDL_PushEvent(&pushed))
    {
        std::cerr << "SDL synthetic event failed: " << SDL_GetError() << '\n';
        SDL_Quit();
        return 1;
    }
    SDL_Event received{};
    bool observed = false;
    while (SDL_PollEvent(&received)) observed = observed || received.type == SDL_EVENT_USER;
    SDL_Quit();
    return observed ? 0 : 1;
}
