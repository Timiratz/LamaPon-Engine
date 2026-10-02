#pragma once

#include "LamaPon/Graphics/GraphicsQuality.h"
#include "LamaPon/Graphics/GraphicsResource.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace DirectX
{
    struct XMFLOAT4X4;
}

namespace LamaPon
{
    class AssetManager;
    class ClusteredLights;
    class DebugDrawingBackend;
    class GpuProfilerBackend;
    class RenderTarget;
    class ShadowMap;
    struct LightingState;

    enum class RenderingApiFallbackReason
    {
        None,
        // 旧モジュールとの列挙値互換を維持します。
        NotImplemented,
        Unsupported,
        InitializationFailed,
        UnknownApi
    };

    // 互換用の起動指定で、現在は選択したAPIだけで起動します。
    enum class GraphicsStartupProfile : std::uint8_t
    {
        FullRenderer,
        AllowD3D12ExperimentalRenderer,
        // 旧モジュール用の別名です。
        AllowD3D12ExperimentalBootstrap =
            AllowD3D12ExperimentalRenderer
    };

    struct GraphicsBackendSelection final
    {
        // 要求した描画API
        RenderingApi requestedApi{
            RenderingApi::DirectX11 };
        // 使用する描画API
        RenderingApi activeApi{
            RenderingApi::DirectX11 };
        // 代替APIの選択理由
        RenderingApiFallbackReason fallbackReason{
            RenderingApiFallbackReason::None };
    };

    // 起動APIを選びます(requestedApi: 要求APIでAutoはD3D11)。
    [[nodiscard]] GraphicsBackendSelection
        SelectGraphicsBackend(
            RenderingApi requestedApi) noexcept;
    // 起動APIを選びます(requestedApi: 要求APIでAutoはD3D11, profile: 選択に使わない互換指定)。
    [[nodiscard]] GraphicsBackendSelection
        SelectGraphicsBackend(
            RenderingApi requestedApi,
            GraphicsStartupProfile profile) noexcept;

    struct GraphicsBackendCreateInfo final
    {
        // 描画先のHWND
        void* nativeWindow{};
        // 描画幅（ピクセル）
        std::uint32_t width{};
        // 描画高（ピクセル）
        std::uint32_t height{};
        // ソフト描画優先
        bool preferWarpAdapter{};
        // 検証レイヤーの有効化
        bool enableDebugLayer{};
    };

    // 容量・使用量・予算はバイト単位で、取得可否は各Availableで判定します。
    struct GraphicsVideoMemoryStatistics final
    {
        // 専用メモリ容量
        std::uint64_t dedicatedBytes{};
        // 共有メモリ容量
        std::uint64_t sharedSystemBytes{};
        // ローカル使用量
        std::uint64_t localUsageBytes{};
        // ローカル予算
        std::uint64_t localBudgetBytes{};
        // 非ローカル使用量
        std::uint64_t nonLocalUsageBytes{};
        // 非ローカル予算
        std::uint64_t nonLocalBudgetBytes{};
        // アダプター取得可否
        bool adapterAvailable{};
        // 容量情報の取得可否
        bool descriptionAvailable{};
        // ローカル予算の取得可否
        bool localBudgetAvailable{};
        // 非ローカル予算の取得可否
        bool nonLocalBudgetAvailable{};
    };

    // 主描画先を復元する状態で、パイプライン全体は保存しません。
    class GraphicsOutputState
    {
    public:
        // 保存した描画先状態を破棄します。
        virtual ~GraphicsOutputState() = default;

        // 状態のコピーを禁止します。
        GraphicsOutputState(const GraphicsOutputState&) = delete;
        // 状態のコピー代入を禁止します。
        GraphicsOutputState& operator=(
            const GraphicsOutputState&) = delete;

    protected:
        // 空の描画先状態を作ります。
        GraphicsOutputState() = default;
    };

    // 描画APIの共通契約で、既存モジュールとの互換のため仮想関数の順序を維持します。
    class GraphicsBackend
    {
    public:
        // バックエンドを破棄します。
        virtual ~GraphicsBackend() = default;

        // バックエンドの基底を構築します。
        GraphicsBackend() = default;
        // バックエンドのコピーを禁止します。
        GraphicsBackend(const GraphicsBackend&) = delete;
        // バックエンドのコピー代入を禁止します。
        GraphicsBackend& operator=(
            const GraphicsBackend&) = delete;

        // 使用する描画APIを返します。
        [[nodiscard]] virtual RenderingApi
            Api() const noexcept = 0;
        // 描画資源の初期化状態を返します。
        [[nodiscard]] virtual bool
            IsInitialized() const noexcept = 0;

        // 描画資源を初期化します(createInfo: ウィンドウ・寸法・アダプター設定)。
        virtual void Initialize(
            const GraphicsBackendCreateInfo& createInfo) = 0;

        // 所有GPU資源の破棄前に参照を外し、残る命令を送信します。
        virtual void PrepareForResourceRelease() noexcept = 0;
        // GPU資源と描画状態を解放します。
        virtual void Shutdown() noexcept = 0;

        // バックバッファを再作成します(width: 描画幅, height: 描画高)。
        virtual void Resize(
            std::uint32_t width,
            std::uint32_t height) = 0;
        // バックバッファを設定して消去します(clearColor: RGBAの消去色)。
        virtual void BindAndClearBackBuffer(
            const float clearColor[4]) = 0;
        // 描画APIの検証メッセージを回収します。
        virtual void DrainDebugMessages() = 0;
        // 描画結果を表示します(vSyncEnabled: 垂直同期の有効化)。
        virtual void Present(bool vSyncEnabled) = 0;

        // 表示画像をCPUへ読み戻します(width: 画像幅の出力, height: 画像高の出力)。
        [[nodiscard]] virtual std::vector<std::uint8_t>
            CaptureBackBuffer(
                std::uint32_t& width,
                std::uint32_t& height) const = 0;
        // 垂直同期なしの表示でティアリングを許可できるか返します。
        [[nodiscard]] virtual bool
            TearingAllowed() const noexcept = 0;

        // 初期化済みバックバッファとビューポートを設定し、深度を外します。
        virtual void BindBackBuffer() = 0;

        // 描画先資源を再作成します(target: 更新する描画先, width: 画像幅, height: 画像高)。
        virtual void ResizeOffscreenTarget(
            RenderTarget& target,
            std::uint32_t width,
            std::uint32_t height) = 0;
        // 描画先を設定して色と深度を消去します(target: 描画先, clearColor: RGBAの消去色)。
        virtual void BeginOffscreenTarget(
            RenderTarget& target,
            const float clearColor[4]) = 0;
        // 画像を消さず描画先を設定します(target: 描画先)。
        virtual void BindOffscreenTarget(
            RenderTarget& target) = 0;
        // 描画先を変えず表示用画像へ確定します(target: 確定する描画先)。
        virtual void PublishOffscreenTarget(
            RenderTarget& target) = 0;
        // 消去・復元せず深度だけを設定します(target: 深度の描画先)。
        virtual void BindOffscreenTargetDepthOnly(
            RenderTarget& target) = 0;
        // 描画先を変えず読み取り用深度を保存します(target: 保存先)。
        virtual void CaptureOffscreenTargetDepth(
            RenderTarget& target) = 0;
        // SSR用HDR履歴を保存します(target: 保存先, viewProjection: 画像描画時の行列)。
        virtual void CaptureOffscreenTargetColorHistory(
            RenderTarget& target,
            const DirectX::XMFLOAT4X4& viewProjection) = 0;
        // TAA用履歴を保存します(target: 保存先, viewProjection: ジッターなしの再投影行列)。
        virtual void CaptureOffscreenTargetTemporalHistory(
            RenderTarget& target,
            const DirectX::XMFLOAT4X4& viewProjection) = 0;
        // 待機せず線形平均輝度を返し、未完了ならnulloptです(target: 測定結果の描画先)。
        [[nodiscard]] virtual std::optional<float>
            TryReadOffscreenTargetLuminance(
                RenderTarget& target) = 0;
        // 輝度測定結果を非同期読み戻し用に保存します(target: 測定結果の描画先)。
        virtual void CaptureOffscreenTargetLuminance(
            RenderTarget& target) = 0;
        // 影の深度配列を作ります(shadowMap: 作成先, resolution: 一辺の画素数, cascadeCount: 配列面数, cube: 6面のキューブ指定)。
        virtual void InitializeShadowMap(
            ShadowMap& shadowMap,
            std::uint32_t resolution,
            std::uint32_t cascadeCount,
            bool cube) = 0;
        // 影の面を設定して深度を消去します(shadowMap: 影の描画先, cascadeIndex: 描画面の番号)。
        // 無効な影・範囲外の面・描画中の再入は何もしません。
        virtual void BeginShadowMap(
            ShadowMap& shadowMap,
            std::uint32_t cascadeIndex) = 0;
        // 影描画前の描画先とビューポートへ戻します(shadowMap: 終了する影の描画先)。
        virtual void EndShadowMap(
            ShadowMap& shadowMap) = 0;
        // Forward+用のライト一覧と番号表を更新します(clusteredLights: GPU資源, lighting: ライト情報と結果, view: ビュー行列, projection: 射影行列, width: 画面幅, height: 画面高)。
        virtual void UpdateClusteredLights(
            ClusteredLights& clusteredLights,
            LightingState& lighting,
            const DirectX::XMFLOAT4X4& view,
            const DirectX::XMFLOAT4X4& projection,
            std::uint32_t width,
            std::uint32_t height) = 0;
        // 主描画先の復元状態を保存します。
        // 同じバックエンドの描画区間で使い、Resizeや資源解放をまたがせません。
        [[nodiscard]] virtual std::unique_ptr<GraphicsOutputState>
            CaptureOutputState() = 0;
        // 保存した主描画先を復元します(state: 同じバックエンドで保存した状態)。
        virtual void RestoreOutputState(
            const GraphicsOutputState& state) = 0;
        // アダプター容量とOS予算を返し、取得失敗は各Availableで示します。
        [[nodiscard]] virtual GraphicsVideoMemoryStatistics
            QueryVideoMemoryStatistics() const noexcept = 0;
        // 線分描画器を生成し、初期化済みバックエンドより先に破棄します。
        [[nodiscard]] virtual std::unique_ptr<DebugDrawingBackend>
            CreateDebugDrawingBackend() = 0;
        // 初期化中だけ所有する計測器を貸し出し、未初期化ならnullptrです。
        [[nodiscard]] virtual GpuProfilerBackend*
            ProfilerBackend() noexcept = 0;

        // 単色のRGBA8テクスチャを作ります(color: 各成分0～255のRGBA)。
        [[nodiscard]] virtual GraphicsTextureHandle
            CreateSolidRgba8Texture(
                const std::array<std::uint8_t, 4>& color) = 0;
        // テクスチャを保持する読み取りビューを作ります(texture: 同じバックエンド世代の画像)。
        [[nodiscard]] virtual GraphicsViewHandle
            CreateShaderResourceView(
                const GraphicsTextureHandle& texture) = 0;

        // 頂点バッファへ先頭から書き込みます(buffer: 更新するハンドル, data: 頂点のバイト列)。
        // 空データ・GPU確保や転送の失敗はfalseでハンドルを維持し、異なる世代はinvalid_argumentです。
        [[nodiscard]] virtual bool UpdateDynamicVertexBuffer(
            GraphicsBufferHandle& buffer,
            std::span<const std::byte> data) = 0;

        // 2D画像を生成します(description: 形式・寸法・更新方式, initialData: 全ミップの初期値)。
        // 生成は準備ワーカーでも可能で、PerMipUpdateだけ初期値を空にでき、Immutableは後続更新できません。
        [[nodiscard]] virtual GraphicsTextureHandle CreateTexture2D(
            const GraphicsTexture2DDescription& description,
            std::span<const GraphicsTextureSubresourceData>
                initialData) = 0;
        // ミップ画像を更新します(texture: 更新可能な同世代の画像, mipLevel: ミップ番号, data: 画像データと行ピッチ)。
        // 即時命令を送信するため描画スレッドから呼びます。
        virtual void UpdateTexture2D(
            const GraphicsTextureHandle& texture,
            std::uint32_t mipLevel,
            const GraphicsTextureSubresourceData& data) = 0;
        // 指定範囲の読み取りビューを作ります(texture: 同じバックエンド世代の画像, description: 形式とミップ範囲)。
        [[nodiscard]] virtual GraphicsViewHandle
            CreateShaderResourceView(
                const GraphicsTextureHandle& texture,
                const GraphicsTextureViewDescription& description) = 0;

        // 描画先が所有する表示ビューを世代確認して返します(target: 表示する描画先)。
        [[nodiscard]] virtual GraphicsViewHandle
            CreateOffscreenDisplayView(
                const RenderTarget& target) = 0;

        // 頂点バッファを1本設定します(buffer: 同世代の頂点資源, slot: 入力スロット, stride: 頂点間隔のバイト数, offset: 先頭のバイト位置)。
        virtual void BindVertexBuffer(
            const GraphicsBufferHandle& buffer,
            std::uint32_t slot,
            std::uint32_t stride,
            std::uint32_t offset) = 0;

        // D3D11の連続tレジスターへ設定します(firstSlot: 先頭番号, resources: 設定するビュー列, fallback: 無効要素の代替ビュー)。
        // 未初期化・不正範囲・D3D12の非空範囲はfalseで、有効な空範囲は何もせず成功します。
        // 代替も無効ならnullを設定し、呼び出し側は記録した描画の終了までハンドルを保持します。
        [[nodiscard]] virtual bool TryBindPixelShaderResources(
            std::uint32_t firstSlot,
            std::span<const GraphicsViewHandle> resources,
            const GraphicsViewHandle& fallback) noexcept = 0;

        // 3D画像の全ミップを生成します(description: 形式と寸法, initialData: 全ミップの体積データ)。
        [[nodiscard]] virtual GraphicsTextureHandle CreateTexture3D(
            const GraphicsTexture3DDescription& description,
            std::span<const GraphicsTextureSubresourceData>
                initialData) = 0;

        // 現在の資源世代に属するかだけを返します(view: 確認するビュー)。
        [[nodiscard]] virtual bool IsViewCurrent(
            const GraphicsViewHandle& view) const noexcept = 0;

        // Forward+資源を全て完成後に公開します(clusteredLights: 作成先, assets: シェーダーの取得元, shaderPath: シェーダーのパス)。
        virtual void InitializeClusteredLights(
            ClusteredLights& clusteredLights,
            AssetManager& assets,
            const std::filesystem::path& shaderPath) = 0;
    };

    // D3D11またはD3D12を生成し、他は例外です(activeApi: 選択済みのAPI)。
    [[nodiscard]] std::unique_ptr<GraphicsBackend>
        CreateGraphicsBackend(RenderingApi activeApi);
}
