#include "LamaPon/Core/HttpClient.h"

#include <Windows.h>

#include <winhttp.h>

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
    std::string HttpGetText(
        const std::wstring& host,
        const std::wstring& path,
        const std::size_t maxBytes,
        const std::wstring& extraHeaders)
    {
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
        return std::string(
            bytes.begin(),
            bytes.end());
    }

    std::vector<std::uint8_t> HttpGetBytes(
        const std::wstring& host,
        const std::wstring& path,
        const std::size_t maxBytes)
    {
        std::vector<std::uint8_t> bytes;
        if (!HttpGetCore(host, path, maxBytes, {}, bytes))
        {
            return {};
        }
        return bytes;
    }
}
