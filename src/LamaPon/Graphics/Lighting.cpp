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
            XMFLOAT3 direction{};
            XMFLOAT3 color{};
            float strength{};
        };

        const XMVECTOR ambient = XMVectorScale(
            XMLoadFloat3(&lighting.ambientColor),
            std::max(lighting.ambientIntensity, 0.0f));

        // DGSLEffectだけがIEffectLights::MaxDirectionalLights(3)を
        // 4へ上書きしている。他の実装（BasicEffect/SkinnedEffect/
        // EnvironmentMapEffect等）へ3を超えるwhichLightを渡すと
        // DirectXTK側がstd::out_of_rangeを投げるため、実際の実装型に
        // 応じて上限を切り替える。
        std::size_t effectMaxLights =
            static_cast<std::size_t>(
                IEffectLights::MaxDirectionalLights);

        if (auto* basic =
            dynamic_cast<BasicEffect*>(&effect))
        {
            basic->SetLightingEnabled(true);
            basic->SetPerPixelLighting(true);
        }
        else if (auto* dgsl =
            dynamic_cast<DGSLEffect*>(&effect))
        {
            dgsl->SetLightingEnabled(true);
            effectMaxLights =
                static_cast<std::size_t>(
                    DGSLEffect::MaxDirectionalLights);
        }
        else if (auto* skinned =
            dynamic_cast<SkinnedEffect*>(&effect))
        {
            skinned->SetPerPixelLighting(true);
        }
        else if (auto* environment =
            dynamic_cast<EnvironmentMapEffect*>(&effect))
        {
            environment->SetPerPixelLighting(true);
        }
        effect.SetAmbientLightColor(ambient);
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

        std::array<
            Candidate,
            MaximumDirectionalLights
                + MaximumPointLights
                + MaximumSpotLights> candidates{};
        std::size_t candidateCount{};

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

        for (std::size_t index = 0;
            index < lighting.directionalLightCount;
            ++index)
        {
            const auto& light = lighting.directionalLights[index];
            addCandidate(
                light.direction,
                light.color,
                std::max(light.intensity, 0.0f));
        }

        const XMVECTOR object = XMLoadFloat3(&objectPosition);
        for (std::size_t index = 0;
            index < lighting.pointLightCount;
            ++index)
        {
            const auto& light = lighting.pointLights[index];
            const XMVECTOR fromLight = XMVectorSubtract(
                object,
                XMLoadFloat3(&light.position));
            const float distance =
                XMVectorGetX(XMVector3Length(fromLight));
            const float safeRange = std::max(light.range, 0.001f);
            if (distance >= safeRange)
            {
                continue;
            }

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
            const float falloff =
                1.0f - distance / safeRange;
            addCandidate(
                direction,
                light.color,
                std::max(light.intensity, 0.0f)
                    * falloff
                    * falloff);
        }

        for (std::size_t index = 0;
            index < lighting.spotLightCount;
            ++index)
        {
            const auto& light = lighting.spotLights[index];
            const XMVECTOR fromLight = XMVectorSubtract(
                object,
                XMLoadFloat3(&light.position));
            const float distance =
                XMVectorGetX(XMVector3Length(fromLight));
            const float safeRange = std::max(light.range, 0.001f);
            if (distance >= safeRange)
            {
                continue;
            }

            const XMVECTOR lightToObject = distance <= 0.0001f
                ? XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f)
                : XMVectorScale(fromLight, 1.0f / distance);
            const float cone = XMVectorGetX(XMVector3Dot(
                XMVector3Normalize(
                    XMLoadFloat3(&light.direction)),
                lightToObject));
            const float coneWidth = std::max(
                light.innerConeCosine - light.outerConeCosine,
                0.0001f);
            const float coneAttenuation = std::clamp(
                (cone - light.outerConeCosine) / coneWidth,
                0.0f,
                1.0f);
            if (coneAttenuation <= 0.0f)
            {
                continue;
            }

            XMFLOAT3 direction{};
            XMStoreFloat3(&direction, lightToObject);
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

        const std::size_t lightCount = std::min(
            MaximumDirectionalLights,
            effectMaxLights);
        for (std::size_t index = 0;
            index < lightCount;
            ++index)
        {
            const int lightIndex = static_cast<int>(index);
            const bool enabled = index < candidateCount;
            effect.SetLightEnabled(lightIndex, enabled);
            if (!enabled)
            {
                continue;
            }

            const auto& light = candidates[index];
            const XMVECTOR direction = XMVector3Normalize(
                XMLoadFloat3(&light.direction));
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
