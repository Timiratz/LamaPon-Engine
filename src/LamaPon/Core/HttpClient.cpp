#include "LamaPon/Core/HttpClient.h"

#include <Windows.h>

#include <winhttp.h>

#include <algorithm>
#include <cwctype>
#include <limits>
#include <string_view>

namespace
{
    // WinHTTPハンドルのRAII。
    struct InternetHandle final
    {
        // 所有するWinHTTPハンドル
        HINTERNET handle{};

        // 所有するWinHTTPハンドルを閉じます。
        ~InternetHandle()
        {
            if (handle != nullptr)
            {
                WinHttpCloseHandle(handle);
            }
        }
    };

    // 直前のWin32エラーを診断文字列へ変換します(operation: 失敗した操作名)。
    std::string WindowsError(const char* operation)
    {
        // 操作直後のWin32エラー番号
        const auto error = GetLastError();
        return std::string(operation)
            + " failed with Windows error "
            + std::to_string(error);
    }

    // ヘッダーに禁止する改行があるかを返します(value: 検証する文字列)。
    bool HasHeaderBreak(const std::wstring_view value)
    {
        return value.find(L'\r') != std::wstring_view::npos
            || value.find(L'\n') != std::wstring_view::npos;
    }

    // 大文字小文字を区別せずヘッダー名を照合します(left: 比較元の名前, right: 比較先の名前)。
    bool HeaderNameEquals(
        const std::wstring_view left,
        const std::wstring_view right)
    {
        // 大文字小文字を区別せず文字を照合します(a: 比較元の文字, b: 比較先の文字)。
        return left.size() == right.size()
            && std::equal(
                left.begin(),
                left.end(),
                right.begin(),
                [](const wchar_t a, const wchar_t b)
                {
                    return towlower(a) == towlower(b);
                });
    }

    // 応答ヘッダーを名前と値へ分解します(request: 応答を受信したハンドル, response: ヘッダーの追加先)。
    bool ReadResponseHeaders(
        const HINTERNET request,
        LamaPon::HttpResponse& response)
    {
        // ヘッダー取得に必要なバイト数
        DWORD size{};
        WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_RAW_HEADERS_CRLF,
            WINHTTP_HEADER_NAME_BY_INDEX,
            WINHTTP_NO_OUTPUT_BUFFER,
            &size,
            WINHTTP_NO_HEADER_INDEX);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER
            || size < sizeof(wchar_t))
        {
            return false;
        }

        // CRLF区切りの応答ヘッダー全文
        std::wstring raw(size / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(
                request,
                WINHTTP_QUERY_RAW_HEADERS_CRLF,
                WINHTTP_HEADER_NAME_BY_INDEX,
                raw.data(),
                &size,
                WINHTTP_NO_HEADER_INDEX) == FALSE)
        {
            return false;
        }
        raw.resize(size / sizeof(wchar_t));
        while (!raw.empty() && raw.back() == L'\0')
        {
            raw.pop_back();
        }

        // 読み取るヘッダー行の先頭位置
        std::size_t lineStart{};
        // 状態行を読み飛ばす最初の反復か
        bool firstLine = true;
        while (lineStart < raw.size())
        {
            // 現在のヘッダー行の末尾位置
            const auto lineEnd = raw.find(L"\r\n", lineStart);
            // 名前と値へ分解するヘッダー行
            const auto line = raw.substr(
                lineStart,
                lineEnd == std::wstring::npos
                    ? std::wstring::npos
                    : lineEnd - lineStart);
            lineStart = lineEnd == std::wstring::npos
                ? raw.size()
                : lineEnd + 2;
            if (firstLine)
            {
                firstLine = false;
                continue;
            }
            // ヘッダー名と値を分ける位置
            const auto colon = line.find(L':');
            if (colon == std::wstring::npos || colon == 0)
            {
                continue;
            }
            // 先頭の空白を除いた値の開始位置
            auto valueStart = colon + 1;
            while (valueStart < line.size()
                && (line[valueStart] == L' '
                    || line[valueStart] == L'\t'))
            {
                ++valueStart;
            }
            response.headers.emplace_back(
                line.substr(0, colon),
                line.substr(valueStart));
        }
        return true;
    }

    // 許可するループバック表記かを返します(host: URL内のホスト名)。
    bool IsLoopbackHost(const std::wstring_view host)
    {
        // 小文字へ統一するホスト名
        std::wstring lower(host);
        // ホスト名の文字を小文字へ変換します(value: ホスト名の各文字)。
        std::transform(
            lower.begin(),
            lower.end(),
            lower.begin(),
            [](const wchar_t value)
            {
                return static_cast<wchar_t>(towlower(value));
            });
        return lower == L"127.0.0.1"
            || lower == L"localhost"
            || lower == L"[::1]"
            || lower == L"::1";
    }

    struct CrackedUrl final
    {
        // URLから抽出したホスト名
        std::wstring host;
        // クエリーを含む要求パス
        std::wstring path;
        // 接続先のポート番号
        INTERNET_PORT port{};
        // HTTPSで接続するか
        bool secure{};
    };

    // URLを分解し初回接続先を検証します(request: 接続要求, cracked: 分解結果の出力先, error: 失敗理由の出力先)。
    bool CrackRequestUrl(
        const LamaPon::HttpRequest& request,
        CrackedUrl& cracked,
        std::string& error)
    {
        if (request.url.empty())
        {
            error = "HTTP request URL is empty.";
            return false;
        }

        // WinHTTPが分解するURL成分
        URL_COMPONENTS components{};
        components.dwStructSize = sizeof(components);
        components.dwSchemeLength = static_cast<DWORD>(-1);
        components.dwHostNameLength = static_cast<DWORD>(-1);
        components.dwUserNameLength = static_cast<DWORD>(-1);
        components.dwPasswordLength = static_cast<DWORD>(-1);
        components.dwUrlPathLength = static_cast<DWORD>(-1);
        components.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (WinHttpCrackUrl(
                request.url.c_str(),
                0,
                ICU_REJECT_USERPWD,
                &components) == FALSE)
        {
            error = WindowsError("WinHttpCrackUrl");
            return false;
        }

        cracked.host.assign(
            components.lpszHostName,
            components.dwHostNameLength);
        cracked.path.assign(
            components.lpszUrlPath,
            components.dwUrlPathLength);
        if (cracked.path.empty())
        {
            cracked.path = L"/";
        }
        if (components.dwExtraInfoLength > 0)
        {
            cracked.path.append(
                components.lpszExtraInfo,
                components.dwExtraInfoLength);
        }
        cracked.port = components.nPort;
        cracked.secure =
            components.nScheme == INTERNET_SCHEME_HTTPS;

        if (cracked.host.empty()
            || components.dwUserNameLength > 0
            || components.dwPasswordLength > 0
            || request.url.find(L'#') != std::wstring::npos)
        {
            error = "HTTP request URL must not contain user information or a fragment.";
            return false;
        }
        if (!cracked.secure
            && !(components.nScheme == INTERNET_SCHEME_HTTP
                && request.allowInsecureLoopback
                && IsLoopbackHost(cracked.host)))
        {
            error = "HTTP request URL must use HTTPS; only an explicitly enabled loopback URL may use HTTP.";
            return false;
        }
        return true;
    }

    // 要求を同期送信して応答を読み切ります(request: 送信設定と本文)。
    LamaPon::HttpResponse SendRequest(
        const LamaPon::HttpRequest& request)
    {
        // 本文とヘッダーと通信結果
        LamaPon::HttpResponse response;
        // 検証して分解した接続先URL
        CrackedUrl url;
        if (!CrackRequestUrl(
                request,
                url,
                response.transportError))
        {
            return response;
        }
        if (request.method.empty()
            || HasHeaderBreak(request.method))
        {
            response.transportError = "HTTP request method is invalid.";
            return response;
        }
        if (request.body.size()
            > static_cast<std::size_t>(
                std::numeric_limits<DWORD>::max()))
        {
            response.transportError = "HTTP request body is too large.";
            return response;
        }
        // name: 送信ヘッダー名
        // value: 送信ヘッダー値
        for (const auto& [name, value] : request.headers)
        {
            if (name.empty()
                || HasHeaderBreak(name)
                || HasHeaderBreak(value)
                || name.find(L':') != std::wstring::npos)
            {
                response.transportError = "HTTP request contains an invalid header.";
                return response;
            }
        }
        // 自動追従を許可するメソッドか
        const bool redirectSafeMethod =
            request.method == L"GET" || request.method == L"HEAD";
        if (request.followRedirects
            && (!redirectSafeMethod
                || !request.headers.empty()
                || !request.body.empty()))
        {
            response.transportError =
                "Only header-free GET or HEAD requests without a body may follow redirects automatically.";
            return response;
        }

        // WinHTTPセッションの所有者
        InternetHandle session;
        session.handle = WinHttpOpen(
            L"LamaPon",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
        if (session.handle == nullptr)
        {
            response.transportError = WindowsError("WinHttpOpen");
            return response;
        }
        WinHttpSetTimeouts(session.handle, 4000, 4000, 8000, 30000);

        // 接続先ホストのハンドル所有者
        InternetHandle connection;
        connection.handle = WinHttpConnect(
            session.handle,
            url.host.c_str(),
            url.port,
            0);
        if (connection.handle == nullptr)
        {
            response.transportError = WindowsError("WinHttpConnect");
            return response;
        }

        // 送信要求のハンドル所有者
        InternetHandle handle;
        handle.handle = WinHttpOpenRequest(
            connection.handle,
            request.method.c_str(),
            url.path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            url.secure ? WINHTTP_FLAG_SECURE : 0);
        if (handle.handle == nullptr)
        {
            response.transportError = WindowsError("WinHttpOpenRequest");
            return response;
        }
        // リダイレクトの自動追従方針
        DWORD redirectPolicy = request.followRedirects
            ? WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP
            : WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (WinHttpSetOption(
                handle.handle,
                WINHTTP_OPTION_REDIRECT_POLICY,
                &redirectPolicy,
                sizeof(redirectPolicy)) == FALSE)
        {
            response.transportError =
                WindowsError("WinHttpSetOption(redirect policy)");
            return response;
        }

        // name: 送信ヘッダー名
        // value: 送信ヘッダー値
        for (const auto& [name, value] : request.headers)
        {
            // WinHTTPへ渡す名前と値の1行
            const std::wstring header = name + L": " + value;
            if (WinHttpAddRequestHeaders(
                    handle.handle,
                    header.c_str(),
                    static_cast<DWORD>(-1),
                    WINHTTP_ADDREQ_FLAG_ADD
                        | WINHTTP_ADDREQ_FLAG_REPLACE) == FALSE)
            {
                response.transportError =
                    WindowsError("WinHttpAddRequestHeaders");
                return response;
            }
        }

        // WinHTTPへ渡す送信本文の先頭
        auto* body = request.body.empty()
            ? WINHTTP_NO_REQUEST_DATA
            : const_cast<std::uint8_t*>(request.body.data());
        // 送信本文のバイト数
        const auto bodySize =
            static_cast<DWORD>(request.body.size());
        if (WinHttpSendRequest(
                handle.handle,
                WINHTTP_NO_ADDITIONAL_HEADERS,
                0,
                body,
                bodySize,
                bodySize,
                0) == FALSE
            || WinHttpReceiveResponse(
                handle.handle,
                nullptr) == FALSE)
        {
            response.transportError = WindowsError(
                "WinHttpSendRequest/WinHttpReceiveResponse");
            return response;
        }

        // 応答のHTTPステータス番号
        DWORD statusCode{};
        // ステータス出力の容量バイト数
        DWORD statusSize = sizeof(statusCode);
        if (WinHttpQueryHeaders(
                handle.handle,
                WINHTTP_QUERY_STATUS_CODE
                    | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &statusCode,
                &statusSize,
                WINHTTP_NO_HEADER_INDEX) == FALSE)
        {
            response.transportError =
                WindowsError("WinHttpQueryHeaders");
            return response;
        }
        response.statusCode = statusCode;
        if (!ReadResponseHeaders(handle.handle, response))
        {
            response.transportError =
                WindowsError("WinHttpQueryHeaders(raw headers)");
            return response;
        }

        for (;;)
        {
            // 次に受信できる本文のバイト数
            DWORD available{};
            if (WinHttpQueryDataAvailable(
                    handle.handle,
                    &available) == FALSE)
            {
                response.transportError =
                    WindowsError("WinHttpQueryDataAvailable");
                response.body.clear();
                return response;
            }
            if (available == 0)
            {
                break;
            }
            if (response.body.size() > request.maxResponseBytes
                || available > request.maxResponseBytes
                    - response.body.size())
            {
                response.transportError =
                    "HTTP response exceeded its configured size limit.";
                response.body.clear();
                return response;
            }
            // 応答本文へ追加する先頭位置
            const auto offset = response.body.size();
            response.body.resize(offset + available);
            // 実際に受信した本文のバイト数
            DWORD read{};
            if (WinHttpReadData(
                    handle.handle,
                    response.body.data() + offset,
                    available,
                    &read) == FALSE)
            {
                response.transportError =
                    WindowsError("WinHttpReadData");
                response.body.clear();
                return response;
            }
            response.body.resize(offset + read);
            if (read == 0)
            {
                break;
            }
        }
        return response;
    }

    // HTTPS GETを同期実行します(host: 接続先, path: 要求パス, maxBytes: 本文上限バイト数, extraHeaders: 完成済みの追加ヘッダー, out: 受信本文の出力先)。
    // falseの場合は部分データを含むoutを使わず、追加ヘッダーがあれば自動追従を禁止します。
    bool HttpGetCore(
        const std::wstring& host,
        const std::wstring& path,
        const std::size_t maxBytes,
        const std::wstring& extraHeaders,
        std::vector<std::uint8_t>& out)
    {
        out.clear();

        // WinHTTPセッションの所有者
        InternetHandle session;
        session.handle = WinHttpOpen(
            L"LamaPon",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
        if (session.handle == nullptr)
        {
            return false;
        }
        // 接続系は短く、受信はダウンロード用に長めに取ります。
        WinHttpSetTimeouts(session.handle, 4000, 4000, 8000, 30000);

        // 接続先ホストのハンドル所有者
        InternetHandle connection;
        connection.handle = WinHttpConnect(
            session.handle,
            host.c_str(),
            INTERNET_DEFAULT_HTTPS_PORT,
            0);
        if (connection.handle == nullptr)
        {
            return false;
        }

        // 互換GET要求のハンドル所有者
        InternetHandle request;
        request.handle = WinHttpOpenRequest(
            connection.handle,
            L"GET",
            path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE);
        if (request.handle == nullptr)
        {
            return false;
        }
        if (!extraHeaders.empty())
        {
            // 追加ヘッダー転送を防ぐ追従禁止
            DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
            if (WinHttpSetOption(
                    request.handle,
                    WINHTTP_OPTION_REDIRECT_POLICY,
                    &redirectPolicy,
                    sizeof(redirectPolicy)) == FALSE)
            {
                return false;
            }
        }

        // WinHTTPへ渡す追加ヘッダー先頭
        const wchar_t* headers = extraHeaders.empty()
            ? WINHTTP_NO_ADDITIONAL_HEADERS
            : extraHeaders.c_str();
        // 追加ヘッダー長、終端読取は-1
        const DWORD headersLength = extraHeaders.empty()
            ? 0
            : static_cast<DWORD>(-1);
        if (WinHttpSendRequest(
                request.handle,
                headers,
                headersLength,
                WINHTTP_NO_REQUEST_DATA,
                0,
                0,
                0) == FALSE
            || WinHttpReceiveResponse(
                request.handle,
                nullptr) == FALSE)
        {
            return false;
        }

        // 互換GET応答のHTTPステータス
        DWORD statusCode = 0;
        // ステータス出力の容量バイト数
        DWORD statusSize = sizeof(statusCode);
        if (WinHttpQueryHeaders(
                request.handle,
                WINHTTP_QUERY_STATUS_CODE
                    | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &statusCode,
                &statusSize,
                WINHTTP_NO_HEADER_INDEX) == FALSE
            || statusCode != 200)
        {
            return false;
        }

        for (;;)
        {
            // 次に受信できる本文のバイト数
            DWORD available = 0;
            if (WinHttpQueryDataAvailable(
                    request.handle,
                    &available) == FALSE)
            {
                return false;
            }
            if (available == 0)
            {
                break;
            }
            if (out.size() + available > maxBytes)
            {
                // 想定外に大きい応答は安全側で失敗にします。
                return false;
            }
            // 本文の出力先へ追加する位置
            const std::size_t offset = out.size();
            out.resize(offset + available);
            // 実際に受信した本文のバイト数
            DWORD read = 0;
            if (WinHttpReadData(
                    request.handle,
                    out.data() + offset,
                    available,
                    &read) == FALSE)
            {
                return false;
            }
            out.resize(offset + read);
            if (read == 0)
            {
                break;
            }
        }
        return true;
    }
}

namespace LamaPon
{
    std::optional<std::wstring> HttpResponse::Header(
        const std::wstring_view name) const
    {
        // headerName: 受信ヘッダー名
        // value: 受信ヘッダー値
        for (const auto& [headerName, value] : headers)
        {
            if (HeaderNameEquals(headerName, name))
            {
                return value;
            }
        }
        return std::nullopt;
    }

    HttpResponse HttpSend(const HttpRequest& request)
    {
        return SendRequest(request);
    }

    std::string HttpGetText(
        const std::wstring& host,
        const std::wstring& path,
        const std::size_t maxBytes,
        const std::wstring& extraHeaders)
    {
        // 本文取得のためのGET要求設定
        HttpRequest request;
        request.url = L"https://" + host + path;
        request.maxResponseBytes = maxBytes;
        request.followRedirects = true;
        if (!extraHeaders.empty())
        {
            // 追加ヘッダー付きの完成済み文字列は互換GET経路へ渡します。
            // 互換経路で受信する本文
            std::vector<std::uint8_t> bytes;
            if (!HttpGetCore(
                    host,
                    path,
                    maxBytes,
                    extraHeaders,
                    bytes))
            {
                return {};
            }
            return std::string(bytes.begin(), bytes.end());
        }
        // GETで受信した応答と通信結果
        const auto response = HttpSend(request);
        if (!response.TransportSucceeded()
            || response.statusCode != 200)
        {
            return {};
        }
        return response.Text();
    }

    std::vector<std::uint8_t> HttpGetBytes(
        const std::wstring& host,
        const std::wstring& path,
        const std::size_t maxBytes)
    {
        // 本文取得のためのGET要求設定
        HttpRequest request;
        request.url = L"https://" + host + path;
        request.maxResponseBytes = maxBytes;
        request.followRedirects = true;
        // GETで受信した応答と通信結果
        const auto response = HttpSend(request);
        if (!response.TransportSucceeded()
            || response.statusCode != 200)
        {
            return {};
        }
        return response.body;
    }
}
