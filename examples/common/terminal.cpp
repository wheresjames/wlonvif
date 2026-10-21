/**
 * @file terminal.cpp
 * @brief Username and password prompts, with echo turned off for the password.
 */

#include "terminal.hpp"

#include <cstdio>
#include <iostream>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace wlonvif::examples::detail
{
    Error argument_error(std::string message)
    {
        return Error{ErrorCategory::invalid_argument, {}, std::move(message)};
    }

    bool input_is_terminal()
    {
#ifdef _WIN32
        return _isatty(_fileno(stdin)) != 0;
#else
        return isatty(fileno(stdin)) != 0;
#endif
    }

    Result<std::string> prompt_username()
    {
        if (!input_is_terminal())
        {
            return argument_error("no terminal available for username prompt");
        }
        // Prompts go to stderr, so stdout stays clean for output such as --json.
        std::cerr << "Username: " << std::flush;
        std::string username;
        if (!std::getline(std::cin, username) || username.empty())
        {
            return argument_error("username is required when a password is supplied");
        }
        return username;
    }

    Result<std::string> prompt_password()
    {
        std::cerr << "Password: " << std::flush;
#ifdef _WIN32
        HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        if (input == INVALID_HANDLE_VALUE || !GetConsoleMode(input, &mode))
        {
            return argument_error("no terminal available for password prompt");
        }
        SetConsoleMode(input, mode & ~ENABLE_ECHO_INPUT);
        std::string password;
        std::getline(std::cin, password);
        SetConsoleMode(input, mode);
#else
        if (!isatty(fileno(stdin)))
        {
            return argument_error("no terminal available for password prompt");
        }
        termios original{};
        if (tcgetattr(fileno(stdin), &original) != 0)
        {
            return argument_error("unable to configure password prompt");
        }
        // Turn echo off while the password is typed, then restore the terminal.
        auto hidden = original;
        hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
        (void)tcsetattr(fileno(stdin), TCSAFLUSH, &hidden);
        std::string password;
        std::getline(std::cin, password);
        (void)tcsetattr(fileno(stdin), TCSAFLUSH, &original);
#endif
        std::cerr << '\n'; // The Enter key was not echoed either.
        return password;
    }
} // namespace wlonvif::examples::detail
