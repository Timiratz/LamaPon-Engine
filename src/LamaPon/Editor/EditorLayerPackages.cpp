#include "LamaPon/Editor/EditorLayer.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/HttpClient.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/Version.h"
#include "LamaPon/Core/VersionCompare.h"
#include "LamaPon/Editor/PackageManager.h"
#include "LamaPon/Editor/Live2DCubismSetup.h"
#include "LamaPon/Components/NativeScriptComponent.h"
#include "LamaPon/Graphics/GraphicsBackendPackage.h"
#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Scene/Scene.h"
#include "LamaPon/Scripting/GameModuleHost.h"

#include <imgui.h>

#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace LamaPon
{
    namespace
    {

        // 最初に指定フォルダーを選択します(dialog: Windowsフォルダーダイアログ, initialPath: 初期選択パス文字列)。
        int CALLBACK BrowseForFolderInitialCallback(
            const HWND dialog,
            const UINT message,
            const LPARAM,
            const LPARAM userData)
        {
            if (message == BFFM_INITIALIZED && userData != 0)
            {
                SendMessageW(
                    dialog,
                    BFFM_SETSELECTIONW,
                    TRUE,
                    userData);
            }
            return 0;
        }

        // SDKの展開先フォルダーを選び、キャンセル時は空を返します(owner: ダイアログの所有ウィンドウ)。
        std::optional<std::filesystem::path> BrowseForCubismSdkFolder(
            const HWND owner)
        {
            std::array<wchar_t, MAX_PATH> displayName{};
            BROWSEINFOW browse{};
            browse.hwndOwner = owner;
            browse.pszDisplayName = displayName.data();
            browse.lpszTitle =
                L"Cubism SDK for Nativeの展開先を選択";
            browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            browse.lpfn = BrowseForFolderInitialCallback;
            const PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
            if (item == nullptr)
            {
                return std::nullopt;
            }

            std::array<wchar_t, MAX_PATH> selectedPath{};
            const bool pathRead = SHGetPathFromIDListW(
                item,
                selectedPath.data()) != FALSE;
            CoTaskMemFree(item);
            if (!pathRead)
            {
                throw std::runtime_error(
                    "選択したSDKフォルダーのパスを読み取れませんでした。");
            }
            return std::filesystem::path(selectedPath.data());
        }

        // Cubismモデルのmodel3 JSONを選び、キャンセル時は空を返します(owner: ダイアログの所有ウィンドウ)。
        std::optional<std::filesystem::path> BrowseForModel3Json(
            const HWND owner)
        {
            std::array<wchar_t, 32768> selectedPath{};
            constexpr wchar_t filter[] =
                L"Live2Dモデル (*.model3.json)\0*.model3.json\0"
                L"すべてのファイル (*.*)\0*.*\0\0";
            OPENFILENAMEW dialog{};
            dialog.lStructSize = sizeof(dialog);
            dialog.hwndOwner = owner;
            dialog.lpstrFilter = filter;
            dialog.nFilterIndex = 1;
            dialog.lpstrFile = selectedPath.data();
            dialog.nMaxFile =
                static_cast<DWORD>(selectedPath.size());
            dialog.lpstrTitle = L"Live2Dモデルを選択";
            dialog.Flags = OFN_EXPLORER
                | OFN_FILEMUSTEXIST
                | OFN_PATHMUSTEXIST
                | OFN_NOCHANGEDIR;
            if (!GetOpenFileNameW(&dialog))
            {
                return std::nullopt;
            }
            return std::filesystem::path(selectedPath.data());
        }

        // パッケージ内に拡張子が小文字cppの通常ファイルがあるか調べます(packageDirectory: 調べるパッケージのパス)。
        bool PackageContainsScripts(
            const std::filesystem::path& packageDirectory)
        {
            // パッケージ操作の失敗理由
            std::error_code error;
            // パッケージ内の再帰走査位置
            for (auto iterator =
                    std::filesystem::
                        recursive_directory_iterator(
                            packageDirectory,
                            error);
                !error && iterator
                    != std::filesystem::
                        recursive_directory_iterator{};
                iterator.increment(error))
            {
                if (iterator->is_regular_file(error)
                    && iterator->path().extension()
                        == L".cpp")
                {
                    return true;
                }
            }
            return false;
        }

        // 1024基準のKBかMBへ切り上げ、ゼロなら未指定表示を返します(bytes: ZIPのサイズ・byte)。
        std::string FormatPackageSize(
            const std::uint64_t bytes)
        {
            if (bytes == 0)
            {
                return "-";
            }
            if (bytes < 1024ull * 1024ull)
            {
                return std::to_string(
                    (bytes + 1023) / 1024) + " KB";
            }
            return std::to_string(
                (bytes + 1024ull * 1024ull - 1)
                / (1024ull * 1024ull)) + " MB";
        }

        // 再起動が必要な反映時期の操作案内を返します(activation: パッケージの反映時期)。
        std::string RestartNotice(
            const PackageActivation activation)
        {
            if (!PackageRequiresRestart(activation))
            {
                return {};
            }
            if (activation == PackageActivation::RestartAndRebuild)
            {
                return "。反映するにはエディターを再起動し、"
                    "ゲームを再ビルドしてください";
            }
            return "。反映するにはエディターまたはゲームを"
                "再起動してください";
        }

        // 描画APIの表示名を返します(api: プロジェクトの描画API)。
        const char* RenderingApiDisplayName(
            const RenderingApi api) noexcept
        {
            switch (api)
            {
            case RenderingApi::Auto:
                return "Auto";
            case RenderingApi::DirectX12Experimental:
                return "DirectX 12 Experimental";
            case RenderingApi::DirectX11:
            default:
                return "DirectX 11";
            }
        }
    }

    // 動作中または未回収のパッケージworkerの完了を待ちます。
    void EditorLayer::JoinPackageWorker()
    {
        if (m_packageWorker.joinable())
        {
            m_packageWorker.join();
        }
    }

    // 配布一覧にあるパッケージの配置済み版数を更新します。
    void EditorLayer::RefreshInstalledPackageVersions()
    {
        m_installedPackageVersions.clear();
        // パッケージ保存先のassets
        const auto assetRoot =
            m_graphics.Assets().AssetRoot();
        // 操作または表示するパッケージ
        for (const auto& package : m_packages)
        {
            m_installedPackageVersions[package.name] =
                InstalledPackageVersion(
                    assetRoot,
                    package.name);
        }
    }

    // 前のworkerを待ち、配布一覧の取得をバックグラウンドで開始します。
    void EditorLayer::StartPackageIndexFetch()
    {
        JoinPackageWorker();
        m_packageBusy = true;
        m_packageListState = PackageListState::Loading;
        m_packagePanelError.clear();
        // バックグラウンドで処理し、完了結果をmutexで保護して渡します。
        m_packageWorker = std::thread(
            [this]
            {
                // パッケージ操作の失敗理由
                std::string error;
                // 解析した配布一覧の取得結果
                std::vector<PackageInfo> index;
                // 取得した配布一覧JSONの全文
                const auto body = HttpGetText(
                    PackageIndexHost,
                    PackageIndexPath);
                if (body.empty())
                {
                    error =
                        "パッケージ一覧を取得できませんでした。"
                        "接続とリポジトリの公開状態を確認してください。";
                }
                else
                {
                    try
                    {
                        index = ParsePackageIndex(body);
                    }
                    // 操作失敗を表示へ渡します(exception: 失敗理由)。
                    catch (const std::exception& exception)
                    {
                        error = std::string(
                            "パッケージ一覧を解釈できませんでした: ")
                            + exception.what();
                    }
                }

                // 完了結果の書込を保護するロック
                std::scoped_lock lock(m_packageResultMutex);
                m_packageWorkerResult = {};
                m_packageWorkerResult.ready = true;
                m_packageWorkerResult.error =
                    std::move(error);
                m_packageWorkerResult.index =
                    std::move(index);
            });
    }

    // 前のworkerを待ち、ZIP取得と検証・配置をバックグラウンドで開始します(package: コピーして保持する配布情報)。
    void EditorLayer::StartPackageInstall(
        const PackageInfo& package)
    {
        JoinPackageWorker();
        m_packageBusy = true;
        m_packagePanelError.clear();
        // パッケージ保存先のassets
        const auto assetRoot =
            m_graphics.Assets().AssetRoot();
        // バックグラウンドで処理し、完了結果をmutexで保護して渡します。
        m_packageWorker = std::thread(
            [this, package, assetRoot]
            {
                // パッケージ操作の失敗理由
                std::string error;
                // 導入パッケージにcppがあるか
                bool hasScripts = false;
                // ZIP取得先のホスト名
                std::wstring host;
                // ZIP取得先のパス
                std::wstring path;
                if (!SplitHttpsUrl(
                        package.downloadUrl,
                        host,
                        path))
                {
                    error = "ダウンロードURLが不正です。";
                }
                else
                {
                    // 取得したZIPのバイト列
                    const auto bytes = HttpGetBytes(
                        host,
                        path);
                    if (bytes.empty())
                    {
                        error =
                            "ダウンロードに失敗しました。"
                            "接続を確認して、もう一度お試しください。";
                    }
                    else
                    {
                        try
                        {
                            InstallPackage(
                                assetRoot,
                                package,
                                bytes);
                            hasScripts =
                                PackageContainsScripts(
                                    PackageInstallDirectory(
                                        assetRoot,
                                        package.name));
                        }
                        // 導入処理の失敗を記録します(exception: 失敗理由)。
                        catch (
                            const std::exception& exception)
                        {
                            error = exception.what();
                        }
                    }
                }

                // 完了結果の書込を保護するロック
                std::scoped_lock lock(m_packageResultMutex);
                m_packageWorkerResult = {};
                m_packageWorkerResult.ready = true;
                m_packageWorkerResult.wasInstall = true;
                m_packageWorkerResult.error =
                    std::move(error);
                m_packageWorkerResult.installedDisplayName =
                    package.displayName;
                m_packageWorkerResult.installedHasScripts =
                    hasScripts;
                m_packageWorkerResult.installedActivation =
                    error.empty()
                        ? InstalledPackageActivation(
                            assetRoot,
                            package.name)
                        : package.activation;
            });
    }

    // UIスレッドで完了結果を回収し、表示・資産一覧・必要なビルドを更新します。
    void EditorLayer::ConsumePackageWorkerResult()
    {
        // 回収したworkerの完了結果
        PackageWorkerResult result;
        {
            // 完了結果の回収を保護するロック
            std::scoped_lock lock(m_packageResultMutex);
            if (!m_packageWorkerResult.ready)
            {
                return;
            }
            result = std::move(m_packageWorkerResult);
            m_packageWorkerResult = {};
        }
        JoinPackageWorker();
        m_packageBusy = false;

        if (result.wasInstall)
        {
            if (result.error.empty())
            {
                SetStatus(
                    "パッケージ『"
                    + result.installedDisplayName
                    + "』をインストールしました"
                    + RestartNotice(
                        result.installedActivation));
                RefreshAssets();
                if (result.installedHasScripts)
                {
                    // C++スクリプト入りならGame Moduleを自動ビルドして、そのまま使える状態にします。
                    static_cast<void>(BuildGameModule());
                }
            }
            else
            {
                m_packagePanelError = result.error;
                SetStatus(
                    "パッケージのインストールに失敗しました: "
                    + result.error,
                    true);
            }
        }
        else
        {
            if (result.error.empty())
            {
                m_packages = std::move(result.index);
                m_packageListState =
                    PackageListState::Ready;
                if (m_selectedPackageIndex < 0
                    || m_selectedPackageIndex
                        >= static_cast<int>(m_packages.size())
                    || m_packages[static_cast<std::size_t>(
                        m_selectedPackageIndex)].target
                        != m_packageTargetFilter)
                {
                    m_selectedPackageIndex = -1;
                    // 配布一覧の項目添字
                    for (std::size_t index = 0;
                        index < m_packages.size(); ++index)
                    {
                        if (m_packages[index].target
                            == m_packageTargetFilter)
                        {
                            m_selectedPackageIndex =
                                static_cast<int>(index);
                            break;
                        }
                    }
                }
            }
            else
            {
                m_packageListState =
                    PackageListState::Failed;
                m_packagePanelError = result.error;
            }
        }
        RefreshInstalledPackageVersions();
    }

    // 配置済みパッケージから公式一覧に無いものを表示して削除操作を提供します。
    void EditorLayer::DrawInstalledPackagesSection()
    {

        // 配置済みパッケージの親パス
        const auto packagesRoot =
            m_graphics.Assets().AssetRoot() / L"packages";
        if (!std::filesystem::is_directory(packagesRoot))
        {
            return;
        }

        // 公式一覧に無い名前と版数の一覧
        std::vector<std::pair<std::string, std::string>>
            others;
        // パッケージ操作の失敗理由
        std::error_code error;
        // 配置済みパッケージのフォルダー
        for (const auto& entry :
            std::filesystem::directory_iterator(
                packagesRoot,
                error))
        {
            if (error || !entry.is_directory())
            {
                continue;
            }
            // 配置済みパッケージの名前
            const auto name =
                PathToUtf8(entry.path().filename());
            // 公式一覧に同名のパッケージがあるか(package: 比較する配布情報)。
            const bool official = std::ranges::any_of(
                m_packages,
                [&name](const PackageInfo& package)
                {
                    return package.name == name;
                });
            if (official)
            {
                continue;
            }
            others.emplace_back(
                name,
                InstalledPackageVersion(
                    m_graphics.Assets().AssetRoot(),
                    name));
        }
        if (others.empty())
        {
            return;
        }

        ImGui::SeparatorText(
            "このプロジェクトのパッケージ");
        // 公式一覧外のパッケージ名と配置済み版数
        for (const auto& [name, version] : others)
        {
            ImGui::BulletText(
                "%s%s%s",
                name.c_str(),
                version.empty() ? "" : "  v",
                version.c_str());
            ImGui::SameLine();
            ImGui::PushID(name.c_str());
            ImGui::BeginDisabled(m_packageBusy);
            if (ImGui::SmallButton("削除"))
            {
                try
                {
                    // 削除後の反映タイミング
                    const auto activation =
                        InstalledPackageActivation(
                            m_graphics.Assets().AssetRoot(),
                            name);
                    UninstallPackage(
                        m_graphics.Assets().AssetRoot(),
                        name);
                    RefreshAssets();
                    SetStatus(
                        "パッケージを削除しました: " + name
                        + RestartNotice(activation));
                }
                // 操作失敗を表示へ渡します(exception: 失敗理由)。
                catch (const std::exception& exception)
                {
                    SetStatus(exception.what(), true);
                }
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::Spacing();
    }

    // 再生中以外に手元のZIPを選んで配置し、cppを含む場合はビルドを要求します。
    void EditorLayer::ImportPackageFromZipDialog()
    {
        if (m_playing)
        {
            return;
        }

        // 選択したZIPのパス出力領域
        std::array<wchar_t, 32768> filename{};
        // ZIP選択のファイル種別指定
        constexpr wchar_t filter[] =
            L"LamaPonパッケージ (*.zip)\0*.zip\0\0";

        // WindowsのZIP選択情報
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = m_window;
        dialog.lpstrFilter = filter;
        dialog.nFilterIndex = 1;
        dialog.lpstrFile = filename.data();
        dialog.nMaxFile =
            static_cast<DWORD>(filename.size());
        dialog.lpstrTitle =
            L"パッケージのZipを選択";
        dialog.Flags = OFN_FILEMUSTEXIST
            | OFN_PATHMUSTEXIST
            | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&dialog))
        {
            return;
        }

        try
        {
            // ZIPから導入したパッケージ情報
            const auto installed = InstallPackageFromFile(
                m_graphics.Assets().AssetRoot(),
                std::filesystem::path{ filename.data() });
            RefreshAssets();
            RefreshInstalledPackageVersions();
            SetStatus(
                "パッケージを読み込みました: "
                + installed.displayName
                + " v"
                + installed.version
                + RestartNotice(installed.activation));
            // C++スクリプトを含む場合はそのまま使えるようにGame Moduleをビルドします。
            if (PackageContainsScripts(
                PackageInstallDirectory(
                    m_graphics.Assets().AssetRoot(),
                    installed.name)))
            {
                static_cast<void>(BuildGameModule());
            }
        }
        // 導入失敗を表示へ渡します(exception: 失敗理由)。
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

    // 作成用入力と前回結果を初期化してダイアログ表示を要求します。
    void EditorLayer::OpenPackageBuildDialog()
    {
        m_packageBuildNameBuffer.fill('\0');
        m_packageBuildDisplayNameBuffer.fill('\0');
        m_packageBuildDescriptionBuffer.fill('\0');
        m_packageBuildAuthorBuffer.fill('\0');
        m_packageBuildVersionBuffer.fill('\0');
        strncpy_s(
            m_packageBuildVersionBuffer.data(),
            m_packageBuildVersionBuffer.size(),
            "1.0",
            _TRUNCATE);
        m_packageBuildError.clear();
        m_packageBuildIndexEntry.clear();
        m_packageBuildDialogRequested = true;
    }

    // 配布情報を入力してZIPを作り、配布一覧用のJSONを表示します。
    void EditorLayer::DrawPackageBuildDialog()
    {
        // 作成ダイアログの識別名
        constexpr const char* popupName =
            "パッケージを作成##PackageBuild";
        if (m_packageBuildDialogRequested)
        {
            ImGui::OpenPopup(popupName);
            m_packageBuildDialogRequested = false;
        }

        ImGui::SetNextWindowSize(
            ImVec2{ 620.0f, 0.0f },
            ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal(
            popupName,
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }

        ImGui::TextWrapped(
            "assets/packages/<パッケージ名>/ に入れたファイルを、"
            "配布できるZipへまとめます。まずそのフォルダーを作って、"
            "スクリプトやPrefabを入れてください。");
        ImGui::Separator();

        ImGui::SetNextItemWidth(320.0f);
        ImGui::InputText(
            "パッケージ名",
            m_packageBuildNameBuffer.data(),
            m_packageBuildNameBuffer.size());
        ImGui::TextDisabled(
            "英小文字・数字・-・_ のみ（フォルダー名になります）");
        ImGui::SetNextItemWidth(320.0f);
        ImGui::InputText(
            "表示名",
            m_packageBuildDisplayNameBuffer.data(),
            m_packageBuildDisplayNameBuffer.size());
        ImGui::SetNextItemWidth(460.0f);
        ImGui::InputTextMultiline(
            "説明",
            m_packageBuildDescriptionBuffer.data(),
            m_packageBuildDescriptionBuffer.size(),
            ImVec2{ 460.0f, 60.0f });
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputText(
            "作者",
            m_packageBuildAuthorBuffer.data(),
            m_packageBuildAuthorBuffer.size());
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputText(
            "バージョン",
            m_packageBuildVersionBuffer.data(),
            m_packageBuildVersionBuffer.size());
        ImGui::SameLine();
        ImGui::TextDisabled(
            "必要エンジン: v%.*s",
            static_cast<int>(
                LamaPon::VersionString.size()),
            LamaPon::VersionString.data());

        if (!m_packageBuildError.empty())
        {
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.35f, 0.30f, 1.0f },
                "%s",
                m_packageBuildError.c_str());
        }
        if (!m_packageBuildIndexEntry.empty())
        {
            ImGui::Separator();
            ImGui::TextWrapped(
                "配布するには、下のJSONを配布リポジトリの"
                "packages/index.jsonへ追加し、Zipを同じ"
                "packages/フォルダーへ置きます。");
            ImGui::InputTextMultiline(
                "##PackageIndexEntry",
                m_packageBuildIndexEntry.data(),
                m_packageBuildIndexEntry.size() + 1,
                ImVec2{ 560.0f, 150.0f },
                ImGuiInputTextFlags_ReadOnly);
            if (ImGui::Button("JSONをコピー"))
            {
                ImGui::SetClipboardText(
                    m_packageBuildIndexEntry.c_str());
                SetStatus("index.json用のJSONをコピーしました");
            }
            ImGui::SameLine();
        }

        ImGui::Spacing();
        if (ImGui::Button(
            "Zipを作成",
            ImVec2{ 130.0f, 0.0f }))
        {
            try
            {
                // 入力したパッケージ配布情報
                PackageInfo package;
                package.name =
                    m_packageBuildNameBuffer.data();
                package.displayName =
                    m_packageBuildDisplayNameBuffer.data();
                package.description =
                    m_packageBuildDescriptionBuffer.data();
                package.author =
                    m_packageBuildAuthorBuffer.data();
                package.version =
                    m_packageBuildVersionBuffer.data();
                package.minimumEngineVersion =
                    std::string(LamaPon::VersionString);

                // 配布ZIPの保存先
                const auto outputDirectory =
                    m_graphics.Assets().AssetRoot()
                        .parent_path()
                    / L"dist"
                    / L"packages";
                // 作成したZIPと配布一覧用JSON
                const auto built = BuildPackage(
                    m_graphics.Assets().AssetRoot(),
                    package,
                    outputDirectory);
                m_packageBuildError.clear();
                m_packageBuildIndexEntry =
                    built.indexEntryJson;
                RefreshAssets();
                RefreshInstalledPackageVersions();
                SetStatus(
                    "パッケージを作成しました: "
                    + PathToUtf8(built.zipPath)
                    + "（"
                    + std::to_string(built.fileCount)
                    + "ファイル）");
            }
            // 作成失敗を表示へ渡します(exception: 失敗理由)。
            catch (const std::exception& exception)
            {
                m_packageBuildError = exception.what();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("閉じる"))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // パッケージ管理からLive2D導入の入口を開き、必要なら配布一覧を取得します。
    void EditorLayer::OpenLive2DSetupWizard()
    {
        m_live2DSetupOpen = true;
        m_live2DSetupPopupRequested = true;
        m_live2DSetupMessage.clear();
        if (m_packageListState == PackageListState::NotLoaded
            && !m_packageBusy)
        {
            StartPackageIndexFetch();
        }
    }

    // 拡張・公式SDK・モデル素材を順に導入し、モデルScriptをシーンへ追加します。
    void EditorLayer::DrawLive2DSetupWizard()
    {
        if (!m_live2DSetupOpen)
        {
            return;
        }

        constexpr const char* popupName =
            "Live2Dをはじめる##Live2DSetup";
        if (m_live2DSetupPopupRequested)
        {
            ImGui::OpenPopup(popupName);
            m_live2DSetupPopupRequested = false;
        }

        ImGui::SetNextWindowSize(
            ImVec2{ 730.0f, 670.0f },
            ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal(
                popupName,
                nullptr,
                ImGuiWindowFlags_NoCollapse))
        {
            return;
        }

        ImGui::BeginChild(
            "Live2DSetupContents",
            ImVec2{ 0.0f, -ImGui::GetFrameHeightWithSpacing() },
            false,
            ImGuiWindowFlags_AlwaysVerticalScrollbar);
        ImGui::TextWrapped(
            "Live2D拡張、公式Cubism Core、モデル素材を順に準備します。"
            "画面の案内に沿って進めてください。");
        ImGui::Separator();

        const auto assetRoot = m_graphics.Assets().AssetRoot();
        const auto packageRoot = PackageInstallDirectory(
            assetRoot,
            "live2d-cubism");
        const bool extensionInstalled = std::filesystem::is_regular_file(
            packageRoot / L"package.json");

        ImGui::SeparatorText("1. Live2D拡張");
        ImGui::TextWrapped(
            "モデル読み込みと2D描画を行う拡張を、このゲームへ追加します。");
        if (!extensionInstalled)
        {
            const auto package = std::ranges::find_if(
                m_packages,
                [](const PackageInfo& candidate)
                {
                    return candidate.name == "live2d-cubism"
                        && candidate.target == PackageTarget::Project;
                });
            if (package != m_packages.end())
            {
                ImGui::BeginDisabled(
                    m_packageBusy || m_playing);
                if (ImGui::Button(
                    "Live2D拡張をインストール",
                    ImVec2{ 235.0f, 0.0f }))
                {
                    StartPackageInstall(*package);
                }
                ImGui::EndDisabled();
            }
            else if (m_packageListState == PackageListState::Failed)
            {
                ImGui::TextColored(
                    ImVec4{ 1.0f, 0.65f, 0.30f, 1.0f },
                    "パッケージ一覧を取得できませんでした。");
                ImGui::BeginDisabled(m_packageBusy || m_playing);
                if (ImGui::Button("一覧を再読み込み"))
                {
                    StartPackageIndexFetch();
                }
                ImGui::EndDisabled();
            }
            else
            {
                ImGui::TextDisabled(
                    m_packageBusy
                        ? "Live2D拡張を含むパッケージ一覧を取得中です..."
                        : "パッケージ一覧を読み込んでいます...");
            }
        }
        else
        {
            ImGui::TextColored(
                ImVec4{ 0.45f, 0.80f, 0.55f, 1.0f },
                "Live2D拡張はこのゲームへ導入済みです。");
        }

        ImGui::SeparatorText("2. 公式Cubism SDK");
        ImGui::TextWrapped(
            "Cubism CoreはLive2D公式SDKから取得します。SDKの利用条件を確認し、"
            "公式サイトからNative版をダウンロードして展開してください。"
            "SDK/Coreはこのパッケージに同梱しません。");
        if (ImGui::Button(
            "Live2D公式 SDK ダウンロードページを開く"))
        {
            const HINSTANCE result = ShellExecuteW(
                m_window,
                L"open",
                L"https://www.live2d.com/sdk/download/native/",
                nullptr,
                nullptr,
                SW_SHOWNORMAL);
            if (reinterpret_cast<std::intptr_t>(result) <= 32)
            {
                m_live2DSetupMessage =
                    "ブラウザーを開けませんでした。公式サイトのURLをコピーして開いてください。";
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("URLをコピー"))
        {
            ImGui::SetClipboardText(
                "https://www.live2d.com/sdk/download/native/");
            m_live2DSetupMessage =
                "公式ダウンロードページのURLをコピーしました。";
        }

        ImGui::SeparatorText("3. SDKの検査と導入");
        ImGui::TextWrapped(
            "展開先の上位フォルダーを選択してください。LamaPonがCoreの"
            "ヘッダー、Windows x64 DLL、MSVC /MDライブラリ、対応APIを検査します。");
        ImGui::BeginDisabled(!extensionInstalled || m_packageBusy || m_playing);
        if (ImGui::Button("Cubism SDKの展開先を選ぶ..."))
        {
            try
            {
                if (const auto selected =
                    BrowseForCubismSdkFolder(m_window))
                {
                    m_live2DSdkSource = *selected;
                    const auto inspection =
                        InspectLive2DCubismSdk(m_live2DSdkSource);
                    m_live2DSdkSourceValid = inspection.IsValid();
                    m_live2DSetupMessage = inspection.IsValid()
                        ? "SDKを検査しました。プロジェクトへの導入を続けられます。"
                        : inspection.error;
                }
            }
            catch (const std::exception& exception)
            {
                m_live2DSdkSourceValid = false;
                m_live2DSetupMessage = exception.what();
            }
        }
        ImGui::EndDisabled();

        if (!m_live2DSdkSource.empty())
        {
            ImGui::TextWrapped(
                "選択先: %s",
                PathToUtf8(m_live2DSdkSource).c_str());
            ImGui::SameLine();
            ImGui::TextColored(
                m_live2DSdkSourceValid
                    ? ImVec4{ 0.45f, 0.80f, 0.55f, 1.0f }
                    : ImVec4{ 1.0f, 0.55f, 0.35f, 1.0f },
                m_live2DSdkSourceValid ? "検査OK" : "要確認");
        }

        const bool sdkBusy = m_packageBusy
            || m_playing
            || m_gameModuleBuildProcess != nullptr;
        ImGui::BeginDisabled(
            !extensionInstalled
            || !m_live2DSdkSourceValid
            || sdkBusy);
        if (ImGui::Button(
            "検査済みSDKをこのプロジェクトへ導入",
            ImVec2{ 285.0f, 0.0f }))
        {
            try
            {
                const auto inspection =
                    InspectLive2DCubismSdk(m_live2DSdkSource);
                if (!inspection.IsValid())
                {
                    m_live2DSdkSourceValid = false;
                    m_live2DSetupMessage = inspection.error;
                }
                else
                {
                    InstallLive2DCubismSdk(
                        inspection,
                        packageRoot);
                    m_live2DSdkStatusChecked = true;
                    m_live2DSdkInstalledValid = true;
                    m_live2DSdkInstalledError.clear();
                    m_live2DSdkBuildPending = true;
                    m_live2DSdkBuildFailed = false;
                    m_live2DSetupMessage =
                        "Cubism Coreを導入しました。Game Moduleを再ビルドしています。";
                    RefreshAssets();
                    if (!BuildGameModule())
                    {
                        m_live2DSdkBuildFailed = true;
                    }
                }
            }
            catch (const std::exception& exception)
            {
                m_live2DSetupMessage = exception.what();
            }
        }
        ImGui::EndDisabled();

        if (extensionInstalled && !m_live2DSdkStatusChecked)
        {
            const auto installedInspection = InspectLive2DCubismSdk(
                packageRoot / L"sdk");
            m_live2DSdkInstalledValid = installedInspection.IsValid();
            m_live2DSdkInstalledError = installedInspection.error;
            m_live2DSdkStatusChecked = true;
        }
        if (extensionInstalled && m_live2DSdkStatusChecked)
        {
            if (m_live2DSdkInstalledValid)
            {
                ImGui::TextColored(
                    ImVec4{ 0.45f, 0.80f, 0.55f, 1.0f },
                    "Cubism Coreはプロジェクトへ導入済みです。");
            }
            else
            {
                ImGui::TextDisabled(
                    "プロジェクト内のCoreは未導入です。"
                    "上のSDKフォルダー選択から進めてください。");
                if (!m_live2DSdkInstalledError.empty())
                {
                    ImGui::TextWrapped(
                        "検査結果: %s",
                        m_live2DSdkInstalledError.c_str());
                }
            }
        }

        ImGui::SeparatorText("4. Game Moduleのビルド");
        if (m_gameModuleBuildProcess != nullptr)
        {
            ImGui::TextDisabled(
                "Game Moduleをビルドしています。完了するとモデルを追加できます。");
        }
        else if (m_live2DSdkInstalledValid)
        {
            auto* const module = GameModuleHost::Current();
            const bool live2DRegistered = module != nullptr
                && module->FindComponent("Game.Live2DModel") != nullptr;
            if (m_live2DSdkBuildFailed)
            {
                ImGui::TextColored(
                    ImVec4{ 1.0f, 0.55f, 0.35f, 1.0f },
                    "Game Moduleのビルドに失敗しました。ログを確認して再試行してください。");
                ImGui::BeginDisabled(m_playing || m_packageBusy);
                if (ImGui::Button("Game Moduleを再ビルド"))
                {
                    m_live2DSdkBuildPending = true;
                    m_live2DSdkBuildFailed = false;
                    if (!BuildGameModule())
                    {
                        m_live2DSdkBuildFailed = true;
                    }
                }
                ImGui::EndDisabled();
                if (!m_gameModuleBuildLogPath.empty())
                {
                    ImGui::TextWrapped(
                        "ログ: %s",
                        PathToUtf8(m_gameModuleBuildLogPath).c_str());
                }
            }
            else if (live2DRegistered && !m_live2DSdkBuildPending)
            {
                ImGui::TextColored(
                    ImVec4{ 0.45f, 0.80f, 0.55f, 1.0f },
                    "Live2D Scriptのビルドと読み込みが完了しています。");
            }
            else
            {
                ImGui::TextWrapped(
                    "SDKを導入した後にGame Moduleをビルドしてください。"
                    "成功するとモデルをシーンへ追加できます。");
                ImGui::BeginDisabled(m_playing);
                if (ImGui::Button("Game Moduleをビルド"))
                {
                    static_cast<void>(BuildGameModule());
                }
                ImGui::EndDisabled();
            }
        }
        else
        {
            ImGui::TextDisabled(
                "Cubism Coreを導入すると、Game Moduleを自動で再ビルドします。");
        }

        ImGui::SeparatorText("5. 最初のLive2Dモデル");
        ImGui::TextWrapped(
            "model3 JSONを選ぶと、同じフォルダーのモデル素材を"
            "assets/Live2Dへコピーし、現在のシーンにLive2Dモデルを追加します。");
        auto* const module = GameModuleHost::Current();
        const bool live2DReady = extensionInstalled
            && m_live2DSdkInstalledValid
            && !m_live2DSdkBuildPending
            && !m_live2DSdkBuildFailed
            && module != nullptr
            && module->FindComponent("Game.Live2DModel") != nullptr;
        ImGui::BeginDisabled(
            !live2DReady || m_playing || m_packageBusy
            || m_gameModuleBuildProcess != nullptr);
        if (ImGui::Button(
            "model3.jsonを選んでシーンへ追加...",
            ImVec2{ 285.0f, 0.0f }))
        {
            try
            {
                if (const auto selected = BrowseForModel3Json(m_window))
                {
                    const auto relativeModel = ImportLive2DModel(
                        *selected,
                        assetRoot);
                    RefreshAssets();
                    // 論理画面の中央に表示する設定JSON
                    const nlohmann::json properties{
                        { "modelPath", PathToUtf8(relativeModel) },
                        { "positionX", m_projectWindowSize[0] * 0.5f },
                        { "positionY", m_projectWindowSize[1] * 0.5f },
                        { "pixelsPerUnit", 0.0f }
                    };
                    auto& gameObject = m_scene.CreateGameObject(
                        "Live2Dモデル");
                    gameObject.AddComponent<NativeScriptComponent>(
                        "Game.Live2DModel",
                        properties.dump());
                    m_selectedObjectId = gameObject.Id();
                    m_selectedAsset.clear();
                    RecordHistory();
                    m_live2DImportedModelPath =
                        PathToUtf8(relativeModel);
                    m_live2DSetupMessage =
                        "モデルをシーンへ追加しました。再生して表示を確認してください。";
                    SetStatus(
                        "Live2Dモデルをシーンへ追加しました。再生して表示を確認してください");
                }
            }
            catch (const std::exception& exception)
            {
                m_live2DSetupMessage = exception.what();
            }
        }
        ImGui::EndDisabled();

        if (!live2DReady && m_live2DSdkInstalledValid
            && m_gameModuleBuildProcess == nullptr)
        {
            ImGui::TextDisabled(
                "Live2D Scriptが読み込まれていません。Game Moduleをビルドしてください。");
        }
        if (!m_live2DImportedModelPath.empty())
        {
            ImGui::TextWrapped(
                "追加したモデル: %s",
                m_live2DImportedModelPath.c_str());
        }

        if (!m_live2DSetupMessage.empty())
        {
            ImGui::Spacing();
            ImGui::TextWrapped(
                "%s",
                m_live2DSetupMessage.c_str());
        }

        ImGui::EndChild();
        ImGui::Separator();
        if (ImGui::Button("閉じる"))
        {
            m_live2DSetupOpen = false;
            m_live2DSdkStatusChecked = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // 配布一覧と導入状態を表示し、エンジン互換性を確認して操作を提供します(open: パネルの表示状態)。
    void EditorLayer::DrawPackagesPanel(bool& open)
    {
        if (!open)
        {
            return;
        }

        ImGui::SetNextWindowSize(
            ImVec2{ 640.0f, 420.0f },
            ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(
                "パッケージ",
                &open,
                ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            return;
        }

        DrawLive2DSetupWizard();

        // 初回表示で自動的に一覧を取得します。
        if (m_packageListState
                == PackageListState::NotLoaded
            && !m_packageBusy)
        {
            StartPackageIndexFetch();
        }

        ImGui::BeginDisabled(m_packageBusy);
        if (ImGui::Button("再読み込み"))
        {
            StartPackageIndexFetch();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (m_packageBusy)
        {
            ImGui::TextDisabled("通信中...");
        }
        else if (m_packageListState
            == PackageListState::Ready)
        {
            ImGui::TextDisabled(
                "%d件のパッケージ",
                static_cast<int>(m_packages.size()));
        }

        // D3D12パッケージの配置パス
        const auto d3d12PackageDirectory =
            PackageInstallDirectory(
                m_graphics.Assets().AssetRoot(),
                DirectX12BackendPackageName);
        if (std::filesystem::is_directory(
                d3d12PackageDirectory))
        {
            ImGui::Spacing();
            ImGui::SeparatorText("描画API");
            ImGui::Text(
                "現在の設定: %s",
                RenderingApiDisplayName(
                    m_projectSettings.graphics.renderingApi));
            ImGui::TextWrapped(
                "DirectX 12 Rendererを使用するには、プロジェクトの"
                "描画APIを選択してエディターを再起動してください。");
            if (ImGui::Button("描画APIを選択..."))
            {
                m_projectSettingsCategory = 1;
                OpenProjectSettingsDialog();
            }
        }

        if (m_packageListState == PackageListState::Failed)
        {
            ImGui::TextColored(
                ImVec4{ 1.0f, 0.65f, 0.30f, 1.0f },
                "%s",
                m_packagePanelError.c_str());
            ImGui::TextWrapped(
                "接続を確認して「再読み込み」を押してください。"
                "インストール済みのパッケージは、オフラインでも"
                "そのまま動作します。");
            ImGui::End();
            return;
        }

        // 公式一覧に無い、手元で入れた／作ったパッケージ。
        DrawInstalledPackagesSection();

        if (m_packageListState == PackageListState::Ready
            && m_packages.empty())
        {
            ImGui::Spacing();
            ImGui::SeparatorText("パッケージ");
            ImGui::TextWrapped(
                "パッケージはまだ登録されていません。"
                "公開までお楽しみに！");
            ImGui::End();
            return;
        }

        if (m_packageListState != PackageListState::Ready)
        {
            ImGui::End();
            return;
        }

        ImGui::SeparatorText("パッケージ");

        // 追加先の分類を切り替えて最初の一致項目を選びます(target: 選択する追加先の分類)。
        const auto selectFirstOfTarget = [this](
            const PackageTarget target)
        {
            m_packageTargetFilter = target;
            m_selectedPackageIndex = -1;
            // 配布一覧の項目添字
            for (std::size_t index = 0;
                index < m_packages.size(); ++index)
            {
                if (m_packages[index].target == target)
                {
                    m_selectedPackageIndex =
                        static_cast<int>(index);
                    break;
                }
            }
        };
        if (ImGui::RadioButton(
                "ゲームに追加",
                m_packageTargetFilter == PackageTarget::Project))
        {
            selectFirstOfTarget(PackageTarget::Project);
        }
        ImGui::SameLine();
        if (ImGui::RadioButton(
                "エンジンの機能を追加",
                m_packageTargetFilter == PackageTarget::Engine))
        {
            selectFirstOfTarget(PackageTarget::Engine);
        }
        ImGui::TextDisabled(
            "%s",
            m_packageTargetFilter == PackageTarget::Project
                ? "スクリプト・Prefab・素材などをゲームへ追加します。"
                : "描画機能などを追加します。再起動が必要な場合があります。");

        // 左に一覧、右に選択したパッケージの詳細。
        ImGui::BeginChild(
            "PackageList",
            ImVec2{ 240.0f, 0.0f },
            true);
        // 選択した分類の配布項目が無いか判定します(package: 比較する配布情報)。
        if (std::ranges::none_of(
                m_packages,
                [this](const PackageInfo& package)
                {
                    return package.target
                        == m_packageTargetFilter;
                }))
        {
            ImGui::TextWrapped(
                "この分類のパッケージはまだありません。");
        }
        // 配布一覧の項目添字
        for (std::size_t index = 0;
            index < m_packages.size();
            ++index)
        {
            // 操作または表示するパッケージ
            const auto& package = m_packages[index];
            if (package.target != m_packageTargetFilter)
            {
                continue;
            }
            // 導入済みバージョンの検索結果
            const auto installedIterator =
                m_installedPackageVersions.find(
                    package.name);
            // 導入済みのバージョンがあるか
            const bool installed =
                installedIterator
                    != m_installedPackageVersions.end()
                && !installedIterator->second.empty();
            // 配布一覧の項目表示名
            std::string label = package.displayName;
            if (installed)
            {
                label += "  [導入済み]";
            }
            label += "##pkg"
                + std::to_string(index);
            if (ImGui::Selectable(
                label.c_str(),
                m_selectedPackageIndex
                    == static_cast<int>(index)))
            {
                m_selectedPackageIndex =
                    static_cast<int>(index);
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();

        ImGui::BeginChild("PackageDetails");
        if (m_selectedPackageIndex >= 0
            && m_selectedPackageIndex
                < static_cast<int>(m_packages.size())
            && m_packages[static_cast<std::size_t>(
                m_selectedPackageIndex)].target
                == m_packageTargetFilter)
        {
            // 操作または表示するパッケージ
            const auto& package = m_packages[
                static_cast<std::size_t>(
                    m_selectedPackageIndex)];
            // 配置済みパッケージの版数
            const std::string installedVersion =
                m_installedPackageVersions.count(
                    package.name) != 0
                    ? m_installedPackageVersions.at(
                        package.name)
                    : std::string{};
            // 導入済みのバージョンがあるか
            const bool installed =
                !installedVersion.empty();
            // 新しい配布版があるか
            const bool updateAvailable = installed
                && IsNewerVersion(
                    installedVersion,
                    package.version);

            // 必要エンジン版数を満たさないか
            const bool engineTooOld =
                !package.minimumEngineVersion.empty()
                && IsNewerVersion(
                    LamaPon::VersionString,
                    package.minimumEngineVersion);

            ImGui::TextUnformatted(
                package.displayName.c_str());
            ImGui::TextDisabled(
                "v%s ・ %s ・ %s",
                package.version.c_str(),
                package.author.empty()
                    ? "公式"
                    : package.author.c_str(),
                FormatPackageSize(
                    package.sizeBytes).c_str());
            if (installed)
            {
                ImGui::TextColored(
                    ImVec4{ 0.45f, 0.80f, 0.55f, 1.0f },
                    "導入済み v%s%s",
                    installedVersion.c_str(),
                    updateAvailable
                        ? "（更新があります）"
                        : "");
            }
            ImGui::Separator();
            ImGui::TextWrapped(
                "%s",
                package.description.c_str());
            ImGui::TextDisabled(
                "追加先: %s",
                package.target == PackageTarget::Engine
                    ? "エンジンの機能" : "ゲーム");
            ImGui::SeparatorText("使えるまでの手順");
            if (package.name == DirectX12BackendPackageName)
            {
                ImGui::TextWrapped(
                    "インストール後、プロジェクト設定で"
                    "DirectX 12 Experimentalを選び、"
                    "エディターを再起動してください。");
            }
            else if (package.activation
                == PackageActivation::RestartAndRebuild)
            {
                ImGui::TextWrapped(
                    "インストール後、エディターを再起動し、"
                    "ゲームを再ビルドしてください。");
            }
            else if (package.activation
                == PackageActivation::Restart)
            {
                ImGui::TextWrapped(
                    "インストール後、エディターまたはゲームを"
                    "再起動してください。");
            }
            else
            {
                ImGui::TextWrapped(
                    "インストール後に使えます。C++スクリプトが"
                    "含まれる場合は自動でビルドします。");
            }
            if (PackageRequiresRestart(package.activation))
            {
                ImGui::TextDisabled(
                    "更新・削除も再起動後に反映されます。");
            }
            ImGui::Spacing();

            if (engineTooOld)
            {
                ImGui::TextColored(
                    ImVec4{ 1.0f, 0.65f, 0.30f, 1.0f },
                    "このパッケージにはエンジン v%s 以降が必要です"
                    "（現在 v%.*s）。",
                    package.minimumEngineVersion.c_str(),
                    static_cast<int>(
                        LamaPon::VersionString.size()),
                    LamaPon::VersionString.data());
                ImGui::TextWrapped(
                    "LamaPon Hubを起動すると新しいエンジンの"
                    "ダウンロード案内が表示されます。");
            }

            ImGui::BeginDisabled(
                m_packageBusy || engineTooOld);
            // 導入状況に応じた操作表示名
            const char* installLabel = !installed
                ? "インストール"
                : (updateAvailable
                    ? "更新する"
                    : "再インストール");
            if (ImGui::Button(
                installLabel,
                ImVec2{ 150.0f, 0.0f }))
            {
                StartPackageInstall(package);
            }
            ImGui::EndDisabled();
            if (installed)
            {
                ImGui::SameLine();
                ImGui::BeginDisabled(m_packageBusy);
                if (ImGui::Button("削除"))
                {
                    try
                    {
                        UninstallPackage(
                            m_graphics.Assets().AssetRoot(),
                            package.name);
                        SetStatus(
                            "パッケージ『"
                            + package.displayName
                            + "』を削除しました"
                            + RestartNotice(package.activation));
                        RefreshAssets();
                    }
                    // 操作失敗を表示へ渡します(exception: 失敗理由)。
                    catch (const std::exception& exception)
                    {
                        SetStatus(
                            exception.what(),
                            true);
                    }
                    RefreshInstalledPackageVersions();
                }
                ImGui::EndDisabled();
            }
            if (!m_packagePanelError.empty())
            {
                ImGui::TextColored(
                    ImVec4{ 1.0f, 0.35f, 0.30f, 1.0f },
                    "%s",
                    m_packagePanelError.c_str());
            }
            ImGui::Spacing();
            ImGui::TextDisabled(
                "インストール先: assets/packages/%s/",
                package.name.c_str());
        }
        ImGui::EndChild();

        ImGui::End();
    }
}
