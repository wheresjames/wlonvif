#pragma once

/**
 * @file curl_init.hpp
 * @brief Internal helper serializing libcurl's global initialization.
 */

#include <curl/curl.h>
#include <mutex>

namespace wlonvif::detail
{
    /**
     * @brief Initialize libcurl exactly once per process.
     *
     * curl_global_init() is documented as not thread safe, and both the transport
     * and the discovery worker reach it from arbitrary threads.
     */
    inline void initialize_curl()
    {
        static std::once_flag flag;
        std::call_once(flag, [] { (void)curl_global_init(CURL_GLOBAL_DEFAULT); });
    }
} // namespace wlonvif::detail
