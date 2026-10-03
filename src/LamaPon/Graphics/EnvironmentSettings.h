#pragma once

#include "LamaPon/Graphics/GraphicsResource.h"

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace LamaPon
{
    // 環境ベイクのキューブ面解像度
    inline constexpr std::uint32_t EnvironmentProbeBakeFaceSize = 128;

    struct SkySettings final
    {
        // 空の描画の有効有無
        bool enabled{};
        // 空頂部の色
        DirectX::XMFLOAT3 topColor{ 0.06f, 0.18f, 0.42f };
        // 地平線の色
        DirectX::XMFLOAT3 horizonColor{ 0.48f, 0.68f, 0.9f };
        // 地面側の色
        DirectX::XMFLOAT3 groundColor{ 0.04f, 0.05f, 0.08f };
        // 空の描画強度
        float intensity{ 1.0f };
        // キューブ画像、空なら色補間
        std::filesystem::path cubemapPath{};
        // 環境照明の強度、0は無効
        float iblIntensity{ 1.0f };
        // 太陽高度による自動配色の有無
        bool sunDriven{};
    };

    // 空の描画へ渡す解決済みの太陽情報。
    struct SkySunDescription final
    {
        // 光の進行方向と逆の太陽方向
        DirectX::XMFLOAT3 directionToSun{ 0.0f, 1.0f, 0.0f };
        // 強度を乗じた太陽の光色
        DirectX::XMFLOAT3 color{ 1.0f, 1.0f, 1.0f };
        // 太陽の角半径ラジアン
        float angularRadius{ 0.004625f };
    };

    // 太陽の高度から決まる空と環境光。
    struct SunDrivenSky final
    {
        // 太陽高度で決めた空頂部の色
        DirectX::XMFLOAT3 topColor{};
        // 太陽高度で決めた地平線の色
        DirectX::XMFLOAT3 horizonColor{};
        // 太陽高度で決めた地面側の色
        DirectX::XMFLOAT3 groundColor{};
        // 正規化した環境光色
        DirectX::XMFLOAT3 ambientColor{};
        // 太陽高度で決めた環境光強度
        float ambientIntensity{};
        // 昼の比率、0は夜、1は昼
        float dayAmount{};
    };

    // 太陽の高さから空と環境光の色を求める(towardSun: 太陽へ向かう単位方向)。
    [[nodiscard]] inline SunDrivenSky EvaluateSunDrivenSky(
        const DirectX::XMFLOAT3& towardSun)
    {
        // 0から1に収める(value: 対象の値)。
        const auto saturateValue = [](const float value)
        {
            return value < 0.0f
                ? 0.0f
                : (value > 1.0f ? 1.0f : value);
        };
        // 三次補間で0から1へ変換する(edge0: 下端, edge1: 上端, value: 対象の値)。
        const auto smooth = [&saturateValue](
            const float edge0,
            const float edge1,
            const float value)
        {
            // 範囲内に収めた補間率
            const float t = saturateValue(
                (value - edge0) / (edge1 - edge0));
            return t * t * (3.0f - 2.0f * t);
        };
        // 二つの色を線形補間する(a: 始点色, b: 終点色, t: 補間率)。
        const auto mix = [](
            const DirectX::XMFLOAT3& a,
            const DirectX::XMFLOAT3& b,
            const float t)
        {
            return DirectX::XMFLOAT3{
                a.x + (b.x - a.x) * t,
                a.y + (b.y - a.y) * t,
                a.z + (b.z - a.z) * t
            };
        };

        // 太陽方向Yの高度正弦
        const float elevation = towardSun.y;
        // 太陽高度による昼の比率
        const float dayAmount = smooth(-0.10f, 0.30f, elevation);
        // 地平付近で増す朝夕の比率
        const float duskAmount =
            smooth(-0.22f, -0.02f, elevation)
            * (1.0f - smooth(0.02f, 0.30f, elevation));

        // 夜の空頂部の色
        const DirectX::XMFLOAT3 nightTop{ 0.010f, 0.020f, 0.060f };
        // 夜の地平線の色
        const DirectX::XMFLOAT3 nightHorizon{
            0.035f, 0.055f, 0.120f };
        // 昼の空頂部の色
        const DirectX::XMFLOAT3 dayTop{ 0.130f, 0.330f, 0.720f };
        // 昼の地平線の色
        const DirectX::XMFLOAT3 dayHorizon{
            0.600f, 0.760f, 0.940f };
        // 朝夕の空頂部の色
        const DirectX::XMFLOAT3 duskTop{ 0.170f, 0.160f, 0.360f };
        // 朝夕の地平線の色
        const DirectX::XMFLOAT3 duskHorizon{
            0.960f, 0.420f, 0.150f };

        // 返却する空と環境光の配色
        SunDrivenSky result{};
        result.dayAmount = dayAmount;
        result.topColor = mix(
            mix(nightTop, dayTop, dayAmount),
            duskTop,
            duskAmount);
        result.horizonColor = mix(
            mix(nightHorizon, dayHorizon, dayAmount),
            duskHorizon,
            duskAmount);
        // 地面側は地平の色を暗くした反射色とする。
        result.groundColor = {
            result.horizonColor.x * 0.18f + 0.010f,
            result.horizonColor.y * 0.18f + 0.011f,
            result.horizonColor.z * 0.18f + 0.014f
        };
        // 環境光は空頂部と地平の色を混ぜる。
        result.ambientColor = mix(
            result.topColor,
            result.horizonColor,
            0.55f);
        // 環境光色の最大成分
        const float maximumChannel = std::max(
            result.ambientColor.x,
            std::max(
                result.ambientColor.y,
                result.ambientColor.z));
        if (maximumChannel > 0.001f)
        {
            // 色の最大成分を1へ正規化し、強度は別の係数で渡す。
            result.ambientColor = {
                result.ambientColor.x / maximumChannel,
                result.ambientColor.y / maximumChannel,
                result.ambientColor.z / maximumChannel
            };
        }
        result.ambientIntensity =
            0.045f + 0.305f * dayAmount;
        return result;
    }

    struct FogSettings final
    {
        // 霧の有効有無
        bool enabled{};
        // 霧の色
        DirectX::XMFLOAT3 color{ 0.48f, 0.62f, 0.76f };
        // 霧が始まる距離
        float startDistance{ 8.0f };
        // 霧が最大になる距離
        float endDistance{ 35.0f };
        // 霧の密度
        float density{ 0.015f };
    };

    struct BloomSettings final
    {
        // ブルームの有効有無
        bool enabled{};
        // ブルームを抽出する輝度閾値
        float threshold{ 0.72f };
        // ブルームの加算強度
        float intensity{ 0.45f };
        // ぼかしサンプルの半径
        float radius{ 2.0f };
    };

    // 深度から再構成した法線と距離差で形状の境界を描くアウトライン。
    struct ScreenOutlineSettings final
    {
        // 画面輪郭の有効有無
        bool enabled{};
        // 輪郭の色
        DirectX::XMFLOAT3 color{ 0.02f, 0.02f, 0.02f };
        // 輪郭の強度
        float intensity{ 1.0f };
        // 近傍を調べる画素間隔、1～4
        float thickness{ 1.0f };
        // 境界とする相対距離差の閾値
        float depthThreshold{ 0.025f };
        // 境界とする法線差の閾値
        float normalThreshold{ 0.25f };
    };

    // 画面の高輝度部からゴースト・ハロー・筋を生成するレンズフレア。
    struct ScreenSpaceLensFlareSettings final
    {
        // レンズフレアの有効有無
        bool enabled{};
        // フレアを抽出する輝度閾値
        float threshold{ 1.6f };
        // フレアの強度
        float intensity{ 0.28f };
        // 画面中心からのゴースト間隔
        float ghostDispersal{ 0.35f };
        // ハローの幅
        float haloWidth{ 0.35f };
        // 色ずれの強度
        float chromaticAberration{ 0.06f };
        // 光の筋の強度
        float streakIntensity{ 0.18f };
        // 光の筋の長さ
        float streakLength{ 0.22f };
        // 半円を等分する筋の方向数、1～4
        std::uint32_t streakDirections{ 1 };
        // 最初の筋の角度、0度は水平
        float streakAngleDegrees{ 0.0f };
    };

    // メインパスの深度で錯乱円を求め、半解像度でぼかす被写界深度。
    struct DepthOfFieldSettings final
    {
        // 被写界深度の有効有無
        bool enabled{};
        // ピントを合わせるワールド距離
        float focusDistance{ 10.0f };
        // 焦点周りの無ぼけ距離幅
        float focusRange{ 2.0f };
        // 焦点からのずれによるぼけ倍率
        float blurStrength{ 1.0f };
        // ぼけ半径の画素上限
        float maximumRadius{ 10.0f };
    };

    // 深度の再投影でカメラ移動によるブレを描き、物体単独の移動は扱わない。
    struct MotionBlurSettings final
    {
        // カメラブラーの有効有無
        bool enabled{};
        // フレーム間の画面移動量の倍率
        float intensity{ 0.5f };
        // ブレ幅の画素上限
        float maximumRadius{ 16.0f };
    };

    // トーンマップ前のHDR輝度から露出段数を求め、手動露出へ加算する自動露出。
    struct AutoExposureSettings final
    {
        // 自動露出の有効有無
        bool enabled{};
        // 目標の中間グレー輝度
        float keyValue{ 0.18f };
        // 測定輝度の下限
        float minimumLuminance{ 0.02f };
        // 測定輝度の上限
        float maximumLuminance{ 8.0f };
        // 明所への順応速度毎秒
        float speedToBright{ 3.0f };
        // 暗所への順応速度毎秒
        float speedToDark{ 1.0f };
    };

    // 深度から接地部や隙間の陰りを求める画面空間遮蔽。
    struct AmbientOcclusionSettings final
    {
        // 画面空間遮蔽の有効有無
        bool enabled{};
        // 遮蔽を探すワールド距離
        float radius{ 0.5f };
        // 遮蔽の濃さ、0～1
        float strength{ 0.6f };
    };

    // 射影ずらしと履歴の再投影で平滑化し、速度バッファーを使わず近傍クランプで残像を抑える。
    struct TemporalAntiAliasingSettings final
    {
        // 時間的平滑化の有効有無
        bool enabled{};
        // 前フレームを残す比率
        float historyWeight{ 0.9f };
        // 射影ずらしの画素倍率
        float jitterScale{ 1.0f };
        // 履歴の近傍クランプの許容幅
        float clampTolerance{ 1.0f };
    };

    // 現在の行列をSceneが渡し、履歴・深度・前フレームの行列はRenderTargetが補うTAA入力。
    struct TemporalAntiAliasingInputs final
    {
        // 現在のビュー射影の逆行列
        DirectX::XMFLOAT4X4 inverseViewProjection{};
        // 現在のビュー射影行列
        DirectX::XMFLOAT4X4 viewProjection{};
    };

    // 深度と前フレームのHDRを使う反射で、画面外の不足分はプローブまたはSky環境へ戻す。
    struct ScreenSpaceReflectionSettings final
    {
        // 画面空間反射の有効有無
        bool enabled{};
        // 画面空間反射の強度
        float intensity{ 1.0f };
        // 反射を探索するワールド距離上限
        float maximumDistance{ 12.0f };
        // 反射を探索する反復上限
        std::uint32_t stepCount{ 48 };
        // 交差と見なすワールド深度厚さ
        float thickness{ 1.2f };
        // 反射を適用する粗さの上限
        float roughnessCutoff{ 0.45f };
    };

    // カスケード影で光の散乱を求めるため、影が有効な方向ライトを必要とする。
    struct VolumetricLightSettings final
    {
        // 散乱光の有効有無
        bool enabled{};
        // 散乱光の強度
        float intensity{ 0.35f };
        // レイに沿ったサンプル数
        std::uint32_t sampleCount{ 32 };
        // 散乱光を調べるワールド距離上限
        float maximumDistance{ 24.0f };
        // 前方散乱の強さ、0～0.95
        float scattering{ 0.6f };
    };

    // Sceneが渡す散乱光の入力で、深度はRenderTargetが補う。
    struct VolumetricLightInputs final
    {
        // 方向影のカスケードビュー
        GraphicsViewHandle cascadeShadow;
        // ワールド位置を復元する逆行列
        DirectX::XMFLOAT4X4 inverseViewProjection{};
        // カメラのワールド位置
        DirectX::XMFLOAT3 cameraPosition{};
        // 光が進むワールド方向
        DirectX::XMFLOAT3 lightDirection{};
        // 光のRGB色
        DirectX::XMFLOAT3 lightColor{ 1.0f, 1.0f, 1.0f };
        // カスケード別のライト射影行列
        std::array<DirectX::XMFLOAT4X4, 4>
            cascadeViewProjections{};
        // 使用する影カスケード数
        std::uint32_t cascadeCount{};
        // 影の深度比較バイアス
        float shadowBias{ 0.002f };
        // 影マップ一辺の解像度
        float shadowResolution{ 2048.0f };
    };

    // 3D格子のプローブで一回反射の間接光を保持し、設定変更後は手動で再ベイクする。
    struct BakedGlobalIlluminationSettings final
    {
        // ベイク間接光の有効有無
        bool enabled{};
        // ベイク領域のワールド中心
        DirectX::XMFLOAT3 center{};
        // ベイク領域のワールドサイズ
        DirectX::XMFLOAT3 size{ 20.0f, 10.0f, 20.0f };
        // 横方向のプローブ数
        std::uint32_t resolutionX{ 8 };
        // 縦方向のプローブ数
        std::uint32_t resolutionY{ 4 };
        // 奥行方向のプローブ数
        std::uint32_t resolutionZ{ 8 };
        // ベイク間接光の適用強度
        float intensity{ 1.0f };
    };

    // 間接光プローブの各軸上限
    inline constexpr std::uint32_t
        BakedGlobalIlluminationMaximumAxisResolution = 64;
    // 間接光プローブの総数上限
    inline constexpr std::size_t
        BakedGlobalIlluminationMaximumProbeCount = 32768;
    // プローブごとのRGB球面調和係数数
    inline constexpr std::size_t
        BakedGlobalIlluminationCoefficientsPerProbe = 12;


    // 各軸1～64かつ総数32768以下のプローブ数を返し、不正なら空を返す(resolutionX: 横分割数, resolutionY: 縦分割数, resolutionZ: 奥行分割数)。
    [[nodiscard]] inline constexpr std::optional<std::size_t>
        BakedGlobalIlluminationProbeCount(
            const std::uint32_t resolutionX,
            const std::uint32_t resolutionY,
            const std::uint32_t resolutionZ) noexcept
    {
        if (resolutionX == 0
            || resolutionX
                > BakedGlobalIlluminationMaximumAxisResolution
            || resolutionY == 0
            || resolutionY
                > BakedGlobalIlluminationMaximumAxisResolution
            || resolutionZ == 0
            || resolutionZ
                > BakedGlobalIlluminationMaximumAxisResolution)
        {
            return std::nullopt;
        }
        // 三軸を乗じたプローブ総数
        const auto count =
            static_cast<std::uint64_t>(resolutionX)
            * resolutionY
            * resolutionZ;
        if (count > BakedGlobalIlluminationMaximumProbeCount)
        {
            return std::nullopt;
        }
        return static_cast<std::size_t>(count);
    }

    struct ColorGradingSettings final
    {
        // トーンマップの有効有無
        bool toneMappingEnabled{ true };
        // 色調整の有効有無
        bool enabled{ true };
        // 手動露出の補正段数
        float exposure{ 0.15f };
        // コントラスト倍率
        float contrast{ 1.05f };
        // 彩度倍率
        float saturation{ 1.08f };
        // 色温度の補正量
        float temperature{ 0.02f };
        // 緑とマゼンタの色補正量
        float tint{};
        // 周辺減光の強度
        float vignette{ 0.12f };
        // 毎フレーム埋める自動露出段数
        // 保存用設定ではなく手動露出へ加算する作業値で、色調整の有効有無に左右されない。
        float autoExposureStops{};
    };
}
