#include "LamaPon/Core/PersistenceProfiles.h"
#include "LamaPon/Core/LocalPersistenceDocuments.h"
#include "LamaPon/Core/PlayerPrefs.h"
#include "LamaPon/Core/SaveData.h"
#include "LamaPon/Editor/PersistencePanelState.h"
#include "LamaPon/Online/CloudSaveClient.h"
#include "LamaPon/Online/CloudSaveJournal.h"
#include "LamaPon/Online/OnlinePersistenceCoordinator.h"
#include "LamaPon/Online/OnlineServicesTesting.h"
#include "LamaPon/Online/CloudSaveSynchronizer.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
    // Coordinator: 対象の永続化調整器。
    using Coordinator =
        LamaPon::Detail::OnlinePersistenceCoordinator;
    // DetachResult: 調整器の切り離し結果。
    using DetachResult =
        LamaPon::Detail::OnlinePersistenceDetachResult;

    // SaveDataStoreがコピー・ムーブ不可であることを検証します。
    static_assert(!std::is_copy_constructible_v<LamaPon::SaveDataStore>);
    static_assert(!std::is_copy_assignable_v<LamaPon::SaveDataStore>);
    static_assert(!std::is_move_constructible_v<LamaPon::SaveDataStore>);
    static_assert(!std::is_move_assignable_v<LamaPon::SaveDataStore>);

    // Require(condition: 成功条件, message: 失敗理由) 条件不成立を例外化します。
    void Require(const bool condition, const char* message)
    {
        // 不成立の条件を検出します。
        if (!condition)
        {
            // テスト失敗を例外で通知します。
            throw std::runtime_error(message);
        }
    }

    // Throws(function: 例外確認処理) 関数が例外を送出するか調べます。
    template<class Function>
    bool Throws(Function&& function)
    {
        // 例外送出の有無を捕捉します。
        try
        {
            std::forward<Function>(function)();
            // 例外が送出されませんでした。
            return false;
        }
        // 送出された例外を成功条件として扱います。
        catch (...)
        {
            // 例外送出を結果として返します。
            return true;
        }
    }

    // ファイルハンドルの後始末を検証します。
    struct FileHandle final
    {
        // value: 所有するWindowsハンドル。
        HANDLE value{ INVALID_HANDLE_VALUE };

        // ~FileHandle() 所有ハンドルを閉じます。
        ~FileHandle()
        {
            Close();
        }

        // Close(): 有効なハンドルを閉じて無効化します。
        void Close() noexcept
        {
            // 有効なハンドルだけを閉じます。
            if (value != INVALID_HANDLE_VALUE)
            {
                CloseHandle(value);
                value = INVALID_HANDLE_VALUE;
            }
        }
    };

    // JsonResponse(status: HTTP状態, document: 応答JSON) JSON応答を構築します。
    LamaPon::HttpResponse JsonResponse(
        const std::uint32_t status,
        const nlohmann::json& document = nlohmann::json::object())
    {
        // response: HTTP応答の格納先。
        LamaPon::HttpResponse response;
        response.statusCode = status;
        // 本文を持つ応答だけJSONをシリアライズします。
        if (status != 204u)
        {
            // text: シリアライズ済みJSON。
            const auto text = document.dump();
            response.body.assign(text.begin(), text.end());
        }
        // 構築したHTTP応答を返します。
        return response;
    }

    // EmptyCloudManifestResponse(): 空のクラウド保存一覧を返します。
    LamaPon::HttpResponse EmptyCloudManifestResponse()
    {
        // response: 空一覧を含むHTTP応答。
        auto response = JsonResponse(
            200u,
            {
                { "protocolVersion", 1u },
                { "items", nlohmann::json::array() }
            });
        // JSON本文の文字コードを応答ヘッダーに示します。
        response.headers.emplace_back(
            L"Content-Type",
            L"application/json; charset=utf-8");
        // 応答本文の長さをヘッダーに示します。
        response.headers.emplace_back(
            L"Content-Length",
            std::to_wstring(response.body.size()));
        // 完成した空一覧応答を返します。
        return response;
    }

    // SessionJson(accessToken: 認証トークン, refreshToken: 更新トークン, playerId: プレイヤーID, expiresIn: 有効秒数) セッションJSONを構築します。
    nlohmann::json SessionJson(
        const std::string_view accessToken,
        const std::string_view refreshToken,
        const std::string_view playerId,
        const std::uint32_t expiresIn = 30u)
    {
        // 入力した認証情報とプレイヤー情報をまとめます。
        return {
            { "accessToken", accessToken },
            { "refreshToken", refreshToken },
            { "expiresIn", expiresIn },
            {
                "player",
                {
                    { "id", playerId },
                    { "displayName", "test-player" },
                    { "avatarUrl", "" },
                    { "linkedProvider", "discord" }
                }
            }
        };
    }

    // HTTP応答列を返し、指定要求でワーカーを停止できるテスト用バックエンドです。
    class ScriptedBackend final
    {
    public:
        // ScriptedBackend(responses: 返却応答列, blockedRequest: 停止する要求番号) テスト応答を設定します。
        explicit ScriptedBackend(
            std::deque<LamaPon::HttpResponse> responses,
            const std::optional<std::size_t> blockedRequest = std::nullopt)
            : m_responses(std::move(responses))
            , m_blockedRequest(blockedRequest)
        {
        }

        // Send(request: HTTP要求) 要求を記録して対応する応答を返します。
        LamaPon::HttpResponse Send(const LamaPon::HttpRequest& request)
        {
            // request: 要求一覧と停止条件を保護するロック。
            std::unique_lock lock(m_mutex);
            // requestIndex: 今回記録する要求番号。
            const auto requestIndex = m_requests.size();
            m_requests.push_back(request);
            // 応答列を使い切った不正要求を検出します。
            if (m_responses.empty())
            {
                // unexpected: 予期しない要求を示す応答。
                LamaPon::HttpResponse unexpected;
                unexpected.transportError = "Unexpected request.";
                // 不正要求の通信エラーを返します。
                return unexpected;
            }
            // response: 今回返す応答。
            auto response = std::move(m_responses.front());
            m_responses.pop_front();
            // 指定された要求だけ完了を保留します。
            if (m_blockedRequest == requestIndex)
            {
                m_requestBlocked = true;
                m_condition.notify_all();
                // ReleaseBlockedRequestの通知まで応答ワーカーを待たせます。
                m_condition.wait(
                    lock,
                    [this]
                    {
                        // 要求再開状態を待機条件へ返します。
                        return m_releaseBlockedRequest;
                    });
            }
            // 設定済み応答を呼び出し元へ返します。
            return response;
        }

        // WaitUntilRequestBlocked(): 指定要求が停止状態になるまで待ちます。
        void WaitUntilRequestBlocked()
        {
            // lock: 要求停止状態を保護するロック。
            std::unique_lock lock(m_mutex);
            // 停止通知を最大10秒待ち、未到着ならテストを失敗させます。
            if (!m_condition.wait_for(
                    lock,
                    std::chrono::seconds(10),
                    [this]
                    {
                        // 要求停止状態を待機条件へ返します。
                        return m_requestBlocked;
                    }))
            {
                // 想定要求が停止しなかったことを通知します。
                throw std::runtime_error(
                    "Expected online request did not block.");
            }
        }

        // ReleaseBlockedRequest(): 停止中の要求を再開させます。
        void ReleaseBlockedRequest() noexcept
        {
            {
                // lock: 再開フラグを保護する短期ロック。
                std::scoped_lock lock(m_mutex);
                // 待機中の応答を解放します。
                m_releaseBlockedRequest = true;
            }
            // 条件変数の待機を起こします。
            m_condition.notify_all();
        }

        // Requests(): 記録済みHTTP要求のコピーを返します。
        [[nodiscard]] std::vector<LamaPon::HttpRequest> Requests() const
        {
            // 要求一覧を保護するロック。
            std::scoped_lock lock(m_mutex);
            // ロック解放後に要求一覧のコピーを返します。
            return m_requests;
        }

    private:
        // 要求列と状態を保護するロック。
        mutable std::mutex m_mutex;
        // 要求停止状態の通知。
        std::condition_variable m_condition;
        // 次に返すHTTP応答列。
        std::deque<LamaPon::HttpResponse> m_responses;
        // 受信したHTTP要求列。
        std::vector<LamaPon::HttpRequest> m_requests;
        // 完了を保留する要求番号。
        std::optional<std::size_t> m_blockedRequest;
        // 対象要求が停止した状態。
        bool m_requestBlocked{};
        // 停止中の要求を解放する状態。
        bool m_releaseBlockedRequest{};
    };

    // メモリ上の更新トークン保存状態を共有します。
    struct TokenStoreState final
    {
        // 保存トークンの読込状態。
        LamaPon::Detail::RefreshTokenLoadStatus loadStatus{
            LamaPon::Detail::RefreshTokenLoadStatus::NotFound
        };
        // 保存済み更新トークン。
        std::string token;
        // トークン保存の呼び出し回数。
        std::size_t saveCount{};
        // トークン削除の呼び出し回数。
        std::size_t deleteCount{};
        // 保存処理の失敗指定。
        bool failSave{};
        // 削除処理の失敗指定。
        bool failDelete{};
        // 保存完了時に呼ぶテスト処理。
        std::function<void()> onSave;
    };

    // OnlineServices向けのメモリ内更新トークンストアです。
    class MemoryTokenStore final
        : public LamaPon::Detail::IRefreshTokenStore
    {
    public:
        // MemoryTokenStore(state: 共有保存状態) テスト用ストアを初期化します。
        explicit MemoryTokenStore(
            std::shared_ptr<TokenStoreState> state)
            : m_state(std::move(state))
        {
        }

        // Load(): 共有状態から更新トークンを読み込みます。
        LamaPon::Detail::RefreshTokenLoadResult Load() override
        {
            // result: 読込状態とトークンの返却値。
            LamaPon::Detail::RefreshTokenLoadResult result;
            result.status = m_state->loadStatus;
            // 読込済みトークンだけ結果へコピーします。
            if (result.Loaded())
            {
                result.refreshToken = m_state->token;
            }
            // 読込結果を返します。
            return result;
        }

        // Save(refreshToken: 更新トークン) 共有状態へ保存し、指定の失敗を再現します。
        LamaPon::Detail::OnlinePlatformResult Save(
            const std::string_view refreshToken) override
        {
            ++m_state->saveCount;
            // 保存完了フックがある場合は呼び出します。
            if (m_state->onSave)
            {
                m_state->onSave();
            }
            // 指定された保存失敗を返します。
            if (m_state->failSave)
            {
                // 内部エラー情報を返して上位層の秘匿処理を検証します。
                return { false, "private", "private" };
            }
            m_state->token = refreshToken;
            m_state->loadStatus =
                LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
            // 保存成功を返します。
            return { true, {}, {} };
        }

        // Delete(): 共有状態から更新トークンを削除します。
        LamaPon::Detail::OnlinePlatformResult Delete() override
        {
            ++m_state->deleteCount;
            // 指定された削除失敗を返します。
            if (m_state->failDelete)
            {
                // 内部エラー情報を返して上位層の秘匿処理を検証します。
                return { false, "private", "private" };
            }
            m_state->token.clear();
            m_state->loadStatus =
                LamaPon::Detail::RefreshTokenLoadStatus::NotFound;
            // 削除成功を返します。
            return { true, {}, {} };
        }

    private:
        // テスト間で共有するトークン状態。
        std::shared_ptr<TokenStoreState> m_state;
    };

    // OnlineConfiguration(): テスト用サービス接続設定を返します。
    LamaPon::OnlineServiceConfiguration OnlineConfiguration()
    {
        // configuration: 接続先とサービス識別子。
        LamaPon::OnlineServiceConfiguration configuration;
        configuration.serviceBaseUrl =
            "https://online.example.test/tenant";
        configuration.gameId = "coordinator-game";
        configuration.environmentId = "test";
        configuration.openAuthorizationBrowser = false;
        // 設定済みオンライン構成を返します。
        return configuration;
    }

    // UpdateUntil(predicate: 継続条件, timeoutMessage: 期限切れ理由) サービス更新を続けて条件を待ちます。
    template<class Predicate>
    void UpdateUntil(
        LamaPon::OnlineServices& services,
        Predicate&& predicate,
        const char* timeoutMessage)
    {
        // deadline: 条件待ちの終了時刻。
        const auto deadline =
            std::chrono::steady_clock::now()
            + std::chrono::seconds(10);
        // 条件成立または期限切れまでサービスを更新します。
        while (!std::forward<Predicate>(predicate)())
        {
            services.Update(0.0f);
            // 待機期限切れを検出します。
            if (std::chrono::steady_clock::now() >= deadline)
            {
                // 呼び出し元の失敗理由を通知します。
                throw std::runtime_error(timeoutMessage);
            }
            std::this_thread::yield();
        }
    }

    // WaitUntilRequestCount(backend: 要求記録先, expectedCount: 必要件数, timeoutMessage: 期限切れ理由) 到着件数を待ちます。
    void WaitUntilRequestCount(
        const std::shared_ptr<ScriptedBackend>& backend,
        const std::size_t expectedCount,
        const char* timeoutMessage)
    {
        // deadline: 要求到着を待つ終了時刻。
        const auto deadline =
            std::chrono::steady_clock::now()
            + std::chrono::seconds(10);
        // 必要件数に達するか期限切れまで待ちます。
        while (backend->Requests().size() < expectedCount)
        {
            // 待機期限切れを検出します。
            if (std::chrono::steady_clock::now() >= deadline)
            {
                // 呼び出し元の失敗理由を通知します。
                throw std::runtime_error(timeoutMessage);
            }
            std::this_thread::yield();
        }
    }

    // WaitUntilCurrentTaskCompleted(services: 更新対象, timeoutMessage: 期限切れ理由) 現在タスクの完了を待ちます。
    void WaitUntilCurrentTaskCompleted(
        LamaPon::OnlineServices& services,
        const char* timeoutMessage)
    {
        // deadline: タスク完了を待つ終了時刻。
        const auto deadline =
            std::chrono::steady_clock::now()
            + std::chrono::seconds(10);
        // タスク完了または期限切れまでサービスを更新します。
        while (!LamaPon::Detail::OnlineServicesTestAccess::
            CurrentTaskCompleted(services))
        {
            // 待機期限切れを検出します。
            if (std::chrono::steady_clock::now() >= deadline)
            {
                // 呼び出し元の失敗理由を通知します。
                throw std::runtime_error(timeoutMessage);
            }
            std::this_thread::yield();
        }
    }

    // CompleteDiscordLogin(services: 対象サービス) 認証待ち状態までサインインを進めます。
    void CompleteDiscordLogin(LamaPon::OnlineServices& services)
    {
        // 認証フローの開始可否を検証します。
        Require(
            services.BeginDiscordSignIn(),
            "Could not begin the integration login.");
        // 認証ページ待ち状態までサービスを更新します。
        UpdateUntil(
            services,
            [&]
            {
                // 認証待ち状態への到達を判定します。
                return services.State()
                    == LamaPon::OnlineAccountState::WaitingForAuthorization;
            },
            "Login start did not complete.");
        // 認証待ち状態を次の更新へ反映します。
        services.Update(1.0f);
    }

    // HasBearer(request: HTTP要求, token: 期待トークン) Authorization値を照合します。
    bool HasBearer(
        const LamaPon::HttpRequest& request,
        const std::wstring_view token)
    {
        // expected: HTTP Bearer形式の期待値。
        const auto expected = std::wstring(L"Bearer ")
            + std::wstring(token);
        // nameとvalue: 要求ヘッダーの名前と内容。
        for (const auto& [name, value] : request.headers)
        {
            // Authorizationヘッダーの値だけを照合します。
            if (name == L"Authorization" && value == expected)
            {
                // 期待するBearer値が見つかりました。
                return true;
            }
        }
        // 一致するAuthorizationヘッダーはありません。
        return false;
    }

    // TestRoot(): 永続化調整テスト専用の出力先を返します。
    std::filesystem::path TestRoot()
    {
        // root: test-output配下に固定したテスト出力先。
        const auto root = std::filesystem::absolute(
            std::filesystem::current_path()
            / L"test-output"
            / L"online-profile").lexically_normal();
        // 後始末対象が専用出力先の直下であることを検証します。
        Require(
            root.parent_path().filename() == L"test-output",
            "Coordinator test cleanup root escaped test-output.");
        // 正規化したテスト出力先を返します。
        return root;
    }

    // ResetTestRoot(): 専用出力先を空にして作り直します。
    void ResetTestRoot()
    {
        // error: 削除または作成時のファイルシステムエラー。
        std::error_code error;
        // 前回のテスト成果物を削除します。
        std::filesystem::remove_all(TestRoot(), error);
        // 削除失敗をテスト失敗にします。
        Require(!error, "Coordinator test cleanup failed.");
        // 空の専用出力先を作成します。
        std::filesystem::create_directories(TestRoot(), error);
        // 作成失敗をテスト失敗にします。
        Require(!error, "Coordinator test root creation failed.");
    }

    // CaseRoot(name: ケース名) ケース固有の保存先を返します。
    std::filesystem::path CaseRoot(const std::string_view name)
    {
        // ケース名とアカウント表示名から衝突しない保存先を構成します。
        return TestRoot()
            / std::filesystem::path(std::u8string(
                reinterpret_cast<const char8_t*>(name.data()),
                reinterpret_cast<const char8_t*>(
                    name.data() + name.size())))
            / L"GuestDisplayName";
    }

    // WriteText(path: 出力先, text: UTF-8内容) テスト用ファイルを書き込みます。
    void WriteText(
        const std::filesystem::path& path,
        const std::string_view text)
    {
        std::filesystem::create_directories(path.parent_path());
        // output: バイナリモードで上書きするファイル。
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << text;
        // 書き込み失敗をテスト失敗にします。
        Require(static_cast<bool>(output), "Could not write test fixture.");
    }

    // ReadText(path: 読込元) テスト用ファイル全体を文字列で返します。
    std::string ReadText(const std::filesystem::path& path)
    {
        // input: バイナリモードで読むテストファイル。
        std::ifstream input(path, std::ios::binary);
        // 読込失敗をテスト失敗にします。
        Require(static_cast<bool>(input), "Could not read test fixture.");
        // ファイル全体を反復子で読み取って返します。
        return {
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()
        };
    }

    // ConfigureCoordinator(coordinator: 対象調整器) テスト用の保存名前空間を設定します。
    void ConfigureCoordinator(Coordinator& coordinator)
    {
        // テスト専用のサービス識別子と接続先を登録します。
        coordinator.ConfigureNamespace(
            "coordinator-game",
            "test",
            "https://online.example.test/tenant/",
            false);
    }

    // ConfigureCloudCoordinator(coordinator: 対象調整器) 予期しない通信を失敗にするCloudSaveClientを設定します。
    void ConfigureCloudCoordinator(Coordinator& coordinator)
    {
        // client: 通信失敗を返すテスト用クライアントと、そのresponse。
        auto client = std::make_shared<
            LamaPon::Detail::CloudSaveClient>(
                "https://online.example.test/tenant/",
                "coordinator-game",
                "test",
                false,
                [](const LamaPon::HttpRequest&)
                {
                    // response: 想定外通信を示すHTTP応答。
                    LamaPon::HttpResponse response;
                    response.transportError =
                        "Unexpected checkpoint fixture request.";
                    // 生成したHTTP応答を呼び出し元へ返します。
                    return response;
                });
        // 生成したクライアントと固定名前空間を調整器へ登録します。
        coordinator.ConfigureNamespace(
            "coordinator-game",
            "test",
            "https://online.example.test/tenant/",
            false,
            std::move(client));
    }

    // アカウント切替でEditorの永続化編集内容が破棄されることを検証します。
    void TestEditorDraftsAreInvalidatedAcrossProfileBindings()
    {
        // state: ゲストアカウントに結び付いたEditor状態。
        LamaPon::Detail::PersistencePanelState state(
            L"C:/test/guest/PlayerPrefs.json",
            L"C:/test/guest/Saves");
        // ゲストのPlayerPrefsキーと値を入力します。
        strncpy_s(
            state.playerPrefKey.data(),
            state.playerPrefKey.size(),
            "guest-key",
            _TRUNCATE);
        // ゲストの保存スロットとJSONを入力します。
        strncpy_s(
            state.playerPrefValue.data(),
            state.playerPrefValue.size(),
            "guest-value",
            _TRUNCATE);
        strncpy_s(
            state.saveSlot.data(),
            state.saveSlot.size(),
            "guest-slot",
            _TRUNCATE);
        strncpy_s(
            state.saveJson.data(),
            state.saveJson.size(),
            R"({"guest":true})",
            _TRUNCATE);
        // 入力フォームにゲストの選択状態を設定します。
        state.selectedSaveSlot = "guest-slot";
        state.playerPrefType = 3;
        state.playerPrefBoolean = true;
        // initialRevision: 現在のアカウント結合版。
        const auto initialRevision = state.BindingRevision();

        // 同じ保存先では編集内容と結合版を維持します。
        Require(
            !state.SynchronizeBinding(
                L"C:/test/guest/PlayerPrefs.json",
                L"C:/test/guest/Saves")
                && std::string(state.playerPrefKey.data()) == "guest-key"
                && state.BindingRevision() == initialRevision,
            "Unchanged persistence binding discarded the editor draft.");

        // 別アカウントへ切り替えると編集内容と削除確認状態を初期化します。
        Require(
            state.SynchronizeBinding(
                L"C:/test/account/PlayerPrefs.json",
                L"C:/test/account/Saves")
                && state.BindingRevision() != initialRevision
                && state.playerPrefKey.front() == '\0'
                && state.playerPrefValue.front() == '\0'
                && state.saveSlot.front() == '\0'
                && std::string(state.saveJson.data()) == "{}"
                && state.selectedSaveSlot.empty()
                && state.playerPrefType == 0
                && !state.playerPrefBoolean
                && state.CloseDeleteAllPopupRequested(),
            "Profile switch retained an editor persistence draft.");
        // 閉じる通知を受領した後はポップアップ要求を消去します。
        state.AcknowledgeCloseDeleteAllPopup();
        // 古いPlayerPrefs削除ポップアップが残らないことを検証します。
        Require(
            !state.CloseDeleteAllPopupRequested(),
            "Stale PlayerPrefs delete popup was not invalidated.");
    }

    // 厳密読込後の差し替えや削除で保存済みスナップショットを損なわないことを検証します。
    void TestValidatedPlayerPrefsSnapshotClosesReopenRace()
    {
        // root: スナップショット競合ケースの保存先。
        const auto root = CaseRoot("snapshot-race");
        // path: アカウントのPlayerPrefsファイル。
        const auto path = root / L"Account" / L"PlayerPrefs.json";
        // source: 読込済みスナップショットを作るPlayerPrefs。
        LamaPon::PlayerPrefs source(path);
        source.Load();
        source.SetString("snapshot", "validated-value");
        source.Save();
        // snapshot: 検証済みのPlayerPrefs内容。
        const auto snapshot =
            LamaPon::Detail::LocalPersistenceDocuments::ReadPlayerPrefs(
                source);

        WriteText(path, "{strict-invalid");
        // replacedAfterRead: ディスク差し替え後にスナップショットを適用する対象。
        LamaPon::PlayerPrefs replacedAfterRead(path);
        LamaPon::Detail::LocalPersistenceDocuments::
            LoadPlayerPrefsSnapshot(replacedAfterRead, snapshot);
        // 差し替え後も準備済み値を読み込み、不正ディスク内容を上書きしません。
        Require(
            replacedAfterRead.GetString("snapshot") == "validated-value"
                && !replacedAfterRead.IsDirty()
                && !replacedAfterRead.HasLoadFailure()
                && LamaPon::Detail::LocalPersistenceDocuments::
                    ReadPlayerPrefs(replacedAfterRead).state
                    == LamaPon::Detail::
                        LocalPersistenceDocumentState::Corrupt,
            "Strict-invalid replacement changed or was overwritten by the prepared snapshot.");

        // removeError: 競合ケース用ファイル削除のエラー。
        std::error_code removeError;
        std::filesystem::remove(path, removeError);
        Require(!removeError, "Could not delete snapshot race fixture.");
        // deletedAfterRead: ディスク削除後にスナップショットを適用する対象。
        LamaPon::PlayerPrefs deletedAfterRead(path);
        LamaPon::Detail::LocalPersistenceDocuments::
            LoadPlayerPrefsSnapshot(deletedAfterRead, snapshot);
        // 削除後も準備済み値を使い、ディスクに空ファイルを作りません。
        Require(
            deletedAfterRead.GetString("snapshot") == "validated-value"
                && !std::filesystem::exists(path),
            "Deletion after strict read was mistaken for an empty account.");
    }

    // アカウント切替の保存分離と永続化オブジェクトの同一性を検証します。
    void TestTransactionalProfileSwitchAndStableObjects()
    {
        // root: トランザクション切替ケースの保存先。
        const auto root = CaseRoot("transactional-switch");
        // preferences: ゲストとアカウント間で再利用するPlayerPrefs。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        // saves: 同一インスタンスで再結合するセーブストア。
        LamaPon::SaveDataStore saves(root / L"Saves");
        // coordinator: ゲストとアカウントの保存先を切り替える調整器。
        Coordinator coordinator(preferences, saves, root);
        ConfigureCoordinator(coordinator);

        // preferencesAddress: 切替前のPlayerPrefsインスタンス位置。
        auto* const preferencesAddress = &preferences;
        // savesAddress: 切替前のSaveDataStoreインスタンス位置。
        auto* const savesAddress = &saves;
        // guestPreferencesPath: 復元対象のゲスト設定ファイル。
        const auto guestPreferencesPath = preferences.FilePath();
        // guestSaveDirectory: 復元対象のゲストセーブ先。
        const auto guestSaveDirectory = saves.Directory();

        // preparedA: アカウントA用に準備した切替トランザクション。
        auto preparedA = coordinator.PrepareAccount("player-A");
        // 準備済みトランザクションが有効な状態で確定できることを検証します。
        Require(
            preparedA.IsValid()
                && coordinator.CommitPrepared(std::move(preparedA)),
            "Could not commit account A.");
        // アカウントA切替後もオブジェクトを保ち、ゲストデータを分離します。
        Require(
            &preferences == preferencesAddress
                && &saves == savesAddress
                && preferences.FilePath() != guestPreferencesPath
                && saves.Directory() != guestSaveDirectory
                && preferences.GetString("owner").empty()
                && coordinator.IsAccountActive()
                && coordinator.Journal() != nullptr
                && !coordinator.ActiveAccountStorageKey().empty(),
            "Account A did not preserve object identity and isolation.");

        // アカウントA専用データを保存してローカル変更通知を検証します。
        preferences.SetString("owner", "account-A");
        preferences.Save();
        saves.SaveJson("slot-a", R"({"level":7})");
        // 保存操作が調整器へローカルコミット通知を送ったことを検証します。
        Require(
            coordinator.ConsumeLocalCommitSignal(),
            "Account local commits did not reach the coordinator.");

        // ゲストへ戻すと以前の保存先・値・オブジェクトを復元します。
        Require(
            coordinator.DetachToGuest() == DetachResult::SavedAccount
                && !coordinator.IsAccountActive()
                && preferences.FilePath() == guestPreferencesPath
                && saves.Directory() == guestSaveDirectory
                && preferences.GetString("owner") == "guest"
                && &preferences == preferencesAddress
                && &saves == savesAddress,
            "Detach did not restore the exact guest objects and state.");

        // preparedB: アカウントB用の切替トランザクション。
        auto preparedB = coordinator.PrepareAccount("player-B");
        // アカウントBがアカウントAの永続化データを読めないことを検証します。
        Require(
            coordinator.CommitPrepared(std::move(preparedB))
                && preferences.GetString("owner").empty()
                && !saves.HasSlot("slot-a"),
            "Account B observed account A persistence.");
        // アカウントBの保存先を解放します。
        static_cast<void>(coordinator.DetachToGuest());
    }

    // アクティブアカウント中は両永続化先の外部再結合が拒否されることを検証します。
    void TestActiveAccountOwnsBothPersistenceBindings()
    {
        // root: 保存先リースケースのルート。
        const auto root = CaseRoot("binding-lease");
        // preferences: アカウントに結合するPlayerPrefs。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        // saves: アカウントに結合するSaveDataStore。
        LamaPon::SaveDataStore saves(root / L"Saves");
        // coordinator: 永続化リースを所有する調整器。
        Coordinator coordinator(preferences, saves, root);
        ConfigureCoordinator(coordinator);
        // prepared: リース取得に使うアカウント切替トランザクション。
        auto prepared = coordinator.PrepareAccount("lease-player");
        // アカウントを有効化してリースを取得します。
        Require(
            coordinator.CommitPrepared(std::move(prepared)),
            "Could not activate binding-lease fixture account.");

        // accountPreferencesPath: リース中のPlayerPrefs保存先。
        const auto accountPreferencesPath = preferences.FilePath();
        // accountSaveDirectory: リース中のセーブ保存先。
        const auto accountSaveDirectory = saves.Directory();
        // profiles: ゲスト切替や取込経路を試すProfile管理器。
        const LamaPon::PersistenceProfiles profiles(
            root,
            "coordinator-game",
            "test");
        // 公開Rebindでアカウント用PlayerPrefsのリースを越えられないことを検証します。
        Require(
            Throws([&]
            {
                preferences.Rebind(root / L"EscapedPrefs.json");
            }),
            "Public PlayerPrefs::Rebind bypassed the account lease.");
        // SaveDataStoreの直接再結合も同じリースで拒否されます。
        saves.Rebind(root / L"EscapedSaves");
        // 拒否された再結合が両アカウント保存先を維持することを検証します。
        Require(
            preferences.FilePath() == accountPreferencesPath
                && saves.Directory() == accountSaveDirectory,
            "Public SaveDataStore::Rebind bypassed the account lease.");
        // ゲスト切替経由でもアクティブアカウントを部分再結合できません。
        Require(
            Throws([&]
            {
                profiles.RebindGuest(preferences, saves);
            })
                && preferences.FilePath() == accountPreferencesPath
                && saves.Directory() == accountSaveDirectory,
            "PersistenceProfiles created a half-rebound active account.");
        // importResult: リース中のゲスト取込結果。
        const auto importResult = profiles.ImportGuestToAccount(
            preferences,
            saves,
            "lease-import-player");
        // ゲスト取込が失敗し、どちらの保存先も維持することを検証します。
        Require(
            importResult.status
                    == LamaPon::GuestPersistenceImportStatus::Failed
                && importResult.error
                    == "Persistence binding is owned by online persistence."
                && preferences.FilePath() == accountPreferencesPath
                && saves.Directory() == accountSaveDirectory,
            "Guest import bypassed the active account lease.");

        // remoteSource: 外部置換するPlayerPrefsデータの生成元。
        LamaPon::PlayerPrefs remoteSource(root / L"RemoteSource.json");
        remoteSource.Load();
        remoteSource.SetString("remote", "replacement");
        // remoteText: 置換するPlayerPrefsのJSON表現。
        const auto remoteText = remoteSource.SerializeToJson();
        // remoteBytes: ApplyPlayerPrefsへ渡す置換データ。
        const std::vector<std::uint8_t> remoteBytes(
            remoteText.begin(),
            remoteText.end());
        LamaPon::Detail::LocalPersistenceDocuments::ApplyPlayerPrefs(
            preferences,
            remoteBytes);
        // 外部置換後もアカウント保存先のリースが残ることを検証します。
        Require(
            preferences.GetString("remote") == "replacement"
                && Throws([&]
                {
                    preferences.Rebind(root / L"EscapedAfterApply.json");
                }),
            "PlayerPrefs replacement state dropped the account lease.");

        // ゲスト切離しがアカウントリースを解放します。
        Require(
            coordinator.DetachToGuest() == DetachResult::SavedAccount,
            "Binding-lease fixture did not detach cleanly.");
        // reboundPreferences: リース解放後に許可されるPlayerPrefs先。
        const auto reboundPreferences = root / L"ReboundPrefs.json";
        // reboundSaves: リース解放後に許可されるセーブ先。
        const auto reboundSaves = root / L"ReboundSaves";
        preferences.Rebind(reboundPreferences);
        saves.Rebind(reboundSaves);
        // 切離し後は両ストアの再結合先を変更できます。
        Require(
            preferences.FilePath() == reboundPreferences
                && saves.Directory() == reboundSaves,
            "Guest detach did not release both persistence leases.");
    }

    // 同じアカウントを二つの調整器が同時に有効化できないことを検証します。
    void TestAccountProfileSessionLeaseRejectsSecondActivation()
    {
        // root: セッションリース競合ケースの保存先。
        const auto root = CaseRoot("profile-session-lease");
        // firstPreferences: 先にリースを取得するPlayerPrefs。
        LamaPon::PlayerPrefs firstPreferences(root / L"PlayerPrefs.json");
        firstPreferences.Load();
        // firstSaves: 先にリースを取得するセーブストア。
        LamaPon::SaveDataStore firstSaves(root / L"Saves");
        // first: 最初にアカウントを有効化する調整器。
        Coordinator first(firstPreferences, firstSaves, root);
        ConfigureCoordinator(first);
        // firstPrepared: 最初のアカウント有効化トランザクション。
        auto firstPrepared = first.PrepareAccount("shared-account");
        // 最初の調整器がセッションリースを取得します。
        Require(
            first.CommitPrepared(std::move(firstPrepared)),
            "First account activation did not acquire its session lease.");

        // secondPreferences: 同じ保存先を狙う二つ目のPlayerPrefs。
        LamaPon::PlayerPrefs secondPreferences(root / L"PlayerPrefs.json");
        secondPreferences.Load();
        // secondSaves: 同じ保存先を狙う二つ目のセーブストア。
        LamaPon::SaveDataStore secondSaves(root / L"Saves");
        // second: 同じアカウントを競合して有効化する調整器。
        Coordinator second(secondPreferences, secondSaves, root);
        ConfigureCoordinator(second);
        // 先行リース中の二つ目の準備が拒否されることを検証します。
        Require(
            Throws([&]
            {
                (void)second.PrepareAccount("shared-account");
            })
                && !second.IsAccountActive(),
            "A second coordinator activated an account with an active lease.");

        // 最初の切離しでセッションリースが解放されます。
        Require(
            first.DetachToGuest() == DetachResult::SavedAccount,
            "First account did not release its session lease on detach.");
        // secondPrepared: リース解放後に再試行するトランザクション。
        auto secondPrepared = second.PrepareAccount("shared-account");
        // リース解放後は二つ目の調整器が有効化できます。
        Require(
            second.CommitPrepared(std::move(secondPrepared))
                && second.IsAccountActive(),
            "Account activation did not recover after the first lease released.");
        // 二つ目の調整器もテスト終了前に切り離します。
        static_cast<void>(second.DetachToGuest());
    }

    // 準備後にゲスト保存先が変わった場合はトランザクションを確定できないことを検証します。
    void TestPreparedCommitRejectsChangedGuestBinding()
    {
        // root: ゲスト保存先変更ケースの保存先。
        const auto root = CaseRoot("prepared-binding-change");
        // preferences: ゲストPlayerPrefsストア。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        // saves: ゲストセーブストア。
        LamaPon::SaveDataStore saves(root / L"Saves");
        // coordinator: 準備済み切替を管理する調整器。
        Coordinator coordinator(preferences, saves, root);
        ConfigureCoordinator(coordinator);

        // prepared: ゲスト結合情報を記録した切替トランザクション。
        auto prepared = coordinator.PrepareAccount("prepared-player");
        // changedPreferences: 準備後に差し替えるPlayerPrefs保存先。
        const auto changedPreferences = root / L"ChangedPrefs.json";
        // changedSaves: 準備後に差し替えるセーブ保存先。
        const auto changedSaves = root / L"ChangedSaves";
        preferences.Rebind(changedPreferences);
        saves.Rebind(changedSaves);
        // 古いゲスト結合を基にした切替が拒否されることを検証します。
        Require(
            !coordinator.CommitPrepared(std::move(prepared))
                && !coordinator.IsAccountActive()
                && preferences.FilePath() == changedPreferences
                && saves.Directory() == changedSaves,
            "Prepared transaction committed after its guest binding changed.");
    }

    // 名前空間変更後は古い準備済みトランザクションを確定できないことを検証します。
    void TestStalePreparedTransactionCannotCommit()
    {
        // root: 準備済みトランザクション失効ケースの保存先。
        const auto root = CaseRoot("stale-transaction");
        // preferences: ゲストPlayerPrefsストア。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        // saves: ゲストセーブストア。
        LamaPon::SaveDataStore saves(root / L"Saves");
        // coordinator: 名前空間世代を管理する調整器。
        Coordinator coordinator(preferences, saves, root);
        ConfigureCoordinator(coordinator);

        // guestPath: 変更前のゲスト保存先。
        const auto guestPath = preferences.FilePath();
        // prepared: 旧名前空間で準備した切替トランザクション。
        auto prepared = coordinator.PrepareAccount("player-stale");
        // 名前空間変更で準備済みトランザクションを失効させます。
        coordinator.ConfigureNamespace(
            "coordinator-game",
            "other-environment",
            "https://online.example.test/tenant/",
            false);
        // 失効した切替が拒否されゲスト保存先を保つことを検証します。
        Require(
            !coordinator.CommitPrepared(std::move(prepared))
                && preferences.FilePath() == guestPath
                && !coordinator.IsAccountActive(),
            "A stale prepared account changed the active profile.");
    }

    // 壊れたアカウント文書を空データとして扱わず、ゲストを維持することを検証します。
    void TestCorruptAccountDocumentsFailClosed()
    {
        {
            // root: PlayerPrefs破損ケースの保存先。
            const auto root = CaseRoot("corrupt-preferences");
            // preferences: ゲストPlayerPrefsストア。
            LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
            preferences.Load();
            preferences.SetString("owner", "guest");
            // saves: 同じアカウントに結合するセーブストア。
            LamaPon::SaveDataStore saves(root / L"Saves");
            // coordinator: アカウント切替を試す調整器。
            Coordinator coordinator(preferences, saves, root);
            ConfigureCoordinator(coordinator);
            // profiles: アカウント文書の保存先を解決する管理器。
            const LamaPon::PersistenceProfiles profiles(
                root,
                "coordinator-game",
                "test");
            // account: 破損文書を書き込むアカウント保存先。
            const auto account = profiles.Account("player-corrupt-prefs");
            WriteText(account.playerPrefsFile, "{not-json");

            // 壊れたPlayerPrefsで準備が失敗し、ゲスト値が残ることを検証します。
            Require(
                Throws([&]
                {
                    static_cast<void>(coordinator.PrepareAccount(
                        "player-corrupt-prefs"));
                })
                    && !coordinator.IsAccountActive()
                    && preferences.GetString("owner") == "guest",
                "Corrupt account PlayerPrefs replaced the guest profile.");
        }

        {
            // root: セーブデータ破損ケースの保存先。
            const auto root = CaseRoot("corrupt-save");
            // preferences: ゲストPlayerPrefsストア。
            LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
            preferences.Load();
            // saves: ゲストセーブストア。
            LamaPon::SaveDataStore saves(root / L"Saves");
            // coordinator: アカウント切替を試す調整器。
            Coordinator coordinator(preferences, saves, root);
            ConfigureCoordinator(coordinator);
            // profiles: アカウント文書の保存先を解決する管理器。
            const LamaPon::PersistenceProfiles profiles(
                root,
                "coordinator-game",
                "test");
            // account: 不正なスロット文書を書き込むアカウント保存先。
            const auto account = profiles.Account("player-corrupt-save");
            WriteText(
                account.saveDataDirectory / L"slot.save.json",
                R"({"format":"LamaPonSaveData","version":1,"slot":"other","data":{}})");

            // 不正スロットで準備が失敗し、両ゲスト保存先が残ることを検証します。
            Require(
                Throws([&]
                {
                    static_cast<void>(coordinator.PrepareAccount(
                        "player-corrupt-save"));
                })
                    && !coordinator.IsAccountActive()
                    && preferences.FilePath() == root / L"PlayerPrefs.json"
                    && saves.Directory() == root / L"Saves",
                "Corrupt account SaveData was mistaken for an empty profile.");
        }
    }

    // アカウント保存失敗時にメモリを隔離し、明示的な復旧で保存できることを検証します。
    void TestFailedAccountSaveIsQuarantinedAndRetried()
    {
        // root: 保存隔離と復旧ケースのルート。
        const auto root = CaseRoot("quarantine");
        // preferences: ゲストとアカウントで再利用するPlayerPrefs。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        preferences.Save();
        // saves: ゲストとアカウントで再利用するセーブストア。
        LamaPon::SaveDataStore saves(root / L"Saves");
        // coordinator: 保存隔離と復旧状態を管理する調整器。
        Coordinator coordinator(preferences, saves, root);
        ConfigureCoordinator(coordinator);

        // prepared: 隔離対象アカウントの切替トランザクション。
        auto prepared = coordinator.PrepareAccount("player-quarantine");
        // 隔離テスト用アカウントを有効化します。
        Require(
            coordinator.CommitPrepared(std::move(prepared)),
            "Could not activate quarantine fixture account.");
        preferences.SetString("unsaved", "memory-value");

        // accountPath: 書込ロックで保存を失敗させるアカウント設定先。
        const auto accountPath = preferences.FilePath();
        std::filesystem::create_directories(accountPath.parent_path());
        WriteText(
            accountPath,
            R"({"format":"LamaPonPlayerPrefs","version":1,"values":{}})");
        // lock: アカウント設定ファイルへの書き込みを止めるWindowsハンドル。
        FileHandle lock;
        lock.value = CreateFileW(
            accountPath.c_str(),
            GENERIC_READ,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        // アカウント保存失敗後にゲストへ戻して隔離状態を維持します。
        Require(
            lock.value != INVALID_HANDLE_VALUE,
            "Could not lock the account PlayerPrefs fixture.");

        // 隔離データが未復旧の間は別アカウントの準備を拒否します。
        Require(
            coordinator.DetachToGuest()
                    == DetachResult::QuarantinedAccount
                && coordinator.HasQuarantinedAccount()
                && preferences.GetString("owner") == "guest"
                && preferences.FilePath() == root / L"PlayerPrefs.json",
            "A failed account Save did not restore guest and quarantine memory.");
        Require(
            Throws([&]
            {
                static_cast<void>(
                    coordinator.PrepareAccount("another-player"));
            }),
            "A second account replaced quarantined unsaved memory.");

        // peerPreferences: 別調整器が参照するゲストPlayerPrefs。
        LamaPon::PlayerPrefs peerPreferences(root / L"PlayerPrefs.json");
        peerPreferences.Load();
        // peerSaves: 別調整器が参照するゲストセーブストア。
        LamaPon::SaveDataStore peerSaves(root / L"Saves");
        // peer: 隔離中のアカウントを再取得できないことを試す調整器。
        Coordinator peer(
            peerPreferences,
            peerSaves,
            root);
        ConfigureCoordinator(peer);
        // 隔離されたアカウントのプロセス内リースが維持されることを検証します。
        Require(
            Throws([&]
            {
                static_cast<void>(
                    peer.PrepareAccount("player-quarantine"));
            }),
            "A quarantined account released its process lifetime lease early.");

        // recovery: 明示的な再保存に使う復旧版。
        const auto recovery = coordinator.RecoveryStatus();
        lock.Close();
        // 書込ロックを解放して指定版のメモリ状態を復旧します。
        Require(
            recovery.revision != 0
                && coordinator.RestorePendingRecovery(recovery.revision)
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Succeeded
                && !coordinator.HasQuarantinedAccount(),
            "Explicit Restore reported a completed memory Save as stale.");

        // verification: 復旧内容をディスクから読み直すPlayerPrefs。
        LamaPon::PlayerPrefs verification(accountPath);
        verification.Load();
        // 復旧したメモリ値が永続化されたことを検証します。
        Require(
            verification.GetString("unsaved") == "memory-value",
            "Quarantined memory was not durably recovered.");
        // peerPrepared: 復旧後に別調整器で取得するアカウント切替。
        auto peerPrepared = peer.PrepareAccount("player-quarantine");
        // 復旧により別調整器がアカウントを準備できるようになります。
        Require(
            peerPrepared.IsValid(),
            "Recovered quarantine did not release the account lease.");
    }

    // 切離し前に基準ファイルのない削除意図を永続化することを検証します。
    void TestImmediateDetachCheckpointsBaselineLessDelete()
    {
        // CTestの深いbuild先でもMAX_PATH内に収まるケース名を使います。
        // root: 切離し時のチェックポイントケース保存先。
        const auto root = CaseRoot("detach-cp");
        // profiles: アカウント文書の保存先を解決する管理器。
        const LamaPon::PersistenceProfiles profiles(
            root,
            "coordinator-game",
            "test");
        // account: 事前作成するセーブデータのアカウント保存先。
        const auto account = profiles.Account("checkpoint-player");
        {
            // seed: チェックポイント前にスロットを作る一時ストア。
            LamaPon::SaveDataStore seed(account.saveDataDirectory);
            seed.SaveJson("checkpoint-slot", R"({"delete":true})");
        }

        // preferences: ゲストPlayerPrefsストア。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        // saves: ゲストセーブストア。
        LamaPon::SaveDataStore saves(root / L"Saves");
        // coordinator: 切離し時のクラウドチェックポイントを実行する調整器。
        Coordinator coordinator(preferences, saves, root);
        ConfigureCloudCoordinator(coordinator);

        // prepared: チェックポイント対象のアカウント切替。
        auto prepared = coordinator.PrepareAccount(
            "checkpoint-player",
            "checkpoint-access-token");
        Require(
            coordinator.CommitPrepared(std::move(prepared)),
            "Could not activate the detach checkpoint fixture.");
        // journalLock: WAL更新を拒否させるジャーナルのWindowsハンドル。
        FileHandle journalLock;
        // journalLockPath: CloudSaveJournalの排他ロック先。
        const auto journalLockPath = coordinator.Journal()->FilePath()
            .parent_path() / L"CloudSaveJournal.lock";
        journalLock.value = CreateFileW(
            journalLockPath.c_str(),
            GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES,
            0u,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        // WALを保存できない削除ではローカルセーブを先に消しません。
        Require(
            journalLock.value != INVALID_HANDLE_VALUE
                && !saves.DeleteSlot("checkpoint-slot")
                && LamaPon::Detail::LocalPersistenceDocuments::ReadSaveData(
                    saves,
                    "checkpoint-slot").state
                    == LamaPon::Detail::LocalPersistenceDocumentState::Loaded,
            "Local delete advanced before its intent WAL was durable.");
        // WAL更新を妨げていたハンドルを解放します。
        journalLock.Close();

        // targetLock: スロット置換を拒否させる既存ファイルのハンドル。
        FileHandle targetLock;
        // targetPath: 削除意図を反映するセーブスロット。
        const auto targetPath = saves.SlotPath("checkpoint-slot");
        targetLock.value = CreateFileW(
            targetPath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        // スロットロック取得後に削除失敗とWAL意図の保持を検証します。
        Require(
            targetLock.value != INVALID_HANDLE_VALUE
                && Throws([&]
                {
                    static_cast<void>(saves.DeleteSlot("checkpoint-slot"));
                })
                && coordinator.Journal()->HasLocalDeleteIntent(
                    LamaPon::CloudSaveResource::SaveSlot(
                        "checkpoint-slot")),
            "A failed local delete did not leave a recoverable WAL intent.");
        // スロット書込みを妨げていたハンドルを解放します。
        targetLock.Close();
        // 中断したローカル削除意図の再照合を依頼します。
        coordinator.Synchronizer()->RequestReconcile();
        // ジャーナルへ意図を書き込める状態でローカル削除を確定します。
        Require(
            !coordinator.Journal()->HasLocalDeleteIntent(
                LamaPon::CloudSaveResource::SaveSlot("checkpoint-slot"))
                && LamaPon::Detail::LocalPersistenceDocuments::ReadSaveData(
                    saves,
                    "checkpoint-slot").state
                    == LamaPon::Detail::LocalPersistenceDocumentState::Loaded,
            "A loaded local document did not clear an aborted delete intent.");
        // WAL記録済みのセーブ削除を実行します。
        Require(
            saves.DeleteSlot("checkpoint-slot"),
            "Could not commit the pre-WAL local delete.");

        // チェックポイント保存後に正常切離しできたことを確認します。
        Require(
            coordinator.DetachToGuest() == DetachResult::SavedAccount,
            "A durable detach checkpoint unexpectedly quarantined the account.");
        // reopened: 保存済み削除意図を再確認するアカウント切替。
        auto reopened = coordinator.PrepareAccount(
            "checkpoint-player",
            "checkpoint-access-token-2");
        // 切離し前に保存された削除意図が次回サインインにも残ることを検証します。
        Require(
            coordinator.CommitPrepared(std::move(reopened))
                && coordinator.Journal()
                && coordinator.Journal()->HasLocalDeleteIntent(
                    LamaPon::CloudSaveResource::SaveSlot(
                        "checkpoint-slot")),
            "Immediate sign-out lost a baseline-less local delete intent.");
        // 再検証後にアカウント保存先を解放します。
        static_cast<void>(coordinator.DetachToGuest());
    }

    // 切離しチェックポイント失敗時は復旧で破棄するまでアカウントリースを保持します。
    void TestFailedDetachCheckpointKeepsAccountLeaseUntilDiscard()
    {
        // CTestの深いbuild先でもMAX_PATH内に収まるケース名を使います。
        // root: チェックポイント失敗ケースの保存先。
        const auto root = CaseRoot("checkpoint-q");
        // profiles: アカウント文書の保存先を解決する管理器。
        const LamaPon::PersistenceProfiles profiles(
            root,
            "coordinator-game",
            "test");
        // account: 事前作成するスロットの保存先。
        const auto account = profiles.Account("checkpoint-player");
        {
            // seed: 削除意図を作る初期セーブストア。
            LamaPon::SaveDataStore seed(account.saveDataDirectory);
            seed.SaveJson("checkpoint-slot", R"({"delete":true})");
        }

        // preferences: ゲストPlayerPrefsストア。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        // saves: アカウントとゲストで再利用するセーブストア。
        LamaPon::SaveDataStore saves(root / L"Saves");
        // coordinator: 切離しチェックポイントと復旧を管理する調整器。
        Coordinator coordinator(preferences, saves, root);
        ConfigureCloudCoordinator(coordinator);
        // prepared: チェックポイント失敗を起こすアカウント切替。
        auto prepared = coordinator.PrepareAccount(
            "checkpoint-player",
            "checkpoint-access");
        // 失敗ケース用アカウントを有効化します。
        Require(
            coordinator.CommitPrepared(std::move(prepared)),
            "Could not activate the failed checkpoint fixture.");
        {
            // observerを抑止して旧経路の削除を再現します。
            // チェックポイント失敗自体が隔離されることを検証します。
            LamaPon::Detail::ScopedLocalPersistenceObserverSuppression suppress;
            // チェックポイント前にローカル削除だけを実行します。
            Require(
                saves.DeleteSlot("checkpoint-slot"),
                "Could not prepare the failed checkpoint deletion edge.");
        }

        // lockPath: チェックポイント更新を妨げるジャーナルロック先。
        const auto lockPath = coordinator.Journal()->FilePath()
            .parent_path() / L"CloudSaveJournal.lock";
        // lock: 復旧記録の書込みを失敗させるWindowsハンドル。
        FileHandle lock;
        lock.value = CreateFileW(
            lockPath.c_str(),
            GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES,
            0u,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        // ジャーナルロックを取得できたことを検証します。
        Require(
            lock.value != INVALID_HANDLE_VALUE,
            "Could not hold the detach checkpoint journal lock.");
        // チェックポイント失敗が隔離と復旧待ち状態になることを検証します。
        Require(
            coordinator.DetachToGuest()
                    == DetachResult::QuarantinedAccount
                && coordinator.HasPendingRecovery(),
            "A failed detach checkpoint was not quarantined.");

        // peerPreferences: 別調整器が使うゲストPlayerPrefs。
        LamaPon::PlayerPrefs peerPreferences(root / L"PlayerPrefs.json");
        peerPreferences.Load();
        // peerSaves: 別調整器が使うゲストセーブストア。
        LamaPon::SaveDataStore peerSaves(root / L"Saves");
        // peer: 隔離中のアカウントを再取得できないか試す調整器。
        Coordinator peer(peerPreferences, peerSaves, root);
        ConfigureCoordinator(peer);
        // 隔離中は同じアカウントのリースを他調整器へ渡しません。
        Require(
            Throws([&]
            {
                static_cast<void>(peer.PrepareAccount(
                    "checkpoint-player"));
            }),
            "A checkpoint-failed quarantine released its account lease.");

        // recovery: 復旧の再試行に使う記録版。
        const auto recovery = coordinator.RecoveryStatus();
        // ロック中の復旧が未完了として残ることを検証します。
        Require(
            recovery.revision != 0
                && coordinator.RestorePendingRecovery(recovery.revision)
                    != LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Succeeded
                && coordinator.HasPendingRecovery(),
            "A locked checkpoint was incorrectly reported as recovered.");
        // 復旧先を塞いでいたジャーナルロックを解放します。
        lock.Close();
        // 同じ復旧版を再試行して隔離状態を解除します。
        Require(
            coordinator.RestorePendingRecovery(recovery.revision)
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Succeeded
                && !coordinator.HasPendingRecovery(),
            "A durable checkpoint plan could not be retried after lock recovery.");
        // peerPrepared: 復旧後に別調整器が準備するアカウント切替。
        auto peerPrepared = peer.PrepareAccount("checkpoint-player");
        // 削除意図を維持したまま別調整器がリースを取得できることを検証します。
        Require(
            peerPrepared.IsValid()
                && peer.CommitPrepared(std::move(peerPrepared))
                && peer.Journal()
                && peer.Journal()->HasLocalDeleteIntent(
                    LamaPon::CloudSaveResource::SaveSlot(
                        "checkpoint-slot")),
            "Checkpoint recovery did not preserve intent or release the account lease.");
        // 別調整器のテスト用アカウントを切り離します。
        static_cast<void>(peer.DetachToGuest());
    }

    // チェックポイント計画を判定できない場合は自動復旧せず、明示破棄まで隔離します。
    void TestUndeterminedDetachCheckpointRemainsFailClosed()
    {
        // root: 判定不能なチェックポイントケースの保存先。
        const auto root = CaseRoot("checkpoint-scan-failure");
        // profiles: アカウント文書の保存先を解決する管理器。
        const LamaPon::PersistenceProfiles profiles(
            root,
            "coordinator-game",
            "test");
        // account: 破損スロットを作るアカウント保存先。
        const auto account = profiles.Account("checkpoint-scan-player");
        {
            // seed: 判定不能にする元のスロットを作成するストア。
            LamaPon::SaveDataStore seed(account.saveDataDirectory);
            seed.SaveJson("scan-slot", R"({"value":1})");
        }

        // preferences: ゲストPlayerPrefsストア。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        // saves: ゲストとアカウントで再利用するセーブストア。
        LamaPon::SaveDataStore saves(root / L"Saves");
        // coordinator: チェックポイント判定と復旧を管理する調整器。
        Coordinator coordinator(preferences, saves, root);
        ConfigureCloudCoordinator(coordinator);
        // prepared: 破損後のチェックポイントを試すアカウント切替。
        auto prepared = coordinator.PrepareAccount(
            "checkpoint-scan-player",
            "checkpoint-access");
        // 判定不能ケースのアカウントを有効化します。
        Require(
            coordinator.CommitPrepared(std::move(prepared)),
            "Could not activate the checkpoint scan fixture.");

        // チェックポイント対象のローカルスロットを破損させます。
        WriteText(saves.SlotPath("scan-slot"), "{not-json");
        // 判定不能なチェックポイントは隔離して復旧待ちにします。
        Require(
            coordinator.DetachToGuest()
                    == DetachResult::QuarantinedAccount
                && coordinator.HasPendingRecovery(),
            "An undetermined detach checkpoint was not quarantined.");
        // recovery: 明示破棄に使う判定不能な復旧状態。
        const auto recovery = coordinator.RecoveryStatus();
        // 操作計画のない復旧にRestoreを提示しないことを検証します。
        Require(
            recovery.state
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryState::UnavailableSidecar
                && recovery.revision != 0,
            "An undetermined checkpoint incorrectly exposed Restore.");

        {
            // repaired: 破損スロットを置き換えるテスト用ストア。
            LamaPon::SaveDataStore repaired(account.saveDataDirectory);
            repaired.SaveJson("scan-slot", R"({"value":2})");
        }
        // フレーム更新では判定不能状態を自動解決しません。
        coordinator.EndFrame();
        // 自動復旧を拒否し、明示的な破棄だけが隔離を解除することを検証します。
        Require(
            coordinator.HasPendingRecovery()
                && coordinator.RestorePendingRecovery(recovery.revision)
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Failed,
            "A checkpoint with no determined operation plan auto-recovered.");
        // 明示破棄が成功して復旧待ち状態を解除することを検証します。
        Require(
            coordinator.DiscardPendingRecovery(recovery.revision)
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Succeeded
                && !coordinator.HasPendingRecovery(),
            "Explicit discard could not release an undetermined checkpoint.");
    }

    // 未変更PlayerPrefsの読込失敗は復旧隔離せず、後続アカウントを許可します。
    void TestCleanLoadFailureDoesNotPermanentlyBlockProfiles()
    {
        // root: 未変更読込失敗ケースの保存先。
        const auto root = CaseRoot("clean-load-failure");
        // preferences: ゲストとアカウントで再利用するPlayerPrefs。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        preferences.Save();
        // saves: ゲストとアカウントで再利用するセーブストア。
        LamaPon::SaveDataStore saves(root / L"Saves");
        // coordinator: 読込失敗後のプロファイル切替を管理する調整器。
        Coordinator coordinator(preferences, saves, root);
        ConfigureCoordinator(coordinator);

        // prepared: 読込失敗を起こすアカウント切替。
        auto prepared = coordinator.PrepareAccount("clean-failure-player");
        // アカウントを有効化し、保存済みデータを用意します。
        Require(
            coordinator.CommitPrepared(std::move(prepared)),
            "Could not activate clean load-failure account.");
        preferences.SetString("saved", "account-value");
        preferences.Save();
        WriteText(preferences.FilePath(), "{not-json");
        // 未変更状態の厳密読込失敗を記録します。
        Require(
            Throws([&]
            {
                preferences.Reload();
            })
                && preferences.HasLoadFailure()
                && !preferences.IsDirty(),
            "Clean PlayerPrefs reload failure fixture was invalid.");

        // 未変更読込失敗ではゲスト復帰と後続準備が隔離されません。
        Require(
            coordinator.DetachToGuest() == DetachResult::SavedAccount
                && !coordinator.HasPendingRecovery()
                && preferences.FilePath() == root / L"PlayerPrefs.json"
                && preferences.GetString("owner") == "guest",
            "Clean load failure created a permanent quarantine.");
        // 元の破損アカウントだけは再度拒否されます。
        Require(
            Throws([&]
            {
                static_cast<void>(
                    coordinator.PrepareAccount("clean-failure-player"));
            })
                && !coordinator.HasPendingRecovery(),
            "Corrupt original account was not rejected independently.");
        // other: 読込失敗後も準備可能な別アカウント。
        auto other = coordinator.PrepareAccount("clean-failure-other");
        // 別アカウントの有効化が阻害されないことを検証します。
        Require(
            coordinator.CommitPrepared(std::move(other)),
            "Clean load failure blocked every later account.");
        // 後続アカウントを切り離してテスト状態を戻します。
        static_cast<void>(coordinator.DetachToGuest());
    }

    // 変更済みPlayerPrefsの読込失敗を厳密sidecarへ退避し、再起動後も復旧検出します。
    void TestDirtyLoadFailureUsesRecoverableStrictSidecar()
    {
        // root: 変更済み読込失敗ケースの保存先。
        const auto root = CaseRoot("dirty-load-failure-recovery");
        // profiles: アカウントsidecarの保存先を解決する管理器。
        const LamaPon::PersistenceProfiles profiles(
            root,
            "coordinator-game",
            "test");
        // account: 復旧sidecarを置くアカウント保存先。
        const auto account = profiles.Account("recovery-player");
        // recoverySidecar: 永続化された復旧文書の最終パス。
        std::filesystem::path recoverySidecar;

        {
            // preferences: ゲストとアカウントで再利用するPlayerPrefs。
            LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
            preferences.Load();
            preferences.SetString("owner", "guest");
            preferences.Save();
            // saves: ゲストとアカウントで再利用するセーブストア。
            LamaPon::SaveDataStore saves(root / L"Saves");
            // coordinator: 失敗時のメモリ隔離と復旧を管理する調整器。
            Coordinator coordinator(preferences, saves, root);
            ConfigureCoordinator(coordinator);
            // prepared: 復旧対象アカウントの切替トランザクション。
            auto prepared = coordinator.PrepareAccount("recovery-player");
            // 変更済みデータを作り、ディスク破損後の復旧元にします。
            Require(
                coordinator.CommitPrepared(std::move(prepared)),
                "Could not activate dirty load-failure account.");
            preferences.SetString("saved", "before-corruption");
            preferences.Save();
            preferences.SetString("unsaved", "memory-snapshot");
            WriteText(account.playerPrefsFile, "{not-json");
            // dirtyな再読込失敗後もメモリ状態を保持します。
            Require(
                Throws([&]
                {
                    preferences.Reload();
                })
                    && preferences.HasLoadFailure()
                    && preferences.IsDirty()
                    && preferences.GetString("unsaved")
                        == "memory-snapshot",
                "Dirty PlayerPrefs reload failure lost its memory state.");

            // 同一内容の外部sidecarも所有物とせず、memory quarantineを維持します。
            // blockedRecoverySidecar: 外部writerが先に作成した復旧ファイル。
            const auto blockedRecoverySidecar =
                account.rootDirectory / L"Recovery.prefs";
            // externalSnapshot: ディスク破損後も保持するPlayerPrefs内容。
            const auto externalSnapshot = preferences.SerializeToJson();
            WriteText(blockedRecoverySidecar, externalSnapshot);
            // 外部sidecarを上書きせず、ゲスト復帰後もメモリを隔離します。
            Require(
                coordinator.DetachToGuest()
                        == DetachResult::QuarantinedAccount
                    && coordinator.RecoveryState()
                        == LamaPon::Detail::
                            OnlinePersistenceRecoveryState::UnavailableSidecar
                    && coordinator.HasQuarantinedAccount()
                    && coordinator.HasPendingRecovery()
                    && ReadText(blockedRecoverySidecar)
                        == externalSnapshot
                    && preferences.FilePath()
                        == root / L"PlayerPrefs.json"
                    && preferences.GetString("owner") == "guest",
                "Failed sidecar publish discarded dirty account memory.");

            // removeError: 外部sidecar削除時のファイルシステムエラー。
            std::error_code removeError;
            std::filesystem::remove(
                blockedRecoverySidecar,
                removeError);
            // 対象sidecarを削除できたことを確認します。
            Require(
                !removeError,
                "Could not unblock recovery sidecar target.");
            LamaPon::Detail::SetOnlinePersistenceRecoveryTestFailPoint(
                LamaPon::Detail::OnlinePersistenceRecoveryTestFailPoint::
                    AfterSidecarPublishBeforeVerification);
            // 曖昧なpublishはメモリを破棄せず再検証へ回します。
            coordinator.EndFrame();
            // 再検証が終わるまで隔離状態とsidecarを維持します。
            Require(
                coordinator.RecoveryState()
                        == LamaPon::Detail::
                            OnlinePersistenceRecoveryState::MemorySnapshot
                    && coordinator.HasQuarantinedAccount()
                    && std::filesystem::exists(blockedRecoverySidecar),
                "An ambiguous sidecar publish discarded memory before strict verification.");
            // recoveryDeadline: sidecar再検証の上限時刻。
            const auto recoveryDeadline =
                std::chrono::steady_clock::now()
                + std::chrono::seconds(10);
            // durableなsidecarになるか期限切れまでフレーム更新します。
            while (coordinator.RecoveryState()
                    != LamaPon::Detail::
                        OnlinePersistenceRecoveryState::DurableSidecar
                && std::chrono::steady_clock::now()
                    < recoveryDeadline)
            {
                coordinator.EndFrame();
                std::this_thread::yield();
            }
            // recoverySidecar: 検証済みsidecarの実ファイルパス。
            recoverySidecar = coordinator.RecoverySidecarPath();
            // memory quarantineが解除され、復旧sidecarが永続化されたことを検証します。
            Require(
                coordinator.RecoveryState()
                        == LamaPon::Detail::
                            OnlinePersistenceRecoveryState::DurableSidecar
                    && !coordinator.HasQuarantinedAccount()
                    && coordinator.HasPendingRecovery()
                    && !recoverySidecar.empty()
                    && std::filesystem::is_regular_file(recoverySidecar),
                "Dirty memory was not moved to a durable recovery sidecar.");

            // strictSidecar: 復旧ファイルを通常の厳密読込で検証するストア。
            LamaPon::PlayerPrefs strictSidecar(recoverySidecar);
            strictSidecar.Load();
            // sidecarに未保存のメモリ値が含まれることを検証します。
            Require(
                strictSidecar.GetString("unsaved") == "memory-snapshot",
                "Recovery sidecar did not contain the full memory snapshot.");
        }

        // 再起動後の調整器もsidecarを検出し、通常有効化を拒否します。
        // preferences: 再起動後に読み込むゲストPlayerPrefs。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        // saves: 再起動後のゲストセーブストア。
        LamaPon::SaveDataStore saves(root / L"Saves");
        // restarted: sidecar検出を試す新しい調整器。
        Coordinator restarted(preferences, saves, root);
        ConfigureCoordinator(restarted);
        // 通常準備ではdurable sidecarを見落とさないことを検証します。
        Require(
            Throws([&]
            {
                static_cast<void>(
                    restarted.PrepareAccount("recovery-player"));
            })
                && restarted.RecoveryState()
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryState::DurableSidecar
                && restarted.RecoverySidecarPath() == recoverySidecar,
            "Restarted coordinator ignored an account recovery sidecar.");

        // sidecarLock: 復旧ファイルの削除を止めるWindowsハンドル。
        FileHandle sidecarLock;
        sidecarLock.value = CreateFileW(
            recoverySidecar.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        // sidecarLockを取得できたことを確認します。
        Require(
            sidecarLock.value != INVALID_HANDLE_VALUE,
            "Could not lock recovery sidecar fixture.");
        // ロック中の削除失敗を復旧待ちとして保持します。
        Require(
            !restarted.RestorePendingRecovery()
                && restarted.HasPendingRecovery(),
            "Sidecar delete failure was not retained for retry.");
        // appliedBeforeDelete: sidecar削除前に適用されたアカウント文書。
        LamaPon::PlayerPrefs appliedBeforeDelete(account.playerPrefsFile);
        appliedBeforeDelete.Load();
        // sidecar削除前に復旧内容が元の文書へ適用済みであることを検証します。
        Require(
            appliedBeforeDelete.GetString("unsaved")
                == "memory-snapshot",
            "Recovery did not apply the original before sidecar deletion.");

        // 復旧sidecar削除を妨げていたハンドルを解放します。
        sidecarLock.Close();
        // 削除失敗後の復旧を再試行してsidecarを削除します。
        Require(
            restarted.RestorePendingRecovery()
                && !restarted.HasPendingRecovery()
                && !std::filesystem::exists(recoverySidecar),
            "Recovery sidecar could not be retried after delete failure.");
        // restored: 復旧済みアカウントの再有効化トランザクション。
        auto restored = restarted.PrepareAccount("recovery-player");
        // 復旧後にアカウント文書へ値が戻ることを検証します。
        Require(
            restarted.CommitPrepared(std::move(restored))
                && preferences.GetString("unsaved")
                    == "memory-snapshot",
            "Explicit recovery did not restore the account profile.");
        // 復旧済みアカウントを切り離します。
        static_cast<void>(restarted.DetachToGuest());
    }

    // 明示破棄は復旧sidecarだけを削除し、元のアカウント文書を維持します。
    void TestExplicitRecoveryDiscardPreservesOriginal()
    {
        // root: 明示破棄ケースの保存先。
        const auto root = CaseRoot("recovery-discard");
        // profiles: 元文書とsidecarの保存先を解決する管理器。
        const LamaPon::PersistenceProfiles profiles(
            root,
            "coordinator-game",
            "test");
        // account: 元文書とsidecarを持つ対象アカウント。
        const auto account = profiles.Account("discard-player");
        // original: sidecar破棄後も保持する元PlayerPrefs文書。
        LamaPon::PlayerPrefs original(account.playerPrefsFile);
        original.Load();
        original.SetString("owner", "original-disk");
        original.Save();
        // sidecarPath: 明示破棄の対象となる復旧sidecar。
        const auto sidecarPath =
            account.rootDirectory / L"Recovery.prefs";
        // sidecar: 削除対象sidecarへ復旧用の異なる値を書きます。
        LamaPon::PlayerPrefs sidecar(sidecarPath);
        sidecar.Load();
        sidecar.SetString("owner", "discarded-snapshot");
        sidecar.Save();

        // preferences: ゲストPlayerPrefsストア。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        // saves: ゲストセーブストア。
        LamaPon::SaveDataStore saves(root / L"Saves");
        // coordinator: sidecar検出と明示破棄を管理する調整器。
        Coordinator coordinator(preferences, saves, root);
        ConfigureCoordinator(coordinator);
        // prepareRejected: sidecar検出で通常準備が拒否された状態。
        const bool prepareRejected = Throws([&]
        {
            static_cast<void>(
                coordinator.PrepareAccount("discard-player"));
        });
        // discardStatus: 最初のsidecar破棄に使う復旧版。
        const auto discardStatus = coordinator.RecoveryStatus();
        // discardResult: sidecarのみを削除する明示破棄の結果。
        const auto discardResult = coordinator.DiscardPendingRecovery(
            discardStatus.revision);
        // 元文書を保ってsidecarだけを削除したことを検証します。
        Require(
            prepareRejected
                && discardResult
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Succeeded
                && !coordinator.HasPendingRecovery()
                && !std::filesystem::exists(sidecarPath),
            "Explicit recovery discard did not remove only the sidecar.");
        // verification: 元のPlayerPrefs文書を再読込するストア。
        LamaPon::PlayerPrefs verification(account.playerPrefsFile);
        verification.Load();
        // 明示破棄で元文書が置換されないことを検証します。
        Require(
            verification.GetString("owner") == "original-disk",
            "Recovery discard overwrote the original account document.");

        // 別の破損sidecarを作り、破損状態の版管理を検証します。
        WriteText(sidecarPath, "{not-json");
        Require(
            Throws([&]
            {
                (void)coordinator.PrepareAccount("discard-player");
            })
                && coordinator.RecoveryState()
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryState::UnavailableSidecar,
            "Corrupt recovery sidecar was not detected.");
        // corrupt: 最初に観測した破損sidecarの版。
        const auto corrupt = coordinator.RecoveryStatus();
        WriteText(sidecarPath, "{different-corrupt-json");
        // changedCorrupt: 外部変更後のsidecar版。
        const auto changedCorrupt = coordinator.RecoveryStatus();
        // 古い版の破棄を拒否し、最新観測版だけを削除できることを検証します。
        Require(
            changedCorrupt.revision != corrupt.revision
                && coordinator.DiscardPendingRecovery(corrupt.revision)
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Stale
                && coordinator.DiscardPendingRecovery(
                    changedCorrupt.revision)
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Succeeded
                && !std::filesystem::exists(sidecarPath),
            "A latest-revision explicit discard could not remove the observed replacement.");
        // 再度同一破損を作り、プロファイルリース下の破棄を検証します。
        WriteText(sidecarPath, "{not-json");
        Require(
            Throws([&]
            {
                (void)coordinator.PrepareAccount("discard-player");
            }),
            "A second corrupt recovery incident was not detected.");
        // restoredCorrupt: 再検出した破損sidecarの版。
        const auto restoredCorrupt = coordinator.RecoveryStatus();
        // 変更されていない破損sidecarを最新版として削除できることを検証します。
        Require(
            restoredCorrupt.revision != 0
                && coordinator.DiscardPendingRecovery(
                    restoredCorrupt.revision)
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Succeeded
                && !std::filesystem::exists(sidecarPath),
            "Explicit discard could not remove an unchanged corrupt sidecar under its profile lease.");
    }

    // メモリ復旧と競合するsidecarを破棄した後、永続sidecarを再検出して破棄します。
    void TestMemoryRecoveryDiscardPromotesExistingSidecar()
    {
        // run(caseName: ケース名, externalSidecar: 外部sidecar有無) 競合状態を独立して検証します。
        const auto run = [](
            const std::string_view caseName,
            const bool externalSidecar)
        {
            // root: 競合ケース固有の保存先。
            const auto root = CaseRoot(caseName);
            // profiles: sidecarの保存先を解決する管理器。
            const LamaPon::PersistenceProfiles profiles(
                root,
                "coordinator-game",
                "test");
            // account: 競合するアカウント保存先。
            const auto account = profiles.Account("discard-memory-player");
            // sidecarPath: メモリ復旧と競合するsidecar。
            const auto sidecarPath =
                account.rootDirectory / L"Recovery.prefs";

            // preferences: ゲストPlayerPrefsストア。
            LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
            preferences.Load();
            // saves: ゲストセーブストア。
            LamaPon::SaveDataStore saves(root / L"Saves");
            // coordinator: メモリ隔離とsidecar復旧を管理する調整器。
            Coordinator coordinator(preferences, saves, root);
            ConfigureCoordinator(coordinator);
            // prepared: 競合対象アカウントの有効化トランザクション。
            auto prepared = coordinator.PrepareAccount(
                "discard-memory-player");
            // 競合ケース用アカウントを有効化します。
            Require(
                coordinator.CommitPrepared(std::move(prepared)),
                "Could not activate the memory discard fixture.");

            preferences.SetString("unsaved", "memory-snapshot");
            WriteText(account.playerPrefsFile, "{not-json");
            // 破損後もdirtyなメモリ状態が保たれることを検証します。
            Require(
                Throws([&] { preferences.Reload(); })
                    && preferences.HasLoadFailure()
                    && preferences.IsDirty(),
                "Could not create a dirty load-failure snapshot.");

            // 外部sidecarと曖昧publishの両方の競合元を作ります。
            if (externalSidecar)
            {
                // external: 先に作成される外部sidecarの書込ストア。
                LamaPon::PlayerPrefs external(sidecarPath);
                external.Load();
                external.SetString("owner", "external-sidecar");
                external.Save();
            }
            // 外部sidecarがない場合はpublish後検証を曖昧にします。
            else
            {
                LamaPon::Detail::SetOnlinePersistenceRecoveryTestFailPoint(
                    LamaPon::Detail::
                        OnlinePersistenceRecoveryTestFailPoint::
                            AfterSidecarPublishBeforeVerification);
            }

            // 競合したsidecarを残してアカウントを隔離します。
            Require(
                coordinator.DetachToGuest()
                        == DetachResult::QuarantinedAccount
                    && coordinator.HasPendingRecovery()
                    && std::filesystem::exists(sidecarPath),
                "Memory recovery collision was not quarantined.");
            // preserved: 最初の明示破棄で維持するsidecar内容。
            const auto preserved = ReadText(sidecarPath);
            // first: memory quarantine状態の最初の復旧版。
            const auto first = coordinator.RecoveryStatus();
            // 初回破棄は観測sidecarを置換も削除もしないことを検証します。
            Require(
                first.revision != 0
                    && coordinator.DiscardPendingRecovery(first.revision)
                        == LamaPon::Detail::
                            OnlinePersistenceRecoveryOperationResult::Succeeded
                    && coordinator.HasPendingRecovery()
                    && std::filesystem::exists(sidecarPath)
                    && ReadText(sidecarPath) == preserved,
                "First-stage discard removed or replaced the observed sidecar.");

            // promoted: sidecarを永続復旧状態へ昇格した後の版。
            const auto promoted = coordinator.RecoveryStatus();
            // 昇格後の明示破棄がsidecarと待機状態を削除することを検証します。
            Require(
                promoted.revision != first.revision
                    && promoted.state
                        == LamaPon::Detail::
                            OnlinePersistenceRecoveryState::DurableSidecar
                    && coordinator.DiscardPendingRecovery(promoted.revision)
                        == LamaPon::Detail::
                            OnlinePersistenceRecoveryOperationResult::Succeeded
                    && !coordinator.HasPendingRecovery()
                    && !std::filesystem::exists(sidecarPath),
                "Promoted recovery incident could not be discarded explicitly.");
        };

        // 既存外部sidecarとの競合を検証します。
        run("discard-memory-external", true);
        // 曖昧なpublishで生成したsidecarとの競合を検証します。
        run("discard-memory-published", false);
    }

    // sidecar検出後の解決は所有調整器だけに許可し、切離しまでアカウントリースを保ちます。
    void TestRecoverySidecarResolutionReacquiresProfileLease()
    {
        // root: 復旧リース競合ケースの保存先。
        const auto root = CaseRoot("recovery-sidecar-lease");
        // profiles: 対象アカウントの保存先を解決する管理器。
        const LamaPon::PersistenceProfiles profiles(
            root,
            "coordinator-game",
            "test");
        // account: 競合調整器が共有するアカウント保存先。
        const auto account = profiles.Account("recovery-race-player");
        // sidecarPath: 先に検出させる復旧文書。
        const auto sidecarPath = account.rootDirectory / L"Recovery.prefs";
        // sidecar: 復旧対象の所有者情報を持つ文書。
        LamaPon::PlayerPrefs sidecar(sidecarPath);
        sidecar.Load();
        sidecar.SetString("owner", "recovered-by-peer");
        sidecar.Save();

        // firstPreferences: sidecarを最初に検出するゲストストア。
        LamaPon::PlayerPrefs firstPreferences(root / L"PlayerPrefs.json");
        firstPreferences.Load();
        // firstSaves: 最初の調整器のゲストセーブストア。
        LamaPon::SaveDataStore firstSaves(root / L"Saves");
        // first: sidecarを検出し解決する調整器。
        Coordinator first(firstPreferences, firstSaves, root);
        ConfigureCoordinator(first);
        // 最初の調整器だけが検出済みsidecarの所有権を得ます。
        Require(
            Throws([&]
            {
                (void)first.PrepareAccount("recovery-race-player");
            })
                && first.HasPendingRecovery(),
            "First coordinator did not cache the detected sidecar.");

        // secondPreferences: 競合を試す二つ目のゲストストア。
        LamaPon::PlayerPrefs secondPreferences(root / L"PlayerPrefs.json");
        secondPreferences.Load();
        // secondSaves: 二つ目の調整器のゲストセーブストア。
        LamaPon::SaveDataStore secondSaves(root / L"Saves");
        // second: sidecarの所有リースを持たない競合調整器。
        Coordinator second(secondPreferences, secondSaves, root);
        ConfigureCoordinator(second);
        // 所有調整器のリース中はpeerが復旧状態を取得できません。
        Require(
            Throws([&]
            {
                (void)second.PrepareAccount("recovery-race-player");
            })
                && !second.HasPendingRecovery(),
            "A peer inspected recovery state while the detecting owner held its lease.");
        // sidecar検出者がリース下で復旧を完了できることを検証します。
        Require(
            first.RestorePendingRecovery(),
            "Detecting coordinator could not resolve the sidecar under its lease.");
        // prepared: 復旧済みアカウントの有効化トランザクション。
        auto prepared = first.PrepareAccount("recovery-race-player");
        // 復旧した所有者情報を有効なアカウントとして読み込みます。
        Require(
            first.CommitPrepared(std::move(prepared))
                && firstPreferences.GetString("owner")
                    == "recovered-by-peer",
            "Detecting coordinator did not activate the recovered profile.");

        // 最初の調整器が切り離すまではpeerの有効化を拒否します。
        Require(
            Throws([&]
            {
                (void)second.PrepareAccount("recovery-race-player");
            })
                && !second.IsAccountActive(),
            "A peer activated the recovered account before its owner detached.");
        // 所有調整器を切り離してアカウントリースを解放します。
        static_cast<void>(first.DetachToGuest());
        // secondPrepared: リース解放後に再試行するアカウント切替。
        auto secondPrepared = second.PrepareAccount("recovery-race-player");
        // 解放済みリースをpeerが引き継げることを検証します。
        Require(
            second.CommitPrepared(std::move(secondPrepared)),
            "Peer could not activate after recovery owner released its lease.");
        // peerのアカウントもテスト終了前に切り離します。
        static_cast<void>(second.DetachToGuest());
    }

    // 空・巨大・差し替え済み・消失sidecarを安全に明示破棄できることを検証します。
    void TestEmptyAndOversizedRecoverySidecarsCanBeDiscardedSafely()
    {
        // root: 破損sidecar検証ケースの保存先。
        const auto root = CaseRoot("recovery-corrupt-availability");
        // profiles: 対象sidecarの保存先を解決する管理器。
        const LamaPon::PersistenceProfiles profiles(
            root,
            "coordinator-game",
            "test");
        // account: 破損sidecarを検証する対象アカウント。
        const auto account = profiles.Account("corrupt-sidecar-player");
        // sidecarPath: 破損・置換する復旧sidecar。
        const auto sidecarPath = account.rootDirectory / L"Recovery.prefs";
        // preferences: ゲストPlayerPrefsストア。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        // saves: ゲストセーブストア。
        LamaPon::SaveDataStore saves(root / L"Saves");
        // coordinator: 破損sidecarの検出と破棄を管理する調整器。
        Coordinator coordinator(preferences, saves, root);
        ConfigureCoordinator(coordinator);

        // 空sidecarを作り通常有効化が拒否されることを確認します。
        WriteText(sidecarPath, {});
        Require(
            Throws([&]
            {
                (void)coordinator.PrepareAccount(
                    "corrupt-sidecar-player");
            }),
            "An empty recovery sidecar was not quarantined.");
        // empty: 空sidecarの破棄に使う復旧版。
        const auto empty = coordinator.RecoveryStatus();
        // 変更されていない空sidecarを削除できることを検証します。
        Require(
            empty.revision != 0u
                && coordinator.DiscardPendingRecovery(empty.revision)
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Succeeded
                && !coordinator.HasPendingRecovery()
                && !std::filesystem::exists(sidecarPath),
            "An unchanged empty recovery sidecar could not be discarded.");

        // oversized: 許容サイズ上限を超えるsidecar本文。
        const std::string oversized(
            LamaPon::CloudPreferencesMaxBytes + 1u,
            'a');
        WriteText(sidecarPath, oversized);
        // 巨大sidecarも隔離し、変更がなければ明示破棄できます。
        Require(
            Throws([&]
            {
                (void)coordinator.PrepareAccount(
                    "corrupt-sidecar-player");
            }),
            "An oversized recovery sidecar was not quarantined.");
        // unchangedLarge: 巨大sidecarの破棄版。
        const auto unchangedLarge = coordinator.RecoveryStatus();
        // 未変更の巨大sidecarを安全に削除します。
        Require(
            coordinator.DiscardPendingRecovery(unchangedLarge.revision)
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Succeeded
                && !std::filesystem::exists(sidecarPath),
            "An unchanged oversized recovery sidecar could not be discarded.");

        WriteText(sidecarPath, oversized);
        // 同一巨大内容を再作成し、外部差し替え時の版更新を検証します。
        Require(
            Throws([&]
            {
                (void)coordinator.PrepareAccount(
                    "corrupt-sidecar-player");
            }),
            "The replacement recovery incident was not quarantined.");
        // beforeReplacement: 外部置換前に観測したsidecar版。
        const auto beforeReplacement = coordinator.RecoveryStatus();
        // replacementPath: 原子的置換用の一時ファイル先。
        const auto replacementPath =
            account.rootDirectory / L"Recovery.replacement";
        WriteText(
            replacementPath,
            std::string(oversized.size(), 'b'));
        // 外部writerが巨大sidecarを置換できたことを検証します。
        Require(
            MoveFileExW(
                replacementPath.c_str(),
                sidecarPath.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE,
            "Oversized recovery replacement could not be published.");
        // afterReplacement: 外部置換後に再観測したsidecar版。
        const auto afterReplacement = coordinator.RecoveryStatus();
        // 古い版を拒否し、最新観測版だけを破棄できることを検証します。
        Require(
            afterReplacement.revision != beforeReplacement.revision
                && coordinator.DiscardPendingRecovery(
                    beforeReplacement.revision)
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Stale
                && coordinator.DiscardPendingRecovery(
                    afterReplacement.revision)
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Succeeded
                && !std::filesystem::exists(sidecarPath)
                && !coordinator.HasPendingRecovery(),
            "The latest observed oversized replacement could not be discarded safely.");

        // 再作成した巨大sidecarを外部削除し、消失時の破棄を検証します。
        WriteText(sidecarPath, oversized);
        Require(
            Throws([&]
            {
                (void)coordinator.PrepareAccount(
                    "corrupt-sidecar-player");
            }),
            "The missing-sidecar recovery incident was not quarantined.");
        // beforeMissing: 削除前に観測した巨大sidecar版。
        const auto beforeMissing = coordinator.RecoveryStatus();
        // removeError: fixture sidecarの削除エラー。
        std::error_code removeError;
        std::filesystem::remove(sidecarPath, removeError);
        Require(!removeError, "Recovery sidecar removal fixture failed.");
        // missing: 外部削除後に更新された復旧版。
        const auto missing = coordinator.RecoveryStatus();
        // 消失済みsidecarが復旧待ちを残さないことを検証します。
        Require(
            missing.revision != beforeMissing.revision
                && coordinator.DiscardPendingRecovery(missing.revision)
                    == LamaPon::Detail::
                        OnlinePersistenceRecoveryOperationResult::Succeeded
                && !coordinator.HasPendingRecovery(),
            "An already-missing sidecar kept recovery permanently blocked.");
    }

    // 公開復旧APIの版管理、操作ガード、Busy競合時の順序を検証します。
    void TestPublicRecoveryRevisionAndOperationGuards()
    {
        // root: 公開復旧APIケースの保存先。
        const auto root = CaseRoot("public-recovery-api");
        // preferences: ゲストPlayerPrefsストア。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        // saves: ゲストセーブストア。
        LamaPon::SaveDataStore saves(root / L"Saves");
        // backend: ログイン開始応答を返し、指定要求で停止するHTTPバックエンド。
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "recovery-busy" },
                        { "pollToken", "recovery-busy-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/recovery-busy"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    })
            },
            0u);
        // services: 公開復旧APIを呼び出すテスト用オンラインサービス。
        // request callbackはHTTP要求をbackendへ転送します。
        auto services = LamaPon::Detail::OnlineServicesTestAccess::Create(
            OnlineConfiguration(),
            [backend](const LamaPon::HttpRequest& request)
            {
                // HTTP要求をbackendへ送り応答を返します。
                return backend->Send(request);
            },
            std::make_unique<MemoryTokenStore>(
                std::make_shared<TokenStoreState>()));
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);
        // coordinator: servicesが使用する永続化調整器。
        auto* const coordinator =
            LamaPon::Detail::OnlinePersistenceAccess::Coordinator(*services);
        // 調整器がサービスへ接続済みであることを検証します。
        Require(coordinator != nullptr, "Recovery API coordinator is missing.");

        // profiles: sidecarのアカウント保存先を解決する管理器。
        const LamaPon::PersistenceProfiles profiles(
            root,
            "coordinator-game",
            "test");
        // account: 公開APIの復旧対象アカウント。
        const auto account = profiles.Account("public-recovery-player");
        // sidecarPath: 公開APIから検出する復旧sidecar。
        const auto sidecarPath = account.rootDirectory / L"Recovery.prefs";
        // writeSidecar(value: snapshot値) テスト用sidecarの内容を更新します。
        const auto writeSidecar = [&](const std::string_view value)
        {
            // sidecar: 指定値を書き込むPlayerPrefs文書。
            LamaPon::PlayerPrefs sidecar(sidecarPath);
            sidecar.Load();
            sidecar.SetString("snapshot", std::string(value));
            sidecar.Save();
        };
        // 初回復旧incidentを作ります。
        writeSidecar("first");
        // sidecar検出で通常のアカウント準備が拒否されます。
        Require(
            Throws([&]
            {
                (void)coordinator->PrepareAccount("public-recovery-player");
            }),
            "Recovery API fixture was not detected.");
        // first: 最初の復旧incidentと公開revision。
        const auto first = services->PersistenceRecoveryStatus();
        // 初回revisionが安定した非ゼロ値として公開されることを検証します。
        Require(
            first.state
                    == LamaPon::OnlinePersistenceRecoveryState::DurableSidecar
                && first.revision != 0
                && services->PersistenceRecoveryStatus().revision
                    == first.revision,
            "Recovery status did not expose a stable nonzero revision.");

        // requestsBefore: 復旧中に許可しないHTTP通信の基準件数。
        const auto requestsBefore = backend->Requests().size();
        // 復旧待ち中はログイン開始を拒否し、HTTPを送信しません。
        Require(
            !services->BeginDiscordSignIn()
                && services->LastErrorCode()
                    == "persistence_recovery_required"
                && backend->Requests().size() == requestsBefore,
            "Pending recovery did not reject sign-in before HTTP dispatch.");
        // disabled: 名前空間無効化を試す構成。
        LamaPon::OnlineServiceConfiguration disabled;
        // 復旧待ち中のConfigure失敗で既存名前空間とrevisionを維持します。
        Require(
            Throws([&]
            {
                services->Configure(disabled);
            })
                && coordinator->IsNamespaceEnabled()
                && services->PersistenceRecoveryStatus().revision
                    == first.revision,
            "Pending recovery changed namespace during rejected Configure.");

        // expectedSidecar: sidecarの置換前に読み込む期待文書。
        LamaPon::PlayerPrefs expectedSidecar(sidecarPath);
        // expectedDocument: 置換前に検証したsidecarスナップショット。
        const auto expectedDocument =
            LamaPon::Detail::LocalPersistenceDocuments::ReadPlayerPrefs(
                expectedSidecar);
        // sidecarを外部変更し、置換後のrevisionを作ります。
        writeSidecar("tampered");
        // changed: 外部変更後に公開された復旧状態。
        const auto changed = services->PersistenceRecoveryStatus();
        // 置換後sidecarを信頼せず、古いrevisionの操作も拒否します。
        Require(
            expectedDocument.state
                    == LamaPon::Detail::LocalPersistenceDocumentState::Loaded
                && changed.state
                    == LamaPon::OnlinePersistenceRecoveryState::UnavailableSidecar
                && changed.revision != first.revision
                && services->DiscardPersistence(first.revision)
                    == LamaPon::OnlinePersistenceOperationResult::Stale
                && services->RestorePersistence(changed.revision)
                    == LamaPon::OnlinePersistenceOperationResult::Failed,
            "Sidecar replacement was adopted by Restore as a trusted snapshot.");
        // 最新revisionでのみsidecar破棄が成功します。
        Require(
            services->DiscardPersistence(changed.revision)
                    == LamaPon::OnlinePersistenceOperationResult::Succeeded
                && services->PersistenceRecoveryStatus().state
                    == LamaPon::OnlinePersistenceRecoveryState::None
                && !std::filesystem::exists(sidecarPath),
            "Latest-revision replacement discard failed.");

        // revisionを巻き戻さず、別incidentで古いUI tokenが再利用されるのを防ぎます。
        // 次の復旧incidentを作成します。
        writeSidecar("second");
        // 二件目のsidecarも通常準備で検出されます。
        Require(
            Throws([&]
            {
                (void)coordinator->PrepareAccount("public-recovery-player");
            }),
            "Second recovery incident was not detected.");
        // second: 初回とは異なるrevisionを持つ復旧incident。
        const auto second = services->PersistenceRecoveryStatus();
        // 古いincidentのrevisionでは新しいincidentを操作できません。
        Require(
            second.revision != 0
                && second.revision != first.revision
                && services->DiscardPersistence(first.revision)
                    == LamaPon::OnlinePersistenceOperationResult::Stale
                && services->DiscardPersistence(second.revision)
                    == LamaPon::OnlinePersistenceOperationResult::Succeeded,
            "Recovery revision allowed a stale cross-incident operation.");

        // 次のテスト要求をbackendへ送ります。
        Require(
            services->BeginDiscordSignIn(),
            "Could not start Busy recovery ordering fixture.");
        // 認証HTTP要求がbackend内で停止するまで待ちます。
        backend->WaitUntilRequestBlocked();
        // Busy競合の最中に三件目の復旧incidentを作ります。
        writeSidecar("busy-third");
        // ログインBusy中もsidecar検出は復旧状態へ移ります。
        Require(
            Throws([&]
            {
                (void)coordinator->PrepareAccount("public-recovery-player");
            }),
            "Busy recovery incident was not detected.");
        // busyRecovery: 通信Busy中に公開された復旧状態。
        const auto busyRecovery = services->PersistenceRecoveryStatus();
        // 古いrevision判定をBusy判定より先に行うことを検証します。
        Require(
            services->DiscardPersistence(first.revision)
                    == LamaPon::OnlinePersistenceOperationResult::Stale
                && services->DiscardPersistence(busyRecovery.revision)
                    == LamaPon::OnlinePersistenceOperationResult::Busy,
            "Recovery stale revision was not checked before Busy.");
        // 停止していた認証HTTP要求を再開します。
        backend->ReleaseBlockedRequest();
        // 認証待ち状態へ進むまでサービスを更新します。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証待ち状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::WaitingForAuthorization;
            },
            "Busy recovery login start did not finish.");
        // Busy状態を解消するため認証フローをキャンセルします。
        services->CancelDiscordSignIn();
        // currentRecovery: Busy解消後に破棄する最新復旧状態。
        const auto currentRecovery = services->PersistenceRecoveryStatus();
        // Busy終了後の最新revisionで復旧を破棄できます。
        Require(
            services->DiscardPersistence(currentRecovery.revision)
                == LamaPon::OnlinePersistenceOperationResult::Succeeded,
            "Recovery could not be discarded after Busy ended.");
    }

    // 公開競合一覧の安定ID、Busy時の操作順、競合解決後の削除を検証します。
    void TestPublicCloudConflictRegistryAndResolution()
    {
        // root: 公開クラウド競合ケースの保存先。
        const auto root = CaseRoot("public-cloud-conflicts");
        // preferences: アカウントと同期するPlayerPrefs。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        // saves: アカウントと同期するセーブストア。
        LamaPon::SaveDataStore saves(root / L"Saves");

        // authorized: 初回ログインで返すセッション。
        auto authorized = SessionJson(
            "public-cloud-access",
            "public-cloud-refresh",
            "public-cloud-player",
            900u);
        // 初回ログイン応答を認証済み状態にします。
        authorized["status"] = "authorized";
        // refreshed: クラウド更新後に返すセッション。
        const auto refreshed = SessionJson(
            "public-cloud-access-2",
            "public-cloud-refresh-2",
            "public-cloud-player",
            900u);
        // backend: ログイン・更新応答を返し、更新要求で停止するバックエンド。
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "public-cloud-login" },
                        { "pollToken", "public-cloud-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/public-cloud"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(200u, refreshed),
                JsonResponse(204u)
            },
            2u);
        // services: 公開競合APIを呼ぶテスト用サービス。
        // request callbackはすべてbackendへ転送します。
        auto services = LamaPon::Detail::OnlineServicesTestAccess::Create(
            OnlineConfiguration(),
            [backend](const LamaPon::HttpRequest& request)
            {
                // HTTP要求をbackendへ送り応答を返します。
                return backend->Send(request);
            },
            std::make_unique<MemoryTokenStore>(
                std::make_shared<TokenStoreState>()));
        // ゲストストアをオンライン永続化へ接続します。
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);
        // クラウド競合テスト用アカウントへサインインします。
        CompleteDiscordLogin(*services);
        // SignedIn状態までサービスを更新します。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証済み状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Public cloud conflict login did not complete.");

        // クラウド競合のローカル基準となるPlayerPrefsを保存します。
        preferences.SetString("local", "preferences");
        preferences.Save();
        // クラウド競合のローカル基準となるセーブ枠を保存します。
        saves.SaveJson("public-slot", R"({"local":"slot"})");
        // coordinator: serviceが保持する永続化調整器。
        auto* const coordinator =
            LamaPon::Detail::OnlinePersistenceAccess::Coordinator(*services);
        // journal: ローカル基準と競合記録を保持するクラウドジャーナル。
        auto* const journal = coordinator ? coordinator->Journal() : nullptr;
        // クラウドジャーナルが接続済みであることを検証します。
        Require(journal != nullptr, "Public cloud journal is missing.");
        // preferencesDocument: 同期するローカルPlayerPrefs文書。
        const auto preferencesDocument =
            LamaPon::Detail::LocalPersistenceDocuments::ReadPlayerPrefs(
                preferences);
        // slotDocument: 同期するローカルセーブ文書。
        const auto slotDocument =
            LamaPon::Detail::LocalPersistenceDocuments::ReadSaveData(
                saves,
                "public-slot");
        // 両ローカル文書が有効な同期元として読めることを検証します。
        Require(
            preferencesDocument.state
                    == LamaPon::Detail::LocalPersistenceDocumentState::Loaded
                && slotDocument.state
                    == LamaPon::Detail::LocalPersistenceDocumentState::Loaded,
            "Public cloud local fixtures were not readable.");

        // preferencesResource: PlayerPrefsを表すクラウド資源。
        const auto preferencesResource =
            LamaPon::CloudSaveResource::Preferences();
        // slotResource: セーブ枠を表すクラウド資源。
        const auto slotResource =
            LamaPon::CloudSaveResource::SaveSlot("public-slot");
        // PreferencesMutation: preferences競合用のローカル変更ID。
        constexpr std::string_view PreferencesMutation =
            "11111111-1111-4111-8111-111111111111";
        // SlotMutation: セーブ枠競合用のローカル変更ID。
        constexpr std::string_view SlotMutation =
            "22222222-2222-4222-8222-222222222222";
        // 両資源にローカル変更とリモート競合を記録します。
        journal->QueuePut(
            preferencesResource,
            preferencesDocument.bytes,
            PreferencesMutation);
        journal->RecordConflict(
            preferencesResource,
            PreferencesMutation,
            { preferencesResource, "\"remote-prefs\"", true, {}, {} });
        journal->QueuePut(
            slotResource,
            slotDocument.bytes,
            SlotMutation);
        journal->RecordConflict(
            slotResource,
            SlotMutation,
            { slotResource, "\"remote-slot\"", true, {}, {} });

        // firstとsecond: 呼出しをまたいで安定する競合一覧。
        const auto first = services->CloudConflicts();
        // second: 再取得した競合一覧。
        const auto second = services->CloudConflicts();
        // 両一覧にpreferencesとセーブ枠の競合が残ることを検証します。
        Require(
            first.size() == 2u && second.size() == 2u,
            "Public conflict enumeration lost journal conflicts.");
        // findByKind(conflicts: 公開競合一覧, kind: 資源種別) 指定種別の競合を探します。
        const auto findByKind = [](const auto& conflicts, const auto kind)
        {
            // 対象種別と一致する一覧要素を返します。
            return std::find_if(
                conflicts.begin(),
                conflicts.end(),
                [kind](const LamaPon::OnlineCloudConflict& conflict)
                {
                    // 競合資源種別が一致するか判定します。
                    return conflict.kind == kind;
                });
        };
        // firstPreferences: 初回一覧のPlayerPrefs競合。
        const auto firstPreferences = findByKind(
            first,
            LamaPon::OnlineCloudResourceKind::Preferences);
        // firstSlot: 初回一覧のセーブ枠競合。
        const auto firstSlot = findByKind(
            first,
            LamaPon::OnlineCloudResourceKind::SaveSlot);
        // secondPreferences: 再取得した一覧のPlayerPrefs競合。
        const auto secondPreferences = findByKind(
            second,
            LamaPon::OnlineCloudResourceKind::Preferences);
        // secondSlot: 再取得した一覧のセーブ枠競合。
        const auto secondSlot = findByKind(
            second,
            LamaPon::OnlineCloudResourceKind::SaveSlot);
        // canonicalOpaque(value: 公開競合ID) 不透明IDが正規16進形式か調べます。
        const auto canonicalOpaque = [](const std::string_view value)
        {
            // 32桁の小文字16進値だけを有効とします。
            return value.size() == 32u
                && value.find_first_not_of("0123456789abcdef")
                    == std::string_view::npos;
        };
        // 安定IDと公開メタデータが正しく隠蔽・維持されることを検証します。
        Require(
            firstPreferences != first.end()
                && firstSlot != first.end()
                && secondPreferences != second.end()
                && secondSlot != second.end()
                && canonicalOpaque(firstPreferences->id)
                && canonicalOpaque(firstSlot->id)
                && firstPreferences->id == secondPreferences->id
                && firstSlot->id == secondSlot->id
                && firstPreferences->id != PreferencesMutation
                && firstSlot->id != SlotMutation
                && firstPreferences->slot.empty()
                && firstPreferences->localByteLength
                    == preferencesDocument.bytes.size()
                && firstPreferences->remoteDeleted
                && firstSlot->slot == "public-slot"
                && firstSlot->localByteLength == slotDocument.bytes.size()
                && firstSlot->remoteDeleted,
            "Public conflict DTO exposed unstable or incorrect metadata.");
        // 未知の競合IDをStaleとして拒否します。
        Require(
            services->ResolveCloudConflict(
                "stale-opaque-id",
                LamaPon::OnlineCloudConflictResolution::UseLocal)
                    == LamaPon::OnlinePersistenceOperationResult::Stale,
            "Unknown conflict ID was not rejected as stale.");

        // refresh tokenを期限切れにして更新要求を開始します。
        services->Update(850.0f);
        // 更新HTTP要求がbackend内で停止するまで待ちます。
        backend->WaitUntilRequestBlocked();
        // 認証Busy中は同期と競合解決をBusyとして保ちます。
        Require(
            services->RequestCloudSync()
                    == LamaPon::OnlinePersistenceOperationResult::Busy
                && services->ResolveCloudConflict(
                firstPreferences->id,
                LamaPon::OnlineCloudConflictResolution::UseLocal)
                    == LamaPon::OnlinePersistenceOperationResult::Busy
                && services->CloudConflicts().front().id
                    == firstPreferences->id,
            "A valid conflict ID was not preserved while auth was Busy.");
        // synchronizer: 競合を外部から消す同期器。
        auto* const synchronizer = coordinator->Synchronizer();
        Require(synchronizer != nullptr, "Public cloud synchronizer is missing.");
        // PlayerPrefs競合をリモート採用で先に解決します。
        synchronizer->ResolveConflict(
            preferencesResource,
            PreferencesMutation,
            LamaPon::Detail::CloudSaveConflictResolution::UseRemote);
        // 消えた競合はBusyよりStaleとして扱います。
        Require(
            services->ResolveCloudConflict(
                firstPreferences->id,
                LamaPon::OnlineCloudConflictResolution::UseLocal)
                    == LamaPon::OnlinePersistenceOperationResult::Stale,
            "A disappeared conflict was reported Busy instead of Stale.");
        // remainingWhileBusy: PlayerPrefs競合を除いた一覧。
        const auto remainingWhileBusy = services->CloudConflicts();
        // 競合一覧の縮小で無関係なセーブIDを失わないことを検証します。
        Require(
            remainingWhileBusy.size() == 1u
                && remainingWhileBusy.front().id == firstSlot->id,
            "Pruning a stale conflict invalidated an unrelated ID.");
        // 停止していたtoken更新要求を再開します。
        backend->ReleaseBlockedRequest();
        // refresh完了後にSignedInへ戻るまでサービスを更新します。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証済み状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Public conflict refresh did not complete.");
        // 残ったセーブ枠競合をローカル採用で解決します。
        Require(
            services->ResolveCloudConflict(
                firstSlot->id,
                LamaPon::OnlineCloudConflictResolution::UseLocal)
                    == LamaPon::OnlinePersistenceOperationResult::Succeeded,
            "Public UseLocal conflict resolution failed.");
        // remaining: 解決後に公開される競合一覧。
        const auto remaining = services->CloudConflicts();
        // 競合がすべて解決済みであることを検証します。
        Require(
            remaining.empty(),
            "Resolved conflicts remained in the public registry.");

        // 解決後のローカルPlayerPrefs文書を再生成します。
        preferences.SetString("local", "after-resolution");
        preferences.Save();
        // recreatedPreferences: サインアウト競合として再投入する文書。
        const auto recreatedPreferences =
            LamaPon::Detail::LocalPersistenceDocuments::ReadPlayerPrefs(
                preferences);
        // SignOutMutation: サインアウト競合用のローカル変更ID。
        constexpr std::string_view SignOutMutation =
            "33333333-3333-4333-8333-333333333333";
        // サインアウト後に無効化される競合IDを作成します。
        journal->QueuePut(
            preferencesResource,
            recreatedPreferences.bytes,
            SignOutMutation,
            std::optional<std::string>{ "\"remote-prefs\"" });
        journal->RecordConflict(
            preferencesResource,
            SignOutMutation,
            { preferencesResource, "\"remote-prefs\"", true, {}, {} });
        // beforeSignOut: サインアウト前に公開される競合一覧。
        const auto beforeSignOut = services->CloudConflicts();
        Require(
            beforeSignOut.size() == 1u,
            "Sign-out conflict fixture was not published.");
        // staleAfterSignOut: サインアウトで無効になる旧競合ID。
        const auto staleAfterSignOut = beforeSignOut.front().id;
        // アカウントを切り離して競合IDを無効化します。
        services->SignOut();
        // サインアウト後に旧IDがStaleとして拒否されます。
        Require(
            services->ResolveCloudConflict(
                staleAfterSignOut,
                LamaPon::OnlineCloudConflictResolution::UseRemote)
                    == LamaPon::OnlinePersistenceOperationResult::Stale,
            "Sign-out did not invalidate conflict IDs before Busy checks.");
    }

    // 認証情報保存失敗時はアカウント切替を戻し、発行済みセッションを破棄します。
    void TestCredentialSaveFailureRollsBackAndRevokesSession()
    {
        // root: 認証保存失敗ケースの保存先。
        const auto root = CaseRoot("credential-save-rollback");
        // preferences: ロールバック後に復元するゲストPlayerPrefs。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        // saves: ロールバック後に復元するゲストセーブストア。
        LamaPon::SaveDataStore saves(root / L"Saves");

        // authorized: 保存失敗を起こす認証済みセッション。
        auto authorized = SessionJson(
            "rejected-access-token",
            "rejected-refresh-token",
            "rejected-player");
        // 応答を認証完了として扱います。
        authorized["status"] = "authorized";
        // backend: 認証・poll・logout応答を返すHTTPバックエンド。
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "transaction" },
                        { "pollToken", "poll-token" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/authorize"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(204u)
            });
        // storeState: 更新トークン保存結果を観測する共有状態。
        auto storeState = std::make_shared<TokenStoreState>();
        storeState->failSave = true;
        // services: 認証保存失敗経路を実行するオンラインサービス。
        // request callbackはHTTP要求をbackendへ転送します。
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    // HTTP要求をbackendへ送り応答を返します。
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        // ゲスト保存先をオンライン永続化へ接続します。
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);

        // 資格情報保存失敗を起こす認証を開始します。
        CompleteDiscordLogin(*services);
        // 失敗後のlogoutとロールバック完了までサービスを更新します。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証エラー状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::Error;
            },
            "Rejected session cleanup did not finish.");

        // requests: 失敗後にlogoutまで送信されたHTTP要求列。
        const auto requests = backend->Requests();
        // session、ゲストbinding、logout要求が安全に復元されたことを検証します。
        Require(
            !services->IsSignedIn()
                && services->Player().playerId.empty()
                && preferences.FilePath() == root / L"PlayerPrefs.json"
                && saves.Directory() == root / L"Saves"
                && preferences.GetString("owner") == "guest"
                && storeState->saveCount == 1u
                && storeState->deleteCount == 1u
                && storeState->token.empty()
                && requests.size() == 3u
                && requests.back().url.ends_with(
                    L"/v1/auth/session/logout")
                && HasBearer(
                    requests.back(),
                    L"rejected-access-token"),
            "Credential Save failure exposed or retained a session.");
    }

    // 明示同期要求のBusy状態と、終端停止後の要求失敗を検証します。
    void TestRequestCloudSyncReportsBusyAndTerminalStop()
    {
        // root: 明示同期要求状態ケースの保存先。
        const auto root = CaseRoot("public-cloud-request-state");
        // preferences: オンラインアカウントへ結合するPlayerPrefs。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        // saves: オンラインアカウントへ結合するセーブストア。
        LamaPon::SaveDataStore saves(root / L"Saves");
        // authorized: 初回ログイン完了時に返すセッション。
        auto authorized = SessionJson(
            "request-cloud-access",
            "request-cloud-refresh",
            "request-cloud-player",
            900u);
        // poll応答を認証済み状態にします。
        authorized["status"] = "authorized";
        // requestCount: fixtureへ届いたHTTP要求数。
        auto requestCount = std::make_shared<std::atomic_size_t>();
        // services: 同期要求状態を検証するサービス。
        // request callbackはendpoint別応答を返して要求数を記録します。
        auto services = LamaPon::Detail::OnlineServicesTestAccess::Create(
            OnlineConfiguration(),
            [authorized, requestCount](const LamaPon::HttpRequest& request)
            {
                requestCount->fetch_add(1u, std::memory_order_relaxed);
                // 認証開始要求にはauthorization情報を返します。
                if (request.url.ends_with(L"/v1/auth/login/start"))
                {
                    // ログイン開始に必要なpoll情報を返します。
                    return JsonResponse(
                        201u,
                        {
                            { "transactionId", "request-cloud-login" },
                            { "pollToken", "request-cloud-poll" },
                            {
                                "authorizationUrl",
                                "https://login.example.test/request-cloud"
                            },
                            { "expiresIn", 300u },
                            { "pollInterval", 1u }
                        });
                }
                // 認証完了要求にはauthorizedセッションを返します。
                if (request.url.ends_with(L"/v1/auth/login/complete"))
                {
                    // 準備済みの認証セッションを返します。
                    return JsonResponse(200u, authorized);
                }
                // manifest要求には空の同期一覧を返します。
                if (request.url.ends_with(
                        L"/v1/cloud-saves/manifest"))
                {
                    // 空のクラウド保存一覧を返します。
                    return EmptyCloudManifestResponse();
                }
                // logout要求を成功応答で完了させます。
                if (request.url.ends_with(L"/v1/auth/session/logout"))
                {
                    // logoutが受け付けられたことを返します。
                    return JsonResponse(204u);
                }
                // unexpected: fixtureに定義されていない要求。
                LamaPon::HttpResponse unexpected;
                unexpected.transportError = "Unexpected request.";
                // 未定義要求を通信失敗として返します。
                return unexpected;
            },
            std::make_unique<MemoryTokenStore>(
                std::make_shared<TokenStoreState>()));
        // ゲスト保存先をサービスへ接続します。
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);
        // テスト用アカウントへサインインします。
        CompleteDiscordLogin(*services);
        // 同期がIdleへ戻るまで更新を続けます。
        // idleDeadline: 初回同期を待つ終了時刻。
        const auto idleDeadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(10);
        // 認証と初回同期が完了するか期限切れまで更新します。
        while ((services->State()
                    != LamaPon::OnlineAccountState::SignedIn
                || services->CloudSyncStatus().state
                    != LamaPon::OnlineCloudSyncState::Idle)
            && std::chrono::steady_clock::now() < idleDeadline)
        {
            services->Update(0.0f);
            LamaPon::Detail::OnlinePersistenceAccess::EndFrame(*services);
            std::this_thread::yield();
        }
        // initialCloudStatus: 初回同期後の公開状態。
        const auto initialCloudStatus = services->CloudSyncStatus();
        // 同期がIdleにならなかった場合は状態と要求数を報告します。
        if (services->State() != LamaPon::OnlineAccountState::SignedIn
            || initialCloudStatus.state
                != LamaPon::OnlineCloudSyncState::Idle)
        {
            // fixture準備の失敗を状態情報と共に通知します。
            throw std::runtime_error(
                "Cloud request fixture did not become idle (state="
                + std::to_string(static_cast<int>(initialCloudStatus.state))
                + ", stop="
                + std::to_string(
                    static_cast<int>(initialCloudStatus.stopReason))
                + ", requests="
                + std::to_string(requestCount->load())
                + ").");
        }

        // ローカル文書破損後も明示要求自体は受理されます。
        WriteText(preferences.FilePath(), "{corrupt-local");
        // 終端停止前の同期要求を受理することを検証します。
        Require(
            services->RequestCloudSync()
                == LamaPon::OnlinePersistenceOperationResult::Succeeded,
            "A healthy synchronizer rejected an explicit request.");
        // stoppedDeadline: 同期停止を待つ終了時刻。
        const auto stoppedDeadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(10);
        // synchronizer停止または期限切れまでサービスを更新します。
        while (services->CloudSyncStatus().state
                    != LamaPon::OnlineCloudSyncState::Stopped
            && std::chrono::steady_clock::now() < stoppedDeadline)
        {
            services->Update(0.0f);
            LamaPon::Detail::OnlinePersistenceAccess::EndFrame(*services);
            std::this_thread::yield();
        }
        // 終端停止後は同じ明示要求をFailedとして返します。
        Require(
            services->CloudSyncStatus().state
                    == LamaPon::OnlineCloudSyncState::Stopped
                && services->RequestCloudSync()
                == LamaPon::OnlinePersistenceOperationResult::Failed,
            "A terminally stopped synchronizer reported request success.");
    }

    // 認証保存失敗とlogout再試行中は以前commit済みの資格情報を保持します。
    void TestFailedCredentialSaveKeepsPriorCommitPendingUntilDeleted()
    {
        // runCase(logoutSucceeds: logout応答の成否) 両logout結果の資格情報保持を検証します。
        const auto runCase = [](const bool logoutSucceeds)
        {
            // root: logout結果別の保存失敗ケース出力先。
            const auto root = CaseRoot(
                logoutSucceeds
                    ? "failed-save-logout-success"
                    : "failed-save-logout-failure");
            // preferences: ロールバック対象のゲストPlayerPrefs。
            LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
            preferences.Load();
            preferences.SetString("owner", "guest");
            // saves: ロールバック対象のゲストセーブストア。
            LamaPon::SaveDataStore saves(root / L"Saves");

            // authorized: 保存失敗を起こす新しい認証セッション。
            auto authorized = SessionJson(
                "failed-save-access",
                "failed-save-candidate-refresh",
                "failed-save-player");
            // poll結果を認証済み状態にします。
            authorized["status"] = "authorized";
            // logoutResponse: logout成否に応じたfixture応答。
            const auto logoutResponse = logoutSucceeds
                ? JsonResponse(204u)
                : JsonResponse(
                    503u,
                    {
                        {
                            "error",
                            { { "code", "logout_failed" } }
                        }
                    });
            // backend: 認証とlogoutの応答を返すバックエンド。
            auto backend = std::make_shared<ScriptedBackend>(
                std::deque<LamaPon::HttpResponse>{
                    JsonResponse(
                        201u,
                        {
                            { "transactionId", "failed-save-transaction" },
                            { "pollToken", "failed-save-poll" },
                            {
                                "authorizationUrl",
                                "https://login.example.test/failed-save"
                            },
                            { "expiresIn", 300u },
                            { "pollInterval", 1u }
                        }),
                    JsonResponse(200u, authorized),
                    logoutResponse
                });
            // storeState: 以前のcommit値とSave失敗を記録する保存状態。
            auto storeState = std::make_shared<TokenStoreState>();
            // services: 保存失敗後のlogoutと資格情報再試行を実行するサービス。
            // request callbackはHTTP要求をbackendへ転送します。
            auto services =
                LamaPon::Detail::OnlineServicesTestAccess::Create(
                    OnlineConfiguration(),
                    [backend](const LamaPon::HttpRequest& request)
                    {
                        // HTTP要求をbackendへ送り応答を返します。
                        return backend->Send(request);
                    },
                    std::make_unique<MemoryTokenStore>(storeState));
            // Store契約: Save失敗では旧tokenを保ち、Delete成功まで置換しません。
            // Load後の以前commit済み資格情報を再現します。
            storeState->loadStatus =
                LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
            storeState->token = "previous-committed-refresh";
            storeState->failSave = true;
            storeState->failDelete = true;
            // ゲスト保存先をサービスへ接続します。
            LamaPon::Detail::OnlinePersistenceAccess::Attach(
                *services,
                preferences,
                saves,
                root);
            // Save失敗とlogout処理を起こす認証を開始します。
            CompleteDiscordLogin(*services);
            // 資格情報rollbackがErrorへ到達するまで更新します。
            UpdateUntil(
                *services,
                [&]
                {
                    // 認証エラー状態への到達を判定します。
                    return services->State()
                        == LamaPon::OnlineAccountState::Error;
                },
                "Ambiguous credential rollback did not finish.");
            // 以前のtokenとprimary errorを保ち、accountをguestへ戻します。
            Require(
                services->LastErrorCode() == "credential_save_failed"
                    && storeState->saveCount == 1u
                    && storeState->deleteCount == 1u
                    && storeState->token
                        == "previous-committed-refresh"
                    && preferences.FilePath()
                        == root / L"PlayerPrefs.json",
                "Ambiguous credential rollback lost its primary error or pending token.");

            // 保留中の資格情報削除が新規sign-inを閉じることを検証します。
            Require(
                !services->BeginDiscordSignIn()
                    && services->LastErrorCode()
                        == "credential_delete_failed"
                    && storeState->deleteCount == 2u
                    && backend->Requests().size() == 3u,
                "New sign-in did not fail closed on a pending credential delete.");
            // Configureも資格情報削除の完了までは状態を置換できません。
            Require(
                Throws([&]
                {
                    services->Configure(OnlineConfiguration());
                })
                    && storeState->deleteCount == 3u
                    && backend->Requests().size() == 3u
                    && preferences.FilePath()
                        == root / L"PlayerPrefs.json",
                "Configure replaced state while credential deletion was pending.");

            // 終了時の削除再試行を成功させる設定にします。
            storeState->failDelete = false;
            // サービス破棄時に保留中token削除を再試行します。
            services.reset();
            // 最終再試行が旧資格情報を消去したことを検証します。
            Require(
                storeState->deleteCount == 4u
                    && storeState->token.empty(),
                "Destruction did not resolve the sticky credential delete.");
        };

        // logout成功時も保存失敗後の保留状態を確認します。
        runCase(true);
        // logout失敗時の保留状態も確認します。
        runCase(false);
    }

    // SignOut直後にguestへ戻し、再ログイン時に同じアカウント保存先を復元します。
    void TestSignOutImmediatelyRestoresGuestAndReloginRestoresAccount()
    {
        // root: sign-outと再ログインケースの保存先。
        const auto root = CaseRoot("signout-immediate-guest");
        // preferences: ゲストとアカウント間で再利用するPlayerPrefs。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        preferences.Save();
        // saves: ゲストとアカウント間で再利用するセーブストア。
        LamaPon::SaveDataStore saves(root / L"Saves");

        // firstAuthorized: 初回ログインで発行するアカウントAセッション。
        auto firstAuthorized = SessionJson(
            "account-a-access-1",
            "account-a-refresh-1",
            "player-A",
            900u);
        // 初回poll応答を認証済み状態にします。
        firstAuthorized["status"] = "authorized";
        // secondAuthorized: 再ログインで発行する更新済みセッション。
        auto secondAuthorized = SessionJson(
            "account-a-access-2",
            "account-a-refresh-2",
            "player-A",
            900u);
        // 二回目poll応答を認証済み状態にします。
        secondAuthorized["status"] = "authorized";
        // backend: 初回ログイン、logout、再ログインの応答列。
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "account-a-first" },
                        { "pollToken", "account-a-first-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/account-a-first"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, firstAuthorized),
                JsonResponse(204u),
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "account-a-second" },
                        { "pollToken", "account-a-second-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/account-a-second"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, secondAuthorized)
            },
            2u);
        // storeState: 保存資格情報の呼出しを記録する状態。
        auto storeState = std::make_shared<TokenStoreState>();
        // services: sign-outと再ログインを実行するオンラインサービス。
        // request callbackはHTTP要求をbackendへ転送します。
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    // HTTP要求をbackendへ送り応答を返します。
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        // 同じPlayerPrefsとセーブストアをオンライン永続化へ接続します。
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);

        // preferencesAddress: PlayerPrefsインスタンスの安定性を確認する参照。
        auto* const preferencesAddress = &preferences;
        // savesAddress: SaveDataStoreインスタンスの安定性を確認する参照。
        auto* const savesAddress = &saves;
        // 初回セッションを有効化します。
        CompleteDiscordLogin(*services);
        // 初回ログインがSignedInへ進むまで更新します。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証済み状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Account A login did not complete.");
        // accountPreferencesPath: 初回サインイン時のアカウント設定先。
        const auto accountPreferencesPath = preferences.FilePath();
        // accountSaveDirectory: 初回サインイン時のアカウントセーブ先。
        const auto accountSaveDirectory = saves.Directory();
        // 再ログイン後の復元に使うアカウント専用データを保存します。
        preferences.SetString("account-only", "persisted-A");
        saves.SaveJson("account-slot", R"({"owner":"A"})");

        // SignOut return前にguest bindingへ戻し、logout待ちは別に継続します。
        services->SignOut();
        // blockedなlogout中にもguest bindingとobject identityを保ちます。
        Require(
            services->State()
                    == LamaPon::OnlineAccountState::SigningOut
                && !services->IsSignedIn()
                && preferences.FilePath() == root / L"PlayerPrefs.json"
                && saves.Directory() == root / L"Saves"
                && preferences.GetString("owner") == "guest"
                && preferences.GetString("account-only").empty()
                && &preferences == preferencesAddress
                && &saves == savesAddress,
            "SignOut returned while account persistence was still exposed.");
        // logout要求がbackend内で停止するまで待ちます。
        backend->WaitUntilRequestBlocked();
        // logout要求の応答を返します。
        backend->ReleaseBlockedRequest();
        // 初回アカウントlogout完了まで更新します。
        UpdateUntil(
            *services,
            [&]
            {
                // サインアウト状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::SignedOut;
            },
            "Account A logout did not finish.");
        // 同じアカウントへ二回目のサインインを開始します。
        CompleteDiscordLogin(*services);
        // 再ログインがSignedInへ進むまで更新します。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証済み状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Account A second login did not complete.");
        // アカウント固有の保存内容と両ストアの同一性が戻ることを検証します。
        Require(
            preferences.FilePath() == accountPreferencesPath
                && saves.Directory() == accountSaveDirectory
                && preferences.GetString("account-only") == "persisted-A"
                && saves.HasSlot("account-slot")
                && &preferences == preferencesAddress
                && &saves == savesAddress,
            "Account A persistence was not restored on re-login.");
    }

    // 保存済みtoken復元ではアカウント保存先を切り替えてからセッションを公開します。
    void TestStoredRestoreActivatesAccountBeforePublishingSession()
    {
        // root: 保存済みsession復元順序のケース保存先。
        const auto root = CaseRoot("stored-restore-order");
        // preferences: guestと復元accountで再利用するPlayerPrefs。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        // saves: guestと復元accountで再利用するセーブストア。
        LamaPon::SaveDataStore saves(root / L"Saves");
        // profiles: 復元対象accountの保存先を解決する管理器。
        const LamaPon::PersistenceProfiles profiles(
            root,
            "coordinator-game",
            "test");
        // account: session復元で切り替える保存領域。
        const auto account = profiles.Account("restored-player");
        // accountPreferences: 復元順序で読むaccount所有文書。
        LamaPon::PlayerPrefs accountPreferences(account.playerPrefsFile);
        accountPreferences.Load();
        accountPreferences.SetString("owner", "restored-account");
        accountPreferences.Save();

        // backend: 保存済みtokenのsession復元応答。
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    200u,
                    SessionJson(
                        "restored-access",
                        "rotated-refresh",
                        "restored-player",
                        900u))
            });
        // storeState: 保存済みtokenと再発行tokenを観測する状態。
        auto storeState = std::make_shared<TokenStoreState>();
        // services: 保存済みsession復元を実行するサービス。
        // request callbackはHTTP要求をbackendへ転送します。
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    // HTTP要求をbackendへ送り応答を返します。
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        // guest保存先をonline persistenceへ接続します。
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);

        // observedAccountBeforeTokenSave: token保存前にaccountが有効だった状態。
        bool observedAccountBeforeTokenSave{};
        // observedSessionStillPrivate: token保存中にsessionを公開しない状態。
        bool observedSessionStillPrivate{};
        // 保存hookでaccount切替とsession公開順序を観測します。
        storeState->onSave = [&]
        {
            // coordinator: hook時点のaccount切替状態を照会する調整器。
            const auto* const coordinator =
                LamaPon::Detail::OnlinePersistenceAccess::Coordinator(
                    *services);
            observedAccountBeforeTokenSave = coordinator
                && coordinator->IsAccountActive()
                && preferences.FilePath() == account.playerPrefsFile
                && saves.Directory() == account.saveDataDirectory
                && preferences.GetString("owner") == "restored-account";
            observedSessionStillPrivate =
                services->State()
                    == LamaPon::OnlineAccountState::RestoringSession
                && !services->IsSignedIn()
                && services->Player().playerId.empty();
        };
        // 保存済みrefresh tokenを読込済みとして再現します。
        storeState->loadStatus =
            LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
        storeState->token = "stored-refresh";
        // Configureで保存済みsession復元を開始します。
        services->Configure(OnlineConfiguration());
        // session応答前はguest bindingと未認証状態を保ちます。
        Require(
            services->State()
                    == LamaPon::OnlineAccountState::RestoringSession
                && preferences.FilePath() == root / L"PlayerPrefs.json"
                && preferences.GetString("owner") == "guest",
            "Stored restore changed persistence before its response arrived.");

        // session復元がSignedInへ進むまでサービスを更新します。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証済み状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Stored session restore did not finish.");
        // account切替完了後にtoken保存し、最後にsessionを公開したことを検証します。
        Require(
            observedAccountBeforeTokenSave
                && observedSessionStillPrivate
                && services->Player().playerId == "restored-player"
                && preferences.FilePath() == account.playerPrefsFile
                && preferences.GetString("owner") == "restored-account"
                && storeState->token == "rotated-refresh"
                && storeState->saveCount == 1u,
            "Stored restore published the session before account activation.");
    }

    // 破損accountの保存済みsession復元を拒否し、tokenを削除してguestを維持します。
    void TestCorruptAccountRejectsStoredRestoreAndDeletesCredential()
    {
        // root: 破損account復元ケースの保存先。
        const auto root = CaseRoot("stored-restore-corrupt-account");
        // preferences: 復元失敗後も維持するguest PlayerPrefs。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        // saves: 復元失敗後も維持するguestセーブストア。
        LamaPon::SaveDataStore saves(root / L"Saves");
        // profiles: 破損accountの文書パスを解決する管理器。
        const LamaPon::PersistenceProfiles profiles(
            root,
            "coordinator-game",
            "test");
        // account: 破損文書を配置する復元対象。
        const auto account = profiles.Account("corrupt-restored-player");
        // accountの厳密読込を失敗させます。
        WriteText(account.playerPrefsFile, "{not-json");

        // backend: 破損account復元応答とlogout応答。
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    200u,
                    SessionJson(
                        "corrupt-restore-access",
                        "corrupt-restore-rotation",
                        "corrupt-restored-player",
                        900u)),
                JsonResponse(204u)
            });
        // storeState: 保存済みtokenの削除結果を記録する状態。
        auto storeState = std::make_shared<TokenStoreState>();
        // services: 破損accountの復元拒否を実行するサービス。
        // request callbackはHTTP要求をbackendへ転送します。
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    // HTTP要求をbackendへ送り応答を返します。
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        // guest保存先をonline persistenceへ接続します。
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);
        // 保存済みtokenが存在する状態から復元を始めます。
        storeState->loadStatus =
            LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
        storeState->token = "stored-corrupt-profile-refresh";
        // Configureで保存済みsession復元を始めます。
        services->Configure(OnlineConfiguration());

        // 破損account拒否とtoken logoutが完了するまでサービスを更新します。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証エラー状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::Error;
            },
            "Corrupt restored account cleanup did not finish.");
        // requests: logoutまでにbackendが受信したHTTP要求。
        const auto requests = backend->Requests();
        // 未認証guest状態を維持し、保存済みcredentialをlogoutしたことを検証します。
        Require(
            !services->IsSignedIn()
                && services->Player().playerId.empty()
                && services->LastErrorCode()
                    == "persistence_activation_failed"
                && preferences.FilePath() == root / L"PlayerPrefs.json"
                && saves.Directory() == root / L"Saves"
                && preferences.GetString("owner") == "guest"
                && storeState->saveCount == 0u
                && storeState->deleteCount == 1u
                && storeState->token.empty()
                && requests.size() == 2u
                && requests.back().url.ends_with(
                    L"/v1/auth/session/logout")
                && HasBearer(
                    requests.back(),
                    L"corrupt-restore-access"),
            "Corrupt account restore exposed data or retained credentials.");
    }

    // refresh後のtoken保存失敗では更新sessionを破棄してguestへ戻します。
    void TestRefreshCredentialSaveFailureRevokesRotatedSession()
    {
        // root: refresh保存失敗ケースの保存先。
        const auto root = CaseRoot("refresh-save-rollback");
        // preferences: rollback後に維持するguest PlayerPrefs。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        // saves: rollback後に維持するguestセーブストア。
        LamaPon::SaveDataStore saves(root / L"Saves");

        // authorized: 初回サインインで使用するsession。
        auto authorized = SessionJson(
            "initial-access",
            "initial-refresh",
            "rotation-player");
        // poll応答を認証済み状態にします。
        authorized["status"] = "authorized";
        // backend: 初回login、refresh、logout応答。
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "rotation-transaction" },
                        { "pollToken", "rotation-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/rotation"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(
                    200u,
                    SessionJson(
                        "rotated-access",
                        "rotated-refresh",
                        "rotation-player",
                        900u)),
                JsonResponse(204u)
            });
        // storeState: token保存・削除回数を記録する状態。
        auto storeState = std::make_shared<TokenStoreState>();
        // services: refresh token保存失敗経路を実行するサービス。
        // request callbackはHTTP要求をbackendへ転送します。
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    // HTTP要求をbackendへ送り応答を返します。
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        // guest保存先をオンライン永続化へ接続します。
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);
        // rotation前のsessionを有効化します。
        CompleteDiscordLogin(*services);
        // 初回サインイン完了までサービスを更新します。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証済み状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Rotation fixture login did not complete.");

        // refreshされたtokenの保存を失敗させます。
        storeState->failSave = true;
        // token期限到達でrefresh処理を始めます。
        services->Update(24.0f);
        // 更新sessionの破棄処理が終わるまで待ちます。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証エラー状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::Error;
            },
            "Rotated token Save failure cleanup did not finish.");
        // requests: logoutまで送信されたHTTP要求。
        const auto requests = backend->Requests();
        // guest bindingと失効した更新sessionのlogoutを検証します。
        Require(
            !services->IsSignedIn()
                && services->LastErrorCode() == "credential_save_failed"
                && preferences.FilePath() == root / L"PlayerPrefs.json"
                && saves.Directory() == root / L"Saves"
                && preferences.GetString("owner") == "guest"
                && storeState->saveCount == 2u
                && storeState->deleteCount == 1u
                && storeState->token.empty()
                && requests.size() == 4u
                && HasBearer(requests.back(), L"rotated-access"),
            "Refresh token Save failure did not revoke the rotated session.");
    }

    // refresh要求中のSignOutは直ちにguestへ戻し、遅着sessionを破棄します。
    void TestRefreshSignOutRaceKeepsImmediateGuestBinding()
    {
        // root: refreshとSignOut競合ケースの保存先。
        const auto root = CaseRoot("refresh-signout-binding");
        // preferences: SignOut時にguestへ戻すPlayerPrefs。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        // saves: SignOut時にguestへ戻すセーブストア。
        LamaPon::SaveDataStore saves(root / L"Saves");

        // authorized: 初回sessionを返すlogin response。
        auto authorized = SessionJson(
            "race-old-access",
            "race-old-refresh",
            "race-player");
        // poll responseを認証済み状態にします。
        authorized["status"] = "authorized";
        // backend: login、refresh、logout応答を返してrefreshを停止します。
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "race-transaction" },
                        { "pollToken", "race-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/race"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(
                    200u,
                    SessionJson(
                        "race-new-access",
                        "race-new-refresh",
                        "race-player",
                        900u)),
                JsonResponse(204u)
            },
            2u);
        // storeState: refresh tokenの保存・削除状態。
        auto storeState = std::make_shared<TokenStoreState>();
        // services: refresh要求中のSignOut競合を実行するサービス。
        // request callbackはHTTP要求をbackendへ転送します。
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    // HTTP要求をbackendへ送り応答を返します。
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        // guest保存先をオンライン永続化へ接続します。
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);
        // 初回sessionを有効化します。
        CompleteDiscordLogin(*services);
        // 初回ログインがSignedInへ進むまで更新します。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証済み状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Refresh race fixture login did not complete.");
        // SignOut後に漏れていないことを確認するaccount専用値を保存します。
        preferences.SetString("race-account", "private");

        // proactive refreshを起動します。
        services->Update(24.0f);
        // refresh中状態を確認します。
        Require(
            services->State()
                == LamaPon::OnlineAccountState::RefreshingSession,
            "Proactive refresh was not started.");
        // refresh要求がbackend内で停止するまで待ちます。
        backend->WaitUntilRequestBlocked();
        // refresh応答前にSignOutし、guest bindingへ即時復帰します。
        services->SignOut();
        // SignOut返却時にguest bindingと未認証状態であることを検証します。
        Require(
            services->State()
                    == LamaPon::OnlineAccountState::SigningOut
                && !services->IsSignedIn()
                && preferences.FilePath() == root / L"PlayerPrefs.json"
                && saves.Directory() == root / L"Saves"
                && preferences.GetString("owner") == "guest"
                && preferences.GetString("race-account").empty(),
            "Refresh-race SignOut retained the account binding.");
        // 遅着refresh応答を返します。
        backend->ReleaseBlockedRequest();
        // logout完了までサービスを更新します。
        UpdateUntil(
            *services,
            [&]
            {
                // サインアウト状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::SignedOut;
            },
            "Refresh-race logout did not finish.");
        // requests: refresh応答後のlogoutを含むHTTP要求列。
        const auto requests = backend->Requests();
        // 遅着sessionが公開されず、資格情報削除とlogoutが行われたことを検証します。
        Require(
            storeState->saveCount == 1u
                && storeState->deleteCount == 1u
                && storeState->token.empty()
                && requests.size() == 4u
                && HasBearer(requests.back(), L"race-new-access"),
            "Late refresh was published or its new session was not revoked.");
    }

    // terminal refresh失敗の全経路でaccount保存先をguestへ戻します。
    void TestTerminalRefreshFailuresDetachAccountPersistence()
    {
        // runCase(caseName: ケース名, refreshResponse: 応答, throwFromWorker: 例外指定, crossExpiryWithCompletedWorker: 期限越え, expectedError: 期待error, expectCredentialDelete: 削除期待) 各失敗経路を検証します。
        const auto runCase = [](
            const std::string_view caseName,
            LamaPon::HttpResponse refreshResponse,
            const bool throwFromWorker,
            const bool crossExpiryWithCompletedWorker,
            const std::string_view expectedError,
            const bool expectCredentialDelete)
        {
            // root: 失敗経路ごとの保存先。
            const auto root = CaseRoot(caseName);
            // preferences: terminal失敗後に復元するguest PlayerPrefs。
            LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
            preferences.Load();
            preferences.SetString("owner", "guest");
            // saves: terminal失敗後に復元するguestセーブストア。
            LamaPon::SaveDataStore saves(root / L"Saves");

            // authorized: refresh失敗前に作る初回session。
            auto authorized = SessionJson(
                "terminal-old-access",
                "terminal-old-refresh",
                "terminal-player");
            // 初回poll応答を認証済みにします。
            authorized["status"] = "authorized";
            // backend: login、poll、指定refresh結果を返します。
            auto backend = std::make_shared<ScriptedBackend>(
                std::deque<LamaPon::HttpResponse>{
                    JsonResponse(
                        201u,
                        {
                            { "transactionId", "terminal-transaction" },
                            { "pollToken", "terminal-poll" },
                            {
                                "authorizationUrl",
                                "https://login.example.test/terminal"
                            },
                            { "expiresIn", 300u },
                            { "pollInterval", 1u }
                        }),
                    JsonResponse(200u, authorized),
                    std::move(refreshResponse)
                },
                2u);
            // storeState: credential更新と削除を記録する状態。
            auto storeState = std::make_shared<TokenStoreState>();
            // services: 指定したterminal refresh失敗を実行するサービス。
            // request callbackは要求を記録し、指定時はrefresh workerを例外終了させます。
            auto services =
                LamaPon::Detail::OnlineServicesTestAccess::Create(
                    OnlineConfiguration(),
                    [backend, throwFromWorker](
                        const LamaPon::HttpRequest& request)
                    {
                        // response: backendが返すrefresh応答。
                        auto response = backend->Send(request);
                        // throwFromWorker指定時だけrefresh workerを失敗させます。
                        if (throwFromWorker
                            && request.url.ends_with(
                                L"/v1/auth/session/refresh"))
                        {
                            // transport外の例外経路を再現します。
                            throw std::runtime_error(
                                "private refresh worker exception");
                        }
                        // backend応答をサービスへ返します。
                        return response;
                    },
                    std::make_unique<MemoryTokenStore>(storeState));
            // guest保存先をサービスへ接続します。
            LamaPon::Detail::OnlinePersistenceAccess::Attach(
                *services,
                preferences,
                saves,
                root);
            // refresh失敗前のaccountを有効化します。
            CompleteDiscordLogin(*services);
            // login完了を待ちます。
            UpdateUntil(
                *services,
                [&]
                {
                    // 認証済み状態への到達を判定します。
                    return services->State()
                        == LamaPon::OnlineAccountState::SignedIn;
                },
                "Terminal refresh fixture login did not complete.");
            // accountPath: 失敗前のアカウントPlayerPrefs保存先。
            const auto accountPath = preferences.FilePath();
            preferences.SetString("terminal-account", "durable-value");

            // refresh開始後に期限を越え、全terminal error経路でguestへ戻します。
            services->Update(24.0f);
            // refresh応答を完了させて期限越え経路を選びます。
            backend->WaitUntilRequestBlocked();
            backend->ReleaseBlockedRequest();
            // 完了済みworkerのreap前に期限を越えるケースです。
            if (crossExpiryWithCompletedWorker)
            {
                // deadline: refresh worker完了を待つ期限。
                const auto deadline =
                    std::chrono::steady_clock::now()
                    + std::chrono::seconds(10);
                // worker完了または期限切れまで待機します。
                while (!LamaPon::Detail::OnlineServicesTestAccess::
                    CurrentTaskCompleted(*services))
                {
                    // worker完了待ちの期限切れを検出します。
                    if (std::chrono::steady_clock::now() >= deadline)
                    {
                        // 完了しないrefresh workerを失敗として通知します。
                        throw std::runtime_error(
                            "Terminal refresh worker did not complete.");
                    }
                    std::this_thread::yield();
                }
                // worker結果をreapし、期限越えを観測させます。
                services->Update(6.0f);
            }
            // terminal refresh失敗がErrorへ到達するまで更新します。
            UpdateUntil(
                *services,
                [&]
                {
                    // 認証エラー状態への到達を判定します。
                    return services->State()
                        == LamaPon::OnlineAccountState::Error;
                },
                "Terminal refresh failure did not finish.");
            // sessionとaccount bindingを破棄し、期待errorを公開します。
            Require(
                !services->IsSignedIn()
                    && services->Player().playerId.empty()
                    && services->LastErrorCode() == expectedError
                    && preferences.FilePath()
                        == root / L"PlayerPrefs.json"
                    && saves.Directory() == root / L"Saves"
                    && preferences.GetString("owner") == "guest"
                    && preferences.GetString("terminal-account").empty()
                    && storeState->deleteCount
                        == (expectCredentialDelete ? 1u : 0u),
                "Terminal refresh failure retained the account binding.");
            // persisted: account保存済み値を検証する再読込ストア。
            LamaPon::PlayerPrefs persisted(accountPath);
            persisted.Load();
            // terminal refresh失敗後もaccountデータを保存しています。
            Require(
                persisted.GetString("terminal-account")
                    == "durable-value",
                "Terminal refresh detach lost dirty account data.");
        };

        // invalid token応答では保存資格情報の削除を期待します。
        runCase(
            "terminal-invalid",
            JsonResponse(
                401u,
                {
                    {
                        "error",
                        { { "code", "invalid_refresh_token" } }
                    }
                }),
            false,
            false,
            "stored_session_invalid",
            true);
        // transportFailure: 通信層で発生するrefresh失敗応答。
        LamaPon::HttpResponse transportFailure;
        transportFailure.transportError = "private terminal network error";
        // 通信失敗では旧資格情報を削除しません。
        runCase(
            "terminal-transport",
            std::move(transportFailure),
            false,
            true,
            "network_error",
            false);
        // worker例外では安全のため保存資格情報を削除します。
        runCase(
            "terminal-exception",
            LamaPon::HttpResponse{},
            true,
            true,
            "request_failed",
            true);
    }

    // refresh期限切れ時にguestへ戻し、遅着した更新sessionを失効させます。
    void TestRefreshExpiryImmediatelyDetachesAndRevokesLateSession()
    {
        // runCase(exactExpiryBoundary: 期限境界で完了させるか) 境界と進行中refreshを検証します。
        const auto runCase = [](const bool exactExpiryBoundary)
        {
            // root: 期限境界ケースごとの保存先。
            const auto root = CaseRoot(
                exactExpiryBoundary
                    ? "refresh-expiry-exact"
                    : "refresh-expiry-inflight");
            // preferences: refresh失敗後に復元するguest PlayerPrefs。
            LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
            preferences.Load();
            preferences.SetString("owner", "guest");
            // saves: refresh失敗後に復元するguestセーブストア。
            LamaPon::SaveDataStore saves(root / L"Saves");

            // authorized: 初回refresh前のsession。
            auto authorized = SessionJson(
                "expiry-old-access",
                "expiry-old-refresh",
                "expiry-player");
            // 初回poll応答を認証済みにします。
            authorized["status"] = "authorized";
            // backend: 初回session、late refresh、logout応答。
            auto backend = std::make_shared<ScriptedBackend>(
                std::deque<LamaPon::HttpResponse>{
                    JsonResponse(
                        201u,
                        {
                            { "transactionId", "expiry-transaction" },
                            { "pollToken", "expiry-poll" },
                            {
                                "authorizationUrl",
                                "https://login.example.test/expiry"
                            },
                            { "expiresIn", 300u },
                            { "pollInterval", 1u }
                        }),
                    JsonResponse(200u, authorized),
                    JsonResponse(
                        200u,
                        SessionJson(
                            "expiry-new-access",
                            "expiry-new-refresh",
                            "expiry-player",
                            900u)),
                    JsonResponse(204u)
                },
                2u);
            // storeState: refresh tokenの保存・削除状態。
            auto storeState = std::make_shared<TokenStoreState>();
            // services: refresh期限競合を実行するオンラインサービス。
            // request callbackはHTTP要求をbackendへ転送します。
            auto services =
                LamaPon::Detail::OnlineServicesTestAccess::Create(
                    OnlineConfiguration(),
                    [backend](const LamaPon::HttpRequest& request)
                    {
                        // HTTP要求をbackendへ送り応答を返します。
                        return backend->Send(request);
                    },
                    std::make_unique<MemoryTokenStore>(storeState));
            // guest保存先をオンライン永続化へ接続します。
            LamaPon::Detail::OnlinePersistenceAccess::Attach(
                *services,
                preferences,
                saves,
                root);
            // 初回account sessionを有効化します。
            CompleteDiscordLogin(*services);
            // 初回ログインがSignedInへ進むまでサービスを更新します。
            UpdateUntil(
                *services,
                [&]
                {
                    // 認証済み状態への到達を判定します。
                    return services->State()
                        == LamaPon::OnlineAccountState::SignedIn;
                },
                "Refresh-expiry fixture login did not complete.");
            // accountPath: 期限切れ前のアカウントPlayerPrefs保存先。
            const auto accountPath = preferences.FilePath();
            preferences.SetString("expiry-account", "durable-value");

            // 期限境界ちょうどとrefresh進行中の二経路を切り替えます。
            if (exactExpiryBoundary)
            {
                services->Update(30.0f);
            }
            // 外部refreshが進行中の場合を処理します。
            else
            {
                services->Update(24.0f);
                backend->WaitUntilRequestBlocked();
                services->Update(6.0f);
            }
            // 期限到達で即時にguestへ戻し、tokenを破棄したことを検証します。
            Require(
                services->State()
                        == LamaPon::OnlineAccountState::SigningOut
                    && !services->IsSignedIn()
                    && services->Player().playerId.empty()
                    && services->LastErrorCode() == "request_failed"
                    && preferences.FilePath()
                        == root / L"PlayerPrefs.json"
                    && saves.Directory() == root / L"Saves"
                    && preferences.GetString("owner") == "guest"
                    && preferences.GetString("expiry-account").empty()
                    && storeState->deleteCount == 1u
                    && storeState->token.empty(),
                "Expired in-flight refresh remained publicly signed in.");
            // 期限境界ちょうどのケースはlogout前にrefresh要求を待ちます。
            if (exactExpiryBoundary)
            {
                backend->WaitUntilRequestBlocked();
            }
            // 遅着するrefresh成功応答を返します。
            backend->ReleaseBlockedRequest();
            // 遅着sessionのlogout完了までサービスを更新します。
            UpdateUntil(
                *services,
                [&]
                {
                    // 認証エラー状態への到達を判定します。
                    return services->State()
                        == LamaPon::OnlineAccountState::Error;
                },
                "Expired refresh cleanup did not finish.");

            // requests: 遅着refreshとlogoutを含むHTTP要求列。
            const auto requests = backend->Requests();
            // late sessionが公開されずlogoutされたことを検証します。
            Require(
                requests.size() == 4u
                    && HasBearer(
                        requests.back(),
                        L"expiry-new-access")
                    && preferences.FilePath()
                        == root / L"PlayerPrefs.json"
                    && !services->IsSignedIn(),
                "Late refresh success was published or not revoked.");
            // persisted: 期限切れ後も保つaccount文書を再読込するストア。
            LamaPon::PlayerPrefs persisted(accountPath);
            persisted.Load();
            // guest復帰時にdirtyなaccount値が保存済みであることを検証します。
            Require(
                persisted.GetString("expiry-account")
                    == "durable-value",
                "Expiry detach lost dirty account PlayerPrefs.");
        };

        // refresh進行中に期限を越えるケースを検証します。
        runCase(false);
        // refresh完了直前の期限境界も検証します。
        runCase(true);
    }

    // 完了済みrefresh結果をreapする前にTTL切れを検出してsessionを失効させます。
    void TestCompletedRefreshExpiresBeforeReap()
    {
        // root: 完了済みrefresh期限ケースの保存先。
        const auto root = CaseRoot("refresh-completion-expiry");
        // preferences: 期限切れ後に復元するguest PlayerPrefs。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        // saves: 期限切れ後に復元するguestセーブストア。
        LamaPon::SaveDataStore saves(root / L"Saves");

        // authorized: refresh前の初回session。
        auto authorized = SessionJson(
            "completion-old-access",
            "completion-old-refresh",
            "completion-player",
            30u);
        // login poll結果を認証済みにします。
        authorized["status"] = "authorized";
        // backend: login、短命refresh、logout応答。
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "completion-transaction" },
                        { "pollToken", "completion-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/completion"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(
                    200u,
                    SessionJson(
                        "completion-new-access",
                        "completion-new-refresh",
                        "completion-player",
                        1u)),
                JsonResponse(204u)
            });
        // storeState: refresh tokenの保存状態。
        auto storeState = std::make_shared<TokenStoreState>();
        // services: 完了済みrefresh結果のTTL切れを検証するサービス。
        // request callbackはHTTP要求をbackendへ転送します。
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    // HTTP要求をbackendへ送り応答を返します。
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        // guest保存先をオンライン永続化へ接続します。
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);
        // 初回sessionを有効化します。
        CompleteDiscordLogin(*services);
        // 初回ログインの完了を待ちます。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証済み状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Completion-expiry fixture login did not complete.");
        // guest復帰後に永続化を検証するaccount専用値。
        preferences.SetString("completion-account", "durable-value");

        // 短いrefresh期限を迎える前にworkerを完了させます。
        services->Update(24.0f);
        // worker結果がmailboxへ届くまで待ちます。
        WaitUntilCurrentTaskCompleted(
            *services,
            "Completed refresh was not ready for TTL aging.");
        // 完了結果のTTLを期限切れに進めます。
        Require(
            LamaPon::Detail::OnlineServicesTestAccess::
                AgeCurrentTaskCompletion(*services, 31.0f),
            "Could not age the completed refresh result.");
        // 結果をreapし、期限切れ時の即時切離しを実行します。
        services->Update(0.0f);

        // refresh期限切れ時にguest bindingへ戻りtokenを削除したことを検証します。
        Require(
            services->State()
                    == LamaPon::OnlineAccountState::SigningOut
                && !services->IsSignedIn()
                && services->Player().playerId.empty()
                && preferences.FilePath() == root / L"PlayerPrefs.json"
                && saves.Directory() == root / L"Saves"
                && preferences.GetString("owner") == "guest"
                && preferences.GetString("completion-account").empty()
                && storeState->saveCount == 1u
                && storeState->deleteCount == 1u
                && storeState->token.empty(),
            "An expired completed refresh was published or kept account persistence active.");
        // revoke後のlogoutがErrorへ進むまでサービスを更新します。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証エラー状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::Error;
            },
            "Expired completed refresh cleanup did not finish.");
        // requests: refresh revokeに使われたHTTP要求列。
        const auto requests = backend->Requests();
        // logoutに更新後access tokenが使われ、errorが適切なことを検証します。
        Require(
            requests.size() == 4u
                && HasBearer(
                    requests.back(),
                    L"completion-new-access")
                && services->LastErrorCode() == "request_failed",
            "Expired completed refresh was not revoked exactly once.");
    }

    // 完了済みrestoreとpollの短命sessionをreap前に期限切れとして拒否します。
    void TestCompletedInitialSessionsExpireBeforeReap()
    {
        {
            // root: 保存済みrestore期限ケースの保存先。
            const auto root = CaseRoot("restore-completion-expiry");
            // preferences: 期限切れ後に復元するguest PlayerPrefs。
            LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
            preferences.Load();
            preferences.SetString("owner", "guest");
            // saves: 期限切れ後に復元するguestセーブストア。
            LamaPon::SaveDataStore saves(root / L"Saves");
            // backend: 短命restore応答とlogout応答。
            auto backend = std::make_shared<ScriptedBackend>(
                std::deque<LamaPon::HttpResponse>{
                    JsonResponse(
                        200u,
                        SessionJson(
                            "expired-restore-access",
                            "expired-restore-candidate",
                            "expired-restore-player",
                            1u)),
                    JsonResponse(204u)
                });
            // storeState: 保存済みtokenと削除結果。
            auto storeState = std::make_shared<TokenStoreState>();
            storeState->loadStatus =
                LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
            storeState->token = "expired-restore-stored";
            // disabledConfiguration: token読込後に有効化する初期構成。
            auto disabledConfiguration = OnlineConfiguration();
            disabledConfiguration.serviceBaseUrl.clear();
            // services: 保存済みrestoreの期限を検証するサービス。
            // request callbackはHTTP要求をbackendへ転送します。
            auto services =
                LamaPon::Detail::OnlineServicesTestAccess::Create(
                    std::move(disabledConfiguration),
                    [backend](const LamaPon::HttpRequest& request)
                    {
                        // HTTP要求をbackendへ送り応答を返します。
                        return backend->Send(request);
                    },
                    std::make_unique<MemoryTokenStore>(storeState));
            // guest保存先をオンライン永続化へ接続します。
            LamaPon::Detail::OnlinePersistenceAccess::Attach(
                *services,
                preferences,
                saves,
                root);
            // 有効構成へ変更して保存済みtokenのrestoreを開始します。
            services->Configure(OnlineConfiguration());

            // restore結果がmailboxへ届くまで待ちます。
            WaitUntilCurrentTaskCompleted(
                *services,
                "Completed restore was not ready for TTL aging.");
            // 完了済みrestore結果のTTLを期限切れに進めます。
            Require(
                LamaPon::Detail::OnlineServicesTestAccess::
                    AgeCurrentTaskCompletion(*services, 31.0f),
                "Could not age the completed restore.");
            // 結果をreapし期限切れsessionをlogoutします。
            services->Update(0.0f);
            // restore期限切れでguest bindingへ戻りtokenを削除します。
            Require(
                services->State()
                        == LamaPon::OnlineAccountState::SigningOut
                    && !services->IsSignedIn()
                    && preferences.FilePath()
                        == root / L"PlayerPrefs.json"
                    && saves.Directory() == root / L"Saves"
                    && preferences.GetString("owner") == "guest"
                    && storeState->saveCount == 0u
                    && storeState->deleteCount == 1u
                    && storeState->token.empty(),
                "Expired completed restore exposed an account binding or credential.");
            // logout完了後にError状態へ進むまで更新します。
            UpdateUntil(
                *services,
                [&]
                {
                    // 認証エラー状態への到達を判定します。
                    return services->State()
                        == LamaPon::OnlineAccountState::Error;
                },
                "Expired completed restore cleanup did not finish.");
            // requests: restoreとlogoutで送信されたHTTP要求。
            const auto requests = backend->Requests();
            // expiryしたaccess tokenがrevokeされたことを検証します。
            Require(
                requests.size() == 2u
                    && HasBearer(
                        requests.back(),
                        L"expired-restore-access"),
                "Expired completed restore access token was not revoked.");
        }

        {
            // root: 完了済みpoll期限ケースの保存先。
            const auto root = CaseRoot("poll-completion-expiry");
            // preferences: 期限切れ後に復元するguest PlayerPrefs。
            LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
            preferences.Load();
            preferences.SetString("owner", "guest");
            // saves: 期限切れ後に復元するguestセーブストア。
            LamaPon::SaveDataStore saves(root / L"Saves");
            // authorized: 有効期限の短いpoll成功応答。
            auto authorized = SessionJson(
                "expired-poll-access",
                "expired-poll-candidate",
                "expired-poll-player",
                1u);
            // poll結果を認証済み状態にします。
            authorized["status"] = "authorized";
            // backend: login開始、poll、revoke応答。
            auto backend = std::make_shared<ScriptedBackend>(
                std::deque<LamaPon::HttpResponse>{
                    JsonResponse(
                        201u,
                        {
                            {
                                "transactionId",
                                "expired-poll-transaction"
                            },
                            { "pollToken", "expired-poll-secret" },
                            {
                                "authorizationUrl",
                                "https://login.example.test/expired-poll"
                            },
                            { "expiresIn", 300u },
                            { "pollInterval", 1u }
                        }),
                    JsonResponse(200u, authorized),
                    JsonResponse(204u)
                });
            // storeState: token保存・削除結果を記録する状態。
            auto storeState = std::make_shared<TokenStoreState>();
            // services: 完了済みpollの期限切れを検証するサービス。
            // request callbackはHTTP要求をbackendへ転送します。
            auto services =
                LamaPon::Detail::OnlineServicesTestAccess::Create(
                    OnlineConfiguration(),
                    [backend](const LamaPon::HttpRequest& request)
                    {
                        // HTTP要求をbackendへ送り応答を返します。
                        return backend->Send(request);
                    },
                    std::make_unique<MemoryTokenStore>(storeState));
            // guest保存先をオンライン永続化へ接続します。
            LamaPon::Detail::OnlinePersistenceAccess::Attach(
                *services,
                preferences,
                saves,
                root);
            // poll成功までの認証フローを開始します。
            CompleteDiscordLogin(*services);
            // poll worker結果をmailboxまで進めます。
            WaitUntilCurrentTaskCompleted(
                *services,
                "Completed poll was not ready for TTL aging.");
            // 完了poll結果のTTLを期限切れに進めます。
            Require(
                LamaPon::Detail::OnlineServicesTestAccess::
                    AgeCurrentTaskCompletion(*services, 31.0f),
                "Could not age the completed poll.");
            // 結果をreapし有効期限切れを判定します。
            services->Update(0.0f);
            // poll tokenを保存せずguest bindingを維持します。
            Require(
                services->State()
                        == LamaPon::OnlineAccountState::SigningOut
                    && !services->IsSignedIn()
                    && preferences.FilePath()
                        == root / L"PlayerPrefs.json"
                    && saves.Directory() == root / L"Saves"
                    && preferences.GetString("owner") == "guest"
                    && storeState->saveCount == 0u
                    && storeState->deleteCount == 0u,
                "Expired completed authorization exposed an account binding or credential.");
            // 発行された短命sessionのlogout完了を待ちます。
            UpdateUntil(
                *services,
                [&]
                {
                    // 認証エラー状態への到達を判定します。
                    return services->State()
                        == LamaPon::OnlineAccountState::Error;
                },
                "Expired completed authorization cleanup did not finish.");
            // requests: poll成功とsession revokeのHTTP要求。
            const auto requests = backend->Requests();
            // expired poll access tokenがrevokeされたことを検証します。
            Require(
                requests.size() == 3u
                    && HasBearer(
                        requests.back(),
                        L"expired-poll-access"),
                "Expired completed authorization access token was not revoked.");
        }
    }

    // refresh要求中のOnlineServices破棄は待機せず即時にguestへ戻します。
    void TestDestructionDuringRefreshImmediatelyRestoresGuest()
    {
        // root: destructor中のrefresh競合ケース保存先。
        const auto root = CaseRoot("destructor-refresh");
        // preferences: 破棄時にguestへ戻すPlayerPrefs。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        // saves: 破棄時にguestへ戻すセーブストア。
        LamaPon::SaveDataStore saves(root / L"Saves");

        // authorized: 初回サインインで返すsession。
        auto authorized = SessionJson(
            "destructor-old-access",
            "destructor-old-refresh",
            "destructor-player");
        // poll応答を認証済みにします。
        authorized["status"] = "authorized";
        // backend: login、refresh、logout応答を返しrefreshを停止します。
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "destructor-transaction" },
                        { "pollToken", "destructor-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/destructor"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(
                    200u,
                    SessionJson(
                        "destructor-new-access",
                        "destructor-new-refresh",
                        "destructor-player",
                        900u)),
                JsonResponse(204u)
            },
            2u);
        // storeState: credential削除の呼出し状態。
        auto storeState = std::make_shared<TokenStoreState>();
        // services: 破棄中のrefresh cleanupを実行するサービス。
        // request callbackはHTTP要求をbackendへ転送します。
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    // HTTP要求をbackendへ送り応答を返します。
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        // guest保存先をオンライン永続化へ接続します。
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);
        // 初回account sessionを有効化します。
        CompleteDiscordLogin(*services);
        // 初回login完了を待ちます。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証済み状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Destructor fixture login did not complete.");
        // 破棄後にguestから見えないaccount専用値。
        preferences.SetString("destructor-account", "private");
        // proactive refreshを開始します。
        services->Update(24.0f);
        // refresh要求がworker内で停止するまで待ちます。
        backend->WaitUntilRequestBlocked();

        // started: デストラクター所要時間の基準時刻。
        const auto started = std::chrono::steady_clock::now();
        // サービスを破棄して直ちにguestへ復帰させます。
        services.reset();
        // elapsed: 破棄とguest復帰に要した時間。
        const auto elapsed =
            std::chrono::steady_clock::now() - started;
        // 破棄が短時間で終わりaccount bindingを残さないことを検証します。
        Require(
            elapsed < std::chrono::seconds(1)
                && preferences.FilePath() == root / L"PlayerPrefs.json"
                && saves.Directory() == root / L"Saves"
                && preferences.GetString("owner") == "guest"
                && preferences.GetString("destructor-account").empty()
                && storeState->deleteCount == 1u
                && storeState->token.empty(),
            "OnlineServices destruction waited or retained account binding.");
        // 破棄後もworkerが受け取れるlate refresh responseを返します。
        backend->ReleaseBlockedRequest();
        // 遅着sessionのrevoke要求が到着するまで待ちます。
        WaitUntilRequestCount(
            backend,
            4u,
            "Late refresh success was not revoked after destruction.");
        // requests: 破棄後のlogout要求を含むHTTP要求列。
        const auto requests = backend->Requests();
        // 遅着sessionがrevokeされ、credential削除が重複しないことを検証します。
        Require(
            requests.size() == 4u
                && HasBearer(
                    requests.back(),
                    L"destructor-new-access")
                && storeState->deleteCount == 1u,
            "Destroyed refresh performed incomplete or duplicate cleanup.");
    }

    // worker完了後に結果をreapせず破棄しても更新sessionを失効させます。
    void TestCompletedUnreapedRefreshIsRevokedAfterDestruction()
    {
        // authorized: 初回サインインで返すsession。
        auto authorized = SessionJson(
            "unreaped-old-access",
            "unreaped-old-refresh",
            "unreaped-player");
        // poll応答を認証済み状態にします。
        authorized["status"] = "authorized";
        // backend: login、refresh、logout応答。
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "unreaped-transaction" },
                        { "pollToken", "unreaped-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/unreaped"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(
                    200u,
                    SessionJson(
                        "unreaped-new-access",
                        "unreaped-new-refresh",
                        "unreaped-player",
                        900u)),
                JsonResponse(204u)
            });
        // storeState: credentialの保存・削除状態。
        auto storeState = std::make_shared<TokenStoreState>();
        // services: 未reap refresh結果の破棄時cleanupを検証します。
        // request callbackはHTTP要求をbackendへ転送します。
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    // HTTP要求をbackendへ送り応答を返します。
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        // account sessionを有効化します。
        CompleteDiscordLogin(*services);
        // SignedInへ進むまでサービスを更新します。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証済み状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Completed-unreaped fixture login did not complete.");
        // proactive refreshを開始します。
        services->Update(24.0f);
        // refresh中状態を確認します。
        Require(
            services->State()
                == LamaPon::OnlineAccountState::RefreshingSession,
            "Completed-unreaped refresh did not start.");

        // deadline: refresh worker完了を待つ上限時刻。
        const auto deadline =
            std::chrono::steady_clock::now()
            + std::chrono::seconds(10);
        // refresh結果がmailboxへ届くか期限切れまで待ちます。
        while (!LamaPon::Detail::OnlineServicesTestAccess::
            CurrentTaskCompleted(*services))
        {
            // worker完了待ちの期限切れを検出します。
            if (std::chrono::steady_clock::now() >= deadline)
            {
                // refresh workerの完了遅延を通知します。
                throw std::runtime_error(
                    "Refresh worker did not complete before destruction.");
            }
            std::this_thread::yield();
        }

        // 完了結果をreapする前にサービスを破棄します。
        services.reset();
        // 破棄時に旧tokenを削除したことを検証します。
        Require(
            storeState->deleteCount == 1u
                && storeState->token.empty(),
            "Completed-unreaped refresh retained its old credential.");
        // 遅着refresh sessionのlogout要求が届くまで待ちます。
        WaitUntilRequestCount(
            backend,
            4u,
            "Completed-unreaped refresh did not revoke its new access token.");
        // requests: refresh revokeに使われたHTTP要求。
        const auto requests = backend->Requests();
        // logoutで新access tokenを使用し、credential削除を重複しないことを検証します。
        Require(
            requests.size() == 4u
                && HasBearer(requests.back(), L"unreaped-new-access")
                && storeState->deleteCount == 1u,
            "Completed-unreaped refresh cleanup ran incorrectly.");
    }

    // 保存済みsession restore中の破棄はtokenを削除し、遅着sessionを失効させます。
    void TestDestroyedRestoreDeletesCredentialAndRevokesLateSession()
    {
        // backend: restore応答とlogout応答を返し、restoreを停止します。
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    200u,
                    SessionJson(
                        "destroyed-restore-access",
                        "destroyed-restore-refresh",
                        "destroyed-restore-player",
                        900u)),
                JsonResponse(204u)
            },
            0u);
        // storeState: restore前に保存済みtokenを保持する状態。
        auto storeState = std::make_shared<TokenStoreState>();
        storeState->loadStatus =
            LamaPon::Detail::RefreshTokenLoadStatus::Loaded;
        storeState->token = "destroyed-restore-old-refresh";
        // services: 保存済みrestore中に破棄するオンラインサービス。
        // request callbackはHTTP要求をbackendへ転送します。
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    // HTTP要求をbackendへ送り応答を返します。
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        // restore要求がbackend内で停止するまで待ちます。
        backend->WaitUntilRequestBlocked();

        // started: restore中の破棄時間を計る開始時刻。
        const auto started = std::chrono::steady_clock::now();
        // restore中に破棄して保存済みcredentialを削除します。
        services.reset();
        // elapsed: restore中の破棄所要時間。
        const auto elapsed =
            std::chrono::steady_clock::now() - started;
        // 破棄を待たずに完了しtokenを削除したことを検証します。
        Require(
            elapsed < std::chrono::seconds(1)
                && storeState->deleteCount == 1u
                && storeState->token.empty(),
            "Destroyed restore waited or retained its old credential.");
        // 遅着restore sessionを受け取れるようworkerを解放します。
        backend->ReleaseBlockedRequest();
        // logout要求が送信されるまで待ちます。
        WaitUntilRequestCount(
            backend,
            2u,
            "Destroyed restore did not revoke its late session.");
        // requests: restoreとlate session revokeの要求列。
        const auto requests = backend->Requests();
        // restore token削除を一度だけ行い、late sessionをlogoutしたことを検証します。
        Require(
            requests.size() == 2u
                && HasBearer(
                    requests.back(),
                    L"destroyed-restore-access")
                && storeState->deleteCount == 1u,
            "Destroyed restore cleanup ran incorrectly.");
    }

    // poll中の破棄は無関係なcredentialを保ち、遅着sessionを失効させます。
    void TestDestroyedPollRevokesLateAuthorizedSession()
    {
        // authorized: poll完了後に発行されるsession。
        auto authorized = SessionJson(
            "destroyed-poll-access",
            "destroyed-poll-refresh",
            "destroyed-poll-player");
        // poll応答を認証済み状態にします。
        authorized["status"] = "authorized";
        // backend: login、poll、late session logoutを返します。
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "destroyed-poll-transaction" },
                        { "pollToken", "destroyed-poll-secret" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/destroyed-poll"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(204u)
            },
            1u);
        // storeState: poll破棄前後の保存credential状態。
        auto storeState = std::make_shared<TokenStoreState>();
        // services: authorization poll中に破棄するオンラインサービス。
        // request callbackはHTTP要求をbackendへ転送します。
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    // HTTP要求をbackendへ送り応答を返します。
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        // 認証要求を開始します。
        Require(
            services->BeginDiscordSignIn(),
            "Destroyed-poll login did not start.");
        // 認証ページ待ち状態へ進めます。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証待ち状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::WaitingForAuthorization;
            },
            "Destroyed-poll login start did not complete.");
        // poll要求を開始します。
        services->Update(1.0f);
        // poll応答がbackend内で停止するまで待ちます。
        backend->WaitUntilRequestBlocked();

        // poll中の破棄では無関係な保存credentialを消しません。
        services.reset();
        Require(
            storeState->deleteCount == 0u,
            "Destroyed poll deleted an unrelated stored credential.");
        // authorization済み応答をworkerへ返します。
        backend->ReleaseBlockedRequest();
        // late session revoke要求が届くまで待ちます。
        WaitUntilRequestCount(
            backend,
            3u,
            "Destroyed poll did not revoke its late authorized session.");
        // requests: login、poll、revokeのHTTP要求列。
        const auto requests = backend->Requests();
        // late sessionは失効し、保存済みtokenの削除は行われません。
        Require(
            requests.size() == 3u
                && HasBearer(
                    requests.back(),
                    L"destroyed-poll-access")
                && storeState->deleteCount == 0u,
            "Destroyed poll cleanup ran incorrectly.");
    }

    // 通常のSignedIn破棄では次回restore用refresh tokenを保持します。
    void TestSignedInDestructionPreservesStoredRefreshToken()
    {
        // authorized: token保存を行う初回session。
        auto authorized = SessionJson(
            "preserved-access",
            "preserved-refresh",
            "preserved-player",
            900u);
        // poll応答を認証済みにします。
        authorized["status"] = "authorized";
        // backend: login開始と認証完了の応答列。
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "preserved-transaction" },
                        { "pollToken", "preserved-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/preserved"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized)
            });
        // storeState: refresh token保存状態。
        auto storeState = std::make_shared<TokenStoreState>();
        // services: 通常SignIn後に破棄するオンラインサービス。
        // request callbackはHTTP要求をbackendへ転送します。
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    // HTTP要求をbackendへ送り応答を返します。
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        // accountへサインインします。
        CompleteDiscordLogin(*services);
        // SignedInへ到達するまで更新します。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証済み状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Preserved-credential fixture login did not complete.");
        // SignOutを介さず破棄し、保存済みcredentialを保持します。
        services.reset();

        // 通常破棄ではlogoutやtoken削除が発生しないことを検証します。
        Require(
            storeState->saveCount == 1u
                && storeState->deleteCount == 0u
                && storeState->token == "preserved-refresh"
                && backend->Requests().size() == 2u,
            "Normal signed-in destruction discarded the resumable credential.");
    }

    // SignOut中の破棄は失敗したcredential削除を再試行します。
    void TestDestroyedSignOutRetriesFailedCredentialDeletion()
    {
        // authorized: refresh前に有効化するsession。
        auto authorized = SessionJson(
            "retry-delete-old-access",
            "retry-delete-old-refresh",
            "retry-delete-player");
        // poll応答を認証済みにします。
        authorized["status"] = "authorized";
        // backend: login、refresh、logout応答を返しrefreshを停止します。
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "retry-delete-transaction" },
                        { "pollToken", "retry-delete-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/retry-delete"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(
                    200u,
                    SessionJson(
                        "retry-delete-new-access",
                        "retry-delete-new-refresh",
                        "retry-delete-player",
                        900u)),
                JsonResponse(204u)
            },
            2u);
        // storeState: token削除失敗とdestructor再試行の状態。
        auto storeState = std::make_shared<TokenStoreState>();
        // services: SignOutと破棄時のcredential再試行を行うサービス。
        // request callbackはHTTP要求をbackendへ転送します。
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    // HTTP要求をbackendへ送り応答を返します。
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        // accountへサインインします。
        CompleteDiscordLogin(*services);
        // SignedIn状態まで更新します。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証済み状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Delete-retry fixture login did not complete.");
        // refresh要求中のSignOut競合を開始します。
        services->Update(24.0f);
        // refresh要求がbackend内で停止するまで待ちます。
        backend->WaitUntilRequestBlocked();

        // SignOut中の一回目credential削除を失敗させます。
        storeState->failDelete = true;
        // logoutと即時guest復帰を開始します。
        services->SignOut();
        // 失敗credentialがpendingのまま残ることを検証します。
        Require(
            services->State()
                    == LamaPon::OnlineAccountState::SigningOut
                && storeState->deleteCount == 1u
                && !storeState->token.empty(),
            "Delete-retry fixture did not retain the failed credential.");
        // destructorの再試行を成功させます。
        storeState->failDelete = false;
        // SignOut完了前に破棄し、pending削除を再試行します。
        services.reset();
        // 破棄時にcredentialをちょうど一度再削除したことを検証します。
        Require(
            storeState->deleteCount == 2u
                && storeState->token.empty(),
            "Destruction did not retry the failed SignOut credential delete.");

        // 遅着refresh応答を返してlate session revokeを許可します。
        backend->ReleaseBlockedRequest();
        // revoke要求がbackendへ届くまで待ちます。
        WaitUntilRequestCount(
            backend,
            4u,
            "Destroyed SignOut did not revoke the late rotated session.");
        // requests: late session revokeを含むHTTP要求。
        const auto requests = backend->Requests();
        // late access tokenを失効させ、credential削除を重複しないことを検証します。
        Require(
            requests.size() == 4u
                && HasBearer(
                    requests.back(),
                    L"retry-delete-new-access")
                && storeState->deleteCount == 2u,
            "Destroyed SignOut duplicated or skipped abandoned cleanup.");
    }

    // logout完了後も失敗したcredential削除を保留し、破棄時に再試行します。
    void TestCompletedSignOutKeepsFailedCredentialDeletePending()
    {
        // authorized: SignOut対象の初回session。
        auto authorized = SessionJson(
            "completed-signout-access",
            "completed-signout-refresh",
            "completed-signout-player",
            900u);
        // poll応答を認証済みにします。
        authorized["status"] = "authorized";
        // backend: login、poll、logout応答。
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        {
                            "transactionId",
                            "completed-signout-transaction"
                        },
                        { "pollToken", "completed-signout-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/completed-signout"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(204u)
            });
        // storeState: credential削除失敗と再試行の状態。
        auto storeState = std::make_shared<TokenStoreState>();
        // services: logout完了後の削除pendingを検証するサービス。
        // request callbackはHTTP要求をbackendへ転送します。
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    // HTTP要求をbackendへ送り応答を返します。
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        // accountへサインインします。
        CompleteDiscordLogin(*services);
        // SignedIn状態まで更新します。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証済み状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Completed-SignOut fixture login did not complete.");

        // 最初のcredential削除を失敗させます。
        storeState->failDelete = true;
        // SignOutを開始してlogout処理を完了させます。
        services->SignOut();
        // logout後も削除エラーとtokenを保持することを検証します。
        UpdateUntil(
            *services,
            [&]
            {
                // サインアウト状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::SignedOut;
            },
            "SignOut logout did not complete after credential failure.");
        // logout完了後に失敗credential削除をpendingのまま維持します。
        Require(
            services->LastErrorCode() == "credential_delete_failed"
                && storeState->deleteCount == 1u
                && !storeState->token.empty(),
            "Completed SignOut forgot its failed credential deletion.");

        // destructorの削除再試行を成功させます。
        storeState->failDelete = false;
        // 保留credential削除をサービス破棄で再試行します。
        services.reset();
        // 削除再試行後にtokenを空にします。
        Require(
            storeState->deleteCount == 2u
                && storeState->token.empty(),
            "Destructor did not retry a completed SignOut deletion failure.");
    }

    // 失敗するConfigureは認証client、永続化namespace、guest bindingを変更しません。
    void TestConfigureFailurePreservesClientNamespaceAndBinding()
    {
        // root: Configure失敗保証ケースの保存先。
        const auto root = CaseRoot("configure-strong-guarantee");
        // preferences: guestとaccountで再利用するPlayerPrefs。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        preferences.Save();
        // saves: guestとaccountで再利用するセーブストア。
        LamaPon::SaveDataStore saves(root / L"Saves");

        // authorized: 初回ログイン完了時のsession。
        auto authorized = SessionJson(
            "configure-access",
            "configure-refresh",
            "configure-player",
            900u);
        // poll応答を認証済みにします。
        authorized["status"] = "authorized";
        // backend: login、logout、旧client probeの応答列。
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "configure-transaction" },
                        { "pollToken", "configure-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/configure"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(204u),
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "old-client-probe" },
                        { "pollToken", "old-client-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/old-client"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    })
            });
        // storeState: refresh token保存状態。
        auto storeState = std::make_shared<TokenStoreState>();
        // services: 失敗Configure前後のclientを比較するサービス。
        // request callbackはHTTP要求をbackendへ転送します。
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    // HTTP要求をbackendへ送り応答を返します。
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        // guest保存先をサービスへ接続します。
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);
        // 旧namespaceのaccountへサインインします。
        CompleteDiscordLogin(*services);
        // 初回サインインが完了するまで更新します。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証済み状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Configure fixture login did not complete.");
        // diskへ保存する値とmemoryのみの値を分けて準備します。
        preferences.SetString("durable", "before-lock");
        preferences.Save();
        preferences.SetString("quarantined", "memory");
        // accountPath: write lockを作る現行account文書。
        const auto accountPath = preferences.FilePath();
        // lock: account文書のRecovery sidecar作成を妨げるハンドル。
        FileHandle lock;
        lock.value = CreateFileW(
            accountPath.c_str(),
            GENERIC_READ,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        // Configure失敗を起こすfile lockを取得します。
        Require(
            lock.value != INVALID_HANDLE_VALUE,
            "Could not lock Configure account fixture.");

        // accountを切り離し、書込失敗でmemory quarantineを作ります。
        services->SignOut();
        // logoutと隔離状態の準備が完了するまで更新します。
        UpdateUntil(
            *services,
            [&]
            {
                // サインアウト状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::SignedOut;
            },
            "Configure fixture logout did not finish.");
        // coordinator: serviceが使用する永続化調整器。
        auto* const coordinator =
            LamaPon::Detail::OnlinePersistenceAccess::Coordinator(
                *services);
        // lockされたaccount memoryが隔離状態で残ることを確認します。
        Require(
            coordinator && coordinator->HasQuarantinedAccount(),
            "Configure fixture did not retain quarantined memory.");
        // oldEpoch: Configure前のnamespace世代。
        const auto oldEpoch = coordinator->ProfileEpoch();

        // replacement: 適用失敗させる別clientとnamespace構成。
        auto replacement = OnlineConfiguration();
        replacement.serviceBaseUrl =
            "https://replacement.example.test/new-tenant";
        replacement.gameId = "replacement-game";
        replacement.environmentId = "replacement";
        // 失敗Configure後も旧client、namespace、guest bindingを保ちます。
        Require(
            Throws([&]
            {
                services->Configure(std::move(replacement));
            })
                && coordinator->ProfileEpoch() == oldEpoch
                && coordinator->HasQuarantinedAccount()
                && preferences.FilePath() == root / L"PlayerPrefs.json"
                && saves.Directory() == root / L"Saves"
                && preferences.GetString("owner") == "guest"
                && services->State()
                    == LamaPon::OnlineAccountState::SignedOut,
            "Failed Configure changed client state, namespace, or binding.");

        // sidecar書込みを妨げていたfile lockを解放します。
        lock.Close();
        // 隔離されたmemory復旧を進めます。
        coordinator->EndFrame();
        // recoveryが完了してquarantineを解放することを確認します。
        Require(
            !coordinator->HasQuarantinedAccount(),
            "Configure fixture quarantine did not recover.");
        // oldProfiles: Configure前のnamespaceで解決する保存先。
        const LamaPon::PersistenceProfiles oldProfiles(
            root,
            "coordinator-game",
            "test");
        // replacementProfiles: 新namespaceが使われた場合の比較先。
        const LamaPon::PersistenceProfiles replacementProfiles(
            root,
            "replacement-game",
            "replacement");
        // expectedOld: 維持される旧namespaceのaccount文書。
        const auto expectedOld = oldProfiles.Account("namespace-probe");
        // unexpectedReplacement: 誤適用された場合の新namespace文書。
        const auto unexpectedReplacement =
            replacementProfiles.Account("namespace-probe");
        // prepared: 旧namespaceでのaccount切替。
        auto prepared = coordinator->PrepareAccount(
            "namespace-probe",
            "namespace-probe-access-token");
        // 切離し後も旧namespaceの保存先であることを検証します。
        Require(
            coordinator->CommitPrepared(std::move(prepared))
                && preferences.FilePath() == expectedOld.playerPrefsFile
                && preferences.FilePath()
                    != unexpectedReplacement.playerPrefsFile,
            "Failed Configure replaced the persistence namespace.");
        // probe前にaccount bindingをguestへ戻します。
        static_cast<void>(coordinator->DetachToGuest());

        // 失敗Configure後も旧authentication clientを再利用できます。
        Require(
            services->BeginDiscordSignIn(),
            "Old client was unusable after failed Configure.");
        // 旧clientから認証待ち状態へ進むまで更新します。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証待ち状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::WaitingForAuthorization;
            },
            "Old client probe did not complete.");
        // requests: 失敗Configure後にbackendが受信した要求列。
        const auto requests = backend->Requests();
        // probe要求が旧接続先へ送られたことを検証します。
        Require(
            requests.size() == 4u
                && requests.back().url.starts_with(
                    L"https://online.example.test/tenant/"),
            "Failed Configure replaced the active authentication client.");
        // 確認用の認証フローを終了します。
        services->CancelDiscordSignIn();
    }

    // refresh応答のplayer ID変更はfail-closedしguest保存先へ戻します。
    void TestRefreshIdentityChangeFailsClosed()
    {
        // root: refresh identity変更ケースの保存先。
        const auto root = CaseRoot("refresh-identity-change");
        // preferences: identity失敗後に復元するguest PlayerPrefs。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        preferences.Load();
        preferences.SetString("owner", "guest");
        // saves: identity失敗後に復元するguestセーブストア。
        LamaPon::SaveDataStore saves(root / L"Saves");

        // authorized: 最初にログインするaccount A session。
        auto authorized = SessionJson(
            "account-a-access",
            "account-a-refresh",
            "player-A");
        // 初回poll応答を認証済み状態にします。
        authorized["status"] = "authorized";
        // backend: account A login、account B refresh、logout応答。
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "identity-transaction" },
                        { "pollToken", "identity-poll" },
                        {
                            "authorizationUrl",
                            "https://login.example.test/identity"
                        },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(
                    200u,
                    SessionJson(
                        "account-b-access",
                        "account-b-refresh",
                        "player-B")),
                JsonResponse(204u)
            });
        // storeState: refresh token保存・削除状態。
        auto storeState = std::make_shared<TokenStoreState>();
        // services: refresh identity変更を拒否するサービス。
        // request callbackはHTTP要求をbackendへ転送します。
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    // HTTP要求をbackendへ送り応答を返します。
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        // guest保存先をサービスへ接続します。
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);

        // 最初のaccount Aへサインインします。
        CompleteDiscordLogin(*services);
        // login完了までサービスを更新します。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証済み状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Account A login did not complete.");
        // account Aのidentityと保存先が有効であることを確認します。
        Require(
            services->Player().playerId == "player-A"
                && preferences.FilePath() != root / L"PlayerPrefs.json",
            "Account A persistence was not activated.");

        // 別identityを返すrefreshを開始します。
        services->Update(24.0f);
        // identity不一致がterminal Errorになるまで更新します。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証エラー状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::Error;
            },
            "Cross-account refresh was not rejected.");

        // requests: 不一致sessionのrevokeを含むHTTP要求列。
        const auto requests = backend->Requests();
        // cross-account refreshを拒否してguest bindingとcredentialを戻します。
        Require(
            !services->IsSignedIn()
                && services->Player().playerId.empty()
                && preferences.FilePath() == root / L"PlayerPrefs.json"
                && saves.Directory() == root / L"Saves"
                && preferences.GetString("owner") == "guest"
                && storeState->saveCount == 1u
                && storeState->deleteCount == 1u
                && storeState->token.empty()
                && requests.size() == 4u
                && HasBearer(requests.back(), L"account-b-access"),
            "Cross-account refresh changed the active account profile.");
    }

    // TestCloudUnauthorizedRefreshUsesCooldownUntilWireSuccess(): cloud 401はwire成功までcooldownしtoken世代を維持します。
    void TestCloudUnauthorizedRefreshUsesCooldownUntilWireSuccess()
    {
        // root: cloud 401 cooldownケースの保存先。
        const auto root = CaseRoot("cloud-401-cooldown");
        // preferences: cloud同期へ変更を通知するPlayerPrefs。
        LamaPon::PlayerPrefs preferences(root / L"PlayerPrefs.json");
        // guest状態のPlayerPrefsを読み込みます。
        preferences.Load();
        // saves: cloud同期対象のローカル保存先。
        LamaPon::SaveDataStore saves(root / L"Saves");

        // authorized: 初回pollで返す有効なsession。
        auto authorized = SessionJson(
            "cloud-access-1",
            "cloud-refresh-1",
            "cloud-player",
            900u);
        // 初回poll応答を認証済み状態にします。
        authorized["status"] = "authorized";
        // backend: login後のcloud 401とrefresh応答列。
        auto backend = std::make_shared<ScriptedBackend>(
            std::deque<LamaPon::HttpResponse>{
                JsonResponse(
                    201u,
                    {
                        { "transactionId", "cloud-transaction" },
                        { "pollToken", "cloud-poll" },
                        { "authorizationUrl", "https://login.example.test/cloud" },
                        { "expiresIn", 300u },
                        { "pollInterval", 1u }
                    }),
                JsonResponse(200u, authorized),
                JsonResponse(401u),
                JsonResponse(
                    200u,
                    SessionJson(
                        "cloud-access-2",
                        "cloud-refresh-2",
                        "cloud-player",
                        900u)),
                JsonResponse(401u),
                JsonResponse(
                    200u,
                    SessionJson(
                        "cloud-access-3",
                        "cloud-refresh-3",
                        "cloud-player",
                        900u)),
                EmptyCloudManifestResponse(),
                JsonResponse(401u),
                JsonResponse(
                    200u,
                    SessionJson(
                        "cloud-access-4",
                        "cloud-refresh-4",
                        "cloud-player",
                        900u))
            });
        // storeState: refresh token保存・削除状態。
        auto storeState = std::make_shared<TokenStoreState>();
        // services: scripted backendで動くonline service。
        // request: HTTP要求をscripted backendへ転送します。
        auto services =
            LamaPon::Detail::OnlineServicesTestAccess::Create(
                OnlineConfiguration(),
                [backend](const LamaPon::HttpRequest& request)
                {
                    // HTTP要求をbackendへ送り応答を返します。
                    return backend->Send(request);
                },
                std::make_unique<MemoryTokenStore>(storeState));
        // guest保存先をserviceへ接続します。
        LamaPon::Detail::OnlinePersistenceAccess::Attach(
            *services,
            preferences,
            saves,
            root);
        // Discord loginを完了させます。
        CompleteDiscordLogin(*services);
        // 認証済み状態になるまでserviceを更新します。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証済み状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Cloud cooldown login did not complete.");
        // coordinator: serviceが使う永続化調整器。
        auto* const coordinator =
            LamaPon::Detail::OnlinePersistenceAccess::Coordinator(*services);
        // accountにcloud synchronizerが有効化されたことを確認します。
        Require(
            coordinator && coordinator->Synchronizer(),
            "Cloud synchronizer was not activated with the account.");

        // pumpUntilRequestCount(count): 指定要求数へ達するまでframeを進めます。
        const auto pumpUntilRequestCount = [&](const std::size_t count)
        {
            // deadline: 非同期要求を待つ期限。
            const auto deadline = std::chrono::steady_clock::now()
                + std::chrono::seconds(10);
            // 必要数の要求が届くまでframe処理を進めます。
            while (backend->Requests().size() < count)
            {
                // cloud同期のframe処理を実行します。
                LamaPon::Detail::OnlinePersistenceAccess::EndFrame(*services);
                // online serviceの非同期状態を進めます。
                services->Update(0.0f);
                // 待機期限切れを検出します。
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    // 要求列が進まない場合はケースを失敗させます。
                    throw std::runtime_error(
                        "Cloud cooldown request sequence timed out.");
                }
                // worker threadへ実行機会を渡します。
                std::this_thread::yield();
            }
        };
        // pumpUntilCloudState(expected): 同期器が指定状態へ達するまでframeを進めます。
        const auto pumpUntilCloudState = [&](const auto expected)
        {
            // deadline: cloud状態遷移を待つ期限。
            const auto deadline = std::chrono::steady_clock::now()
                + std::chrono::seconds(10);
            // 同期器が指定状態になるまでframe処理を進めます。
            while (coordinator->Synchronizer()->Status().state != expected)
            {
                // cloud同期のframe処理を実行します。
                LamaPon::Detail::OnlinePersistenceAccess::EndFrame(*services);
                // online serviceの非同期状態を進めます。
                services->Update(0.0f);
                // 待機期限切れを検出します。
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    // 状態遷移が止まった場合はケースを失敗させます。
                    throw std::runtime_error(
                        "Cloud cooldown state transition timed out.");
                }
                // worker threadへ実行機会を渡します。
                std::this_thread::yield();
            }
        };

        // 初回cloud要求とrefresh要求が届くまで更新します。
        pumpUntilRequestCount(4u);
        // refresh後も認証済み状態になるまで待ちます。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証状態と要求数の両方を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn
                    && backend->Requests().size() >= 4u;
            },
            "First cloud-triggered refresh did not complete.");
        // cloud 401要求が届くまで更新します。
        pumpUntilRequestCount(5u);
        // synchronizerがUnauthorizedへ遷移するまで待ちます。
        pumpUntilCloudState(
            LamaPon::Detail::CloudSaveSynchronizerState::Unauthorized);
        // Unauthorized状態をserviceへ反映します。
        services->Update(0.0f);

        // cooldown期間内に追加refreshがないことを確認します。
        services->Update(4.9f);
        // cooldown前に要求数が変わらないことを検証します。
        Require(
            backend->Requests().size() == 5u,
            "A repeated cloud 401 bypassed the minimum cooldown.");
        // cooldownを超えて次のcloud要求を進めます。
        services->Update(0.2f);
        // 次のcloud要求とrefresh要求を待ちます。
        pumpUntilRequestCount(6u);
        // refresh後も認証済み状態になるまで待ちます。
        UpdateUntil(
            *services,
            [&]
            {
                // 認証済み状態への到達を判定します。
                return services->State()
                    == LamaPon::OnlineAccountState::SignedIn;
            },
            "Cooled-down cloud refresh did not complete.");
        // cloud同期応答が届くまで更新します。
        pumpUntilRequestCount(7u);
        // 正常なwire応答でIdleへ戻るまで待ちます。
        pumpUntilCloudState(
            LamaPon::Detail::CloudSaveSynchronizerState::Idle);
        // Idle状態をserviceへ反映します。
        services->Update(0.0f);

        // 次のcloud同期要求を発生させます。
        preferences.SetInteger("after-healthy-wire", 1);
        // PlayerPrefsの変更を保存します。
        preferences.Save();
        // cloud 401要求が届くまで更新します。
        pumpUntilRequestCount(8u);
        // cloud synchronizerがUnauthorizedへ遷移するまで待ちます。
        pumpUntilCloudState(
            LamaPon::Detail::CloudSaveSynchronizerState::Unauthorized);
        // Unauthorized状態をserviceへ反映します。
        services->Update(0.0f);
        // cooldown後のrefresh要求が届くまで更新します。
        pumpUntilRequestCount(9u);
        // requests: login・cloud・refresh要求の記録。
        const auto requests = backend->Requests();
        // refreshToken(index): 指定refresh要求から保存tokenを読みます。
        const auto refreshToken = [&requests](const std::size_t index)
        {
            // 指定要求から送信したrefresh tokenを返します。
            return nlohmann::json::parse(std::string{
                requests[index].body.begin(),
                requests[index].body.end()
            }).value("refreshToken", std::string{});
        };
        // cloud 401とrefreshがtoken世代ごとに対応することを検証します。
        Require(
            requests.size() == 9u
                && HasBearer(requests[2], L"cloud-access-1")
                && HasBearer(requests[4], L"cloud-access-2")
                && HasBearer(requests[6], L"cloud-access-3")
                && HasBearer(requests[7], L"cloud-access-3")
                && requests[3].url.ends_with(
                    L"/v1/auth/session/refresh")
                && requests[5].url.ends_with(
                    L"/v1/auth/session/refresh")
                && requests[8].url.ends_with(
                    L"/v1/auth/session/refresh")
                && refreshToken(3u) == "cloud-refresh-1"
                && refreshToken(5u) == "cloud-refresh-2"
                && refreshToken(8u) == "cloud-refresh-3",
            "Cloud 401 refresh fencing/cooldown did not follow token generations.");
    }
}

// main(): 全永続化テストを実行し成否をprocessへ返します。
int main()
{
    // 初期化またはcase実行の失敗を捕捉します。
    try
    {
        // 前回実行のテスト保存先を初期化します。
        ResetTestRoot();
        // run(name, test): case失敗へ名前を付けて再送出します。
        const auto run = [](const char* name, const auto test)
        {
            // 個別caseの例外へcontextを付けます。
            try
            {
                // 登録されたcaseを実行します。
                test();
            }
            // exception: 個別caseで発生した失敗。
            catch (const std::exception& exception)
            {
                // 元の説明へcase名を付けて呼び出し元へ返します。
                throw std::runtime_error(
                    std::string(name) + ": " + exception.what());
            }
        };
        // 全永続化ケースを名前付きで順番に実行します。
        run("editor draft", TestEditorDraftsAreInvalidatedAcrossProfileBindings);
        run("snapshot race", TestValidatedPlayerPrefsSnapshotClosesReopenRace);
        run("transactional switch", TestTransactionalProfileSwitchAndStableObjects);
        run("binding lease", TestActiveAccountOwnsBothPersistenceBindings);
        run("profile session lease", TestAccountProfileSessionLeaseRejectsSecondActivation);
        run("prepared binding", TestPreparedCommitRejectsChangedGuestBinding);
        run("stale transaction", TestStalePreparedTransactionCannotCommit);
        run("corrupt documents", TestCorruptAccountDocumentsFailClosed);
        run("save quarantine", TestFailedAccountSaveIsQuarantinedAndRetried);
        run("detach delete checkpoint", TestImmediateDetachCheckpointsBaselineLessDelete);
        run("checkpoint quarantine", TestFailedDetachCheckpointKeepsAccountLeaseUntilDiscard);
        run("checkpoint scan failure", TestUndeterminedDetachCheckpointRemainsFailClosed);
        run("clean load failure", TestCleanLoadFailureDoesNotPermanentlyBlockProfiles);
        run("dirty recovery", TestDirtyLoadFailureUsesRecoverableStrictSidecar);
        run("recovery discard", TestExplicitRecoveryDiscardPreservesOriginal);
        run("memory recovery discard", TestMemoryRecoveryDiscardPromotesExistingSidecar);
        run("corrupt recovery availability", TestEmptyAndOversizedRecoverySidecarsCanBeDiscardedSafely);
        run("recovery sidecar lease", TestRecoverySidecarResolutionReacquiresProfileLease);
        run("public recovery API", TestPublicRecoveryRevisionAndOperationGuards);
        run("public cloud conflict API", TestPublicCloudConflictRegistryAndResolution);
        run("public cloud request state", TestRequestCloudSyncReportsBusyAndTerminalStop);
        run("credential rollback", TestCredentialSaveFailureRollsBackAndRevokesSession);
        run("failed credential pending", TestFailedCredentialSaveKeepsPriorCommitPendingUntilDeleted);
        run("immediate signout", TestSignOutImmediatelyRestoresGuestAndReloginRestoresAccount);
        run("stored restore", TestStoredRestoreActivatesAccountBeforePublishingSession);
        run("corrupt restore", TestCorruptAccountRejectsStoredRestoreAndDeletesCredential);
        run("refresh save rollback", TestRefreshCredentialSaveFailureRevokesRotatedSession);
        run("refresh signout", TestRefreshSignOutRaceKeepsImmediateGuestBinding);
        run("terminal refresh", TestTerminalRefreshFailuresDetachAccountPersistence);
        run("refresh expiry", TestRefreshExpiryImmediatelyDetachesAndRevokesLateSession);
        run("completed refresh expiry", TestCompletedRefreshExpiresBeforeReap);
        run("completed initial expiry", TestCompletedInitialSessionsExpireBeforeReap);
        run("inflight destruction", TestDestructionDuringRefreshImmediatelyRestoresGuest);
        run("unreaped destruction", TestCompletedUnreapedRefreshIsRevokedAfterDestruction);
        run("restore destruction", TestDestroyedRestoreDeletesCredentialAndRevokesLateSession);
        run("poll destruction", TestDestroyedPollRevokesLateAuthorizedSession);
        run("signed-in destruction", TestSignedInDestructionPreservesStoredRefreshToken);
        run("destructor delete retry", TestDestroyedSignOutRetriesFailedCredentialDeletion);
        run("completed delete retry", TestCompletedSignOutKeepsFailedCredentialDeletePending);
        run("configure guarantee", TestConfigureFailurePreservesClientNamespaceAndBinding);
        run("refresh identity", TestRefreshIdentityChangeFailsClosed);
        run("cloud 401 cooldown", TestCloudUnauthorizedRefreshUsesCooldownUntilWireSuccess);
        // 成功メッセージを標準出力へ出します。
        std::cout << "Online persistence coordinator tests passed.\n";
        // 全case成功をprocess exit codeで通知します。
        return 0;
    }
    // exception: 初期化またはcase実行中に発生した失敗。
    catch (const std::exception& exception)
    {
        // 失敗の説明を標準エラーへ出します。
        std::cerr << "Online persistence coordinator tests failed: "
                  << exception.what() << '\n';
        // テスト失敗をprocess exit codeで通知します。
        return 1;
    }
}
