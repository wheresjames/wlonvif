#include <wlonvif/wlonvif.hpp>
#include <openssl/crypto.h>

namespace wlonvif
{
    struct Runtime::State
    {
        std::mutex mutex;
        std::condition_variable cv;
        std::queue<std::function<void()>> queue;
        bool stopping{false};
        std::map<std::size_t, std::function<void()>> shutdown_participants;
        std::size_t next_shutdown_participant{1};
    };

    std::string_view library_version() noexcept { return "0.1.0"; }
    LibraryFeatures library_features() noexcept { return {}; }
    Runtime::Runtime() : state_(std::make_shared<State>())
    {
        worker_ = std::thread([state = state_] { run(std::move(state)); });
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
            auto state = state_;
            std::lock_guard lock(state->mutex);
            if (state->stopping)
            {
                return false;
            }
            state->queue.push(std::move(fn));
            state->cv.notify_one();
            return true;
        }
        catch (...)
        {
            return false;
        }
    }
    Result<void> Runtime::shutdown()
    {
        std::lock_guard shutdown_lock(shutdown_mutex_);
        auto state = state_;
        std::map<std::size_t, std::function<void()>> participants;
        {
            std::lock_guard lock(state->mutex);
            if (state->stopping)
            {
                if (!worker_.joinable())
                {
                    return {};
                }
            }
            state->stopping = true;
            participants.swap(state->shutdown_participants);
        }
        for (auto& [identifier, participant] : participants)
        {
            (void)identifier;
            try { participant(); } catch (...) {}
        }
        state->cv.notify_all();
        if (worker_.joinable() && worker_.get_id() == std::this_thread::get_id())
        {
            // The worker uses State rather than Runtime, so it is safe for the
            // Runtime object to disappear while the loop finishes this task.
            worker_.detach();
        }
        else if (worker_.joinable())
        {
            worker_.join();
        }
        return {};
    }
    std::size_t Runtime::register_shutdown_participant(std::function<void()> participant)
    {
        auto state = state_;
        std::lock_guard lock(state->mutex);
        if (state->stopping) return 0;
        const auto identifier = state->next_shutdown_participant++;
        state->shutdown_participants.emplace(identifier, std::move(participant));
        return identifier;
    }
    void Runtime::unregister_shutdown_participant(std::size_t identifier) noexcept
    {
        auto state = state_;
        std::lock_guard lock(state->mutex);
        state->shutdown_participants.erase(identifier);
    }
    void Runtime::run(std::shared_ptr<State> state)
    {
        for (;;)
        {
            std::function<void()> fn;
            {
                std::unique_lock lock(state->mutex);
                state->cv.wait(
                    lock,
                    [&]
                    {
                        return state->stopping || !state->queue.empty();
                    }
                );
                if (state->queue.empty() && state->stopping)
                {
                    return;
                }
                fn = std::move(state->queue.front());
                state->queue.pop();
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
        return (state_ && state_->load()) || (other_ && other_->load());
    }

    CredentialLease::CredentialLease(std::string username, std::string password, bool cleartext)
        : username_(std::move(username)), password_(std::move(password)),
          allow_cleartext_basic_(cleartext) {}
    CredentialLease::~CredentialLease() { erase(); }
    CredentialLease::CredentialLease(CredentialLease&& other) noexcept
        : username_(std::move(other.username_)), password_(std::move(other.password_)),
          allow_cleartext_basic_(other.allow_cleartext_basic_) { other.erase(); }
    CredentialLease& CredentialLease::operator=(CredentialLease&& other) noexcept
    {
        if (this != &other)
        {
            erase();
            username_ = std::move(other.username_);
            password_ = std::move(other.password_);
            allow_cleartext_basic_ = other.allow_cleartext_basic_;
            other.erase();
        }
        return *this;
    }
    void CredentialLease::erase() noexcept
    {
        if (!username_.empty()) OPENSSL_cleanse(username_.data(), username_.size());
        if (!password_.empty()) OPENSSL_cleanse(password_.data(), password_.size());
        username_.clear();
        password_.clear();
        allow_cleartext_basic_ = false;
    }

    namespace
    {
        class FixedCredentialProvider final : public CredentialProvider
        {
          public:
            explicit FixedCredentialProvider(Credentials value) : value_(std::move(value)) {}
            ~FixedCredentialProvider() override { close(); }
            Task<Result<CredentialLease>> acquire(CallOptions options) override
            {
                return Task<Result<CredentialLease>>(
                    [this, options](auto done)
                    {
                        Result<CredentialLease> result = Error{ErrorCategory::internal};
                        {
                            std::lock_guard lock(mutex_);
                            if (closed_)
                                result = Error{ErrorCategory::closed_handle, {}, "credential provider is closed"};
                            else if (options.cancellation.stop_requested())
                                result = Error{ErrorCategory::cancelled};
                            else if (std::chrono::steady_clock::now() >= options.deadline)
                                result = Error{ErrorCategory::timeout};
                            else
                                result = CredentialLease{
                                    value_.username, value_.password, value_.allow_cleartext_basic
                                };
                        }
                        // A completion may synchronously close the session and call
                        // back into this provider, so never invoke it under mutex_.
                        done(std::move(result));
                    }
                );
            }
            void close() noexcept override
            {
                std::lock_guard lock(mutex_);
                if (closed_) return;
                closed_ = true;
                if (!value_.username.empty()) OPENSSL_cleanse(value_.username.data(), value_.username.size());
                if (!value_.password.empty()) OPENSSL_cleanse(value_.password.data(), value_.password.size());
                value_ = {};
            }
          private:
            std::mutex mutex_;
            Credentials value_;
            bool closed_{};
        };
    }
    std::shared_ptr<CredentialProvider> make_fixed_credential_provider(Credentials credentials)
    {
        return std::make_shared<FixedCredentialProvider>(std::move(credentials));
    }
} // namespace wlonvif
