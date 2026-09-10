#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace LamaPon
{
    struct HttpRequest final
    {
        std::wstring url;
        std::wstring method{ L"GET" };
        std::vector<std::pair<std::wstring, std::wstring>> headers;
        std::vector<std::uint8_t> body;
        std::size_t maxResponseBytes{ 1024u * 1024u };
        // ヘッダーや本文を別originへ渡さないため、自動追従は
        // ヘッダー・本文なしのGET/HEADだけに制限します。公開GETだけを
        // 行う既存ヘルパーは明示的にtrueへ設定します。
        bool followRedirects{};
        // 配布ゲームではfalseのまま使います。ローカルの参照サーバーを
        // 開発中に試す場合だけ、http://127.0.0.1 を許可します。
        bool allowInsecureLoopback{};
    };

    struct HttpResponse final
    {
        std::uint32_t statusCode{};
        std::vector<std::pair<std::wstring, std::wstring>> headers;
        std::vector<std::uint8_t> body;
        std::string transportError;

        [[nodiscard]] bool TransportSucceeded() const noexcept
        {
            return transportError.empty();
        }

        [[nodiscard]] bool IsSuccessStatus() const noexcept
        {
            return statusCode >= 200 && statusCode < 300;
        }

        [[nodiscard]] std::string Text() const
        {
            return std::string(body.begin(), body.end());
        }

        [[nodiscard]] std::optional<std::wstring> Header(
            std::wstring_view name) const;
    };

    // 任意のHTTPSエンドポイントへ要求を送ります。HTTPは、明示的に
    // 許可した127.0.0.1/localhostへの開発接続だけに制限します。
    // HTTPステータスが4xx/5xxでも通信自体が成功していれば本文を返します。
    [[nodiscard]] HttpResponse HttpSend(const HttpRequest& request);

    // HTTPS GETで応答本文を取得する軽量ヘルパー（WinHTTP使用）。
    // Hubのアップデート確認やエディターのパッケージ取得に使用します。
    // 失敗時は空を返し、例外を投げません。追加ヘッダーが無いGETは
    // リダイレクトへ追従し、追加ヘッダー付きGETは漏えい防止のため
    // 追従しません。

    // テキスト応答（JSON等）を取得します。
    [[nodiscard]] std::string HttpGetText(
        const std::wstring& host,
        const std::wstring& path,
        std::size_t maxBytes = 1024u * 1024u,
        const std::wstring& extraHeaders = {});

    // バイナリ応答（Zip等のダウンロード）を取得します。
    [[nodiscard]] std::vector<std::uint8_t> HttpGetBytes(
        const std::wstring& host,
        const std::wstring& path,
        std::size_t maxBytes = 256u * 1024u * 1024u);
}
