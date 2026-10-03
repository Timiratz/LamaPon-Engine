#pragma once

#include "LamaPon/Core/HttpClient.h"
#include "LamaPon/Online/OnlineServices.h"
#include "LamaPon/Online/WindowsOnlinePlatform.h"

#include <functional>
#include <memory>

namespace LamaPon::Detail
{
    // Runtime内の単体テスト専用で、通常のゲームコードはOnlineServices.hを使います。
    class OnlineServicesTestAccess final
    {
    public:
        using HttpSender =
            std::function<HttpResponse(const HttpRequest&)>;

        // テスト用の通信・保存・URL起動処理でサービスを作る(configuration: 認証設定, sender: 必須のHTTP送信処理, refreshTokenStore: 資格情報保存の所有先, authorizationLauncher: URL起動処理の所有先)。
        [[nodiscard]] static LAMAPON_API
            std::unique_ptr<OnlineServices> Create(
                OnlineServiceConfiguration configuration,
                HttpSender sender,
                std::unique_ptr<IRefreshTokenStore> refreshTokenStore = {},
                std::unique_ptr<IAuthorizationLauncher>
                    authorizationLauncher = {});

        // 結果を反映せず現在のworkerの完了状態を確認する(services: 検査するサービス)。
        [[nodiscard]] static LAMAPON_API bool CurrentTaskCompleted(
            const OnlineServices& services) noexcept;

        // 停止時間を再現するため完了時刻だけを過去へずらす(services: 操作するサービス, elapsedSeconds: 遡る秒数・0以上1年以内)。
        [[nodiscard]] static LAMAPON_API bool AgeCurrentTaskCompletion(
            OnlineServices& services,
            float elapsedSeconds) noexcept;
    };
}
