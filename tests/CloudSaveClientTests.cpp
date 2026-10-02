#include "LamaPon/Online/CloudSaveClient.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using Json = nlohmann::json;

    // 空JSONのbase64url本文
    constexpr std::string_view EmptyObjectContent = "e30";
    // 空JSON本文のSHA-256
    constexpr std::string_view EmptyObjectHash =
        "RBNvo1WzZ4oRRq0W9-hknpT7T8If536DEMBg9hyq_4o";
    // レベルJSONのbase64url本文
    constexpr std::string_view LevelContent = "eyJsZXZlbCI6N30";
    // レベルJSON本文のSHA-256
    constexpr std::string_view LevelHash =
        "fUV07UsUNLX3A0GWwak_tdwTcH_rWlJ1CqBLNiwLRwY";
    // 作成要求の冪等性キー
    constexpr std::string_view MutationId =
        "123e4567-e89b-42d3-a456-426614174000";
    // 削除要求の冪等性キー
    constexpr std::string_view DeleteMutationId =
        "123e4567-e89b-42d3-b456-426614174001";
    // 競合要求の冪等性キー
    constexpr std::string_view ConflictMutationId =
        "123e4567-e89b-42d3-8456-426614174002";

    // 条件を満たさなければテストを失敗させます。
    // Require(condition: 成功条件, message: 失敗理由)
    void Require(const bool condition, const char* message)
    {
        // 失敗条件
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // 設定領域を表すリソースJSONを返します。
    Json PreferencesResource()
    {
        return Json{ { "kind", "preferences" } };
    }

    // 指定スロットを表すリソースJSONを返します。
    // SlotResource(slot: スロット名)
    Json SlotResource(std::string slot = "slot-1")
    {
        return Json{
            { "kind", "save_slot" },
            { "slot", std::move(slot) }
        };
    }

    // 有効なクラウド保存スナップショットを作ります。
    // LiveSnapshot(resource: リソース識別子, etag: 強い版番号, content: base64url本文, hash: 本文SHA-256, byteLength: 本文バイト数, protocolVersion: 版番号を含める設定)
    Json LiveSnapshot(
        Json resource,
        std::string etag,
        const std::string_view content,
        const std::string_view hash,
        const std::size_t byteLength,
        const bool protocolVersion = true)
    {
        // 応答用スナップショットJSON
        Json result{
            { "resource", std::move(resource) },
            { "etag", std::move(etag) },
            { "deleted", false },
            { "byteLength", byteLength },
            { "sha256", hash },
            { "content", content }
        };
        // プロトコル版を含める場合
        if (protocolVersion)
        {
            result["protocolVersion"] = 1;
        }
        return result;
    }

    // 削除済み保存を表すスナップショットを作ります。
    // Tombstone(resource: リソース識別子, etag: 強い版番号, protocolVersion: 版番号を含める設定)
    Json Tombstone(
        Json resource,
        std::string etag,
        const bool protocolVersion = true)
    {
        // 応答用削除済みスナップショットJSON
        Json result{
            { "resource", std::move(resource) },
            { "etag", std::move(etag) },
            { "deleted", true },
            { "byteLength", 0 }
        };
        // プロトコル版を含める場合
        if (protocolVersion)
        {
            result["protocolVersion"] = 1;
        }
        return result;
    }

    // JSON本文とHTTPヘッダーを持つ応答を作ります。
    // JsonResponse(status: HTTP状態コード, json: 応答JSON, etag: 強い版番号)
    LamaPon::HttpResponse JsonResponse(
        const std::uint32_t status,
        const Json& json,
        std::string_view etag = {})
    {
        // 組み立てるHTTP応答
        LamaPon::HttpResponse response;
        response.statusCode = status;
        // UTF-8で直列化した応答本文
        const auto text = json.dump();
        response.body.assign(text.begin(), text.end());
        response.headers.emplace_back(
            L"Content-Type",
            L"application/json; charset=utf-8");
        response.headers.emplace_back(
            L"Content-Length",
            std::to_wstring(response.body.size()));
        // ETagが指定された場合のみ追加
        if (!etag.empty())
        {
            response.headers.emplace_back(
                L"ETag",
                std::wstring(etag.begin(), etag.end()));
        }
        return response;
    }

    // HTTPヘッダー名をASCII大文字小文字無視で比較します。
    // HeaderNameEquals(left: 比較元ヘッダー, right: 比較先ヘッダー)
    bool HeaderNameEquals(
        const std::wstring_view left,
        const std::wstring_view right)
    {
        // 長さが異なれば一致しない
        if (left.size() != right.size())
        {
            return false;
        }
        // 各文字を同じ大小文字に揃えて比較
        for (std::size_t index = 0; index < left.size(); ++index)
        {
            // ASCII小文字へ揃える関数
            // fold(value: 比較する文字)
            const auto fold = [](const wchar_t value)
            {
                return value >= L'A' && value <= L'Z'
                    ? value - L'A' + L'a'
                    : value;
            };
            // 大小文字を揃えても異なる場合
            if (fold(left[index]) != fold(right[index]))
            {
                return false;
            }
        }
        return true;
    }

    // 条件に一致するHTTPヘッダー数を返します。
    // HeaderCount(request: 検索する要求, name: ヘッダー名, value: 指定時の値)
    std::size_t HeaderCount(
        const LamaPon::HttpRequest& request,
        const std::wstring_view name,
        const std::wstring_view value = {})
    {
        // 条件一致したヘッダー数
        std::size_t count{};
        // 要求内の各ヘッダー
        for (const auto& [headerName, headerValue] : request.headers)
        {
            // 名前と任意の値が一致する場合
            if (HeaderNameEquals(headerName, name)
                && (value.empty() || headerValue == value))
            {
                ++count;
            }
        }
        return count;
    }

    struct ScriptedBackend final
    {
        // 次の送信呼び出しへ返す応答列
        std::deque<LamaPon::HttpResponse> responses;
        // テスト中に送信された要求列
        std::vector<LamaPon::HttpRequest> requests;

        // 要求を記録し、先頭の用意済み応答を返します。
        // Send(request: 送信要求)
        LamaPon::HttpResponse Send(const LamaPon::HttpRequest& request)
        {
            requests.push_back(request);
            // 予定外の要求は明示的な通信エラーを返す
            if (responses.empty())
            {
                // 想定外要求を示す応答
                LamaPon::HttpResponse response;
                response.transportError = "unexpected request secret";
                return response;
            }
            // 次に返す応答を取り出す
            auto response = std::move(responses.front());
            responses.pop_front();
            return response;
        }
    };

    // 要求を偽バックエンドへ送るテスト用クライアントを作ります。
    // MakeClient(backend: 要求と応答を記録するバックエンド)
    LamaPon::Detail::CloudSaveClient MakeClient(
        ScriptedBackend& backend)
    {
        return LamaPon::Detail::CloudSaveClient(
            "https://online.example.test/",
            "game-42",
            "staging",
            false,
            // request: テストバックエンドへ送る要求
            [&backend](const LamaPon::HttpRequest& request)
            {
                return backend.Send(request);
            });
    }

    // 読み書き削除と要求規約の正常系を検証します。
    void TestSuccessfulProtocol()
    {
        // 要求と応答を記録するHTTPモック
        ScriptedBackend backend;
        backend.responses.push_back(JsonResponse(
            200,
            {
                { "protocolVersion", 1 },
                {
                    "items",
                    Json::array({
                        {
                            { "resource", PreferencesResource() },
                            { "etag", "\"prefs-1\"" },
                            { "deleted", false },
                            { "byteLength", 2 },
                            { "sha256", EmptyObjectHash }
                        },
                        {
                            { "resource", SlotResource() },
                            { "etag", "\"slot-0\"" },
                            { "deleted", true },
                            { "byteLength", 0 }
                        }
                    })
                }
            }));
        backend.responses.push_back(JsonResponse(
            200,
            LiveSnapshot(
                PreferencesResource(),
                "\"prefs-1\"",
                EmptyObjectContent,
                EmptyObjectHash,
                2),
            "\"prefs-1\""));
        backend.responses.push_back(JsonResponse(
            201,
            LiveSnapshot(
                SlotResource(),
                "\"slot-1\"",
                LevelContent,
                LevelHash,
                11),
            "\"slot-1\""));
        backend.responses.push_back(JsonResponse(
            200,
            Tombstone(SlotResource(), "\"slot-2\""),
            "\"slot-2\""));
        backend.responses.push_back(JsonResponse(
            412,
            {
                { "protocolVersion", 1 },
                { "error", { { "code", "revision_conflict" } } },
                {
                    "current",
                    LiveSnapshot(
                        SlotResource(),
                        "\"slot-remote\"",
                        EmptyObjectContent,
                        EmptyObjectHash,
                        2,
                        false)
                }
            },
            "\"slot-remote\""));

        // テスト対象のクラウドクライアント
        auto client = MakeClient(backend);
        // 取得したクラウド一覧
        const auto manifest = client.FetchManifest("backend-access-token");
        Require(
            manifest.outcome.Succeeded()
                && manifest.items.size() == 2
                && manifest.items[0].sha256 == EmptyObjectHash
                && manifest.items[1].deleted,
            "A valid cloud manifest was not parsed.");

        // 読み込んだ設定スナップショット
        const auto read = client.Read(
            "backend-access-token",
            LamaPon::CloudSaveResource::Preferences());
        Require(
            read.outcome.Succeeded()
                && read.snapshot
                && std::string(
                    read.snapshot->content.begin(),
                    read.snapshot->content.end()) == "{}",
            "A valid cloud snapshot was not parsed.");

        // 作成・更新するレベルJSONのUTF-8本文
        const std::vector<std::uint8_t> level{
            '{', '"', 'l', 'e', 'v', 'e', 'l', '"', ':', '7', '}'
        };
        // 作成応答
        const auto put = client.Put(
            "backend-access-token",
            LamaPon::CloudSaveResource::SaveSlot("slot-1"),
            level,
            MutationId);
        Require(
            put.outcome.Succeeded()
                && put.snapshot
                && put.snapshot->etag == "\"slot-1\"",
            "A valid cloud create response was not parsed.");

        // 削除応答
        const auto deleted = client.Delete(
            "backend-access-token",
            LamaPon::CloudSaveResource::SaveSlot("slot-1"),
            DeleteMutationId,
            "\"slot-1\"");
        Require(
            deleted.outcome.Succeeded()
                && deleted.snapshot
                && deleted.snapshot->deleted,
            "A valid cloud delete response was not parsed.");

        // 版番号競合の応答
        const auto conflict = client.Put(
            "backend-access-token",
            LamaPon::CloudSaveResource::SaveSlot("slot-1"),
            level,
            ConflictMutationId,
            "\"stale\"");
        Require(
            conflict.outcome.status
                    == LamaPon::Detail::CloudSaveWireStatus::Conflict
                && conflict.conflictSnapshot
                && conflict.conflictSnapshot->etag == "\"slot-remote\"",
            "A valid CAS conflict was not retained.");

        Require(
            backend.responses.empty() && backend.requests.size() == 5,
            "The cloud client retried or omitted a request.");
        // 送信順に期待するHTTPメソッド
        const std::array<std::wstring_view, 5> methods{
            L"GET", L"POST", L"PUT", L"DELETE", L"PUT"
        };
        // 送信順に期待するAPIパス
        const std::array<std::wstring_view, 5> paths{
            L"/v1/cloud-saves/manifest",
            L"/v1/cloud-saves/read",
            L"/v1/cloud-saves/item",
            L"/v1/cloud-saves/item",
            L"/v1/cloud-saves/item"
        };
        // 送信された要求ごとに宛先と秘密情報の扱いを検証
        for (std::size_t index = 0; index < backend.requests.size(); ++index)
        {
            // 今回検証する送信要求
            const auto& request = backend.requests[index];
            Require(
                request.method == methods[index]
                    && request.url
                        == L"https://online.example.test"
                            + std::wstring(paths[index])
                    && !request.followRedirects
                    && HeaderCount(
                        request,
                        L"Authorization",
                        L"Bearer backend-access-token") == 1
                    && HeaderCount(
                        request,
                        L"X-LamaPon-Game-Id",
                        L"game-42") == 1
                    && HeaderCount(
                        request,
                        L"X-LamaPon-Environment-Id",
                        L"staging") == 1
                    && HeaderCount(
                        request,
                        L"Cache-Control",
                        L"no-store") == 1,
                "A cloud request violated its endpoint or secret policy.");
            // 秘密情報混入を調べるUTF-8本文
            const std::string requestText(
                request.body.begin(),
                request.body.end());
            Require(
                requestText.find("backend-access-token")
                        == std::string::npos
                    && requestText.find("playerId") == std::string::npos
                    && requestText.find("discord") == std::string::npos,
                "A cloud request body contained an owner or token.");
        }
        Require(
            backend.requests[0].body.empty()
                && HeaderCount(backend.requests[0], L"Content-Type") == 0
                && HeaderCount(
                    backend.requests[2],
                    L"If-None-Match",
                    L"*") == 1
                && HeaderCount(backend.requests[2], L"If-Match") == 0
                && HeaderCount(
                    backend.requests[3],
                    L"If-Match",
                    L"\"slot-1\"") == 1
                && HeaderCount(
                    backend.requests[4],
                    L"If-Match",
                    L"\"stale\"") == 1
                && HeaderCount(
                    backend.requests[2],
                    L"Idempotency-Key",
                    std::wstring(MutationId.begin(), MutationId.end())) == 1
                && HeaderCount(
                    backend.requests[3],
                    L"Idempotency-Key",
                    std::wstring(
                        DeleteMutationId.begin(),
                        DeleteMutationId.end())) == 1
                && HeaderCount(
                    backend.requests[4],
                    L"Idempotency-Key",
                    std::wstring(
                        ConflictMutationId.begin(),
                        ConflictMutationId.end())) == 1,
            "Cloud CAS or idempotency headers were incorrect.");
        // 作成要求本文を解析したJSON
        const auto putBody = Json::parse(
            std::string(
                backend.requests[2].body.begin(),
                backend.requests[2].body.end()));
        Require(
            putBody.at("content").get<std::string>()
                    == std::string(LevelContent)
                && putBody.at("sha256").get<std::string>()
                    == std::string(LevelHash)
                && putBody.at("byteLength") == 11
                && putBody.at("resource").at("slot") == "slot-1",
            "Cloud create payload was not canonical.");
    }

    // 不正入力がHTTP送信へ到達しないことを検証します。
    void TestInvalidInputsDoNotSend()
    {
        // 要求数を確認する偽バックエンド
        ScriptedBackend backend;
        // 入力検証対象のクライアント
        auto client = MakeClient(backend);
        // 正常なJSON本文
        const std::vector<std::uint8_t> valid{ '{', '}' };
        // 閉じ括弧が不足したJSON本文
        const std::vector<std::uint8_t> invalid{ '{' };
        // ネスト上限を超えるJSON文字列
        std::string tooDeepJson(65, '[');
        tooDeepJson += "null";
        tooDeepJson.append(65, ']');
        // ネスト上限超過のUTF-8本文
        const std::vector<std::uint8_t> tooDeep(
            tooDeepJson.begin(),
            tooDeepJson.end());

        Require(
            client.FetchManifest("bad token").outcome.status
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidRequest
                && client.Read(
                    "token",
                    LamaPon::CloudSaveResource::SaveSlot("CON"))
                    .outcome.status
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidRequest
                && client.Put(
                    "token",
                    LamaPon::CloudSaveResource::Preferences(),
                    invalid,
                    MutationId).outcome.status
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidRequest
                && client.Put(
                    "token",
                    LamaPon::CloudSaveResource::Preferences(),
                    valid,
                    "not-a-uuid").outcome.status
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidRequest
                && client.Put(
                    "token",
                    LamaPon::CloudSaveResource::Preferences(),
                    tooDeep,
                    MutationId).outcome.status
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidRequest
                && client.Delete(
                    "token",
                    LamaPon::CloudSaveResource::Preferences(),
                    MutationId,
                    "W/\"weak\"").outcome.status
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidRequest
                && backend.requests.empty(),
            "Invalid cloud input reached the HTTP sender.");

        // Windowsの予約名を順に拒否
        for (const std::string_view reserved : {
                "CON", "con.txt", "NUL", "AUX.save", "PRN",
                "CLOCK$", "CONIN$", "CONOUT$", "COM1", "LPT9.log",
                "COM¹", "LPT².txt", "COM³.save", "CON .txt" })
        {
            Require(
                client.Read(
                    "token",
                    LamaPon::CloudSaveResource::SaveSlot(
                        std::string(reserved))).outcome.status
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidRequest,
                "A Windows reserved save slot was accepted.");
        }
        Require(
            backend.requests.empty(),
            "A Windows reserved save slot reached the HTTP sender.");

        // 危険な識別子の拒否結果
        bool invalidNamespaceRejected{};
        // 不正な名前空間のコンストラクター検証
        try
        {
            // 不正な識別子で構築する検査対象
            LamaPon::Detail::CloudSaveClient invalidClient(
                "https://online.example.test",
                "bad/game",
                "production");
        }
        // 不正な識別子に対する期待例外
        catch (const std::invalid_argument&)
        {
            invalidNamespaceRejected = true;
        }
        Require(
            invalidNamespaceRejected,
            "An unsafe cloud namespace was accepted.");
    }

    // 用意した応答で読取結果と要求数を返します。
    // ReadWithResponse(response: テスト応答, requestCount: 送信数の出力先, publicMessage: 公開エラーの出力先)
    LamaPon::Detail::CloudSaveWireStatus ReadWithResponse(
        LamaPon::HttpResponse response,
        std::size_t& requestCount,
        std::string& publicMessage)
    {
        // 指定応答を一度だけ返すクライアント
        LamaPon::Detail::CloudSaveClient client(
            "https://online.example.test",
            "game",
            "production",
            false,
            [&response, &requestCount](const LamaPon::HttpRequest&)
            {
                ++requestCount;
                return response;
            });
        // 読取操作の結果
        const auto result = client.Read(
            "backend-token",
            LamaPon::CloudSaveResource::Preferences());
        publicMessage = result.outcome.errorMessage
            + result.outcome.serviceCode;
        return result.outcome.status;
    }

    // 不正応答の拒否とHTTP状態の対応を検証します。
    void TestInvalidResponsesAndStatusMapping()
    {
        {
            // ETag重複を含むテスト応答
            auto response = JsonResponse(
                200,
                LiveSnapshot(
                    PreferencesResource(),
                    "\"v1\"",
                    EmptyObjectContent,
                    EmptyObjectHash,
                    2),
                "\"v1\"");
            response.headers.emplace_back(L"etag", L"\"v1\"");
            // 応答を受け取った要求数
            std::size_t count{};
            // 利用者へ返すエラー文
            std::string message;
            Require(
                ReadWithResponse(response, count, message)
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidResponse
                    && count == 1,
                "Duplicate response ETags were accepted.");
        }
        {
            // 非正規base64urlを含むテスト応答
            auto response = JsonResponse(
                200,
                LiveSnapshot(
                    PreferencesResource(),
                    "\"v1\"",
                    "e31",
                    EmptyObjectHash,
                    2),
                "\"v1\"");
            // 応答を受け取った要求数
            std::size_t count{};
            // 利用者へ返すエラー文
            std::string message;
            Require(
                ReadWithResponse(response, count, message)
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidResponse,
                "Non-canonical base64url was accepted.");
        }
        {
            // 不正Content-Typeを持つテスト応答
            auto response = JsonResponse(
                200,
                LiveSnapshot(
                    PreferencesResource(),
                    "\"v1\"",
                    EmptyObjectContent,
                    EmptyObjectHash,
                    2),
                "\"v1\"");
            response.headers[0].second = L"text/json";
            // 応答を受け取った要求数
            std::size_t count{};
            // 利用者へ返すエラー文
            std::string message;
            Require(
                ReadWithResponse(response, count, message)
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidResponse,
                "An invalid JSON Content-Type was accepted.");
        }
        {
            // Content-Length不一致を持つテスト応答
            auto response = JsonResponse(
                200,
                LiveSnapshot(
                    PreferencesResource(),
                    "\"v1\"",
                    EmptyObjectContent,
                    EmptyObjectHash,
                    2),
                "\"v1\"");
            response.headers[1].second = L"1";
            // 応答を受け取った要求数
            std::size_t count{};
            // 利用者へ返すエラー文
            std::string message;
            Require(
                ReadWithResponse(response, count, message)
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidResponse,
                "A mismatched Content-Length was accepted.");
        }
        {
            // 上限を超える本文を持つテスト応答
            LamaPon::HttpResponse response;
            response.statusCode = 401;
            response.body.assign(2000000, 'x');
            // 応答を受け取った要求数
            std::size_t count{};
            // 利用者へ返すエラー文
            std::string message;
            Require(
                ReadWithResponse(response, count, message)
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidResponse,
                "A fake sender bypassed the response size cap.");
        }
        {
            // 過大なRetry-Afterを持つテスト応答
            LamaPon::HttpResponse response;
            response.statusCode = 429;
            response.headers.emplace_back(L"Retry-After", L"999");
            // レート制限応答を返すクライアント
            LamaPon::Detail::CloudSaveClient client(
                "https://online.example.test",
                "game",
                "production",
                false,
                // 要求ごとに同じレート制限応答を返す
                [response](const LamaPon::HttpRequest&)
                {
                    return response;
                });
            // レート制限として分類された一覧取得結果
            const auto result = client.FetchManifest("backend-token");
            Require(
                result.outcome.status
                    == LamaPon::Detail::CloudSaveWireStatus::RateLimited
                    && result.outcome.retryAfterSeconds == 300,
                "Rate limit retry metadata was not bounded.");
        }
        {
            // 認証失敗を表すテスト応答
            LamaPon::HttpResponse response;
            response.statusCode = 401;
            // 応答を受け取った要求数
            std::size_t count{};
            // 利用者へ返すエラー文
            std::string message;
            Require(
                ReadWithResponse(response, count, message)
                    == LamaPon::Detail::CloudSaveWireStatus::Unauthorized
                    && count == 1,
                "An authorization failure was misclassified.");
        }
        {
            // 秘密文字列のエラーコードを持つ応答
            auto response = JsonResponse(
                401,
                { { "error", { { "code", "backend-token" } } } });
            // 応答を受け取った要求数
            std::size_t count{};
            // 秘密文字列を含まない公開エラー文
            std::string message;
            Require(
                ReadWithResponse(response, count, message)
                        == LamaPon::Detail::CloudSaveWireStatus::Unauthorized
                    && message.find("backend-token") == std::string::npos
                    && message.find("unauthorized") != std::string::npos,
                "A server-controlled error code escaped through the outcome.");
        }
        {
            // 競合応答を返す偽バックエンド
            ScriptedBackend backend;
            backend.responses.push_back(JsonResponse(
                412,
                {
                    { "protocolVersion", 1 },
                    { "error", { { "code", "revision_conflict" } } },
                    {
                        "current",
                        LiveSnapshot(
                            PreferencesResource(),
                            "\"remote\"",
                            EmptyObjectContent,
                            EmptyObjectHash,
                            2,
                            false)
                    }
                }));
            // 競合応答の検査対象クライアント
            auto client = MakeClient(backend);
            // 作成要求に使う正規JSON本文
            const std::vector<std::uint8_t> content{ '{', '}' };
            // ETag欠落競合への書込結果
            const auto result = client.Put(
                "backend-token",
                LamaPon::CloudSaveResource::Preferences(),
                content,
                MutationId,
                "\"stale\"");
            Require(
                result.outcome.status
                        == LamaPon::Detail::CloudSaveWireStatus::InvalidResponse
                    && backend.requests.size() == 1,
                "A conflict without a strong response ETag was accepted.");
        }
        {
            // 対象データなしを表すテスト応答
            LamaPon::HttpResponse response;
            response.statusCode = 404;
            // 応答を受け取った要求数
            std::size_t count{};
            // 利用者へ返すエラー文
            std::string message;
            Require(
                ReadWithResponse(response, count, message)
                    == LamaPon::Detail::CloudSaveWireStatus::NotFound
                    && count == 1,
                "A missing cloud item was misclassified.");
        }
        {
            // 一時的なサービス障害を表す応答
            LamaPon::HttpResponse response;
            response.statusCode = 503;
            // 応答を受け取った要求数
            std::size_t count{};
            // 利用者へ返すエラー文
            std::string message;
            Require(
                ReadWithResponse(response, count, message)
                    == LamaPon::Detail::CloudSaveWireStatus::RetryableServiceError
                    && count == 1,
                "A retryable service status was misclassified or retried.");
        }
        {
            // 秘密情報を含む通信エラー応答
            LamaPon::HttpResponse response;
            response.transportError =
                "transport leaked backend-token and player-secret";
            // 応答を受け取った要求数
            std::size_t count{};
            // 秘密情報が除去された公開エラー文
            std::string message;
            Require(
                ReadWithResponse(response, count, message)
                    == LamaPon::Detail::CloudSaveWireStatus::TransportError
                    && message.find("backend-token") == std::string::npos
                    && message.find("player-secret") == std::string::npos,
                "A transport failure leaked a secret.");
        }
        {
            // 送信コールバックの呼出回数
            std::size_t count{};
            // 送信時に例外を投げるクライアント
            LamaPon::Detail::CloudSaveClient client(
                "https://online.example.test",
                "game",
                "production",
                false,
                // 要求を数えてから故意に失敗させる
                [&count](const LamaPon::HttpRequest&) -> LamaPon::HttpResponse
                {
                    ++count;
                    throw std::runtime_error("backend-token secret");
                });
            // 送信例外が通信失敗になることを確認
            const auto result = client.FetchManifest("backend-token");
            Require(
                result.outcome.status
                    == LamaPon::Detail::CloudSaveWireStatus::TransportError
                    && result.outcome.errorMessage.find("backend-token")
                        == std::string::npos
                    && count == 1,
                "A sender exception escaped or was retried.");
        }
    }

    // 競合一覧、重複JSON、上限超過を拒否することを検証します。
    void TestManifestConflictsFailClosed()
    {
        // 大文字小文字だけ異なるスロット応答
        ScriptedBackend backend;
        backend.responses.push_back(JsonResponse(
            200,
            {
                { "protocolVersion", 1 },
                {
                    "items",
                    Json::array({
                        {
                            { "resource", SlotResource("Save") },
                            { "etag", "\"one\"" },
                            { "deleted", false },
                            { "byteLength", 2 },
                            { "sha256", EmptyObjectHash }
                        },
                        {
                            { "resource", SlotResource("save") },
                            { "etag", "\"two\"" },
                            { "deleted", false },
                            { "byteLength", 2 },
                            { "sha256", EmptyObjectHash }
                        }
                    })
                }
            }));
        // 競合一覧の解析対象クライアント
        auto client = MakeClient(backend);
        // 競合一覧の取得結果
        const auto result = client.FetchManifest("backend-token");
        Require(
            result.outcome.status
                    == LamaPon::Detail::CloudSaveWireStatus::InvalidResponse
                && result.items.empty(),
            "Case-colliding remote slots were accepted.");

        // 重複キー応答の偽バックエンド
        ScriptedBackend duplicateKeys;
        // JSONオブジェクトに重複キーを含む応答
        LamaPon::HttpResponse duplicateResponse;
        duplicateResponse.statusCode = 200;
        // protocolVersionを重複させたJSON本文
        const std::string duplicateBody =
            R"({"protocolVersion":1,"protocolVersion":1,"items":[]})";
        duplicateResponse.body.assign(
            duplicateBody.begin(),
            duplicateBody.end());
        duplicateResponse.headers.emplace_back(
            L"Content-Type",
            L"application/json");
        duplicateResponse.headers.emplace_back(
            L"Content-Length",
            std::to_wstring(duplicateResponse.body.size()));
        duplicateKeys.responses.push_back(std::move(duplicateResponse));
        // 重複キー応答を検査するクライアント
        auto duplicateClient = MakeClient(duplicateKeys);
        Require(
            duplicateClient.FetchManifest("backend-token").outcome.status
                == LamaPon::Detail::CloudSaveWireStatus::InvalidResponse,
            "Duplicate JSON keys were accepted.");

        // スロット数上限検査の偽バックエンド
        ScriptedBackend tooMany;
        // 上限を超える一覧JSON配列
        Json items = Json::array();
        // 33スロットの応答を作成
        for (int index = 0; index < 33; ++index)
        {
            items.push_back({
                { "resource", SlotResource("slot-" + std::to_string(index)) },
                { "etag", "\"revision-" + std::to_string(index) + "\"" },
                { "deleted", true },
                { "byteLength", 0 }
            });
        }
        tooMany.responses.push_back(JsonResponse(
            200,
            {
                { "protocolVersion", 1 },
                { "items", std::move(items) }
            }));
        // スロット数上限応答の検査対象
        auto tooManyClient = MakeClient(tooMany);
        Require(
            tooManyClient.FetchManifest("backend-token").outcome.status
                == LamaPon::Detail::CloudSaveWireStatus::InvalidResponse,
            "A manifest with 33 save slots was accepted.");

        // アカウント容量上限検査の偽バックエンド
        ScriptedBackend overQuota;
        // 容量上限を超える保存一覧
        Json quotaItems = Json::array();
        // 17個の最大サイズ保存を作成
        for (int index = 0; index < 17; ++index)
        {
            quotaItems.push_back({
                { "resource", SlotResource("quota-" + std::to_string(index)) },
                { "etag", "\"quota-" + std::to_string(index) + "\"" },
                { "deleted", false },
                { "byteLength", LamaPon::CloudSaveSlotMaxBytes },
                { "sha256", EmptyObjectHash }
            });
        }
        overQuota.responses.push_back(JsonResponse(
            200,
            {
                { "protocolVersion", 1 },
                { "items", std::move(quotaItems) }
            }));
        // 容量上限応答の検査対象
        auto overQuotaClient = MakeClient(overQuota);
        Require(
            overQuotaClient.FetchManifest("backend-token").outcome.status
                == LamaPon::Detail::CloudSaveWireStatus::InvalidResponse,
            "A manifest above the account byte quota was accepted.");
    }
}

// クラウド保存クライアントのプロトコル検証を実行します。
int main()
{
    // 例外を失敗コードとして報告
    try
    {
        TestSuccessfulProtocol();
        TestInvalidInputsDoNotSend();
        TestInvalidResponsesAndStatusMapping();
        TestManifestConflictsFailClosed();
        std::cout << "Cloud save wire protocol tests passed.\n";
        // 全検証の成功
        return 0;
    }
    // テスト例外を標準エラーへ出力
    // catch(exception: 検証中に発生した例外)
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
