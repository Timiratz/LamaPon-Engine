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
    };
}
