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
        // 接続先の完全なURL
        std::wstring url;
        // HTTPメソッド
        std::wstring method{ L"GET" };
        // 送信するヘッダー名と値
        std::vector<std::pair<std::wstring, std::wstring>> headers;
        // 送信する本文のバイト列
        std::vector<std::uint8_t> body;
        // 応答本文の上限バイト数
        std::size_t maxResponseBytes{ 1024u * 1024u };
        // ヘッダーや本文を別originへ渡さないため、自動追従はヘッダー・本文なしのGET/HEADだけに制限します。
        // リダイレクトの自動追従
        bool followRedirects{};
        // HTTPは明示的に許可したローカル開発接続だけで使い、配布ゲームではfalseにします。
        // ローカルHTTP接続の許可
        bool allowInsecureLoopback{};
    };

    struct HttpResponse final
    {
        // HTTP応答のステータス
        std::uint32_t statusCode{};
        // 受信したヘッダー名と値
        std::vector<std::pair<std::wstring, std::wstring>> headers;
        // 受信した本文のバイト列
        std::vector<std::uint8_t> body;
        // 通信失敗の理由、成功時は空
        std::string transportError;

        // HTTPステータスによらず、通信エラーがないかを返します。
        [[nodiscard]] bool TransportSucceeded() const noexcept
        {
            return transportError.empty();
        }

        // HTTPステータスが2xxかを返します。
        [[nodiscard]] bool IsSuccessStatus() const noexcept
        {
            return statusCode >= 200 && statusCode < 300;
        }

        // 文字コードを変換せず本文のバイト列をstd::stringへ複製して返します。
        [[nodiscard]] std::string Text() const
        {
            return std::string(body.begin(), body.end());
        }

        // 大文字小文字を区別せず最初に一致するヘッダー値を返します(name: ヘッダー名)。
        [[nodiscard]] std::optional<std::wstring> Header(
            std::wstring_view name) const;
    };

    // 同期的に要求を送り、応答と通信エラーを返します(request: 送信設定と本文)。
    // 初回のHTTP URLは明示許可したループバックに限り、HTTPSからHTTPへの自動降格は拒否します。
    // タイムアウトは名前解決4秒・接続4秒・送信8秒・受信30秒です。
    // HTTPステータスが4xx/5xxでも通信自体が成功していれば本文を返します。
    [[nodiscard]] HttpResponse HttpSend(const HttpRequest& request);

    // HTTPS GETで本文を文字列として取得します(host: 接続先ホスト, path: 要求パス, maxBytes: 本文の上限バイト数, extraHeaders: 追加ヘッダー)。
    // 通信失敗やステータスが200以外なら空を返し、追加ヘッダーがない場合だけリダイレクトへ追従します。
    [[nodiscard]] std::string HttpGetText(
        const std::wstring& host,
        const std::wstring& path,
        std::size_t maxBytes = 1024u * 1024u,
        const std::wstring& extraHeaders = {});

    // HTTPS GETで本文をバイト列として取得します(host: 接続先ホスト, path: 要求パス, maxBytes: 本文の上限バイト数)。
    // リダイレクトへ追従し、通信失敗やステータスが200以外なら空を返します。
    [[nodiscard]] std::vector<std::uint8_t> HttpGetBytes(
        const std::wstring& host,
        const std::wstring& path,
        std::size_t maxBytes = 256u * 1024u * 1024u);
}
