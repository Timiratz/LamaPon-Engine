#pragma once

#include "LamaPon/Graphics/EnvironmentSettings.h"
#include "LamaPon/Graphics/GraphicsResource.h"

#include <DirectXMath.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace DirectX
{
    inline namespace DX11
    {
        class IEffectLights;
    }
}

namespace LamaPon
{
    // 各ライト上限とGPU用の定数配置はHLSL側と一致させる。
    // 公開データへの項目追加は末尾に置き、既存の位置指定初期化との互換性を保つ。
    // 方向ライトの配列上限
    inline constexpr std::size_t MaximumDirectionalLights = 4;
    // 点ライトの配列上限
    inline constexpr std::size_t MaximumPointLights = 16;
    // スポットライトの配列上限
    inline constexpr std::size_t MaximumSpotLights = 8;
    // 方向影のカスケード上限
    inline constexpr std::size_t MaximumShadowCascades = 4;

    struct DirectionalLightData final
    {
        // 光が進むワールド方向
        DirectX::XMFLOAT3 direction{ 0.0f, -1.0f, 0.0f };
        // 光のRGB色
        DirectX::XMFLOAT3 color{ 1.0f, 1.0f, 1.0f };
        // 光の強度
        float intensity{ 1.0f };
        // 光源の角半径ラジアン
        float angularRadius{ 0.004625f };
    };

    struct PointLightData final
    {
        // 光源のワールド位置
        DirectX::XMFLOAT3 position{};
        // 光が届く距離
        float range{ 10.0f };
        // 光のRGB色
        DirectX::XMFLOAT3 color{ 1.0f, 1.0f, 1.0f };
        // 光の強度
        float intensity{ 1.0f };
    };

    struct SpotLightData final
    {
        // 光源のワールド位置
        DirectX::XMFLOAT3 position{};
        // 光が届く距離
        float range{ 10.0f };
        // 光が進むワールド方向
        DirectX::XMFLOAT3 direction{ 0.0f, -1.0f, 0.0f };
        // 内側コーン半角の余弦
        float innerConeCosine{ 0.9238795f };
        // 光のRGB色
        DirectX::XMFLOAT3 color{ 1.0f, 1.0f, 1.0f };
        // 光の強度
        float intensity{ 1.0f };
        // 外側コーン半角の余弦
        float outerConeCosine{ 0.8191520f };
        // GPU定数の配置調整
        DirectX::XMFLOAT3 padding{};
    };

    struct DirectionalShadowData final
    {
        // カスケード別のライト射影行列
        std::array<
            DirectX::XMFLOAT4X4,
            MaximumShadowCascades> lightViewProjections{};
        // カスケード別の分割距離
        std::array<
            float,
            MaximumShadowCascades> cascadeSplits{};
        // 方向影の配列ビュー
        GraphicsViewHandle texture;
        // 方向ライト一覧の添字
        std::size_t lightIndex{};
        // 使用するカスケード数
        std::size_t cascadeCount{};
        // 影の深度比較バイアス
        float bias{ 0.0015f };
        // 法線方向の影バイアス
        float normalBias{ 0.0025f };
        // 影による減光の強度
        float strength{ 0.85f };
        // 方向影の有効有無
        bool enabled{};
    };


    // スポット影の配列面上限
    inline constexpr std::size_t MaximumSpotShadows = 4;

    struct SpotShadowData final
    {
        // スポット影のライト射影行列
        DirectX::XMFLOAT4X4 lightViewProjection{};
        // スポット添字、負なら未使用
        std::ptrdiff_t lightIndex{ -1 };
        // 影の深度比較バイアス
        float bias{ 0.002f };
        // 法線方向の影バイアス
        float normalBias{ 0.01f };
        // 影による減光の強度
        float strength{ 0.9f };
        // スポット影の有効有無
        bool enabled{};
    };

    struct PointShadowData final
    {
        // 点ライト影のキューブビュー
        GraphicsViewHandle texture;
        // 点ライト添字、負なら影なし
        std::ptrdiff_t lightIndex{ -1 };
        // 影の深度比較バイアス
        float bias{ 0.002f };
        // 影による減光の強度
        float strength{ 0.9f };
        // 点ライト影の有効有無
        bool enabled{};
    };

    // 環境キューブによる間接照明。
    // 鏡面・拡散の両ビューが揃わない場合は元の環境キューブを使う。
    struct EnvironmentMapData final
    {
        // 元の環境キューブビュー
        GraphicsViewHandle texture;
        // 事前フィルターした鏡面ビュー
        GraphicsViewHandle specular;
        // 拡散照度のキューブビュー
        GraphicsViewHandle irradiance;
        // 鏡面反射の最大ミップ
        float specularMaximumMip{};
        // 環境照明の強度
        float intensity{ 1.0f };
        // 環境照明の有効有無
        bool enabled{};
    };

    // HLSLのGpuLightと同じ順のfloat4四個、64バイトのライト情報。
    // extraParametersのyは点0・スポット1、zは(点ライト添字またはスポット影スロット)+1で、0は影なしを表す。
    struct GpuLight final
    {
        // xyzは位置、wは到達距離
        DirectX::XMFLOAT4 positionRange{};
        // rgbは色、wは強度
        DirectX::XMFLOAT4 colorIntensity{};
        // xyzは方向、wは内側コーン余弦
        DirectX::XMFLOAT4 directionInnerCosine{};
        // 外側コーン・種別・影参照・予約
        DirectX::XMFLOAT4 extraParameters{};
    };


    // クラスタ経路のライト総数上限
    inline constexpr std::size_t MaximumClusteredLights = 256;

    // バックエンドのカリング処理が更新するクラスタ照明の結合情報。
    struct ClusteredLightingData final
    {
        // ライト情報の読込ビュー
        GraphicsViewHandle lights;
        // クラスタ別ライト索引のビュー
        GraphicsViewHandle lightIndices;
        // クラスタ別ライト数のビュー
        GraphicsViewHandle clusterCounts;
        // 奥行分割の近面距離
        float nearPlane{ 0.1f };
        // 奥行分割の遠面距離
        float farPlane{ 1000.0f };
        // 画素からクラスタへ変換する幅逆数
        float inverseWidth{};
        // 画素からクラスタへ変換する高逆数
        float inverseHeight{};
        // 使用するクラスタライト数
        std::uint32_t lightCount{};
        // クラスタ照明の有効有無
        bool enabled{};
    };

    // 深度プリパス後に解決し、環境光とIBLへ適用する画面空間の遮蔽。
    struct ScreenAmbientOcclusionData final
    {
        // 画面空間の遮蔽率ビュー
        GraphicsViewHandle texture;
        // 画素座標をUVへ変換する幅逆数
        float inverseWidth{};
        // 画素座標をUVへ変換する高逆数
        float inverseHeight{};
        // 画面空間遮蔽の有効有無
        bool enabled{};
    };

    // 前フレームのHDR画像と現在の深度で求める画面空間反射の入力。
    struct ScreenSpaceReflectionData final
    {
        // 前フレームのHDR画像ビュー
        GraphicsViewHandle texture;
        // 現在の深度プリパスのビュー
        GraphicsViewHandle depth;
        // 前フレームのビュー射影行列
        DirectX::XMFLOAT4X4 previousViewProjection{};
        // 画素座標をUVへ変換する幅逆数
        float inverseWidth{};
        // 画素座標をUVへ変換する高逆数
        float inverseHeight{};
        // 深度復元用の射影_33
        float projectionZ{};
        // 深度復元用の射影_43
        float projectionW{};
        // 画面空間反射の強度
        float intensity{ 1.0f };
        // 反射を探索する最大距離
        float maximumDistance{ 12.0f };
        // 交差と見なす深度の厚さ
        float thickness{ 1.2f };
        // 反射を使う粗さの上限
        float roughnessCutoff{ 0.45f };
        // 反射を探索するステップ数
        std::uint32_t stepCount{ 48 };
        // 画面空間反射の有効有無
        bool enabled{};
        // 深度ピラミッドの最終ミップ
        std::uint32_t depthPyramidMaximumMip{};
    };

    // RGB別の3Dテクスチャに(x・y・z係数・定数項)を格納するL1球面調和の間接光。
    struct BakedGlobalIlluminationData final
    {
        // 赤の球面調和係数の3Dビュー
        GraphicsViewHandle redCoefficients;
        // 緑の球面調和係数の3Dビュー
        GraphicsViewHandle greenCoefficients;
        // 青の球面調和係数の3Dビュー
        GraphicsViewHandle blueCoefficients;
        // 照度領域のワールド最小位置
        DirectX::XMFLOAT3 volumeMinimum{};
        // 照度領域のワールドサイズ
        DirectX::XMFLOAT3 volumeSize{ 1.0f, 1.0f, 1.0f };
        // 照度領域の各軸の分割数
        DirectX::XMFLOAT3 resolution{ 1.0f, 1.0f, 1.0f };
        // ベイクした間接光の強度
        float intensity{ 1.0f };
        // ベイクした間接光の有効有無
        bool enabled{};
    };

    struct LightingState final
    {
        // 共通の環境光色
        DirectX::XMFLOAT3 ambientColor{ 0.65f, 0.72f, 0.85f };
        // 共通の環境光強度
        float ambientIntensity{ 0.35f };
        // 方向ライトの固定長一覧
        std::array<
            DirectionalLightData,
            MaximumDirectionalLights> directionalLights{};
        // 有効な方向ライト数
        std::size_t directionalLightCount{};
        // 点ライトの固定長一覧
        std::array<
            PointLightData,
            MaximumPointLights> pointLights{};
        // 有効な点ライト数
        std::size_t pointLightCount{};
        // スポットライトの固定長一覧
        std::array<
            SpotLightData,
            MaximumSpotLights> spotLights{};
        // 有効なスポットライト数
        std::size_t spotLightCount{};
        // 方向影の描画情報
        DirectionalShadowData directionalShadow;
        // スポット影のスロット一覧
        std::array<
            SpotShadowData,
            MaximumSpotShadows> spotShadows{};
        // スポット影の配列ビュー
        GraphicsViewHandle spotShadowTexture;
        // 点ライト影の描画情報
        PointShadowData pointShadow;
        // キューブ環境の照明情報
        EnvironmentMapData environment;
        // シーンの霧設定
        FogSettings fog;
        // 方向影のPCF用解像度
        float directionalShadowResolution{ 2048.0f };
        // ローカル影のPCF用解像度
        float localShadowResolution{ 1024.0f };
        // 画面空間の遮蔽情報
        ScreenAmbientOcclusionData screenAmbientOcclusion;
        // クラスタ経路の全ライト一覧
        // 点・スポットの先頭は固定長一覧と同じ順に保ち、影参照の添字を一致させる。
        std::vector<GpuLight> clusteredLights;
        // クラスタカリングの結果
        ClusteredLightingData clustered;
        // 画面空間の反射情報
        ScreenSpaceReflectionData screenSpaceReflection;
        // ベイクした間接光の情報
        BakedGlobalIlluminationData bakedGlobalIllumination;
    };

    // 物体位置で強いライトを選びDirectXTKの照明へ適用する(effect: 更新する照明効果, lighting: シーン照明の状態, objectPosition: ワールド位置)。
    // 各ライト数は配列の上限内とし、ローカル光を方向光へ近似して通常3灯、DGSLは4灯を適用する。
    void ApplyLighting(
        DirectX::IEffectLights& effect,
        const LightingState& lighting,
        const DirectX::XMFLOAT3& objectPosition);
}
