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
        // サーバー本文やtokenを診断へ転記せず、固定の識別子と診断文だけを使います。
        // 通信操作の結果種別
        CloudSaveWireStatus status{
            CloudSaveWireStatus::InvalidResponse
        };
        // 再試行までの推奨待機秒数
        std::uint32_t retryAfterSeconds{};
        // エンジン定義の固定エラー識別子
        std::string serviceCode;
        // 応答内容を含まない固定診断
        std::string errorMessage;

        // 通信操作が成功したかを返す。
        [[nodiscard]] bool Succeeded() const noexcept
        {
            return status == CloudSaveWireStatus::Succeeded;
        }
    };

    struct CloudSaveManifestResult final
    {
        // 一覧取得の通信結果
        CloudSaveWireOutcome outcome;
        // 検証済みの保存データ一覧
        std::vector<CloudSaveManifestItem> items;
    };

    struct CloudSaveItemResult final
    {
        // 読み書きの通信結果
        CloudSaveWireOutcome outcome;
        // 成功時の保存状態
        std::optional<CloudSaveSnapshot> snapshot;
        // CAS競合時点のリモート状態
        std::optional<CloudSaveSnapshot> conflictSnapshot;
    };

    using CloudSaveHttpSender =
        std::function<HttpResponse(const HttpRequest&)>;

    // wire v1の同期通信だけを担い、journal・再試行・スレッド・キャンセルは上位層で管理します。
    // 所有者は本文へ入れず、サービスがBearerのsubとgame/environmentヘッダーから決めます。
    class CloudSaveClient final
    {
    public:
        // URLと名前空間とHTTP送信処理を設定する(serviceBaseUrl: サービスの基点URL, gameId: ゲームの名前空間ID, environmentId: 環境の名前空間ID, allowInsecureLoopback: ローカルHTTPを許可するか, sender: HTTP送信・空なら標準)。
        explicit CloudSaveClient(
            std::string serviceBaseUrl,
            std::string gameId,
            std::string environmentId,
            bool allowInsecureLoopback = false,
            CloudSaveHttpSender sender = {});

        // 保存状態の一覧を検証して取得する(accessToken: サービスの短命token)。
        [[nodiscard]] CloudSaveManifestResult FetchManifest(
            std::string_view accessToken) const;
        // 保存内容の形式と整合性を検証して取得する(accessToken: サービスの短命token, resource: 取得する保存先)。
        [[nodiscard]] CloudSaveItemResult Read(
            std::string_view accessToken,
            const CloudSaveResource& resource) const;

        // 保存内容を作成またはCAS更新する(accessToken: サービスの短命token, resource: 保存先, content: JSONのバイト列, mutationId: 再送共通の小文字UUIDv4, baseEtag: 更新元の引用符付きETag)。
        // baseEtagなしはIf-None-Match: *による新規作成、指定時はIf-Matchによる更新を要求します。
        [[nodiscard]] CloudSaveItemResult Put(
            std::string_view accessToken,
            const CloudSaveResource& resource,
            const std::vector<std::uint8_t>& content,
            std::string_view mutationId,
            std::optional<std::string> baseEtag = std::nullopt) const;
        // 一致するリビジョンを削除状態にする(accessToken: サービスの短命token, resource: 削除する保存先, mutationId: 再送共通の小文字UUIDv4, baseEtag: 削除元の引用符付きETag)。
        [[nodiscard]] CloudSaveItemResult Delete(
            std::string_view accessToken,
            const CloudSaveResource& resource,
            std::string_view mutationId,
            std::string_view baseEtag) const;

        // 正規化したサービスの基点URLを借用する。
        [[nodiscard]] const std::string&
            ServiceBaseUrl() const noexcept
        {
            return m_serviceBaseUrl;
        }

    private:
        // 名前空間と認証を付けてリダイレクトなしで送る(method: HTTPメソッド, path: 基点へ追加するパス, accessToken: サービスの短命token, body: JSON本文・空なら本文なし, headers: 操作固有のヘッダー, maxResponseBytes: 応答の上限バイト数)。
        [[nodiscard]] HttpResponse Send(
            std::wstring method,
            std::string_view path,
            std::string_view accessToken,
            std::string_view body,
            std::vector<std::pair<
                std::wstring,
                std::wstring>> headers,
            std::size_t maxResponseBytes) const;

        // 正規化したサービスの基点URL
        std::string m_serviceBaseUrl;
        // ゲーム用の名前空間ID
        std::string m_gameId;
        // 環境用の名前空間ID
        std::string m_environmentId;
        // ローカルHTTPの許可状態
        bool m_allowInsecureLoopback{};
        // 同期HTTPの送信処理
        CloudSaveHttpSender m_sender;
    };
}
