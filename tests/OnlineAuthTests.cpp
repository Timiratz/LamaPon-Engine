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
        // 受信したHTTP応答
        LamaPon::HttpResponse response;
        // ループバック要求の生データ
        std::string requestText;
    };

    // Require(condition: 成立条件, message: 失敗理由): 条件不成立を検査失敗にする。
    void Require(const bool condition, const char* message)
    {
        // 検査条件の不成立を検出する。
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // JsonResponse(status: HTTP状態, body: JSON本文): 指定内容のHTTP応答を作る。
    LamaPon::HttpResponse JsonResponse(
        const std::uint32_t status,
        const nlohmann::json& body)
    {
        // 応答の組み立て先
        LamaPon::HttpResponse response;
        response.statusCode = status;
        // JSONのシリアライズ結果
        const auto text = body.dump();
        response.body.assign(text.begin(), text.end());
        return response;
    }

    // HasHeader(request: 検査対象, name: ヘッダー名, value: 期待値): 要求に指定ヘッダーがあるか調べる。
    bool HasHeader(
        const LamaPon::HttpRequest& request,
        const std::wstring& name,
        const std::wstring& value)
    {
        // 要求の全ヘッダーを調べる。
        for (const auto& header : request.headers)
        {
            // 名前と値の一致を確認する。
            if (header.first == name && header.second == value)
            {
                return true;
            }
        }
        return false;
    }

    // RequestLoopbackHeaders(): ローカルHTTP応答のヘッダーを取得する。
    LoopbackResult RequestLoopbackHeaders()
    {
        // Winsock初期化状態
        WSADATA sockets{};
        // Winsock初期化に失敗した場合を検出する。
        if (WSAStartup(MAKEWORD(2, 2), &sockets) != 0)
        {
            throw std::runtime_error("WSAStartup failed.");
        }

        // ローカル接続を受け付けるソケット
        const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        // ソケット作成に失敗した場合を検出する。
        if (listener == INVALID_SOCKET)
        {
            WSACleanup();
            throw std::runtime_error("Could not create loopback socket.");
        }
        // ローカル接続先アドレス
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        // bindまたはlistenの失敗を検出する。
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
        // getsocknameへ渡すアドレス長
        int addressSize = sizeof(address);
        // 割り当てられたポートの取得失敗を検出する。
        if (getsockname(
                listener,
                reinterpret_cast<sockaddr*>(&address),
                &addressSize) == SOCKET_ERROR)
        {
            closesocket(listener);
            WSACleanup();
            throw std::runtime_error("Could not read loopback port.");
        }

        // 送信した要求の生データ
        std::string requestText;
        // 接続を処理するローカルサーバー
        std::jthread server(
            [listener, &requestText]
            {
                // 接続したクライアントソケット
                const SOCKET client = accept(listener, nullptr, nullptr);
                // 接続の受付失敗を処理する。
                if (client == INVALID_SOCKET)
                {
                    return;
                }
                // 受信要求の一時バッファー
                char requestBytes[2048]{};
                // 受信したバイト数
                const auto received = recv(
                    client,
                    requestBytes,
                    sizeof(requestBytes),
                    0);
                // 要求データが届いた場合に保存する。
                if (received > 0)
                {
                    requestText.assign(
                        requestBytes,
                        static_cast<std::size_t>(received));
                }
                // テスト用HTTP応答
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

        // ローカルサーバーへ送る要求
        LamaPon::HttpRequest request;
        request.url = L"http://127.0.0.1:"
            + std::to_wstring(ntohs(address.sin_port))
            + L"?check=headers";
        request.allowInsecureLoopback = true;
        // ループバック応答
        const auto response = LamaPon::HttpSend(request);
        closesocket(listener);
        server.join();
        WSACleanup();
        return { response, std::move(requestText) };
    }
}

// main(): HTTP安全性とオンライン認証プロトコルを検証する。
int main()
{
    // 検査失敗を終了コードへ変換する。
    try
    {
        // 平文リモートURLの拒否結果
        bool insecureRejected{};
        // HTTPリモートURLの拒否を検査する。
        try
        {
            // 拒否対象の認証クライアント
            LamaPon::Detail::DiscordAuthClient invalid(
                "http://example.com");
        }
        // 期待したURL拒否を記録する。
        catch (const std::invalid_argument&)
        {
            insecureRejected = true;
        }
        Require(
            insecureRejected,
            "An insecure remote service URL was accepted.");
        // 偽装ループバックURLの拒否結果
        bool disguisedLoopbackRejected{};
        // localhost風のリモートURL拒否を検査する。
        try
        {
            // 拒否対象の認証クライアント
            LamaPon::Detail::DiscordAuthClient invalid(
                "http://localhost.example.com",
                true);
        }
        // 期待したURL拒否を記録する。
        catch (const std::invalid_argument&)
        {
            disguisedLoopbackRejected = true;
        }
        Require(
            disguisedLoopbackRejected,
            "A remote host disguised as loopback was accepted.");

        // 平文リモートURLを持つ要求
        LamaPon::HttpRequest unsafeRequest;
        unsafeRequest.url = L"http://example.com/api";
        // 安全でない要求の拒否応答
        const auto unsafeResponse = LamaPon::HttpSend(unsafeRequest);
        Require(
            !unsafeResponse.TransportSucceeded()
                && unsafeResponse.statusCode == 0,
            "HttpSend attempted an insecure remote request.");

        // 大文字小文字を無視する検索対象応答
        LamaPon::HttpResponse headerResponse;
        headerResponse.headers.emplace_back(L"ETag", L"revision-7");
        Require(
            headerResponse.Header(L"etag") == L"revision-7",
            "HTTP response header lookup was case-sensitive.");

        // 実際のループバック応答と要求
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

        // 認証情報を含むリダイレクト要求
        LamaPon::HttpRequest redirectWithSecret;
        redirectWithSecret.url = L"https://example.com/start";
        redirectWithSecret.followRedirects = true;
        redirectWithSecret.headers.emplace_back(
            L"Authorization",
            L"Bearer must-not-leak");
        Require(
            !LamaPon::HttpSend(redirectWithSecret).TransportSucceeded(),
            "A request with secrets could follow redirects.");

        // 本文を含むリダイレクト要求
        LamaPon::HttpRequest redirectWithBody;
        redirectWithBody.url = L"https://example.com/start";
        redirectWithBody.method = L"POST";
        redirectWithBody.followRedirects = true;
        redirectWithBody.body = { 's', 'e', 'c', 'r', 'e', 't' };
        Require(
            !LamaPon::HttpSend(redirectWithBody).TransportSucceeded(),
            "A request body could follow a redirect.");

        // 認証情報を埋め込んだURL
        LamaPon::HttpRequest userInfo;
        userInfo.url = L"https://user:password@example.com/path";
        Require(
            !LamaPon::HttpSend(userInfo).TransportSucceeded(),
            "A URL containing user information was accepted.");

        // 空のユーザー情報を含むURL
        LamaPon::HttpRequest emptyUserInfo;
        emptyUserInfo.url = L"https://@example.com/path";
        Require(
            !LamaPon::HttpSend(emptyUserInfo).TransportSucceeded(),
            "An empty user-info marker was accepted.");

        // CRLFを含むヘッダー値
        LamaPon::HttpRequest injectedHeader;
        injectedHeader.url = L"https://example.com/path";
        injectedHeader.headers.emplace_back(
            L"X-Test",
            L"safe\r\nAuthorization: leaked");
        Require(
            !LamaPon::HttpSend(injectedHeader).TransportSucceeded(),
            "A request header containing CRLF was accepted.");

        // 認証処理へ順に返す応答列
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
        // 不正JSONを返す更新応答
        LamaPon::HttpResponse malformedRefresh;
        malformedRefresh.statusCode = 200;
        // トークン文字列を含む不完全JSON
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
        // ログアウト成功応答
        LamaPon::HttpResponse logoutResponse;
        logoutResponse.statusCode = 204;
        responses.push_back(logoutResponse);

        // 認証処理が送った要求履歴
        std::vector<LamaPon::HttpRequest> requests;
        // sender(request: 送信要求): 要求を記録して応答列から返す。
        const auto sender =
            [&responses, &requests](const LamaPon::HttpRequest& request)
            {
                requests.push_back(request);
                // 応答列が尽きた予期しない要求を検出する。
                if (responses.empty())
                {
                    // 想定外要求を示す失敗応答
                    LamaPon::HttpResponse missing;
                    missing.transportError = "Unexpected request.";
                    return missing;
                }
                // 次に返す応答
                auto response = std::move(responses.front());
                responses.pop_front();
                return response;
            };

        // テスト用オンライン認証クライアント
        LamaPon::Detail::DiscordAuthClient client(
            "https://online.example.test/",
            false,
            sender,
            "auth-tests",
            "staging");
        Require(
            client.ServiceBaseUrl() == "https://online.example.test",
            "Online service base URL was not normalized.");

        // 開始応答とログイン取引
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

        // 保留中のログイン状態
        const auto pending = client.PollLogin(
            started.transaction.transactionId,
            started.transaction.pollToken);
        Require(
            pending.status
                    == LamaPon::Detail::DiscordLoginPollStatus::Pending
                && pending.retryAfterSeconds == 3,
            "Pending Discord login was not retained.");

        // 認証済みのログイン状態
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

        // 不正応答による更新失敗
        const auto malformed = client.RefreshSession(
            authorized.session.refreshToken);
        Require(
            !malformed.Succeeded()
                && malformed.errorMessage.find("must-not-appear")
                    == std::string::npos,
            "A malformed response leaked token text into an error.");

        // 更新後の認証セッション
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
        // 認証要求の共通ヘッダーを確認する。
        for (const auto& request : requests)
        {
            Require(
                HasHeader(
                    request,
                    L"X-LamaPon-Game-Id",
                    L"auth-tests")
                    && HasHeader(
                        request,
                        L"X-LamaPon-Environment-Id",
                        L"staging"),
                "An authentication request omitted its backend namespace.");
        }

        std::cout << "Online authentication protocol tests passed.\n";
        return 0;
    }
    // 例外(exception: 検査失敗情報)を標準エラーへ出力する。
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
