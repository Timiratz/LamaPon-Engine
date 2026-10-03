#pragma once


#include "LamaPon/Graphics/GraphicsBackend.h"

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace LamaPon
{
    namespace Detail
    {
        class D3D12ResourceDomain;
        struct ShadowMapBackendState;
    }

    class D3D12GpuProfilerBackend;

    class D3D12Backend final : public GraphicsBackend
    {
    public:
        // D3D12バックエンドを構築する。
        D3D12Backend();
        // GPU処理の終了を待ち、資源を解放してバックエンドを破棄する。
        ~D3D12Backend() override;

        // バックエンドのコピーを禁止する。
        D3D12Backend(const D3D12Backend&) = delete;
        // バックエンドのコピー代入を禁止する。
        D3D12Backend& operator=(const D3D12Backend&) = delete;

        // 使用する描画APIを返します。
        [[nodiscard]] RenderingApi Api() const noexcept override
        {
            return RenderingApi::DirectX12Experimental;
        }
        // 描画資源の初期化状態を返します。
        [[nodiscard]] bool IsInitialized() const noexcept override;

        // 描画資源を初期化します(createInfo: ウィンドウ・寸法・アダプター設定)。
        void Initialize(
            const GraphicsBackendCreateInfo& createInfo) override;
        // 記録中の命令を送信してGPU完了を待ち、破棄前の参照を外す。
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
        [[nodiscard]] std::vector<std::uint8_t> CaptureBackBuffer(
            std::uint32_t& width,
            std::uint32_t& height) const override;
        // 垂直同期なしの表示でティアリングを許可できるか返します。
        [[nodiscard]] bool TearingAllowed() const noexcept override
        {
            return m_tearingAllowed;
        }
        // バックバッファと主深度を設定し、主ビューポートと切り抜き範囲へ戻す。
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
        void BindOffscreenTarget(RenderTarget& target) override;
        // 描画先を変えず表示用画像へ確定します(target: 確定する描画先)。
        void PublishOffscreenTarget(RenderTarget& target) override;
        // 消去・復元せず深度だけを設定します(target: 深度の描画先)。
        void BindOffscreenTargetDepthOnly(RenderTarget& target) override;
        // 描画先を変えず読み取り用深度を保存します(target: 保存先)。
        void CaptureOffscreenTargetDepth(RenderTarget& target) override;
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
        void EndShadowMap(ShadowMap& shadowMap) override;
        // Forward+用のライト一覧と番号表を更新します(clusteredLights: GPU資源, lighting: ライト情報と結果, view: ビュー行列, projection: 射影行列, width: 画面幅, height: 画面高)。
        void UpdateClusteredLights(
            ClusteredLights& clusteredLights,
            LightingState& lighting,
            const DirectX::XMFLOAT4X4& view,
            const DirectX::XMFLOAT4X4& projection,
            std::uint32_t width,
            std::uint32_t height) override;
        // 主描画先の復元状態を保存します。
        [[nodiscard]] std::unique_ptr<GraphicsOutputState>
            CaptureOutputState() override;
        // 保存した主描画先を復元します(state: 同じバックエンドで保存した状態)。
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
        [[nodiscard]] GraphicsTextureHandle CreateSolidRgba8Texture(
            const std::array<std::uint8_t, 4>& color) override;
        // テクスチャを保持する読み取りビューを作ります(texture: 同じバックエンド世代の画像)。
        [[nodiscard]] GraphicsViewHandle CreateShaderResourceView(
            const GraphicsTextureHandle& texture) override;
        // 頂点バッファへ先頭から書き込みます(buffer: 更新するハンドル, data: 頂点のバイト列)。
        [[nodiscard]] bool UpdateDynamicVertexBuffer(
            GraphicsBufferHandle& buffer,
            std::span<const std::byte> data) override;
        // PerMipUpdateだけ初期データを省略できる。
        // 2D画像を生成する(description: 形式・寸法・更新方式, initialData: 全ミップ分または更新用に空)。
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
        [[nodiscard]] GraphicsViewHandle CreateShaderResourceView(
            const GraphicsTextureHandle& texture,
            const GraphicsTextureViewDescription& description) override;
        // 描画先が所有する表示ビューを世代確認して返します(target: 表示する描画先)。
        [[nodiscard]] GraphicsViewHandle CreateOffscreenDisplayView(
            const RenderTarget& target) override;
        // 頂点バッファを1本設定します(buffer: 同世代の頂点資源, slot: 入力スロット, stride: 頂点間隔のバイト数, offset: 先頭のバイト位置)。
        void BindVertexBuffer(
            const GraphicsBufferHandle& buffer,
            std::uint32_t slot,
            std::uint32_t stride,
            std::uint32_t offset) override;
        // 連続スロットへの直接設定は空の指定だけ受け付ける(firstSlot: 先頭番号, resources: 設定するビュー列, fallback: 互換用の代替ビュー)。
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

        // D3D12専用描画器へ渡すSRVの記述子と寸法。
        struct ShaderResourceBinding final
        {
            // SRVのGPU記述子
            D3D12_GPU_DESCRIPTOR_HANDLE descriptor{};
            // 最上位ミップの幅
            std::uint32_t width{};
            // 最上位ミップの高さ
            std::uint32_t height{};
            // シェーダーから見た画像次元
            D3D12_SRV_DIMENSION dimension{ D3D12_SRV_DIMENSION_TEXTURE2D };
        };

        // 記録したフレームのGPU処理が完了するまで有効な転送領域。
        struct FrameUploadAllocation final
        {
            // 借用する転送バッファ
            ID3D12Resource* resource{};
            // 転送領域の先頭バイト
            std::uint64_t offset{};
            // 転送領域のGPUアドレス
            D3D12_GPU_VIRTUAL_ADDRESS gpuAddress{};
            // CPU書き込み先のポインター
            std::byte* data{};
        };

        // 表示用バックバッファの形式
        static constexpr DXGI_FORMAT PrimaryColorFormat =
            DXGI_FORMAT_R8G8B8A8_UNORM;
        // 主描画の深度・ステンシル形式
        static constexpr DXGI_FORMAT PrimaryDepthFormat =
            DXGI_FORMAT_D24_UNORM_S8_UINT;
        // 影用の深度画像形式
        static constexpr DXGI_FORMAT ShadowDepthFormat =
            DXGI_FORMAT_D32_FLOAT;

        struct ExternalShaderResourceDescriptor final
        {
            // SRVを書き込むCPU記述子
            D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
            // SRVを参照するGPU記述子
            D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
            // 返却するSRV枠の番号
            std::uint32_t slot{};
        };

        // 所有するデバイスを初期化中だけ貸し出します。
        [[nodiscard]] ID3D12Device* Device() const noexcept
        {
            return m_device.Get();
        }
        // 描画と転送に共有する命令キューを初期化中だけ貸し出す。
        [[nodiscard]] ID3D12CommandQueue* CommandQueue() const noexcept
        {
            return m_commandQueue.Get();
        }
        // バックバッファとフレーム確保器の個数を返す。
        [[nodiscard]] std::size_t FramesInFlight() const noexcept
        {
            return BackBufferCount;
        }
        // 現在のバックバッファ番号を返す。
        [[nodiscard]] std::size_t CurrentBackBufferIndex() const noexcept
        {
            return m_currentBackBufferIndex;
        }
        // 必要なら記録を開き、現在の画像描画先またはバックバッファを設定して命令リストを貸し出す。
        [[nodiscard]] ID3D12GraphicsCommandList* BeginFrameCommands();
        // 描画先を変えず記録中の命令リストを貸し出し、未初期化・未記録なら例外を送出する。
        [[nodiscard]] ID3D12GraphicsCommandList*
            CurrentFrameCommands();
        // 記録中の命令リストを貸し出し、閉じていれば新規記録せずnullptrを返す。
        [[nodiscard]] ID3D12GraphicsCommandList*
            RecordingFrameCommands() const noexcept
        {
            return m_commandListOpen ? m_commandList.Get() : nullptr;
        }
        // 影の深度面を描画中か返す。
        [[nodiscard]] bool IsShadowPassActive() const noexcept
        {
            return m_activeShadowMap != nullptr;
        }
        // 影または画像描画先で深度だけを描画中か返す。
        [[nodiscard]] bool IsDepthOnlyPassActive() const noexcept
        {
            return m_activeShadowMap != nullptr
                || (m_activeOffscreenTarget != nullptr
                    && m_activeOffscreenDepthOnly);
        }
        // 資源世代が所有する共通SRVヒープを貸し出し、未生成ならnullptrを返す。
        [[nodiscard]] ID3D12DescriptorHeap*
            ShaderResourceDescriptorHeap() const noexcept;
        // 外部描画器用のSRV枠を確保し、使用後はReleaseExternalShaderResourceDescriptorへ返却する。
        [[nodiscard]] ExternalShaderResourceDescriptor
            AllocateExternalShaderResourceDescriptor();
        // SRV枠を返却し、GPU完了後に再利用する(slot: 同じ資源世代で確保した番号)。
        void ReleaseExternalShaderResourceDescriptor(
            std::uint32_t slot) noexcept;
        // SRVを解決し、別世代・別用途・空ならnulloptを返す(view: 解決する保持ハンドル)。
        [[nodiscard]] std::optional<ShaderResourceBinding>
            TryResolveShaderResource(
                const GraphicsViewHandle& view) const noexcept;
        // 記録中の同じフレームでCPU書き込み領域を確保する(bytes: 正のバイト数, alignment: 正の2の累乗の境界)。
        [[nodiscard]] FrameUploadAllocation AllocateFrameUpload(
            std::uint64_t bytes,
            std::uint64_t alignment);
        // 現在のビューポートを借用して返す。
        [[nodiscard]] const D3D12_VIEWPORT&
            ActiveViewport() const noexcept
        {
            return m_activeViewport;
        }
        // 現在の切り抜き範囲を借用して返す。
        [[nodiscard]] const D3D12_RECT&
            ActiveScissorRectangle() const noexcept
        {
            return m_activeScissorRect;
        }
        // 現在の色描画先の形式を返す。
        [[nodiscard]] DXGI_FORMAT ActiveColorFormat() const noexcept
        {
            return m_activeColorFormat;
        }
        // 現在の深度描画先の形式を返す。
        [[nodiscard]] DXGI_FORMAT ActiveDepthFormat() const noexcept
        {
            return m_activeDepthFormat;
        }
        // 現在色をSRVに移し、別の色バッファを描画先へ設定する(target: 同世代の描画先)。
        [[nodiscard]] GraphicsViewHandle BeginOffscreenPostProcess(
            RenderTarget& target);
        // 現在色と処理結果を交換して描画先を再設定する(target: 開始時と同じ描画先)。
        void EndOffscreenPostProcess(RenderTarget& target);
        // 色バッファを交換せず元の描画先へ戻す(target: 失敗した処理の描画先)。
        void AbortOffscreenPostProcess(RenderTarget& target) noexcept;
        // 1/4解像度から2×2平均で1画素まで縮小する段数を返す(target: 測定対象の描画先)。
        [[nodiscard]] std::uint32_t OffscreenLuminanceLevelCount(
            const RenderTarget& target) const;
        // 現在色または前段をSRVとして返し、深度なしの輝度測定段を設定する(target: 測定先, level: 0から始まる縮小段)。
        [[nodiscard]] GraphicsViewHandle BeginOffscreenLuminancePass(
            RenderTarget& target,
            std::uint32_t level);
        // 未読の輝度測定結果を捨てる(target: 測定先)。
        void DiscardOffscreenTargetLuminance(RenderTarget& target) noexcept;
        // 深度コピーまたは遮蔽画像をSRVとして返し、半解像度の遮蔽処理先を設定する(target: 深度を事前保存した描画先, blur: 遮蔽画像をぼかすか)。
        [[nodiscard]] GraphicsViewHandle BeginOffscreenAmbientOcclusionPass(
            RenderTarget& target,
            bool blur);
        // 遮蔽画像を読み取り状態へ戻し、深度専用の描画先を設定する(target: 遮蔽処理の描画先)。
        void EndOffscreenAmbientOcclusion(RenderTarget& target);
        // 例外を外へ出さず遮蔽処理の状態を復元する(target: 失敗した処理の描画先)。
        void AbortOffscreenAmbientOcclusion(RenderTarget& target) noexcept;
        // 1/4解像度の光条処理先と入力SRVを返す(target: 処理対象, pass: 0は現在色、1・2は中間画像を交互に使用)。
        [[nodiscard]] GraphicsViewHandle BeginOffscreenLensFlareStreakPass(
            RenderTarget& target,
            std::uint32_t pass);
        // 光条画像を返し、通常の描画先へ戻す(target: 3パスを実行した描画先)。
        [[nodiscard]] GraphicsViewHandle EndOffscreenLensFlareStreaks(
            RenderTarget& target);
        // 例外を外へ出さず光条処理の描画先を戻す(target: 失敗した処理の描画先)。
        void AbortOffscreenLensFlareStreaks(RenderTarget& target) noexcept;
        // 深度コピーまたは前段をSRVとして返し、Hi-Zの出力段を設定する(target: 深度を事前保存した描画先, mip: 0から始まるミップ番号)。
        [[nodiscard]] GraphicsViewHandle BeginOffscreenReflectionDepthPass(
            RenderTarget& target,
            std::uint32_t mip);
        // 全ミップを読み取り状態へ戻し、描画先を設定する(target: Hi-Z処理の描画先, depthOnly: 深度だけの描画へ戻すか)。
        void EndOffscreenReflectionDepthPyramid(
            RenderTarget& target,
            bool depthOnly);
        // 例外を外へ出さずHi-Z処理の状態を復元する(target: 失敗した処理の描画先, depthOnly: 深度だけの描画へ戻すか)。
        void AbortOffscreenReflectionDepthPyramid(
            RenderTarget& target,
            bool depthOnly) noexcept;
        // 指定画像が深度専用の描画先として設定中か返す(target: 確認する描画先)。
        [[nodiscard]] bool IsOffscreenTargetBoundDepthOnly(
            const RenderTarget& target) const noexcept;
        // Compute Shaderの出力UAVと入力SRVの割り当て。
        struct ComputeBindings final
        {
            // 出力UAVのGPU記述子
            D3D12_GPU_DESCRIPTOR_HANDLE output{};
            // 入力2枠のSRV記述子
            std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 2> inputs{};
            // 出力画像の幅
            std::uint32_t width{};
            // 出力画像の高さ
            std::uint32_t height{};
        };
        // 表示画像をUAV、入力をCompute用SRVへ移す(target: 書き込み可能な同世代の出力, inputs: 出力と異なる同世代の画像2枠)。
        [[nodiscard]] ComputeBindings BeginOffscreenCompute(
            RenderTarget& target,
            const std::array<GraphicsViewHandle, 2>& inputs);
        // 入力と出力をピクセルシェーダー用の読み取り状態へ戻す(target: 開始時の出力, inputs: 開始時の入力2枠)。
        void EndOffscreenCompute(
            RenderTarget& target,
            const std::array<GraphicsViewHandle, 2>& inputs) noexcept;
        // 対応次元は2D・2D配列・キューブ・3D・バッファ。
        // 読み取りがゼロになる空SRVを返す(dimension: シェーダーから見た資源次元)。
        [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE
            NullShaderResourceDescriptor(D3D12_SRV_DIMENSION dimension);
        // 不変の6面画像とキューブSRVを作る(faceDescription: 正方形の面設定, subresources: 面ごとに全ミップを並べたデータ)。
        [[nodiscard]] std::pair<GraphicsTextureHandle, GraphicsViewHandle>
            CreateTextureCube(
                const GraphicsTexture2DDescription& faceDescription,
                std::span<const GraphicsTextureSubresourceData>
                    subresources);
        // 不変の画像配列とSRVを作る(description: 面の設定, arraySize: 通常配列は面数、キューブ配列は個数, cubeArray: キューブ配列の指定, subresources: 面ごとに全ミップを並べたデータ)。
        [[nodiscard]] std::pair<GraphicsTextureHandle, GraphicsViewHandle>
            CreateTextureArray(
                const GraphicsTexture2DDescription& description,
                std::uint32_t arraySize,
                bool cubeArray,
                std::span<const GraphicsTextureSubresourceData>
                    subresources);
        // ミップごとに6面のUAVへ書き、キューブSRVとして読む事前畳み込み先。
        struct ComputeCubeTarget final
        {
            // 出力キューブの保持ハンドル
            GraphicsTextureHandle texture;
            // キューブSRVの保持ハンドル
            GraphicsViewHandle view;
            // 各ミップの6面UAV記述子
            std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> mipAccess;
        };
        // RGBA16Fのキューブ画像と各段の6面UAVを作る(size: 正の一辺の画素数, mipLevels: ミップ段数)。
        [[nodiscard]] ComputeCubeTarget CreateComputeCubeTarget(
            std::uint32_t size,
            std::uint32_t mipLevels);
        // 入力をCompute読み取り、別のキューブ出力をUAVへ移す(source: 同世代のキューブまたは2Dビュー, target: 同世代の出力画像)。
        [[nodiscard]] ShaderResourceBinding BeginCubeCompute(
            const GraphicsViewHandle& source,
            const GraphicsTextureHandle& target);
        // 入力とキューブ出力をピクセルシェーダー用の状態へ戻す(source: 開始時の入力, target: 開始時の出力)。
        void EndCubeCompute(
            const GraphicsViewHandle& source,
            const GraphicsTextureHandle& target) noexcept;

    private:
        // 同時に進行するフレーム数
        static constexpr std::size_t BackBufferCount = 2;

        struct FrameUploadChunk final
        {
            // 保持する転送バッファ
            Microsoft::WRL::ComPtr<ID3D12Resource> resource;
            // CPU書き込み先のポインター
            std::byte* data{};
            // バッファ容量のバイト数
            std::uint64_t capacity{};
            // 割り当て済みのバイト数
            std::uint64_t used{};
        };

        // 表示用の色・深度資源を生成します(width: 描画幅, height: 描画高)。
        void CreateSizeDependentResources(
            std::uint32_t width,
            std::uint32_t height);
        // バックバッファ・主深度・描画先の状態を解放する。
        void ReleaseSizeDependentResources() noexcept;
        // ワーカー転送用の命令確保器・命令リスト・完了フェンスを作る。
        void CreateUploadContext();
        // ワーカー転送用の命令資源と完了イベントを解放する。
        void ReleaseUploadContext() noexcept;
        // 対応フレームのGPU完了を待ち、確保器と転送領域を再利用して記録を開く。
        void OpenCommandList();
        // 現在の表示バッファに必要な状態遷移を記録する(state: 遷移先の資源状態)。
        void TransitionCurrentBackBuffer(
            D3D12_RESOURCE_STATES state);
        // 現在の表示バッファと主深度を設定し、主ビューポートと切り抜き範囲へ戻す。
        void BindPrimaryOutput();
        // 記録中なら計測を確定し、命令リストを閉じてキューへ送信する。
        void CloseAndExecuteOpenCommands();
        // 次の主描画完了値を予約し、その後に解放される資源の待機値を更新する。
        [[nodiscard]] std::uint64_t ReserveFrameFenceValue() noexcept;
        // 主描画キューの完了値を発行し、現在のフレームの再利用待機値として保存する。
        [[nodiscard]] std::uint64_t SignalCurrentBackBuffer();
        // 指定した主描画処理の完了を待つ(value: 主描画フェンスの完了値)。
        void WaitForFence(std::uint64_t value);
        // 主描画キューへ完了値を発行し、それまでの全命令の終了を待つ。
        void WaitForGpu();
        // 記録中の命令を送信して全処理のGPU完了を待つ。
        void DrainGpu();
        // GPU完了を確認できる退避資源と記述子を解放する。
        void CollectRetiredResources() noexcept;
        // GPU完了後にフレームの転送領域を再利用可能にする(index: 完了済みフレームの番号)。
        void ResetFrameUploadArena(std::size_t index) noexcept;
        // 独立した命令資源で同期転送し、ワーカーからの転送を排他する(texture: 転送先の画像, firstMipLevel: 先頭の副資源番号, data: 連続する副資源のデータ, updateExistingTexture: 既存画像の更新指定)。
        void SubmitTextureUpload(
            const Microsoft::WRL::ComPtr<ID3D12Resource>& texture,
            std::uint32_t firstMipLevel,
            std::span<const GraphicsTextureSubresourceData> data,
            bool updateExistingTexture);
        // 表示画像をRGBA8へ同期読み戻しする(width: 画像幅の出力, height: 画像高の出力)。
        [[nodiscard]] std::vector<std::uint8_t>
            CaptureBackBufferImpl(
                std::uint32_t& width,
                std::uint32_t& height);
        // 保持するDXGIファクトリー
        Microsoft::WRL::ComPtr<IDXGIFactory4> m_factory;
        // 選択したGPUアダプター
        Microsoft::WRL::ComPtr<IDXGIAdapter1> m_adapter;
        // 保持するD3D12デバイス
        Microsoft::WRL::ComPtr<ID3D12Device> m_device;
        // 描画と転送の命令キュー
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;
        // 表示用のスワップチェーン
        Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
        // バックバッファのRTV領域
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
        // 主深度のDSV領域
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
        // 表示用の色バッファ配列
        std::array<
            Microsoft::WRL::ComPtr<ID3D12Resource>,
            BackBufferCount> m_backBuffers;
        // 主描画の深度バッファ
        Microsoft::WRL::ComPtr<ID3D12Resource> m_depthBuffer;
        // フレーム別の命令確保器
        std::array<
            Microsoft::WRL::ComPtr<ID3D12CommandAllocator>,
            BackBufferCount> m_commandAllocators;
        // 主描画の命令リスト
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
        // 主描画のGPU完了フェンス
        Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
        // D3D12検証メッセージ
        Microsoft::WRL::ComPtr<ID3D12InfoQueue> m_infoQueue;
        // 主描画の完了待機イベント
        HANDLE m_fenceEvent{};
        // 各フレームのGPU完了値
        std::array<std::uint64_t, BackBufferCount> m_frameFenceValues{};
        // 各表示バッファの資源状態
        std::array<D3D12_RESOURCE_STATES, BackBufferCount>
            m_backBufferStates{
                D3D12_RESOURCE_STATE_PRESENT,
                D3D12_RESOURCE_STATE_PRESENT };
        // 次に発行する主描画完了値
        std::uint64_t m_nextFenceValue{ 1 };
        // 現在の表示バッファ番号
        std::uint32_t m_currentBackBufferIndex{};
        // RTV記述子のバイト幅
        std::uint32_t m_rtvDescriptorSize{};
        // バックバッファの幅
        std::uint32_t m_width{};
        // バックバッファの高さ
        std::uint32_t m_height{};
        // 主描画先のビューポート
        D3D12_VIEWPORT m_viewport{};
        // 主描画先の切り抜き範囲
        D3D12_RECT m_scissorRect{};
        // 現在のビューポート
        D3D12_VIEWPORT m_activeViewport{};
        // 現在の切り抜き範囲
        D3D12_RECT m_activeScissorRect{};
        // 現在の描画先の色形式
        DXGI_FORMAT m_activeColorFormat{ PrimaryColorFormat };
        // 現在の描画先の深度形式
        DXGI_FORMAT m_activeDepthFormat{ PrimaryDepthFormat };
        // 同期なしの表示を許可するか
        bool m_tearingAllowed{};
        // 主命令リストが記録中か
        bool m_commandListOpen{};
        // 借用する現在の画像描画先
        RenderTarget* m_activeOffscreenTarget{};
        // 画像描画先で深度だけ描くか
        bool m_activeOffscreenDepthOnly{};
        // 借用する現在の影描画先
        Detail::ShadowMapBackendState* m_activeShadowMap{};

        // ワーカーと共有する復旧不能状態
        std::atomic_bool m_terminalFailure{ false };
        // 同期に失敗した読み戻し資源は、GPUが参照し得るためShutdownまで保持する。
        // 同期失敗後に保持する読み戻し
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>
            m_retainedSubmissionResources;

        // ハンドルの最終解放後も、資源と記述子をGPU処理の完了まで保持する。
        // 資源と記述子を管理する世代
        std::shared_ptr<Detail::D3D12ResourceDomain> m_resourceDomain;
        // 空のSRV枠は資源世代と寿命を揃え、Shutdownで解除する。
        // 次元別の空SRV枠
        std::array<std::optional<std::uint32_t>, 5>
            m_nullShaderResourceSlots{};
        // 転送領域は対応するフレームのGPU処理完了後に再利用する。
        // フレーム別の転送領域
        std::array<std::vector<FrameUploadChunk>, BackBufferCount>
            m_frameUploadArenas;
        // ワーカー転送の排他制御
        std::mutex m_uploadMutex;
        // 転送専用の命令確保器
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>
            m_uploadCommandAllocator;
        // 転送専用の命令リスト
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>
            m_uploadCommandList;
        // 転送専用のGPU完了フェンス
        Microsoft::WRL::ComPtr<ID3D12Fence> m_uploadFence;
        // 転送の完了待機イベント
        HANDLE m_uploadFenceEvent{};
        // 次に発行する転送完了値
        std::uint64_t m_nextUploadFenceValue{ 1 };
        // 同期失敗後に保持する転送資源
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>
            m_retainedUploadResources;
        // 所有するGPU計測器
        std::unique_ptr<D3D12GpuProfilerBackend> m_gpuProfilerBackend;
    };
}
