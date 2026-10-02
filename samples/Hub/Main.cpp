#include "LamaPon/Core/PathUtils.h"
#include "LamaPon/Core/ProjectInstance.h"
#include "LamaPon/Core/CrashReporter.h"
#include "LamaPon/Core/BuildInfo.h"
#include "LamaPon/Core/Version.h"
#include "LamaPon/Resources/WindowsResource.h"
#include "LamaPon/Hub/ProjectHub.h"
#include "LamaPon/Hub/UpdateChecker.h"

#include <Windows.h>
#include <ShObjIdl.h>

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

    // Hub WindowのClass名
    constexpr wchar_t WindowClassName[] = L"LamaPonHubWindow";

    // 更新結果をlParamで所有権移譲
    constexpr UINT WM_HUB_UPDATE_AVAILABLE = WM_APP + 1;

    // Control識別子
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

    // Win32 Controlの文字列を返します(window: 読み取るControl)。
    std::wstring WindowText(const HWND window)
    {
        // 現在の文字列長
        const int length = GetWindowTextLengthW(window);
        // 終端文字も含めた取得用Buffer
        std::wstring value(
            static_cast<std::size_t>(std::max(length, 0)) + 1,
            L'\0');
        // 文字列がある場合だけControlから読み込みます。
        if (length > 0)
        {
            GetWindowTextW(window, value.data(), length + 1);
        }
        value.resize(static_cast<std::size_t>(std::max(length, 0)));
        return value;
    }

    // 文字列の両端の空白を除きます(value: 補正する文字列)。
    std::wstring Trim(std::wstring value)
    {
    // 空白文字を判定します(character: 判定対象)。
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

    // HubのエラーDialogを表示します(owner: 親Window, exception: 表示する例外)。
    void ShowError(const HWND owner, const std::exception& exception)
    {
        // 例外メッセージのWide文字列
        auto message = LamaPon::Utf8ToWide(exception.what());
        // 空の例外文には汎用メッセージを使います。
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

    // Folder選択Dialogを表示します(owner: 親Window, initialFolder: 初期位置, title: Dialog見出し)。
    std::filesystem::path PickFolder(
        const HWND owner,
        const std::filesystem::path& initialFolder,
        const wchar_t* title)
    {
        // Folder選択Dialog
        IFileDialog* dialog{};
        // Dialog生成結果
        const HRESULT createResult = CoCreateInstance(
            CLSID_FileOpenDialog,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog));
        // Dialogを生成できない場合は起動処理へ失敗を返します。
        if (FAILED(createResult))
        {
            throw std::runtime_error(
                "Could not open the Windows folder picker.");
        }

        // 現在のDialogオプション
        DWORD options{};
        dialog->GetOptions(&options);
        dialog->SetOptions(
            options
            | FOS_PICKFOLDERS
            | FOS_FORCEFILESYSTEM
            | FOS_PATHMUSTEXIST);
        dialog->SetTitle(title);

        // 初期表示先のShell item
        IShellItem* initialItem{};
        // 初期Folderが有効ならDialogの選択開始位置にします。
        if (!initialFolder.empty()
            && SUCCEEDED(SHCreateItemFromParsingName(
                initialFolder.c_str(),
                nullptr,
                IID_PPV_ARGS(&initialItem))))
        {
            dialog->SetFolder(initialItem);
            initialItem->Release();
        }

        // Folder選択Dialogの結果
        const HRESULT showResult = dialog->Show(owner);
        // Cancelは空Pathとして返します。
        if (showResult == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        {
            dialog->Release();
            return {};
        }
        // Dialogの失敗をFolder選択エラーとして返します。
        if (FAILED(showResult))
        {
            dialog->Release();
            throw std::runtime_error(
                "The Windows folder picker failed.");
        }

        // 選択FolderのShell item
        IShellItem* selectedItem{};
        // 選択結果を取得できない場合は失敗を返します。
        if (FAILED(dialog->GetResult(&selectedItem)))
        {
            dialog->Release();
            throw std::runtime_error(
                "The selected folder could not be read.");
        }
        // Shell確保のFolder path
        PWSTR selectedPath{};
        // Folder path取得HRESULT
        const HRESULT pathResult = selectedItem->GetDisplayName(
            SIGDN_FILESYSPATH,
            &selectedPath);
        selectedItem->Release();
        dialog->Release();
        // Path取得失敗時の一時文字列を解放します。
        if (FAILED(pathResult) || selectedPath == nullptr)
        {
            // 失敗時も返却Bufferがあれば解放します。
            if (selectedPath != nullptr)
            {
                CoTaskMemFree(selectedPath);
            }
            throw std::runtime_error(
                "The selected folder has no filesystem path.");
        }
        // 呼び出し元へ返す選択Folder
        const std::filesystem::path result{ selectedPath };
        CoTaskMemFree(selectedPath);
        return result;
    }

    class HubWindow final
    {
    public:
        // HubWindowを初期化します(instance: Win32 instance)。
        explicit HubWindow(const HINSTANCE instance)
            : m_instance(instance)
        {
        }

        // HubWindowのThreadと描画Resourceを解放します。
        ~HubWindow()
        {
            // UI破棄前に更新Check Threadを待ちます。
            if (m_updateThread.joinable())
            {
                m_updateThread.join();
            }
            // Title Fontを解放します。
            if (m_titleFont != nullptr)
            {
                DeleteObject(m_titleFont);
            }
            // 標準Fontを解放します。
            if (m_font != nullptr)
            {
                DeleteObject(m_font);
            }
            // 背景Brushを解放します。
            if (m_backgroundBrush != nullptr)
            {
                DeleteObject(m_backgroundBrush);
            }
            // Panel Brushを解放します。
            if (m_panelBrush != nullptr)
            {
                DeleteObject(m_panelBrush);
            }
            // Update Banner Brushを解放します。
            if (m_bannerBrush != nullptr)
            {
                DeleteObject(m_bannerBrush);
            }
        }

        // HubのMain Windowを作成します(showCommand: Win32表示方法)。
        void Create(const int showCommand)
        {
            // 登録するHub Window class
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
            // Window classを登録できなければ初期化を中断します。
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
            // Hub Windowを作成できなければ初期化を中断します。
            if (m_window == nullptr)
            {
                throw std::runtime_error(
                    "Could not create the LamaPon Hub window.");
            }
            ShowWindow(m_window, showCommand);
            UpdateWindow(m_window);
        }

        // Hub WindowのWin32 message loopを実行します。
        int Run() const
        {
            // message loopの状態
            MSG message{};
            // 終了messageまでWindow messageをdispatchします。
            while (GetMessageW(&message, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            return static_cast<int>(message.wParam);
        }

    private:
        // messageを振り分けます(window: 対象, message: 種別, wParam: 補助値, lParam: 補助値)。
        static LRESULT CALLBACK WindowProcedure(
            const HWND window,
            const UINT message,
            const WPARAM wParam,
            const LPARAM lParam)
        {
            // Windowに紐付けた状態
            HubWindow* self = reinterpret_cast<HubWindow*>(
                GetWindowLongPtrW(window, GWLP_USERDATA));
            // WM_NCCREATEでHubWindowをWindowへ紐付けます。
            if (message == WM_NCCREATE)
            {
                // Windowの生成情報
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

        // UI部品を追加します(className: 種別, text: 表示文字, style: 形式, id: 識別子)。
        HWND AddControl(
            const wchar_t* className,
            const wchar_t* text,
            const DWORD style,
            const int id)
        {
            // 作成したWin32 Control
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
            // Controlを作成できなければ初期化を中断します。
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

        // Hubの操作用Controlを作成します。
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
            // 保存先の前回値
            auto initialLocation =
                Hub::LoadLastProjectLocation();
            // 保存先が未指定か無効なら既定位置へ戻します。
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
            // C++ Script障害から復旧するSafe Mode
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

            // EngineのVersion表示text
            const auto versionText = LamaPon::Utf8ToWide(
                LamaPon::FormatBuildLabel());
            m_versionLabel = AddControl(
                WC_STATICW,
                versionText.c_str(),
                SS_RIGHT | SS_ENDELLIPSIS,
                0);

            // 更新通知を出す領域
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

        // GitHub Releasesを非同期確認し、新版ならUIへ通知します。
        void StartUpdateCheck()
        {
            m_updateThread = std::thread(
                [window = m_window]
                {
                    // GitHub Releasesの確認結果
                    auto result = Hub::CheckForEngineUpdate();
                    // 更新なしまたは通知済みVersionは表示しません。
                    if (!result.updateAvailable
                        || result.latestVersion
                            == Hub::LoadSkippedUpdateVersion())
                    {
                        return;
                    }
                    // UI Threadへ所有権を渡す更新結果
                    auto payload = std::make_unique<
                        Hub::UpdateCheckResult>(
                            std::move(result));
                    // 通知成功時だけ受信側へ結果所有権を渡します。
                    if (PostMessageW(
                            window,
                            WM_HUB_UPDATE_AVAILABLE,
                            0,
                            reinterpret_cast<LPARAM>(
                                payload.get())) != FALSE)
                    {
                        static_cast<void>(payload.release());
                    }
                });
        }

        // 新版の情報を通知Bannerへ表示します。
        void ShowUpdateBanner()
        {
            // HubとEngineのVersion比較text
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

        // Update Bannerと操作Buttonを隠します。
        void HideUpdateBanner()
        {
            m_updateBannerVisible = false;
            ShowWindow(m_updateBanner, SW_HIDE);
            ShowWindow(m_updateDownload, SW_HIDE);
            ShowWindow(m_updateSkip, SW_HIDE);
            LayoutControls();
            InvalidateRect(m_window, nullptr, TRUE);
        }

        // Client領域の大きさに合わせてControlを配置します。
        void LayoutControls() const
        {
            // Window Client領域
            RECT area{};
            GetClientRect(m_window, &area);
            // Client幅
            const int width = area.right - area.left;
            // Client高
            const int height = area.bottom - area.top;
            // Controlの外周余白
            constexpr int margin = 28;
            // 左ラベル列の幅
            constexpr int labelWidth = 142;
            // 行Controlの高さ
            constexpr int rowHeight = 31;
            // 左右余白を除いたContent幅
            const int contentWidth = std::max(width - margin * 2, 400);

            MoveWindow(m_title, margin, 14, contentWidth, 40, TRUE);

            // 更新バナーはタイトル直下へ挟み、以降の行を押し下げます。
            if (m_updateBannerVisible)
            {
                // Update Bannerの上端
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

            // contentの開始Y座標
            const int contentTop = m_updateBannerVisible ? 134 : 53;
            MoveWindow(
                m_newProjectLabel,
                margin,
                contentTop,
                contentWidth,
                28,
                TRUE);

            // 新規Project設定行のY座標
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

            // Recent一覧の開始Y座標
            const int recentTop = contentTop + 172;
            MoveWindow(
                m_recentLabel,
                margin,
                recentTop,
                contentWidth,
                28,
                TRUE);
            // 下部操作ButtonのY座標
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
                std::max(contentWidth - 370, 100),
                24,
                TRUE);
            MoveWindow(
                m_versionLabel,
                width - margin - 360,
                height - margin - 24,
                360,
                24,
                TRUE);
        }

        // 保存済みRecent ProjectをListへ反映します。
        void RefreshRecentProjects()
        {
            m_projects = Hub::LoadRecentProjects();
            SendMessageW(m_recentProjects, LB_RESETCONTENT, 0, 0);
            // recent Projectを追加(project: 対象)。
            for (const auto& project : m_projects)
            {
                // List表示用Project名
                const auto name = LamaPon::Utf8ToWide(project.name);
                // Project名とPathを含む選択表示
                const auto label = name + L"   —   "
                    + project.path.native();
                SendMessageW(
                    m_recentProjects,
                    LB_ADDSTRING,
                    0,
                    reinterpret_cast<LPARAM>(label.c_str()));
            }
            // Projectがある場合は先頭を選択状態にします。
            if (!m_projects.empty())
            {
                SendMessageW(m_recentProjects, LB_SETCURSEL, 0, 0);
            }
        }

        // Listで選択中のProject indexを返します。
        [[nodiscard]] int SelectedProjectIndex() const noexcept
        {
            // ListBoxが返した選択index
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

        // 選択ProjectのPathを返します。
        std::filesystem::path SelectedProject() const
        {
            // 選択中Projectのindex
            const int selection = SelectedProjectIndex();
            // 選択が無ければ呼び出し側へエラーを返します。
            if (selection < 0)
            {
                throw std::runtime_error(
                    "プロジェクトが選択されていません。");
            }
            return m_projects[static_cast<std::size_t>(selection)].path;
        }

        // Status欄にvalueを表示します(value: 表示する文言)。
        void SetStatus(std::wstring value) const
        {
            SetWindowTextW(m_status, value.c_str());
        }

        // 新規Projectの保存先Folderを選びます。
        void BrowseProjectLocation()
        {
            // 現在入力されている保存先
            auto current = std::filesystem::path(
                WindowText(m_projectLocation));
            // 入力場所が無効なら標準Project Folderを選択開始位置にします。
            if (!std::filesystem::is_directory(current))
            {
                current = Hub::DefaultProjectsDirectory();
            }
            // 利用者が選んだ保存先Folder
            const auto selected = PickFolder(
                m_window,
                current,
                L"LamaPonプロジェクトの保存場所を選択");
            // Cancel以外ではProject保存先へ選択値を反映します。
            if (!selected.empty())
            {
                SetWindowTextW(
                    m_projectLocation,
                    selected.c_str());
            }
        }

        // 入力内容から新規Projectを作成してEditorを起動します。
        void CreateNewProject()
        {
            // 空白除去後のProject名
            const auto name = Trim(WindowText(m_projectName));
            // 空白除去後の保存先
            const auto location = Trim(WindowText(m_projectLocation));
            // 必須項目が空なら作成を拒否します。
            if (name.empty() || location.empty())
            {
                throw std::invalid_argument(
                    "プロジェクト名と保存場所を入力してください。");
            }
            // Folder名に使えない文字
            constexpr wchar_t InvalidCharacters[] = L"<>:\"/\\|?*";
            // WindowsのFolder名規則に合わない入力を拒否します。
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

            // 選択中Templateのindex
            const int templateIndex = static_cast<int>(SendMessageW(
                m_projectTemplate,
                CB_GETCURSEL,
                0,
                0));
            // 生成に使うTemplate
            Hub::ProjectTemplate projectTemplate =
                Hub::ProjectTemplate::ThreeDimensional;
            // 2D Templateを選択した場合
            if (templateIndex == 1)
            {
                projectTemplate =
                    Hub::ProjectTemplate::TwoDimensional;
            }
            // 3D学習Templateを選択した場合
            else if (templateIndex == 2)
            {
                projectTemplate =
                    Hub::ProjectTemplate::LearningThreeDimensional;
            }
            // 2D学習Templateを選択した場合
            else if (templateIndex == 3)
            {
                projectTemplate =
                    Hub::ProjectTemplate::LearningTwoDimensional;
            }

            // 新しく作成するProjectのRoot
            const auto root =
                std::filesystem::path(location) / name;
            Hub::CreateProject(
                root,
                LamaPon::WideToUtf8(name),
                projectTemplate);
            // 次回のHub起動用に保存場所を記録します。
            Hub::SaveLastProjectLocation(
                std::filesystem::path(location));
            Hub::AddRecentProject(root);
            RefreshRecentProjects();
            SetStatus(L"プロジェクトを作成しました: " + root.native());
            LaunchProject(root);
        }

        // Folderを選び、既存ProjectをRecent Listへ追加します。
        void AddExistingProject()
        {
            // 利用者が選んだ既存Project候補
            const auto selected = PickFolder(
                m_window,
                Hub::DefaultProjectsDirectory(),
                L"LamaPonプロジェクトを選択");
            // Dialog Cancel時は何も追加しません。
            if (selected.empty())
            {
                return;
            }
            // Project Settingsを持たないFolderは追加しません。
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

        // 指定ProjectのEditorを起動します(projectRoot: Project root, safeMode: Safe Modeで開くか)。
        void LaunchProject(
            const std::filesystem::path& projectRoot,
            const bool safeMode = false)
        {
            // すでにEditorで開いているProjectは再起動しません。
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

            // Editor executable path
            const auto editorPath =
                LamaPon::ExecutableDirectory() / L"LamaPonEditor.exe";
            // Editor executableが無ければ起動できません。
            if (!std::filesystem::is_regular_file(editorPath))
            {
                throw std::runtime_error(
                    "LamaPonEditor.exeがHubと同じフォルダーにありません。");
            }
            Hub::AddRecentProject(projectRoot);
            RefreshRecentProjects();

            // Editor起動command
            std::wstring commandLine = L"\""
                + editorPath.native()
                + L"\" --project \""
                + projectRoot.native()
                + L"\"";
            // Safe Mode起動だけに起動flagを追加します。
            if (safeMode)
            {
                commandLine += L" --safe";
            }
            // Win32 Process起動情報
            STARTUPINFOW startupInfo{};
            startupInfo.cb = sizeof(startupInfo);
            // 子Editor Process情報
            PROCESS_INFORMATION processInfo{};
            // EditorをProject rootをWorking Directoryにして起動します。
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

        // 選択ProjectをRecent Listから除外します。
        void RemoveSelectedProject()
        {
            // 除外するProjectのPath
            const auto selected = SelectedProject();
            Hub::RemoveRecentProject(selected);
            RefreshRecentProjects();
            SetStatus(L"一覧から除外しました（ファイルは削除されません）。");
        }

        // UIコマンドを処理します(wParam: コマンドと通知)。
        void HandleCommand(const WPARAM wParam)
        {
            // UI部品の識別番号
            const int id = LOWORD(wParam);
            // UI操作の通知番号
            const int notification = HIWORD(wParam);
            // UI操作の例外を捕捉します。
            try
            {
                // 保存先選択を開きます。
                if (id == IdBrowseLocation && notification == BN_CLICKED)
                {
                    BrowseProjectLocation();
                }
                // 新規Projectを作成します。
                else if (id == IdCreateProject
                    && notification == BN_CLICKED)
                {
                    CreateNewProject();
                }
                // 既存Projectを一覧へ追加します。
                else if (id == IdOpenProject
                    && notification == BN_CLICKED)
                {
                    AddExistingProject();
                }
                // 選択Projectを通常起動します。
                else if (id == IdLaunchProject
                    && notification == BN_CLICKED)
                {
                    LaunchProject(SelectedProject());
                }
                // 選択ProjectをSafe Modeで起動します。
                else if (id == IdLaunchProjectSafe
                    && notification == BN_CLICKED)
                {
                    LaunchProject(SelectedProject(), true);
                }
                // 選択Projectを一覧から除外します。
                else if (id == IdRemoveProject
                    && notification == BN_CLICKED)
                {
                    RemoveSelectedProject();
                }
                // 選択Projectをダブルクリックで起動します。
                else if (id == IdRecentProjects
                    && notification == LBN_DBLCLK)
                {
                    LaunchProject(SelectedProject());
                }
                // 更新ページを既定browserで開きます。
                else if (id == IdUpdateDownload
                    && notification == BN_CLICKED)
                {
                    // 更新配布ページのURL
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
                // 選択Versionの更新通知を非表示にします。
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
            // UI操作の失敗を利用者へ通知します。
            catch (const std::exception& exception)
            {
                ShowError(m_window, exception);
            }
        }

        // messageを処理します(message: 種別, wParam: 補助値, lParam: 補助値)。
        LRESULT HandleMessage(
            const UINT message,
            const WPARAM wParam,
            const LPARAM lParam)
        {
            // Hub windowのmessageを振り分けます。
            switch (message)
            {
            // UI部品と更新確認を初期化します。
            case WM_CREATE:
                // window生成時の初期化失敗を捕捉します。
                try
                {
                    CreateControls();
                    StartUpdateCheck();
                }
                // 初期化失敗を表示してwindow生成を中止します。
                catch (const std::exception& exception)
                {
                    ShowError(m_window, exception);
                    return -1;
                }
                return 0;
            // 非同期更新結果の所有権を受け取ります。
            case WM_HUB_UPDATE_AVAILABLE:
            {
                // 所有権を受信した更新結果
                const std::unique_ptr<Hub::UpdateCheckResult>
                    payload(
                        reinterpret_cast<
                            Hub::UpdateCheckResult*>(lParam));
                // 有効な更新結果だけを表示へ反映します。
                if (payload != nullptr)
                {
                    m_update = *payload;
                    ShowUpdateBanner();
                }
                return 0;
            }
            // windowサイズに合わせて配置します。
            case WM_SIZE:
                LayoutControls();
                return 0;
            // windowの最小サイズを指定します。
            case WM_GETMINMAXINFO:
            {
                // 最小サイズ設定data
                auto* information = reinterpret_cast<MINMAXINFO*>(lParam);
                information->ptMinTrackSize = { 860, 760 };
                return 0;
            }
            // UI操作をコマンド処理へ渡します。
            case WM_COMMAND:
                HandleCommand(wParam);
                return 0;
            // 静的text部品の色を設定します。
            case WM_CTLCOLORSTATIC:
            {
                // 色設定対象のdevice context
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
            // 入力欄と一覧の配色を設定します。
            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORLISTBOX:
            {
                // 色設定対象のdevice context
                const auto deviceContext = reinterpret_cast<HDC>(wParam);
                SetTextColor(deviceContext, RGB(235, 240, 248));
                SetBkColor(deviceContext, RGB(31, 36, 45));
                return reinterpret_cast<LRESULT>(m_panelBrush);
            }
            // Hubの背景を塗り直します。
            case WM_ERASEBKGND:
            {
                // 再描画するclient領域
                RECT area{};
                GetClientRect(m_window, &area);
                FillRect(
                    reinterpret_cast<HDC>(wParam),
                    &area,
                    m_backgroundBrush);
                return 1;
            }
            // message loopを終了させます。
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            // 未処理messageをWindowsへ委譲します。
            default:
                return DefWindowProcW(
                    m_window,
                    message,
                    wParam,
                    lParam);
            }
        }

        // process instance
        HINSTANCE m_instance{};
        // Hub本体のwindow handle
        HWND m_window{};
        // Hubタイトル表示部品
        HWND m_title{};
        // 新規Project欄の見出し
        HWND m_newProjectLabel{};
        // Project名欄の見出し
        HWND m_nameLabel{};
        // Project名の入力欄
        HWND m_projectName{};
        // 保存先欄の見出し
        HWND m_locationLabel{};
        // Project保存先の入力欄
        HWND m_projectLocation{};
        // 保存先選択button
        HWND m_browseLocation{};
        // Template欄の見出し
        HWND m_templateLabel{};
        // Project Templateの選択欄
        HWND m_projectTemplate{};
        // Project作成button
        HWND m_createProject{};
        // Recent Project欄の見出し
        HWND m_recentLabel{};
        // Recent Projectの一覧
        HWND m_recentProjects{};
        // 既存Project追加button
        HWND m_openProject{};
        // 通常起動button
        HWND m_launchProject{};
        // Safe Mode起動button
        HWND m_launchProjectSafe{};
        // 一覧除外button
        HWND m_removeProject{};
        // 操作結果のstatus表示
        HWND m_status{};
        // Version表示部品
        HWND m_versionLabel{};
        // 更新通知banner部品
        HWND m_updateBanner{};
        // 更新配布pageを開くbutton
        HWND m_updateDownload{};
        // 更新通知を隠すbutton
        HWND m_updateSkip{};
        // 共通部品のfont
        HFONT m_font{};
        // title部品のfont
        HFONT m_titleFont{};
        // Hub背景のbrush
        HBRUSH m_backgroundBrush{};
        // 入力部品の背景brush
        HBRUSH m_panelBrush{};
        // 更新通知の背景brush
        HBRUSH m_bannerBrush{};
        // 更新通知の表示状態
        bool m_updateBannerVisible{};
        // 最新の更新確認結果
        Hub::UpdateCheckResult m_update;
        // 終了時にjoinする更新確認thread
        std::thread m_updateThread;
        // 表示中のRecent Project
        std::vector<Hub::RecentProject> m_projects;
    };
}
// Hubを起動します(instance: process instance, showCommand: 表示方法)。
int WINAPI wWinMain(
    const HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    const int showCommand)
{
    // 多重起動時は既存Hubを前面に出します。
    const HANDLE instanceMutex = CreateMutexW(
        nullptr,
        TRUE,
        L"LamaPonHubSingleInstance");
    // 起動済みHubなら既存windowを前面に出します。
    if (instanceMutex != nullptr
        && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        // 既存Hubのwindow handle
        const HWND existing = FindWindowW(
            WindowClassName,
            nullptr);
        // 見つかったHubを前面へ出します。
        if (existing != nullptr)
        {
            // 最小化中のHubを復元します。
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
    // COM初期化結果
    const HRESULT comResult = CoInitializeEx(
        nullptr,
        COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    // Hub windowの生成とmessage loopを実行します。
    try
    {
        // Hub windowの状態とmessage loop
        HubWindow window(instance);
        window.Create(showCommand);
        // message loopの終了code
        const int result = window.Run();
        // COM初期化に成功したthreadを終了処理します。
        if (SUCCEEDED(comResult))
        {
            CoUninitialize();
        }
        return result;
    }
    // 起動時の例外を診断記録とdialogへ出します。
    catch (const std::exception& exception)
    {
        static_cast<void>(
            LamaPon::CrashReporter::WriteDiagnostic(
                exception.what()));
        ShowError(nullptr, exception);
        // COM初期化に成功したthreadを終了処理します。
        if (SUCCEEDED(comResult))
        {
            CoUninitialize();
        }
        return 1;
    }
}
