// Game ModuleのDiscord Rich Presenceを公式Social SDKへ中継します。
// OAuthなしでApplication IDだけを使います。
#pragma once

// package.jsonのnative.definesでSDKの利用有無を切り替えます。
#if defined(LAMAPON_DISCORD_SOCIAL_SDK)

#include "LamaPon/Online/DiscordPresence.h"

#include <discordpp.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace LamaPonPackages
{
        // SDK呼び出しはApplicationスレッドで行い、Tickでは待機しません。
    class DiscordSocialPresenceBackend final
        : public LamaPon::DiscordPresenceBackend
    {
    public:
        // SDKアダプターを生成します。
        DiscordSocialPresenceBackend();
        // SDK接続と応答コールバックを終了します。
        ~DiscordSocialPresenceBackend() override;

        // Discord接続を初期化します(applicationId: 10進数のApplication ID)。
        [[nodiscard]] bool Initialize(
            std::string_view applicationId) override;
        // SDK接続を終了します。
        void Shutdown() noexcept override;

        // Discord表示を更新します(activity: 表示内容)。
        [[nodiscard]] bool SetActivity(
            const LamaPon::DiscordActivity& activity) override;
        // Discord表示を消去します。
        void ClearActivity() noexcept override;

        // SDK応答を処理します(elapsedSeconds: 経過秒数)。
        void Tick(float elapsedSeconds) noexcept override;

        // SDKが利用可能か返します。
        [[nodiscard]] bool IsAvailable() const noexcept override;
        // 最後に発生したエラーを返します。
        [[nodiscard]] std::string_view
            LastError() const noexcept override;

    private:
        // UpdateRichPresenceの応答はRunCallbacksと同じスレッドで処理します。
        // 非同期応答を受け取るSDKクライアント
        std::shared_ptr<discordpp::Client> m_client;
        // 破棄後の応答を無効にする共有フラグ
        std::shared_ptr<bool> m_alive;
        // 最後のSDKエラー
        std::string m_lastError;
        // 現在の接続可用性
        bool m_available{};
    };
}

#endif
