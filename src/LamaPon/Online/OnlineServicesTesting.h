#pragma once

#include "LamaPon/Core/HttpClient.h"
#include "LamaPon/Online/OnlineServices.h"
#include "LamaPon/Online/WindowsOnlinePlatform.h"

#include <functional>
#include <memory>

namespace LamaPon::Detail
{
    // Runtime内部の単体テスト専用構築口です。通常のゲームコードは
    // OnlineServices.hだけを利用します。
    class OnlineServicesTestAccess final
    {
    public:
        using HttpSender =
            std::function<HttpResponse(const HttpRequest&)>;

        [[nodiscard]] static LAMAPON_API
            std::unique_ptr<OnlineServices> Create(
                OnlineServiceConfiguration configuration,
                HttpSender sender,
                std::unique_ptr<IRefreshTokenStore> refreshTokenStore = {},
                std::unique_ptr<IAuthorizationLauncher>
                    authorizationLauncher = {});

        // owner破棄時の「worker完了済み・Update未reap」を
        // 待機なしで決定論的に検証するための内部テスト専用状態です。
        [[nodiscard]] static LAMAPON_API bool CurrentTaskCompleted(
            const OnlineServices& services) noexcept;

        // OS suspend/main-thread stallをsleepなしで再現し、完了結果の
        // token TTLがreapまでにも減ることを検証します。
        [[nodiscard]] static LAMAPON_API bool AgeCurrentTaskCompletion(
            OnlineServices& services,
            float elapsedSeconds) noexcept;
    };
}
