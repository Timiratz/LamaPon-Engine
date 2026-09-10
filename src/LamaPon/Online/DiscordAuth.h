#pragma once

#include "LamaPon/Core/HttpClient.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace LamaPon::Detail
{
    struct OnlinePlayerProfile final
    {
        // バックエンドが発行する内部プレイヤーIDです。
        // Discord IDをセーブの主キーとして直接使いません。
        std::string playerId;
        std::string displayName;
        std::string avatarUrl;
        std::string linkedProvider;
    };

    struct OnlineSession final
    {
        std::string accessToken;
        std::string refreshToken;
        std::uint32_t expiresInSeconds{};
        OnlinePlayerProfile player;
    };

    struct DiscordLoginTransaction final
    {
        std::string transactionId;
        std::string pollToken;
        std::string authorizationUrl;
        std::uint32_t expiresInSeconds{};
        std::uint32_t pollIntervalSeconds{ 1 };
    };

    struct DiscordLoginStartResult final
    {
        DiscordLoginTransaction transaction;
        std::string errorCode;
        std::string errorMessage;

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
        DiscordLoginPollStatus status{
            DiscordLoginPollStatus::Failed
        };
        OnlineSession session;
        std::uint32_t retryAfterSeconds{ 1 };
        std::string errorCode;
        std::string errorMessage;
    };

    struct OnlineSessionResult final
    {
        OnlineSession session;
        std::string errorCode;
        std::string errorMessage;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return errorCode.empty();
        }
    };

    using OnlineHttpSender =
        std::function<HttpResponse(const HttpRequest&)>;

    // Discordのclient_secretやDiscord tokenをゲームへ置かず、
    // LamaPonバックエンドを介してログインするクライアントです。
    // 各メソッドは同期処理なので、ゲームループからはOnlineServicesの
    // 非同期ラッパーを通して利用します。
    class DiscordAuthClient final
    {
    public:
        explicit DiscordAuthClient(
            std::string serviceBaseUrl,
            bool allowInsecureLoopback = false,
            OnlineHttpSender sender = {},
            std::string gameId = {},
            std::string environmentId = "production");

        [[nodiscard]] DiscordLoginStartResult
            BeginLogin() const;
        [[nodiscard]] DiscordLoginPollResult
            PollLogin(
                std::string_view transactionId,
                std::string_view pollToken) const;
        [[nodiscard]] OnlineSessionResult
            RefreshSession(std::string_view refreshToken) const;
        [[nodiscard]] bool Logout(
            std::string_view accessToken) const;

        [[nodiscard]] const std::string& ServiceBaseUrl() const noexcept
        {
            return m_serviceBaseUrl;
        }

    private:
        [[nodiscard]] HttpResponse PostJson(
            std::string_view path,
            std::string_view json,
            std::string_view bearerToken = {}) const;

        std::string m_serviceBaseUrl;
        bool m_allowInsecureLoopback{};
        OnlineHttpSender m_sender;
        std::string m_gameId;
        std::string m_environmentId;
    };
}
