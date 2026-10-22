#include "LamaPon/Editor/EditorExtensionApi.h"

#include <cstdint>

namespace
{
    // チェックボックスのサンプル状態
    std::int32_t g_enabled = 1;
    // スライダーのサンプル状態
    float g_strength = 0.5f;

    // UI APIを使ってサンプルパネルを描きます(extensionUserData: 拡張状態, panelUserData: パネル状態, ui: ホストUI, open: 表示状態)。
    void LAMAPON_EDITOR_EXTENSION_CALL DrawPanel(
        void* const extensionUserData,
        void* const panelUserData,
        const LamaPonEditorUiApiV1* const ui,
        std::int32_t* const open)
    {
        static_cast<void>(extensionUserData);
        static_cast<void>(panelUserData);
        static_cast<void>(open);
        if (ui == nullptr || ui->text == nullptr
            || ui->checkbox == nullptr || ui->sliderFloat == nullptr)
        {
            return;
        }
        ui->text(ui->userData, "LamaPon外部Editor拡張のサンプルです。");
        ui->checkbox(ui->userData, "有効", &g_enabled);
        ui->sliderFloat(
            ui->userData,
            "強度",
            &g_strength,
            0.0f,
            1.0f);
    }

    // Editor拡張が使うホストAPIを検証します(host: LamaPonホスト関数, extensionUserData: 拡張状態出力)。
    std::int32_t LAMAPON_EDITOR_EXTENSION_CALL Initialize(
        const LamaPonEditorExtensionHostApiV1* const host,
        void** const extensionUserData)
    {
        if (extensionUserData == nullptr)
        {
            return 0;
        }
        *extensionUserData = nullptr;
        if (host == nullptr || host->abiVersion
                != LAMAPON_EDITOR_EXTENSION_ABI_VERSION
            || host->ui == nullptr)
        {
            return 0;
        }
        if (host->log != nullptr)
        {
            host->log(host->userData, 0, "Sample Editor Extension loaded.");
        }
        return 1;
    }

    // 登録するサンプルパネル
    const LamaPonEditorPanelV1 g_panels[]{
        {
            sizeof(LamaPonEditorPanelV1),
            "sample.tools.panel",
            "外部拡張サンプル",
            "サンプル",
            LAMAPON_EDITOR_PANEL_DEFAULT_OPEN
                | LAMAPON_EDITOR_PANEL_SHOW_IN_WINDOW_MENU,
            nullptr,
            DrawPanel
        }
    };

    // LamaPonへ返す拡張記述
    const LamaPonEditorExtensionV1 g_extension{
        sizeof(LamaPonEditorExtensionV1),
        LAMAPON_EDITOR_EXTENSION_ABI_VERSION,
        "sample.editor-tools",
        "Editor Tools Sample",
        g_panels,
        1,
        Initialize,
        nullptr,
        nullptr
    };
}

extern "C" LAMAPON_EDITOR_EXTENSION_EXPORT
const LamaPonEditorExtensionV1* LAMAPON_EDITOR_EXTENSION_CALL
LamaPonGetEditorExtensionV1()
{
    return &g_extension;
}
