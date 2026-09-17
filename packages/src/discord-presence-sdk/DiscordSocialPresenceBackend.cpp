// Discord Social SDKを使ったRich Presenceアダプターの実装です。
// このファイルだけがdiscordpp（Discord SDK）へ触れます。ゲームや
// Scriptからは LamaPon::DiscordPresence だけを使ってください。
#include "DiscordSocialPresenceBackend.h"

#if defined(LAMAPON_DISCORD_SOCIAL_SDK)

#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace
{
    // Application IDは10進数の公開IDです。LamaPon側でも桁の検証を
    // しますが、SDKへ渡すuint64_tへ直すのはここの責任です。
    bool ParseApplicationId(
        const std::string_view text,
        std::uint64_t& value) noexcept
    {
        if (text.empty()
            || text.size() > 20u)
        {
            return false;
        }
        std::uint64_t parsed = 0u;
        for (const auto character : text)
        {
            if (character < '0' || character > '9')
            {
                return false;
            }
            const auto digit = static_cast<std::uint64_t>(
                character - '0');
            constexpr auto limit =
                (std::numeric_limits<std::uint64_t>::max)();
            if (parsed > (limit - digit) / 10u)
            {
                return false;
            }
            parsed = parsed * 10u + digit;
        }
        if (parsed == 0u)
        {
            return false;
        }
        value = parsed;
        return true;
    }

    // Discordの表示内容へ変換します。空の項目は「指定なし」として
    // 送らず、Discord側の既定表示に任せます。
    discordpp::Activity ToDiscordActivity(
        const LamaPon::DiscordActivity& activity)
    {
        discordpp::Activity result;
        result.SetType(discordpp::ActivityTypes::Playing);
        if (!activity.details.empty())
        {
            result.SetDetails(activity.details);
        }
        if (!activity.state.empty())
        {
            result.SetState(activity.state);
        }
        if (!activity.largeImageKey.empty()
            || !activity.largeImageText.empty()
            || !activity.smallImageKey.empty()
            || !activity.smallImageText.empty())
        {
            discordpp::ActivityAssets assets;
            if (!activity.largeImageKey.empty())
            {
                assets.SetLargeImage(activity.largeImageKey);
            }
            if (!activity.largeImageText.empty())
            {
                assets.SetLargeText(activity.largeImageText);
            }
            if (!activity.smallImageKey.empty())
            {
                assets.SetSmallImage(activity.smallImageKey);
            }
            if (!activity.smallImageText.empty())
            {
                assets.SetSmallText(activity.smallImageText);
            }
            result.SetAssets(std::move(assets));
        }
        // 0は「指定なし」です。startだけなら経過時間、endだけなら
        // 残り時間としてDiscordが表示します。
        if (activity.startTimestamp > 0
            || activity.endTimestamp > 0)
        {
            discordpp::ActivityTimestamps timestamps;
            if (activity.startTimestamp > 0)
            {
                timestamps.SetStart(static_cast<std::uint64_t>(
                    activity.startTimestamp));
            }
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
    DiscordSocialPresenceBackend::DiscordSocialPresenceBackend()
        : m_alive(std::make_shared<bool>(true))
    {
    }

    DiscordSocialPresenceBackend::~DiscordSocialPresenceBackend()
    {
        // 送信中の応答が後から届いても、破棄済みのthisへ触らせません。
        if (m_alive)
        {
            *m_alive = false;
        }
        Shutdown();
    }

    bool DiscordSocialPresenceBackend::Initialize(
        const std::string_view applicationId)
    {
        m_lastError.clear();
        std::uint64_t parsed = 0u;
        if (!ParseApplicationId(applicationId, parsed))
        {
            m_lastError =
                "Discord Application IDが正しくありません。";
            m_available = false;
            return false;
        }
        try
        {
            m_client = std::make_shared<discordpp::Client>();
            // ログインを伴わないRich Presence専用の経路です。
            // OAuthもtokenも使いません。
            m_client->SetApplicationId(parsed);
        }
        catch (const std::exception& failure)
        {
            m_client.reset();
            m_lastError = failure.what();
            m_available = false;
            return false;
        }
        // Discordが起動しているかは、最初のUpdateRichPresenceの
        // 応答で分かります。ここで接続を待つとゲームの起動が
        // 止まるため、いったん利用可能として進めます。
        m_available = true;
        return true;
    }

    void DiscordSocialPresenceBackend::Shutdown() noexcept
    {
        if (m_client)
        {
            try
            {
                m_client->ClearRichPresence();
            }
            catch (...)
            {
                // 終了処理では失敗しても何もできません。
            }
            m_client.reset();
        }
        m_available = false;
    }

    bool DiscordSocialPresenceBackend::SetActivity(
        const LamaPon::DiscordActivity& activity)
    {
        if (!m_client)
        {
            m_lastError =
                "Discord Social SDKが初期化されていません。";
            return false;
        }
        try
        {
            auto converted = ToDiscordActivity(activity);
            const std::weak_ptr<bool> alive = m_alive;
            m_client->UpdateRichPresence(
                std::move(converted),
                [this, alive](
                    const discordpp::ClientResult& result)
                {
                    const auto guard = alive.lock();
                    if (!guard || !*guard)
                    {
                        return;
                    }
                    if (result.Successful())
                    {
                        m_available = true;
                        m_lastError.clear();
                        return;
                    }
                    // Discord未起動などで送れなかった場合です。
                    // LamaPon側がUnavailableへ落として警告を出し、
                    // 復帰したら送り直します。
                    m_available = false;
                    m_lastError = result.ToString();
                });
            return true;
        }
        catch (const std::exception& failure)
        {
            m_available = false;
            m_lastError = failure.what();
            return false;
        }
    }

    void DiscordSocialPresenceBackend::ClearActivity() noexcept
    {
        if (!m_client)
        {
            return;
        }
        try
        {
            m_client->ClearRichPresence();
        }
        catch (const std::exception& failure)
        {
            m_lastError = failure.what();
        }
        catch (...)
        {
            m_lastError = "Discord Activityを消去できませんでした。";
        }
    }

    void DiscordSocialPresenceBackend::Tick(float) noexcept
    {
        // SDKの応答処理を進めます。独自スレッドは作りません。
        try
        {
            discordpp::RunCallbacks();
        }
        catch (...)
        {
            // 応答処理の失敗でゲームを止めません。
        }
    }

    bool DiscordSocialPresenceBackend::IsAvailable() const noexcept
    {
        return m_client != nullptr && m_available;
    }

    std::string_view
        DiscordSocialPresenceBackend::LastError() const noexcept
    {
        return m_lastError;
    }
}

namespace
{
    // Game Module DLLの読み込み時に登録します。Applicationは
    // Game Moduleを読み込んでからConfigureDiscordPresence()を
    // 呼ぶため、この時点で登録しておけば間に合います。
    // Project Settingsでオフにしていれば、backendは作られません。
    struct BackendRegistration final
    {
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

    const BackendRegistration Registration{};
}

#endif
