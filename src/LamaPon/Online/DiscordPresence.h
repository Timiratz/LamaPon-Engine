#pragma once

#include "LamaPon/Core/Api.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace LamaPon
{
    // 表示テキストの最小バイト数
    inline constexpr std::size_t DiscordActivityTextMinBytes = 2u;
    // 表示テキストの最大バイト数
    inline constexpr std::size_t DiscordActivityTextMaxBytes = 128u;

    // 画像名の最大バイト数
    inline constexpr std::size_t DiscordActivityImageKeyMaxBytes = 256u;
    // Application IDの最大長
    inline constexpr std::size_t DiscordApplicationIdMaxBytes = 32u;

    // Discordへの更新間隔・秒
    inline constexpr float DiscordPresenceUpdateIntervalSeconds = 15.0f;

    // Game Module DLLのレイアウトを保つため、新しいフィールドは末尾へ追加します。
    // 開始時刻だけなら経過時間、終了時刻だけなら残り時間の表示に使います。
    struct DiscordActivity final
    {
        // 表示の1行目
        std::string details;
        // 表示の2行目
        std::string state;

        // 大画像のAsset名またはURL
        std::string largeImageKey;
        // 大画像の説明文
        std::string largeImageText;

        // 小画像のAsset名またはURL
        std::string smallImageKey;
        // 小画像の説明文
        std::string smallImageText;

        // 開始のUnix秒・0なら未指定
        std::int64_t startTimestamp{};
        // 終了のUnix秒・0なら未指定
        std::int64_t endTimestamp{};
    };

    // Activityの時刻に使う現在のUnix秒を返す。
    [[nodiscard]] LAMAPON_API std::int64_t
        DiscordPresenceUnixTime() noexcept;

    // 公開設定だけをproject.jsonへ保存し、秘密やアクセストークンを含めません。
    struct DiscordPresenceConfiguration final
    {
        // Discord表示機能の有効指定
        bool enabled{};
        // ゲームの公開Application ID
        std::string applicationId;
        // 大画像未指定時の画像名
        std::string defaultLargeImageKey;
        // 大画像説明未指定時の説明
        std::string defaultLargeImageText;
    };

    enum class DiscordPresenceState : std::uint8_t
    {
        // 設定が無効、またはApplication IDが未設定です。
        Disabled,
        // Discord未起動・アダプター未登録などで接続を利用できません。
        Unavailable,
        // backendは使えますが、まだActivityを出していません。
        Ready,
        // Activityを表示中です。
        Active
    };

    // 表示機能の状態の英語名を返す(state: 表示機能の状態)。
    [[nodiscard]] LAMAPON_API std::string_view
        DiscordPresenceStateName(
            DiscordPresenceState state) noexcept;

    // バックエンドはApplicationのスレッドで呼び出し、TickをブロックさせずDiscordPresenceから利用します。
    class DiscordPresenceBackend
    {
    public:
        // 表示バックエンドを破棄する。
        virtual ~DiscordPresenceBackend() = default;

        // 未接続の表示バックエンドを作る。
        DiscordPresenceBackend() = default;
        // バックエンドの複製を禁止する。
        DiscordPresenceBackend(const DiscordPresenceBackend&) = delete;
        // バックエンドのコピー代入を禁止する。
        DiscordPresenceBackend& operator=(
            const DiscordPresenceBackend&) = delete;
        // バックエンドの移動を禁止する。
        DiscordPresenceBackend(DiscordPresenceBackend&&) = delete;
        // バックエンドのムーブ代入を禁止する。
        DiscordPresenceBackend& operator=(
            DiscordPresenceBackend&&) = delete;

        // 例外を投げずにDiscord接続を試みる(applicationId: ゲームの公開ID)。
        [[nodiscard]] virtual bool Initialize(
            std::string_view applicationId) = 0;
        // Discordとの接続を終了する。
        virtual void Shutdown() noexcept = 0;

        // 検証済みのActivityを送り、送信失敗時はfalseを返す(activity: 表示する内容)。
        [[nodiscard]] virtual bool SetActivity(
            const DiscordActivity& activity) = 0;
        // 表示中のActivityを消す。
        virtual void ClearActivity() noexcept = 0;

        // 非同期の接続と受信を進め、すぐに戻る(elapsedSeconds: 経過秒数)。
        virtual void Tick(float elapsedSeconds) noexcept = 0;

        // Discordとの接続が利用可能かを返す。
        [[nodiscard]] virtual bool IsAvailable() const noexcept = 0;

        // バックエンドのエラーを借用し、有効期間は次の呼び出しまでとする。
        [[nodiscard]] virtual std::string_view
            LastError() const noexcept
        {
            return {};
        }
    };

    using DiscordPresenceBackendFactory =
        std::function<std::unique_ptr<DiscordPresenceBackend>()>;

    // Configure前にApplicationのスレッドでアダプターを登録する(factory: バックエンドを作る関数)。
    LAMAPON_API void SetDiscordPresenceBackendFactory(
        DiscordPresenceBackendFactory factory);
    // 登録したバックエンドを作り、未登録なら空を返す。
    [[nodiscard]] LAMAPON_API std::unique_ptr<DiscordPresenceBackend>
        MakeDiscordPresenceBackend();

    // アカウント連携やクラウドセーブと独立した表示機能で、全操作をApplicationと同じスレッドで行います。
    class DiscordPresence final
    {
    public:
        // 無効状態の表示管理を作る。
        LAMAPON_API DiscordPresence();
        // 表示とバックエンドを終了する。
        LAMAPON_API ~DiscordPresence();

        // 表示管理の複製を禁止する。
        DiscordPresence(const DiscordPresence&) = delete;
        // 表示管理のコピー代入を禁止する。
        DiscordPresence& operator=(const DiscordPresence&) = delete;
        // 表示管理の移動を禁止する。
        DiscordPresence(DiscordPresence&&) = delete;
        // 表示管理のムーブ代入を禁止する。
        DiscordPresence& operator=(DiscordPresence&&) = delete;

        // 接続先を設定し、無効指定またはIDなしなら接続を作らない(configuration: 公開表示設定)。
        // SDKは同梱せず、アダプター未登録ならUnavailableのままゲームを継続します。
        LAMAPON_API void Configure(
            DiscordPresenceConfiguration configuration);
        // 表示・接続・保留内容を消して無効化する。
        LAMAPON_API void Shutdown() noexcept;

        // 再接続と更新間隔を毎フレーム進める(elapsedSeconds: 経過秒数)。
        LAMAPON_API void Tick(float elapsedSeconds) noexcept;

        // 既定画像を補って表示要求を保留する(activity: 表示する内容)。
        // 利用不可ならfalseでも検証済みの要求を保持し、復帰時に最新の内容を送ります。
        [[nodiscard]] LAMAPON_API bool SetActivity(
            const DiscordActivity& activity) noexcept;
        // 詳細と状態から新しい表示要求を作る(details: 表示の1行目, state: 表示の2行目)。
        [[nodiscard]] LAMAPON_API bool SetActivity(
            std::string_view details,
            std::string_view state) noexcept;
        // 内容を消す要求を更新枠で送信し、同じ要求の重複を避ける。
        LAMAPON_API void ClearActivity() noexcept;

        // 表示バックエンドが利用可能かを返す。
        [[nodiscard]] LAMAPON_API bool IsAvailable() const noexcept;
        // 接続と表示の状態を返す。
        [[nodiscard]] LAMAPON_API DiscordPresenceState
            State() const noexcept;
        // 現在保持する表示要求があるかを返す。
        [[nodiscard]] LAMAPON_API bool HasActivity() const noexcept;
        // 既定画像を補った最新の表示要求を借用する。
        [[nodiscard]] LAMAPON_API const DiscordActivity&
            Activity() const noexcept;
        // 設定した公開Application IDを借用する。
        [[nodiscard]] LAMAPON_API const std::string&
            ApplicationId() const noexcept;
        // 直近の接続または入力エラーを借用する。
        [[nodiscard]] LAMAPON_API const std::string&
            LastError() const noexcept;

    private:
        struct Implementation;

        // 表示管理状態の所有先
        std::unique_ptr<Implementation> m_implementation;
    };
}
