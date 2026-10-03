#include "LamaPon/Graphics/Lighting.h"

#include <Effects.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

namespace LamaPon
{
    void ApplyLighting(
        DirectX::IEffectLights& effect,
        const LightingState& lighting,
        const DirectX::XMFLOAT3& objectPosition)
    {
        using namespace DirectX;

        struct Candidate final
        {
            // 光が進む方向
            XMFLOAT3 direction{};
            // 候補の強度を適用した光色
            XMFLOAT3 color{};
            // 物体位置での実効強度
            float strength{};
        };

        // 強度を適用した環境光色
        const XMVECTOR ambient = XMVectorScale(
            XMLoadFloat3(&lighting.ambientColor),
            std::max(lighting.ambientIntensity, 0.0f));

        // DirectXTKの実装ごとのライト上限を使い、未対応の添字による例外を避ける。
        // 効果の実装が扱えるライト上限
        std::size_t effectMaxLights =
            static_cast<std::size_t>(
                IEffectLights::MaxDirectionalLights);

        // 基本照明効果の参照
        if (auto* basic =
            dynamic_cast<BasicEffect*>(&effect))
        {
            basic->SetLightingEnabled(true);
            basic->SetPerPixelLighting(true);
        }
        // DGSL照明効果の参照
        else if (auto* dgsl =
            dynamic_cast<DGSLEffect*>(&effect))
        {
            dgsl->SetLightingEnabled(true);
            effectMaxLights =
                static_cast<std::size_t>(
                    DGSLEffect::MaxDirectionalLights);
        }
        // スキン用照明効果の参照
        else if (auto* skinned =
            dynamic_cast<SkinnedEffect*>(&effect))
        {
            skinned->SetPerPixelLighting(true);
        }
        // 環境マップ照明効果の参照
        else if (auto* environment =
            dynamic_cast<EnvironmentMapEffect*>(&effect))
        {
            environment->SetPerPixelLighting(true);
        }
        effect.SetAmbientLightColor(ambient);
        // 霧対応効果の参照
        if (auto* fog =
            dynamic_cast<IEffectFog*>(&effect))
        {
            fog->SetFogEnabled(
                lighting.fog.enabled);
            fog->SetFogStart(
                lighting.fog.startDistance);
            fog->SetFogEnd(
                lighting.fog.endDistance);
            fog->SetFogColor(
                XMLoadFloat3(
                    &lighting.fog.color));
        }

        // 距離とコーン減衰を適用した候補
        std::array<
            Candidate,
            MaximumDirectionalLights
                + MaximumPointLights
                + MaximumSpotLights> candidates{};
        // 登録済みのライト候補数
        std::size_t candidateCount{};

        // 弱い光と上限超過を除いて候補へ加える(direction: 光が進む方向, color: 光の色, strength: 位置での実効強度)。
        const auto addCandidate =
            [&candidates, &candidateCount](
                const XMFLOAT3& direction,
                const XMFLOAT3& color,
                const float strength)
            {
                if (candidateCount >= candidates.size()
                    || strength <= 0.0001f)
                {
                    return;
                }
                candidates[candidateCount++] = {
                    direction,
                    color,
                    strength
                };
            };

        // 評価・適用するライトの添字
        for (std::size_t index = 0;
            index < lighting.directionalLightCount;
            ++index)
        {
            // 評価・適用するライト情報
            const auto& light = lighting.directionalLights[index];
            addCandidate(
                light.direction,
                light.color,
                std::max(light.intensity, 0.0f));
        }

        // 物体のワールド位置
        const XMVECTOR object = XMLoadFloat3(&objectPosition);
        // 評価・適用するライトの添字
        for (std::size_t index = 0;
            index < lighting.pointLightCount;
            ++index)
        {
            // 評価・適用するライト情報
            const auto& light = lighting.pointLights[index];
            // 光源から物体へのベクトル
            const XMVECTOR fromLight = XMVectorSubtract(
                object,
                XMLoadFloat3(&light.position));
            // 光源から物体までの距離
            const float distance =
                XMVectorGetX(XMVector3Length(fromLight));
            // ゼロを除いた光の到達距離
            const float safeRange = std::max(light.range, 0.001f);
            if (distance >= safeRange)
            {
                continue;
            }

            // 光が進む方向
            XMFLOAT3 direction{};
            if (distance <= 0.0001f)
            {
                direction = { 0.0f, -1.0f, 0.0f };
            }
            else
            {
                XMStoreFloat3(
                    &direction,
                    XMVectorScale(fromLight, 1.0f / distance));
            }
            // 到達距離による減衰係数
            const float falloff =
                1.0f - distance / safeRange;
            addCandidate(
                direction,
                light.color,
                std::max(light.intensity, 0.0f)
                    * falloff
                    * falloff);
        }

        // 評価・適用するライトの添字
        for (std::size_t index = 0;
            index < lighting.spotLightCount;
            ++index)
        {
            // 評価・適用するライト情報
            const auto& light = lighting.spotLights[index];
            // 光源から物体へのベクトル
            const XMVECTOR fromLight = XMVectorSubtract(
                object,
                XMLoadFloat3(&light.position));
            // 光源から物体までの距離
            const float distance =
                XMVectorGetX(XMVector3Length(fromLight));
            // ゼロを除いた光の到達距離
            const float safeRange = std::max(light.range, 0.001f);
            if (distance >= safeRange)
            {
                continue;
            }

            // 光源から物体への単位方向
            const XMVECTOR lightToObject = distance <= 0.0001f
                ? XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f)
                : XMVectorScale(fromLight, 1.0f / distance);
            // 光の向きと物体方向の内積
            const float cone = XMVectorGetX(XMVector3Dot(
                XMVector3Normalize(
                    XMLoadFloat3(&light.direction)),
                lightToObject));
            // 内外コーン余弦の差
            const float coneWidth = std::max(
                light.innerConeCosine - light.outerConeCosine,
                0.0001f);
            // コーン内の減衰係数
            const float coneAttenuation = std::clamp(
                (cone - light.outerConeCosine) / coneWidth,
                0.0f,
                1.0f);
            if (coneAttenuation <= 0.0f)
            {
                continue;
            }

            // 光が進む方向
            XMFLOAT3 direction{};
            XMStoreFloat3(&direction, lightToObject);
            // 到達距離による減衰係数
            const float falloff =
                1.0f - distance / safeRange;
            addCandidate(
                direction,
                light.color,
                std::max(light.intensity, 0.0f)
                    * falloff
                    * falloff
                    * coneAttenuation
                    * coneAttenuation);
        }

        std::ranges::sort(
            candidates.begin(),
            candidates.begin()
                + static_cast<std::ptrdiff_t>(candidateCount),
            std::greater{},
            &Candidate::strength);

        // 効果に設定するライト枠数
        const std::size_t lightCount = std::min(
            MaximumDirectionalLights,
            effectMaxLights);
        // 評価・適用するライトの添字
        for (std::size_t index = 0;
            // 効果に設定するライト枠数
            index < lightCount;
            ++index)
        {
            // 効果に設定する整数添字
            const int lightIndex = static_cast<int>(index);
            // 対応するライト候補の有無
            const bool enabled = index < candidateCount;
            effect.SetLightEnabled(lightIndex, enabled);
            if (!enabled)
            {
                continue;
            }

            // 評価・適用するライト情報
            const auto& light = candidates[index];
            // 光が進む方向
            const XMVECTOR direction = XMVector3Normalize(
                XMLoadFloat3(&light.direction));
            // 候補の強度を適用した光色
            const XMVECTOR color = XMVectorScale(
                XMLoadFloat3(&light.color),
                light.strength);

            effect.SetLightDirection(lightIndex, direction);
            effect.SetLightDiffuseColor(lightIndex, color);
            if (dynamic_cast<EnvironmentMapEffect*>(
                &effect) == nullptr)
            {
                effect.SetLightSpecularColor(
                    lightIndex,
                    XMVectorScale(color, 0.35f));
            }
        }
    }
}
