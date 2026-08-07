#pragma once

/**
 * @file dispatch.hpp
 * @brief Internal helper guaranteeing that an operation completion always runs.
 */

#include <functional>
#include <memory>
#include <utility>
#include <wlonvif/wlonvif.hpp>

namespace wlonvif::detail
{
    /**
     * @brief Deliver @p work through @p runtime, or inline when it cannot accept it.
     *
     * Runtime::post() refuses work once shutdown has begun. A completion dropped
     * there never runs, so every caller blocked on the operation waits forever with
     * no deadline to rescue it. Running the completion on the calling thread keeps
     * the exactly-once contract even while the runtime is being torn down.
     */
    inline void dispatch(const std::shared_ptr<Runtime>& runtime, std::function<void()> work)
    {
        if (runtime && runtime->post(work))
        {
            return;
        }
        work();
    }
} // namespace wlonvif::detail
