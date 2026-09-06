#include "LamaPon/Editor/EditorLayer.h"

#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Core/HttpClient.h"
#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/Version.h"
#include "LamaPon/Core/VersionCompare.h"
#include "LamaPon/Editor/PackageManager.h"
#include "LamaPon/Graphics/GraphicsDevice.h"

#include <imgui.h>

// ファイル選択ダイアログ（GetOpenFileNameW）に必要。
#include <commdlg.h>

#include <algorithm>
#include <array>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace LamaPon
{
    namespace
    {
        // パッケージにC++スクリプトが含まれるか
        // （含まれる場合はGame Moduleの自動ビルドを起動します）。
        bool PackageContainsScripts(
            const std::filesystem::path& packageDirectory)
        {
            std::error_code error;
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
    }

    void EditorLayer::JoinPackageWorker()
    {
        if (m_packageWorker.joinable())
        {
            m_packageWorker.join();
        }
    }

    void EditorLayer::RefreshInstalledPackageVersions()
    {
        m_installedPackageVersions.clear();
        const auto assetRoot =
            m_graphics.Assets().AssetRoot();
        for (const auto& package : m_packages)
        {
            m_installedPackageVersions[package.name] =
                InstalledPackageVersion(
                    assetRoot,
                    package.name);
        }
    }

    void EditorLayer::StartPackageIndexFetch()
    {
        JoinPackageWorker();
        m_packageBusy = true;
        m_packageListState = PackageListState::Loading;
        m_packagePanelError.clear();
        m_packageWorker = std::thread(
            [this]
            {
                std::string error;
                std::vector<PackageInfo> index;
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
                    catch (const std::exception& exception)
                    {
                        error = std::string(
                            "パッケージ一覧を解釈できませんでした: ")
                            + exception.what();
                    }
                }

                std::scoped_lock lock(m_packageResultMutex);
                m_packageWorkerResult = {};
                m_packageWorkerResult.ready = true;
                m_packageWorkerResult.error =
                    std::move(error);
                m_packageWorkerResult.index =
                    std::move(index);
            });
    }

    void EditorLayer::StartPackageInstall(
        const PackageInfo& package)
    {
        JoinPackageWorker();
        m_packageBusy = true;
        m_packagePanelError.clear();
        const auto assetRoot =
            m_graphics.Assets().AssetRoot();
        m_packageWorker = std::thread(
            [this, package, assetRoot]
            {
                std::string error;
                bool hasScripts = false;
                std::wstring host;
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
                        catch (
                            const std::exception& exception)
                        {
                            error = exception.what();
                        }
                    }
                }

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
            });
    }

    void EditorLayer::ConsumePackageWorkerResult()
    {
        PackageWorkerResult result;
        {
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
                    + "』をインストールしました");
                RefreshAssets();
                if (result.installedHasScripts)
                {
                    // C++スクリプト入りならGame Moduleを
                    // 自動ビルドして、そのまま使える状態にします。
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
                    && !m_packages.empty())
                {
                    m_selectedPackageIndex = 0;
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

    void EditorLayer::DrawInstalledPackagesSection()
    {
        // assets/packages/ を直接見て、公式一覧に載っていない
        // パッケージ（自作・受け取り物）を分けて表示します。
        const auto packagesRoot =
            m_graphics.Assets().AssetRoot() / L"packages";
        if (!std::filesystem::is_directory(packagesRoot))
        {
            return;
        }

        std::vector<std::pair<std::string, std::string>>
            others;
        std::error_code error;
        for (const auto& entry :
            std::filesystem::directory_iterator(
                packagesRoot,
                error))
        {
            if (error || !entry.is_directory())
            {
                continue;
            }
            const auto name =
                PathToUtf8(entry.path().filename());
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
            "このプロジェクトのパッケージ（公式一覧外）");
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
                    UninstallPackage(
                        m_graphics.Assets().AssetRoot(),
                        name);
                    RefreshAssets();
                    SetStatus(
                        "パッケージを削除しました: " + name);
                }
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

    void EditorLayer::ImportPackageFromZipDialog()
    {
        if (m_playing)
        {
            return;
        }

        std::array<wchar_t, 32768> filename{};
        constexpr wchar_t filter[] =
            L"LamaPonパッケージ (*.zip)\0*.zip\0\0";

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
            const auto installed = InstallPackageFromFile(
                m_graphics.Assets().AssetRoot(),
                std::filesystem::path{ filename.data() });
            RefreshAssets();
            RefreshInstalledPackageVersions();
            SetStatus(
                "パッケージを読み込みました: "
                + installed.displayName
                + " v"
                + installed.version);
            // C++スクリプトを含む場合はそのまま使えるように
            // Game Moduleをビルドします。
            if (PackageContainsScripts(
                PackageInstallDirectory(
                    m_graphics.Assets().AssetRoot(),
                    installed.name)))
            {
                static_cast<void>(BuildGameModule());
            }
        }
        catch (const std::exception& exception)
        {
            SetStatus(exception.what(), true);
        }
    }

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

    void EditorLayer::DrawPackageBuildDialog()
    {
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

                const auto outputDirectory =
                    m_graphics.Assets().AssetRoot()
                        .parent_path()
                    / L"dist"
                    / L"packages";
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
            ImGui::SeparatorText("公式パッケージ");
            ImGui::TextWrapped(
                "公式パッケージはまだ登録されていません。"
                "公開までお楽しみに！");
            ImGui::End();
            return;
        }

        if (m_packageListState != PackageListState::Ready)
        {
            ImGui::End();
            return;
        }

        ImGui::SeparatorText("公式パッケージ");

        // 左に一覧、右に選択したパッケージの詳細。
        ImGui::BeginChild(
            "PackageList",
            ImVec2{ 240.0f, 0.0f },
            true);
        for (std::size_t index = 0;
            index < m_packages.size();
            ++index)
        {
            const auto& package = m_packages[index];
            const auto installedIterator =
                m_installedPackageVersions.find(
                    package.name);
            const bool installed =
                installedIterator
                    != m_installedPackageVersions.end()
                && !installedIterator->second.empty();
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
                < static_cast<int>(m_packages.size()))
        {
            const auto& package = m_packages[
                static_cast<std::size_t>(
                    m_selectedPackageIndex)];
            const std::string installedVersion =
                m_installedPackageVersions.count(
                    package.name) != 0
                    ? m_installedPackageVersions.at(
                        package.name)
                    : std::string{};
            const bool installed =
                !installedVersion.empty();
            const bool updateAvailable = installed
                && IsNewerVersion(
                    installedVersion,
                    package.version);
            // エンジンが古い場合はインストールさせず、更新を促します。
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
                            + "』を削除しました");
                        RefreshAssets();
                    }
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
