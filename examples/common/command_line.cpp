/**
 * @file command_line.cpp
 * @brief Options every example accepts, and where credentials come from.
 *
 * Credentials are looked up in this order: `--username`/`--password`, a
 * password file, an environment variable, and finally a terminal prompt. A
 * password on the command line is visible to other users of the machine, so
 * the README recommends the file or the environment variable.
 */

#include "example_support.hpp"
#include "terminal.hpp"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#ifdef _WIN32
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace wlonvif::examples
{
    using detail::argument_error;

    namespace
    {
        /// The first line of a password file, which must be private to its owner.
        Result<std::string> read_password_file(const std::string& path)
        {
#ifndef _WIN32
            struct stat status{};
            if (stat(path.c_str(), &status) != 0)
            {
                return argument_error("unable to inspect password file");
            }
            if ((status.st_mode & (S_IRWXG | S_IRWXO)) != 0)
            {
                return argument_error("password file must not be accessible by group or others");
            }
#endif
            std::ifstream input(path);
            std::string password;
            if (!input || !std::getline(input, password))
            {
                return argument_error("unable to read password file");
            }
            // Files written on Windows end lines with CR LF.
            if (!password.empty() && password.back() == '\r')
            {
                password.pop_back();
            }
            return password;
        }
    } // namespace

    Result<ParsedArguments> parse_common_arguments(int argc, char** argv)
    {
        ParsedArguments parsed;
        for (int index = 1; index < argc; ++index)
        {
            const std::string argument(argv[index]);
            const bool needs_value = argument == "--timeout" || argument == "--username" ||
                                     argument == "--password" || argument == "--password-file" ||
                                     argument == "--password-env";
            if (needs_value && index + 1 >= argc)
            {
                return argument_error("missing value for " + argument);
            }
            if (argument == "--timeout")
            {
                std::size_t timeout = 0;
                if (!parse_size(argv[++index], timeout) || timeout > 3600)
                {
                    return argument_error("timeout must be between 1 and 3600 seconds");
                }
                parsed.common.timeout_seconds = static_cast<int>(timeout);
                parsed.common.timeout_set = true;
            }
            else if (argument == "--username")
            {
                parsed.common.username = argv[++index];
            }
            else if (argument == "--password")
            {
                parsed.common.password = argv[++index];
            }
            else if (argument == "--password-file")
            {
                parsed.common.password_file = argv[++index];
            }
            else if (argument == "--password-env")
            {
                parsed.common.password_environment = argv[++index];
            }
            else if (argument == "--allow-cleartext-basic")
            {
                parsed.common.allow_cleartext_basic = true;
            }
            else if (argument == "--json")
            {
                parsed.common.json = true;
            }
            else if (argument == "--no-progress")
            {
                parsed.common.show_progress = false;
            }
            else
            {
                // Everything else belongs to the example.
                parsed.remaining.push_back(argument);
            }
        }
        return parsed;
    }

    Result<std::optional<Credentials>> load_credentials(const CommonOptions& options)
    {
        std::string username = options.username;
        if (username.empty())
        {
            if (const char* environment_username = std::getenv("WLONVIF_USERNAME"))
            {
                username = environment_username;
            }
        }

        std::optional<std::string> password;
        if (!options.password.empty())
        {
            password = options.password;
        }
        else if (!options.password_file.empty())
        {
            auto loaded = read_password_file(options.password_file);
            if (!loaded)
            {
                return loaded.error();
            }
            password = std::move(loaded.value());
        }
        else if (!options.password_environment.empty())
        {
            if (const char* value = std::getenv(options.password_environment.c_str()))
            {
                password = value;
            }
        }

        // Neither given: anonymous access. One given: ask for the other.
        if (username.empty() && !password)
        {
            return std::optional<Credentials>{};
        }
        if (username.empty())
        {
            auto prompted = detail::prompt_username();
            if (!prompted)
            {
                return prompted.error();
            }
            username = std::move(prompted.value());
        }
        if (!password)
        {
            auto prompted = detail::prompt_password();
            if (!prompted)
            {
                return prompted.error();
            }
            password = std::move(prompted.value());
        }
        return std::optional<Credentials>(
            Credentials{std::move(username), std::move(*password), options.allow_cleartext_basic}
        );
    }

    bool parse_size(std::string_view text, std::size_t& output)
    {
        std::size_t value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0)
        {
            return false;
        }
        output = value;
        return true;
    }

    bool progress_is_terminal()
    {
#ifdef _WIN32
        return _isatty(_fileno(stderr)) != 0;
#else
        return isatty(fileno(stderr)) != 0;
#endif
    }
} // namespace wlonvif::examples
