#pragma once

#include "LamaPon/Graphics/GraphicsResource.h"

#include <cstdint>
#include <memory>

namespace LamaPon
{
    namespace Detail
    {
        struct ShadowMapBackendAccess;
        struct ShadowMapBackendState;
    }

    // バックエンドが所有する影マップの公開窓口。
    class ShadowMap final
    {
    public:
        // バックエンド状態を持たない影マップを作成する。
        ShadowMap() noexcept;
        // 所有するバックエンド状態を解放する。
        ~ShadowMap() noexcept;

        // 所有状態のコピーを禁止する。
        ShadowMap(const ShadowMap&) = delete;
        // 所有状態のコピー代入を禁止する。
        ShadowMap& operator=(const ShadowMap&) = delete;
        // 公開窓口の移動を禁止する。
        ShadowMap(ShadowMap&&) = delete;
        // 公開窓口の移動代入を禁止する。
        ShadowMap& operator=(ShadowMap&&) = delete;

        // 影マップの読込ビューを取得し、未公開なら空を返す。
        [[nodiscard]] GraphicsViewHandle ViewHandle() const noexcept;
        // 影マップ一辺の解像度を取得し、未公開なら0を返す。
        [[nodiscard]] std::uint32_t Resolution() const noexcept;
        // カスケード数を取得し、未公開なら0を返す。
        [[nodiscard]] std::uint32_t CascadeCount() const noexcept;
        // 初期化・ビュー・解像度・カスケード数の有効性を判定する。
        [[nodiscard]] bool IsValid() const noexcept;

    private:
        friend struct Detail::ShadowMapBackendAccess;

        // 旧ABI用にD3D11のSRVを借用し、他の状態ならヌルを返す。
        // API55以前のモジュールが版検査へ到達できるよう、旧関数の転送先を残す。
        // GraphicsDeviceLegacyExports.defのエイリアスはRCXのthisとRAXの戻り値が一致するx64用。
        [[nodiscard]] void* LegacyNativeView() const noexcept;

        // 完成後に差し替えるバックエンド状態
        std::unique_ptr<Detail::ShadowMapBackendState> m_backendState;
    };
}
