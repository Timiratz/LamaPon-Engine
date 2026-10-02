#pragma once

#include "LamaPon/Online/NetworkRoomDirectory.h"
#include <memory>

namespace LamaPon
{
    enum class NetworkDiscoveryScope { Lan, SameComputer };
    // LAN検索はローカルUDPだけを使い、インターネットのサービスへ問い合わせません。
    class NetworkRoomBrowser final
    {
    public:
        // LAN検索の状態を作る。
        LAMAPON_API NetworkRoomBrowser();
        // 検索ソケットを閉じて状態を破棄する。
        LAMAPON_API ~NetworkRoomBrowser();
        // 検索状態の複製を禁止する。
        NetworkRoomBrowser(const NetworkRoomBrowser&) = delete;
        // 検索状態のコピー代入を禁止する。
        NetworkRoomBrowser& operator=(const NetworkRoomBrowser&) = delete;
        // 検索を開始し、ソケットの起動結果を返す(configuration: 通信設定, scope: 検索範囲)。
        LAMAPON_API bool Start(NetworkConfiguration configuration,
            NetworkDiscoveryScope scope = NetworkDiscoveryScope::Lan);
        // 起動中の検索要求を再送する。
        LAMAPON_API void Refresh();
        // 広告を最大64件処理して4秒を超えて途絶えた結果を除く(seconds: 経過秒数)。
        LAMAPON_API void Update(float seconds);
        // 検索を止め、結果とエラーを消す。
        LAMAPON_API void Stop();
        // 検索ソケットが起動中かを返す。
        [[nodiscard]] LAMAPON_API bool IsSearching() const noexcept;
        // 検索結果を借用し、参照先の要素は更新や停止で無効になり得る。
        [[nodiscard]] LAMAPON_API const std::vector<NetworkRoom>& Rooms() const noexcept;
        // 直近のエラーを借用し、内容は更新や停止で変わる。
        [[nodiscard]] LAMAPON_API const std::string& LastError() const noexcept;
    private:
        struct Implementation;
        // 検索状態の所有先
        std::unique_ptr<Implementation> m_impl;
    };
}
