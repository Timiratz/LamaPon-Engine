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
        HINTERNET handle{};

        ~InternetHandle()
        {
            if (handle != nullptr)
            {
                WinHttpCloseHandle(handle);
            }
        }
    };

    std::string WindowsError(const char* operation)
    {
        const auto error = GetLastError();
        return std::string(operation)
            + " failed with Windows error "
            + std::to_string(error);
    }

    bool HasHeaderBreak(const std::wstring_view value)
    {
        return value.find(L'\r') != std::wstring_view::npos
            || value.find(L'\n') != std::wstring_view::npos;
    }

    bool HeaderNameEquals(
        const std::wstring_view left,
        const std::wstring_view right)
    {
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

    bool ReadResponseHeaders(
        const HINTERNET request,
        LamaPon::HttpResponse& response)
    {
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

        std::size_t lineStart{};
        bool firstLine = true;
        while (lineStart < raw.size())
        {
            const auto lineEnd = raw.find(L"\r\n", lineStart);
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
            const auto colon = line.find(L':');
            if (colon == std::wstring::npos || colon == 0)
            {
                continue;
            }
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

    bool IsLoopbackHost(const std::wstring_view host)
    {
        std::wstring lower(host);
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
        std::wstring host;
        std::wstring path;
        INTERNET_PORT port{};
        bool secure{};
    };

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

    LamaPon::HttpResponse SendRequest(
        const LamaPon::HttpRequest& request)
    {
        LamaPon::HttpResponse response;
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

        for (const auto& [name, value] : request.headers)
        {
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

        auto* body = request.body.empty()
            ? WINHTTP_NO_REQUEST_DATA
            : const_cast<std::uint8_t*>(request.body.data());
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

        DWORD statusCode{};
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
            const auto offset = response.body.size();
            response.body.resize(offset + available);
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

    // 共通のGET本体。成功時のみtrueを返し、outへ本文を書き込みます。
    bool HttpGetCore(
        const std::wstring& host,
        const std::wstring& path,
        const std::size_t maxBytes,
        const std::wstring& extraHeaders,
        std::vector<std::uint8_t>& out)
    {
        out.clear();

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

        const wchar_t* headers = extraHeaders.empty()
            ? WINHTTP_NO_ADDITIONAL_HEADERS
            : extraHeaders.c_str();
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

        DWORD statusCode = 0;
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
            const std::size_t offset = out.size();
            out.resize(offset + available);
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
        HttpRequest request;
        request.url = L"https://" + host + path;
        request.maxResponseBytes = maxBytes;
        request.followRedirects = true;
        if (!extraHeaders.empty())
        {
            // 互換APIは従来どおり完成済みのヘッダー文字列を受けます。
            // 改行を含む複数ヘッダーは汎用APIへ渡さず、旧実装を使います。
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
        HttpRequest request;
        request.url = L"https://" + host + path;
        request.maxResponseBytes = maxBytes;
        request.followRedirects = true;
        const auto response = HttpSend(request);
        if (!response.TransportSucceeded()
            || response.statusCode != 200)
        {
            return {};
        }
        return response.body;
    }
}
