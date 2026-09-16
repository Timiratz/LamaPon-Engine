#pragma once

#include "LamaPon/Core/HttpClient.h"
#include "LamaPon/Online/CloudSave.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon::Detail
{
    enum class CloudSaveWireStatus : std::uint8_t
    {
        Succeeded,
        NotFound,
        Conflict,
        Unauthorized,
        RateLimited,
        RetryableServiceError,
        Rejected,
        TransportError,
        InvalidRequest,
        InvalidResponse
    };

    struct CloudSaveWireOutcome final
    {
        CloudSaveWireStatus status{
            CloudSaveWireStatus::InvalidResponse
        };
        std::uint32_t retryAfterSeconds{};
        // エンジンが定義した固定codeだけです。サーバー本文やtokenは
        // serviceCode/errorMessageへ転記しません。
        std::string serviceCode;
        std::string errorMessage;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return status == CloudSaveWireStatus::Succeeded;
        }
    };

    struct CloudSaveManifestResult final
    {
        CloudSaveWireOutcome outcome;
        std::vector<CloudSaveManifestItem> items;
    };

    struct CloudSaveItemResult final
    {
        CloudSaveWireOutcome outcome;
        // Succeeded時のrevisionです。NotFound等では空です。
        std::optional<CloudSaveSnapshot> snapshot;
        // Conflict時だけ、CAS判定時点のremote snapshotを保持します。
        std::optional<CloudSaveSnapshot> conflictSnapshot;
    };

    using CloudSaveHttpSender =
        std::function<HttpResponse(const HttpRequest&)>;

    // Cloud save wire protocol v1だけを扱う同期クライアントです。
    // journal、retry scheduling、スレッド・cancelは上位層の責務です。
    // bearerはDiscord tokenではなくLamaPon backendの短命tokenです。
    // endpointは /v1/cloud-saves/manifest (GET)、read (POST)、
    // item (PUT/DELETE) に固定し、resourceはJSON本文の
    // {kind:"preferences"} または
    // {kind:"save_slot",slot:"..."} で表します。所有者は本文で
    // 指定せず、backendがBearerのsubとgame/environment headerを
    // 結び付けて決定します。
    class CloudSaveClient final
    {
    public:
        explicit CloudSaveClient(
            std::string serviceBaseUrl,
            std::string gameId,
            std::string environmentId,
            bool allowInsecureLoopback = false,
            CloudSaveHttpSender sender = {});

        [[nodiscard]] CloudSaveManifestResult FetchManifest(
            std::string_view accessToken) const;
        [[nodiscard]] CloudSaveItemResult Read(
            std::string_view accessToken,
            const CloudSaveResource& resource) const;

        // baseEtagが無い場合は新規作成（If-None-Match: *）、ある場合は
        // CAS更新（If-Match）です。
        [[nodiscard]] CloudSaveItemResult Put(
            std::string_view accessToken,
            const CloudSaveResource& resource,
            const std::vector<std::uint8_t>& content,
            std::string_view mutationId,
            std::optional<std::string> baseEtag = std::nullopt) const;
        [[nodiscard]] CloudSaveItemResult Delete(
            std::string_view accessToken,
            const CloudSaveResource& resource,
            std::string_view mutationId,
            std::string_view baseEtag) const;

        [[nodiscard]] const std::string&
            ServiceBaseUrl() const noexcept
        {
            return m_serviceBaseUrl;
        }

    private:
        [[nodiscard]] HttpResponse Send(
            std::wstring method,
            std::string_view path,
            std::string_view accessToken,
            std::string_view body,
            std::vector<std::pair<
                std::wstring,
                std::wstring>> headers,
            std::size_t maxResponseBytes) const;

        std::string m_serviceBaseUrl;
        std::string m_gameId;
        std::string m_environmentId;
        bool m_allowInsecureLoopback{};
        CloudSaveHttpSender m_sender;
    };
}
