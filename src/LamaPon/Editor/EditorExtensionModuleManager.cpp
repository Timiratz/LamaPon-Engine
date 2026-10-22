#include "LamaPon/Editor/EditorExtensionModuleManager.h"

#include "LamaPon/Core/Log.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/Version.h"
#include "LamaPon/Core/VersionCompare.h"
#include "LamaPon/Editor/EditorExtensionApi.h"
#include "LamaPon/Editor/EditorExtensionRegistry.h"

#include <Windows.h>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace
{
    // 1拡張から登録できるパネルの上限
    constexpr std::uint32_t MaximumEditorExtensionPanels = 64;

    // UTF-8文字列をEditorのログへ記録します(userData: 未使用, level: 0=情報・1=警告・2=エラー, message: 診断本文)。
    void LAMAPON_EDITOR_EXTENSION_CALL LogExtensionMessage(
        void* const userData,
        const std::uint32_t level,
        const char* const message)
    {
        static_cast<void>(userData);
        // 診断本文の所有コピー
        const std::string text = message == nullptr ? "" : message;
        // 有効なログ重要度
        const auto logLevel = level <= 2
            ? static_cast<LamaPon::LogLevel>(level)
            : LamaPon::LogLevel::Info;
        LamaPon::Logger::Instance().Write(
            logLevel,
            "[Editor Extension] " + text);
    }

    // UIテキストを表示します(userData: 未使用, text: UTF-8表示文字列)。
    void LAMAPON_EDITOR_EXTENSION_CALL DrawExtensionText(
        void* const userData,
        const char* const text)
    {
        static_cast<void>(userData);
        ImGui::TextUnformatted(text == nullptr ? "" : text);
    }

    // 押下されたボタンを真で返します(userData: 未使用, label: ボタン名)。
    std::int32_t LAMAPON_EDITOR_EXTENSION_CALL DrawExtensionButton(
        void* const userData,
        const char* const label)
    {
        static_cast<void>(userData);
        return label != nullptr && ImGui::Button(label) ? 1 : 0;
    }

    // チェック値を編集して変更有無を返します(userData: 未使用, label: 項目名, value: 0または1の入出力先)。
    std::int32_t LAMAPON_EDITOR_EXTENSION_CALL DrawExtensionCheckbox(
        void* const userData,
        const char* const label,
        std::int32_t* const value)
    {
        static_cast<void>(userData);
        if (label == nullptr || value == nullptr)
        {
            return 0;
        }
        // ImGui用の真偽値
        bool checked = *value != 0;
        // チェック状態が変わったか
        const bool changed = ImGui::Checkbox(label, &checked);
        *value = checked ? 1 : 0;
        return changed ? 1 : 0;
    }

    // 浮動小数の範囲スライダーを表示します(userData: 未使用, label: 項目名, value: 編集値, minimum: 下限, maximum: 上限)。
    std::int32_t LAMAPON_EDITOR_EXTENSION_CALL DrawExtensionSliderFloat(
        void* const userData,
        const char* const label,
        float* const value,
        const float minimum,
        const float maximum)
    {
        static_cast<void>(userData);
        if (label == nullptr || value == nullptr || minimum > maximum)
        {
            return 0;
        }
        return ImGui::SliderFloat(label, value, minimum, maximum)
            ? 1
            : 0;
    }

    // 浮動小数の入力欄を表示します(userData: 未使用, label: 項目名, value: 編集値)。
    std::int32_t LAMAPON_EDITOR_EXTENSION_CALL DrawExtensionInputFloat(
        void* const userData,
        const char* const label,
        float* const value)
    {
        static_cast<void>(userData);
        if (label == nullptr || value == nullptr)
        {
            return 0;
        }
        return ImGui::InputFloat(label, value) ? 1 : 0;
    }

    // UI内へ区切り線を表示します(userData: 未使用)。
    void LAMAPON_EDITOR_EXTENSION_CALL DrawExtensionSeparator(
        void* const userData)
    {
        static_cast<void>(userData);
        ImGui::Separator();
    }

    // 拡張パネルから使うUI関数表
    const LamaPonEditorUiApiV1 g_editorUiApi{
        static_cast<std::uint32_t>(sizeof(LamaPonEditorUiApiV1)),
        nullptr,
        DrawExtensionText,
        DrawExtensionButton,
        DrawExtensionCheckbox,
        DrawExtensionSliderFloat,
        DrawExtensionInputFloat,
        DrawExtensionSeparator
    };

    // 拡張の初期化処理へ渡すホスト関数表
    const LamaPonEditorExtensionHostApiV1 g_extensionHostApi{
        static_cast<std::uint32_t>(
            sizeof(LamaPonEditorExtensionHostApiV1)),
        LAMAPON_EDITOR_EXTENSION_ABI_VERSION,
        nullptr,
        LogExtensionMessage,
        &g_editorUiApi
    };

    // DLL内へ抜ける相対パスだけを許可します(path: package.jsonの相対パス)。
    bool IsSafeRelativePath(const std::filesystem::path& path)
    {
        if (path.empty() || path.is_absolute()
            || path.has_root_name() || path.has_root_directory())
        {
            return false;
        }
        // 正規化した相対パスの各要素
        for (const auto& component : path.lexically_normal())
        {
            if (component == L"..")
            {
                return false;
            }
        }
        return true;
    }

    // DLLの正規化パスがパッケージ内か確認します(directory: package.jsonの親, candidate: 読み込むDLL)。
    bool IsWithinDirectory(
        const std::filesystem::path& directory,
        const std::filesystem::path& candidate)
    {
        // ファイルシステムの正規化エラー
        std::error_code error;
        // パッケージルートの正規化表記
        const auto root = std::filesystem::weakly_canonical(
            directory,
            error).native();
        if (error)
        {
            return false;
        }
        // DLLの正規化表記
        const auto file = std::filesystem::weakly_canonical(
            candidate,
            error).native();
        if (error || file.size() <= root.size()
            || _wcsnicmp(file.c_str(), root.c_str(), root.size()) != 0)
        {
            return false;
        }
        return root.ends_with(L'\\') || root.ends_with(L'/')
            || file[root.size()] == L'\\'
            || file[root.size()] == L'/';
    }

    // NULLでないUTF-8値を所有文字列へ移します(value: 借用文字列)。
    std::string CopyText(const char* const value)
    {
        return value == nullptr ? std::string{} : std::string{ value };
    }

    struct AdvancePackageIterator final
    {
        // 進めるパッケージ列挙子
        std::filesystem::directory_iterator& iterator;
        // 列挙子を進めたときのエラー出力先
        std::error_code& error;

        // 現在のパッケージ処理後に列挙子を進めます。
        ~AdvancePackageIterator()
        {
            iterator.increment(error);
        }
    };

}

namespace LamaPon
{
    struct EditorExtensionModuleManager::ModuleState final
    {
        // ロード済みDLL
        HMODULE module{};
        // DLLが所有する登録情報
        const LamaPonEditorExtensionV1* descriptor{};
        // 拡張初期化で受け取る不透明状態
        void* userData{};
        // 登録した拡張ID
        std::string id;
        // 初期化が成功した状態
        bool initialized{};
        // callback例外を通知済みか
        bool callbackFailed{};

        // 登録済み拡張の終了処理とDLL解放を行います(forceShutdown: 初期化失敗時にも終了callbackを呼ぶか)。
        void Release(const bool forceShutdown = false) noexcept
        {
            if (module != nullptr && descriptor != nullptr
                && descriptor->shutdown != nullptr
                && (initialized || forceShutdown))
            {
                try
                {
                    descriptor->shutdown(userData);
                }
                catch (...)
                {
                    try
                    {
                        Logger::Instance().Warning(
                            "外部Editor拡張の終了callbackが例外を送出しました: "
                            + id);
                    }
                    catch (...)
                    {
                    }
                }
            }
            initialized = false;
            userData = nullptr;
            descriptor = nullptr;
            if (module != nullptr)
            {
                FreeLibrary(module);
                module = nullptr;
            }
        }

        // 残ったDLLを解放します。
        ~ModuleState()
        {
            Release();
        }

        // callbackの失敗を拡張ごとに一度だけ記録します(callbackName: 失敗した処理名)。
        void ReportCallbackFailure(const std::string_view callbackName)
            noexcept
        {
            if (callbackFailed)
            {
                return;
            }
            callbackFailed = true;
            try
            {
                Logger::Instance().Warning(
                    "外部Editor拡張の"
                    + std::string{ callbackName }
                    + " callbackが例外を送出しました: " + id);
            }
            catch (...)
            {
            }
        }
    };

    EditorExtensionModuleManager::~EditorExtensionModuleManager()
    {
        Shutdown();
    }

    std::vector<std::string> EditorExtensionModuleManager::LoadInstalled(
        const std::filesystem::path& assetRoot,
        EditorExtensionRegistry& registry)
    {
        if (!m_modules.empty())
        {
            Shutdown();
        }
        m_registry = &registry;
        // インストール済みパッケージの場所
        const auto packagesDirectory = assetRoot / L"packages";
        // 読み込み時の診断
        std::vector<std::string> diagnostics;
        // フォルダー走査のエラー
        std::error_code iterationError;
        if (!std::filesystem::is_directory(
                packagesDirectory,
                iterationError))
        {
            return diagnostics;
        }

        // インストール済みパッケージ候補
        std::filesystem::directory_iterator packageIterator{
            packagesDirectory,
            iterationError
        };
        // パッケージ走査の終端
        const std::filesystem::directory_iterator packageEnd;
        while (!iterationError && packageIterator != packageEnd)
        {
            // 現在検査するパッケージフォルダー
            const auto packageDirectory = packageIterator->path();
            // 継続や例外終了でも現在のフォルダーを処理してから次へ進みます。
            const AdvancePackageIterator advancePackage{
                packageIterator,
                iterationError
            };
            // パッケージエントリーの検査エラー
            std::error_code packageError;
            if (!std::filesystem::is_directory(
                    packageDirectory,
                    packageError)
                || packageError)
            {
                continue;
            }

            // パッケージ設定ファイル
            const auto manifestPath = packageDirectory / L"package.json";
            // package.jsonの検査エラー
            std::error_code manifestError;
            if (!std::filesystem::is_regular_file(
                    manifestPath,
                    manifestError)
                || manifestError)
            {
                continue;
            }
            try
            {
                // パッケージ設定入力
                std::ifstream input(manifestPath, std::ios::binary);
                // パッケージ設定JSON
                nlohmann::json manifest;
                input >> manifest;
                if (!manifest.is_object()
                    || !manifest.contains("editorExtension"))
                {
                    continue;
                }

                // 外部Editor拡張設定
                const auto& extension = manifest.at("editorExtension");
                if (!extension.is_object())
                {
                    diagnostics.push_back(
                        "Editor拡張設定が不正です: "
                        + PathToUtf8(packageDirectory));
                    continue;
                }
                if (manifest.value("target", std::string{ "Project" })
                    != "Engine")
                {
                    diagnostics.push_back(
                        "Editor拡張DLLはtarget=Engineのパッケージにだけ登録できます: "
                        + PathToUtf8(packageDirectory));
                    continue;
                }
                // DLL有効化に必要な再起動方式
                const auto activation = manifest.value(
                    "activation",
                    std::string{});
                if (activation != "Restart"
                    && activation != "RestartAndRebuild")
                {
                    diagnostics.push_back(
                        "Editor拡張パッケージにはactivation=Restartが必要です: "
                        + PathToUtf8(packageDirectory));
                    continue;
                }
                // パッケージが要求する最低エンジン版
                const auto minimumEngineVersion = manifest.value(
                    "minimumEngineVersion",
                    std::string{});
                if (!minimumEngineVersion.empty()
                    && (ParseVersionNumbers(minimumEngineVersion).empty()
                        || IsNewerVersion(
                            VersionString,
                            minimumEngineVersion)))
                {
                    diagnostics.push_back(
                        "Editor拡張パッケージとエンジンのバージョンが一致しません: "
                        + PathToUtf8(packageDirectory));
                    continue;
                }
                // 拡張マニフェストのABI版
                const auto abiVersion = extension.value(
                    "abiVersion",
                    std::uint32_t{});
                // 拡張DLLのpackage.json相対パス
                const auto runtimeRelative = PathFromUtf8(
                    extension.value("runtimeLibrary", std::string{}));
                if (abiVersion != LAMAPON_EDITOR_EXTENSION_ABI_VERSION
                    || !IsSafeRelativePath(runtimeRelative)
                    || runtimeRelative.extension() != L".dll")
                {
                    diagnostics.push_back(
                        "Editor拡張のABIまたはruntimeLibraryが不正です: "
                        + PathToUtf8(packageDirectory));
                    continue;
                }
                // パッケージ内の正規化したDLLパス
                const auto runtimeLibrary =
                    (packageDirectory / runtimeRelative).lexically_normal();
                if (!std::filesystem::is_regular_file(runtimeLibrary)
                    || !IsWithinDirectory(
                        packageDirectory,
                        runtimeLibrary))
                {
                    diagnostics.push_back(
                        "Editor拡張DLLが見つからないか、パッケージ外を指しています: "
                        + PathToUtf8(runtimeLibrary));
                    continue;
                }
                // 読み込む拡張DLL
                const HMODULE module = LoadLibraryExW(
                    runtimeLibrary.c_str(),
                    nullptr,
                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
                        | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                if (module == nullptr)
                {
                    diagnostics.push_back(
                        "Editor拡張DLLを読み込めませんでした: "
                        + PathToUtf8(runtimeLibrary));
                    continue;
                }
                // DLLのABI記述を取得する関数
                const auto getExtension = reinterpret_cast<
                    LamaPonGetEditorExtensionV1Function>(GetProcAddress(
                        module,
                        "LamaPonGetEditorExtensionV1"));
                if (getExtension == nullptr)
                {
                    FreeLibrary(module);
                    diagnostics.push_back(
                        "Editor拡張DLLにLamaPonGetEditorExtensionV1がありません: "
                        + PathToUtf8(runtimeLibrary));
                    continue;
                }

                // DLLのcallbackを保持する状態
                auto state = std::make_shared<ModuleState>();
                state->module = module;
                state->descriptor = getExtension();
                if (state->descriptor == nullptr
                    || state->descriptor->structSize
                        < sizeof(LamaPonEditorExtensionV1)
                    || state->descriptor->abiVersion
                        != LAMAPON_EDITOR_EXTENSION_ABI_VERSION
                    || state->descriptor->id == nullptr
                    || state->descriptor->displayName == nullptr
                    || state->descriptor->initialize == nullptr
                    || state->descriptor->panelCount
                        > MaximumEditorExtensionPanels
                    || (state->descriptor->panelCount > 0
                        && state->descriptor->panels == nullptr))
                {
                    diagnostics.push_back(
                        "Editor拡張DLLの登録情報またはABI版が不正です: "
                        + PathToUtf8(runtimeLibrary));
                    continue;
                }
                // エンジンへ登録する拡張情報
                EditorExtensionDefinition definition;
                definition.id = CopyText(state->descriptor->id);
                definition.displayName = CopyText(
                    state->descriptor->displayName);
                state->id = definition.id;
                if (definition.id.empty() || definition.displayName.empty())
                {
                    diagnostics.push_back(
                        "Editor拡張DLLのIDまたは表示名が空です: "
                        + PathToUtf8(runtimeLibrary));
                    continue;
                }

                // ABI記述内の各パネル
                for (std::uint32_t panelIndex = 0;
                    panelIndex < state->descriptor->panelCount;
                    ++panelIndex)
                {
                    // 現在検査するパネル記述
                    const auto& sourcePanel =
                        state->descriptor->panels[panelIndex];
                    if (sourcePanel.structSize < sizeof(LamaPonEditorPanelV1)
                        || sourcePanel.id == nullptr
                        || sourcePanel.displayName == nullptr
                        || sourcePanel.draw == nullptr)
                    {
                        definition.panels.clear();
                        break;
                    }
                    // エンジンへ登録するパネル情報
                    EditorPanelDefinition panel;
                    panel.id = CopyText(sourcePanel.id);
                    panel.displayName = CopyText(sourcePanel.displayName);
                    panel.windowMenuGroup = CopyText(
                        sourcePanel.windowMenuGroup);
                    panel.defaultOpen = (sourcePanel.flags
                        & LAMAPON_EDITOR_PANEL_DEFAULT_OPEN) != 0;
                    panel.showInWindowMenu = (sourcePanel.flags
                        & LAMAPON_EDITOR_PANEL_SHOW_IN_WINDOW_MENU) != 0;
                    // DLL callbackとパネルの利用者状態
                    const auto drawCallback = sourcePanel.draw;
                    void* const panelUserData = sourcePanel.userData;
                    // ImGuiパネルを開いてDLLへ内容を描画させます(open: 現在の表示状態)。
                    panel.draw = [state,
                                  drawCallback,
                                  panelUserData,
                                  title = panel.displayName](bool& open)
                    {
                        // ImGuiが更新する表示状態
                        bool visible = open;
                        if (ImGui::Begin(title.c_str(), &visible))
                        {
                            // C ABI callbackへ渡す表示状態
                            std::int32_t extensionOpen = visible ? 1 : 0;
                            if (!state->callbackFailed
                                && state->initialized)
                            {
                                try
                                {
                                    drawCallback(
                                        state->userData,
                                        panelUserData,
                                        &g_editorUiApi,
                                        &extensionOpen);
                                }
                                catch (...)
                                {
                                    state->ReportCallbackFailure("描画");
                                }
                            }
                            visible = extensionOpen != 0;
                        }
                        ImGui::End();
                        open = visible;
                    };
                    definition.panels.push_back(std::move(panel));
                }
                if (definition.panels.size()
                    != state->descriptor->panelCount)
                {
                    diagnostics.push_back(
                        "Editor拡張DLLのパネル記述が不正です: "
                        + PathToUtf8(runtimeLibrary));
                    continue;
                }

                // 初期化callbackを登録時に1回呼びます。
                definition.onAttach = [state]
                {
                    if (state->descriptor->initialize(
                            &g_extensionHostApi,
                            &state->userData) == 0)
                    {
                        state->Release(true);
                        throw std::runtime_error(
                            "Editor拡張DLLの初期化に失敗しました。");
                    }
                    state->initialized = true;
                };
                // Editorの各フレームで外部拡張を更新します。
                definition.onUpdate = [state]
                {
                    if (!state->initialized || state->callbackFailed
                        || state->descriptor->update == nullptr)
                    {
                        return;
                    }
                    try
                    {
                        state->descriptor->update(state->userData);
                    }
                    catch (...)
                    {
                        state->ReportCallbackFailure("更新");
                    }
                };
                // 拡張解除時にDLLの終了callbackを呼びます。
                definition.onShutdown = [state]
                {
                    state->Release();
                };

                // C++ ABIをまたがずEditor拡張を登録します。
                std::string registrationError;
                m_modules.push_back(state);
                if (!registry.Register(
                        std::move(definition),
                        &registrationError))
                {
                    state->Release();
                    m_modules.pop_back();
                    diagnostics.push_back(
                        "Editor拡張を登録できませんでした: "
                        + registrationError);
                    continue;
                }
            }
            catch (const std::exception& exception)
            {
                diagnostics.push_back(
                    "Editor拡張パッケージを読み込めませんでした: "
                    + PathToUtf8(packageDirectory)
                    + " (" + exception.what() + ")");
            }
            catch (...)
            {
                diagnostics.push_back(
                    "Editor拡張パッケージを読み込めませんでした: "
                    + PathToUtf8(packageDirectory));
            }
        }
        if (iterationError)
        {
            diagnostics.push_back(
                "Editor拡張パッケージのフォルダーを走査できませんでした: "
                + PathToUtf8(packagesDirectory));
        }
        return diagnostics;
    }

    void EditorExtensionModuleManager::Shutdown() noexcept
    {
        // 終了時に逆順で解除するモジュール番号
        for (std::size_t index = m_modules.size(); index > 0; --index)
        {
            // 登録した拡張の所有状態
            const auto& module = m_modules[index - 1];
            if (m_registry != nullptr)
            {
                static_cast<void>(m_registry->Unregister(module->id));
            }
            module->Release();
        }
        m_modules.clear();
        m_registry = nullptr;
    }
}
