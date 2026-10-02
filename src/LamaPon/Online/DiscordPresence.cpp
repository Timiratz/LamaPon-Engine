#include "LamaPon/Online/DiscordPresence.h"

#include "LamaPon/Core/Log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace
{
    // 32文字以内のASCII数字だけの公開IDかを判定する(value: 検査する公開ID)。
    [[nodiscard]] bool IsApplicationId(
        const std::string_view value) noexcept
    {
        if (value.empty()
            || value.size()
                > LamaPon::DiscordApplicationIdMaxBytes)
        {
            return false;
        }
        // ASCII数字かを判定(character: IDの1文字)。
        return std::ranges::all_of(
            value,
            [](const char character) noexcept
            {
                return character >= '0' && character <= '9';
            });
    }

    // 改行・NUL・DELなどの制御文字を調べる(value: 検査する文字列)。
    [[nodiscard]] bool HasControlCharacter(
        const std::string_view value) noexcept
    {
        // 制御文字かを判定(character: 表示文字列の1文字)。
        return std::ranges::any_of(
            value,
            [](const char character) noexcept
            {
                // 1文字の符号なし表現
                const auto code =
                    static_cast<unsigned char>(character);
                return code < 0x20u || code == 0x7Fu;
            });
    }

    // 空または2〜128バイトの表示文を検証する(value: 表示文, fieldName: 診断用の項目名, error: 検証エラーの出力先)。
    [[nodiscard]] bool ValidateText(
        const std::string_view value,
        const char* const fieldName,
        std::string& error)
    {
        if (value.empty())
        {
            return true;
        }
        if (HasControlCharacter(value))
        {
            error = std::string{ fieldName }
                + "に制御文字は使えません。";
            return false;
        }
        if (value.size() < LamaPon::DiscordActivityTextMinBytes)
        {
            error = std::string{ fieldName }
                + "はDiscordの仕様で2バイト以上必要です。";
            return false;
        }
        if (value.size() > LamaPon::DiscordActivityTextMaxBytes)
        {
            error = std::string{ fieldName }
                + "がDiscordの上限（128バイト）を超えています。";
            return false;
        }
        return true;
    }

    // 空または256バイト以内の画像名を検証する(value: 画像名またはURL, fieldName: 診断用の項目名, error: 検証エラーの出力先)。
    [[nodiscard]] bool ValidateImageKey(
        const std::string_view value,
        const char* const fieldName,
        std::string& error)
    {
        if (value.empty())
        {
            return true;
        }
        if (HasControlCharacter(value))
        {
            error = std::string{ fieldName }
                + "に制御文字は使えません。";
            return false;
        }
        if (value.size()
            > LamaPon::DiscordActivityImageKeyMaxBytes)
        {
            error = std::string{ fieldName }
                + "がDiscordの上限（256バイト）を超えています。";
            return false;
        }
        return true;
    }

    // 表示文・画像名・時刻の組合せを検証する(activity: 表示要求, error: 検証エラーの出力先)。
    [[nodiscard]] bool ValidateActivity(
        const LamaPon::DiscordActivity& activity,
        std::string& error)
    {
        if (!ValidateText(activity.details, "details", error)
            || !ValidateText(activity.state, "state", error)
            || !ValidateText(
                activity.largeImageText,
                "largeImageText",
                error)
            || !ValidateText(
                activity.smallImageText,
                "smallImageText",
                error)
            || !ValidateImageKey(
                activity.largeImageKey,
                "largeImageKey",
                error)
            || !ValidateImageKey(
                activity.smallImageKey,
                "smallImageKey",
                error))
        {
            return false;
        }
        if (activity.startTimestamp < 0
            || activity.endTimestamp < 0)
        {
            error = "タイムスタンプに負の値は使えません。";
            return false;
        }
        if (activity.startTimestamp != 0
            && activity.endTimestamp != 0
            && activity.endTimestamp < activity.startTimestamp)
        {
            error =
                "endTimestampはstartTimestamp以降にしてください。";
            return false;
        }
        // 小さい画像だけを指定してもDiscordは表示しません。
        if (activity.largeImageKey.empty()
            && !activity.smallImageKey.empty())
        {
            error =
                "smallImageKeyを使うにはlargeImageKeyも必要です。";
            return false;
        }
        return true;
    }

    // プロセスで共有するバックエンド生成関数を借用する。
    LamaPon::DiscordPresenceBackendFactory& BackendFactory()
    {
        // 登録されたバックエンド生成関数
        static LamaPon::DiscordPresenceBackendFactory factory;
        return factory;
    }
}

namespace LamaPon
{
    std::int64_t DiscordPresenceUnixTime() noexcept
    {
        return std::chrono::duration_cast<
            std::chrono::seconds>(
                std::chrono::system_clock::now()
                    .time_since_epoch())
            .count();
    }

    std::string_view DiscordPresenceStateName(
        const DiscordPresenceState state) noexcept
    {
        switch (state)
        {
        case DiscordPresenceState::Disabled:
            return "Disabled";
        case DiscordPresenceState::Unavailable:
            return "Unavailable";
        case DiscordPresenceState::Ready:
            return "Ready";
        case DiscordPresenceState::Active:
            return "Active";
        }
        return "Disabled";
    }

    void SetDiscordPresenceBackendFactory(
        DiscordPresenceBackendFactory factory)
    {
        BackendFactory() = std::move(factory);
    }

    std::unique_ptr<DiscordPresenceBackend>
        MakeDiscordPresenceBackend()
    {
        // 登録されたバックエンド生成関数
        const auto& factory = BackendFactory();
        if (!factory)
        {
            // アダプター未登録時はバックエンドなしで動作します。
            return {};
        }
        return factory();
    }

    struct DiscordPresence::Implementation final
    {
        // 公開の表示設定
        DiscordPresenceConfiguration configuration;
        // 表示バックエンドの所有先
        std::unique_ptr<DiscordPresenceBackend> backend;
        // 接続と表示の状態
        DiscordPresenceState state{
            DiscordPresenceState::Disabled
        };
        // 直近の失敗理由
        std::string lastError;
        // 利用不可でも表示内容を保持し、接続復帰時に送り直します。
        // 最新の表示要求
        DiscordActivity activity;
        // 表示する内容を保持しているか
        bool hasActivity{};
        // 送信待ちの更新要求があるか
        bool pendingUpdate{};
        // バックエンドを初期化済みか
        bool initialized{};
        // backendアダプターが未登録なら再試行しても意味がありません。
        // アダプターが未登録か
        bool backendMissing{};
        // 前回の表示更新からの秒数
        float secondsSinceUpdate{
            DiscordPresenceUpdateIntervalSeconds
        };
        // 初期化の再試行までの残り秒数
        float initializeRetrySeconds{};
        // 利用不可の警告を報告済みか
        bool unavailableReported{};

        // 失敗理由を保存し、利用不可の警告を一度だけ出す(reason: 表示する失敗理由)。
        void ReportUnavailable(const std::string_view reason)
        {
            lastError = std::string{ reason };
            if (unavailableReported)
            {
                return;
            }
            unavailableReported = true;
            Logger::Instance().Warning(
                "Discord Rich Presenceを利用できません。"
                "Discord Activityなしでゲームを続行します: "
                + lastError);
        }

        // 初期化済みのバックエンドが接続を利用できるかを調べる。
        [[nodiscard]] bool BackendAvailable() const noexcept
        {
            return initialized
                && backend != nullptr
                && backend->IsAvailable();
        }

        // アダプターを作って接続を試し、成功時は保留内容をすぐに送る。
        void TryInitialize()
        {
            if (initialized || backendMissing)
            {
                return;
            }
            if (!backend)
            {
                backend = MakeDiscordPresenceBackend();
                if (!backend)
                {
                    backendMissing = true;
                    state = DiscordPresenceState::Unavailable;
                    ReportUnavailable(
                        "Discord Presenceアダプターが"
                        "登録されていません。");
                    return;
                }
            }
            if (!backend->Initialize(configuration.applicationId))
            {
                state = DiscordPresenceState::Unavailable;
                // バックエンドが返す失敗理由
                const auto reason = backend->LastError();
                ReportUnavailable(
                    reason.empty()
                        ? std::string_view{
                            "Discordクライアントへ接続できません。" }
                        : reason);
                // Discordを後から起動した場合に拾えるよう、一定間隔で接続をやり直します。
                initializeRetrySeconds =
                    DiscordPresenceUpdateIntervalSeconds;
                return;
            }
            initialized = true;
            unavailableReported = false;
            lastError.clear();
            state = DiscordPresenceState::Ready;
            // 接続直後は間隔待ちを挟まず反映します。
            secondsSinceUpdate =
                DiscordPresenceUpdateIntervalSeconds;
            if (hasActivity)
            {
                pendingUpdate = true;
            }
            Flush();
        }

        // 保留している最新の表示または消去要求を送信する。
        void Flush()
        {
            if (!pendingUpdate || !BackendAvailable())
            {
                return;
            }
            if (!hasActivity)
            {
                backend->ClearActivity();
                pendingUpdate = false;
                secondsSinceUpdate = 0.0f;
                state = DiscordPresenceState::Ready;
                return;
            }
            if (!backend->SetActivity(activity))
            {
                // バックエンドが返す失敗理由
                const auto reason = backend->LastError();
                lastError = reason.empty()
                    ? "Discordへ Activityを送信できませんでした。"
                    : std::string{ reason };
                // 次の更新枠でもう一度送ります。
                secondsSinceUpdate = 0.0f;
                return;
            }
            pendingUpdate = false;
            secondsSinceUpdate = 0.0f;
            lastError.clear();
            state = DiscordPresenceState::Active;
        }

        // 更新間隔に達していれば保留要求を送信する。
        void FlushIfDue()
        {
            if (secondsSinceUpdate
                >= DiscordPresenceUpdateIntervalSeconds)
            {
                Flush();
            }
        }

        // 表示を消してバックエンドを終了し、所有権を解放する。
        void ReleaseBackend() noexcept
        {
            if (backend)
            {
                backend->ClearActivity();
                backend->Shutdown();
                backend.reset();
            }
            initialized = false;
        }
    };

    DiscordPresence::DiscordPresence()
        : m_implementation(std::make_unique<Implementation>())
    {
    }

    DiscordPresence::~DiscordPresence()
    {
        Shutdown();
    }

    void DiscordPresence::Configure(
        DiscordPresenceConfiguration configuration)
    {
        // 表示管理状態の参照
        auto& implementation = *m_implementation;
        // 初期化済みの同じ公開IDか
        const bool sameConnection =
            implementation.initialized
            && configuration.enabled
            && configuration.applicationId
                == implementation.configuration.applicationId;
        if (sameConnection)
        {
            // 既定画像だけの変更でDiscordとの接続を切りません。
            implementation.configuration = std::move(configuration);
            return;
        }

        implementation.ReleaseBackend();
        implementation.configuration = std::move(configuration);
        implementation.state = DiscordPresenceState::Disabled;
        implementation.activity = {};
        implementation.hasActivity = false;
        implementation.pendingUpdate = false;
        implementation.backendMissing = false;
        implementation.unavailableReported = false;
        implementation.initializeRetrySeconds = 0.0f;
        implementation.secondsSinceUpdate =
            DiscordPresenceUpdateIntervalSeconds;
        implementation.lastError.clear();

        if (!implementation.configuration.enabled)
        {
            return;
        }
        if (implementation.configuration.applicationId.empty())
        {
            implementation.lastError =
                "Discord Application IDが未設定です。";
            Logger::Instance().Warning(
                "Discord Rich Presenceが有効ですが、"
                "Discord Application IDが未設定のため無効にしました。");
            return;
        }
        if (!IsApplicationId(
            implementation.configuration.applicationId))
        {
            implementation.lastError =
                "Discord Application IDはASCII数字だけで"
                "指定してください。";
            Logger::Instance().Warning(
                "Discord Rich Presenceを無効にしました: "
                + implementation.lastError);
            implementation.configuration.applicationId.clear();
            return;
        }
        implementation.TryInitialize();
    }

    void DiscordPresence::Shutdown() noexcept
    {
        // 表示管理状態の参照
        auto& implementation = *m_implementation;
        implementation.ReleaseBackend();
        implementation.configuration.enabled = false;
        implementation.state = DiscordPresenceState::Disabled;
        implementation.activity = {};
        implementation.hasActivity = false;
        implementation.pendingUpdate = false;
        implementation.backendMissing = false;
        implementation.unavailableReported = false;
        implementation.initializeRetrySeconds = 0.0f;
        implementation.secondsSinceUpdate =
            DiscordPresenceUpdateIntervalSeconds;
    }

    void DiscordPresence::Tick(float elapsedSeconds) noexcept
    {
        // 表示管理状態の参照
        auto& implementation = *m_implementation;
        if (!implementation.configuration.enabled
            || implementation.configuration.applicationId.empty())
        {
            return;
        }
        if (!std::isfinite(elapsedSeconds)
            || elapsedSeconds < 0.0f)
        {
            elapsedSeconds = 0.0f;
        }
        if (!implementation.initialized)
        {
            if (implementation.backendMissing)
            {
                return;
            }
            implementation.initializeRetrySeconds -=
                elapsedSeconds;
            if (implementation.initializeRetrySeconds > 0.0f)
            {
                return;
            }
            implementation.TryInitialize();
            return;
        }

        implementation.backend->Tick(elapsedSeconds);
        if (!implementation.backend->IsAvailable())
        {
            if (implementation.state
                != DiscordPresenceState::Unavailable)
            {
                implementation.state =
                    DiscordPresenceState::Unavailable;
                implementation.ReportUnavailable(
                    "Discordクライアントとの接続が切れました。");
            }
            // 復帰したフレームで待たずに送り直せるようにします。
            implementation.pendingUpdate =
                implementation.pendingUpdate
                || implementation.hasActivity;
            implementation.secondsSinceUpdate =
                DiscordPresenceUpdateIntervalSeconds;
            return;
        }
        if (implementation.state
            == DiscordPresenceState::Unavailable)
        {
            implementation.state = DiscordPresenceState::Ready;
            implementation.unavailableReported = false;
            implementation.lastError.clear();
        }
        implementation.secondsSinceUpdate += elapsedSeconds;
        implementation.FlushIfDue();
    }

    bool DiscordPresence::SetActivity(
        const DiscordActivity& activity) noexcept
    {
        // 表示管理状態の参照
        auto& implementation = *m_implementation;
        if (!implementation.configuration.enabled
            || implementation.configuration.applicationId.empty())
        {
            implementation.lastError =
                "Discord Rich Presenceが無効です。";
            return false;
        }
        // 既定画像を補った表示要求
        auto resolved = activity;
        if (resolved.largeImageKey.empty())
        {
            resolved.largeImageKey =
                implementation.configuration.defaultLargeImageKey;
        }
        if (resolved.largeImageText.empty())
        {
            resolved.largeImageText =
                implementation.configuration.defaultLargeImageText;
        }
        // 表示内容の検証エラー
        std::string error;
        if (!ValidateActivity(resolved, error))
        {
            implementation.lastError = std::move(error);
            return false;
        }

        implementation.activity = std::move(resolved);
        implementation.hasActivity = true;
        implementation.pendingUpdate = true;
        if (!implementation.BackendAvailable())
        {
            implementation.lastError =
                "Discord Rich Presenceを利用できません。"
                "接続できた時点で最後の内容を送信します。";
            return false;
        }
        implementation.lastError.clear();
        implementation.FlushIfDue();
        return true;
    }

    bool DiscordPresence::SetActivity(
        const std::string_view details,
        const std::string_view state) noexcept
    {
        // 最新の表示要求
        DiscordActivity activity;
        activity.details = details;
        activity.state = state;
        return SetActivity(activity);
    }

    void DiscordPresence::ClearActivity() noexcept
    {
        // 表示管理状態の参照
        auto& implementation = *m_implementation;
        if (!implementation.hasActivity
            && !implementation.pendingUpdate)
        {
            // 何度呼んでも余計な送信をしません。
            return;
        }
        implementation.activity = {};
        implementation.hasActivity = false;
        implementation.pendingUpdate = true;
        implementation.FlushIfDue();
    }

    bool DiscordPresence::IsAvailable() const noexcept
    {
        return m_implementation->BackendAvailable();
    }

    DiscordPresenceState DiscordPresence::State() const noexcept
    {
        return m_implementation->state;
    }

    bool DiscordPresence::HasActivity() const noexcept
    {
        return m_implementation->hasActivity;
    }

    const DiscordActivity& DiscordPresence::Activity() const noexcept
    {
        return m_implementation->activity;
    }

    const std::string& DiscordPresence::ApplicationId() const noexcept
    {
        return m_implementation->configuration.applicationId;
    }

    const std::string& DiscordPresence::LastError() const noexcept
    {
        return m_implementation->lastError;
    }
}
