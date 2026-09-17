// LamaPonのDiscord Rich Presenceを、Discord公式のSocial SDKへ
// つなぐアダプターです。
//
//   Game / Script -> LamaPon::DiscordPresence
//                 -> LamaPon::DiscordPresenceBackend   ← この実装
//                 -> discordpp::Client (Discord Social SDK)
//
// ゲーム側のコードがこのヘッダーをincludeする必要はありません。
// パッケージを入れるだけで、Game Moduleの読み込み時にアダプターが
// 自動登録されます。
//
// Discordアカウント連携（ログイン）とは独立しています。このアダプター
// はDiscordのOAuthを一切行わず、Application IDだけでRich Presenceを
// 表示します。access tokenもrefresh tokenも扱いません。
#pragma once

// SDKが未配置のままGame Moduleへ紛れ込んでも、リンクエラーではなく
// 「何も持たないファイル」になるようにします。このマクロは
// package.jsonのnative.definesから渡されます。
#if defined(LAMAPON_DISCORD_SOCIAL_SDK)

#include "LamaPon/Online/DiscordPresence.h"

// SDK本体のヘッダーです。package.jsonのnative.includeDirectories
// （sdk/include）が通っていれば見つかります。前方宣言にすると
// SDK側のclass/structの綴りへ依存してしまうため、素直に読みます。
#include <discordpp.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace LamaPonPackages
{
    // すべてApplicationを動かすスレッドから呼ばれます。Tick()は毎
    // フレーム呼ばれるため、ブロックしません。
    class DiscordSocialPresenceBackend final
        : public LamaPon::DiscordPresenceBackend
    {
    public:
        DiscordSocialPresenceBackend();
        ~DiscordSocialPresenceBackend() override;

        [[nodiscard]] bool Initialize(
            std::string_view applicationId) override;
        void Shutdown() noexcept override;

        [[nodiscard]] bool SetActivity(
            const LamaPon::DiscordActivity& activity) override;
        void ClearActivity() noexcept override;

        void Tick(float elapsedSeconds) noexcept override;

        [[nodiscard]] bool IsAvailable() const noexcept override;
        [[nodiscard]] std::string_view
            LastError() const noexcept override;

    private:
        // UpdateRichPresenceの結果はcallbackで後から届きます。
        // callbackはRunCallbacks()の中、つまり同じスレッドから
        // 呼ばれるので、追加の排他は要りません。
        std::shared_ptr<discordpp::Client> m_client;
        std::shared_ptr<bool> m_alive;
        std::string m_lastError;
        bool m_available{};
    };
}

#endif
