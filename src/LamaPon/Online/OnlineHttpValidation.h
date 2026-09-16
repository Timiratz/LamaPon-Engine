#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace LamaPon::Detail
{
    [[nodiscard]] std::string NormalizeOnlineServiceBaseUrl(
        std::string value,
        bool allowInsecureLoopback);

    [[nodiscard]] bool IsSafeOnlineOpaqueValue(
        std::string_view value,
        std::size_t maxBytes);

    // Authorization headerへ入れられるvisible ASCIIだけを許可します。
    [[nodiscard]] bool IsSafeOnlineBearerToken(
        std::string_view value,
        std::size_t maxBytes = 8192);

    // backend上のゲーム・環境namespaceに使うASCII識別子です。
    // header値へ安全に載せられ、資格情報storeと同じ文字集合にします。
    [[nodiscard]] bool IsSafeOnlineNamespaceId(
        std::string_view value,
        std::size_t maxBytes);

    [[nodiscard]] bool IsSafeOnlineBrowserUrl(
        std::string_view value,
        bool allowInsecureLoopback);
}
