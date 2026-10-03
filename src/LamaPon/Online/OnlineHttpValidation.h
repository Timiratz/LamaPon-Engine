#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace LamaPon::Detail
{
    // 末尾のスラッシュを除き、不正なURLは例外にする(value: サービスURL, allowInsecureLoopback: ローカルHTTPを許可するか)。
    // HTTPSまたは許可したローカルHTTPを受け付け、ユーザー情報・クエリー・フラグメントを拒否します。
    [[nodiscard]] std::string NormalizeOnlineServiceBaseUrl(
        std::string value,
        bool allowInsecureLoopback);

    // 空白や制御文字のない値かを判定する(value: 検査する値, maxBytes: 上限バイト数)。
    [[nodiscard]] bool IsSafeOnlineOpaqueValue(
        std::string_view value,
        std::size_t maxBytes);

    // 空でないvisible ASCIIのトークンかを判定する(value: 認証トークン, maxBytes: 上限バイト数)。
    [[nodiscard]] bool IsSafeOnlineBearerToken(
        std::string_view value,
        std::size_t maxBytes = 8192);

    // 英数字とピリオド・下線・ハイフンの識別子かを判定する(value: 名前空間ID, maxBytes: 上限バイト数)。
    [[nodiscard]] bool IsSafeOnlineNamespaceId(
        std::string_view value,
        std::size_t maxBytes);

    // ブラウザー用URLを判定する(value: 開くURL, allowInsecureLoopback: ローカルHTTPを許可するか)。
    // HTTPSまたは許可したローカルHTTPを受け付け、クエリーとフラグメントは使用できます。
    [[nodiscard]] bool IsSafeOnlineBrowserUrl(
        std::string_view value,
        bool allowInsecureLoopback);
}
