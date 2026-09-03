#include "LamaPon/Graphics/SkeletalModel.h"

#include "LamaPon/Graphics/ShaderRenderState.h"

#include "LamaPon/Core/Profiler.h"
#include "LamaPon/Graphics/Lighting.h"
#include "LamaPon/Graphics/LitEffect.h"
#include "LamaPon/Graphics/LitMaterial.h"

#include <CommonStates.h>
#include <Effects.h>
#include <VertexTypes.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <stdexcept>

namespace
{
    bool IsOutsideClipBounds(
        const LamaPon::Bounds3D& bounds,
        DirectX::FXMMATRIX localToClip) noexcept
    {
        std::array<bool, 6> allOutside{
            true, true, true, true, true, true
        };
        for (const int x : { -1, 1 })
        {
            for (const int y : { -1, 1 })
            {
                for (const int z : { -1, 1 })
                {
                    DirectX::XMFLOAT4 clip{};
                    DirectX::XMStoreFloat4(
                        &clip,
                        DirectX::XMVector4Transform(
                            DirectX::XMVectorSet(
                                x < 0
                                    ? bounds.minimum.x
                                    : bounds.maximum.x,
                                y < 0
                                    ? bounds.minimum.y
                                    : bounds.maximum.y,
                                z < 0
                                    ? bounds.minimum.z
                                    : bounds.maximum.z,
                                1.0f),
                            localToClip));
                    if (!std::isfinite(clip.x)
                        || !std::isfinite(clip.y)
                        || !std::isfinite(clip.z)
                        || !std::isfinite(clip.w))
                    {
                        return false;
                    }
                    const std::array distances{
                        clip.x + clip.w,
                        clip.w - clip.x,
                        clip.y + clip.w,
                        clip.w - clip.y,
                        clip.z,
                        clip.w - clip.z
                    };
                    for (std::size_t plane = 0;
                        plane < distances.size();
                        ++plane)
                    {
                        if (distances[plane] >= 0.0f)
                        {
                            allOutside[plane] = false;
                        }
                    }
                }
            }
        }
        return std::ranges::any_of(
            allOutside,
            [](const bool outside)
            {
                return outside;
            });
    }

    template<typename Key>
    std::size_t UpperKeyIndex(
        const std::vector<Key>& keys,
        const float time)
    {
        return static_cast<std::size_t>(
            std::ranges::upper_bound(
                keys,
                time,
                {},
                &Key::time) - keys.begin());
    }

    DirectX::XMFLOAT3 SampleVector(
        const LamaPon::SkeletalVectorChannel& channel,
        const float time,
        const DirectX::XMFLOAT3& fallback)
    {
        using namespace DirectX;
        if (channel.keys.empty())
        {
            return fallback;
        }
        if (channel.keys.size() == 1
            || time <= channel.keys.front().time)
        {
            return channel.keys.front().value;
        }
        if (time >= channel.keys.back().time)
        {
            return channel.keys.back().value;
        }

        const std::size_t upper =
            UpperKeyIndex(channel.keys, time);
        const auto& left = channel.keys[upper - 1];
        const auto& right = channel.keys[upper];
        if (channel.interpolation
            == LamaPon::SkeletalInterpolation::Step)
        {
            return left.value;
        }

        const float span =
            std::max(right.time - left.time, 0.000001f);
        const float amount =
            std::clamp((time - left.time) / span, 0.0f, 1.0f);
        XMVECTOR result{};
        if (channel.interpolation
            == LamaPon::SkeletalInterpolation::CubicSpline)
        {
            const float t2 = amount * amount;
            const float t3 = t2 * amount;
            const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
            const float h10 = t3 - 2.0f * t2 + amount;
            const float h01 = -2.0f * t3 + 3.0f * t2;
            const float h11 = t3 - t2;
            result =
                XMVectorScale(XMLoadFloat3(&left.value), h00)
                + XMVectorScale(
                    XMLoadFloat3(&left.outTangent),
                    h10 * span)
                + XMVectorScale(XMLoadFloat3(&right.value), h01)
                + XMVectorScale(
                    XMLoadFloat3(&right.inTangent),
                    h11 * span);
        }
        else
        {
            result = XMVectorLerp(
                XMLoadFloat3(&left.value),
                XMLoadFloat3(&right.value),
                amount);
        }

        XMFLOAT3 sampled{};
        XMStoreFloat3(&sampled, result);
        return sampled;
    }

    DirectX::XMFLOAT4 SampleQuaternion(
        const LamaPon::SkeletalQuaternionChannel& channel,
        const float time,
        const DirectX::XMFLOAT4& fallback)
    {
        using namespace DirectX;
        if (channel.keys.empty())
        {
            return fallback;
        }
        if (channel.keys.size() == 1
            || time <= channel.keys.front().time)
        {
            return channel.keys.front().value;
        }
        if (time >= channel.keys.back().time)
        {
            return channel.keys.back().value;
        }

        const std::size_t upper =
            UpperKeyIndex(channel.keys, time);
        const auto& left = channel.keys[upper - 1];
        const auto& right = channel.keys[upper];
        if (channel.interpolation
            == LamaPon::SkeletalInterpolation::Step)
        {
            return left.value;
        }

        const float span =
            std::max(right.time - left.time, 0.000001f);
        const float amount =
            std::clamp((time - left.time) / span, 0.0f, 1.0f);
        XMVECTOR result{};
        if (channel.interpolation
            == LamaPon::SkeletalInterpolation::CubicSpline)
        {
            const float t2 = amount * amount;
            const float t3 = t2 * amount;
            const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
            const float h10 = t3 - 2.0f * t2 + amount;
            const float h01 = -2.0f * t3 + 3.0f * t2;
            const float h11 = t3 - t2;
            result =
                XMVectorScale(XMLoadFloat4(&left.value), h00)
                + XMVectorScale(
                    XMLoadFloat4(&left.outTangent),
                    h10 * span)
                + XMVectorScale(XMLoadFloat4(&right.value), h01)
                + XMVectorScale(
                    XMLoadFloat4(&right.inTangent),
                    h11 * span);
            result = XMQuaternionNormalize(result);
        }
        else
        {
            XMVECTOR rightValue =
                XMLoadFloat4(&right.value);
            const XMVECTOR leftValue =
                XMLoadFloat4(&left.value);
            if (XMVectorGetX(
                    XMVector4Dot(leftValue, rightValue)) < 0.0f)
            {
                rightValue = XMVectorNegate(rightValue);
            }
            result = XMQuaternionSlerp(
                leftValue,
                rightValue,
                amount);
        }

        XMFLOAT4 sampled{};
        XMStoreFloat4(&sampled, result);
        return sampled;
    }

    float SpecularPowerFromRoughness(const float roughness) noexcept
    {
        const float squared = roughness * roughness;
        return std::clamp(
            2.0f / std::max(squared * squared, 0.0001f) - 2.0f,
            1.0f,
            128.0f);
    }

    void SampleLocalPose(
        const std::vector<LamaPon::SkeletalNode>& nodes,
        const LamaPon::SkeletalAnimationClip* clip,
        const float time,
        std::vector<LamaPon::SkeletalPoseTransform>& localPose)
    {
        localPose.resize(nodes.size());
        for (std::size_t index = 0; index < nodes.size(); ++index)
        {
            localPose[index] = nodes[index].bindPose;
        }
        if (clip == nullptr)
        {
            return;
        }
        for (const auto& track : clip->tracks)
        {
            if (track.node >= localPose.size())
            {
                continue;
            }
            auto& pose = localPose[track.node];
            pose.translation = SampleVector(
                track.translation,
                time,
                pose.translation);
            pose.rotation = SampleQuaternion(
                track.rotation,
                time,
                pose.rotation);
            pose.scale = SampleVector(
                track.scale,
                time,
                pose.scale);
        }
    }

    void BuildGlobalPose(
        const std::vector<LamaPon::SkeletalNode>& nodes,
        const std::vector<LamaPon::SkeletalPoseTransform>& localPose,
        std::vector<DirectX::XMFLOAT4X4>& globalPose)
    {
        globalPose.resize(nodes.size());
        std::vector<unsigned char> state(nodes.size());
        std::function<DirectX::XMMATRIX(std::size_t)> resolve =
            [&](const std::size_t index)
            {
                using namespace DirectX;
                if (state[index] == 2)
                {
                    return XMLoadFloat4x4(&globalPose[index]);
                }
                if (state[index] == 1)
                {
                    throw std::runtime_error(
                        "Skeleton contains a cyclic node hierarchy.");
                }
                state[index] = 1;
                XMMATRIX global =
                    LamaPon::SkeletalModel::LocalMatrix(
                        localPose[index]);
                const auto parent = nodes[index].parent;
                if (parent >= 0)
                {
                    const auto parentIndex =
                        static_cast<std::size_t>(parent);
                    if (parentIndex >= nodes.size())
                    {
                        throw std::runtime_error(
                            "Skeleton node has an invalid parent.");
                    }
                    global *= resolve(parentIndex);
                }
                XMStoreFloat4x4(&globalPose[index], global);
                state[index] = 2;
                return global;
            };

        for (std::size_t index = 0; index < nodes.size(); ++index)
        {
            static_cast<void>(resolve(index));
        }
    }
}

namespace LamaPon
{
    std::size_t SkeletalModel::SelectAutomaticLod(
        DirectX::FXMMATRIX ownerWorld,
        DirectX::CXMMATRIX view,
        DirectX::CXMMATRIX projection,
        const float quality) const noexcept
    {
        if (!hasLocalBounds)
        {
            return 0;
        }

        const DirectX::XMVECTOR center = DirectX::XMVectorSet(
            (localBounds.minimum.x + localBounds.maximum.x) * 0.5f,
            (localBounds.minimum.y + localBounds.maximum.y) * 0.5f,
            (localBounds.minimum.z + localBounds.maximum.z) * 0.5f,
            1.0f);
        const float halfX = std::abs(
            localBounds.maximum.x - localBounds.minimum.x) * 0.5f;
        const float halfY = std::abs(
            localBounds.maximum.y - localBounds.minimum.y) * 0.5f;
        const float halfZ = std::abs(
            localBounds.maximum.z - localBounds.minimum.z) * 0.5f;
        const float localRadius = std::sqrt(
            halfX * halfX + halfY * halfY + halfZ * halfZ);
        const float scale = std::max({
            DirectX::XMVectorGetX(
                DirectX::XMVector3Length(ownerWorld.r[0])),
            DirectX::XMVectorGetX(
                DirectX::XMVector3Length(ownerWorld.r[1])),
            DirectX::XMVectorGetX(
                DirectX::XMVector3Length(ownerWorld.r[2]))
        });
        const auto viewPosition = DirectX::XMVector4Transform(
            DirectX::XMVector4Transform(center, ownerWorld),
            view);
        const float distance = std::max(
            std::abs(DirectX::XMVectorGetZ(viewPosition)),
            0.001f);
        const float focalScale = std::max(
            std::abs(
                DirectX::XMVectorGetY(projection.r[1])),
            0.001f);
        const float projectedRadius =
            localRadius * scale * focalScale / distance
            * std::clamp(quality, 0.25f, 2.0f);

        // NDC半径がおよそ画面高の4%未満ならLOD2、12%未満なら
        // LOD1です。距離そのものではなく画面上の大きさで選ぶため、
        // モデルの実寸やFOVが違っても同じ見え方になります。
        if (projectedRadius < 0.04f)
        {
            return 2;
        }
        if (projectedRadius < 0.12f)
        {
            return 1;
        }
        return 0;
    }

    std::uint64_t SkeletalModel::TriangleCount(
        const std::size_t lodLevel) const noexcept
    {
        std::uint64_t indexCount{};
        for (const auto& primitive : primitives)
        {
            std::uint32_t selected = primitive.indexCount;
            if (lodLevel > 0)
            {
                for (std::size_t level = std::min<std::size_t>(
                        lodLevel,
                        primitive.lodIndexCounts.size());
                    level > 0;
                    --level)
                {
                    if (primitive.lodIndexCounts[level - 1] > 0)
                    {
                        selected = primitive.lodIndexCounts[level - 1];
                        break;
                    }
                }
            }
            indexCount += selected;
        }
        return indexCount / 3u;
    }

    DirectX::XMMATRIX SkeletalModel::LocalMatrix(
        const SkeletalPoseTransform& transform) noexcept
    {
        using namespace DirectX;
        return XMMatrixScaling(
                transform.scale.x,
                transform.scale.y,
                transform.scale.z)
            * XMMatrixRotationQuaternion(
                XMLoadFloat4(&transform.rotation))
            * XMMatrixTranslation(
                transform.translation.x,
                transform.translation.y,
                transform.translation.z);
    }

    void SkeletalModel::SamplePose(
        const std::vector<SkeletalNode>& nodes,
        const SkeletalAnimationClip* clip,
        const float time,
        std::vector<SkeletalPoseTransform>& localPose,
        std::vector<DirectX::XMFLOAT4X4>& globalPose)
    {
        SampleLocalPose(nodes, clip, time, localPose);
        BuildGlobalPose(nodes, localPose, globalPose);
    }

    void SkeletalModel::SampleBlendedPose(
        const std::vector<SkeletalNode>& nodes,
        const SkeletalAnimationClip* fromClip,
        const float fromTime,
        const SkeletalAnimationClip* toClip,
        const float toTime,
        const float amount,
        std::vector<SkeletalPoseTransform>& localPose,
        std::vector<DirectX::XMFLOAT4X4>& globalPose)
    {
        std::vector<SkeletalPoseTransform> fromPose;
        std::vector<SkeletalPoseTransform> toPose;
        SampleLocalPose(
            nodes,
            fromClip,
            fromTime,
            fromPose);
        SampleLocalPose(
            nodes,
            toClip,
            toTime,
            toPose);

        const float blend = std::clamp(amount, 0.0f, 1.0f);
        localPose.resize(nodes.size());
        for (std::size_t index = 0; index < nodes.size(); ++index)
        {
            using namespace DirectX;
            auto& result = localPose[index];
            const auto& from = fromPose[index];
            const auto& to = toPose[index];
            XMStoreFloat3(
                &result.translation,
                XMVectorLerp(
                    XMLoadFloat3(&from.translation),
                    XMLoadFloat3(&to.translation),
                    blend));
            XMVECTOR fromRotation =
                XMLoadFloat4(&from.rotation);
            XMVECTOR toRotation =
                XMLoadFloat4(&to.rotation);
            if (XMVectorGetX(
                    XMVector4Dot(
                        fromRotation,
                        toRotation)) < 0.0f)
            {
                toRotation = XMVectorNegate(toRotation);
            }
            XMStoreFloat4(
                &result.rotation,
                XMQuaternionNormalize(
                    XMQuaternionSlerp(
                        fromRotation,
                        toRotation,
                        blend)));
            XMStoreFloat3(
                &result.scale,
                XMVectorLerp(
                    XMLoadFloat3(&from.scale),
                    XMLoadFloat3(&to.scale),
                    blend));
        }
        BuildGlobalPose(nodes, localPose, globalPose);
    }

    void SkeletalModel::SampleWeightedPose(
        const std::vector<SkeletalNode>& nodes,
        const std::vector<SkeletalPoseSample>& samples,
        std::vector<SkeletalPoseTransform>& localPose,
        std::vector<DirectX::XMFLOAT4X4>& globalPose,
        const std::size_t removeRootMotionNode)
    {
        localPose.clear();
        float totalWeight{};
        std::vector<SkeletalPoseTransform> sampledPose;
        for (const auto& sample : samples)
        {
            const float weight =
                std::max(sample.weight, 0.0f);
            if (weight <= 0.0f)
            {
                continue;
            }
            SampleLocalPose(
                nodes,
                sample.clip,
                sample.time,
                sampledPose);
            if (localPose.empty())
            {
                localPose = sampledPose;
                totalWeight = weight;
                continue;
            }

            const float newTotal =
                totalWeight + weight;
            const float amount = weight / newTotal;
            for (std::size_t index = 0;
                index < localPose.size();
                ++index)
            {
                using namespace DirectX;
                auto& result = localPose[index];
                const auto& next = sampledPose[index];
                XMStoreFloat3(
                    &result.translation,
                    XMVectorLerp(
                        XMLoadFloat3(&result.translation),
                        XMLoadFloat3(&next.translation),
                        amount));
                XMVECTOR fromRotation =
                    XMLoadFloat4(&result.rotation);
                XMVECTOR toRotation =
                    XMLoadFloat4(&next.rotation);
                if (XMVectorGetX(
                        XMVector4Dot(
                            fromRotation,
                            toRotation)) < 0.0f)
                {
                    toRotation =
                        XMVectorNegate(toRotation);
                }
                XMStoreFloat4(
                    &result.rotation,
                    XMQuaternionNormalize(
                        XMQuaternionSlerp(
                            fromRotation,
                            toRotation,
                            amount)));
                XMStoreFloat3(
                    &result.scale,
                    XMVectorLerp(
                        XMLoadFloat3(&result.scale),
                        XMLoadFloat3(&next.scale),
                        amount));
            }
            totalWeight = newTotal;
        }
        if (localPose.empty())
        {
            SampleLocalPose(
                nodes,
                nullptr,
                0.0f,
                localPose);
        }
        if (removeRootMotionNode < localPose.size())
        {
            localPose[removeRootMotionNode].translation =
                nodes[removeRootMotionNode]
                    .bindPose.translation;
            localPose[removeRootMotionNode].rotation =
                nodes[removeRootMotionNode]
                    .bindPose.rotation;
        }
        BuildGlobalPose(nodes, localPose, globalPose);
    }

    void SkeletalModel::Draw(
        ID3D11DeviceContext* context,
        DirectX::CommonStates& states,
        const LightingState& lighting,
        DirectX::FXMMATRIX ownerWorld,
        DirectX::CXMMATRIX view,
        DirectX::CXMMATRIX projection,
        const SkeletalAnimationClip* clip,
        const float time,
        const bool wireframe,
        const LitMaterial* materialOverride,
        ID3D11ShaderResourceView* albedoOverride,
        ID3D11ShaderResourceView* normalOverride,
        const PbrTextures* pbrOverride,
        const SkeletalAnimationClip* blendClip,
        const float blendTime,
        const float blendAmount,
        const std::vector<SkeletalPoseSample>*
            weightedSamples,
        const std::size_t removeRootMotionNode,
        LitEffect* customEffect,
        ID3D11InputLayout* customInputLayout,
        // ヘッダのコメント参照。既定値はヘッダ側にあります。
        const bool depthOnly,
        const std::vector<DirectX::XMFLOAT4X4>*
            globalPoseOverride,
        const float automaticLodQuality) const
    {
        using namespace DirectX;
        (void)customInputLayout;
        if (context == nullptr)
        {
            return;
        }

        std::vector<SkeletalPoseTransform> localPose;
        std::vector<XMFLOAT4X4> globalPose;
        if (globalPoseOverride == nullptr)
        {
            LAMAPON_PROFILE_SCOPE("SkeletalModel.Pose");
            if (weightedSamples != nullptr
                && !weightedSamples->empty())
            {
                SampleWeightedPose(
                    nodes,
                    *weightedSamples,
                    localPose,
                    globalPose,
                    removeRootMotionNode);
            }
            else if (blendClip != nullptr && blendAmount > 0.0f)
            {
                SampleBlendedPose(
                    nodes,
                    clip,
                    time,
                    blendClip,
                    blendTime,
                    blendAmount,
                    localPose,
                    globalPose);
            }
            else
            {
                SamplePose(nodes, clip, time, localPose, globalPose);
            }
        }
        const auto& resolvedGlobalPose =
            globalPoseOverride != nullptr
                ? *globalPoseOverride
                : globalPose;
        const XMMATRIX identity = XMMatrixIdentity();
        std::vector<XMMATRIX> palette;
        // マテリアル上書きが無いとき、モデル自身の材質からLit用の
        // マテリアルを組み立てます。プリミティブごとに作り直すと
        // カスタムベクトル分（1KB超）の初期化が毎回走るので、
        // 1個を使い回して値だけ差し替えます。
        LitMaterial primitiveMaterial;
        const std::size_t automaticLod = SelectAutomaticLod(
            ownerWorld,
            view,
            projection,
            automaticLodQuality);

        LAMAPON_PROFILE_SCOPE("SkeletalModel.Primitives");
        for (const bool alphaPass : { false, true })
        {
            for (const auto& primitive : primitives)
            {
                const bool useCustom =
                    customEffect != nullptr;
                // Lit（customEffect）経路もDirectXTKの頂点シェーダーで
                // スキニングするため、effectとinputLayoutはどちらの
                // 経路でも必須です。
                if (!primitive.vertexBuffer
                    || !primitive.indexBuffer
                    || !primitive.effect
                    || !primitive.inputLayout
                    || primitive.meshNode
                        >= resolvedGlobalPose.size())
                {
                    continue;
                }

                const float effectiveAlpha = materialOverride != nullptr
                    ? materialOverride->BaseColor().w
                    : primitive.baseColor.w;
                const bool usesAlpha =
                    primitive.alpha || effectiveAlpha < 0.999f;
                if (usesAlpha != alphaPass)
                {
                    continue;
                }

                const XMMATRIX meshGlobal =
                    XMLoadFloat4x4(
                        &resolvedGlobalPose[primitive.meshNode]);
                // 複数ノードを持つ静的モデルは、GameObjectへ展開せず
                // 子メッシュ単位で視錐台判定します。スキニング形状は
                // バインド姿勢の境界から変形するため、ここでは弾きません。
                if (primitives.size() > 1
                    && primitive.skin < 0
                    && primitive.hasLocalBounds
                    && IsOutsideClipBounds(
                        primitive.localBounds,
                        meshGlobal
                            * ownerWorld
                            * view
                            * projection))
                {
                    continue;
                }
                palette.clear();
                if (primitive.skin >= 0)
                {
                    const auto skinIndex =
                        static_cast<std::size_t>(primitive.skin);
                    if (skinIndex >= skins.size())
                    {
                        continue;
                    }
                    const auto& skin = skins[skinIndex];
                    palette.reserve(skin.joints.size());
                    const XMMATRIX inverseMesh =
                        XMMatrixInverse(nullptr, meshGlobal);
                    for (std::size_t index = 0;
                        index < skin.joints.size();
                        ++index)
                    {
                        const auto joint = skin.joints[index];
                        if (joint >= resolvedGlobalPose.size())
                        {
                            palette.push_back(identity);
                            continue;
                        }
                        const XMMATRIX inverseBind =
                            index < skin.inverseBindMatrices.size()
                                ? XMLoadFloat4x4(
                                    &skin.inverseBindMatrices[index])
                                : identity;
                        palette.push_back(
                            inverseBind
                            * XMLoadFloat4x4(
                                &resolvedGlobalPose[joint])
                            * inverseMesh);
                    }
                }
                if (palette.empty())
                {
                    palette.push_back(identity);
                }

                ID3D11Buffer* selectedIndexBuffer =
                    primitive.indexBuffer.Get();
                std::uint32_t selectedIndexCount =
                    primitive.indexCount;
                for (std::size_t level = std::min<std::size_t>(
                        automaticLod,
                        primitive.lodIndexBuffers.size());
                    level > 0;
                    --level)
                {
                    if (primitive.lodIndexBuffers[level - 1]
                        && primitive.lodIndexCounts[level - 1] > 0)
                    {
                        selectedIndexBuffer = primitive
                            .lodIndexBuffers[level - 1].Get();
                        selectedIndexCount = primitive
                            .lodIndexCounts[level - 1];
                        break;
                    }
                }

                XMFLOAT4 color = primitive.baseColor;
                float roughness = primitive.roughness;
                ID3D11ShaderResourceView* texture =
                    primitive.texture.Get();
                ID3D11ShaderResourceView* normalTexture =
                    primitive.normalTexture.Get();
                if (materialOverride != nullptr)
                {
                    color = materialOverride->BaseColor();
                    roughness = materialOverride->Roughness();
                    if (albedoOverride != nullptr)
                    {
                        texture = albedoOverride;
                    }
                    if (normalOverride != nullptr)
                    {
                        normalTexture = normalOverride;
                    }
                }

                XMFLOAT3 objectPosition{};
                XMStoreFloat3(
                    &objectPosition,
                    (meshGlobal * ownerWorld).r[3]);
                const auto configureEffect =
                    [&](auto& effect)
                    {
                        effect.SetBoneTransforms(
                            palette.data(),
                            palette.size());
                        effect.SetMatrices(
                            meshGlobal * ownerWorld,
                            view,
                            projection);
                        effect.SetDiffuseColor(XMLoadFloat4(&color));
                        effect.SetAlpha(color.w);
                        effect.SetSpecularColor(
                            XMVectorReplicate(
                                std::lerp(
                                    0.45f,
                                    0.08f,
                                    roughness)));
                        effect.SetSpecularPower(
                            SpecularPowerFromRoughness(roughness));
                        effect.SetTexture(texture);
                        ApplyLighting(
                            effect,
                            lighting,
                            objectPosition);
                    };

                const bool useCutout = !useCustom
                    && primitive.textureHasTransparency
                    && primitive.cutoutEffect != nullptr
                    && primitive.cutoutInputLayout != nullptr;
                if (useCustom)
                {
                    configureEffect(*primitive.effect);
                    primitive.effect->SetPerPixelLighting(true);
                    customEffect->SetMatrices(
                        meshGlobal * ownerWorld,
                        view,
                        projection);
                    if (materialOverride != nullptr)
                    {
                        customEffect->SetMaterial(
                            *materialOverride);
                    }
                    else
                    {
                        // 上書きが無い場合はモデル自身の材質で描きます
                        // （インポーターが読んだmetallicも反映）。
                        primitiveMaterial.SetBaseColor(color);
                        primitiveMaterial.SetRoughness(roughness);
                        primitiveMaterial.SetMetallic(
                            primitive.metallic);
                        customEffect->SetMaterial(
                            primitiveMaterial);
                    }
                    // マテリアル上書き中はLitMaterialのPBRマップが
                    // 正になります（色や粗さと同じ扱い）。上書きが
                    // 無ければモデル自身のマップを使います。
                    PbrTextures pbrTextures{};
                    if (materialOverride != nullptr
                        && pbrOverride != nullptr)
                    {
                        pbrTextures = *pbrOverride;
                    }
                    else
                    {
                        pbrTextures.roughness =
                            primitive.roughnessTexture.Get();
                        pbrTextures.metallic =
                            primitive.metallicTexture.Get();
                        pbrTextures.occlusion =
                            primitive.occlusionTexture.Get();
                        pbrTextures.emissive =
                            primitive.emissiveTexture.Get();
                        pbrTextures.occlusionStrength =
                            primitive.occlusionStrength;
                        pbrTextures.emissiveFactor =
                            primitive.emissiveFactor;
                    }
                    customEffect->SetTextures(
                        texture,
                        normalTexture,
                        pbrTextures);
                    customEffect->SetLighting(lighting);
                }
                else if (useCutout)
                {
                    configureEffect(*primitive.cutoutEffect);
                }
                else
                {
                    configureEffect(*primitive.effect);
                }

                // カスタムShaderが描画状態を宣言している場合は、
                // その指定を優先します（ワイヤーフレーム表示は
                // デバッグ用なので宣言より優先します）。
                const ShaderRenderState* declaredState =
                    useCustom
                        && customEffect
                            ->RenderState().declared
                        ? &customEffect->RenderState()
                        : nullptr;
                if (declaredState != nullptr && !wireframe)
                {
                    switch (declaredState->blend)
                    {
                    case ShaderBlendMode::Alpha:
                        context->OMSetBlendState(
                            states.NonPremultiplied(),
                            nullptr,
                            0xffffffff);
                        break;
                    case ShaderBlendMode::Additive:
                    {
                        // DirectXTKのAdditiveは書き込み先のアルファを
                        // 汚すので、アルファを保存する純加算を使う
                        // （MeshRendererComponentと同じ扱い）。
                        if (!m_additiveBlendPreservingAlpha)
                        {
                            Microsoft::WRL::ComPtr<ID3D11Device> device;
                            context->GetDevice(
                                device.ReleaseAndGetAddressOf());
                            m_additiveBlendPreservingAlpha =
                                CreateAdditiveBlendPreservingAlpha(
                                    device.Get());
                        }
                        context->OMSetBlendState(
                            m_additiveBlendPreservingAlpha
                                ? m_additiveBlendPreservingAlpha.Get()
                                : states.Additive(),
                            nullptr,
                            0xffffffff);
                        break;
                    }
                    case ShaderBlendMode::Premultiplied:
                        context->OMSetBlendState(
                            states.AlphaBlend(),
                            nullptr,
                            0xffffffff);
                        break;
                    case ShaderBlendMode::Opaque:
                    default:
                        context->OMSetBlendState(
                            states.Opaque(),
                            nullptr,
                            0xffffffff);
                        break;
                    }
                    context->OMSetDepthStencilState(
                        declaredState->depthTest
                            ? (declaredState->depthWrite
                                ? states.DepthDefault()
                                : states.DepthRead())
                            : states.DepthNone(),
                        0);
                    switch (declaredState->cull)
                    {
                    case ShaderCullMode::Front:
                        // スキニングモデルは表面がCullClockwise側
                        // なので、frontは反対のCullCounterClockwise
                        // になります。
                        context->RSSetState(
                            states.CullCounterClockwise());
                        break;
                    case ShaderCullMode::None:
                        context->RSSetState(
                            states.CullNone());
                        break;
                    case ShaderCullMode::Back:
                    default:
                        context->RSSetState(
                            primitive.doubleSided
                                ? states.CullNone()
                                : states.CullClockwise());
                        break;
                    }
                }
                else
                {
                    context->OMSetBlendState(
                        alphaPass
                            ? states.NonPremultiplied()
                            : states.Opaque(),
                        nullptr,
                        0xffffffff);
                    context->OMSetDepthStencilState(
                        alphaPass
                            ? states.DepthRead()
                            : states.DepthDefault(),
                        0);
                    context->RSSetState(
                        wireframe
                            ? states.Wireframe()
                            : primitive.doubleSided
                                ? states.CullNone()
                                : states.CullClockwise());
                }

                constexpr UINT stride =
                    sizeof(
                        DirectX::
                            VertexPositionNormalTangentColorTextureSkinning);
                constexpr UINT offset = 0;
                ID3D11Buffer* vertexBuffer =
                    primitive.vertexBuffer.Get();
                context->IASetVertexBuffers(
                    0,
                    1,
                    &vertexBuffer,
                    &stride,
                    &offset);
                context->IASetIndexBuffer(
                    selectedIndexBuffer,
                    DXGI_FORMAT_R32_UINT,
                    0);
                context->IASetInputLayout(
                    useCustom
                        ? primitive.inputLayout.Get()
                        : useCutout
                        ? primitive.cutoutInputLayout.Get()
                        : primitive.inputLayout.Get());
                context->IASetPrimitiveTopology(
                    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                const bool drawOccluded =
                    !depthOnly
                    && useCustom
                    && customEffect->HasOccludedPass()
                    && materialOverride != nullptr
                    && materialOverride
                        ->CustomParameter(4).w > 0.0f
                    && !wireframe;
                if (drawOccluded)
                {
                    context->OMSetBlendState(
                        states.NonPremultiplied(),
                        nullptr,
                        0xffffffff);
                    context->RSSetState(
                        states.CullCounterClockwise());
                    customEffect->ApplyOccluded(context);
                    context->DrawIndexed(
                        selectedIndexCount,
                        0,
                        0);

                    context->OMSetBlendState(
                        alphaPass
                            ? states.NonPremultiplied()
                            : states.Opaque(),
                        nullptr,
                        0xffffffff);
                    context->OMSetDepthStencilState(
                        alphaPass
                            ? states.DepthRead()
                            : states.DepthDefault(),
                        0);
                    context->RSSetState(
                        primitive.doubleSided
                            ? states.CullNone()
                            : states.CullClockwise());
                }
                const bool drawOutline =
                    !depthOnly
                    && useCustom
                    && customEffect->HasOutline()
                    && materialOverride != nullptr
                    && materialOverride
                        ->CustomParameter(3).x > 0.0f
                    && !wireframe;
                if (drawOutline)
                {
                    context->OMSetBlendState(
                        states.Opaque(),
                        nullptr,
                        0xffffffff);
                    context->OMSetDepthStencilState(
                        states.DepthDefault(),
                        0);
                    context->RSSetState(
                        states.CullCounterClockwise());
                    customEffect->ApplyOutline(context);
                    context->DrawIndexed(
                        selectedIndexCount,
                        0,
                        0);

                    context->OMSetBlendState(
                        alphaPass
                            ? states.NonPremultiplied()
                            : states.Opaque(),
                        nullptr,
                        0xffffffff);
                    context->OMSetDepthStencilState(
                        alphaPass
                            ? states.DepthRead()
                            : states.DepthDefault(),
                        0);
                    context->RSSetState(
                        primitive.doubleSided
                            ? states.CullNone()
                            : states.CullClockwise());
                }
                if (useCustom)
                {
                    primitive.effect->Apply(context);
                    if (!depthOnly)
                    {
                        customEffect->ApplyPixelOnly(context);
                    }
                }
                else if (useCutout)
                {
                    primitive.cutoutEffect->Apply(context);
                }
                else
                {
                    primitive.effect->Apply(context);
                }
                // 深度パスにはレンダーターゲットがありません。
                // DirectXTKのEffectは必ずピクセルシェーダーを設定
                // するので、外さないと影マップの解像度ぶん、
                // 捨てられるだけの計算が毎フレーム走ります。
                // **切り抜き（アルファテスト）だけは残します**
                // ――discardで影の形を決めているためです。
                if (depthOnly && !useCutout)
                {
                    context->PSSetShader(nullptr, nullptr, 0);
                }
                context->DrawIndexed(
                    selectedIndexCount,
                    0,
                    0);
            }
        }
    }
}
