#pragma once

#include "LamaPon/Core/HttpClient.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace LamaPon::Detail
{
    // Discord IDをセーブの主キーにせず、バックエンドが発行するplayerIdを使います。
    struct OnlinePlayerProfile final
    {
        // バックエンドの内部プレイヤーID
        std::string playerId;
        // プレイヤーの表示名
        std::string displayName;
        // アバター画像のHTTPS URL
        std::string avatarUrl;
        // 連携した認証プロバイダー
        std::string linkedProvider;
    };

    struct OnlineSession final
    {
        // サービスのアクセス用token
        std::string accessToken;
        // セッション更新用token
        std::string refreshToken;
        // アクセスtokenの有効秒数
        std::uint32_t expiresInSeconds{};
        // ログインしたプレイヤー情報
        OnlinePlayerProfile player;
    };

    struct DiscordLoginTransaction final
    {
        // ログイン要求の識別子
        std::string transactionId;
        // 承認確認用の秘密token
        std::string pollToken;
        // ブラウザーで開く認可URL
        std::string authorizationUrl;
        // ログイン要求の有効秒数
        std::uint32_t expiresInSeconds{};
        // 承認確認の推奨間隔・秒
        std::uint32_t pollIntervalSeconds{ 1 };
    };

    struct DiscordLoginStartResult final
    {
        // ブラウザーログイン要求
        DiscordLoginTransaction transaction;
        // 安全に診断するエラー識別子
        std::string errorCode;
        // 通信または検証の失敗理由
        std::string errorMessage;

        // ログイン開始のエラーがないかを返す。
        [[nodiscard]] bool Succeeded() const noexcept
        {
            return errorCode.empty();
        }
    };

    enum class DiscordLoginPollStatus
    {
        Pending,
        Authorized,
        Denied,
        Expired,
        Failed
    };

    struct DiscordLoginPollResult final
    {
        // ブラウザーログインの進行状態
        DiscordLoginPollStatus status{
            DiscordLoginPollStatus::Failed
        };
        // 承認時のサービスセッション
        OnlineSession session;
        // 再確認までの待機秒数
        std::uint32_t retryAfterSeconds{ 1 };
        // 安全に診断するエラー識別子
        std::string errorCode;
        // 通信または検証の失敗理由
        std::string errorMessage;
    };

    struct OnlineSessionResult final
    {
        // 更新したサービスセッション
        OnlineSession session;
        // 安全に診断するエラー識別子
        std::string errorCode;
        // 通信または検証の失敗理由
        std::string errorMessage;

        // セッション更新のエラーがないかを返す。
        [[nodiscard]] bool Succeeded() const noexcept
        {
            return errorCode.empty();
        }
    };

    using OnlineHttpSender =
        std::function<HttpResponse(const HttpRequest&)>;

    // Discordの秘密やtokenをゲームへ置かず、LamaPonバックエンドを介してログインします。
    // 同期メソッドはゲームループから直接呼ばず、OnlineServicesの非同期ラッパーで使います。
    class DiscordAuthClient final
    {
    public:
        // 接続先と同期HTTP送信処理を設定する(serviceBaseUrl: サービスの基点URL, allowInsecureLoopback: ローカルHTTPを許可するか, sender: HTTP送信・空なら標準, gameId: ゲームID・空なら共通, environmentId: バックエンドの環境ID)。
        explicit DiscordAuthClient(
            std::string serviceBaseUrl,
            bool allowInsecureLoopback = false,
            OnlineHttpSender sender = {},
            std::string gameId = {},
            std::string environmentId = "production");

        // ブラウザーログインを開始し、認可URLと承認確認情報を返す。
        [[nodiscard]] DiscordLoginStartResult
            BeginLogin() const;
        // ブラウザー承認の結果を問い合わせる(transactionId: ログイン要求の識別子, pollToken: 承認確認用の秘密token)。
        [[nodiscard]] DiscordLoginPollResult
            PollLogin(
                std::string_view transactionId,
                std::string_view pollToken) const;
        // サービスのセッションを更新する(refreshToken: セッション更新用token)。
        [[nodiscard]] OnlineSessionResult
            RefreshSession(std::string_view refreshToken) const;
        // サービスのログアウトを要求する(accessToken: サービスのアクセスtoken)。
        [[nodiscard]] bool Logout(
            std::string_view accessToken) const;

        // 正規化した基点URLを借用する。
        [[nodiscard]] const std::string& ServiceBaseUrl() const noexcept
        {
            return m_serviceBaseUrl;
        }

    private:
        // リダイレクトなしで最大64KiBの応答を読む(path: 基点に追加するパス, json: JSON本文のバイト列, bearerToken: 任意の認証token)。
        [[nodiscard]] HttpResponse PostJson(
            std::string_view path,
            std::string_view json,
            std::string_view bearerToken = {}) const;

        // 正規化したサービスの基点URL
        std::string m_serviceBaseUrl;
        // ローカルHTTPの許可状態
        bool m_allowInsecureLoopback{};
        // 同期HTTPの送信処理
        OnlineHttpSender m_sender;
        // ゲーム用の名前空間ID
        std::string m_gameId;
        // 環境用の名前空間ID
        std::string m_environmentId;
    };
}
