#include <wlonvif/wlonvif.hpp>

namespace wlonvif
{
    Runtime::Runtime()
        : worker_(
              [this]
              {
                  run();
              }
          )
    {
    }
    Runtime::~Runtime()
    {
        (void)shutdown();
    }
    Result<std::shared_ptr<Runtime>> Runtime::create()
    {
        try
        {
            return std::shared_ptr<Runtime>(new Runtime);
        }
        catch (const std::exception& e)
        {
            return Error{ErrorCategory::internal, {}, e.what()};
        }
    }
    bool Runtime::post(std::function<void()> fn) noexcept
    {
        try
        {
            std::lock_guard lock(mutex_);
            if (stopping_)
            {
                return false;
            }
            queue_.push(std::move(fn));
            cv_.notify_one();
            return true;
        }
        catch (...)
        {
            return false;
        }
    }
    Result<void> Runtime::shutdown()
    {
        {
            std::lock_guard lock(mutex_);
            if (stopping_)
            {
                if (!worker_.joinable())
                {
                    return {};
                }
            }
            stopping_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id())
        {
            worker_.join();
        }
        return {};
    }
    void Runtime::run()
    {
        for (;;)
        {
            std::function<void()> fn;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(
                    lock,
                    [&]
                    {
                        return stopping_ || !queue_.empty();
                    }
                );
                if (queue_.empty() && stopping_)
                {
                    return;
                }
                fn = std::move(queue_.front());
                queue_.pop();
            }
            try
            {
                fn();
            }
            catch (...)
            {
            }
        }
    }
    bool CancellationToken::stop_requested() const noexcept
    {
        return state_ && state_->load();
    }
} // namespace wlonvif
