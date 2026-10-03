#pragma once

#include "LamaPon/Graphics/GraphicsResource.h"
#include "LamaPon/Graphics/ReflectionProbeEnvironment.h"
#include "LamaPon/Graphics/ShaderRenderState.h"

#include <DirectXMath.h>

#include <array>
#include <cstdint>
#include <functional>
#include <span>

namespace LamaPon
{
    // 粒子が生成した四角形の頂点を、描画APIの頂点形式へ変換する。
    struct ParticleRenderVertex final
    {
        // 粒子頂点のワールド座標
        DirectX::XMFLOAT3 position{};
        // 粒子頂点の色
        DirectX::XMFLOAT4 color{};
        // 粒子頂点のUV座標
        DirectX::XMFLOAT2 textureCoordinate{};
    };

    // 頂点列とコールバックはDrawParticlesの同期呼出し中だけ借用する。
    struct ParticleDrawRequest final
    {
        // 四頂点単位の粒子頂点列
        std::span<const ParticleRenderVertex> vertices;
        // ビュー行列
        DirectX::XMFLOAT4X4 view{};
        // 射影行列
        DirectX::XMFLOAT4X4 projection{};
        // 粒子テクスチャ
        GraphicsViewHandle texture;
        // 補助テクスチャ
        GraphicsViewHandle auxiliaryTexture;
        // 未指定時の代替テクスチャ
        GraphicsViewHandle fallbackTexture;
        // 加算混合を使う
        bool additive{ true };

        // 適用成功を返す独自シェーダー処理
        std::function<bool()> applyCustomPixelShader;
    };

    enum class PrimitiveRenderShape : std::uint8_t
    {
        Cube,
        Sphere,
        Cylinder,
        Plane,
        Procedural
    };

    struct PrimitiveRenderVertex final
    {
        // 頂点のローカル座標
        DirectX::XMFLOAT3 position{};
        // 頂点の法線
        DirectX::XMFLOAT3 normal{ 0.0f, 1.0f, 0.0f };
        // 頂点のUV座標
        DirectX::XMFLOAT2 textureCoordinate{};
    };

    // インスタンス描画でslot 1へ並べる、D3D11のInstanceDataと同じ80 bytesです。
    struct PrimitiveInstanceData final
    {
        // 各個体のワールド行列
        DirectX::XMFLOAT4X4 world{};
        // 各個体の乗算色
        DirectX::XMFLOAT4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
    };
    static_assert(sizeof(PrimitiveInstanceData) == 80u);

    struct PrimitiveDirectionalLight final
    {
        // 光が進む方向
        DirectX::XMFLOAT3 direction{ 0.0f, -1.0f, 0.0f };
        // 光の色
        DirectX::XMFLOAT3 color{ 1.0f, 1.0f, 1.0f };
        // 光の強度
        float intensity{ 1.0f };
        // 光源の見かけの半径、ラジアン
        float angularRadius{};
    };

    struct PrimitivePointLight final
    {
        // 光源のワールド座標
        DirectX::XMFLOAT3 position{};
        // 光が届く距離
        float range{ 10.0f };
        // 光の色
        DirectX::XMFLOAT3 color{ 1.0f, 1.0f, 1.0f };
        // 光の強度
        float intensity{ 1.0f };
    };

    // 光の進行方向と円錐角の余弦はSpotLightDataと同じ規約とする。
    struct PrimitiveSpotLight final
    {
        // 光源のワールド座標
        DirectX::XMFLOAT3 position{};
        // 光が届く距離
        float range{ 10.0f };
        // 光が進む方向
        DirectX::XMFLOAT3 direction{ 0.0f, -1.0f, 0.0f };
        // 内側円錐の角度の余弦
        float innerConeCosine{ 0.9238795f };
        // 光の色
        DirectX::XMFLOAT3 color{ 1.0f, 1.0f, 1.0f };
        // 光の強度
        float intensity{ 1.0f };
        // 外側円錐の角度の余弦
        float outerConeCosine{ 0.8191520f };
    };

    struct PrimitiveDirectionalShadow final
    {
        // 各カスケードのビュー射影行列
        std::array<DirectX::XMFLOAT4X4, 4>
            lightViewProjections{};
        // カスケードの分割距離
        std::array<float, 4> cascadeSplits{};
        // 平行光源の深度配列参照
        GraphicsViewHandle texture;
        // 影を作る平行光源の番号
        std::size_t lightIndex{};
        // 有効なカスケード数
        std::size_t cascadeCount{};
        // 深度の比較補正
        float bias{ 0.0015f };
        // 法線方向の位置補正
        float normalBias{ 0.0025f };
        // 影の強度
        float strength{ 0.85f };
        // 影の解像度の逆数
        float inverseResolution{};
        // 平行光源の影を使う
        bool enabled{};
    };

    struct PrimitiveSpotShadow final
    {
        // 光源のビュー射影行列
        DirectX::XMFLOAT4X4 lightViewProjection{};
        // 光源番号、負なら未使用
        std::ptrdiff_t lightIndex{ -1 };
        // 深度の比較補正
        float bias{ 0.002f };
        // 法線方向の位置補正
        float normalBias{ 0.01f };
        // 影の強度
        float strength{ 0.9f };
        // スポット光源の影を使う
        bool enabled{};
    };

    struct PrimitivePointShadow final
    {
        // 点光源の深度キューブ参照
        GraphicsViewHandle texture;
        // 光源番号、負なら未使用
        std::ptrdiff_t lightIndex{ -1 };
        // 深度の比較補正
        float bias{ 0.002f };
        // 影の強度
        float strength{ 0.9f };
        // 点光源の影を使う
        bool enabled{};
    };

    // 遮蔽を画面座標から参照し、環境光にだけ適用する。
    struct PrimitiveScreenAmbientOcclusion final
    {
        // 画面全体の遮蔽参照
        GraphicsViewHandle texture;
        // 画面幅の逆数
        float inverseWidth{};
        // 画面高さの逆数
        float inverseHeight{};
        // 画面空間遮蔽を使う
        bool enabled{};
    };

    // Hi-Zはカメラからの距離を持ち、交点を前フレームへ射影してHDR色を読む。
    struct PrimitiveScreenSpaceReflection final
    {
        // 前フレームのHDR色参照
        GraphicsViewHandle texture;
        // 距離を持つHi-Z深度参照
        GraphicsViewHandle depth;
        // 前フレームのビュー射影行列
        DirectX::XMFLOAT4X4 previousViewProjection{};
        // 画面幅の逆数
        float inverseWidth{};
        // 画面高さの逆数
        float inverseHeight{};
        // 反射の強度
        float intensity{ 1.0f };
        // 反射探索の最大距離
        float maximumDistance{ 12.0f };
        // 交差判定の許容厚さ
        float thickness{ 1.2f };
        // 反射を使う粗さの上限
        float roughnessCutoff{ 0.45f };
        // 反射探索の最大ステップ数
        std::uint32_t stepCount{ 48 };
        // Hi-Zの最大ミップ番号
        std::uint32_t depthPyramidMaximumMip{};
        // 画面空間反射を使う
        bool enabled{};
    };

    // 鏡面・拡散の畳み込み参照が揃わなければ元の環境キューブを使う。
    struct PrimitiveEnvironment final
    {
        // 元の環境キューブ参照
        GraphicsViewHandle texture;
        // 畳み込み済み鏡面参照
        GraphicsViewHandle specular;
        // 畳み込み済み拡散参照
        GraphicsViewHandle irradiance;
        // 鏡面参照の最大ミップ番号
        float specularMaximumMip{};
        // 環境光の強度
        float intensity{ 1.0f };
        // 環境光を使う
        bool enabled{};
    };

    // 三つの参照が揃う場合だけ固定配列に代えてクラスタの光源表を使う。
    struct PrimitiveClusteredLights final
    {
        // 点・スポット光源のデータ参照
        GraphicsViewHandle lights;
        // 各クラスタの光源番号参照
        GraphicsViewHandle lightIndices;
        // 各クラスタの光源数参照
        GraphicsViewHandle clusterCounts;
        // 視錐台の近距離
        float nearPlane{ 0.1f };
        // 視錐台の遠距離
        float farPlane{ 1000.0f };
        // 画面幅の逆数
        float inverseWidth{};
        // 画面高さの逆数
        float inverseHeight{};
        // 有効なクラスタ光源数
        std::uint32_t lightCount{};
        // クラスタ光源を使う
        bool enabled{};
    };

    // LamaPonLitは距離による範囲霧と指数霧、DirectXTKはビュー深度による線形霧とする。
    enum class PrimitiveFogModel : std::uint8_t
    {
        LamaPonLit,
        DirectXTK
    };

    struct PrimitiveFog final
    {
        // 霧の色
        DirectX::XMFLOAT3 color{ 0.48f, 0.62f, 0.76f };
        // 霧が始まる距離
        float startDistance{ 8.0f };
        // 霧が覆う距離
        float endDistance{ 35.0f };
        // 指数霧の密度
        float density{ 0.015f };
        // 霧の計算方式
        PrimitiveFogModel model{ PrimitiveFogModel::LamaPonLit };
        // 霧を使う
        bool enabled{};
    };

    // RGB別のL1球面調和係数をTexture3Dで持ち、LitEffectのt23〜t25と同じ配置にする。
    struct PrimitiveBakedGlobalIllumination final
    {
        // 赤成分のSH係数参照
        GraphicsViewHandle redCoefficients;
        // 緑成分のSH係数参照
        GraphicsViewHandle greenCoefficients;
        // 青成分のSH係数参照
        GraphicsViewHandle blueCoefficients;
        // 照度格子の最小座標
        DirectX::XMFLOAT3 volumeMinimum{};
        // 照度格子の領域寸法
        DirectX::XMFLOAT3 volumeSize{ 1.0f, 1.0f, 1.0f };
        // 照度格子の各軸の点数
        DirectX::XMFLOAT3 resolution{ 1.0f, 1.0f, 1.0f };
        // ベイクした間接光の強度
        float intensity{ 1.0f };
        // ベイクした間接光を使う
        bool enabled{};
    };

    // 頂点・番号・個体情報の列はDrawPrimitiveの同期呼出し中だけ借用する。
    struct PrimitiveDrawRequest final
    {
        // 描画する形状
        PrimitiveRenderShape shape{ PrimitiveRenderShape::Cube };
        // 手続き生成形状の頂点列
        std::span<const PrimitiveRenderVertex> vertices;
        // 三角形の頂点番号列
        std::span<const std::uint32_t> indices;
        // ワールド行列
        DirectX::XMFLOAT4X4 world{};
        // ビュー行列
        DirectX::XMFLOAT4X4 view{};
        // 射影行列
        DirectX::XMFLOAT4X4 projection{};
        // 材質の基準色
        DirectX::XMFLOAT4 baseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        // 材質の粗さ
        float roughness{ 0.5f };
        // 材質の金属度
        float metallic{};
        // 法線テクスチャの強度
        float normalStrength{ 1.0f };
        // 遮蔽テクスチャの強度
        float occlusionStrength{ 1.0f };
        // 発光色の乗算係数
        DirectX::XMFLOAT3 emissiveFactor{};
        // 環境光の色
        DirectX::XMFLOAT3 ambientColor{ 0.65f, 0.72f, 0.85f };
        // 環境光の強度
        float ambientIntensity{ 0.35f };
        // 平行光源の固定配列
        std::array<PrimitiveDirectionalLight, 4> directionalLights{};
        // 有効な平行光源数
        // 光源数は対応する固定配列の容量以下にする。
        std::size_t directionalLightCount{};
        // 点光源の固定配列
        std::array<PrimitivePointLight, 16> pointLights{};
        // 有効な点光源数
        std::size_t pointLightCount{};
        // スポット光源の固定配列
        std::array<PrimitiveSpotLight, 8> spotLights{};
        // 有効なスポット光源数
        std::size_t spotLightCount{};
        // 平行光源の影設定
        PrimitiveDirectionalShadow directionalShadow;
        // スポット光源の影設定
        std::array<PrimitiveSpotShadow, 4> spotShadows{};
        // スポット光源の深度配列参照
        GraphicsViewHandle spotShadowTexture;
        // 点光源の影設定
        PrimitivePointShadow pointShadow;
        // 局所光源の影解像度の逆数
        float localShadowInverseResolution{};
        // 画面空間遮蔽の設定
        PrimitiveScreenAmbientOcclusion screenAmbientOcclusion;
        // 画面空間反射の設定
        PrimitiveScreenSpaceReflection screenSpaceReflection;
        // 空の環境光設定
        PrimitiveEnvironment environment;
        // 霧の設定
        PrimitiveFog fog;
        // クラスタ光源の設定
        PrimitiveClusteredLights clustered;
        // ベイクした間接光の設定
        PrimitiveBakedGlobalIllumination bakedGlobalIllumination;
        // 描画位置の反射プローブ
        // プローブを使えない場合は空の環境光設定へ戻す。
        ReflectionProbeEnvironment reflectionProbe;
        // 基準色テクスチャ
        GraphicsViewHandle albedo;
        // 法線テクスチャ
        GraphicsViewHandle normalTexture;
        // 粗さテクスチャ
        GraphicsViewHandle roughnessTexture;
        // 金属度テクスチャ
        GraphicsViewHandle metallicTexture;
        // 遮蔽テクスチャ
        GraphicsViewHandle occlusionTexture;
        // 発光テクスチャ
        GraphicsViewHandle emissiveTexture;
        // 未指定時の代替テクスチャ
        GraphicsViewHandle fallbackTexture;
        // アルファ混合を使う
        bool alphaBlend{};
        // 深度比較を行う
        bool depthTest{ true };
        // 深度を書き込む
        bool depthWrite{ true };
        // 色を書かず深度だけを描く
        // 設定済みの深度描画先を維持し、材質テクスチャとピクセルシェーダーを使わない。
        bool depthOnly{};
        // 両面の辺だけを描く
        bool wireframe{};
        // ワイヤー表示以外の面除去
        // BackはCullCounterClockwise、FrontはCullClockwiseと同じ向きとする。
        ShaderCullMode cull{ ShaderCullMode::None };
        // 一括描画する個体情報列
        // 個体情報を使う場合は要求側のworldを単位行列にする。
        std::span<const PrimitiveInstanceData> instances;
    };

    // 生成済みの描画要求を稼働中のAPIへ同期的に送る。
    class GraphicsRenderServices
    {
    public:
        // 派生した描画サービスの資源を解放する。
        virtual ~GraphicsRenderServices() = default;

        // 描画サービスの基底を生成する。
        GraphicsRenderServices() = default;
        // 描画サービスの複製を禁止する。
        GraphicsRenderServices(const GraphicsRenderServices&) = delete;
        // 描画サービスの複製代入を禁止する。
        GraphicsRenderServices& operator=(
            const GraphicsRenderServices&) = delete;

        // 四頂点単位の粒子を描画し、処理できなければ偽を返す(request: 粒子と描画条件)。
        [[nodiscard]] virtual bool DrawParticles(
            const ParticleDrawRequest& request) = 0;
        // 共通の形状描画要求を処理し、未対応なら偽を返す。
        [[nodiscard]] virtual bool DrawPrimitive(
            const PrimitiveDrawRequest&)
        {
            return false;
        }
    };
}
