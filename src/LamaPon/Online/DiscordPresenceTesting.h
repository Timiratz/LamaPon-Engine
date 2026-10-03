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
    // Discordクライアントなしで初期化・表示・切断を再現するバックエンドです。
    class FakeDiscordPresenceBackend final
        : public DiscordPresenceBackend
    {
    public:
        // 初期化を成功させるか
        bool initializeSucceeds{ true };
        // 初期化後に利用可能とするか
        bool available{ true };
        // 表示要求を成功させるか
        bool setActivitySucceeds{ true };

        // 初期化に渡された公開ID
        std::string applicationId;
        // 初期化の呼び出し回数
        std::size_t initializeCount{};
        // 終了の呼び出し回数
        std::size_t shutdownCount{};
        // 表示消去の呼び出し回数
        std::size_t clearCount{};
        // 更新の呼び出し回数
        std::size_t tickCount{};
        // 受け取った表示要求の履歴
        std::vector<DiscordActivity> activities;

        // 成功設定に従って初期化を再現する(id: ゲームの公開ID)。
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

        // 終了回数を数え、未初期化へ戻す。
        void Shutdown() noexcept override
        {
            ++shutdownCount;
            m_initialized = false;
        }

        // 成功設定に従って表示内容を記録する(activity: 表示要求)。
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

        // 消去回数を数え、現在の表示を消す。
        void ClearActivity() noexcept override
        {
            ++clearCount;
            m_current.reset();
        }

        // 更新回数を数える。
        void Tick(float) noexcept override
        {
            ++tickCount;
        }

        // 初期化済みで利用可能設定かを返す。
        [[nodiscard]] bool IsAvailable() const noexcept override
        {
            return m_initialized && available;
        }

        // 再現した失敗理由を借用する。
        [[nodiscard]] std::string_view
            LastError() const noexcept override
        {
            return m_lastError;
        }

        // 現在の表示要求を借用する。
        [[nodiscard]] const std::optional<DiscordActivity>&
            CurrentActivity() const noexcept
        {
            return m_current;
        }

    private:
        // 初期化済みの状態
        bool m_initialized{};
        // 再現した失敗理由
        std::string m_lastError;
        // 消去されていない表示要求
        std::optional<DiscordActivity> m_current;
    };
}
