#include "LamaPon/Core/LocalPersistenceDocuments.h"
#include "LamaPon/Core/PersistenceProfiles.h"
#include "LamaPon/Core/PlayerPrefs.h"
#include "LamaPon/Core/SaveData.h"
#include "LamaPon/Online/CloudSaveClient.h"
#include "LamaPon/Online/CloudSaveJournal.h"
#include "LamaPon/Online/CloudSaveSynchronizer.h"

#include <Windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using Json = nlohmann::json;
    using Documents = LamaPon::Detail::LocalPersistenceDocuments;
    using Journal = LamaPon::Detail::CloudSaveJournal;
    using Synchronizer = LamaPon::Detail::CloudSaveSynchronizer;

    // テスト用アカウント識別子
    constexpr std::string_view AccountKey =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    // テスト用認証トークン
    constexpr std::string_view Token = "backend-access-token";
    // テスト用の連続更新ID
    constexpr std::array<std::string_view, 8> MutationIds{
        "123e4567-e89b-42d3-a456-426614174000",
        "123e4567-e89b-42d3-b456-426614174001",
        "123e4567-e89b-42d3-8456-426614174002",
        "123e4567-e89b-42d3-9456-426614174003",
        "123e4567-e89b-42d3-a456-426614174004",
        "123e4567-e89b-42d3-b456-426614174005",
        "123e4567-e89b-42d3-8456-426614174006",
        "123e4567-e89b-42d3-9456-426614174007"
    };

    // 条件不成立をテスト失敗にします。
    // Require(condition: 成否を判定する条件, message: 失敗時の説明)
    void Require(const bool condition, const char* message)
    {
        // 不成立時は失敗理由を例外で伝えます。
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // 同期テスト専用の作業先を返します。
    std::filesystem::path TestRoot()
    {
        // テスト出力配下に限定する作業先
        const auto root = std::filesystem::absolute(
            std::filesystem::current_path()
            / L"test-output"
            / L"cloud-save-synchronizer").lexically_normal();
        // 作業先がテスト出力配下にあることを確認します。
        Require(
            root.parent_path().filename() == L"test-output",
            "Synchronizer test root escaped test-output.");
        return root;
    }

    // 同期テスト専用の作業先を作り直します。
    void ResetTestRoot()
    {
        // 削除・作成時のファイルシステムエラー
        std::error_code error;
        // 前回のテストデータを除去します。
        std::filesystem::remove_all(TestRoot(), error);
        // 削除に失敗した場合はテストを止めます。
        Require(!error, "Synchronizer test cleanup failed.");
        // 空のテスト作業先を作成します。
        std::filesystem::create_directories(TestRoot(), error);
        // 作成に失敗した場合はテストを止めます。
        Require(!error, "Synchronizer test root creation failed.");
    }

    // 信頼済み表示名から保存先を組み立てます。
    // TrustedPath(name: テストケースの保存領域名)
    std::filesystem::path TrustedPath(const std::string_view name)
    {
        return TestRoot()
            / std::filesystem::path(name)
            / L"GuestDisplayName";
    }

    // 信頼済みパスから永続化先一式を作ります。
    // Profile(trusted: プロファイル所有権を示す信頼済みパス)
    LamaPon::PersistenceProfilePaths Profile(
        const std::filesystem::path& trusted)
    {
        // アカウント単位のオンライン保存領域
        const auto root = trusted.parent_path()
            / L"OnlineProfiles"
            / std::filesystem::path(AccountKey);
        return {
            root,
            root / L"PlayerPrefs.json",
            root / L"Saves",
            std::string(AccountKey),
            false
        };
    }

    // 文字列のバイト列を複製します。
    // Bytes(text: 変換する文字列)
    std::vector<std::uint8_t> Bytes(const std::string_view text)
    {
        return {
            reinterpret_cast<const std::uint8_t*>(text.data()),
            reinterpret_cast<const std::uint8_t*>(text.data() + text.size())
        };
    }

    // Base64URL文字対応表
    constexpr char Base64UrlAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    // バイト列をパディングなしのBase64URLへ変換します。
    // Base64Url(bytes: 変換対象の先頭, size: 変換対象の長さ)
    std::string Base64Url(
        const std::uint8_t* const bytes,
        const std::size_t size)
    {
        // 変換結果を順に追加する文字列
        std::string result;
        result.reserve((size * 4u + 2u) / 3u);
        // 未処理データの先頭位置
        std::size_t index{};
        // 3バイトずつ4文字へ変換します。
        while (index + 3u <= size)
        {
            // 3バイトを連結した24ビット値
            const auto value =
                (static_cast<std::uint32_t>(bytes[index]) << 16u)
                | (static_cast<std::uint32_t>(bytes[index + 1u]) << 8u)
                | bytes[index + 2u];
            result.push_back(Base64UrlAlphabet[(value >> 18u) & 0x3fu]);
            result.push_back(Base64UrlAlphabet[(value >> 12u) & 0x3fu]);
            result.push_back(Base64UrlAlphabet[(value >> 6u) & 0x3fu]);
            result.push_back(Base64UrlAlphabet[value & 0x3fu]);
            index += 3u;
        }
        // 末尾に1バイトだけ残った場合の2文字
        if (size - index == 1u)
        {
            // 末尾1バイトを上位側へ配置した値
            const auto value =
                static_cast<std::uint32_t>(bytes[index]) << 16u;
            result.push_back(Base64UrlAlphabet[(value >> 18u) & 0x3fu]);
            result.push_back(Base64UrlAlphabet[(value >> 12u) & 0x3fu]);
        }
        // 末尾に2バイト残った場合の3文字
        else if (size - index == 2u)
        {
            // 末尾2バイトを上位側へ配置した値
            const auto value =
                (static_cast<std::uint32_t>(bytes[index]) << 16u)
                | (static_cast<std::uint32_t>(bytes[index + 1u]) << 8u);
            result.push_back(Base64UrlAlphabet[(value >> 18u) & 0x3fu]);
            result.push_back(Base64UrlAlphabet[(value >> 12u) & 0x3fu]);
            result.push_back(Base64UrlAlphabet[(value >> 6u) & 0x3fu]);
        }
        return result;
    }

    // Windows CNGでバイト列のSHA-256を計算します。
    // Hash(bytes: ハッシュ対象の内容)
    std::string Hash(const std::vector<std::uint8_t>& bytes)
    {
        // SHA-256用CNGプロバイダー
        BCRYPT_ALG_HANDLE algorithm{};
        // CNGプロバイダーを取得します。
        Require(
            BCryptOpenAlgorithmProvider(
                &algorithm,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                0u) >= 0,
            "SHA-256 provider failed.");
        // SHA-256の固定長出力
        std::array<std::uint8_t, 32> digest{};
        // ハッシュ処理のCNGステータス
        const auto status = BCryptHash(
            algorithm,
            nullptr,
            0u,
            const_cast<PUCHAR>(bytes.data()),
            static_cast<ULONG>(bytes.size()),
            digest.data(),
            static_cast<ULONG>(digest.size()));
        BCryptCloseAlgorithmProvider(algorithm, 0u);
        // ハッシュ処理に失敗した場合はテストを止めます。
        Require(status >= 0, "SHA-256 failed.");
        return Base64Url(digest.data(), digest.size());
    }

    // 保存対象の識別情報をJSONへ変換します。
    // ResourceJson(resource: JSON化する保存対象)
    Json ResourceJson(const LamaPon::CloudSaveResource& resource)
    {
        // 設定保存はslotを持たない識別形式にします。
        if (resource.kind == LamaPon::CloudSaveResourceKind::Preferences)
        {
            return { { "kind", "preferences" } };
        }
        return {
            { "kind", "save_slot" },
            { "slot", resource.slot }
        };
    }

    // 保存スナップショットをプロトコルJSONへ変換します。
    // SnapshotJson(snapshot: 保存状態, includeProtocolVersion: 版を含めるか)
    Json SnapshotJson(
        const LamaPon::CloudSaveSnapshot& snapshot,
        const bool includeProtocolVersion = true)
    {
        // 削除状態を含むスナップショットの基本項目
        Json result{
            { "resource", ResourceJson(snapshot.resource) },
            { "etag", snapshot.etag },
            { "deleted", snapshot.deleted },
            { "byteLength", snapshot.content.size() }
        };
        // 削除されていない場合だけ本文とハッシュを含めます。
        if (!snapshot.deleted)
        {
            result["sha256"] = snapshot.sha256;
            result["content"] = Base64Url(
                snapshot.content.data(),
                snapshot.content.size());
        }
        // 呼出側の指定に従ってプロトコル版を付けます。
        if (includeProtocolVersion)
        {
            result["protocolVersion"] = 1;
        }
        return result;
    }

    // マニフェスト項目をプロトコルJSONへ変換します。
    // ManifestItemJson(snapshot: JSON化するマニフェスト項目)
    Json ManifestItemJson(const LamaPon::CloudSaveSnapshot& snapshot)
    {
        // 削除状態を含むマニフェスト項目の共通部分
        Json result{
            { "resource", ResourceJson(snapshot.resource) },
            { "etag", snapshot.etag },
            { "deleted", snapshot.deleted },
            { "byteLength", snapshot.content.size() }
        };
        // 生存データだけハッシュをマニフェストへ含めます。
        if (!snapshot.deleted)
        {
            result["sha256"] = snapshot.sha256;
        }
        return result;
    }

    // JSON本文とHTTPヘッダーを持つ応答を作ります。
    // JsonResponse(status: HTTP状態, json: 本文, etag: 任意の世代タグ)
    LamaPon::HttpResponse JsonResponse(
        const std::uint32_t status,
        const Json& json,
        const std::string_view etag = {})
    {
        // 指定ステータスを持つHTTP応答
        LamaPon::HttpResponse response;
        response.statusCode = status;
        // JSON本文のUTF-8表現
        const auto text = json.dump();
        response.body.assign(text.begin(), text.end());
        response.headers.emplace_back(
            L"Content-Type",
            L"application/json; charset=utf-8");
        response.headers.emplace_back(
            L"Content-Length",
            std::to_wstring(response.body.size()));
        // ETagがあるときだけ応答ヘッダーへ加えます。
        if (!etag.empty())
        {
            response.headers.emplace_back(
                L"ETag",
                std::wstring(etag.begin(), etag.end()));
        }
        return response;
    }

    // 複数スナップショットのマニフェスト応答を作ります。
    // ManifestResponse(snapshots: マニフェストへ含めるスナップショット)
    LamaPon::HttpResponse ManifestResponse(
        const std::vector<LamaPon::CloudSaveSnapshot>& snapshots)
    {
        // マニフェスト項目を格納する配列
        Json items = Json::array();
        // 各スナップショットをJSON配列へ追加
        for (const auto& snapshot : snapshots)
        {
            items.push_back(ManifestItemJson(snapshot));
        }
        return JsonResponse(
            200u,
            { { "protocolVersion", 1 }, { "items", std::move(items) } });
    }

    // 単一スナップショットの応答を作ります。
    // SnapshotResponse(status: HTTPステータス, snapshot: 応答するスナップショット)
    LamaPon::HttpResponse SnapshotResponse(
        const std::uint32_t status,
        const LamaPon::CloudSaveSnapshot& snapshot)
    {
        return JsonResponse(
            status,
            SnapshotJson(snapshot),
            snapshot.etag);
    }

    // 世代競合を示す412応答を作ります。
    // ConflictResponse(snapshot: 競合時点のサーバー状態)
    LamaPon::HttpResponse ConflictResponse(
        const LamaPon::CloudSaveSnapshot& snapshot)
    {
        return JsonResponse(
            412u,
            {
                { "protocolVersion", 1 },
                { "error", { { "code", "revision_conflict" } } },
                { "current", SnapshotJson(snapshot, false) }
            },
            snapshot.etag);
    }

    using Handler =
        std::function<LamaPon::HttpResponse(const LamaPon::HttpRequest&)>;

    struct ScriptedBackend final
    {
        // 次のHTTP応答として使うハンドラーを登録します。
        // Push(handler: 送信要求に応答する処理)
        void Push(Handler handler)
        {
            // 登録順とキューを保護するロック
            std::scoped_lock lock(mutex);
            handlers.push_back(std::move(handler));
        }

        // 登録済みの次応答を使って要求を処理します。
        // Send(request: テスト対象クライアントからの要求)
        LamaPon::HttpResponse Send(const LamaPon::HttpRequest& request)
        {
            // キューから取り出した応答処理
            Handler handler;
            {
                // 要求記録とハンドラーキューを保護します。
                std::scoped_lock lock(mutex);
                requests.push_back(request);
                // 応答処理が登録されていない要求を異常応答にします。
                if (handlers.empty())
                {
                    // 登録外要求を示すトランスポートエラー
                    LamaPon::HttpResponse response;
                    response.transportError = "unexpected fake request";
                    return response;
                }
                // 次に登録された応答処理を取り出します。
                handler = std::move(handlers.front());
                handlers.pop_front();
            }
            return handler(request);
        }

        // これまでに受け取ったHTTP要求を返します。
        [[nodiscard]] std::vector<LamaPon::HttpRequest> Requests() const
        {
            // 要求一覧を保護してコピーします。
            std::scoped_lock lock(mutex);
            return requests;
        }

        // 要求一覧と応答キューを保護する排他制御
        mutable std::mutex mutex;
        // 今後受け取る要求への応答処理
        std::deque<Handler> handlers;
        // 受信した要求の履歴
        std::vector<LamaPon::HttpRequest> requests;
    };

    // スクリプト済みバックエンドへ接続するテスト用クライアントを作ります。
    // MakeClient(backend: HTTP応答を供給するテストバックエンド)
    std::shared_ptr<LamaPon::Detail::CloudSaveClient> MakeClient(
        ScriptedBackend& backend)
    {
        return std::make_shared<LamaPon::Detail::CloudSaveClient>(
            "https://online.example.test/tenant/",
            "sync-game",
            "staging",
            false,
            // request: クライアント要求をテストバックエンドへ渡します。
            [&backend](const LamaPon::HttpRequest& request)
            {
                return backend.Send(request);
            });
    }

    // HTTP要求本文を文字列として返します。
    // RequestText(request: 本文を読み取るHTTP要求)
    std::string RequestText(const LamaPon::HttpRequest& request)
    {
        return { request.body.begin(), request.body.end() };
    }

    // 大文字小文字を区別せずHTTP要求ヘッダーを探します。
    // RequestHeader(request: 検索対象の要求, name: 検索するヘッダー名)
    std::optional<std::wstring> RequestHeader(
        const LamaPon::HttpRequest& request,
        const std::wstring_view name)
    {
        // headerName/value: 要求ヘッダー名と値を検索します。
        for (const auto& [headerName, value] : request.headers)
        {
            // ヘッダー名は大文字小文字を区別しません。
            if (_wcsicmp(headerName.c_str(), std::wstring(name).c_str()) == 0)
            {
                return value;
            }
        }
        return std::nullopt;
    }

    // PUT要求から保存済みスナップショットを復元します。
    // SnapshotFromPut(request: PUT要求, etag: 応答の世代タグ)
    LamaPon::CloudSaveSnapshot SnapshotFromPut(
        const LamaPon::HttpRequest& request,
        std::string etag)
    {
        // 要求本文をJSONとして解析した値
        const auto body = Json::parse(RequestText(request));
        // 要求中のkindに対応する保存対象
        LamaPon::CloudSaveResource resource =
            body["resource"]["kind"] == "preferences"
            ? LamaPon::CloudSaveResource::Preferences()
            : LamaPon::CloudSaveResource::SaveSlot(
                body["resource"]["slot"].get<std::string>());

        // クライアントが出力する正規Base64URL形式だけを復号します。
        // PUT本文のBase64URL文字列
        const auto encoded = body["content"].get<std::string>();
        // Base64URLの6ビット変換
        // character: 変換するBase64URL文字
        auto value = [](const unsigned char character) -> int
        {
            // 大文字は先頭の26値へ割り当てます。
            if (character >= 'A' && character <= 'Z') return character - 'A';
            // 小文字は次の26値へ割り当てます。
            if (character >= 'a' && character <= 'z') return character - 'a' + 26;
            // 数字は次の10値へ割り当てます。
            if (character >= '0' && character <= '9') return character - '0' + 52;
            // URL安全文字のハイフンを62へ割り当てます。
            if (character == '-') return 62;
            // URL安全文字のアンダースコアを63へ割り当てます。
            if (character == '_') return 63;
            return -1;
        };
        // 復号した保存本文
        std::vector<std::uint8_t> content;
        // 受け取った6ビット値の連結領域
        std::uint32_t accumulator{};
        // 連結領域に残る未消費ビット数
        int bits{};
        // 文字を6ビットずつ復号します。
        for (const unsigned char character : encoded)
        {
            accumulator = (accumulator << 6u)
                | static_cast<std::uint32_t>(value(character));
            bits += 6;
            // 復号したビットが1バイト分に達したら出力します。
            if (bits >= 8)
            {
                bits -= 8;
                content.push_back(static_cast<std::uint8_t>(
                    accumulator >> bits));
                accumulator &= bits == 0
                    ? 0u
                    : (1u << bits) - 1u;
            }
        }
        return {
            std::move(resource),
            std::move(etag),
            false,
            std::move(content),
            body["sha256"].get<std::string>()
        };
    }

    // 保存済み内容を持つ生存スナップショットを作ります。
    // LiveSnapshot(resource: 保存対象, etag: 世代タグ, content: 保存内容)
    LamaPon::CloudSaveSnapshot LiveSnapshot(
        LamaPon::CloudSaveResource resource,
        std::string etag,
        std::vector<std::uint8_t> content)
    {
        // 生存内容に対応するSHA-256
        const auto hash = Hash(content);
        return {
            std::move(resource),
            std::move(etag),
            false,
            std::move(content),
            hash
        };
    }

    // 削除済みを表す墓標スナップショットを作ります。
    // Tombstone(resource: 保存対象, etag: 削除世代のタグ)
    LamaPon::CloudSaveSnapshot Tombstone(
        LamaPon::CloudSaveResource resource,
        std::string etag)
    {
        return {
            std::move(resource),
            std::move(etag),
            true,
            {},
            {}
        };
    }

    // 条件成立まで同期器を駆動し、仮想時刻を進めます。
    // PumpUntil(synchronizer: 対象, predicate: 完了条件, now: 仮想時刻)
    template<class Predicate>
    void PumpUntil(
        Synchronizer& synchronizer,
        Predicate predicate,
        std::uint64_t& now)
    {
        // 最大5000回の駆動試行
        for (std::size_t attempt = 0; attempt < 5000u; ++attempt)
        {
            // 現在時刻で同期処理を一度進めます。
            synchronizer.Tick(now);
            now += 10u;
            // 非同期処理が完了したか確認します。
            if (predicate())
            {
                return;
            }
            Sleep(1u);
        }
        throw std::runtime_error("Synchronizer test timed out.");
    }

    // 指定した共有番号から連続する変異IDを生成します。
    // IdGenerator(index: 次に使う変異IDの共有位置)
    LamaPon::Detail::CloudSaveMutationIdGenerator IdGenerator(
        std::shared_ptr<std::size_t> index)
    {
        return [index]
        {
            // IDを使い切った場合はテストを止めます。
            Require(*index < MutationIds.size(), "Mutation IDs exhausted.");
            return std::string(MutationIds[(*index)++]);
        };
    }

    // 切り離し後に進行中の要求が終わるまで同期器を進めます。
    // DrainDetached(synchronizer: 切り離す同期器, now: 進行中の仮想時刻)
    void DrainDetached(Synchronizer& synchronizer, std::uint64_t& now)
    {
        // 新たな同期処理を止めます。
        synchronizer.Detach();
        PumpUntil(
            synchronizer,
            [&synchronizer]
            {
                return !synchronizer.HasInFlightRequest();
            },
            now);
    }

    // 応答中に加えた更新を順番に同期します。
    void TestPendingOverlayChainsAfterAck()
    {
        // 信頼済み保存先
        const auto trusted = TrustedPath("overlay");
        // 信頼済み永続化先の設定
        const auto profile = Profile(trusted);
        // ローカル設定ストア
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        // ローカルセーブデータストア
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        // クラウド同期ジャーナル
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");

        preferences.SetInteger("progress", 1);
        preferences.Save();
        // 先行する設定データのスナップショット
        const auto p1 = Documents::ReadPlayerPrefs(preferences).bytes;

        // 要求と応答を記録するテスト用バックエンド
        ScriptedBackend backend;
        backend.Push([](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({});
        });

        struct Gate final
        {
            // 共有状態を保護する排他制御
            std::mutex mutex;
            // スレッド間の通知待ち条件
            std::condition_variable condition;
            // 処理が待機箇所へ到達した状態
            bool entered{};
            // 待機中のワーカーを解放する状態
            bool release{};
        };
        // ワーカーを待機させる共有ゲート
        auto gate = std::make_shared<Gate>();
        // request: バックエンドが受信したHTTP要求
        backend.Push([gate](const LamaPon::HttpRequest& request)
        {
            {
                // 共有状態を保護するスコープロック
                std::unique_lock lock(gate->mutex);
                gate->entered = true;
                gate->condition.notify_all();
                gate->condition.wait(lock, [&] { return gate->release; });
            }
            // クラウド保存スナップショット
            const auto snapshot = SnapshotFromPut(request, "\"p1\"");
            return SnapshotResponse(201u, snapshot);
        });
        // 後続更新のクラウド応答
        auto p2Remote = std::make_shared<
            std::optional<LamaPon::CloudSaveSnapshot>>();
        // 後続応答を保護する排他制御
        auto p2RemoteMutex = std::make_shared<std::mutex>();
        // request: バックエンドが受信したHTTP要求
        backend.Push([p2Remote, p2RemoteMutex](
            const LamaPon::HttpRequest& request)
        {
            // クラウド保存スナップショット
            const auto snapshot = SnapshotFromPut(request, "\"p2\"");
            {
                // 共有状態を保護するスコープロック
                std::scoped_lock lock(*p2RemoteMutex);
                *p2Remote = snapshot;
            }
            return SnapshotResponse(200u, snapshot);
        });
        backend.Push([p2Remote, p2RemoteMutex](const LamaPon::HttpRequest&)
        {
            // 共有状態を保護するスコープロック
            std::scoped_lock lock(*p2RemoteMutex);
            Require(p2Remote->has_value(), "P2 response was not recorded.");
            return ManifestResponse({ **p2Remote });
        });

        // 次に使う変異IDの共有位置
        auto idIndex = std::make_shared<std::size_t>(0u);
        // テスト対象のクラウド同期器
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(journal, 7u, std::string(AccountKey), std::string(Token));
        // 同期処理で進める仮想時刻
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                // 共有状態を保護するスコープロック
                std::scoped_lock lock(gate->mutex);
                return gate->entered;
            },
            now);

        preferences.SetInteger("progress", 2);
        preferences.Save();
        // 後続する設定データのスナップショット
        const auto p2 = Documents::ReadPlayerPrefs(preferences).bytes;
        Require(p1 != p2, "P2 fixture did not change local bytes.");
        {
            // 共有状態を保護するスコープロック
            std::scoped_lock lock(gate->mutex);
            gate->release = true;
        }
        gate->condition.notify_all();

        PumpUntil(
            synchronizer,
            [&]
            {
                // 同期開始時のクラウド基準値
                const auto baseline = journal.Baseline(
                    LamaPon::CloudSaveResource::Preferences());
                return baseline
                    && baseline->etag == "\"p2\""
                    && baseline->content == p2
                    && !journal.Pending(
                        LamaPon::CloudSaveResource::Preferences())
                    && !synchronizer.HasInFlightRequest();
            },
            now);

        // 送受信したHTTP要求の履歴
        const auto requests = backend.Requests();
        Require(requests.size() == 4u, "P1/P2 made an unexpected wire request.");
        Require(
            requests[0].method == L"GET"
                && requests[1].method == L"PUT"
                && requests[2].method == L"PUT"
                && requests[3].method == L"GET",
            "P1/P2 request order was not manifest, P1, P2.");
        Require(
            RequestHeader(requests[1], L"If-None-Match") == L"*"
                && RequestHeader(requests[2], L"If-Match") == L"\"p1\""
                && RequestHeader(requests[1], L"Idempotency-Key")
                    != RequestHeader(requests[2], L"Idempotency-Key"),
            "P2 did not use a fresh UUID and the P1 ETag.");
        // HTTP要求を順に検査
        for (const auto& request : requests)
        {
            Require(
                RequestText(request).find(AccountKey) == std::string::npos
                    && request.url.find(std::wstring(
                        AccountKey.begin(),
                        AccountKey.end())) == std::wstring::npos,
                "Account storage key escaped into the cloud request.");
        }
        DrainDetached(synchronizer, now);
    }

    // 再試行時も変異IDを維持することを検証します。
    void TestPendingRetryKeepsMutationIdentity()
    {
        // 信頼済み保存先
        const auto trusted = TrustedPath("pending-retry");
        // 信頼済み永続化先の設定
        const auto profile = Profile(trusted);
        // ローカル設定ストア
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        // ローカルセーブデータストア
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        // クラウド同期ジャーナル
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        preferences.SetInteger("retry", 1);
        preferences.Save();

        // 要求と応答を記録するテスト用バックエンド
        ScriptedBackend backend;
        backend.Push([](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({});
        });
        backend.Push([](const LamaPon::HttpRequest&)
        {
            // テストバックエンドのHTTP応答
            LamaPon::HttpResponse response;
            response.statusCode = 503u;
            return response;
        });
        // 成功した更新スナップショット
        auto succeeded = std::make_shared<
            std::optional<LamaPon::CloudSaveSnapshot>>();
        // request: バックエンドが受信したHTTP要求
        backend.Push([succeeded](const LamaPon::HttpRequest& request)
        {
            *succeeded = SnapshotFromPut(request, "\"retry-ok\"");
            return SnapshotResponse(201u, **succeeded);
        });
        backend.Push([succeeded](const LamaPon::HttpRequest&)
        {
            Require(succeeded->has_value(), "Retry snapshot missing.");
            return ManifestResponse({ **succeeded });
        });

        // 次に使う変異IDの共有位置
        auto idIndex = std::make_shared<std::size_t>(0u);
        // テスト対象のクラウド同期器
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(journal, 9u, std::string(AccountKey), std::string(Token));
        // 同期処理で進める仮想時刻
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                return synchronizer.Status().state
                    == LamaPon::Detail::CloudSaveSynchronizerState::BackingOff;
            },
            now);
        // 再試行可能になる時刻
        const auto retryAt = synchronizer.Status().retryAtMilliseconds;
        synchronizer.Tick(retryAt - 1u);
        Require(
            backend.Requests().size() == 2u,
            "Pending mutation retried before backoff expired.");
        now = retryAt;
        PumpUntil(
            synchronizer,
            [&]
            {
                return synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Idle
                    && !synchronizer.HasInFlightRequest();
            },
            now);
        // 送受信したHTTP要求の履歴
        const auto requests = backend.Requests();
        Require(
            requests.size() == 4u
                && RequestHeader(requests[1], L"Idempotency-Key")
                    == RequestHeader(requests[2], L"Idempotency-Key")
                && requests[1].body == requests[2].body,
            "Pending retry changed mutation ID or content.");
        DrainDetached(synchronizer, now);
    }

    // 削除後のローカル再作成をPUTで同期します。
    void TestRemoteDeleteThenLocalRecreationBecomesPut()
    {
        // 信頼済み保存先
        const auto trusted = TrustedPath("delete-recreate");
        // 信頼済み永続化先の設定
        const auto profile = Profile(trusted);
        // ローカル設定ストア
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        // ローカルセーブデータストア
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        // クラウド同期ジャーナル
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");

        preferences.SetInteger("progress", 10);
        preferences.Save();
        // 更新前の保存文書
        const auto oldDocument = Documents::ReadPlayerPrefs(preferences).bytes;
        journal.RecordBaseline(LiveSnapshot(
            LamaPon::CloudSaveResource::Preferences(),
            "\"old\"",
            oldDocument));
        // 削除済み状態を表す値
        const auto deleted = Tombstone(
            LamaPon::CloudSaveResource::Preferences(),
            "\"deleted\"");

        // 要求と応答を記録するテスト用バックエンド
        ScriptedBackend backend;
        backend.Push([deleted](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ deleted });
        });
        // 同期先を設定したクラウドクライアント
        auto client = MakeClient(backend);
        // 次に使う変異IDの共有位置
        auto idIndex = std::make_shared<std::size_t>(0u);
        // テスト対象のクラウド同期器
        Synchronizer synchronizer(
            preferences,
            saves,
            client,
            IdGenerator(idIndex));
        synchronizer.Attach(journal, 11u, std::string(AccountKey), std::string(Token));
        // 同期処理で進める仮想時刻
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                // 同期開始時のクラウド基準値
                const auto baseline = journal.Baseline(
                    LamaPon::CloudSaveResource::Preferences());
                return baseline && baseline->deleted
                    && Documents::ReadPlayerPrefs(preferences).state
                        == LamaPon::Detail::LocalPersistenceDocumentState::Missing
                    && !synchronizer.HasInFlightRequest();
            },
            now);

        preferences.SetInteger("progress", 20);
        preferences.Save();
        // 再作成後のローカル保存内容
        const auto recreated = Documents::ReadPlayerPrefs(preferences).bytes;

        backend.Push([deleted](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ deleted });
        });
        // 再作成後のクラウド状態
        auto remoteRecreated = std::make_shared<
            std::optional<LamaPon::CloudSaveSnapshot>>();
        // request: バックエンドが受信したHTTP要求
        backend.Push([remoteRecreated](const LamaPon::HttpRequest& request)
        {
            *remoteRecreated = SnapshotFromPut(request, "\"recreated\"");
            return SnapshotResponse(200u, **remoteRecreated);
        });
        backend.Push([remoteRecreated](const LamaPon::HttpRequest&)
        {
            Require(remoteRecreated->has_value(), "Recreated snapshot missing.");
            return ManifestResponse({ **remoteRecreated });
        });
        synchronizer.RequestReconcile();

        PumpUntil(
            synchronizer,
            [&]
            {
                // 同期開始時のクラウド基準値
                const auto baseline = journal.Baseline(
                    LamaPon::CloudSaveResource::Preferences());
                return baseline
                    && baseline->etag == "\"recreated\""
                    && baseline->content == recreated
                    && synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Idle
                    && !synchronizer.HasInFlightRequest();
            },
            now);

        // 送受信したHTTP要求の履歴
        const auto requests = backend.Requests();
        Require(requests.size() == 4u, "Delete/recreate request count changed.");
        Require(
            requests[2].method == L"PUT"
                && RequestHeader(requests[2], L"If-Match")
                    == L"\"deleted\"",
            "Local recreation after tombstone was not a CAS Put.");
        DrainDetached(synchronizer, now);
    }

    // 既存の枠名表記を保って削除を反映します。
    void TestRemoteSlotCaseUsesExistingLocalSpelling()
    {
        // 信頼済み保存先
        const auto trusted = TrustedPath("slot-case-identity");
        // 信頼済み永続化先の設定
        const auto profile = Profile(trusted);
        // ローカル設定ストア
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        // ローカルセーブデータストア
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        // クラウド同期ジャーナル
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");

        saves.SaveJson("case-slot", R"({"value":1})");
        // 現在のローカル保存内容
        const auto local = Documents::ReadSaveData(saves, "case-slot");
        Require(
            local.state
                == LamaPon::Detail::LocalPersistenceDocumentState::Loaded,
            "Case fixture local slot was not readable.");
        journal.RecordBaseline(LiveSnapshot(
            LamaPon::CloudSaveResource::SaveSlot("case-slot"),
            "\"case-base\"",
            local.bytes));

        // クラウド上の保存状態
        const auto remote = LiveSnapshot(
            LamaPon::CloudSaveResource::SaveSlot("Case-Slot"),
            "\"case-remote\"",
            Bytes(R"({"format":"LamaPonSaveData","version":1,"slot":"Case-Slot","data":{"value":2}})"));
        // 要求と応答を記録するテスト用バックエンド
        ScriptedBackend backend;
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        // request: バックエンドが受信したHTTP要求
        backend.Push([remote](const LamaPon::HttpRequest& request)
        {
            Require(
                request.method == L"POST",
                "Case-different remote slot was not read.");
            return SnapshotResponse(200u, remote);
        });
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });

        // 次に使う変異IDの共有位置
        auto idIndex = std::make_shared<std::size_t>(0u);
        // テスト対象のクラウド同期器
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(
            journal,
            12u,
            std::string(AccountKey),
            std::string(Token));
        // 同期処理で進める仮想時刻
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                // 同期開始時のクラウド基準値
                const auto baseline = journal.Baseline(
                    LamaPon::CloudSaveResource::SaveSlot("case-slot"));
                return baseline
                    && baseline->etag == "\"case-remote\""
                    && synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Idle
                    && !synchronizer.HasInFlightRequest();
            },
            now);

        // 読み込んだローカルセーブ内容
        const auto payload = saves.LoadJson("case-slot");
        // 対象となるセーブ枠一覧
        const auto slots = saves.ListSlots();
        Require(
            payload
                && Json::parse(*payload).at("value") == 2
                && slots.size() == 1u
                && slots.front() == "case-slot"
                && std::filesystem::is_regular_file(
                    saves.SlotPath("case-slot")),
            "Remote case change created an alias or broke public LoadJson.");
        DrainDetached(synchronizer, now);
    }

    // 世代競合後に最新のクラウド状態を採用します。
    void TestCasConflictRetryAndUseRemote()
    {
        // 信頼済み保存先
        const auto trusted = TrustedPath("conflict");
        // 信頼済み永続化先の設定
        const auto profile = Profile(trusted);
        // ローカル設定ストア
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        // ローカルセーブデータストア
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        // クラウド同期ジャーナル
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");

        preferences.SetInteger("progress", 1);
        preferences.Save();
        // 初期状態を表す保存文書
        const auto baseDocument = Documents::ReadPlayerPrefs(preferences).bytes;
        journal.RecordBaseline(LiveSnapshot(
            LamaPon::CloudSaveResource::Preferences(),
            "\"base\"",
            baseDocument));
        preferences.SetInteger("progress", 2);
        preferences.Save();

        // クラウドから取得した保存文書
        const auto remoteDocument = Bytes(
            R"({"format":"LamaPonPlayerPrefs","version":1,"values":{"remote":{"type":"integer","value":99}}})");
        // クラウド上の保存状態
        const auto remote = LiveSnapshot(
            LamaPon::CloudSaveResource::Preferences(),
            "\"remote\"",
            remoteDocument);

        // 要求と応答を記録するテスト用バックエンド
        ScriptedBackend backend;
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return SnapshotResponse(200u, remote);
        });
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ConflictResponse(remote);
        });
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });

        // 次に使う変異IDの共有位置
        auto idIndex = std::make_shared<std::size_t>(0u);
        // テスト対象のクラウド同期器
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(journal, 13u, std::string(AccountKey), std::string(Token));
        // 同期処理で進める仮想時刻
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                return synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Conflict
                    && journal.Conflict(
                        LamaPon::CloudSaveResource::Preferences()).has_value()
                    && !synchronizer.HasInFlightRequest();
            },
            now);

        // 初回更新の保留記録
        const auto firstPending = journal.Pending(
            LamaPon::CloudSaveResource::Preferences());
        Require(firstPending.has_value(), "Conflict lost its pending mutation.");
        synchronizer.ResolveConflict(
            LamaPon::CloudSaveResource::Preferences(),
            firstPending->mutationId,
            LamaPon::Detail::CloudSaveConflictResolution::RetryLocal);
        // 再試行対象の更新記録
        const auto retryPending = journal.Pending(
            LamaPon::CloudSaveResource::Preferences());
        Require(
            retryPending && retryPending->mutationId != firstPending->mutationId,
            "RetryLocal reused the stale mutation ID.");

        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ConflictResponse(remote);
        });
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        PumpUntil(
            synchronizer,
            [&]
            {
                // 未送信の同期記録
                const auto pending = journal.Pending(
                    LamaPon::CloudSaveResource::Preferences());
                return pending && journal.Conflict(
                        LamaPon::CloudSaveResource::Preferences())
                    && synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Conflict
                    && !synchronizer.HasInFlightRequest();
            },
            now);

        // 最終更新の保留記録
        const auto finalPending = journal.Pending(
            LamaPon::CloudSaveResource::Preferences());
        synchronizer.ResolveConflict(
            LamaPon::CloudSaveResource::Preferences(),
            finalPending->mutationId,
            LamaPon::Detail::CloudSaveConflictResolution::UseRemote);
        Require(
            Documents::ReadPlayerPrefs(preferences).bytes == remoteDocument
                && !journal.Pending(
                    LamaPon::CloudSaveResource::Preferences())
                && !journal.Conflict(
                    LamaPon::CloudSaveResource::Preferences())
                && journal.Baseline(
                    LamaPon::CloudSaveResource::Preferences())->etag
                    == "\"remote\"",
            "UseRemote did not apply local document before resolving journal.");
        DrainDetached(synchronizer, now);
    }

    // 初回同期で双方の内容が競合することを検証します。
    void TestInitialDualContentRequiresConflict()
    {
        // 信頼済み保存先
        const auto trusted = TrustedPath("initial-conflict");
        // 信頼済み永続化先の設定
        const auto profile = Profile(trusted);
        // ローカル設定ストア
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        // ローカルセーブデータストア
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        // クラウド同期ジャーナル
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        preferences.SetInteger("local", 1);
        preferences.Save();
        // 現在のローカル保存内容
        const auto local = Documents::ReadPlayerPrefs(preferences).bytes;
        // クラウド上の保存状態
        const auto remote = LiveSnapshot(
            LamaPon::CloudSaveResource::Preferences(),
            "\"remote-first\"",
            Bytes(R"({"format":"LamaPonPlayerPrefs","version":1,"values":{"remote":{"type":"integer","value":2}}})"));

        // 要求と応答を記録するテスト用バックエンド
        ScriptedBackend backend;
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return SnapshotResponse(200u, remote);
        });
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        // 次に使う変異IDの共有位置
        auto idIndex = std::make_shared<std::size_t>(0u);
        // テスト対象のクラウド同期器
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(journal, 17u, std::string(AccountKey), std::string(Token));
        // 同期処理で進める仮想時刻
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                return synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Conflict
                    && !synchronizer.HasInFlightRequest();
            },
            now);
        // 未送信の同期記録
        const auto pending = journal.Pending(
            LamaPon::CloudSaveResource::Preferences());
        Require(
            pending && pending->content == local
                && journal.Conflict(
                    LamaPon::CloudSaveResource::Preferences())->content
                    == remote.content,
            "Initial local/remote content was not retained as a conflict.");
        // 送受信したHTTP要求の履歴
        const auto requests = backend.Requests();
        Require(
            requests.size() == 3u
                && requests[0].method == L"GET"
                && requests[1].method == L"POST"
                && requests[2].method == L"GET",
            "Initial conflict uploaded or skipped full remote validation.");
        DrainDetached(synchronizer, now);
    }

    // 読込中に確定したローカル更新を優先します。
    void TestLocalCommitInvalidatesActiveRead()
    {
        // 信頼済み保存先
        const auto trusted = TrustedPath("read-invalidation");
        // 信頼済み永続化先の設定
        const auto profile = Profile(trusted);
        // ローカル設定ストア
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        // ローカルセーブデータストア
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        // クラウド同期ジャーナル
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        // クラウド上の保存状態
        const auto remote = LiveSnapshot(
            LamaPon::CloudSaveResource::Preferences(),
            "\"remote-read\"",
            Bytes(R"({"format":"LamaPonPlayerPrefs","version":1,"values":{"remote":{"type":"integer","value":8}}})"));
        struct Gate final
        {
            // 共有状態を保護する排他制御
            std::mutex mutex;
            // スレッド間の通知待ち条件
            std::condition_variable condition;
            // 処理が待機箇所へ到達した状態
            bool entered{};
            // 待機中のワーカーを解放する状態
            bool release{};
        };
        // ワーカーを待機させる共有ゲート
        auto gate = std::make_shared<Gate>();
        // 要求と応答を記録するテスト用バックエンド
        ScriptedBackend backend;
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        backend.Push([gate, remote](const LamaPon::HttpRequest&)
        {
            // 共有状態を保護するスコープロック
            std::unique_lock lock(gate->mutex);
            gate->entered = true;
            gate->condition.notify_all();
            gate->condition.wait(lock, [&] { return gate->release; });
            return SnapshotResponse(200u, remote);
        });
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return SnapshotResponse(200u, remote);
        });
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });

        // 次に使う変異IDの共有位置
        auto idIndex = std::make_shared<std::size_t>(0u);
        // テスト対象のクラウド同期器
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(journal, 18u, std::string(AccountKey), std::string(Token));
        // 同期処理で進める仮想時刻
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                // 共有状態を保護するスコープロック
                std::scoped_lock lock(gate->mutex);
                return gate->entered;
            },
            now);

        preferences.SetInteger("created-during-read", 44);
        preferences.Save();
        // 現在のローカル保存内容
        const auto local = Documents::ReadPlayerPrefs(preferences).bytes;
        synchronizer.RequestReconcile();
        {
            // 共有状態を保護するスコープロック
            std::scoped_lock lock(gate->mutex);
            gate->release = true;
        }
        gate->condition.notify_all();
        PumpUntil(
            synchronizer,
            [&]
            {
                return synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Conflict
                    && !synchronizer.HasInFlightRequest();
            },
            now);
        Require(
            Documents::ReadPlayerPrefs(preferences).bytes == local
                && journal.Pending(
                    LamaPon::CloudSaveResource::Preferences())->content == local
                && journal.Conflict(
                    LamaPon::CloudSaveResource::Preferences())->content
                    == remote.content,
            "An active Read overwrote a newer local commit.");
        // 送受信したHTTP要求の履歴
        const auto requests = backend.Requests();
        Require(
            requests.size() == 5u
                && requests[0].method == L"GET"
                && requests[1].method == L"POST"
                && requests[2].method == L"GET"
                && requests[3].method == L"POST"
                && requests[4].method == L"GET",
            "Invalidated Read was not discarded and reconciled afresh.");
        DrainDetached(synchronizer, now);
    }

    // 初回読込中の削除意図を保持します。
    void TestDeleteDuringInitialReadIsNotInitialMissing()
    {
        // 信頼済み保存先
        const auto trusted = TrustedPath("delete-during-read");
        // 信頼済み永続化先の設定
        const auto profile = Profile(trusted);
        // ローカル設定ストア
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        // ローカルセーブデータストア
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        // クラウド同期ジャーナル
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        preferences.SetInteger("delete-me", 1);
        preferences.Save();
        // クラウド上の保存状態
        const auto remote = LiveSnapshot(
            LamaPon::CloudSaveResource::Preferences(),
            "\"remote-before-delete\"",
            Bytes(R"({"format":"LamaPonPlayerPrefs","version":1,"values":{"remote":{"type":"integer","value":7}}})"));
        // 削除済み状態を表す値
        const auto deleted = Tombstone(
            LamaPon::CloudSaveResource::Preferences(),
            "\"remote-deleted\"");
        struct Gate final
        {
            // 共有状態を保護する排他制御
            std::mutex mutex;
            // スレッド間の通知待ち条件
            std::condition_variable condition;
            // 処理が待機箇所へ到達した状態
            bool entered{};
            // 待機中のワーカーを解放する状態
            bool release{};
        };
        // ワーカーを待機させる共有ゲート
        auto gate = std::make_shared<Gate>();
        // 要求と応答を記録するテスト用バックエンド
        ScriptedBackend backend;
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        backend.Push([gate, remote](const LamaPon::HttpRequest&)
        {
            // 共有状態を保護するスコープロック
            std::unique_lock lock(gate->mutex);
            gate->entered = true;
            gate->condition.notify_all();
            gate->condition.wait(lock, [&] { return gate->release; });
            return SnapshotResponse(200u, remote);
        });
        backend.Push([deleted](const LamaPon::HttpRequest&)
        {
            return SnapshotResponse(200u, deleted);
        });
        backend.Push([deleted](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ deleted });
        });

        // 次に使う変異IDの共有位置
        auto idIndex = std::make_shared<std::size_t>(0u);
        // テスト対象のクラウド同期器
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(journal, 20u, std::string(AccountKey), std::string(Token));
        // 同期処理で進める仮想時刻
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                // 共有状態を保護するスコープロック
                std::scoped_lock lock(gate->mutex);
                return gate->entered;
            },
            now);
        Documents::DeletePlayerPrefs(preferences);
        synchronizer.RequestReconcile();
        // 永続化された削除意図
        const auto durableDelete = journal.Pending(
            LamaPon::CloudSaveResource::Preferences());
        Require(
            durableDelete
                && durableDelete->kind
                    == LamaPon::Detail::CloudSavePendingKind::Delete
                && durableDelete->baseEtag == "\"remote-before-delete\"",
            "Delete during Read was not write-ahead journaled immediately.");
        {
            // 共有状態を保護するスコープロック
            std::scoped_lock lock(gate->mutex);
            gate->release = true;
        }
        gate->condition.notify_all();
        PumpUntil(
            synchronizer,
            [&]
            {
                // 同期開始時のクラウド基準値
                const auto baseline = journal.Baseline(
                    LamaPon::CloudSaveResource::Preferences());
                return baseline && baseline->deleted
                    && synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Idle
                    && !synchronizer.HasInFlightRequest();
            },
            now);
        // 送受信したHTTP要求の履歴
        const auto requests = backend.Requests();
        Require(
            requests.size() == 4u
                && requests[0].method == L"GET"
                && requests[1].method == L"POST"
                && requests[2].method == L"DELETE"
                && requests[3].method == L"GET"
                && RequestHeader(requests[2], L"If-Match")
                    == L"\"remote-before-delete\"",
            "Delete during initial Read was mistaken for initial Missing.");
        DrainDetached(synchronizer, now);
    }

    // ローカル破損時に通信を開始しないことを検証します。
    void TestCorruptLocalStopsBeforeNetwork()
    {
        // 信頼済み保存先
        const auto trusted = TrustedPath("corrupt-local");
        // 信頼済み永続化先の設定
        const auto profile = Profile(trusted);
        // ローカル設定ストア
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        // ローカルセーブデータストア
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        // クラウド同期ジャーナル
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        preferences.SetInteger("valid", 1);
        preferences.Save();
        {
            // 生成する出力先
            std::ofstream output(
                profile.playerPrefsFile,
                std::ios::binary | std::ios::trunc);
            Require(output.good(), "Corrupt fixture open failed.");
            output << "{not-json";
        }

        // 要求と応答を記録するテスト用バックエンド
        ScriptedBackend backend;
        // 次に使う変異IDの共有位置
        auto idIndex = std::make_shared<std::size_t>(0u);
        // テスト対象のクラウド同期器
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        // 拒否された同期操作の結果
        bool rejected{};
        // 不正な永続化先の添付が拒否されることを確認します。
        try
        {
            synchronizer.Attach(
                journal,
                19u,
                std::string(AccountKey),
                std::string(Token));
        }
        // 想定した拒否例外をテスト結果に記録します。
        catch (const std::exception&)
        {
            rejected = true;
        }
        Require(
            rejected
                && !synchronizer.IsAttached()
                && backend.Requests().empty()
                && journal.Generation() == 0u,
            "Corrupt local persistence passed prepared attachment.");
        // 同期処理で進める仮想時刻
        std::uint64_t now{};
        Documents::DeletePlayerPrefs(preferences);
        backend.Push([](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({});
        });
        synchronizer.Attach(
            journal,
            20u,
            std::string(AccountKey),
            std::string(Token));
        PumpUntil(
            synchronizer,
            [&]
            {
                return synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Idle
                    && !synchronizer.HasInFlightRequest();
            },
            now);
        Require(
            backend.Requests().size() == 1u,
            "Explicit retry did not recover a repaired local document.");
        DrainDetached(synchronizer, now);
    }

    // 認証更新と制限応答後の待機を検証します。
    void TestUnauthorizedRefreshAndRateLimitBackoff()
    {
        {
            // 信頼済み保存先
            const auto trusted = TrustedPath("unauthorized");
            // 信頼済み永続化先の設定
            const auto profile = Profile(trusted);
            // ローカル設定ストア
            LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
            // ローカルセーブデータストア
            LamaPon::SaveDataStore saves(profile.saveDataDirectory);
            // クラウド同期ジャーナル
            Journal journal(
                trusted,
                profile,
                "sync-game",
                "staging",
                "https://online.example.test/tenant/");
            // 要求と応答を記録するテスト用バックエンド
            ScriptedBackend backend;
            backend.Push([](const LamaPon::HttpRequest&)
            {
                // テストバックエンドのHTTP応答
                LamaPon::HttpResponse response;
                response.statusCode = 401u;
                return response;
            });
            backend.Push([](const LamaPon::HttpRequest&)
            {
                return ManifestResponse({});
            });
            // 次に使う変異IDの共有位置
            auto idIndex = std::make_shared<std::size_t>(0u);
            // テスト対象のクラウド同期器
            Synchronizer synchronizer(
                preferences,
                saves,
                MakeClient(backend),
                IdGenerator(idIndex));
            synchronizer.Attach(
                journal,
                23u,
                std::string(AccountKey),
                std::string(Token));
            // 同期処理で進める仮想時刻
            std::uint64_t now{};
            PumpUntil(
                synchronizer,
                [&]
                {
                    return synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Unauthorized;
                },
                now);
            synchronizer.Tick(now + 100000u);
            Require(
                backend.Requests().size() == 1u,
                "Unauthorized response was retried automatically.");
            synchronizer.UpdateAccessToken("refreshed-backend-token");
            PumpUntil(
                synchronizer,
                [&]
                {
                    return synchronizer.Status().state
                            == LamaPon::Detail::CloudSaveSynchronizerState::Idle
                        && !synchronizer.HasInFlightRequest();
                },
                now);
            Require(
                backend.Requests().size() == 2u,
                "Token refresh did not resume synchronization exactly once.");
            DrainDetached(synchronizer, now);
        }

        {
            // 信頼済み保存先
            const auto trusted = TrustedPath("rate-limit");
            // 信頼済み永続化先の設定
            const auto profile = Profile(trusted);
            // ローカル設定ストア
            LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
            // ローカルセーブデータストア
            LamaPon::SaveDataStore saves(profile.saveDataDirectory);
            // クラウド同期ジャーナル
            Journal journal(
                trusted,
                profile,
                "sync-game",
                "staging",
                "https://online.example.test/tenant/");
            // 要求と応答を記録するテスト用バックエンド
            ScriptedBackend backend;
            backend.Push([](const LamaPon::HttpRequest&)
            {
                // テストバックエンドのHTTP応答
                LamaPon::HttpResponse response;
                response.statusCode = 429u;
                response.headers.emplace_back(L"Retry-After", L"2");
                return response;
            });
            backend.Push([](const LamaPon::HttpRequest&)
            {
                return ManifestResponse({});
            });
            // 次に使う変異IDの共有位置
            auto idIndex = std::make_shared<std::size_t>(0u);
            // テスト対象のクラウド同期器
            Synchronizer synchronizer(
                preferences,
                saves,
                MakeClient(backend),
                IdGenerator(idIndex));
            synchronizer.Attach(
                journal,
                29u,
                std::string(AccountKey),
                std::string(Token));
            // 同期処理で進める仮想時刻
            std::uint64_t now{};
            PumpUntil(
                synchronizer,
                [&]
                {
                    return synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::BackingOff;
                },
                now);
            // 再試行可能になる時刻
            const auto retryAt = synchronizer.Status().retryAtMilliseconds;
            synchronizer.Tick(retryAt - 1u);
            Require(
                backend.Requests().size() == 1u,
                "Rate-limited request ignored its retry deadline.");
            now = retryAt;
            PumpUntil(
                synchronizer,
                [&]
                {
                    return synchronizer.Status().state
                            == LamaPon::Detail::CloudSaveSynchronizerState::Idle
                        && !synchronizer.HasInFlightRequest();
                },
                now);
            Require(
                backend.Requests().size() == 2u,
                "Rate-limited request did not resume once.");
            DrainDetached(synchronizer, now);
        }
    }

    // ジャーナル競合後も読込を再試行します。
    void TestJournalBusyDuringActiveReadRetriesWithoutHalting()
    {
        // 信頼済み保存先
        const auto trusted = TrustedPath("journal-busy-read");
        // 信頼済み永続化先の設定
        const auto profile = Profile(trusted);
        // ローカル設定ストア
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        preferences.SetInteger("local", 1);
        preferences.Save();
        // ローカルセーブデータストア
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        // クラウド同期ジャーナル
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        // クラウド上の保存状態
        const auto remote = LiveSnapshot(
            LamaPon::CloudSaveResource::Preferences(),
            "\"busy-remote\"",
            Bytes(R"({"format":"LamaPonPlayerPrefs","version":1,"values":{"remote":{"type":"integer","value":2}}})"));
        // 削除済み状態を表す値
        const auto deleted = Tombstone(
            LamaPon::CloudSaveResource::Preferences(),
            "\"busy-deleted\"");
        struct Gate final
        {
            // 共有状態を保護する排他制御
            std::mutex mutex;
            // スレッド間の通知待ち条件
            std::condition_variable condition;
            // 処理が待機箇所へ到達した状態
            bool entered{};
            // 待機中のワーカーを解放する状態
            bool release{};
        };
        // ワーカーを待機させる共有ゲート
        auto gate = std::make_shared<Gate>();
        // 要求と応答を記録するテスト用バックエンド
        ScriptedBackend backend;
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        backend.Push([gate, remote](const LamaPon::HttpRequest&)
        {
            // 共有状態を保護するスコープロック
            std::unique_lock lock(gate->mutex);
            gate->entered = true;
            gate->condition.notify_all();
            gate->condition.wait(lock, [&] { return gate->release; });
            return SnapshotResponse(200u, remote);
        });
        backend.Push([deleted](const LamaPon::HttpRequest&)
        {
            return SnapshotResponse(200u, deleted);
        });
        backend.Push([deleted](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ deleted });
        });

        // 次に使う変異IDの共有位置
        auto idIndex = std::make_shared<std::size_t>(0u);
        // テスト対象のクラウド同期器
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(
            journal,
            31u,
            std::string(AccountKey),
            std::string(Token));
        // 同期処理で進める仮想時刻
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                // 共有状態を保護するスコープロック
                std::scoped_lock lock(gate->mutex);
                return gate->entered;
            },
            now);

        Documents::DeletePlayerPrefs(preferences);
        // ジャーナルのロックファイル位置
        const auto lockPath =
            journal.FilePath().parent_path() / L"CloudSaveJournal.lock";
        // ジャーナルロックの占有ハンドル
        const auto held = CreateFileW(
            lockPath.c_str(),
            GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES,
            0u,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        Require(held != INVALID_HANDLE_VALUE, "Journal busy fixture failed.");
        std::cout << "[cloud-sync] busy acquired" << std::endl;
        synchronizer.RequestReconcile();
        std::cout << "[cloud-sync] busy request returned" << std::endl;
        // 再試行待ちへ入った状態
        const bool enteredBackoff = synchronizer.Status().state
            == LamaPon::Detail::CloudSaveSynchronizerState::BackingOff;
        CloseHandle(held);
        std::cout << "[cloud-sync] busy released" << std::endl;
        {
            // 共有状態を保護するスコープロック
            std::scoped_lock lock(gate->mutex);
            gate->release = true;
        }
        gate->condition.notify_all();
        std::cout << "[cloud-sync] read released" << std::endl;
        Require(
            enteredBackoff,
            "A transient journal lock did not enter bounded backoff.");
        PumpUntil(
            synchronizer,
            [&]
            {
                return synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Idle
                    && !synchronizer.HasInFlightRequest();
            },
            now);
        std::cout << "[cloud-sync] busy idle" << std::endl;
        // 送受信したHTTP要求の履歴
        const auto requests = backend.Requests();
        Require(
            requests.size() == 4u
                && requests[0].method == L"GET"
                && requests[1].method == L"POST"
                && requests[2].method == L"DELETE"
                && requests[3].method == L"GET"
                && !journal.HasLocalDeleteIntent(
                    LamaPon::CloudSaveResource::Preferences())
                && synchronizer.Status().state
                    != LamaPon::Detail::CloudSaveSynchronizerState::Halted,
            "Active Read journal contention lost the delete or halted sync.");
        DrainDetached(synchronizer, now);
    }

    // 容量入替え時に縮小を先行させます。
    void TestQuotaSwapShrinksBeforeGrowth()
    {
        // 信頼済み保存先
        const auto trusted = TrustedPath("quota-swap-order");
        // 信頼済み永続化先の設定
        const auto profile = Profile(trusted);
        // ローカル設定ストア
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        // ローカルセーブデータストア
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        // クラウド同期ジャーナル
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");

        // 保存処理とその同期スナップショット
        // saveAndSnapshot(slot: 保存枠名, blobBytes: 本文サイズ, etag: 基準世代)
        auto saveAndSnapshot = [&saves, &journal](
            const std::string& slot,
            const std::size_t blobBytes,
            const std::string& etag)
        {
            // 要求に含める保存内容
            const auto payload = std::string("{\"blob\":\"")
                + std::string(blobBytes, 'x') + "\"}";
            saves.SaveJson(slot, payload);
            // 保存文書のシリアライズ結果
            const auto document = Documents::ReadSaveData(saves, slot);
            Require(
                document.state
                    == LamaPon::Detail::LocalPersistenceDocumentState::Loaded,
                "Quota fixture SaveData was not readable.");
            // クラウド保存スナップショット
            auto snapshot = LiveSnapshot(
                LamaPon::CloudSaveResource::SaveSlot(slot),
                etag,
                document.bytes);
            journal.RecordBaseline(snapshot);
            return snapshot;
        };

        // クラウド上の保存状態
        std::vector<LamaPon::CloudSaveSnapshot> remote;
        // クラウド保存領域の合計容量
        std::uint64_t remoteTotal{};
        // 15件のセーブ枠を走査
        for (std::size_t index = 0u; index < 15u; ++index)
        {
            // 処理対象のセーブ枠
            const auto slot = "filler-" + std::to_string(index);
            // 同期対象のクラウド資源
            const auto resource =
                LamaPon::CloudSaveResource::SaveSlot(slot);
            // クラウド保存スナップショット
            auto snapshot = LiveSnapshot(
                resource,
                "\"filler-" + std::to_string(index) + "\"",
                Bytes(Json{
                    { "format", "LamaPonSaveData" },
                    { "version", 1 },
                    { "slot", slot },
                    {
                        "data",
                        { { "blob", std::string(1'019'000u, 'x') } }
                    }
                }.dump()));
            // 容量試験のfillerはリモートだけに置き、小さな墓標を記録します。
            journal.RecordBaseline(Tombstone(
                resource,
                "\"filler-base-" + std::to_string(index) + "\""));
            remoteTotal += snapshot.content.size();
            remote.push_back(std::move(snapshot));
        }
        // 増加対象の同期開始状態
        auto growBaseline = saveAndSnapshot(
            "a-grow", 1u, "\"grow-base\"");
        // 縮小対象の同期開始状態
        auto shrinkBaseline = saveAndSnapshot(
            "z-shrink", 800'000u, "\"shrink-base\"");
        remoteTotal += growBaseline.content.size()
            + shrinkBaseline.content.size();
        remote.push_back(growBaseline);
        remote.push_back(shrinkBaseline);

        saves.SaveJson(
            "a-grow",
            std::string("{\"blob\":\"")
                + std::string(800'000u, 'g') + "\"}");
        // 容量増加後のローカル内容
        const auto grownLocal = Documents::ReadSaveData(saves, "a-grow");
        Require(saves.DeleteSlot("z-shrink"), "Quota shrink delete failed.");
        // ローカル保存領域の合計容量
        std::uint64_t localTotal{};
        // ローカル保存領域のセーブ枠一覧
        const auto listing = Documents::ListSaveData(saves);
        // セーブ枠を順に確認
        for (const auto& slot : listing.slots)
        {
            localTotal += Documents::ReadSaveData(saves, slot).bytes.size();
        }
        // 増加更新に必要な追加バイト数
        const auto growth = grownLocal.bytes.size() - growBaseline.content.size();
        Require(
            remoteTotal <= LamaPon::CloudSaveAccountMaxBytes
                && localTotal <= LamaPon::CloudSaveAccountMaxBytes
                && remoteTotal + growth > LamaPon::CloudSaveAccountMaxBytes,
            "Quota swap fixture did not exercise transient overflow.");

        // 縮小更新後のクラウド状態
        const auto remoteAfterShrink = [&]
        {
            // 縮小結果を反映するマニフェスト
            auto value = remote;
            // 項目: 縮小応答を置換
            for (auto& item : value)
            {
                // 縮小対象のslotだけを削除済み応答にします。
                if (item.resource.slot == "z-shrink")
                {
                    item = Tombstone(item.resource, "\"shrink-done\"");
                }
            }
            return value;
        }();
        // 要求と応答を記録するテスト用バックエンド
        ScriptedBackend backend;
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse(remote);
        });
        // request: バックエンドが受信したHTTP要求
        backend.Push([](const LamaPon::HttpRequest& request)
        {
            // 解析したHTTP要求本文
            const auto body = Json::parse(RequestText(request));
            Require(
                request.method == L"DELETE"
                    && body.at("resource").at("slot") == "z-shrink",
                "Quota swap did not send the remote shrink first.");
            return SnapshotResponse(
                200u,
                Tombstone(
                    LamaPon::CloudSaveResource::SaveSlot("z-shrink"),
                    "\"shrink-done\""));
        });
        backend.Push([remoteAfterShrink](const LamaPon::HttpRequest&)
        {
            return ManifestResponse(remoteAfterShrink);
        });
        // request: バックエンドが受信したHTTP要求
        backend.Push([](const LamaPon::HttpRequest& request)
        {
            // 解析したHTTP要求本文
            const auto body = Json::parse(RequestText(request));
            Require(
                request.method == L"PUT"
                    && body.at("resource").at("slot") == "a-grow",
                "Quota swap did not defer remote growth.");
            return SnapshotResponse(
                200u,
                SnapshotFromPut(request, "\"grow-done\""));
        });

        // 次に使う変異IDの共有位置
        auto idIndex = std::make_shared<std::size_t>(0u);
        // テスト対象のクラウド同期器
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(
            journal,
            32u,
            std::string(AccountKey),
            std::string(Token));
        // 同期処理で進める仮想時刻
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                return backend.Requests().size() >= 4u;
            },
            now);
        // 送受信したHTTP要求の履歴
        const auto requests = backend.Requests();
        Require(
            requests.size() >= 4u
                && requests[1].method == L"DELETE"
                && requests[3].method == L"PUT",
            "Quota-safe shrink/growth wire ordering regressed.");
        DrainDetached(synchronizer, now);
    }

    // 取得中に記録した削除意図を再起動後も保ちます。
    void TestDeleteIntentDuringManifestSurvivesRestart()
    {
        // 信頼済み保存先
        const auto trusted = TrustedPath("delete-restart");
        // 信頼済み永続化先の設定
        const auto profile = Profile(trusted);
        // ローカル設定ストア
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        preferences.SetInteger("delete-before-etag", 1);
        preferences.Save();
        // ローカルセーブデータストア
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        // クラウド上の保存状態
        const auto remote = LiveSnapshot(
            LamaPon::CloudSaveResource::Preferences(),
            "\"manifest-live\"",
            Bytes(R"({"format":"LamaPonPlayerPrefs","version":1,"values":{"remote":{"type":"integer","value":5}}})"));
        // 削除済み状態を表す値
        const auto deleted = Tombstone(
            LamaPon::CloudSaveResource::Preferences(),
            "\"manifest-deleted\"");
        struct Gate final
        {
            // 共有状態を保護する排他制御
            std::mutex mutex;
            // スレッド間の通知待ち条件
            std::condition_variable condition;
            // 処理が待機箇所へ到達した状態
            bool entered{};
            // 待機中のワーカーを解放する状態
            bool release{};
        };
        // ワーカーを待機させる共有ゲート
        auto gate = std::make_shared<Gate>();
        // 同期処理で進める仮想時刻
        std::uint64_t now{};
        {
            // クラウド同期ジャーナル
            Journal journal(
                trusted,
                profile,
                "sync-game",
                "staging",
                "https://online.example.test/tenant/");
            // 初回同期に使うテスト用バックエンド
            ScriptedBackend firstBackend;
            firstBackend.Push([gate, remote](const LamaPon::HttpRequest&)
            {
                // 共有状態を保護するスコープロック
                std::unique_lock lock(gate->mutex);
                gate->entered = true;
                gate->condition.notify_all();
                gate->condition.wait(lock, [&] { return gate->release; });
                return ManifestResponse({ remote });
            });
            // 次に使う変異IDの共有位置
            auto idIndex = std::make_shared<std::size_t>(0u);
            // 初回アカウントの同期器
            Synchronizer first(
                preferences,
                saves,
                MakeClient(firstBackend),
                IdGenerator(idIndex));
            first.Attach(
                journal,
                33u,
                std::string(AccountKey),
                std::string(Token));
            PumpUntil(
                first,
                [&]
                {
                    // 共有状態を保護するスコープロック
                    std::scoped_lock lock(gate->mutex);
                    return gate->entered;
                },
                now);
            Documents::DeletePlayerPrefs(preferences);
            first.RequestReconcile();
            Require(
                journal.HasLocalDeleteIntent(
                    LamaPon::CloudSaveResource::Preferences()),
                "Active Manifest did not durably record the local delete intent.");
            first.Detach();
            {
                // 共有状態を保護するスコープロック
                std::scoped_lock lock(gate->mutex);
                gate->release = true;
            }
            gate->condition.notify_all();
            PumpUntil(first, [&] { return !first.HasInFlightRequest(); }, now);
        }

        // 再読込したジャーナル
        Journal reopened(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        Require(
            reopened.HasLocalDeleteIntent(
                LamaPon::CloudSaveResource::Preferences()),
            "Local delete intent was lost across restart.");
        // 要求と応答を記録するテスト用バックエンド
        ScriptedBackend backend;
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        backend.Push([deleted](const LamaPon::HttpRequest&)
        {
            return SnapshotResponse(200u, deleted);
        });
        backend.Push([deleted](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ deleted });
        });
        // 次に使う変異IDの共有位置
        auto idIndex = std::make_shared<std::size_t>(1u);
        // 再接続先アカウントの同期器
        Synchronizer second(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        second.Attach(
            reopened,
            34u,
            std::string(AccountKey),
            std::string(Token));
        PumpUntil(
            second,
            [&]
            {
                return second.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Idle
                    && !second.HasInFlightRequest();
            },
            now);
        // 送受信したHTTP要求の履歴
        const auto requests = backend.Requests();
        Require(
            requests.size() == 3u
                && requests[0].method == L"GET"
                && requests[1].method == L"DELETE"
                && requests[2].method == L"GET"
                && !reopened.HasLocalDeleteIntent(
                    LamaPon::CloudSaveResource::Preferences()),
            "Restart treated a durable delete intent as initial Missing.");
        DrainDetached(second, now);
    }

    // 削除意図が観測済み世代を使うことを検証します。
    void TestPreDeleteIntentUsesObservedBaselineEtag()
    {
        // 信頼済み保存先
        const auto trusted = TrustedPath("predelete-etag");
        // 信頼済み永続化先の設定
        const auto profile = Profile(trusted);
        // ローカル設定ストア
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        // ローカルセーブデータストア
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        // クラウド同期ジャーナル
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        // 同期対象のクラウド資源
        const auto resource =
            LamaPon::CloudSaveResource::SaveSlot("etag-slot");
        saves.SaveJson("etag-slot", R"({"value":1})");
        // 現在のローカル保存内容
        const auto local = Documents::ReadSaveData(saves, "etag-slot");
        // 同期開始時のクラウド基準値
        const auto baseline = LiveSnapshot(
            resource,
            "\"etag-e1\"",
            local.bytes);
        journal.RecordBaseline(baseline);
        // クラウド上の保存状態
        const auto remote = LiveSnapshot(
            resource,
            "\"etag-e2\"",
            Bytes(R"({"format":"LamaPonSaveData","version":1,"slot":"etag-slot","data":{"value":2}})"));

        // 要求と応答を記録するテスト用バックエンド
        ScriptedBackend backend;
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        // request: バックエンドが受信したHTTP要求
        backend.Push([remote](const LamaPon::HttpRequest& request)
        {
            (void)request;
            return ConflictResponse(remote);
        });
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });

        // 次に使う変異IDの共有位置
        auto idIndex = std::make_shared<std::size_t>(0u);
        // テスト対象のクラウド同期器
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(
            journal,
            35u,
            std::string(AccountKey),
            std::string(Token));
        synchronizer.PrepareLocalDelete(resource);
        Require(
            saves.DeleteSlot("etag-slot")
                && journal.HasLocalDeleteIntent(resource),
            "Pre-delete WAL was not durable before the local delete returned.");

        // 同期処理で進める仮想時刻
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                // 停止状態は理由を添えてテスト失敗にします。
                if (synchronizer.Status().state
                    == LamaPon::Detail::CloudSaveSynchronizerState::Halted)
                {
                    throw std::runtime_error(
                        "Pre-delete synchronization halted, reason="
                        + std::to_string(static_cast<int>(
                            synchronizer.Status().stopReason))
                        + ", requests="
                        + std::to_string(backend.Requests().size()));
                }
                return synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Conflict
                    && journal.Conflict(resource).has_value()
                    && !synchronizer.HasInFlightRequest();
            },
            now);
        // 送受信したHTTP要求の履歴
        const auto requests = backend.Requests();
        Require(
            requests.size() == 3u
                && requests[0].method == L"GET"
                && requests[1].method == L"DELETE"
                && requests[2].method == L"GET"
                && RequestHeader(requests[1], L"If-Match")
                    == L"\"etag-e1\""
                && journal.Conflict(resource)->etag == "\"etag-e2\"",
            "Stale pre-delete CAS did not preserve the remote conflict.");
        DrainDetached(synchronizer, now);
    }

    // 墓標復旧時に削除要求を重ねないことを検証します。
    void TestRemoteTombstoneCrashAdvancesBaselineWithoutDelete()
    {
        // 信頼済み保存先
        const auto trusted = TrustedPath("tombstone-crash");
        // 信頼済み永続化先の設定
        const auto profile = Profile(trusted);
        // ローカル設定ストア
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        // ローカルセーブデータストア
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        // クラウド同期ジャーナル
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        // 同期対象のクラウド資源
        const auto resource =
            LamaPon::CloudSaveResource::SaveSlot("crash-slot");
        saves.SaveJson("crash-slot", R"({"value":1})");
        // 変更前の元データ
        const auto original = Documents::ReadSaveData(saves, "crash-slot");
        journal.RecordBaseline(LiveSnapshot(
            resource,
            "\"crash-e1\"",
            original.bytes));
        // 墓標のローカル適用後、基準値記録前のクラッシュを再現します。
        Documents::DeleteSaveData(saves, "crash-slot");
        // クラウド上の保存状態
        const auto remote = Tombstone(resource, "\"crash-e2\"");

        // 要求と応答を記録するテスト用バックエンド
        ScriptedBackend backend;
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        backend.Push([remote](const LamaPon::HttpRequest&)
        {
            return ManifestResponse({ remote });
        });
        // 次に使う変異IDの共有位置
        auto idIndex = std::make_shared<std::size_t>(0u);
        // テスト対象のクラウド同期器
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(
            journal,
            36u,
            std::string(AccountKey),
            std::string(Token));
        // 同期処理で進める仮想時刻
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                // 更新後の保存内容
                const auto updated = journal.Baseline(resource);
                return updated
                    && updated->etag == "\"crash-e2\""
                    && synchronizer.Status().state
                        == LamaPon::Detail::CloudSaveSynchronizerState::Idle
                    && !synchronizer.HasInFlightRequest();
            },
            now);
        // 送受信したHTTP要求の履歴
        const auto requests = backend.Requests();
        Require(
            requests.size() == 2u
                && requests[0].method == L"GET"
                && requests[1].method == L"GET"
                && !journal.Pending(resource)
                && !journal.HasLocalDeleteIntent(resource),
            "Recovered remote tombstone sent a stale DELETE instead of advancing baseline.");
        DrainDetached(synchronizer, now);
    }

    // 保留更新は縮小を先に送ることを検証します。
    void TestPendingMutationsDispatchShrinkBeforeGrowth()
    {
        // Win32のMAX_PATH対策で短いfixture名を使います。
        // 信頼済み保存先
        const auto trusted = TrustedPath("pending-priority");
        // 信頼済み永続化先の設定
        const auto profile = Profile(trusted);
        // ローカル設定ストア
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        // ローカルセーブデータストア
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        // クラウド同期ジャーナル
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        // 容量を増やす保存スロット
        const auto grow =
            LamaPon::CloudSaveResource::SaveSlot("a-grow");
        // 容量を減らす保存スロット
        const auto shrink =
            LamaPon::CloudSaveResource::SaveSlot("z-shrink");

        saves.SaveJson("a-grow", R"({"blob":"x"})");
        // 増加前の保存スロット状態
        const auto growBase =
            Documents::ReadSaveData(saves, "a-grow");
        journal.RecordBaseline(LiveSnapshot(
            grow,
            "\"grow-old\"",
            growBase.bytes));
        saves.SaveJson(
            "a-grow",
            std::string("{\"blob\":\"")
                + std::string(4096u, 'g') + "\"}");
        // 容量増加後の保存スロット状態
        const auto growNext =
            Documents::ReadSaveData(saves, "a-grow");
        journal.QueuePut(
            grow,
            growNext.bytes,
            MutationIds[0],
            "\"grow-old\"");

        saves.SaveJson("z-shrink", R"({"value":1})");
        // 縮小前の保存スロット状態
        const auto shrinkBase =
            Documents::ReadSaveData(saves, "z-shrink");
        journal.RecordBaseline(LiveSnapshot(
            shrink,
            "\"shrink-old\"",
            shrinkBase.bytes));
        Documents::DeleteSaveData(saves, "z-shrink");
        journal.QueueDelete(
            shrink,
            MutationIds[1],
            "\"shrink-old\"");

        // 要求と応答を記録するテスト用バックエンド
        ScriptedBackend backend;
        // request: バックエンドが受信したHTTP要求
        backend.Push([shrink](const LamaPon::HttpRequest& request)
        {
            Require(
                request.method == L"DELETE"
                    && RequestText(request).find("z-shrink")
                        != std::string::npos,
                "Restarted pending growth ran before a pending shrink.");
            return SnapshotResponse(
                200u,
                Tombstone(shrink, "\"shrink-new\""));
        });
        // request: バックエンドが受信したHTTP要求
        backend.Push([](const LamaPon::HttpRequest& request)
        {
            Require(
                request.method == L"PUT"
                    && RequestText(request).find("a-grow")
                        != std::string::npos,
                "Pending growth did not follow the completed shrink.");
            return SnapshotResponse(
                200u,
                SnapshotFromPut(request, "\"grow-new\""));
        });

        // 次に使う変異IDの共有位置
        auto idIndex = std::make_shared<std::size_t>(2u);
        // テスト対象のクラウド同期器
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(
            journal,
            38u,
            std::string(AccountKey),
            std::string(Token));
        // 同期処理で進める仮想時刻
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                return backend.Requests().size() >= 2u;
            },
            now);
        // 送受信したHTTP要求の履歴
        const auto requests = backend.Requests();
        Require(
            requests.size() == 2u
                && requests[0].method == L"DELETE"
                && requests[1].method == L"PUT",
            "Pending dispatch ordering was not shrink-before-growth.");
        DrainDetached(synchronizer, now);
    }

    // 切離し後の古い応答を破棄します。
    void TestDetachDiscardsStaleWorkerWithoutBlocking()
    {
        // 信頼済み保存先
        const auto trusted = TrustedPath("detach-race");
        // 信頼済み永続化先の設定
        const auto profile = Profile(trusted);
        // ローカル設定ストア
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        // ローカルセーブデータストア
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        // クラウド同期ジャーナル
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        struct Gate final
        {
            // 共有状態を保護する排他制御
            std::mutex mutex;
            // スレッド間の通知待ち条件
            std::condition_variable condition;
            // 処理が待機箇所へ到達した状態
            bool entered{};
            // 待機中のワーカーを解放する状態
            bool release{};
        };
        // ワーカーを待機させる共有ゲート
        auto gate = std::make_shared<Gate>();
        // 要求と応答を記録するテスト用バックエンド
        ScriptedBackend backend;
        backend.Push([gate](const LamaPon::HttpRequest&)
        {
            // 共有状態を保護するスコープロック
            std::unique_lock lock(gate->mutex);
            gate->entered = true;
            gate->condition.notify_all();
            gate->condition.wait(lock, [&] { return gate->release; });
            return ManifestResponse({});
        });
        // 次に使う変異IDの共有位置
        auto idIndex = std::make_shared<std::size_t>(0u);
        // テスト対象のクラウド同期器
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(journal, 31u, std::string(AccountKey), std::string(Token));
        // 同期処理で進める仮想時刻
        std::uint64_t now{};
        PumpUntil(
            synchronizer,
            [&]
            {
                // 共有状態を保護するスコープロック
                std::scoped_lock lock(gate->mutex);
                return gate->entered;
            },
            now);

        // ワーカーが開始した状態
        const auto started = std::chrono::steady_clock::now();
        synchronizer.Detach();
        // 経過した待機時間
        const auto elapsed = std::chrono::steady_clock::now() - started;
        Require(
            elapsed < std::chrono::milliseconds(100),
            "Detach waited for the synchronous cloud request.");
        {
            // 共有状態を保護するスコープロック
            std::scoped_lock lock(gate->mutex);
            gate->release = true;
        }
        gate->condition.notify_all();
        PumpUntil(
            synchronizer,
            [&]
            {
                return !synchronizer.HasInFlightRequest();
            },
            now);
        Require(
            synchronizer.Status().state
                    == LamaPon::Detail::CloudSaveSynchronizerState::Detached
                && journal.Generation() == 0u
                && !journal.Pending(
                    LamaPon::CloudSaveResource::Preferences()),
            "A stale worker result mutated detached account state.");
    }

    // 破棄時にワーカー終了を待たないことを検証します。
    void TestDestructorDoesNotJoinWorker()
    {
        // 信頼済み保存先
        const auto trusted = TrustedPath("destructor-race");
        // 信頼済み永続化先の設定
        const auto profile = Profile(trusted);
        // ローカル設定ストア
        LamaPon::PlayerPrefs preferences(profile.playerPrefsFile);
        // ローカルセーブデータストア
        LamaPon::SaveDataStore saves(profile.saveDataDirectory);
        // クラウド同期ジャーナル
        Journal journal(
            trusted,
            profile,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        struct Gate final
        {
            // 共有状態を保護する排他制御
            std::mutex mutex;
            // スレッド間の通知待ち条件
            std::condition_variable condition;
            // 処理が待機箇所へ到達した状態
            bool entered{};
            // 待機中のワーカーを解放する状態
            bool release{};
            // 完了を通知する状態
            bool completed{};
        };
        // ワーカーを待機させる共有ゲート
        auto gate = std::make_shared<Gate>();
        // 要求と応答を記録するテスト用バックエンド
        ScriptedBackend backend;
        backend.Push([gate](const LamaPon::HttpRequest&)
        {
            {
                // 共有状態を保護するスコープロック
                std::unique_lock lock(gate->mutex);
                gate->entered = true;
                gate->condition.notify_all();
                gate->condition.wait(lock, [&] { return gate->release; });
                gate->completed = true;
            }
            gate->condition.notify_all();
            return ManifestResponse({});
        });
        // 次に使う変異IDの共有位置
        auto idIndex = std::make_shared<std::size_t>(0u);
        // テスト対象のクラウド同期器
        auto synchronizer = std::make_unique<Synchronizer>(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer->Attach(
            journal,
            37u,
            std::string(AccountKey),
            std::string(Token));
        // 同期処理で進める仮想時刻
        std::uint64_t now{};
        PumpUntil(
            *synchronizer,
            [&]
            {
                // 共有状態を保護するスコープロック
                std::scoped_lock lock(gate->mutex);
                return gate->entered;
            },
            now);
        // ワーカーを解放する補助スレッド
        std::thread releaser([gate]
        {
            Sleep(250u);
            {
                // 共有状態を保護するスコープロック
                std::scoped_lock lock(gate->mutex);
                gate->release = true;
            }
            gate->condition.notify_all();
        });
        // ワーカーが開始した状態
        const auto started = std::chrono::steady_clock::now();
        synchronizer.reset();
        // 経過した待機時間
        const auto elapsed = std::chrono::steady_clock::now() - started;
        releaser.join();
        Require(
            elapsed < std::chrono::milliseconds(100),
            "Synchronizer destructor joined a wire worker.");
        {
            // 共有状態を保護するスコープロック
            std::unique_lock lock(gate->mutex);
            Require(
                gate->condition.wait_for(
                    lock,
                    std::chrono::seconds(5),
                    [&] { return gate->completed; }),
                "Detached worker did not finish after destruction.");
        }
        // 完了通知後のmailbox反映を待ちます。
        Sleep(10u);
        Require(
            journal.Generation() == 0u,
            "Destroyed synchronizer's worker touched the journal.");
    }

    // 再接続時に競合記録を破棄します。
    void TestConflictDescriptorCacheIsInvalidatedAcrossAttachments()
    {
        // 先行アカウントの信頼済みパス
        const auto trustedA = TrustedPath("descriptor-cache-a");
        // 後続アカウントの信頼済みパス
        const auto trustedB = TrustedPath("descriptor-cache-b");
        // 先行アカウントの保存先設定
        const auto profileA = Profile(trustedA);
        // 後続アカウントの保存先設定
        const auto profileB = Profile(trustedB);
        // ローカル設定ストア
        LamaPon::PlayerPrefs preferences(profileA.playerPrefsFile);
        preferences.SetInteger("value", 1);
        preferences.Save();
        // ローカルセーブデータストア
        LamaPon::SaveDataStore saves(profileA.saveDataDirectory);
        // 先行するアカウントのジャーナル
        Journal journalA(
            trustedA,
            profileA,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");
        // 後続アカウントのジャーナル
        Journal journalB(
            trustedB,
            profileB,
            "sync-game",
            "staging",
            "https://online.example.test/tenant/");

        // 設定ストアのシリアライズ結果
        const auto preferencesBytes =
            Documents::ReadPlayerPrefs(preferences).bytes;
        // セーブ枠のシリアライズ結果
        const auto slotBytes = Bytes(
            R"({"format":"LamaPonSaveData","version":1,"slot":"other-slot","data":{}})");
        // 設定ストアのクラウド識別子
        const auto preferencesResource =
            LamaPon::CloudSaveResource::Preferences();
        // セーブ枠のクラウド識別子
        const auto slotResource =
            LamaPon::CloudSaveResource::SaveSlot("other-slot");
        journalA.QueuePut(
            preferencesResource,
            preferencesBytes,
            MutationIds[0]);
        journalA.RecordConflict(
            preferencesResource,
            MutationIds[0],
            Tombstone(preferencesResource, "\"remote-a\""));
        journalB.QueuePut(slotResource, slotBytes, MutationIds[1]);
        journalB.RecordConflict(
            slotResource,
            MutationIds[1],
            Tombstone(slotResource, "\"remote-b\""));
        Require(
            journalA.Generation() == journalB.Generation(),
            "Descriptor cache journals did not share a generation fixture.");

        // 要求と応答を記録するテスト用バックエンド
        ScriptedBackend backend;
        // 次に使う変異IDの共有位置
        auto idIndex = std::make_shared<std::size_t>(2u);
        // テスト対象のクラウド同期器
        Synchronizer synchronizer(
            preferences,
            saves,
            MakeClient(backend),
            IdGenerator(idIndex));
        synchronizer.Attach(
            journalA,
            77u,
            std::string(AccountKey),
            std::string(Token));
        // 接続中アカウントの競合一覧
        const auto first = synchronizer.Conflicts();
        synchronizer.Detach();
        preferences.Rebind(profileB.playerPrefsFile);
        saves.Rebind(profileB.saveDataDirectory);
        synchronizer.Attach(
            journalB,
            77u,
            std::string(AccountKey),
            std::string(Token));
        // 再接続先アカウントの競合一覧
        const auto second = synchronizer.Conflicts();
        Require(
            first.size() == 1u
                && first.front().resource.kind
                    == LamaPon::CloudSaveResourceKind::Preferences
                && second.size() == 1u
                && second.front().resource.kind
                    == LamaPon::CloudSaveResourceKind::SaveSlot
                && second.front().resource.slot == "other-slot",
            "Conflict descriptor cache leaked across attachments.");
        synchronizer.Detach();
    }
}

// クラウド同期テスト群を実行します。
int main()
{
    // テスト全体の失敗を共通の終了処理へまとめます。
    try
    {
        ResetTestRoot();
        // テストケースを識別名付きで実行します。
        // run(name: テスト識別名, test: 実行するテスト)
        const auto run = [](const char* name, const auto test)
        {
            std::cout << "[cloud-sync] " << name << std::endl;
            // 個別テストを識別名付きで実行します。
            try
            {
                test();
            }
            // exception: 個別テストで発生した失敗情報
            // 失敗へテスト名を付けて呼び出し元へ返します。
            catch (const std::exception& exception)
            {
                throw std::runtime_error(
                    std::string(name) + ": " + exception.what());
            }
        };
        run("pending overlay", TestPendingOverlayChainsAfterAck);
        run("pending retry", TestPendingRetryKeepsMutationIdentity);
        run("delete recreation", TestRemoteDeleteThenLocalRecreationBecomesPut);
        run("slot case identity", TestRemoteSlotCaseUsesExistingLocalSpelling);
        run("conflict", TestCasConflictRetryAndUseRemote);
        run("initial conflict", TestInitialDualContentRequiresConflict);
        run("read invalidation", TestLocalCommitInvalidatesActiveRead);
        run("initial delete", TestDeleteDuringInitialReadIsNotInitialMissing);
        run("corrupt local", TestCorruptLocalStopsBeforeNetwork);
        run("wire retry", TestUnauthorizedRefreshAndRateLimitBackoff);
        run("journal busy read", TestJournalBusyDuringActiveReadRetriesWithoutHalting);
        run("quota swap", TestQuotaSwapShrinksBeforeGrowth);
        run("manifest delete restart", TestDeleteIntentDuringManifestSurvivesRestart);
        run("pre-delete etag", TestPreDeleteIntentUsesObservedBaselineEtag);
        run("tombstone crash", TestRemoteTombstoneCrashAdvancesBaselineWithoutDelete);
        run("pending shrink priority", TestPendingMutationsDispatchShrinkBeforeGrowth);
        run("detach", TestDetachDiscardsStaleWorkerWithoutBlocking);
        run("destructor", TestDestructorDoesNotJoinWorker);
        run("descriptor cache", TestConflictDescriptorCacheIsInvalidatedAcrossAttachments);
        ResetTestRoot();
        std::cout << "Cloud save synchronizer tests passed.\n";
        return 0;
    }
    // error: テスト全体で発生した失敗情報
    // 失敗を標準エラーへ出し、非ゼロで終了します。
    catch (const std::exception& error)
    {
        std::cerr << "Cloud save synchronizer tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
