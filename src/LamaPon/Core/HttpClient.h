#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace LamaPon
{
    // HTTPS GETで応答本文を取得する軽量ヘルパー（WinHTTP使用）。
    // Hubのアップデート確認やエディターのパッケージ取得に使用します。
    // 失敗時は空を返し、例外を投げません。リダイレクトには自動で
    // 追従します。

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
