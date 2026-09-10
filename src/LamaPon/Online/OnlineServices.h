#pragma once

#include "LamaPon/Core/Api.h"

#include <cstdint>
#include <memory>
#include <string>

namespace LamaPon
{
    class OnlineServices;

    struct OnlineServiceConfiguration final
    {
        // Discordのclient_secretをゲームへ設定してはいけません。
        // ここにはLamaPon用バックエンドのベースURLだけを指定します。
        std::string serviceBaseUrl;

        // 配布ビルドではfalseのまま使います。ローカル開発用の
        // 127.0.0.1/localhostバックエンドだけをHTTPで試す設定です。
        bool allowInsecureLoopback{};
    };

    enum class OnlineAccountState : std::uint8_t
    {
        Unconfigured,
        SignedOut,
        StartingSignIn,
        WaitingForAuthorization,
        PollingAuthorization,
        SignedIn,
        SigningOut,
        Error
    };

    // ゲームへ公開してよいプロフィール情報だけを保持します。
    // Discord IDではなく、バックエンドが発行したplayerIdを
    // セーブデータの所有者として利用します。
    struct OnlinePlayerProfile final
    {
        std::string playerId;
        std::string displayName;
        std::string avatarUrl;
        std::string linkedProvider;
    };

    namespace Detail
    {
        class OnlineServicesTestAccess;
    }

    // Discordログインの非同期進行とオンラインアカウント状態を
    // ゲームループ上で管理します。すべての公開メソッドは、
    // Applicationを動かす同じスレッドから呼んでください。
    class OnlineServices final
    {
    public:
        LAMAPON_API OnlineServices();
        explicit LAMAPON_API OnlineServices(
            OnlineServiceConfiguration configuration);
        LAMAPON_API ~OnlineServices();

        OnlineServices(const OnlineServices&) = delete;
        OnlineServices& operator=(const OnlineServices&) = delete;
        OnlineServices(OnlineServices&&) = delete;
        OnlineServices& operator=(OnlineServices&&) = delete;

        // 実行中のログインやサインアウトがある場合はlogic_errorです。
        // 空URLを指定するとUnconfiguredへ戻ります。
        LAMAPON_API void Configure(
            OnlineServiceConfiguration configuration);

        // Applicationが毎フレーム呼び、完了した通信を反映して次の
        // ポーリングを開始します。通信自体はワーカースレッド上です。
        LAMAPON_API void Update(float elapsedSeconds);

        // 開始要求を受理したときだけtrueです。ブラウザー起動は後段で
        // 統合するため、現段階ではAuthorizationUrl()を利用します。
        [[nodiscard]] LAMAPON_API bool BeginDiscordSignIn();
        LAMAPON_API void CancelDiscordSignIn() noexcept;
        LAMAPON_API void SignOut();

        [[nodiscard]] LAMAPON_API OnlineAccountState
            State() const noexcept;
        [[nodiscard]] LAMAPON_API bool IsSignedIn() const noexcept;
        [[nodiscard]] LAMAPON_API const OnlinePlayerProfile&
            Player() const noexcept;
        [[nodiscard]] LAMAPON_API const std::string&
            AuthorizationUrl() const noexcept;
        [[nodiscard]] LAMAPON_API const std::string&
            LastErrorCode() const noexcept;
        [[nodiscard]] LAMAPON_API const std::string&
            LastError() const noexcept;

    private:
        struct Implementation;

        explicit OnlineServices(
            std::unique_ptr<Implementation> implementation);

        friend class Detail::OnlineServicesTestAccess;

        std::unique_ptr<Implementation> m_implementation;
    };

    // Applicationが所有する現在のサービスです。CLIや単体テストでは
    // nullptrを許容します。実体はRuntime DLL側に1つだけ置きます。
    [[nodiscard]] LAMAPON_API OnlineServices*
        ActiveOnlineServices() noexcept;
    LAMAPON_API void SetActiveOnlineServices(
        OnlineServices* services) noexcept;
}
