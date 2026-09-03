#pragma once

#include "LamaPon/Graphics/EnvironmentSettings.h"

#include <DirectXMath.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

struct ID3D11ShaderResourceView;

namespace DirectX
{
    inline namespace DX11
    {
        class IEffectLights;
    }
}

namespace LamaPon
{
    // シェーダー側の配列サイズ（LamaPonLit.hlsl等）と一致させる
    // こと。
    inline constexpr std::size_t MaximumDirectionalLights = 4;
    inline constexpr std::size_t MaximumPointLights = 16;
    inline constexpr std::size_t MaximumSpotLights = 8;
    inline constexpr std::size_t MaximumShadowCascades = 4;

    struct DirectionalLightData final
    {
        DirectX::XMFLOAT3 direction{ 0.0f, -1.0f, 0.0f };
        DirectX::XMFLOAT3 color{ 1.0f, 1.0f, 1.0f };
        float intensity{ 1.0f };
        // 光源の見かけの大きさ（半径・ラジアン）。既定は本物の太陽の
        // 0.53度＝角半径0.00465で、これがハイライトの大きさになります。
        // 0にすると数学的な点になり、つるつるの物体ではハイライトが
        // 消えたように見えます。末尾に足しています（位置指定の
        // 初期化を壊さないため）。
        float angularRadius{ 0.004625f };
    };

    struct PointLightData final
    {
        DirectX::XMFLOAT3 position{};
        float range{ 10.0f };
        DirectX::XMFLOAT3 color{ 1.0f, 1.0f, 1.0f };
        float intensity{ 1.0f };
    };

    struct SpotLightData final
    {
        DirectX::XMFLOAT3 position{};
        float range{ 10.0f };
        DirectX::XMFLOAT3 direction{ 0.0f, -1.0f, 0.0f };
        float innerConeCosine{ 0.9238795f };
        DirectX::XMFLOAT3 color{ 1.0f, 1.0f, 1.0f };
        float intensity{ 1.0f };
        float outerConeCosine{ 0.8191520f };
        DirectX::XMFLOAT3 padding{};
    };

    struct DirectionalShadowData final
    {
        std::array<
            DirectX::XMFLOAT4X4,
            MaximumShadowCascades> lightViewProjections{};
        std::array<
            float,
            MaximumShadowCascades> cascadeSplits{};
        ID3D11ShaderResourceView* texture{};
        std::size_t lightIndex{};
        std::size_t cascadeCount{};
        float bias{ 0.0015f };
        float normalBias{ 0.0025f };
        float strength{ 0.85f };
        bool enabled{};
    };

    // 影付きスポットライトの同時上限（シャドウマップのスライス数）。
    inline constexpr std::size_t MaximumSpotShadows = 4;

    struct SpotShadowData final
    {
        DirectX::XMFLOAT4X4 lightViewProjection{};
        // spotLights配列の添字。負なら未使用スロット。
        std::ptrdiff_t lightIndex{ -1 };
        float bias{ 0.002f };
        float normalBias{ 0.01f };
        float strength{ 0.9f };
        bool enabled{};
    };

    struct PointShadowData final
    {
        ID3D11ShaderResourceView* texture{};
        // pointLights配列の添字。負なら影なし。
        std::ptrdiff_t lightIndex{ -1 };
        float bias{ 0.002f };
        float strength{ 0.9f };
        bool enabled{};
    };

    // キューブマップ環境光（IBL）。
    struct EnvironmentMapData final
    {
        ID3D11ShaderResourceView* texture{};
        // 事前フィルタ済みスペキュラ／放射照度キューブ
        // （EnvironmentRendererが生成。nullならtextureへ
        // フォールバックします）。
        ID3D11ShaderResourceView* specular{};
        ID3D11ShaderResourceView* irradiance{};
        float specularMaximumMip{};
        float intensity{ 1.0f };
        bool enabled{};
    };

    // クラスタライトカリング（Forward+）へ送るライト1灯ぶん。
    // LamaPonLightCulling.hlslのGpuLightと並びを一致させています
    // （4つのfloat4、64バイト）。
    struct GpuLight final
    {
        // xyz=ワールド位置, w=届く距離。
        DirectX::XMFLOAT4 positionRange{};
        // rgb=色, w=強度。
        DirectX::XMFLOAT4 colorIntensity{};
        // xyz=向き（スポット）, w=内側コーンのcos。
        DirectX::XMFLOAT4 directionInnerCosine{};
        // x=外側コーンのcos, y=種別（0=ポイント, 1=スポット）,
        // z=影の参照（スポット=スロット+1, ポイント=ライト番号+1,
        //   0=影なし）, w=予約。
        DirectX::XMFLOAT4 extraParameters{};
    };

    // クラスタ経路で扱うライトの総数上限。定数バッファの16灯と
    // 違い、こちらはStructuredBufferなので大きくできます。
    inline constexpr std::size_t MaximumClusteredLights = 256;

    // クラスタカリングの結果（LitEffectへのバインド情報）。
    // ClusteredLights::Updateが書き込みます。
    struct ClusteredLightingData final
    {
        ID3D11ShaderResourceView* lights{};
        ID3D11ShaderResourceView* lightIndices{};
        ID3D11ShaderResourceView* clusterCounts{};
        float nearPlane{ 0.1f };
        float farPlane{ 1000.0f };
        // ピクセル座標からクラスタの縦横を求めるための1/幅・1/高さ。
        float inverseWidth{};
        float inverseHeight{};
        std::uint32_t lightCount{};
        bool enabled{};
    };

    // 画面空間の遮蔽（SSAO）。深度プリパスの後、ライティングより前に
    // 解決したものをLitシェーダーへ渡します。完成した色へ掛けるのと
    // 違って、環境光／IBL項だけを暗くできます。
    struct ScreenAmbientOcclusionData final
    {
        ID3D11ShaderResourceView* texture{};
        // SV_PositionからUVを作るための1/幅・1/高さ。
        float inverseWidth{};
        float inverseHeight{};
        bool enabled{};
    };

    // SSR（画面空間反射）をLitシェーダーへ渡すための一式。
    // texture は「前フレームのHDRカラー」、depth は深度プリパスの
    // 深度です。previousViewProjection は当たった点を前フレームの
    // 画面座標へ戻すために使います（カメラが動いても映り込みの
    // 位置がずれないようにするため）。
    struct ScreenSpaceReflectionData final
    {
        ID3D11ShaderResourceView* texture{};
        ID3D11ShaderResourceView* depth{};
        DirectX::XMFLOAT4X4 previousViewProjection{};
        float inverseWidth{};
        float inverseHeight{};
        // 深度をビュー空間のZへ戻すための射影の値。
        // SSAOと同じ x=projection._33, y=projection._43 です。
        float projectionZ{};
        float projectionW{};
        float intensity{ 1.0f };
        float maximumDistance{ 12.0f };
        // ScreenSpaceReflectionSettings と同じ既定にしておきます
        // （片方だけ変えると、設定を渡す前の1フレームだけ違う絵に
        // なります）。
        float thickness{ 1.2f };
        float roughnessCutoff{ 0.45f };
        std::uint32_t stepCount{ 48 };
        bool enabled{};
        // Hi-Z深度ピラミッドの最終ミップ番号（末尾追加）。
        std::uint32_t depthPyramidMaximumMip{};
    };

    // ベイクした間接光（照度ボリューム）の描画用データ。
    // 3枚のTexture3Dは、L1球面調和の係数をRGBチャンネル別に
    // 詰めたものです（texel = (x係数, y係数, z係数, 定数項)。
    // シェーダーは dot(float4(法線,1), texel) で環境光を得ます）。
    struct BakedGlobalIlluminationData final
    {
        ID3D11ShaderResourceView* redCoefficients{};
        ID3D11ShaderResourceView* greenCoefficients{};
        ID3D11ShaderResourceView* blueCoefficients{};
        DirectX::XMFLOAT3 volumeMinimum{};
        DirectX::XMFLOAT3 volumeSize{ 1.0f, 1.0f, 1.0f };
        DirectX::XMFLOAT3 resolution{ 1.0f, 1.0f, 1.0f };
        float intensity{ 1.0f };
        bool enabled{};
    };

    struct LightingState final
    {
        DirectX::XMFLOAT3 ambientColor{ 0.65f, 0.72f, 0.85f };
        float ambientIntensity{ 0.35f };
        std::array<
            DirectionalLightData,
            MaximumDirectionalLights> directionalLights{};
        std::size_t directionalLightCount{};
        std::array<
            PointLightData,
            MaximumPointLights> pointLights{};
        std::size_t pointLightCount{};
        std::array<
            SpotLightData,
            MaximumSpotLights> spotLights{};
        std::size_t spotLightCount{};
        DirectionalShadowData directionalShadow;
        std::array<
            SpotShadowData,
            MaximumSpotShadows> spotShadows{};
        ID3D11ShaderResourceView* spotShadowTexture{};
        PointShadowData pointShadow;
        EnvironmentMapData environment;
        FogSettings fog;
        // シャドウマップ解像度（PCFのテクセルサイズ計算用）。
        float directionalShadowResolution{ 2048.0f };
        float localShadowResolution{ 1024.0f };
        // 末尾に足しています（位置指定の初期化を壊さないため）。
        ScreenAmbientOcclusionData screenAmbientOcclusion;
        // クラスタ経路のライト全量（定数バッファの上限に縛られない）。
        // 並びの先頭は従来のpointLights/spotLightsと同じ順なので、
        // 影スロットの対応がそのまま成立します。
        std::vector<GpuLight> clusteredLights;
        ClusteredLightingData clustered;
        // 末尾に足しています（位置指定の初期化を壊さないため）。
        ScreenSpaceReflectionData screenSpaceReflection;
        // ベイクした間接光（照度ボリューム）。L1球面調和のRGB各
        // チャンネルを詰めたTexture3Dが3枚（t23〜t25）。
        BakedGlobalIlluminationData bakedGlobalIllumination;
    };

    void ApplyLighting(
        DirectX::IEffectLights& effect,
        const LightingState& lighting,
        const DirectX::XMFLOAT3& objectPosition);
}
