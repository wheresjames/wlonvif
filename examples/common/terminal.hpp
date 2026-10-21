#pragma once

/**
 * @file terminal.hpp
 * @brief Terminal prompts, private to example_support's own sources.
 */

#include <string>
#include <wlonvif/wlonvif.hpp>

namespace wlonvif::examples::detail
{
    /// An invalid-argument Error for command-line and prompt problems.
    Error argument_error(std::string message);

    /// Whether standard input is a terminal someone can type into.
    bool input_is_terminal();

    /// Ask for a username on the terminal; fails when there is no terminal.
    Result<std::string> prompt_username();

    /// Ask for a password on the terminal without echoing it.
    Result<std::string> prompt_password();
} // namespace wlonvif::examples::detail
