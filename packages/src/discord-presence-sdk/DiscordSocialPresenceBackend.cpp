#include "DiscordSocialPresenceBackend.h"

#if defined(LAMAPON_DISCORD_SOCIAL_SDK)

#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace
{
    // 10進数のDiscord IDを検証・変換します(text: ID文字列, value: 出力先)。
    bool ParseApplicationId(
        const std::string_view text,
        std::uint64_t& value) noexcept
    {
        // 空または桁数超過のIDを拒否します。
        if (text.empty()
            || text.size() > 20u)
        {
            return false;
        }
        // 検証中の数値ID
        std::uint64_t parsed = 0u;
        // IDを10進数として1桁ずつ検証します。
        for (const auto character : text)
        {
            // 数字以外の文字を拒否します。
            if (character < '0' || character > '9')
            {
                return false;
            }
            // 現在の10進数桁
            const auto digit = static_cast<std::uint64_t>(
                character - '0');
            // uint64_tの最大値
            constexpr auto limit =
                (std::numeric_limits<std::uint64_t>::max)();
            // 次の桁でuint64_tがあふれる場合は拒否します。
            if (parsed > (limit - digit) / 10u)
            {
                return false;
            }
            parsed = parsed * 10u + digit;
        }
        // Discord IDとして使えない0を拒否します。
        if (parsed == 0u)
        {
            return false;
        }
        value = parsed;
        return true;
    }

    // 空欄と0時刻を未指定としてActivityへ変換します(activity: LamaPonの表示内容)。
    discordpp::Activity ToDiscordActivity(
        const LamaPon::DiscordActivity& activity)
    {
        // SDKへ渡すActivity
        discordpp::Activity result;
        result.SetType(discordpp::ActivityTypes::Playing);
        // 詳細が指定されている場合だけ表示します。
        if (!activity.details.empty())
        {
            result.SetDetails(activity.details);
        }
        // 状態が指定されている場合だけ表示します。
        if (!activity.state.empty())
        {
            result.SetState(activity.state);
        }
        // アイコン情報がある場合だけAssetsを設定します。
        if (!activity.largeImageKey.empty()
            || !activity.largeImageText.empty()
            || !activity.smallImageKey.empty()
            || !activity.smallImageText.empty())
        {
            // SDKへ渡すアイコンと代替テキスト
            discordpp::ActivityAssets assets;
            // 大きいアイコンが指定されている場合だけ設定します。
            if (!activity.largeImageKey.empty())
            {
                assets.SetLargeImage(activity.largeImageKey);
            }
            // 大きいアイコンの説明がある場合だけ設定します。
            if (!activity.largeImageText.empty())
            {
                assets.SetLargeText(activity.largeImageText);
            }
            // 小さいアイコンが指定されている場合だけ設定します。
            if (!activity.smallImageKey.empty())
            {
                assets.SetSmallImage(activity.smallImageKey);
            }
            // 小さいアイコンの説明がある場合だけ設定します。
            if (!activity.smallImageText.empty())
            {
                assets.SetSmallText(activity.smallImageText);
            }
            result.SetAssets(std::move(assets));
        }
        // 0以外の時刻だけをSDKへ渡します。
        if (activity.startTimestamp > 0
            || activity.endTimestamp > 0)
        {
            // Discord上の経過・残り時間
            discordpp::ActivityTimestamps timestamps;
            // 開始時刻が指定されている場合だけ設定します。
            if (activity.startTimestamp > 0)
            {
                timestamps.SetStart(static_cast<std::uint64_t>(
                    activity.startTimestamp));
            }
            // 終了時刻が指定されている場合だけ設定します。
            if (activity.endTimestamp > 0)
            {
                timestamps.SetEnd(static_cast<std::uint64_t>(
                    activity.endTimestamp));
            }
            result.SetTimestamps(std::move(timestamps));
        }
        return result;
    }
}

namespace LamaPonPackages
{
    // DiscordSocialPresenceBackend(): 生存フラグを初期化します。
    DiscordSocialPresenceBackend::DiscordSocialPresenceBackend()
        : m_alive(std::make_shared<bool>(true))
    {
    }

    // ~DiscordSocialPresenceBackend(): 非同期応答を無効化して終了します。
    DiscordSocialPresenceBackend::~DiscordSocialPresenceBackend()
    {
        // 送信中の応答から破棄済みのthisへ触れないよう無効化します。
        if (m_alive)
        {
            *m_alive = false;
        }
        Shutdown();
    }

    // Initialize(applicationId: Discord Application ID): SDKクライアントを初期化します。
    bool DiscordSocialPresenceBackend::Initialize(
        const std::string_view applicationId)
    {
        m_lastError.clear();
        // SDKへ渡す検証済みApplication ID
        std::uint64_t parsed = 0u;
        // ID検証に失敗した場合は初期化を中止します。
        if (!ParseApplicationId(applicationId, parsed))
        {
            m_lastError =
                "Discord Application IDが正しくありません。";
            m_available = false;
            return false;
        }
        // SDK初期化例外をバックエンドのエラーへ変換します。
        try
        {
            m_client = std::make_shared<discordpp::Client>();
            // ログインを伴わないRich Presence専用の経路です。
            // OAuthもtokenも使いません。
            m_client->SetApplicationId(parsed);
        }
        // failure: SDK初期化に失敗した例外。
        catch (const std::exception& failure)
        {
            m_client.reset();
            m_lastError = failure.what();
            m_available = false;
            return false;
        }
        // Discordが起動しているかは、最初のUpdateRichPresenceの応答で分かります。
        // ここで接続を待つとゲームの起動が止まるため、いったん利用可能として進めます。
        m_available = true;
        return true;
    }

    // Shutdown(): Presenceを解除してSDKクライアントを解放します。
    void DiscordSocialPresenceBackend::Shutdown() noexcept
    {
        // クライアントがある場合だけPresenceを解除します。
        if (m_client)
        {
            // SDK解除時の例外で終了処理を止めません。
            try
            {
                m_client->ClearRichPresence();
            }
            // SDK解除時の例外は終了処理では回復できません。
            catch (...)
            {
            }
            m_client.reset();
        }
        m_available = false;
    }

    // SetActivity(activity: LamaPonのPresence表示内容): Discordへ非同期送信します。
    bool DiscordSocialPresenceBackend::SetActivity(
        const LamaPon::DiscordActivity& activity)
    {
        // 初期化前は送信を拒否します。
        if (!m_client)
        {
            m_lastError =
                "Discord Social SDKが初期化されていません。";
            return false;
        }
        // SDK送信時の例外をバックエンドのエラーへ変換します。
        try
        {
            // converted: Discord SDK形式へ変換したActivity。
            auto converted = ToDiscordActivity(activity);
            // 応答処理時の生存確認
            const std::weak_ptr<bool> alive = m_alive;
            // 非同期のPresence送信結果を反映します(result: SDK応答)。
            m_client->UpdateRichPresence(
                std::move(converted),
                [this, alive](
                    const discordpp::ClientResult& result)
                {
                    // コールバックから参照する生存フラグ
                    const auto guard = alive.lock();
                    // 応答を受け取る対象が破棄済みなら処理を終えます。
                    if (!guard || !*guard)
                    {
                        return;
                    }
                    // 成功した応答で利用可能状態とエラーを更新します。
                    if (result.Successful())
                    {
                        m_available = true;
                        m_lastError.clear();
                        return;
                    }
                    // Discord未起動などで送れなかった場合です。
                    // LamaPon側がUnavailableへ落として警告を出し、復帰したら送り直します。
                    m_available = false;
                    m_lastError = result.ToString();
                });
            return true;
        }
        // failure: Presence送信に失敗した例外。
        catch (const std::exception& failure)
        {
            m_available = false;
            m_lastError = failure.what();
            return false;
        }
    }

    // ClearActivity(): Discord上のPresenceを安全に解除します。
    void DiscordSocialPresenceBackend::ClearActivity() noexcept
    {
        // 初期化前は解除処理を行いません。
        if (!m_client)
        {
            return;
        }
        // SDK解除時の例外をバックエンドのエラーへ変換します。
        try
        {
            m_client->ClearRichPresence();
        }
        // failure: Presence消去に失敗した例外。
        catch (const std::exception& failure)
        {
            m_lastError = failure.what();
        }
        // SDKの不明な解除失敗もエラーに記録します。
        catch (...)
        {
            m_lastError = "Discord Activityを消去できませんでした。";
        }
    }

    // Tick(): Discord SDKの非同期応答を処理します。
    void DiscordSocialPresenceBackend::Tick(float) noexcept
    {
        // SDKの応答を進め、独自スレッドは使いません。
        try
        {
            discordpp::RunCallbacks();
        }
        // 応答処理の失敗は継続できません。
        catch (...)
        {
        }
    }

    // IsAvailable(): SDK初期化と接続状態を返します。
    bool DiscordSocialPresenceBackend::IsAvailable() const noexcept
    {
        return m_client != nullptr && m_available;
    }

    // LastError(): 最後に発生したDiscordエラーを返します。
    std::string_view
        DiscordSocialPresenceBackend::LastError() const noexcept
    {
        return m_lastError;
    }
}

namespace
{
    // Game Moduleの読込時にDiscord Presence実装を登録します。
    struct BackendRegistration final
    {
        // BackendRegistration(): DLL読込時にDiscord実装を登録します。
        BackendRegistration()
        {
            LamaPon::SetDiscordPresenceBackendFactory(
                []
                {
                    return std::unique_ptr<
                        LamaPon::DiscordPresenceBackend>(
                        std::make_unique<LamaPonPackages::
                            DiscordSocialPresenceBackend>());
                });
        }
    };

    // DLLの静的初期化時にBackendを登録するインスタンス
    const BackendRegistration Registration{};
}

#endif
