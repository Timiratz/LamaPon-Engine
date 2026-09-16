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
    // Discordが1件のActivityに許すテキスト長です。Discordはこれを
    // 超える更新を黙って捨てるため、LamaPon側で先に弾きます。
    inline constexpr std::size_t DiscordActivityTextMinBytes = 2u;
    inline constexpr std::size_t DiscordActivityTextMaxBytes = 128u;
    // 画像キーはDiscord Developer PortalのArt Asset名、または
    // Discordが許可する画像URLです。
    inline constexpr std::size_t DiscordActivityImageKeyMaxBytes = 256u;
    inline constexpr std::size_t DiscordApplicationIdMaxBytes = 32u;

    // Discordは1クライアントあたり15秒に1回だけActivity更新を受け付け、
    // 超過分を黙って捨てます。LamaPonは最新の要求だけを保持し、この
    // 間隔でまとめて送ります。
    inline constexpr float DiscordPresenceUpdateIntervalSeconds = 15.0f;

    // ゲームジャンルに依存しない表示内容です。どのフィールドへ何を
    // 入れるかはゲーム制作者が決めます（"Stage 5" / "Year 12" /
    // "Puzzle 48" など）。
    //
    // Discord SDKの型をここへ持ち込まないでください。将来Party、
    // Secrets、Join、Invite、Spectateを足すときは、Game Module DLLの
    // レイアウトを壊さないよう必ず末尾へ追加します。
    struct DiscordActivity final
    {
        // 1行目に表示されます。例: "Chapter 3"
        std::string details;
        // 2行目に表示されます。例: "Boss Battle"
        std::string state;

        // Discord Applicationへ登録したArt Asset名です。
        std::string largeImageKey;
        std::string largeImageText;

        std::string smallImageKey;
        std::string smallImageText;

        // Unix秒です。0は「指定なし」で、Discordは経過時間を
        // 表示しません。startTimestampだけを入れると経過時間、
        // endTimestampだけを入れると残り時間になります。
        std::int64_t startTimestamp{};
        std::int64_t endTimestamp{};
    };

    // startTimestampへ入れる現在時刻（Unix秒）です。
    [[nodiscard]] LAMAPON_API std::int64_t
        DiscordPresenceUnixTime() noexcept;

    // project.jsonへ保存してよい公開設定だけを持ちます。Discordの
    // client_secret、access token、refresh tokenはここへ入れません。
    struct DiscordPresenceConfiguration final
    {
        bool enabled{};
        // Discord Developer Portalでゲームごとに発行する公開IDです。
        // LamaPonは特定のApplication IDを強制しません。
        std::string applicationId;
        // Activity側が空のときに補う既定値です。
        std::string defaultLargeImageKey;
        std::string defaultLargeImageText;
    };

    enum class DiscordPresenceState : std::uint8_t
    {
        // 設定が無効、またはApplication IDが未設定です。
        Disabled,
        // 有効だがbackendへ接続できません（Discord未起動、
        // backend未導入など）。ゲームは通常どおり動きます。
        Unavailable,
        // backendは使えますが、まだActivityを出していません。
        Ready,
        // Activityを表示中です。
        Active
    };

    [[nodiscard]] LAMAPON_API std::string_view
        DiscordPresenceStateName(
            DiscordPresenceState state) noexcept;

    // Discord SDKやDiscordのRPCを直接触る唯一の層です。ゲームや
    // Scriptからはこの型を使わず、DiscordPresenceだけを使います。
    //
    //   Game / Script -> DiscordPresence -> DiscordPresenceBackend
    //                                       -> Discord SDK / API
    //
    // 実装はApplicationを動かすスレッドからだけ呼ばれます。Tick()は
    // 毎フレーム呼ばれるため、ブロックしてはいけません。
    class DiscordPresenceBackend
    {
    public:
        virtual ~DiscordPresenceBackend() = default;

        DiscordPresenceBackend() = default;
        DiscordPresenceBackend(const DiscordPresenceBackend&) = delete;
        DiscordPresenceBackend& operator=(
            const DiscordPresenceBackend&) = delete;
        DiscordPresenceBackend(DiscordPresenceBackend&&) = delete;
        DiscordPresenceBackend& operator=(
            DiscordPresenceBackend&&) = delete;

        // Discordへ接続できなければfalseを返します。例外を投げず、
        // 失敗してもゲームを止めません。
        [[nodiscard]] virtual bool Initialize(
            std::string_view applicationId) = 0;
        virtual void Shutdown() noexcept = 0;

        // 検証済みのActivityを受け取ります。送信できなければfalseです。
        [[nodiscard]] virtual bool SetActivity(
            const DiscordActivity& activity) = 0;
        virtual void ClearActivity() noexcept = 0;

        // 再接続や受信処理をここで進めます。独自スレッドを増やさず、
        // 呼び出しはすぐ返してください。
        virtual void Tick(float elapsedSeconds) noexcept = 0;

        [[nodiscard]] virtual bool IsAvailable() const noexcept = 0;

        // backendが所有する文字列を指します。次の呼び出しまで有効で
        // あれば十分です。
        [[nodiscard]] virtual std::string_view
            LastError() const noexcept
        {
            return {};
        }
    };

    using DiscordPresenceBackendFactory =
        std::function<std::unique_ptr<DiscordPresenceBackend>()>;

    // Discord SDKアダプターを差し込む口です。LamaPonはDiscord SDKを
    // 同梱しないため、既定ではbackendを持たず、Presenceは
    // Unavailableのまま安全に無効化されます。
    // Applicationを動かすスレッドから、Configure()より前に呼びます。
    LAMAPON_API void SetDiscordPresenceBackendFactory(
        DiscordPresenceBackendFactory factory);
    [[nodiscard]] LAMAPON_API std::unique_ptr<DiscordPresenceBackend>
        MakeDiscordPresenceBackend();

    // Discord Rich Presenceの表示を管理します。Discordアカウント連携
    // （DiscordAuth）とクラウドセーブからは完全に独立していて、
    // ログインしていなくても、オンライン設定が無効でも使えます。
    //
    // すべてのメソッドは、Applicationを動かす同じスレッドから
    // 呼んでください。
    class DiscordPresence final
    {
    public:
        LAMAPON_API DiscordPresence();
        LAMAPON_API ~DiscordPresence();

        DiscordPresence(const DiscordPresence&) = delete;
        DiscordPresence& operator=(const DiscordPresence&) = delete;
        DiscordPresence(DiscordPresence&&) = delete;
        DiscordPresence& operator=(DiscordPresence&&) = delete;

        // enabledがfalse、またはapplicationIdが空なら、backendを
        // 作らずDisabledのままにします。backendを作れない場合も
        // 警告ログだけを出し、例外は投げません。
        LAMAPON_API void Configure(
            DiscordPresenceConfiguration configuration);
        LAMAPON_API void Shutdown() noexcept;

        // Application::Update -> OnlineServices::Updateから毎フレーム
        // 呼ばれます。再接続とDiscordの更新間隔をここで進めます。
        LAMAPON_API void Tick(float elapsedSeconds) noexcept;

        // 受理できたときtrueです。未初期化、backend利用不可、
        // 入力が長すぎる場合はfalseを返し、LastError()へ理由を残します。
        [[nodiscard]] LAMAPON_API bool SetActivity(
            const DiscordActivity& activity) noexcept;
        // details / stateだけを差し替える簡易版です。
        [[nodiscard]] LAMAPON_API bool SetActivity(
            std::string_view details,
            std::string_view state) noexcept;
        // 何度呼んでも安全です。
        LAMAPON_API void ClearActivity() noexcept;

        [[nodiscard]] LAMAPON_API bool IsAvailable() const noexcept;
        [[nodiscard]] LAMAPON_API DiscordPresenceState
            State() const noexcept;
        [[nodiscard]] LAMAPON_API bool HasActivity() const noexcept;
        // 既定画像を補い、Discordへ送る形にしたActivityです。
        [[nodiscard]] LAMAPON_API const DiscordActivity&
            Activity() const noexcept;
        [[nodiscard]] LAMAPON_API const std::string&
            ApplicationId() const noexcept;
        [[nodiscard]] LAMAPON_API const std::string&
            LastError() const noexcept;

    private:
        struct Implementation;

        std::unique_ptr<Implementation> m_implementation;
    };
}
