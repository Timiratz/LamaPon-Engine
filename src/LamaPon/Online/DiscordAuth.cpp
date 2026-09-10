#include "LamaPon/Online/DiscordAuth.h"

#include "LamaPon/Core/PathUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace
{
    using Json = nlohmann::json;

    std::string_view UrlAuthority(
        const std::string_view value,
        const std::string_view scheme)
    {
        if (!value.starts_with(scheme))
        {
            return {};
        }
        const auto begin = scheme.size();
        const auto end = value.find_first_of("/?#", begin);
        return value.substr(
            begin,
            end == std::string_view::npos
                ? value.size() - begin
                : end - begin);
    }

    bool IsDigits(const std::string_view value)
    {
        return !value.empty()
            && std::ranges::all_of(
                value,
                [](const unsigned char character)
                {
                    return character >= '0' && character <= '9';
                });
    }

    bool IsLoopbackBaseUrl(const std::string_view value)
    {
        const auto authority = UrlAuthority(value, "http://");
        if (authority == "127.0.0.1"
            || authority == "localhost"
            || authority == "[::1]")
        {
            return true;
        }
        for (const auto host : {
                std::string_view("127.0.0.1"),
                std::string_view("localhost"),
                std::string_view("[::1]") })
        {
            if (authority.starts_with(host)
                && authority.size() > host.size()
                && authority[host.size()] == ':'
                && IsDigits(authority.substr(host.size() + 1)))
            {
                return true;
            }
        }
        return false;
    }

    bool ContainsUnsafeUrlCharacter(const std::string_view value)
    {
        return std::ranges::any_of(
            value,
            [](const unsigned char character)
            {
                return character <= 0x20 || character == 0x7f;
            });
    }

    std::string NormalizeBaseUrl(
        std::string value,
        const bool allowInsecureLoopback)
    {
        while (!value.empty() && value.back() == '/')
        {
            value.pop_back();
        }
        const bool secure = value.starts_with("https://");
        const bool allowedLoopback = allowInsecureLoopback
            && IsLoopbackBaseUrl(value);
        const auto authority = secure
            ? UrlAuthority(value, "https://")
            : UrlAuthority(value, "http://");
        if ((!secure && !allowedLoopback)
            || authority.empty()
            || authority.find('@') != std::string_view::npos
            || value.size() > 2048
            || value.find_first_of("?#") != std::string::npos
            || ContainsUnsafeUrlCharacter(value))
        {
            throw std::invalid_argument(
                "Online service URL must be an HTTPS base URL. "
                "Only an explicitly enabled loopback URL may use HTTP.");
        }
        return value;
    }

    bool IsSafeOpaqueValue(
        const std::string_view value,
        const std::size_t maxBytes)
    {
        return !value.empty()
            && value.size() <= maxBytes
            && !ContainsUnsafeUrlCharacter(value);
    }

    bool IsSafeErrorCode(const std::string_view value)
    {
        return !value.empty()
            && value.size() <= 128
            && std::ranges::all_of(
                value,
                [](const unsigned char character)
                {
                    return (character >= 'a' && character <= 'z')
                        || (character >= '0' && character <= '9')
                        || character == '_'
                        || character == '-'
                        || character == '.';
                });
    }

    bool IsSafeBrowserUrl(
        const std::string_view value,
        const bool allowInsecureLoopback)
    {
        return !value.empty()
            && value.size() <= 2048
            && !ContainsUnsafeUrlCharacter(value)
            && ((!UrlAuthority(value, "https://").empty()
                    && UrlAuthority(value, "https://").find('@')
                        == std::string_view::npos)
                || (allowInsecureLoopback
                    && IsLoopbackBaseUrl(value)));
    }

    std::string LimitedText(
        const Json& value,
        const char* key,
        const std::size_t maxBytes)
    {
        const auto found = value.find(key);
        if (found == value.end() || !found->is_string())
        {
            return {};
        }
        const auto result = found->get<std::string>();
        if (result.size() > maxBytes)
        {
            return {};
        }
        return result;
    }

    void ReadServiceError(
        const LamaPon::HttpResponse& response,
        std::string& code,
        std::string& message)
    {
        code = "service_error";
        message = "Online service returned HTTP "
            + std::to_string(response.statusCode) + ".";
        try
        {
            const auto json = Json::parse(response.Text());
            const auto* error = &json;
            if (const auto nested = json.find("error");
                nested != json.end() && nested->is_object())
            {
                error = &*nested;
            }
            if (auto parsed = LimitedText(*error, "code", 128);
                IsSafeErrorCode(parsed))
            {
                code = std::move(parsed);
            }
        }
        catch (const std::exception&)
        {
            // HTML等でもステータスだけで安全に診断できます。
        }
    }

    bool ParseSession(
        const Json& json,
        LamaPon::Detail::OnlineSession& session,
        std::string& error)
    {
        const auto accessToken = LimitedText(
            json,
            "accessToken",
            8192);
        const auto refreshToken = LimitedText(
            json,
            "refreshToken",
            8192);
        const auto player = json.find("player");
        if (!IsSafeOpaqueValue(accessToken, 8192)
            || !IsSafeOpaqueValue(refreshToken, 8192)
            || player == json.end()
            || !player->is_object())
        {
            error = "Online service returned an invalid session.";
            return false;
        }

        LamaPon::Detail::OnlinePlayerProfile profile;
        profile.playerId = LimitedText(*player, "id", 128);
        profile.displayName = LimitedText(
            *player,
            "displayName",
            256);
        profile.avatarUrl = LimitedText(
            *player,
            "avatarUrl",
            2048);
        profile.linkedProvider = LimitedText(
            *player,
            "linkedProvider",
            64);
        if (!IsSafeOpaqueValue(profile.playerId, 128)
            || profile.displayName.empty())
        {
            error = "Online service returned an invalid player profile.";
            return false;
        }
        if (!profile.avatarUrl.empty()
            && !IsSafeBrowserUrl(profile.avatarUrl, false))
        {
            error = "Online service returned an invalid avatar URL.";
            return false;
        }

        session.accessToken = accessToken;
        session.refreshToken = refreshToken;
        session.expiresInSeconds = std::clamp(
            json.value("expiresIn", 900u),
            30u,
            86400u);
        session.player = std::move(profile);
        return true;
    }

    std::string TransportMessage(
        const LamaPon::HttpResponse& response)
    {
        return response.transportError.empty()
            ? "Online service request failed."
            : response.transportError;
    }
}

namespace LamaPon::Detail
{
    DiscordAuthClient::DiscordAuthClient(
        std::string serviceBaseUrl,
        const bool allowInsecureLoopback,
        OnlineHttpSender sender)
        : m_serviceBaseUrl(NormalizeBaseUrl(
            std::move(serviceBaseUrl),
            allowInsecureLoopback))
        , m_allowInsecureLoopback(allowInsecureLoopback)
        , m_sender(sender ? std::move(sender) : OnlineHttpSender(HttpSend))
    {
    }

    HttpResponse DiscordAuthClient::PostJson(
        const std::string_view path,
        const std::string_view json,
        const std::string_view bearerToken) const
    {
        HttpRequest request;
        request.url = Utf8ToWide(
            m_serviceBaseUrl + std::string(path));
        request.method = L"POST";
        request.headers.emplace_back(L"Accept", L"application/json");
        request.headers.emplace_back(
            L"Content-Type",
            L"application/json; charset=utf-8");
        if (!bearerToken.empty())
        {
            request.headers.emplace_back(
                L"Authorization",
                L"Bearer " + Utf8ToWide(bearerToken));
        }
        request.body.assign(json.begin(), json.end());
        request.maxResponseBytes = 64u * 1024u;
        request.allowInsecureLoopback = m_allowInsecureLoopback;
        request.followRedirects = false;
        return m_sender(request);
    }

    DiscordLoginStartResult DiscordAuthClient::BeginLogin() const
    {
        DiscordLoginStartResult result;
        const Json requestBody{
            { "provider", "discord" },
            { "platform", "windows" },
            { "protocolVersion", 1 }
        };
        const auto response = PostJson(
            "/v1/auth/login/start",
            requestBody.dump());
        if (!response.TransportSucceeded())
        {
            result.errorCode = "network_error";
            result.errorMessage = TransportMessage(response);
            return result;
        }
        if (response.statusCode != 201)
        {
            ReadServiceError(
                response,
                result.errorCode,
                result.errorMessage);
            return result;
        }

        try
        {
            const auto json = Json::parse(response.Text());
            result.transaction.transactionId = LimitedText(
                json,
                "transactionId",
                512);
            result.transaction.pollToken = LimitedText(
                json,
                "pollToken",
                2048);
            result.transaction.authorizationUrl = LimitedText(
                json,
                "authorizationUrl",
                2048);
            result.transaction.expiresInSeconds = std::clamp(
                json.value("expiresIn", 300u),
                30u,
                900u);
            result.transaction.pollIntervalSeconds = std::clamp(
                json.value("pollInterval", 1u),
                1u,
                10u);
            if (!IsSafeOpaqueValue(
                    result.transaction.transactionId,
                    512)
                || !IsSafeOpaqueValue(
                    result.transaction.pollToken,
                    2048)
                || !IsSafeBrowserUrl(
                    result.transaction.authorizationUrl,
                    m_allowInsecureLoopback))
            {
                throw std::runtime_error(
                    "Online service returned an invalid login transaction.");
            }
        }
        catch (const std::exception&)
        {
            result.transaction = {};
            result.errorCode = "invalid_response";
            result.errorMessage =
                "Online service returned an invalid login response.";
        }
        return result;
    }

    DiscordLoginPollResult DiscordAuthClient::PollLogin(
        const std::string_view transactionId,
        const std::string_view pollToken) const
    {
        DiscordLoginPollResult result;
        if (!IsSafeOpaqueValue(transactionId, 512)
            || !IsSafeOpaqueValue(pollToken, 2048))
        {
            result.errorCode = "invalid_transaction";
            result.errorMessage = "Login transaction is invalid.";
            return result;
        }
        const Json requestBody{
            { "transactionId", transactionId },
            { "pollToken", pollToken }
        };
        const auto response = PostJson(
            "/v1/auth/login/complete",
            requestBody.dump());
        if (!response.TransportSucceeded())
        {
            result.errorCode = "network_error";
            result.errorMessage = TransportMessage(response);
            return result;
        }

        try
        {
            const auto json = Json::parse(response.Text());
            const auto status = LimitedText(json, "status", 32);
            if (response.statusCode == 202 && status == "pending")
            {
                result.status = DiscordLoginPollStatus::Pending;
                result.retryAfterSeconds = std::clamp(
                    json.value("retryAfter", 1u),
                    1u,
                    10u);
                return result;
            }
            if (response.statusCode == 200 && status == "authorized")
            {
                std::string parseError;
                if (!ParseSession(json, result.session, parseError))
                {
                    result.errorCode = "invalid_response";
                    result.errorMessage = std::move(parseError);
                    return result;
                }
                result.status = DiscordLoginPollStatus::Authorized;
                return result;
            }
            if (response.statusCode == 403 && status == "denied")
            {
                result.status = DiscordLoginPollStatus::Denied;
                result.errorCode = "login_denied";
                result.errorMessage = "Discord login was denied.";
                return result;
            }
            if (response.statusCode == 410 && status == "expired")
            {
                result.status = DiscordLoginPollStatus::Expired;
                result.errorCode = "login_expired";
                result.errorMessage = "Discord login expired.";
                return result;
            }
        }
        catch (const std::exception&)
        {
            // 共通エラーへ落とし、応答本文やtokenをログへ出しません。
        }
        ReadServiceError(
            response,
            result.errorCode,
            result.errorMessage);
        return result;
    }

    OnlineSessionResult DiscordAuthClient::RefreshSession(
        const std::string_view refreshToken) const
    {
        OnlineSessionResult result;
        if (!IsSafeOpaqueValue(refreshToken, 8192))
        {
            result.errorCode = "invalid_refresh_token";
            result.errorMessage = "Refresh token is invalid.";
            return result;
        }
        const Json requestBody{
            { "refreshToken", refreshToken }
        };
        const auto response = PostJson(
            "/v1/auth/session/refresh",
            requestBody.dump());
        if (!response.TransportSucceeded())
        {
            result.errorCode = "network_error";
            result.errorMessage = TransportMessage(response);
            return result;
        }
        if (response.statusCode != 200)
        {
            ReadServiceError(
                response,
                result.errorCode,
                result.errorMessage);
            return result;
        }
        try
        {
            const auto json = Json::parse(response.Text());
            if (!ParseSession(
                    json,
                    result.session,
                    result.errorMessage))
            {
                result.errorCode = "invalid_response";
            }
        }
        catch (const std::exception&)
        {
            result.errorCode = "invalid_response";
            result.errorMessage =
                "Online service returned an invalid session response.";
        }
        return result;
    }

    bool DiscordAuthClient::Logout(
        const std::string_view accessToken) const
    {
        if (!IsSafeOpaqueValue(accessToken, 8192))
        {
            return false;
        }
        const auto response = PostJson(
            "/v1/auth/session/logout",
            "{}",
            accessToken);
        return response.TransportSucceeded()
            && (response.statusCode == 200
                || response.statusCode == 204);
    }
}
