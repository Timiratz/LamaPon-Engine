#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/ProjectInstance.h"
#include "LamaPon/Core/CrashReporter.h"
#include "LamaPon/Core/Version.h"
#include "LamaPon/Resources/WindowsResource.h"
#include "LamaPon/Hub/ProjectHub.h"
#include "LamaPon/Hub/UpdateChecker.h"

#include <Windows.h>
#include <ShObjIdl.h>

// ShellExecuteW（ダウンロードページを開く）に必要です。
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
    namespace Hub = LamaPon::Hub;

    constexpr wchar_t WindowClassName[] = L"LamaPonHubWindow";

    // バックグラウンドの更新チェック完了通知。
    // lParamにUpdateCheckResult*（受け取り側が解放）を渡します。
    constexpr UINT WM_HUB_UPDATE_AVAILABLE = WM_APP + 1;

    enum ControlId
    {
        IdProjectName = 100,
        IdProjectLocation,
        IdBrowseLocation,
        IdProjectTemplate,
        IdCreateProject,
        IdRecentProjects,
        IdOpenProject,
        IdLaunchProject,
        IdLaunchProjectSafe,
        IdRemoveProject,
        IdStatus,
        IdUpdateBanner,
        IdUpdateDownload,
        IdUpdateSkip
    };

    std::wstring WindowText(const HWND window)
    {
        const int length = GetWindowTextLengthW(window);
        std::wstring value(
            static_cast<std::size_t>(std::max(length, 0)) + 1,
            L'\0');
        if (length > 0)
        {
            GetWindowTextW(window, value.data(), length + 1);
        }
        value.resize(static_cast<std::size_t>(std::max(length, 0)));
        return value;
    }

    std::wstring Trim(std::wstring value)
    {
        const auto isSpace = [](const wchar_t character)
        {
            return iswspace(character) != 0;
        };
        value.erase(
            value.begin(),
            std::find_if_not(
                value.begin(),
                value.end(),
                isSpace));
        value.erase(
            std::find_if_not(
                value.rbegin(),
                value.rend(),
                isSpace).base(),
            value.end());
        return value;
    }

    void ShowError(const HWND owner, const std::exception& exception)
    {
        auto message = LamaPon::Utf8ToWide(exception.what());
        if (message.empty())
        {
            message = L"処理中にエラーが発生しました。";
        }
        MessageBoxW(
            owner,
            message.c_str(),
            L"LamaPon Hub - エラー",
            MB_OK | MB_ICONERROR);
    }

    std::filesystem::path PickFolder(
        const HWND owner,
        const std::filesystem::path& initialFolder,
        const wchar_t* title)
    {
        IFileDialog* dialog{};
        const HRESULT createResult = CoCreateInstance(
            CLSID_FileOpenDialog,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog));
        if (FAILED(createResult))
        {
            throw std::runtime_error(
                "Could not open the Windows folder picker.");
        }

        DWORD options{};
        dialog->GetOptions(&options);
        dialog->SetOptions(
            options
            | FOS_PICKFOLDERS
            | FOS_FORCEFILESYSTEM
            | FOS_PATHMUSTEXIST);
        dialog->SetTitle(title);

        IShellItem* initialItem{};
        if (!initialFolder.empty()
            && SUCCEEDED(SHCreateItemFromParsingName(
                initialFolder.c_str(),
                nullptr,
                IID_PPV_ARGS(&initialItem))))
        {
            dialog->SetFolder(initialItem);
            initialItem->Release();
        }

        const HRESULT showResult = dialog->Show(owner);
        if (showResult == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        {
            dialog->Release();
            return {};
        }
        if (FAILED(showResult))
        {
            dialog->Release();
            throw std::runtime_error(
                "The Windows folder picker failed.");
        }

        IShellItem* selectedItem{};
        if (FAILED(dialog->GetResult(&selectedItem)))
        {
            dialog->Release();
            throw std::runtime_error(
                "The selected folder could not be read.");
        }
        PWSTR selectedPath{};
        const HRESULT pathResult = selectedItem->GetDisplayName(
            SIGDN_FILESYSPATH,
            &selectedPath);
        selectedItem->Release();
        dialog->Release();
        if (FAILED(pathResult) || selectedPath == nullptr)
        {
            if (selectedPath != nullptr)
            {
                CoTaskMemFree(selectedPath);
            }
            throw std::runtime_error(
                "The selected folder has no filesystem path.");
        }
        const std::filesystem::path result{ selectedPath };
        CoTaskMemFree(selectedPath);
        return result;
    }

    class HubWindow final
    {
    public:
        explicit HubWindow(const HINSTANCE instance)
            : m_instance(instance)
        {
        }

        ~HubWindow()
        {
            // 更新チェックスレッドを必ず終わらせてから
            // ウィンドウリソースを破棄します。
            if (m_updateThread.joinable())
            {
                m_updateThread.join();
            }
            if (m_titleFont != nullptr)
            {
                DeleteObject(m_titleFont);
            }
            if (m_font != nullptr)
            {
                DeleteObject(m_font);
            }
            if (m_backgroundBrush != nullptr)
            {
                DeleteObject(m_backgroundBrush);
            }
            if (m_panelBrush != nullptr)
            {
                DeleteObject(m_panelBrush);
            }
            if (m_bannerBrush != nullptr)
            {
                DeleteObject(m_bannerBrush);
            }
        }

        void Create(const int showCommand)
        {
            WNDCLASSEXW windowClass{};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.style = CS_HREDRAW | CS_VREDRAW;
            windowClass.lpfnWndProc = WindowProcedure;
            windowClass.hInstance = m_instance;
            windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            windowClass.hIcon = static_cast<HICON>(LoadImageW(
                m_instance,
                MAKEINTRESOURCEW(IDI_LAMAPON_ENGINE),
                IMAGE_ICON,
                GetSystemMetrics(SM_CXICON),
                GetSystemMetrics(SM_CYICON),
                LR_SHARED));
            windowClass.hIconSm = static_cast<HICON>(LoadImageW(
                m_instance,
                MAKEINTRESOURCEW(IDI_LAMAPON_ENGINE),
                IMAGE_ICON,
                GetSystemMetrics(SM_CXSMICON),
                GetSystemMetrics(SM_CYSMICON),
                LR_SHARED));
            windowClass.hbrBackground = nullptr;
            windowClass.lpszClassName = WindowClassName;
            if (RegisterClassExW(&windowClass) == 0)
            {
                throw std::runtime_error(
                    "Could not register the LamaPon Hub window.");
            }

            m_backgroundBrush = CreateSolidBrush(RGB(20, 23, 29));
            m_panelBrush = CreateSolidBrush(RGB(31, 36, 45));
            m_bannerBrush = CreateSolidBrush(RGB(36, 52, 78));
            m_font = CreateFontW(
                -18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE,
                L"Yu Gothic UI");
            m_titleFont = CreateFontW(
                -30, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE,
                L"Yu Gothic UI");

            m_window = CreateWindowExW(
                0,
                WindowClassName,
                L"LamaPon Hub",
                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                1040,
                820,
                nullptr,
                nullptr,
                m_instance,
                this);
            if (m_window == nullptr)
            {
                throw std::runtime_error(
                    "Could not create the LamaPon Hub window.");
            }
            ShowWindow(m_window, showCommand);
            UpdateWindow(m_window);
        }

        int Run() const
        {
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            return static_cast<int>(message.wParam);
        }

    private:
        static LRESULT CALLBACK WindowProcedure(
            const HWND window,
            const UINT message,
            const WPARAM wParam,
            const LPARAM lParam)
        {
            HubWindow* self = reinterpret_cast<HubWindow*>(
                GetWindowLongPtrW(window, GWLP_USERDATA));
            if (message == WM_NCCREATE)
            {
                const auto* create = reinterpret_cast<CREATESTRUCTW*>(
                    lParam);
                self = static_cast<HubWindow*>(create->lpCreateParams);
                self->m_window = window;
                SetWindowLongPtrW(
                    window,
                    GWLP_USERDATA,
                    reinterpret_cast<LONG_PTR>(self));
            }
            return self != nullptr
                ? self->HandleMessage(message, wParam, lParam)
                : DefWindowProcW(window, message, wParam, lParam);
        }

        HWND AddControl(
            const wchar_t* className,
            const wchar_t* text,
            const DWORD style,
            const int id)
        {
            const HWND control = CreateWindowExW(
                std::wstring_view(className) == WC_EDITW
                    || std::wstring_view(className) == WC_LISTBOXW
                    ? WS_EX_CLIENTEDGE
                    : 0,
                className,
                text,
                WS_CHILD | WS_VISIBLE | style,
                0, 0, 100, 24,
                m_window,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(id)),
                m_instance,
                nullptr);
            if (control == nullptr)
            {
                throw std::runtime_error(
                    "Could not create a LamaPon Hub control.");
            }
            SendMessageW(
                control,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(m_font),
                TRUE);
            return control;
        }

        void CreateControls()
        {
            m_title = AddControl(
                WC_STATICW,
                L"LamaPon Hub",
                SS_LEFT,
                0);
            SendMessageW(
                m_title,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(m_titleFont),
                TRUE);
            m_newProjectLabel = AddControl(
                WC_STATICW,
                L"新規プロジェクト",
                SS_LEFT,
                0);
            m_nameLabel = AddControl(
                WC_STATICW,
                L"プロジェクト名",
                SS_LEFT,
                0);
            m_projectName = AddControl(
                WC_EDITW,
                L"新しいゲーム",
                ES_AUTOHSCROLL | WS_TABSTOP,
                IdProjectName);
            m_locationLabel = AddControl(
                WC_STATICW,
                L"保存場所",
                SS_LEFT,
                0);
            // 前回プロジェクトを作成した保存場所を復元します
            // （未記録・削除済みの場合は既定の場所）。
            auto initialLocation =
                Hub::LoadLastProjectLocation();
            if (initialLocation.empty()
                || !std::filesystem::is_directory(
                    initialLocation))
            {
                initialLocation =
                    Hub::DefaultProjectsDirectory();
            }
            m_projectLocation = AddControl(
                WC_EDITW,
                initialLocation.c_str(),
                ES_AUTOHSCROLL | WS_TABSTOP,
                IdProjectLocation);
            m_browseLocation = AddControl(
                WC_BUTTONW,
                L"参照...",
                BS_PUSHBUTTON | WS_TABSTOP,
                IdBrowseLocation);
            m_templateLabel = AddControl(
                WC_STATICW,
                L"テンプレート",
                SS_LEFT,
                0);
            m_projectTemplate = AddControl(
                WC_COMBOBOXW,
                L"",
                CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                IdProjectTemplate);
            SendMessageW(
                m_projectTemplate,
                CB_ADDSTRING,
                0,
                reinterpret_cast<LPARAM>(L"3D"));
            SendMessageW(
                m_projectTemplate,
                CB_ADDSTRING,
                0,
                reinterpret_cast<LPARAM>(L"2D"));
            SendMessageW(
                m_projectTemplate,
                CB_ADDSTRING,
                0,
                reinterpret_cast<LPARAM>(L"3D学習"));
            SendMessageW(
                m_projectTemplate,
                CB_ADDSTRING,
                0,
                reinterpret_cast<LPARAM>(L"2D学習"));
            SendMessageW(m_projectTemplate, CB_SETCURSEL, 0, 0);
            m_createProject = AddControl(
                WC_BUTTONW,
                L"作成して開く",
                BS_DEFPUSHBUTTON | WS_TABSTOP,
                IdCreateProject);

            m_recentLabel = AddControl(
                WC_STATICW,
                L"最近使ったプロジェクト",
                SS_LEFT,
                0);
            m_recentProjects = AddControl(
                WC_LISTBOXW,
                L"",
                LBS_NOTIFY | LBS_NOINTEGRALHEIGHT
                    | WS_VSCROLL | WS_TABSTOP,
                IdRecentProjects);
            m_openProject = AddControl(
                WC_BUTTONW,
                L"既存プロジェクトを追加...",
                BS_PUSHBUTTON | WS_TABSTOP,
                IdOpenProject);
            m_launchProject = AddControl(
                WC_BUTTONW,
                L"選択したプロジェクトを開く",
                BS_PUSHBUTTON | WS_TABSTOP,
                IdLaunchProject);
            // C++スクリプトが原因でエディターが開けないときの復旧用。
            m_launchProjectSafe = AddControl(
                WC_BUTTONW,
                L"セーフモードで開く",
                BS_PUSHBUTTON | WS_TABSTOP,
                IdLaunchProjectSafe);
            m_removeProject = AddControl(
                WC_BUTTONW,
                L"一覧から除外",
                BS_PUSHBUTTON | WS_TABSTOP,
                IdRemoveProject);
            m_status = AddControl(
                WC_STATICW,
                L"プロジェクトを選択するか、新しく作成してください。",
                SS_LEFT | SS_ENDELLIPSIS,
                IdStatus);

            // 右下にエンジンのバージョンを表示します。
            const auto versionText =
                L"v" + LamaPon::Utf8ToWide(
                    LamaPon::VersionString);
            m_versionLabel = AddControl(
                WC_STATICW,
                versionText.c_str(),
                SS_RIGHT,
                0);

            // エンジン更新の通知バナー（結果が届くまで非表示）。
            m_updateBanner = AddControl(
                WC_STATICW,
                L"",
                SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS,
                IdUpdateBanner);
            m_updateDownload = AddControl(
                WC_BUTTONW,
                L"ダウンロードページを開く",
                BS_PUSHBUTTON | WS_TABSTOP,
                IdUpdateDownload);
            m_updateSkip = AddControl(
                WC_BUTTONW,
                L"このバージョンをスキップ",
                BS_PUSHBUTTON | WS_TABSTOP,
                IdUpdateSkip);
            ShowWindow(m_updateBanner, SW_HIDE);
            ShowWindow(m_updateDownload, SW_HIDE);
            ShowWindow(m_updateSkip, SW_HIDE);

            std::filesystem::create_directories(
                Hub::DefaultProjectsDirectory());
            RefreshRecentProjects();
            LayoutControls();
        }

        // GitHub Releasesの最新版をバックグラウンドで確認します。
        // オフラインやリポジトリ非公開の間は何も起こりません。
        void StartUpdateCheck()
        {
            m_updateThread = std::thread(
                [window = m_window]
                {
                    auto result = Hub::CheckForEngineUpdate();
                    if (!result.updateAvailable
                        || result.latestVersion
                            == Hub::LoadSkippedUpdateVersion())
                    {
                        return;
                    }
                    auto payload = std::make_unique<
                        Hub::UpdateCheckResult>(
                            std::move(result));
                    if (PostMessageW(
                            window,
                            WM_HUB_UPDATE_AVAILABLE,
                            0,
                            reinterpret_cast<LPARAM>(
                                payload.get())) != FALSE)
                    {
                        // 受け取り側（HandleMessage）が解放します。
                        static_cast<void>(payload.release());
                    }
                });
        }

        void ShowUpdateBanner()
        {
            const auto text =
                L"新しいバージョン v"
                + LamaPon::Utf8ToWide(m_update.latestVersion)
                + L" が公開されています（現在: v"
                + LamaPon::Utf8ToWide(
                    std::string(LamaPon::VersionString))
                + L"）";
            SetWindowTextW(m_updateBanner, text.c_str());
            m_updateBannerVisible = true;
            ShowWindow(m_updateBanner, SW_SHOW);
            ShowWindow(m_updateDownload, SW_SHOW);
            ShowWindow(m_updateSkip, SW_SHOW);
            LayoutControls();
            InvalidateRect(m_window, nullptr, TRUE);
        }

        void HideUpdateBanner()
        {
            m_updateBannerVisible = false;
            ShowWindow(m_updateBanner, SW_HIDE);
            ShowWindow(m_updateDownload, SW_HIDE);
            ShowWindow(m_updateSkip, SW_HIDE);
            LayoutControls();
            InvalidateRect(m_window, nullptr, TRUE);
        }

        void LayoutControls() const
        {
            RECT area{};
            GetClientRect(m_window, &area);
            const int width = area.right - area.left;
            const int height = area.bottom - area.top;
            constexpr int margin = 28;
            constexpr int labelWidth = 142;
            constexpr int rowHeight = 31;
            const int contentWidth = std::max(width - margin * 2, 400);

            MoveWindow(m_title, margin, 14, contentWidth, 40, TRUE);

            // 更新バナーはタイトル直下へ挟み、以降の行を押し下げます。
            if (m_updateBannerVisible)
            {
                const int bannerY = 82;
                MoveWindow(
                    m_updateSkip,
                    width - margin - 190,
                    bannerY + 3,
                    186,
                    30,
                    TRUE);
                MoveWindow(
                    m_updateDownload,
                    width - margin - 190 - 204,
                    bannerY + 3,
                    196,
                    30,
                    TRUE);
                MoveWindow(
                    m_updateBanner,
                    margin,
                    bannerY,
                    std::max(
                        width - margin * 2 - 190 - 212,
                        150),
                    36,
                    TRUE);
            }

            const int contentTop = m_updateBannerVisible ? 134 : 53;
            MoveWindow(
                m_newProjectLabel,
                margin,
                contentTop,
                contentWidth,
                28,
                TRUE);

            int y = contentTop + 34;
            MoveWindow(m_nameLabel, margin, y + 4, labelWidth, rowHeight, TRUE);
            MoveWindow(
                m_projectName,
                margin + labelWidth,
                y,
                contentWidth - labelWidth,
                rowHeight,
                TRUE);
            y += 42;
            MoveWindow(m_locationLabel, margin, y + 4, labelWidth, rowHeight, TRUE);
            MoveWindow(
                m_projectLocation,
                margin + labelWidth,
                y,
                contentWidth - labelWidth - 105,
                rowHeight,
                TRUE);
            MoveWindow(
                m_browseLocation,
                width - margin - 95,
                y,
                95,
                rowHeight,
                TRUE);
            y += 42;
            MoveWindow(m_templateLabel, margin, y + 4, labelWidth, rowHeight, TRUE);
            MoveWindow(
                m_projectTemplate,
                margin + labelWidth,
                y,
                220,
                180,
                TRUE);
            MoveWindow(
                m_createProject,
                width - margin - 170,
                y,
                170,
                rowHeight,
                TRUE);

            const int recentTop = contentTop + 172;
            MoveWindow(
                m_recentLabel,
                margin,
                recentTop,
                contentWidth,
                28,
                TRUE);
            const int buttonTop = height - margin - 67;
            MoveWindow(
                m_recentProjects,
                margin,
                recentTop + 34,
                contentWidth,
                std::max(buttonTop - recentTop - 44, 100),
                TRUE);
            MoveWindow(
                m_openProject,
                margin,
                buttonTop,
                225,
                rowHeight,
                TRUE);
            MoveWindow(
                m_removeProject,
                margin + 235,
                buttonTop,
                135,
                rowHeight,
                TRUE);
            MoveWindow(
                m_launchProjectSafe,
                width - margin - 245 - 175,
                buttonTop,
                165,
                rowHeight,
                TRUE);
            MoveWindow(
                m_launchProject,
                width - margin - 245,
                buttonTop,
                245,
                rowHeight,
                TRUE);
            // ステータスは左、バージョンは右下の隅に置きます。
            MoveWindow(
                m_status,
                margin,
                height - margin - 24,
                std::max(contentWidth - 160, 100),
                24,
                TRUE);
            MoveWindow(
                m_versionLabel,
                width - margin - 150,
                height - margin - 24,
                150,
                24,
                TRUE);
        }

        void RefreshRecentProjects()
        {
            m_projects = Hub::LoadRecentProjects();
            SendMessageW(m_recentProjects, LB_RESETCONTENT, 0, 0);
            for (const auto& project : m_projects)
            {
                const auto name = LamaPon::Utf8ToWide(project.name);
                const auto label = name + L"   —   "
                    + project.path.native();
                SendMessageW(
                    m_recentProjects,
                    LB_ADDSTRING,
                    0,
                    reinterpret_cast<LPARAM>(label.c_str()));
            }
            if (!m_projects.empty())
            {
                SendMessageW(m_recentProjects, LB_SETCURSEL, 0, 0);
            }
        }

        [[nodiscard]] int SelectedProjectIndex() const noexcept
        {
            const auto selection = static_cast<int>(SendMessageW(
                m_recentProjects,
                LB_GETCURSEL,
                0,
                0));
            return selection != LB_ERR
                    && selection >= 0
                    && static_cast<std::size_t>(selection)
                        < m_projects.size()
                ? selection
                : -1;
        }

        std::filesystem::path SelectedProject() const
        {
            const int selection = SelectedProjectIndex();
            if (selection < 0)
            {
                throw std::runtime_error(
                    "プロジェクトが選択されていません。");
            }
            return m_projects[static_cast<std::size_t>(selection)].path;
        }

        void SetStatus(std::wstring value) const
        {
            SetWindowTextW(m_status, value.c_str());
        }

        void BrowseProjectLocation()
        {
            auto current = std::filesystem::path(
                WindowText(m_projectLocation));
            if (!std::filesystem::is_directory(current))
            {
                current = Hub::DefaultProjectsDirectory();
            }
            const auto selected = PickFolder(
                m_window,
                current,
                L"LamaPonプロジェクトの保存場所を選択");
            if (!selected.empty())
            {
                SetWindowTextW(
                    m_projectLocation,
                    selected.c_str());
            }
        }

        void CreateNewProject()
        {
            const auto name = Trim(WindowText(m_projectName));
            const auto location = Trim(WindowText(m_projectLocation));
            if (name.empty() || location.empty())
            {
                throw std::invalid_argument(
                    "プロジェクト名と保存場所を入力してください。");
            }
            constexpr wchar_t InvalidCharacters[] = L"<>:\"/\\|?*";
            if (name.find_first_of(InvalidCharacters)
                    != std::wstring::npos
                || name == L"."
                || name == L".."
                || name.back() == L'.'
                || name.back() == L' ')
            {
                throw std::invalid_argument(
                    "プロジェクト名に使用できない文字が含まれています。");
            }

            const int templateIndex = static_cast<int>(SendMessageW(
                m_projectTemplate,
                CB_GETCURSEL,
                0,
                0));
            Hub::ProjectTemplate projectTemplate =
                Hub::ProjectTemplate::ThreeDimensional;
            if (templateIndex == 1)
            {
                projectTemplate =
                    Hub::ProjectTemplate::TwoDimensional;
            }
            else if (templateIndex == 2)
            {
                projectTemplate =
                    Hub::ProjectTemplate::LearningThreeDimensional;
            }
            else if (templateIndex == 3)
            {
                projectTemplate =
                    Hub::ProjectTemplate::LearningTwoDimensional;
            }

            const auto root =
                std::filesystem::path(location) / name;
            Hub::CreateProject(
                root,
                LamaPon::WideToUtf8(name),
                projectTemplate);
            // 次回のHub起動でも同じ保存場所から始められるように
            // 記憶します。
            Hub::SaveLastProjectLocation(
                std::filesystem::path(location));
            Hub::AddRecentProject(root);
            RefreshRecentProjects();
            SetStatus(L"プロジェクトを作成しました: " + root.native());
            LaunchProject(root);
        }

        void AddExistingProject()
        {
            const auto selected = PickFolder(
                m_window,
                Hub::DefaultProjectsDirectory(),
                L"LamaPonプロジェクトを選択");
            if (selected.empty())
            {
                return;
            }
            if (!Hub::IsProject(selected))
            {
                throw std::runtime_error(
                    "選択したフォルダーはLamaPonプロジェクトではありません。"
                    " .lamapon/project.json を確認してください。");
            }
            Hub::AddRecentProject(selected);
            RefreshRecentProjects();
            SetStatus(L"プロジェクトを追加しました: " + selected.native());
        }

        void LaunchProject(
            const std::filesystem::path& projectRoot,
            const bool safeMode = false)
        {
            if (LamaPon::IsProjectEditorOpen(projectRoot))
            {
                SetStatus(
                    L"このプロジェクトは、すでにLamaPon Editorで開かれています: "
                    + projectRoot.native());
                MessageBoxW(
                    m_window,
                    L"このプロジェクトは、すでにLamaPon Editorで開かれています。\n"
                    L"同じプロジェクトを同時に複数開くことはできません。",
                    L"LamaPon Hub",
                    MB_OK | MB_ICONINFORMATION);
                return;
            }

            const auto editorPath =
                LamaPon::ExecutableDirectory() / L"LamaPonEditor.exe";
            if (!std::filesystem::is_regular_file(editorPath))
            {
                throw std::runtime_error(
                    "LamaPonEditor.exeがHubと同じフォルダーにありません。");
            }
            Hub::AddRecentProject(projectRoot);
            RefreshRecentProjects();

            std::wstring commandLine = L"\""
                + editorPath.native()
                + L"\" --project \""
                + projectRoot.native()
                + L"\"";
            if (safeMode)
            {
                // C++スクリプトを読み込まずに開きます
                // （前回クラッシュしたときの復旧用）。
                commandLine += L" --safe";
            }
            STARTUPINFOW startupInfo{};
            startupInfo.cb = sizeof(startupInfo);
            PROCESS_INFORMATION processInfo{};
            if (!CreateProcessW(
                editorPath.c_str(),
                commandLine.data(),
                nullptr,
                nullptr,
                FALSE,
                0,
                nullptr,
                projectRoot.c_str(),
                &startupInfo,
                &processInfo))
            {
                throw std::runtime_error(
                    "LamaPon Editorを起動できませんでした。");
            }
            CloseHandle(processInfo.hThread);
            CloseHandle(processInfo.hProcess);
            SetStatus(L"LamaPon Editorを起動しました: "
                + projectRoot.native());
        }

        void RemoveSelectedProject()
        {
            const auto selected = SelectedProject();
            Hub::RemoveRecentProject(selected);
            RefreshRecentProjects();
            SetStatus(L"一覧から除外しました（ファイルは削除されません）。");
        }

        void HandleCommand(const WPARAM wParam)
        {
            const int id = LOWORD(wParam);
            const int notification = HIWORD(wParam);
            try
            {
                if (id == IdBrowseLocation && notification == BN_CLICKED)
                {
                    BrowseProjectLocation();
                }
                else if (id == IdCreateProject
                    && notification == BN_CLICKED)
                {
                    CreateNewProject();
                }
                else if (id == IdOpenProject
                    && notification == BN_CLICKED)
                {
                    AddExistingProject();
                }
                else if (id == IdLaunchProject
                    && notification == BN_CLICKED)
                {
                    LaunchProject(SelectedProject());
                }
                else if (id == IdLaunchProjectSafe
                    && notification == BN_CLICKED)
                {
                    LaunchProject(SelectedProject(), true);
                }
                else if (id == IdRemoveProject
                    && notification == BN_CLICKED)
                {
                    RemoveSelectedProject();
                }
                else if (id == IdRecentProjects
                    && notification == LBN_DBLCLK)
                {
                    LaunchProject(SelectedProject());
                }
                else if (id == IdUpdateDownload
                    && notification == BN_CLICKED)
                {
                    const auto url = LamaPon::Utf8ToWide(
                        m_update.releaseUrl);
                    ShellExecuteW(
                        m_window,
                        L"open",
                        url.c_str(),
                        nullptr,
                        nullptr,
                        SW_SHOWNORMAL);
                }
                else if (id == IdUpdateSkip
                    && notification == BN_CLICKED)
                {
                    Hub::SaveSkippedUpdateVersion(
                        m_update.latestVersion);
                    HideUpdateBanner();
                    SetStatus(
                        L"バージョン v"
                        + LamaPon::Utf8ToWide(
                            m_update.latestVersion)
                        + L" の通知をスキップしました。");
                }
            }
            catch (const std::exception& exception)
            {
                ShowError(m_window, exception);
            }
        }

        LRESULT HandleMessage(
            const UINT message,
            const WPARAM wParam,
            const LPARAM lParam)
        {
            switch (message)
            {
            case WM_CREATE:
                try
                {
                    CreateControls();
                    StartUpdateCheck();
                }
                catch (const std::exception& exception)
                {
                    ShowError(m_window, exception);
                    return -1;
                }
                return 0;
            case WM_HUB_UPDATE_AVAILABLE:
            {
                const std::unique_ptr<Hub::UpdateCheckResult>
                    payload(
                        reinterpret_cast<
                            Hub::UpdateCheckResult*>(lParam));
                if (payload != nullptr)
                {
                    m_update = *payload;
                    ShowUpdateBanner();
                }
                return 0;
            }
            case WM_SIZE:
                LayoutControls();
                return 0;
            case WM_GETMINMAXINFO:
            {
                auto* information = reinterpret_cast<MINMAXINFO*>(lParam);
                information->ptMinTrackSize = { 860, 760 };
                return 0;
            }
            case WM_COMMAND:
                HandleCommand(wParam);
                return 0;
            case WM_CTLCOLORSTATIC:
            {
                const auto deviceContext = reinterpret_cast<HDC>(wParam);
                // 更新バナーはアクセント色で目立たせます。
                if (reinterpret_cast<HWND>(lParam)
                    == m_updateBanner)
                {
                    SetTextColor(
                        deviceContext,
                        RGB(168, 208, 255));
                    SetBkColor(deviceContext, RGB(36, 52, 78));
                    return reinterpret_cast<LRESULT>(
                        m_bannerBrush);
                }
                // バージョン表示は控えめな色にします。
                if (reinterpret_cast<HWND>(lParam)
                    == m_versionLabel)
                {
                    SetTextColor(
                        deviceContext,
                        RGB(120, 130, 148));
                    SetBkColor(deviceContext, RGB(20, 23, 29));
                    return reinterpret_cast<LRESULT>(
                        m_backgroundBrush);
                }
                SetTextColor(deviceContext, RGB(225, 232, 242));
                SetBkColor(deviceContext, RGB(20, 23, 29));
                return reinterpret_cast<LRESULT>(m_backgroundBrush);
            }
            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORLISTBOX:
            {
                const auto deviceContext = reinterpret_cast<HDC>(wParam);
                SetTextColor(deviceContext, RGB(235, 240, 248));
                SetBkColor(deviceContext, RGB(31, 36, 45));
                return reinterpret_cast<LRESULT>(m_panelBrush);
            }
            case WM_ERASEBKGND:
            {
                RECT area{};
                GetClientRect(m_window, &area);
                FillRect(
                    reinterpret_cast<HDC>(wParam),
                    &area,
                    m_backgroundBrush);
                return 1;
            }
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProcW(
                    m_window,
                    message,
                    wParam,
                    lParam);
            }
        }

        HINSTANCE m_instance{};
        HWND m_window{};
        HWND m_title{};
        HWND m_newProjectLabel{};
        HWND m_nameLabel{};
        HWND m_projectName{};
        HWND m_locationLabel{};
        HWND m_projectLocation{};
        HWND m_browseLocation{};
        HWND m_templateLabel{};
        HWND m_projectTemplate{};
        HWND m_createProject{};
        HWND m_recentLabel{};
        HWND m_recentProjects{};
        HWND m_openProject{};
        HWND m_launchProject{};
        HWND m_launchProjectSafe{};
        HWND m_removeProject{};
        HWND m_status{};
        HWND m_versionLabel{};
        HWND m_updateBanner{};
        HWND m_updateDownload{};
        HWND m_updateSkip{};
        HFONT m_font{};
        HFONT m_titleFont{};
        HBRUSH m_backgroundBrush{};
        HBRUSH m_panelBrush{};
        HBRUSH m_bannerBrush{};
        bool m_updateBannerVisible{};
        Hub::UpdateCheckResult m_update;
        std::thread m_updateThread;
        std::vector<Hub::RecentProject> m_projects;
    };
}
int WINAPI wWinMain(
    const HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    const int showCommand)
{
    // Hubの二重起動を防ぎます。すでに起動している場合は
    // 新しく開かず、既存のウィンドウを前面に出します。
    const HANDLE instanceMutex = CreateMutexW(
        nullptr,
        TRUE,
        L"LamaPonHubSingleInstance");
    if (instanceMutex != nullptr
        && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        const HWND existing = FindWindowW(
            WindowClassName,
            nullptr);
        if (existing != nullptr)
        {
            if (IsIconic(existing))
            {
                ShowWindow(existing, SW_RESTORE);
            }
            SetForegroundWindow(existing);
        }
        CloseHandle(instanceMutex);
        return 0;
    }

    LamaPon::CrashReporter::Install(
        LamaPon::ExecutableDirectory() / L"Crashes",
        "LamaPonHub");
    const HRESULT comResult = CoInitializeEx(
        nullptr,
        COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    try
    {
        HubWindow window(instance);
        window.Create(showCommand);
        const int result = window.Run();
        if (SUCCEEDED(comResult))
        {
            CoUninitialize();
        }
        return result;
    }
    catch (const std::exception& exception)
    {
        static_cast<void>(
            LamaPon::CrashReporter::WriteDiagnostic(
                exception.what()));
        ShowError(nullptr, exception);
        if (SUCCEEDED(comResult))
        {
            CoUninitialize();
        }
        return 1;
    }
}
