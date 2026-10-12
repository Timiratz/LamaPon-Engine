#pragma once

#include "LamaPon/Graphics/GraphicsDeviceApiResources.h"
#include "LamaPon/Graphics/GraphicsDeviceShaderState.h"
#include "LamaPon/Graphics/GraphicsResource.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace DirectX
{
    inline namespace DX11
    {
        class CommonStates;
        class SpriteBatch;
    }
}

namespace LamaPon
{
    class GraphicsBackend;
    class GraphicsRenderServices;
    struct TextureResourceSnapshot;

    namespace Detail
    {
        enum class D3D11SpriteBatchOwner : std::uint8_t
        {
            None,
            Legacy,
            Neutral,
            Poisoned
        };

        // 一つのD3D11世代に属する効果・キャッシュ・描画状態を所有する。
        struct GraphicsDeviceD3D11Resources final
            : GraphicsDeviceApiResources
        {
            // D3D11のバッチと共通状態を作成する(device: 資源作成デバイス, context: 描画コンテキスト)。
            GraphicsDeviceD3D11Resources(
                ID3D11Device* device,
                ID3D11DeviceContext* context);
            // ワーカーの完了を待って所有する描画資源を解放する。
            ~GraphicsDeviceD3D11Resources() override;

            // 所有するAPI資源のコピーを禁止する。
            GraphicsDeviceD3D11Resources(
                const GraphicsDeviceD3D11Resources&) = delete;
            // 所有するAPI資源のコピー代入を禁止する。
            GraphicsDeviceD3D11Resources& operator=(
                const GraphicsDeviceD3D11Resources&) = delete;

            // D3D11のAPI種別を返す。
            [[nodiscard]] RenderingApi Api() const noexcept override
            {
                return RenderingApi::DirectX11;
            }
            // 通常・スキン材質の非同期コンパイルの完了を待つ。
            void QuiesceResourceWork() noexcept override;
            // ワーカーを待ち、要求・保持参照・シェーダー・影などを解放する。
            void ResetHighLevelResources() noexcept override;
            // 高レベル資源とサービスを先に解放してネイティブ描画状態を初期化する。
            void Reset() noexcept override;
            // 描画サービスを借用し、未設定・解放後はヌルを返す。
            [[nodiscard]] GraphicsRenderServices*
                TryRenderServices() noexcept override;
            // 現在の窓口を置き換えて三種の影マップを生成する(backend: 資源作成の基盤, settings: 影の品質設定)。
            // 作成失敗時に以前の窓口は復元せず、影が無効でも空の三窓口を保持する。
            void RecreateShadowMaps(
                GraphicsBackend& backend,
                const GraphicsSettings& settings) override;
            // 方向影の窓口を借用し、未生成・解放後はヌルを返す。
            [[nodiscard]] ShadowMap*
                TryDirectionalShadowMap() const noexcept override;
            // スポット影の窓口を借用し、未生成・解放後はヌルを返す。
            [[nodiscard]] ShadowMap*
                TrySpotShadowMap() const noexcept override;
            // 点ライト影の窓口を借用し、未生成・解放後はヌルを返す。
            [[nodiscard]] ShadowMap*
                TryPointShadowMap() const noexcept override;

            // 環境描画と畳込の描画器
            mutable std::unique_ptr<EnvironmentRenderer>
                environmentRenderer;
            // 通常形状の標準照明効果
            mutable std::unique_ptr<LitEffect> litEffect;
            // スキン形状の標準照明効果
            mutable std::unique_ptr<LitEffect> skinnedLitEffect;
            // 通常材質のエラー表示効果
            mutable std::unique_ptr<LitEffect> errorEffect;
            // スキン材質のエラー表示効果
            mutable std::unique_ptr<LitEffect> skinnedErrorEffect;
            // スプライトのエラー表示効果
            mutable std::unique_ptr<SpriteEffect> spriteErrorEffect;
            // 通常材質のエラー効果作成不可
            mutable bool errorEffectUnavailable{};
            // スキン材質のエラー効果作成不可
            mutable bool skinnedErrorEffectUnavailable{};
            // スプライトのエラー効果作成不可
            mutable bool spriteErrorEffectUnavailable{};
            // 標準照明効果の作成失敗情報
            mutable GraphicsDevice::BuiltInFailure litFailure;
            // スキン照明効果の作成失敗情報
            mutable GraphicsDevice::BuiltInFailure skinnedLitFailure;
            // 環境描画器の作成失敗情報
            mutable GraphicsDevice::BuiltInFailure environmentFailure;
            // 通常材質のシェーダーキャッシュ
            mutable std::unordered_map<
                std::filesystem::path,
                std::unique_ptr<GraphicsDevice::MaterialShaderEntry>>
                materialShaders;
            // スキン材質のシェーダーキャッシュ
            mutable std::unordered_map<
                std::filesystem::path,
                std::unique_ptr<GraphicsDevice::MaterialShaderEntry>>
                skinnedMaterialShaders;
            // スプライトのシェーダーキャッシュ
            mutable std::unordered_map<
                std::filesystem::path,
                std::unique_ptr<GraphicsDevice::SpriteShaderEntry>>
                spriteShaders;
            // 画面効果のシェーダーキャッシュ
            mutable std::unordered_map<
                std::filesystem::path,
                std::unique_ptr<GraphicsDevice::ScreenShaderEntry>>
                screenShaders;
            // 実行待ちの画面効果の要求
            std::vector<GraphicsDevice::QueuedScreenEffect>
                queuedScreenEffects;
            // 計算効果のシェーダーキャッシュ
            mutable std::unordered_map<
                std::filesystem::path,
                std::unique_ptr<GraphicsDevice::ComputeShaderEntry>>
                computeShaders;
            // 方向ライト影の窓口
            std::unique_ptr<ShadowMap> shadowMap;
            // スポットライト影の窓口
            std::unique_ptr<ShadowMap> spotShadowMap;
            // 点ライト影の窓口
            std::unique_ptr<ShadowMap> pointShadowMap;

            // D3D11の共通描画サービス
            std::unique_ptr<GraphicsRenderServices> renderServices;
            // スプライトバッチの描画器
            std::unique_ptr<DirectX::SpriteBatch> spriteBatch;
            // バッチ完了まで保持する画像の世代
            std::vector<std::shared_ptr<
                const TextureResourceSnapshot>> spriteTexturePins;
            // バッチ完了まで保持するビュー
            std::vector<GraphicsViewHandle> spriteViewPins;
            // 空環境の鏡面ビュー
            GraphicsViewHandle skyPrefilteredSpecular;
            // 空環境の拡散ビュー
            GraphicsViewHandle skyPrefilteredIrradiance;
            // 空環境の鏡面最大ミップ
            float skyPrefilteredMaximumMip{};
            // DirectXTKの共通描画状態
            std::unique_ptr<DirectX::CommonStates> commonStates;
            // アルファを維持する加算状態
            mutable Microsoft::WRL::ComPtr<ID3D11BlendState>
                additiveBlendPreservingAlpha;
            // UIクリップ用のシザー状態
            Microsoft::WRL::ComPtr<ID3D11RasterizerState>
                uiScissorRasterizer;
            // UIクリップ矩形の入れ子
            std::vector<D3D11_RECT> uiScissorStack;
            // バッチの所有経路と異常状態
            D3D11SpriteBatchOwner spriteBatchOwner{
                D3D11SpriteBatchOwner::None };
            // 実行中のバッチの識別番号
            std::uint64_t spriteBatchToken{};
            // 次に発行するバッチ番号
            std::uint64_t nextSpriteBatchToken{ 1 };
            // ネイティブバッチの開始有無
            bool spriteBatchNativeBegun{};
            // バッチ中に借用する合成状態
            ID3D11BlendState* spriteBlendState{};
            // バッチ終了時のシェーダー結合処理
            std::function<void()> spriteShaderCallback;
            // 2Dメッシュの頂点シェーダー
            Microsoft::WRL::ComPtr<ID3D11VertexShader>
                spriteMeshVertexShader;
            // 独自シェーダーがないときの2Dメッシュの画素シェーダー
            Microsoft::WRL::ComPtr<ID3D11PixelShader>
                spriteMeshPixelShader;
            // 2Dメッシュの頂点入力配置
            Microsoft::WRL::ComPtr<ID3D11InputLayout>
                spriteMeshInputLayout;
            // 画素座標をクリップ座標へ移す定数
            Microsoft::WRL::ComPtr<ID3D11Buffer>
                spriteMeshViewportBuffer;
            // 2Dメッシュの動的頂点バッファ
            Microsoft::WRL::ComPtr<ID3D11Buffer>
                spriteMeshVertexBuffer;
            // 2Dメッシュの動的索引バッファ
            Microsoft::WRL::ComPtr<ID3D11Buffer>
                spriteMeshIndexBuffer;
            // 頂点バッファに入る頂点数
            UINT spriteMeshVertexCapacity{};
            // 索引バッファに入る索引数
            UINT spriteMeshIndexCapacity{};
        };

        // D3D11の描画サービスを作成する(device: 描画デバイス, context: 描画コンテキスト, backend: 借用する描画基盤)。
        [[nodiscard]] std::unique_ptr<GraphicsRenderServices>
            CreateD3D11GraphicsRenderServices(
                ID3D11Device* device,
                ID3D11DeviceContext* context,
                GraphicsBackend& backend);

        // D3D11のAPI資源とサービスを作成する(device: 資源作成デバイス, context: 描画コンテキスト, backend: 借用する描画基盤)。
        [[nodiscard]] std::unique_ptr<GraphicsDeviceApiResources>
            CreateD3D11GraphicsDeviceApiResources(
                ID3D11Device* device,
                ID3D11DeviceContext* context,
                GraphicsBackend& backend);
    }
}
