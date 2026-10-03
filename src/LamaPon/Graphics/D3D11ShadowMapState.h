#pragma once

#include "LamaPon/Graphics/ShadowMapBackendState.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace LamaPon::Detail
{
    struct D3D11ShadowMapState final : ShadowMapBackendState
    {
        // 初期化済みで全深度ビューと読込ビューが揃うか判定する。
        [[nodiscard]] bool HasNativeResources() const noexcept
        {
            return m_initialized
                && !m_depthStencilViews.empty()
                && m_depthStencilViews.size() == m_cascadeCount
                && m_shaderResourceView != nullptr;
        }
        // 影マップのD3D11読込ビューを借用する。
        [[nodiscard]] ID3D11ShaderResourceView*
            ShaderResourceView() const noexcept;
        // 影マップ資源を作成する(device: 資源作成デバイス, resolution: 一辺の解像度, cascadeCount: カスケード数, cube: 六面キューブの有無)。
        // 未公開の新規状態で呼び、解像度は最小1、通常は1～4カスケード、キューブは6面とする。
        void Initialize(
            ID3D11Device* device,
            std::uint32_t resolution,
            std::uint32_t cascadeCount,
            bool cube);
        // 出力と最初のビューポートを退避して影深度を初期化する(context: 描画コンテキスト, cascadeIndex: 描画する配列面)。
        // 同じコンテキストでEndし、退避対象はRTV一個・DSV・ビューポート一個に限る。
        void Begin(
            ID3D11DeviceContext* context,
            std::uint32_t cascadeIndex);
        // Beginで退避した出力とビューポートを復元する(context: Beginと同じ描画コンテキスト)。
        // PSのt2～t5で解除した影SRVは復元しない。
        void End(ID3D11DeviceContext* context);

        // 影深度を持つ配列テクスチャ
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
        // 配列面ごとの深度書込ビュー
        std::vector<
            Microsoft::WRL::ComPtr<ID3D11DepthStencilView>>
            m_depthStencilViews;
        // 影マップの読込ビュー
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
            m_shaderResourceView;
        // Begin前の最初の描画ターゲット
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_savedRenderTarget;
        // Begin前の深度書込ビュー
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView>
            m_savedDepthStencil;
        // 影マップ用のビューポート
        D3D11_VIEWPORT m_viewport{};
        // Begin前の最初のビューポート
        D3D11_VIEWPORT m_savedViewport{};
        // 復元するビューポートの有無
        bool m_hasSavedViewport{};
    };
}
