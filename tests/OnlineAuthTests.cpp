#include <WinSock2.h>

#include "LamaPon/LamaPon.h"
#include "LamaPon/Online/DiscordAuth.h"

#include <nlohmann/json.hpp>

#include <deque>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    struct LoopbackResult final
    {
        LamaPon::HttpResponse response;
        std::string requestText;
    };

    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    LamaPon::HttpResponse JsonResponse(
        const std::uint32_t status,
        const nlohmann::json& body)
    {
        LamaPon::HttpResponse response;
        response.statusCode = status;
        const auto text = body.dump();
        response.body.assign(text.begin(), text.end());
        return response;
    }

    bool HasHeader(
        const LamaPon::HttpRequest& request,
        const std::wstring& name,
        const std::wstring& value)
    {
        for (const auto& header : request.headers)
        {
            if (header.first == name && header.second == value)
            {
                return true;
            }
        }
        return false;
    }

    LoopbackResult RequestLoopbackHeaders()
    {
        WSADATA sockets{};
        if (WSAStartup(MAKEWORD(2, 2), &sockets) != 0)
        {
            throw std::runtime_error("WSAStartup failed.");
        }

        const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET)
        {
            WSACleanup();
            throw std::runtime_error("Could not create loopback socket.");
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(
                listener,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == SOCKET_ERROR
            || listen(listener, 1) == SOCKET_ERROR)
        {
            closesocket(listener);
            WSACleanup();
            throw std::runtime_error("Could not listen on loopback.");
        }
        int addressSize = sizeof(address);
        if (getsockname(
                listener,
                reinterpret_cast<sockaddr*>(&address),
                &addressSize) == SOCKET_ERROR)
        {
            closesocket(listener);
            WSACleanup();
            throw std::runtime_error("Could not read loopback port.");
        }

        std::string requestText;
        std::jthread server(
            [listener, &requestText]
            {
                const SOCKET client = accept(listener, nullptr, nullptr);
                if (client == INVALID_SOCKET)
                {
                    return;
                }
                char requestBytes[2048]{};
                const auto received = recv(
                    client,
                    requestBytes,
                    sizeof(requestBytes),
                    0);
                if (received > 0)
                {
                    requestText.assign(
                        requestBytes,
                        static_cast<std::size_t>(received));
                }
                constexpr std::string_view response =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: 2\r\n"
                    "ETag: revision-7\r\n"
                    "Retry-After: 4\r\n"
                    "X-Request-Id: request-9\r\n"
                    "Connection: close\r\n\r\n{}";
                static_cast<void>(send(
                    client,
                    response.data(),
                    static_cast<int>(response.size()),
                    0));
                shutdown(client, SD_BOTH);
                closesocket(client);
            });

        LamaPon::HttpRequest request;
        request.url = L"http://127.0.0.1:"
            + std::to_wstring(ntohs(address.sin_port))
            + L"?check=headers";
        request.allowInsecureLoopback = true;
        const auto response = LamaPon::HttpSend(request);
        closesocket(listener);
        server.join();
        WSACleanup();
        return { response, std::move(requestText) };
    }
}

int main()
{
    try
    {
        bool insecureRejected{};
        try
        {
            LamaPon::Detail::DiscordAuthClient invalid(
                "http://example.com");
        }
        catch (const std::invalid_argument&)
        {
            insecureRejected = true;
        }
        Require(
            insecureRejected,
            "An insecure remote service URL was accepted.");
        bool disguisedLoopbackRejected{};
        try
        {
            LamaPon::Detail::DiscordAuthClient invalid(
                "http://localhost.example.com",
                true);
        }
        catch (const std::invalid_argument&)
        {
            disguisedLoopbackRejected = true;
        }
        Require(
            disguisedLoopbackRejected,
            "A remote host disguised as loopback was accepted.");

        LamaPon::HttpRequest unsafeRequest;
        unsafeRequest.url = L"http://example.com/api";
        const auto unsafeResponse = LamaPon::HttpSend(unsafeRequest);
        Require(
            !unsafeResponse.TransportSucceeded()
                && unsafeResponse.statusCode == 0,
            "HttpSend attempted an insecure remote request.");

        LamaPon::HttpResponse headerResponse;
        headerResponse.headers.emplace_back(L"ETag", L"revision-7");
        Require(
            headerResponse.Header(L"etag") == L"revision-7",
            "HTTP response header lookup was case-sensitive.");

        const auto actual = RequestLoopbackHeaders();
        Require(
            actual.response.TransportSucceeded()
                && actual.response.statusCode == 200
                && actual.response.Header(L"ETAG") == L"revision-7"
                && actual.response.Header(L"retry-after") == L"4"
                && actual.response.Header(L"x-request-id")
                    == L"request-9",
            "WinHTTP response headers were not captured.");
        Require(
            actual.requestText.starts_with(
                "GET /?check=headers HTTP/1.1\r\n"),
            "A root URL query was sent without the leading slash.");

        LamaPon::HttpRequest redirectWithSecret;
        redirectWithSecret.url = L"https://example.com/start";
        redirectWithSecret.followRedirects = true;
        redirectWithSecret.headers.emplace_back(
            L"Authorization",
            L"Bearer must-not-leak");
        Require(
            !LamaPon::HttpSend(redirectWithSecret).TransportSucceeded(),
            "A request with secrets could follow redirects.");

        LamaPon::HttpRequest redirectWithBody;
        redirectWithBody.url = L"https://example.com/start";
        redirectWithBody.method = L"POST";
        redirectWithBody.followRedirects = true;
        redirectWithBody.body = { 's', 'e', 'c', 'r', 'e', 't' };
        Require(
            !LamaPon::HttpSend(redirectWithBody).TransportSucceeded(),
            "A request body could follow a redirect.");

        LamaPon::HttpRequest userInfo;
        userInfo.url = L"https://user:password@example.com/path";
        Require(
            !LamaPon::HttpSend(userInfo).TransportSucceeded(),
            "A URL containing user information was accepted.");

        LamaPon::HttpRequest emptyUserInfo;
        emptyUserInfo.url = L"https://@example.com/path";
        Require(
            !LamaPon::HttpSend(emptyUserInfo).TransportSucceeded(),
            "An empty user-info marker was accepted.");

        LamaPon::HttpRequest injectedHeader;
        injectedHeader.url = L"https://example.com/path";
        injectedHeader.headers.emplace_back(
            L"X-Test",
            L"safe\r\nAuthorization: leaked");
        Require(
            !LamaPon::HttpSend(injectedHeader).TransportSucceeded(),
            "A request header containing CRLF was accepted.");

        std::deque<LamaPon::HttpResponse> responses;
        responses.push_back(JsonResponse(
            201,
            {
                { "transactionId", "attempt-1" },
                { "pollToken", "poll-secret" },
                {
                    "authorizationUrl",
                    "https://login.example.test/discord"
                },
                { "expiresIn", 300 },
                { "pollInterval", 2 }
            }));
        responses.push_back(JsonResponse(
            202,
            {
                { "status", "pending" },
                { "retryAfter", 3 }
            }));
        responses.push_back(JsonResponse(
            200,
            {
                { "status", "authorized" },
                { "accessToken", "game-access-token" },
                { "refreshToken", "game-refresh-token" },
                { "expiresIn", 900 },
                {
                    "player",
                    {
                        { "id", "player-42" },
                        { "displayName", "ラマポン" },
                        {
                            "avatarUrl",
                            "https://cdn.discordapp.com/avatar.png"
                        },
                        { "linkedProvider", "discord" }
                    }
                }
            }));
        LamaPon::HttpResponse malformedRefresh;
        malformedRefresh.statusCode = 200;
        const std::string malformedBody =
            R"({"accessToken":"must-not-appear","refreshToken":)";
        malformedRefresh.body.assign(
            malformedBody.begin(),
            malformedBody.end());
        responses.push_back(std::move(malformedRefresh));
        responses.push_back(JsonResponse(
            200,
            {
                { "accessToken", "rotated-access-token" },
                { "refreshToken", "rotated-refresh-token" },
                { "expiresIn", 1200 },
                {
                    "player",
                    {
                        { "id", "player-42" },
                        { "displayName", "ラマポン" },
                        { "avatarUrl", "" },
                        { "linkedProvider", "discord" }
                    }
                }
            }));
        LamaPon::HttpResponse logoutResponse;
        logoutResponse.statusCode = 204;
        responses.push_back(logoutResponse);

        std::vector<LamaPon::HttpRequest> requests;
        const auto sender =
            [&responses, &requests](const LamaPon::HttpRequest& request)
            {
                requests.push_back(request);
                if (responses.empty())
                {
                    LamaPon::HttpResponse missing;
                    missing.transportError = "Unexpected request.";
                    return missing;
                }
                auto response = std::move(responses.front());
                responses.pop_front();
                return response;
            };

        LamaPon::Detail::DiscordAuthClient client(
            "https://online.example.test/",
            false,
            sender);
        Require(
            client.ServiceBaseUrl() == "https://online.example.test",
            "Online service base URL was not normalized.");

        const auto started = client.BeginLogin();
        Require(
            started.Succeeded()
                && started.transaction.transactionId == "attempt-1"
                && started.transaction.pollIntervalSeconds == 2,
            "Discord login transaction could not be started.");
        Require(
            requests.front().method == L"POST"
                && !requests.front().followRedirects
                && requests.front().url
                    == L"https://online.example.test/v1/auth/login/start",
            "Discord login start request was not secure.");

        const auto pending = client.PollLogin(
            started.transaction.transactionId,
            started.transaction.pollToken);
        Require(
            pending.status
                    == LamaPon::Detail::DiscordLoginPollStatus::Pending
                && pending.retryAfterSeconds == 3,
            "Pending Discord login was not retained.");

        const auto authorized = client.PollLogin(
            started.transaction.transactionId,
            started.transaction.pollToken);
        Require(
            authorized.status
                    == LamaPon::Detail::DiscordLoginPollStatus::Authorized
                && authorized.session.player.playerId == "player-42"
                && authorized.session.player.displayName == "ラマポン"
                && authorized.session.accessToken == "game-access-token",
            "Authorized Discord login was not parsed.");

        const auto malformed = client.RefreshSession(
            authorized.session.refreshToken);
        Require(
            !malformed.Succeeded()
                && malformed.errorMessage.find("must-not-appear")
                    == std::string::npos,
            "A malformed response leaked token text into an error.");

        const auto refreshed = client.RefreshSession(
            authorized.session.refreshToken);
        Require(
            refreshed.Succeeded()
                && refreshed.session.accessToken
                    == "rotated-access-token",
            "Online session could not be refreshed.");
        Require(
            client.Logout(refreshed.session.accessToken),
            "Online session could not be logged out.");
        Require(
            HasHeader(
                requests.back(),
                L"Authorization",
                L"Bearer rotated-access-token")
                && !requests.back().followRedirects,
            "Bearer request could follow a redirect or omitted auth.");

        Require(
            responses.empty() && requests.size() == 6,
            "Unexpected online authentication request count.");

        std::cout << "Online authentication protocol tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
