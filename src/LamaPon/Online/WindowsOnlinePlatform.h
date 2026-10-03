#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon::Detail
{
    enum class RefreshTokenLoadStatus : std::uint8_t
    {
        NotFound,
        Loaded,
        Unavailable,
        Corrupt
    };

    struct RefreshTokenLoadResult final
    {
        // tokenの読み込み結果
        RefreshTokenLoadStatus status{
            RefreshTokenLoadStatus::Unavailable
        };
        // 読み込んだ更新用token
        std::string refreshToken;
        // 秘密値を含まないエラー識別子
        std::string errorCode;
        // 秘密値を含まない固定診断
        std::string errorMessage;

        // 更新用tokenを読み込み済みかを返す。
        [[nodiscard]] bool Loaded() const noexcept
        {
            return status == RefreshTokenLoadStatus::Loaded;
        }
    };

    struct OnlinePlatformResult final
    {
        // 操作が成功したか
        bool succeeded{};
        // 秘密値を含まないエラー識別子
        std::string errorCode;
        // 秘密値を含まない診断
        std::string errorMessage;

        // プラットフォーム操作が成功したかを返す。
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return succeeded;
        }
    };

    class IRefreshTokenStore
    {
    public:
        // 資格情報storeを具象型の処理で解放する。
        virtual ~IRefreshTokenStore() = default;

        // 資格情報の同時利用を排他し、既定実装は常に成功とする。
        // 成功後はLoadの前から終了処理の完了まで保持し、同じstoreの再取得は成功とします。
        [[nodiscard]] virtual OnlinePlatformResult AcquireUsageLease()
        {
            return { true, {}, {} };
        }
        // 資格情報の利用ロックを解放し、既定実装は何もしない。
        virtual void ReleaseUsageLease() noexcept
        {
        }

        // 確定済みの更新用tokenと読み込み結果を返す。
        [[nodiscard]] virtual RefreshTokenLoadResult Load() = 0;
        // 更新用tokenを前の確定値を保ちながら永続化する(refreshToken: 次にLoad可能にするtoken)。
        // 成功時だけ新tokenをLoad可能にし、falseまたは例外時は前の確定値を保ち、書き込み残骸もLoad対象にしません。
        [[nodiscard]] virtual OnlinePlatformResult Save(
            std::string_view refreshToken) = 0;
        // 確定値と書き込み残骸を削除して結果を返す。
        [[nodiscard]] virtual OnlinePlatformResult Delete() = 0;
    };

    class IAuthorizationLauncher
    {
    public:
        // URL起動処理を具象型の処理で解放する。
        virtual ~IAuthorizationLauncher() = default;

        // 検証した認証URLをブラウザーで開く(authorizationUrl: 認証開始のURL, allowInsecureLoopback: ローカルHTTPを許可するか)。
        [[nodiscard]] virtual OnlinePlatformResult Launch(
            std::string_view authorizationUrl,
            bool allowInsecureLoopback) = 0;
    };

    // 本番はfactoryのLocalAppData保存先を使い、TEMPへフォールバックしません。
    class WindowsRefreshTokenStore final : public IRefreshTokenStore
    {
    public:
        // 保存先とゲーム・環境別の暗号化補助値を設定する(filePath: 資格情報ファイル, gameId: ゲームの名前空間ID, environmentId: 環境の名前空間ID)。
        WindowsRefreshTokenStore(
            std::filesystem::path filePath,
            std::string gameId,
            std::string environmentId);
        // 資格情報の利用ロックを解放する。
        ~WindowsRefreshTokenStore() override;

        // 資格情報storeの複製を禁止する。
        WindowsRefreshTokenStore(const WindowsRefreshTokenStore&) = delete;
        // 資格情報storeのコピー代入を禁止する。
        WindowsRefreshTokenStore& operator=(
            const WindowsRefreshTokenStore&) = delete;
        // 資格情報storeの移動を禁止する。
        WindowsRefreshTokenStore(WindowsRefreshTokenStore&&) = delete;
        // 資格情報storeの移動代入を禁止する。
        WindowsRefreshTokenStore& operator=(
            WindowsRefreshTokenStore&&) = delete;

        // 親保存先とACLを検証し同時利用を排他する。
        [[nodiscard]] OnlinePlatformResult AcquireUsageLease() override;
        // 保持中の資格情報利用ロックを解放する。
        void ReleaseUsageLease() noexcept override;
        // 属性と形式を検証し現Windowsユーザーのtokenを復号する。
        [[nodiscard]] RefreshTokenLoadResult Load() override;
        // 現Windowsユーザーで暗号化しflush後に確定値を置き換える(refreshToken: 永続化する更新用token)。
        [[nodiscard]] OnlinePlatformResult Save(
            std::string_view refreshToken) override;
        // 確定値と通常ファイルの書き込み残骸を両方削除する。
        [[nodiscard]] OnlinePlatformResult Delete() override;

        // storeの存続中有効な資格情報ファイルのパスを借用する。
        [[nodiscard]] const std::filesystem::path& FilePath() const noexcept
        {
            return m_filePath;
        }

    private:
        // 暗号化したtokenの保存先
        std::filesystem::path m_filePath;
        // ゲームと環境を結び付ける補助値
        std::vector<std::uint8_t> m_entropy;
        // token更新を排他する所有ハンドル
        void* m_usageLeaseHandle{};
        // 永続保存先を利用できるか
        bool m_storageAvailable{};
    };

    enum class WindowsRefreshTokenSaveTestFailPoint : std::uint8_t
    {
        None,
        Protection,
        TemporaryWrite,
        TemporaryAcl,
        Replace
    };

    // 保存の指定段階で一度だけ失敗させる(failPoint: commit前の失敗段階)。
    void SetWindowsRefreshTokenSaveTestFailPoint(
        WindowsRefreshTokenSaveTestFailPoint failPoint) noexcept;
    // 置換直前のテスト処理(context: テスト状態の借用)。
    using WindowsRefreshTokenSaveTestHook =
        void(*)(void* context) noexcept;
    // 置換直前のテスト処理を設定する(hook: テスト処理・nullで解除, context: テスト処理へ渡す借用状態)。
    void SetWindowsRefreshTokenSaveBeforeReplaceHook(
        WindowsRefreshTokenSaveTestHook hook,
        void* context) noexcept;

    // WindowsのURL起動境界を表す(ownerWindow: 所有window, operation: shellの動詞, file: URLまたはファイル, parameters: 起動引数, directory: 作業ディレクトリ, showCommand: windowの表示方法)。
    using ShellOpenFunction = std::function<std::intptr_t(
        void* ownerWindow,
        const wchar_t* operation,
        const wchar_t* file,
        const wchar_t* parameters,
        const wchar_t* directory,
        int showCommand)>;

    class WindowsAuthorizationLauncher final
        : public IAuthorizationLauncher
    {
    public:
        // windowと起動処理を設定する(ownerWindow: 呼出し側が所有するwindow, shellOpen: 起動のAPI境界・空なら標準)。
        explicit WindowsAuthorizationLauncher(
            void* ownerWindow = nullptr,
            ShellOpenFunction shellOpen = {});

        // 認証URLを検証しshellでブラウザーを開く(authorizationUrl: 認証開始のURL, allowInsecureLoopback: ローカルHTTPを許可するか)。
        [[nodiscard]] OnlinePlatformResult Launch(
            std::string_view authorizationUrl,
            bool allowInsecureLoopback) override;

    private:
        // 呼出し側が所有するwindowの借用
        void* m_ownerWindow{};
        // URL起動のWindows API境界
        ShellOpenFunction m_shellOpen;
    };

    // LocalAppDataの名前空間別保存先でstoreを作る(gameId: ゲームの名前空間ID, environmentId: 環境の名前空間ID)。
    [[nodiscard]] std::unique_ptr<IRefreshTokenStore>
        MakeWindowsRefreshTokenStore(
            std::string gameId,
            std::string environmentId = "production");
    // Windowsの認証URL起動処理を作る(ownerWindow: 呼出し側が所有するwindow)。
    [[nodiscard]] std::unique_ptr<IAuthorizationLauncher>
        MakeWindowsAuthorizationLauncher(
            void* ownerWindow = nullptr);
}
