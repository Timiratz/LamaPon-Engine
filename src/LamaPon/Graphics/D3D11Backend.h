#pragma once

#include "LamaPon/Graphics/GraphicsBackend.h"

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>

#include <cstdint>
#include <utility>

namespace LamaPon
{
    class D3D11Backend final : public GraphicsBackend
    {
    public:
        // D3D11バックエンドを構築します。
        D3D11Backend();
        // 描画資源を解放してバックエンドを破棄します。
        ~D3D11Backend() override;

        // バックエンドのコピーを禁止します。
        D3D11Backend(const D3D11Backend&) = delete;
        // バックエンドのコピー代入を禁止します。
        D3D11Backend& operator=(
            const D3D11Backend&) = delete;

        // 使用する描画APIを返します。
        [[nodiscard]] RenderingApi
            Api() const noexcept override
        {
            return RenderingApi::DirectX11;
        }
        // 描画資源の初期化状態を返します。
        [[nodiscard]] bool
            IsInitialized() const noexcept override
        {
            return m_device != nullptr;
        }

        // 描画資源を初期化します(createInfo: ウィンドウ・寸法・アダプター設定)。
        void Initialize(
            const GraphicsBackendCreateInfo& createInfo) override;
        // 所有GPU資源の破棄前に参照を外し、残る命令を送信します。
        void PrepareForResourceRelease() noexcept override;
        // GPU資源と描画状態を解放します。
        void Shutdown() noexcept override;
        // バックバッファを再作成します(width: 描画幅, height: 描画高)。
        void Resize(
            std::uint32_t width,
            std::uint32_t height) override;
        // バックバッファを設定して消去します(clearColor: RGBAの消去色)。
        void BindAndClearBackBuffer(
            const float clearColor[4]) override;
        // 描画APIの検証メッセージを回収します。
        void DrainDebugMessages() override;
        // 描画結果を表示します(vSyncEnabled: 垂直同期の有効化)。
        void Present(bool vSyncEnabled) override;
        // 表示画像をCPUへ読み戻します(width: 画像幅の出力, height: 画像高の出力)。
        [[nodiscard]] std::vector<std::uint8_t>
            CaptureBackBuffer(
                std::uint32_t& width,
                std::uint32_t& height) const override;
        // 垂直同期なしの表示でティアリングを許可できるか返します。
        [[nodiscard]] bool
            TearingAllowed() const noexcept override
        {
            return m_tearingAllowed;
        }
        // 初期化済みバックバッファとビューポートを設定し、深度を外します。
        void BindBackBuffer() override;
        // 描画先資源を再作成します(target: 更新する描画先, width: 画像幅, height: 画像高)。
        void ResizeOffscreenTarget(
            RenderTarget& target,
            std::uint32_t width,
            std::uint32_t height) override;
        // 描画先を設定して色と深度を消去します(target: 描画先, clearColor: RGBAの消去色)。
        void BeginOffscreenTarget(
            RenderTarget& target,
            const float clearColor[4]) override;
        // 画像を消さず描画先を設定します(target: 描画先)。
        void BindOffscreenTarget(
            RenderTarget& target) override;
        // 描画先を変えず表示用画像へ確定します(target: 確定する描画先)。
        void PublishOffscreenTarget(
            RenderTarget& target) override;
        // 消去・復元せず深度だけを設定します(target: 深度の描画先)。
        void BindOffscreenTargetDepthOnly(
            RenderTarget& target) override;
        // 描画先を変えず読み取り用深度を保存します(target: 保存先)。
        void CaptureOffscreenTargetDepth(
            RenderTarget& target) override;
        // SSR用HDR履歴を保存します(target: 保存先, viewProjection: 画像描画時の行列)。
        void CaptureOffscreenTargetColorHistory(
            RenderTarget& target,
            const DirectX::XMFLOAT4X4& viewProjection) override;
        // TAA用履歴を保存します(target: 保存先, viewProjection: ジッターなしの再投影行列)。
        void CaptureOffscreenTargetTemporalHistory(
            RenderTarget& target,
            const DirectX::XMFLOAT4X4& viewProjection) override;
        // 待機せず線形平均輝度を返し、未完了ならnulloptです(target: 測定結果の描画先)。
        [[nodiscard]] std::optional<float>
            TryReadOffscreenTargetLuminance(
                RenderTarget& target) override;
        // 輝度測定結果を非同期読み戻し用に保存します(target: 測定結果の描画先)。
        void CaptureOffscreenTargetLuminance(
            RenderTarget& target) override;
        // 影の深度配列を作ります(shadowMap: 作成先, resolution: 一辺の画素数, cascadeCount: 配列面数, cube: 6面のキューブ指定)。
        void InitializeShadowMap(
            ShadowMap& shadowMap,
            std::uint32_t resolution,
            std::uint32_t cascadeCount,
            bool cube) override;
        // 影の面を設定して深度を消去します(shadowMap: 影の描画先, cascadeIndex: 描画面の番号)。
        void BeginShadowMap(
            ShadowMap& shadowMap,
            std::uint32_t cascadeIndex) override;
        // 影描画前の描画先とビューポートへ戻します(shadowMap: 終了する影の描画先)。
        void EndShadowMap(
            ShadowMap& shadowMap) override;
        // Forward+用のライト一覧と番号表を更新します(clusteredLights: GPU資源, lighting: ライト情報と結果, view: ビュー行列, projection: 射影行列, width: 画面幅, height: 画面高)。
        void UpdateClusteredLights(
            ClusteredLights& clusteredLights,
            LightingState& lighting,
            const DirectX::XMFLOAT4X4& view,
            const DirectX::XMFLOAT4X4& projection,
            std::uint32_t width,
            std::uint32_t height) override;
        // 出力スロット0、深度出力、先頭のビューポートを保持して保存します。
        [[nodiscard]] std::unique_ptr<GraphicsOutputState>
            CaptureOutputState() override;
        // 保存した出力と取得済みのビューポートを復元します(state: 同じデバイスで保存した状態)。
        void RestoreOutputState(
            const GraphicsOutputState& state) override;
        // アダプター容量とOS予算を返し、取得失敗は各Availableで示します。
        [[nodiscard]] GraphicsVideoMemoryStatistics
            QueryVideoMemoryStatistics() const noexcept override;
        // 線分描画器を生成し、初期化済みバックエンドより先に破棄します。
        [[nodiscard]] std::unique_ptr<DebugDrawingBackend>
            CreateDebugDrawingBackend() override;
        // 初期化中だけ所有する計測器を貸し出し、未初期化ならnullptrです。
        [[nodiscard]] GpuProfilerBackend*
            ProfilerBackend() noexcept override;
        // 単色のRGBA8テクスチャを作ります(color: 各成分0～255のRGBA)。
        [[nodiscard]] GraphicsTextureHandle
            CreateSolidRgba8Texture(
                const std::array<std::uint8_t, 4>& color) override;
        // テクスチャを保持する読み取りビューを作ります(texture: 同じバックエンド世代の画像)。
        [[nodiscard]] GraphicsViewHandle
            CreateShaderResourceView(
                const GraphicsTextureHandle& texture) override;
        // 頂点バッファへ先頭から書き込み、空データやGPU処理失敗はfalseです(buffer: 更新するハンドル, data: 頂点のバイト列)。
        [[nodiscard]] bool UpdateDynamicVertexBuffer(
            GraphicsBufferHandle& buffer,
            std::span<const std::byte> data) override;
        // 2D画像を生成します(description: 形式・寸法・更新方式, initialData: 全ミップの初期値)。
        [[nodiscard]] GraphicsTextureHandle CreateTexture2D(
            const GraphicsTexture2DDescription& description,
            std::span<const GraphicsTextureSubresourceData>
                initialData) override;
        // ミップ画像を更新します(texture: 更新可能な同世代の画像, mipLevel: ミップ番号, data: 画像データと行ピッチ)。
        void UpdateTexture2D(
            const GraphicsTextureHandle& texture,
            std::uint32_t mipLevel,
            const GraphicsTextureSubresourceData& data) override;
        // 指定範囲の読み取りビューを作ります(texture: 同じバックエンド世代の画像, description: 形式とミップ範囲)。
        [[nodiscard]] GraphicsViewHandle
            CreateShaderResourceView(
                const GraphicsTextureHandle& texture,
                const GraphicsTextureViewDescription& description) override;
        // 描画先が所有する表示ビューを世代確認して返します(target: 表示する描画先)。
        [[nodiscard]] GraphicsViewHandle
            CreateOffscreenDisplayView(
                const RenderTarget& target) override;
        // 頂点バッファを1本設定します(buffer: 同世代の頂点資源, slot: 入力スロット, stride: 頂点間隔のバイト数, offset: 先頭のバイト位置)。
        void BindVertexBuffer(
            const GraphicsBufferHandle& buffer,
            std::uint32_t slot,
            std::uint32_t stride,
            std::uint32_t offset) override;
        // 無効な要素は代替SRVへ置換してPSへ設定します(firstSlot: 先頭番号, resources: 設定するビュー列, fallback: 無効要素の代替ビュー)。
        // 代替SRVも無効ならnullptrを設定し、範囲・初期化不正だけは何も設定せずfalseを返します。
        [[nodiscard]] bool TryBindPixelShaderResources(
            std::uint32_t firstSlot,
            std::span<const GraphicsViewHandle> resources,
            const GraphicsViewHandle& fallback) noexcept override;
        // 3D画像の全ミップを生成します(description: 形式と寸法, initialData: 全ミップの体積データ)。
        [[nodiscard]] GraphicsTextureHandle CreateTexture3D(
            const GraphicsTexture3DDescription& description,
            std::span<const GraphicsTextureSubresourceData>
                initialData) override;
        // 現在の資源世代に属するかだけを返します(view: 確認するビュー)。
        [[nodiscard]] bool IsViewCurrent(
            const GraphicsViewHandle& view) const noexcept override;
        // Forward+資源を全て完成後に公開します(clusteredLights: 作成先, assets: シェーダーの取得元, shaderPath: シェーダーのパス)。
        void InitializeClusteredLights(
            ClusteredLights& clusteredLights,
            AssetManager& assets,
            const std::filesystem::path& shaderPath) override;

        // 所有するデバイスを初期化中だけ貸し出します。
        [[nodiscard]] ID3D11Device*
            Device() const noexcept
        {
            return m_device.Get();
        }
        // 所有する即時コンテキストを初期化中だけ貸し出します。
        [[nodiscard]] ID3D11DeviceContext*
            Context() const noexcept
        {
            return m_context.Get();
        }
        // SRVを借用し、空ならnullptrを返します(view: 同じバックエンド世代の読み取りビュー)。
        [[nodiscard]] ID3D11ShaderResourceView*
            ResolveShaderResourceView(
                const GraphicsViewHandle& view) const;
        // 頂点バッファを借用し、空ならnullptrを返します(buffer: 同じバックエンド世代のバッファ)。
        [[nodiscard]] ID3D11Buffer* ResolveBuffer(
            const GraphicsBufferHandle& buffer) const;
        // 2D・キューブ・3D画像を保持して取り込みます(view: 所有権を移さない同デバイスSRV)。
        [[nodiscard]] std::pair<
            GraphicsTextureHandle,
            GraphicsViewHandle> ImportShaderResourceView(
                ID3D11ShaderResourceView* view);
        // 同デバイスのSRVを保持して取り込みます(view: 画像またはバッファのSRV)。
        [[nodiscard]] GraphicsViewHandle
            ImportShaderResourceViewHandle(
                ID3D11ShaderResourceView* view);

    private:
        // 表示用の色・深度資源を生成します(width: 描画幅, height: 描画高)。
        void CreateSizeDependentResources(
            std::uint32_t width,
            std::uint32_t height);
        // 選択したアダプターの情報をログへ記録します。
        void LogSelectedAdapter() const;
        // D3D11計測器を作ります(device: 計測器が保持するデバイス, context: 計測器が保持する即時コンテキスト)。
        [[nodiscard]] static std::unique_ptr<GpuProfilerBackend>
            CreateProfilerBackend(
                ID3D11Device* device,
                ID3D11DeviceContext* context);

        // 所有するD3D11デバイス
        Microsoft::WRL::ComPtr<ID3D11Device> m_device;
        // 即時命令のコンテキスト
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
        // 表示用のスワップチェーン
        Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
        // バックバッファの出力ビュー
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView>
            m_renderTargetView;
        // バックバッファの深度画像
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthTexture;
        // バックバッファの深度ビュー
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView>
            m_depthStencilView;
        // バックバッファの描画範囲
        D3D11_VIEWPORT m_viewport{};
        // ティアリングの許可可否
        bool m_tearingAllowed{};
        // 現在の資源世代と寿命管理
        std::shared_ptr<Detail::GraphicsResourceDomain>
            m_resourceDomain;
        // 検証メッセージの取得元
        Microsoft::WRL::ComPtr<ID3D11InfoQueue> m_infoQueue;
        // 回収済み検証メッセージ数
        std::uint64_t m_debugMessagesLogged{};
        // DeviceとContextより先に破棄する計測器
        std::unique_ptr<GpuProfilerBackend> m_gpuProfilerBackend;
    };
}
