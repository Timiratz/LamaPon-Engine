#include "LamaPon/Editor/EditorExtensionRegistry.h"
#include "LamaPon/Editor/PersistencePanelState.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
    // 条件不成立ならテストを失敗させます。
    // Require(condition: 成立条件, message: 失敗理由)
    void Require(const bool condition, const char* const message)
    {
        // 失敗理由を例外で通知
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    // クラウド確認操作の対象競合とrevisionを検証します。
    void TestPersistenceConfirmationFences()
    {
        using LamaPon::Detail::PersistenceConfirmationKind;
        // オンライン確認状態を検査するパネル状態
        LamaPon::Detail::PersistencePanelState state(
            L"C:/test/guest/PlayerPrefs.json",
            L"C:/test/guest/Saves");

        state.BeginConflictConfirmation(
            PersistenceConfirmationKind::RetryLocal,
            "test-conflict");
        Require(
            !state.SynchronizeOnlineConfirmation(true, true, 0),
            "A current cloud conflict confirmation was invalidated.");
        Require(
            state.SynchronizeOnlineConfirmation(true, false, 0)
                && state.OnlineConfirmationKind()
                    == PersistenceConfirmationKind::None
                && state.CloseOnlineConfirmationPopupRequested(),
            "A removed cloud conflict kept its confirmation active.");
        state.AcknowledgeCloseOnlineConfirmationPopup();

        state.BeginConflictConfirmation(
            PersistenceConfirmationKind::UseRemote,
            "test-conflict");
        Require(
            state.SynchronizeOnlineConfirmation(false, true, 0),
            "Signing out kept an account conflict confirmation active.");
        state.AcknowledgeCloseOnlineConfirmationPopup();

        state.BeginRecoveryConfirmation(
            PersistenceConfirmationKind::Restore,
            41);
        Require(
            !state.SynchronizeOnlineConfirmation(false, false, 41),
            "A current recovery confirmation was invalidated.");
        Require(
            state.SynchronizeOnlineConfirmation(false, false, 42)
                && state.CloseOnlineConfirmationPopupRequested(),
            "A stale recovery revision kept its confirmation active.");
        state.AcknowledgeCloseOnlineConfirmationPopup();

        state.BeginRecoveryConfirmation(
            PersistenceConfirmationKind::Discard,
            42);
        Require(
            state.SynchronizeBinding(
                L"C:/test/account/PlayerPrefs.json",
                L"C:/test/account/Saves")
                && state.OnlineConfirmationKind()
                    == PersistenceConfirmationKind::None
                && state.CloseOnlineConfirmationPopupRequested(),
            "A profile binding change kept its confirmation active.");
    }
}

// Editor拡張の登録・表示状態・解放契約を検証します。
int main()
{
    // テスト例外を失敗終了コードへ変換
    int result{};
    // レジストリの拡張ライフサイクルを検査
    try
    {
        TestPersistenceConfirmationFences();

        // テスト対象の拡張レジストリ
        LamaPon::EditorExtensionRegistry registry;
        // 拡張attach callback呼び出し数
        int attachCount{};
        // 拡張update callback呼び出し数
        int updateCount{};
        // Panel draw callback呼び出し数
        int drawCount{};
        // 拡張shutdown callback呼び出し数
        int shutdownCount{};

        // 後から登録したpanelにも保存済み表示状態を適用
        registry.RestorePanelVisibility(
            "example.inspector",
            true);

        // 登録・描画・解放を確認する拡張定義
        LamaPon::EditorExtensionDefinition extension;
        extension.id = "example.tools";
        extension.displayName = "Example Tools";
        extension.panels.push_back({
            "example.inspector",
            "Example Inspector",
            false,
            true,
            // open: 登録時に復元されたパネル表示状態
            [&](bool& open)
            {
                Require(open, "Closed panels must not be drawn.");
                ++drawCount;
            }
        });
        extension.onAttach = [&] { ++attachCount; };
        extension.onUpdate = [&] { ++updateCount; };
        extension.onShutdown = [&] { ++shutdownCount; };

        // 拡張登録失敗時の説明
        std::string error;
        Require(
            registry.Register(std::move(extension), &error),
            "A valid extension must register.");
        Require(error.empty() && attachCount == 1,
            "Registration must attach the extension once.");
        Require(registry.Extensions().size() == 1
                && registry.Panels().size() == 1,
            "Extension and panel records must be discoverable.");
        Require(registry.IsPanelOpen("example.inspector"),
            "Late registrations must consume saved visibility by id.");

        registry.DrawPanels();
        Require(drawCount == 1,
            "A restored open panel must be drawn.");
        Require(registry.SetPanelOpen("example.inspector", false),
            "Registered panel visibility must be writable by id.");
        registry.DrawPanels();
        Require(drawCount == 1,
            "Closed panel callbacks must be skipped.");
        Require(registry.SetPanelOpen("example.inspector", true),
            "Registered panel visibility must be writable by id.");
        registry.DrawPanels();
        registry.Update();
        Require(drawCount == 2 && updateCount == 1,
            "Open panels and extension updates must be dispatched.");

        registry.ResetPanelVisibility();
        Require(!registry.IsPanelOpen("example.inspector"),
            "Layout reset must restore each registered default.");

        // 重複panel idを含む拒否対象の拡張
        LamaPon::EditorExtensionDefinition duplicatePanel;
        duplicatePanel.id = "example.duplicate";
        duplicatePanel.displayName = "Duplicate";
        duplicatePanel.panels.push_back({
            "example.inspector",
            "Conflicting Inspector",
            true,
            true,
            [](bool&) {}
        });
        Require(!registry.Register(std::move(duplicatePanel), &error)
                && !error.empty(),
            "Duplicate panel ids must be rejected atomically.");
        Require(registry.Extensions().size() == 1
                && registry.Panels().size() == 1,
            "A rejected extension must not leave partial records.");

        // window menu groupを持つ拡張と持たないpanelを比較
        LamaPon::EditorExtensionDefinition analysis;
        analysis.id = "example.analysis";
        analysis.displayName = "Example Analysis";
        analysis.panels.push_back({
            "example.profiler",
            "Example Profiler",
            false,
            true,
            [](bool&) {},
            "解析"
        });
        Require(registry.Register(std::move(analysis), &error),
            "A grouped panel must register.");
        // menu groupを持つ登録panel
        const auto* grouped = registry.FindPanel("example.profiler");
        // group指定なしの登録panel
        const auto* ungrouped = registry.FindPanel("example.inspector");
        Require(grouped != nullptr
                && grouped->windowMenuGroup == "解析"
                && ungrouped != nullptr
                && ungrouped->windowMenuGroup.empty(),
            "Window menu groups must be preserved per panel.");
        Require(registry.Unregister("example.analysis"),
            "The grouped extension must be removable by id.");

        Require(registry.Unregister("example.tools"),
            "Registered extensions must be removable by id.");
        Require(shutdownCount == 1
                && registry.Extensions().empty()
                && registry.Panels().empty(),
            "Unregister must shut down and remove owned panels.");
        Require(!registry.Unregister("example.tools"),
            "Removing an unknown extension must report failure.");

        std::cout << "Editor extension registry tests passed.\n";
    }
    // テスト例外を標準エラーと失敗終了コードへ変換
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        result = 1;
    }
    return result;
}
