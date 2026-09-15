#pragma once

#include "LamaPon/Online/DiscordPresence.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace LamaPon::Detail
{
    // Discordクライアントを起動せずにPresenceを検証するための
    // backendです。ヘッダーだけで完結するので、テストからも
    // ゲーム側のデバッグ用途からも使えます。
    class FakeDiscordPresenceBackend final
        : public DiscordPresenceBackend
    {
    public:
        // Discord未導入・未起動を再現します。
        bool initializeSucceeds{ true };
        bool available{ true };
        bool setActivitySucceeds{ true };

        std::string applicationId;
        std::size_t initializeCount{};
        std::size_t shutdownCount{};
        std::size_t clearCount{};
        std::size_t tickCount{};
        std::vector<DiscordActivity> activities;

        [[nodiscard]] bool Initialize(
            const std::string_view id) override
        {
            ++initializeCount;
            applicationId = id;
            if (!initializeSucceeds)
            {
                m_lastError =
                    "Fake backend refused to initialize.";
                return false;
            }
            m_initialized = true;
            m_lastError.clear();
            return true;
        }

        void Shutdown() noexcept override
        {
            ++shutdownCount;
            m_initialized = false;
        }

        [[nodiscard]] bool SetActivity(
            const DiscordActivity& activity) override
        {
            if (!setActivitySucceeds)
            {
                m_lastError =
                    "Fake backend refused the activity.";
                return false;
            }
            activities.push_back(activity);
            m_current = activity;
            m_lastError.clear();
            return true;
        }

        void ClearActivity() noexcept override
        {
            ++clearCount;
            m_current.reset();
        }

        void Tick(float) noexcept override
        {
            ++tickCount;
        }

        [[nodiscard]] bool IsAvailable() const noexcept override
        {
            return m_initialized && available;
        }

        [[nodiscard]] std::string_view
            LastError() const noexcept override
        {
            return m_lastError;
        }

        [[nodiscard]] const std::optional<DiscordActivity>&
            CurrentActivity() const noexcept
        {
            return m_current;
        }

    private:
        bool m_initialized{};
        std::string m_lastError;
        std::optional<DiscordActivity> m_current;
    };
}
