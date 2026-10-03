#include "LamaPon/Graphics/SkeletalModel.h"

#include "LamaPon/Graphics/GraphicsDevice.h"
#include "LamaPon/Graphics/GraphicsDeviceD3D11Access.h"
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
    // 共有効果のパス選択・一時フラグとHS/DS/GSを終了時に戻します。
    class MaterialPassScope final
    {
    public:
        // 終了時に共有効果と一時シェーダーを解除する(effect: 借用する共有効果, context: 借用する即時コンテキスト)。
        MaterialPassScope(
            LamaPon::LitEffect& effect,
            ID3D11DeviceContext* context) noexcept
            : m_effect(effect)
            , m_context(context)
        {
        }

        // 描画終了時の解除を二重に行わないようコピーを禁止する。
        MaterialPassScope(const MaterialPassScope&) = delete;
        // 描画終了時の解除を二重に行わないようコピー代入を禁止する。
        MaterialPassScope& operator=(const MaterialPassScope&) = delete;

        // 共有効果を通常パスへ戻し、一時フラグとHS・DS・GSを解除する。
        ~MaterialPassScope()
        {
            m_effect.SetTessellationDrawEnabled(false);
            m_effect.SetDepthOnlyEnabled(false);
            m_effect.SetInstancingEnabled(false);
            // 先頭へ戻す描画パスの種類
            constexpr std::array roles{
                LamaPon::ShaderPassRole::Forward,
                LamaPon::ShaderPassRole::Skinned,
                LamaPon::ShaderPassRole::Instanced,
                LamaPon::ShaderPassRole::Outline,
                LamaPon::ShaderPassRole::SkinnedOutline,
                LamaPon::ShaderPassRole::Occluded
            };
            // 戻す描画パスの種類
            for (const auto role : roles)
            {
                if (m_effect.PassCount(role) == 0)
                {
                    continue;
                }
                try
                {
                    m_effect.SelectPass(role, 0);
                }
                catch (...)
                {
                    // 破棄時は例外を外へ出さず、各パスの先頭へ戻す失敗を無視します。
                }
            }
            if (m_context != nullptr)
            {
                m_context->HSSetShader(nullptr, nullptr, 0);
                m_context->DSSetShader(nullptr, nullptr, 0);
                m_context->GSSetShader(nullptr, nullptr, 0);
            }
        }

    private:
        // 借用する共有マテリアル効果
        LamaPon::LitEffect& m_effect;
        // 借用する即時コンテキスト
        ID3D11DeviceContext* m_context{};
    };

    // 全頂点が同じクリップ境界面の外なら真を返す(bounds: モデル境界, localToClip: クリップ座標への変換)。
    bool IsOutsideClipBounds(
        const LamaPon::Bounds3D& bounds,
        DirectX::FXMMATRIX localToClip) noexcept
    {
        // 6境界面で全頂点が外側か
        std::array<bool, 6> allOutside{
            true, true, true, true, true, true
        };
        // x方向の境界端符号
        for (const int x : { -1, 1 })
        {
            // y方向の境界端符号
            for (const int y : { -1, 1 })
            {
                // z方向の境界端符号
                for (const int z : { -1, 1 })
                {
                    // 変換したクリップ座標
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
                    // 6境界面からの符号付き距離
                    const std::array distances{
                        clip.x + clip.w,
                        clip.w - clip.x,
                        clip.y + clip.w,
                        clip.w - clip.y,
                        clip.z,
                        clip.w - clip.z
                    };
                    // クリップ境界面の番号
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
        // 全頂点が外側の面があるか返します(outside: その面で全頂点が外側か)。
        return std::ranges::any_of(
            allOutside,
            [](const bool outside)
            {
                return outside;
            });
    }

    // 指定時刻より後の最初のキー番号を返す(keys: 時刻順のキー列, time: 採取時刻の秒数)。
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

    // 指定方式でベクトルを補間し、範囲外では端点を返す(channel: 時刻順の曲線, time: 採取時刻の秒数, fallback: キー未設定時の値)。
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

        // 時刻より後の最初のキー番号
        const std::size_t upper =
            UpperKeyIndex(channel.keys, time);
        // 補間区間の始点キー
        const auto& left = channel.keys[upper - 1];
        // 補間区間の終点キー
        const auto& right = channel.keys[upper];
        if (channel.interpolation
            == LamaPon::SkeletalInterpolation::Step)
        {
            return left.value;
        }

        // 補間区間の秒数
        const float span =
            std::max(right.time - left.time, 0.000001f);
        // 区間内の0～1位置
        const float amount =
            std::clamp((time - left.time) / span, 0.0f, 1.0f);
        // 補間したキー値
        XMVECTOR result{};
        if (channel.interpolation
            == LamaPon::SkeletalInterpolation::CubicSpline)
        {
            // 補間位置の2乗
            const float t2 = amount * amount;
            // 補間位置の3乗
            const float t3 = t2 * amount;
            // 始点キー値の重み
            const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
            // 始点接線の重み
            const float h10 = t3 - 2.0f * t2 + amount;
            // 終点キー値の重み
            const float h01 = -2.0f * t3 + 3.0f * t2;
            // 終点接線の重み
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

        // 格納形式にした採取値
        XMFLOAT3 sampled{};
        XMStoreFloat3(&sampled, result);
        return sampled;
    }

    // 回転を補間し、線形補間は最短経路を使う(channel: 時刻順の曲線, time: 採取時刻の秒数, fallback: キー未設定時の回転)。
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

        // 時刻より後の最初のキー番号
        const std::size_t upper =
            UpperKeyIndex(channel.keys, time);
        // 補間区間の始点キー
        const auto& left = channel.keys[upper - 1];
        // 補間区間の終点キー
        const auto& right = channel.keys[upper];
        if (channel.interpolation
            == LamaPon::SkeletalInterpolation::Step)
        {
            return left.value;
        }

        // 補間区間の秒数
        const float span =
            std::max(right.time - left.time, 0.000001f);
        // 区間内の0～1位置
        const float amount =
            std::clamp((time - left.time) / span, 0.0f, 1.0f);
        // 補間したキー値
        XMVECTOR result{};
        if (channel.interpolation
            == LamaPon::SkeletalInterpolation::CubicSpline)
        {
            // 補間位置の2乗
            const float t2 = amount * amount;
            // 補間位置の3乗
            const float t3 = t2 * amount;
            // 始点キー値の重み
            const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
            // 始点接線の重み
            const float h10 = t3 - 2.0f * t2 + amount;
            // 終点キー値の重み
            const float h01 = -2.0f * t3 + 3.0f * t2;
            // 終点接線の重み
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
            // 終点の回転四元数
            XMVECTOR rightValue =
                XMLoadFloat4(&right.value);
            // 始点の回転四元数
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

        // 格納形式にした採取値
        XMFLOAT4 sampled{};
        XMStoreFloat4(&sampled, result);
        return sampled;
    }

    // 粗さを1～128の反射指数に変換する(roughness: 材質の粗さ)。
    float SpecularPowerFromRoughness(const float roughness) noexcept
    {
        // 粗さの2乗
        const float squared = roughness * roughness;
        return std::clamp(
            2.0f / std::max(squared * squared, 0.0001f) - 2.0f,
            1.0f,
            128.0f);
    }

    // クリップ曲線または初期姿勢から局所姿勢を採取する(nodes: モデル内のノード, clip: 未指定なら初期姿勢, time: 採取時刻の秒数, localPose: ノード数に揃える出力)。
    void SampleLocalPose(
        const std::vector<LamaPon::SkeletalNode>& nodes,
        const LamaPon::SkeletalAnimationClip* clip,
        const float time,
        std::vector<LamaPon::SkeletalPoseTransform>& localPose)
    {
        localPose.resize(nodes.size());
        // 姿勢を採取・解決するノード番号
        for (std::size_t index = 0; index < nodes.size(); ++index)
        {
            localPose[index] = nodes[index].bindPose;
        }
        if (clip == nullptr)
        {
            return;
        }
        // 当該ノードの時間変化
        for (const auto& track : clip->tracks)
        {
            if (track.node >= localPose.size())
            {
                continue;
            }
            // 時間変化を反映する局所姿勢
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

    // 親階層を解決し、不正な親や循環なら例外を送出する(nodes: ノードの階層, localPose: 各ノードの局所姿勢, globalPose: ノード数に揃える全体姿勢の出力)。
    void BuildGlobalPose(
        const std::vector<LamaPon::SkeletalNode>& nodes,
        const std::vector<LamaPon::SkeletalPoseTransform>& localPose,
        std::vector<DirectX::XMFLOAT4X4>& globalPose)
    {
        globalPose.resize(nodes.size());
        // 0未訪問・1探索中・2完了
        std::vector<unsigned char> state(nodes.size());
        // 親から再帰的に全体行列を解決します(index: 対象ノード番号)。
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
                // 親の姿勢を含む全体行列
                XMMATRIX global =
                    LamaPon::SkeletalModel::LocalMatrix(
                        localPose[index]);
                // 現在のノードの親番号
                const auto parent = nodes[index].parent;
                if (parent >= 0)
                {
                    // 検証する親ノードの番号
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

        // 姿勢を採取・解決するノード番号
        for (std::size_t index = 0; index < nodes.size(); ++index)
        {
            static_cast<void>(resolve(index));
        }
    }
}

namespace LamaPon
{
    struct SkeletalModel::TextureInputs final
    {
        // テクスチャを解決する機器
        GraphicsDevice* graphics{};
        // 今回の描画用テクスチャ
        const LitTextureRequest* request{};
        // 互換用の色テクスチャ
        ID3D11ShaderResourceView* legacyAlbedo{};
        // 互換用の法線テクスチャ
        ID3D11ShaderResourceView* legacyNormal{};
        // 互換用のPBRテクスチャ
        const PbrTextures* legacyPbr{};
    };

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

        // モデル境界の中心
        const DirectX::XMVECTOR center = DirectX::XMVectorSet(
            (localBounds.minimum.x + localBounds.maximum.x) * 0.5f,
            (localBounds.minimum.y + localBounds.maximum.y) * 0.5f,
            (localBounds.minimum.z + localBounds.maximum.z) * 0.5f,
            1.0f);
        // 境界のX方向半幅
        const float halfX = std::abs(
            localBounds.maximum.x - localBounds.minimum.x) * 0.5f;
        // 境界のY方向半幅
        const float halfY = std::abs(
            localBounds.maximum.y - localBounds.minimum.y) * 0.5f;
        // 境界のZ方向半幅
        const float halfZ = std::abs(
            localBounds.maximum.z - localBounds.minimum.z) * 0.5f;
        // モデル座標での境界半径
        const float localRadius = std::sqrt(
            halfX * halfX + halfY * halfY + halfZ * halfZ);
        // ワールド変換の最大倍率
        const float scale = std::max({
            DirectX::XMVectorGetX(
                DirectX::XMVector3Length(ownerWorld.r[0])),
            DirectX::XMVectorGetX(
                DirectX::XMVector3Length(ownerWorld.r[1])),
            DirectX::XMVectorGetX(
                DirectX::XMVector3Length(ownerWorld.r[2]))
        });
        // 視点座標での境界中心
        const auto viewPosition = DirectX::XMVector4Transform(
            DirectX::XMVector4Transform(center, ownerWorld),
            view);
        // 視点からの奥行距離
        const float distance = std::max(
            std::abs(DirectX::XMVectorGetZ(viewPosition)),
            0.001f);
        // 縦方向の投影倍率
        const float focalScale = std::max(
            std::abs(
                DirectX::XMVectorGetY(projection.r[1])),
            0.001f);
        // 画面上での投影半径
        const float projectedRadius =
            localRadius * scale * focalScale / distance
            * std::clamp(quality, 0.25f, 2.0f);

        // 投影半径が0.04未満ならLOD2、0.12未満ならLOD1を選ぶ。
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
        // 選択した頂点参照数の合計
        std::uint64_t indexCount{};
        // 頂点参照数を数える描画単位
        for (const auto& primitive : primitives)
        {
            // 選択したLODの頂点参照数
            std::uint32_t selected = primitive.indexCount;
            if (lodLevel > 0)
            {
                // 探すLODの段階
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
        // 補間元のノード局所姿勢
        std::vector<SkeletalPoseTransform> fromPose;
        // 補間先のノード局所姿勢
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

        // 範囲を補正した補間率
        const float blend = std::clamp(amount, 0.0f, 1.0f);
        localPose.resize(nodes.size());
        // 合成するノードの番号
        for (std::size_t index = 0; index < nodes.size(); ++index)
        {
            using namespace DirectX;
            // 合成結果のノード局所姿勢
            auto& result = localPose[index];
            // 補間元のノード姿勢
            const auto& from = fromPose[index];
            // 補間先のノード姿勢
            const auto& to = toPose[index];
            XMStoreFloat3(
                &result.translation,
                XMVectorLerp(
                    XMLoadFloat3(&from.translation),
                    XMLoadFloat3(&to.translation),
                    blend));
            // 補間元の回転四元数
            XMVECTOR fromRotation =
                XMLoadFloat4(&from.rotation);
            // 補間先の回転四元数
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
        // 合成済みの重みの合計
        float totalWeight{};
        // 今回採取したノード局所姿勢
        std::vector<SkeletalPoseTransform> sampledPose;
        // 合成するクリップと重み
        for (const auto& sample : samples)
        {
            // 負値を除いた今回の重み
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

            // 今回分を含む重みの合計
            const float newTotal =
                totalWeight + weight;
            // 今回の姿勢の合成比率
            const float amount = weight / newTotal;
            // 合成するノードの番号
            for (std::size_t index = 0;
                index < localPose.size();
                ++index)
            {
                using namespace DirectX;
                // 合成結果のノード局所姿勢
                auto& result = localPose[index];
                // 次に合成するノード姿勢
                const auto& next = sampledPose[index];
                XMStoreFloat3(
                    &result.translation,
                    XMVectorLerp(
                        XMLoadFloat3(&result.translation),
                        XMLoadFloat3(&next.translation),
                        amount));
                // 補間元の回転四元数
                XMVECTOR fromRotation =
                    XMLoadFloat4(&result.rotation);
                // 補間先の回転四元数
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
        GraphicsDevice& graphics,
        const LightingState& lighting,
        DirectX::FXMMATRIX ownerWorld,
        DirectX::CXMMATRIX view,
        DirectX::CXMMATRIX projection,
        const SkeletalAnimationClip* clip,
        const float time,
        const bool wireframe,
        const LitMaterial* materialOverride,
        const LitTextureRequest* textureOverride,
        const SkeletalAnimationClip* blendClip,
        const float blendTime,
        const float blendAmount,
        const std::vector<SkeletalPoseSample>* weightedSamples,
        const std::size_t removeRootMotionNode,
        LitEffect* customEffect,
        ID3D11InputLayout* customInputLayout,
        const bool depthOnly,
        const std::vector<DirectX::XMFLOAT4X4>* globalPoseOverride,
        const float automaticLodQuality,
        const std::vector<Microsoft::WRL::ComPtr<
            ID3D11InputLayout>>* customColorInputLayouts,
        const std::vector<Microsoft::WRL::ComPtr<
            ID3D11InputLayout>>* customOutlineInputLayouts,
        const std::array<
            ID3D11ShaderResourceView*,
            LitMaterial::CustomTextureCount>* customTextureViews,
        LitEffect* staticManifestEffect,
        const std::vector<Microsoft::WRL::ComPtr<
            ID3D11InputLayout>>* staticManifestColorInputLayouts,
        const std::vector<Microsoft::WRL::ComPtr<
            ID3D11InputLayout>>* staticManifestOutlineInputLayouts,
        const bool depthPrepass,
        const LitMaterial* customParameterSource) const
    {
        // 描画先のD3D11コンテキスト
        auto* const context =
            Detail::GraphicsDeviceD3D11Access::Context(graphics);
        if (context == nullptr)
        {
            return;
        }
        // 描画用テクスチャの参照
        TextureInputs textures{};
        textures.graphics = &graphics;
        textures.request = textureOverride;
        DrawD3D11(
            context,
            Detail::GraphicsDeviceD3D11Access::States(graphics),
            lighting,
            ownerWorld,
            view,
            projection,
            clip,
            time,
            wireframe,
            materialOverride,
            textures,
            blendClip,
            blendTime,
            blendAmount,
            weightedSamples,
            removeRootMotionNode,
            customEffect,
            customInputLayout,
            depthOnly,
            globalPoseOverride,
            automaticLodQuality,
            customColorInputLayouts,
            customOutlineInputLayouts,
            customTextureViews,
            staticManifestEffect,
            staticManifestColorInputLayouts,
            staticManifestOutlineInputLayouts,
            depthPrepass,
            customParameterSource);
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
        const bool depthOnly,
        const std::vector<DirectX::XMFLOAT4X4>*
            globalPoseOverride,
        const float automaticLodQuality) const
    {
        // 描画用テクスチャの参照
        TextureInputs textures{};
        textures.legacyAlbedo = albedoOverride;
        textures.legacyNormal = normalOverride;
        textures.legacyPbr = pbrOverride;
        DrawD3D11(
            context,
            states,
            lighting,
            ownerWorld,
            view,
            projection,
            clip,
            time,
            wireframe,
            materialOverride,
            textures,
            blendClip,
            blendTime,
            blendAmount,
            weightedSamples,
            removeRootMotionNode,
            customEffect,
            customInputLayout,
            depthOnly,
            globalPoseOverride,
            automaticLodQuality,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            false,
            nullptr);
    }

    void SkeletalModel::DrawD3D11(
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
        const TextureInputs& textures,
        const SkeletalAnimationClip* blendClip,
        const float blendTime,
        const float blendAmount,
        const std::vector<SkeletalPoseSample>* weightedSamples,
        const std::size_t removeRootMotionNode,
        LitEffect* customEffect,
        ID3D11InputLayout* customInputLayout,
        const bool depthOnly,
        const std::vector<DirectX::XMFLOAT4X4>* globalPoseOverride,
        const float automaticLodQuality,
        const std::vector<Microsoft::WRL::ComPtr<
            ID3D11InputLayout>>* customColorInputLayouts,
        const std::vector<Microsoft::WRL::ComPtr<
            ID3D11InputLayout>>* customOutlineInputLayouts,
        const std::array<
            ID3D11ShaderResourceView*,
            LitMaterial::CustomTextureCount>* customTextureViews,
        LitEffect* staticManifestEffect,
        const std::vector<Microsoft::WRL::ComPtr<
            ID3D11InputLayout>>* staticManifestColorInputLayouts,
        const std::vector<Microsoft::WRL::ComPtr<
            ID3D11InputLayout>>* staticManifestOutlineInputLayouts,
        const bool depthPrepass,
        const LitMaterial* customParameterSource) const
    {
        using namespace DirectX;
        // customInputLayoutはABI互換の未使用引数で、直書きHLSLにはモデル内蔵の入力レイアウトを使う。
        (void)customInputLayout;
        if (context == nullptr)
        {
            return;
        }
        // 共有効果を通常パスへ戻し、終了時の解除を予約する(effect: 借用する共有効果)。
        const auto prepareEffect = [context](LitEffect* const effect)
            -> std::unique_ptr<MaterialPassScope>
        {
            if (effect == nullptr)
            {
                return {};
            }
            effect->SetInstancingEnabled(false);
            effect->SetTessellationDrawEnabled(false);
            effect->SelectColorPass(0);
            return std::make_unique<MaterialPassScope>(
                *effect,
                context);
        };
        // 共有効果の描画終了時の解除
        auto materialPassScope = prepareEffect(customEffect);
        // 静的効果の描画終了時の解除
        auto staticMaterialPassScope = staticManifestEffect != customEffect
            ? prepareEffect(staticManifestEffect)
            : nullptr;

        // 採取したノード局所姿勢
        std::vector<SkeletalPoseTransform> localPose;
        // 採取したノード全体姿勢
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
        // 今回の描画に使う全体姿勢
        const auto& resolvedGlobalPose =
            globalPoseOverride != nullptr
                ? *globalPoseOverride
                : globalPose;
        // 無変形時の単位行列
        const XMMATRIX identity = XMMatrixIdentity();
        // メッシュ用のボーン行列
        std::vector<XMMATRIX> palette;
        // 大きなカスタム定数配列の再初期化を避け、描画単位ごとに同じ材質へ値を設定する。
        // 使い回す内蔵材質の設定
        LitMaterial primitiveMaterial;
        if (customParameterSource != nullptr)
        {
            // 写す追加値の番号
            for (std::size_t index{};
                 index < LitMaterial::CustomParameterCount;
                 ++index)
            {
                primitiveMaterial.SetCustomParameter(
                    index,
                    customParameterSource->CustomParameter(index));
            }
            // 写す追加ベクトルの番号
            for (std::size_t index{};
                 index < LitMaterial::CustomVectorCount;
                 ++index)
            {
                primitiveMaterial.SetCustomVector(
                    index,
                    customParameterSource->CustomVector(index));
            }
        }
        // 投影サイズから選んだLOD
        const std::size_t automaticLod = SelectAutomaticLod(
            ownerWorld,
            view,
            projection,
            automaticLodQuality);
        // 生ビューと同じ参照をキャッシュし、取り込み失敗なら偽を返す(handle: 更新するキャッシュ, native: 借用する生ビュー)。
        const auto mirrorLegacyTexture =
            [&textures](
                GraphicsViewHandle& handle,
                ID3D11ShaderResourceView* const native) noexcept
        {
            if (textures.graphics == nullptr)
            {
                return true;
            }
            if (native == nullptr)
            {
                // 互換用の参照が空ならキャッシュも解除する。
                handle = {};
                return true;
            }
            if (Detail::GraphicsDeviceD3D11Access::
                    TryResolveD3D11ShaderResourceView(
                        *textures.graphics,
                        handle)
                == native)
            {
                return true;
            }
            try
            {
                handle = textures.graphics
                    ->ImportD3D11ShaderResourceView(native);
                return true;
            }
            catch (...)
            {
                // 別機器の互換用ビューなどを取り込めない場合は、この描画単位を省く。
                return false;
            }
        };

        LAMAPON_PROFILE_SCOPE("SkeletalModel.Primitives");
        // 半透明描画を行う段階か
        for (const bool alphaPass : { false, true })
        {
            // 今回描画するメッシュ単位
            for (const auto& primitive : primitives)
            {
                // 静的モデル用効果を使うか
                const bool useStaticManifestRole =
                    primitive.skin < 0
                    && staticManifestEffect != nullptr;
                // 今回の描画に使う独自効果
                auto* const activeCustomEffect = useStaticManifestRole
                    ? staticManifestEffect
                    : customEffect;
                // 通常パス用の入力レイアウト
                const auto* const activeColorInputLayouts =
                    useStaticManifestRole
                    ? staticManifestColorInputLayouts
                    : customColorInputLayouts;
                // 輪郭パス用の入力レイアウト
                const auto* const activeOutlineInputLayouts =
                    useStaticManifestRole
                    ? staticManifestOutlineInputLayouts
                    : customOutlineInputLayouts;
                // 独自マテリアル効果を使うか
                const bool useCustom =
                    activeCustomEffect != nullptr;
                // Manifestによる描画か
                const bool useManifest = useCustom
                    && activeCustomEffect->IsManifestEffect();
                if (depthOnly
                    && depthPrepass
                    && useCustom
                    && activeCustomEffect->ColorPassCount() != 0)
                {
                    // 先頭パスの描画状態
                    const auto& primaryState =
                        activeCustomEffect->ColorPassRenderState(0);
                    if (primaryState.declared
                        && (primaryState.blend
                                != ShaderBlendMode::Opaque
                            || !primaryState.depthWrite))
                    {
                        // 混在モデル全体を除かず、この描画単位だけ深度プリパスを省く。
                        continue;
                    }
                }
                // Manifestには各パスの入力レイアウト、直書きHLSLと標準描画にはモデル内蔵の頂点処理を使う。
                if (!primitive.vertexBuffer
                    || !primitive.indexBuffer
                    || primitive.meshNode
                        >= resolvedGlobalPose.size()
                    || (!useManifest
                        && (!primitive.effect
                            || !primitive.inputLayout))
                    || (useManifest
                        && (activeColorInputLayouts == nullptr
                            || activeColorInputLayouts->empty())))
                {
                    continue;
                }

                // 上書きを含む材質の透明度
                const float effectiveAlpha = materialOverride != nullptr
                    ? materialOverride->BaseColor().w
                    : primitive.baseColor.w;
                // 半透明段階で描くか
                bool usesAlpha =
                    primitive.alpha || effectiveAlpha < 0.999f;
                if (useCustom)
                {
                    // 全通常パスに状態宣言があるか
                    bool allPassesDeclareState =
                        activeCustomEffect->ColorPassCount() != 0;
                    // 描画順序に依存するパスがあるか
                    bool hasOrderDependentPass{};
                    // 描画するパスの番号
                    for (std::size_t passIndex = 0;
                        passIndex
                            < activeCustomEffect->ColorPassCount();
                        ++passIndex)
                    {
                        // 通常パスの描画状態
                        const auto& state =
                            activeCustomEffect->ColorPassRenderState(
                                passIndex);
                        allPassesDeclareState =
                            allPassesDeclareState && state.declared;
                        hasOrderDependentPass = hasOrderDependentPass
                            || (state.declared
                                && (state.blend
                                        == ShaderBlendMode::Alpha
                                    || state.blend
                                        == ShaderBlendMode::Premultiplied));
                    }
                    if (hasOrderDependentPass)
                    {
                        usesAlpha = true;
                    }
                    else if (allPassesDeclareState)
                    {
                        usesAlpha = false;
                    }
                }
                if (usesAlpha != alphaPass)
                {
                    continue;
                }

                // モデル内のメッシュ全体行列
                const XMMATRIX meshGlobal =
                    XMLoadFloat4x4(
                        &resolvedGlobalPose[primitive.meshNode]);
                // 静的モデルの各メッシュだけ境界で視錐台判定し、変形するスキニング形状は省略しない。
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
                    // 使うスキンの番号
                    const auto skinIndex =
                        static_cast<std::size_t>(primitive.skin);
                    if (skinIndex >= skins.size())
                    {
                        continue;
                    }
                    // 適用する関節と初期姿勢
                    const auto& skin = skins[skinIndex];
                    palette.reserve(skin.joints.size());
                    // メッシュ全体行列の逆行列
                    const XMMATRIX inverseMesh =
                        XMMatrixInverse(nullptr, meshGlobal);
                    // スキン内の関節番号
                    for (std::size_t index = 0;
                        index < skin.joints.size();
                        ++index)
                    {
                        // 関節に対応するノード番号
                        const auto joint = skin.joints[index];
                        if (joint >= resolvedGlobalPose.size())
                        {
                            palette.push_back(identity);
                            continue;
                        }
                        // 関節の初期姿勢の逆行列
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

                // 選択したLODの頂点参照
                ID3D11Buffer* selectedIndexBuffer =
                    primitive.indexBuffer.Get();
                // 選択したLODの頂点参照数
                std::uint32_t selectedIndexCount =
                    primitive.indexCount;
                // 利用可能なLODを探す段階
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

                // 上書きを含む材質の色
                XMFLOAT4 color = primitive.baseColor;
                // 上書きを含む材質の粗さ
                float roughness = primitive.roughness;
                // 今回使う色テクスチャ
                ID3D11ShaderResourceView* texture =
                    primitive.texture.Get();
                // 今回使う法線テクスチャ
                ID3D11ShaderResourceView* normalTexture =
                    primitive.normalTexture.Get();
                if (materialOverride != nullptr)
                {
                    color = materialOverride->BaseColor();
                    roughness = materialOverride->Roughness();
                    if (textures.graphics != nullptr)
                    {
                        // 指定済みハンドルを解決できない描画単位は、内蔵テクスチャへ代替せず省く。
                        if (!useCustom
                            && textures.request != nullptr
                            && textures.request->albedo)
                        {
                            texture = Detail::GraphicsDeviceD3D11Access::
                                TryResolveD3D11ShaderResourceView(
                                    *textures.graphics,
                                    textures.request->albedo);
                            if (texture == nullptr)
                            {
                                continue;
                            }
                        }
                    }
                    else
                    {
                        if (textures.legacyAlbedo != nullptr)
                        {
                            texture = textures.legacyAlbedo;
                        }
                        if (textures.legacyNormal != nullptr)
                        {
                            normalTexture = textures.legacyNormal;
                        }
                    }
                }

                // LitEffectはビューを借用するため、テクスチャ参照を遮蔽・輪郭・通常描画の完了まで保持する。
                // 全描画パスまで保持する参照
                LitTextureRequest effectiveTextures;
                if (useCustom && textures.graphics != nullptr)
                {
                    // 互換用の生ビューを現在の機器へ取り込み、参照先が変わればキャッシュも更新する。
                    // 内蔵テクスチャの参照キャッシュ
                    auto& embedded = primitive.embeddedTextures;
                    if (!mirrorLegacyTexture(
                            embedded.albedo,
                            primitive.texture.Get())
                        || !mirrorLegacyTexture(
                            embedded.normal,
                            primitive.normalTexture.Get())
                        || !mirrorLegacyTexture(
                            embedded.roughness,
                            primitive.roughnessTexture.Get())
                        || !mirrorLegacyTexture(
                            embedded.metallic,
                            primitive.metallicTexture.Get())
                        || !mirrorLegacyTexture(
                            embedded.occlusion,
                            primitive.occlusionTexture.Get())
                        || !mirrorLegacyTexture(
                            embedded.emissive,
                            primitive.emissiveTexture.Get()))
                    {
                        continue;
                    }
                    embedded.occlusionStrength =
                        primitive.occlusionStrength;
                    embedded.emissiveFactor =
                        primitive.emissiveFactor;
                    effectiveTextures = embedded;
                    if (materialOverride != nullptr
                        && textures.request != nullptr)
                    {
                        // 色と法線の未指定は内蔵を継承し、PBRマップは空の指定も含め外部材質で置き換える。
                        if (textures.request->albedo)
                        {
                            effectiveTextures.albedo =
                                textures.request->albedo;
                        }
                        if (textures.request->normal)
                        {
                            effectiveTextures.normal =
                                textures.request->normal;
                        }
                        effectiveTextures.roughness =
                            textures.request->roughness;
                        effectiveTextures.metallic =
                            textures.request->metallic;
                        effectiveTextures.occlusion =
                            textures.request->occlusion;
                        effectiveTextures.emissive =
                            textures.request->emissive;
                        effectiveTextures.customTextures =
                            textures.request->customTextures;
                        effectiveTextures.occlusionStrength =
                            textures.request->occlusionStrength;
                        effectiveTextures.emissiveFactor =
                            textures.request->emissiveFactor;
                    }
                }

                // ワールド座標でのメッシュ位置
                XMFLOAT3 objectPosition{};
                XMStoreFloat3(
                    &objectPosition,
                    (meshGlobal * ownerWorld).r[3]);
                // 姿勢・材質・照明を設定する(effect: 設定先の頂点処理効果)。
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

                // 標準効果の切り抜きを使うか
                const bool useCutout = !useCustom
                    && primitive.textureHasTransparency
                    && primitive.cutoutEffect != nullptr
                    && primitive.cutoutInputLayout != nullptr;
                if (useCustom)
                {
                    if (!useManifest)
                    {
                        // 直書きHLSLのPSSkinnedMainにはDirectXTKの頂点出力を渡す。
                        configureEffect(*primitive.effect);
                        primitive.effect->SetPerPixelLighting(true);
                    }
                    activeCustomEffect->SetMatrices(
                        meshGlobal * ownerWorld,
                        view,
                        projection);
                    // 輪郭・遮蔽パスも通常描画と同じ変形になるよう、両方の効果へ同じボーン行列を設定する。
                    activeCustomEffect->SetBoneTransforms(
                        palette.data(),
                        palette.size());
                    if (materialOverride != nullptr)
                    {
                        activeCustomEffect->SetMaterial(
                            *materialOverride);
                    }
                    else
                    {

                        primitiveMaterial.SetBaseColor(color);
                        primitiveMaterial.SetRoughness(roughness);
                        primitiveMaterial.SetMetallic(
                            primitive.metallic);
                        activeCustomEffect->SetMaterial(
                            primitiveMaterial);
                    }
                    if (textures.graphics != nullptr)
                    {
                        if (!textures.graphics->TrySetLitEffectTextures(
                                *activeCustomEffect,
                                effectiveTextures)
                            || !textures.graphics->TrySetLitEffectLighting(
                                *activeCustomEffect,
                                lighting))
                        {
                            // 無効なハンドルで前の描画状態を使わないよう、この描画単位を省く。
                            continue;
                        }
                    }
                    else
                    {
                        // 互換経路では生ビューの指定を直接適用する。
                        // 互換用のPBRテクスチャ指定
                        PbrTextures pbrTextures{};
                        if (materialOverride != nullptr
                            && textures.legacyPbr != nullptr)
                        {
                            pbrTextures = *textures.legacyPbr;
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
                        activeCustomEffect->SetTextures(
                            texture,
                            normalTexture,
                            pbrTextures);
                        activeCustomEffect->SetCustomTextures(
                            customTextureViews != nullptr
                                ? *customTextureViews
                                : std::array<
                                    ID3D11ShaderResourceView*,
                                    LitMaterial::CustomTextureCount>{});

                        activeCustomEffect->SetLighting(lighting);
                    }
                }
                else if (useCutout)
                {
                    configureEffect(*primitive.cutoutEffect);
                }
                else
                {
                    configureEffect(*primitive.effect);
                }

                // 宣言済みの状態または標準状態を設定する(declaredState: パスの描画状態、未指定は標準)。
                const auto applyDrawState =
                    [&](const ShaderRenderState* declaredState)
                    {
                        // ワイヤーフレーム指定はManifestの描画状態より優先する。
                        if (declaredState != nullptr
                            && declaredState->declared
                            && !wireframe)
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
                                if (!m_additiveBlendPreservingAlpha)
                                {
                                    // 加算合成状態を作る機器
                                    Microsoft::WRL::ComPtr<
                                        ID3D11Device> device;
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
                            return;
                        }

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
                    };

                // 頂点データのバイト幅
                constexpr UINT stride =
                    sizeof(
                        DirectX::
                            VertexPositionNormalTangentColorTextureSkinning);
                // 頂点バッファの開始バイト
                constexpr UINT offset = 0;
                // 借用する頂点バッファ
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
                context->IASetPrimitiveTopology(
                    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                // 遮蔽パスを描くか
                const bool drawOccluded =
                    !depthOnly
                    && useCustom
                    && activeCustomEffect->HasOccludedPass()
                    && materialOverride != nullptr
                    && materialOverride
                        ->CustomParameter(4).w > 0.0f
                    && !wireframe;
                if (drawOccluded)
                {
                    // 追加描画パスの数
                    const auto passCount = useManifest
                        ? activeCustomEffect->PassCount(
                            ShaderPassRole::Occluded)
                        : std::size_t{ 1 };
                    // 描画するパスの番号
                    for (std::size_t passIndex = 0;
                        // 追加描画パスの数
                        passIndex < passCount;
                        ++passIndex)
                    {
                        // 選択したパスの描画状態
                        const ShaderRenderState* renderState{};
                        if (useManifest)
                        {
                            activeCustomEffect->SelectPass(
                                ShaderPassRole::Occluded,
                                passIndex);
                            renderState =
                                &activeCustomEffect->SelectedPassRenderState(
                                    ShaderPassRole::Occluded);
                            if (renderState->declared)
                            {
                                applyDrawState(renderState);
                            }
                            else
                            {
                                context->OMSetBlendState(
                                    states.NonPremultiplied(),
                                    nullptr,
                                    0xffffffff);
                                context->RSSetState(
                                    states.CullCounterClockwise());
                            }
                            // 遮蔽パスは通常パスの先頭の頂点処理を再利用し、ピクセルシェーダーだけ切り替える。
                            context->IASetInputLayout(
                                activeColorInputLayouts->front().Get());
                        }
                        else
                        {
                            context->OMSetBlendState(
                                states.NonPremultiplied(),
                                nullptr,
                                0xffffffff);
                            context->RSSetState(
                                states.CullCounterClockwise());
                            context->IASetInputLayout(
                                primitive.inputLayout.Get());
                        }
                        activeCustomEffect->ApplyOccluded(context);
                        context->DrawIndexed(
                            selectedIndexCount,
                            0,
                            0);
                    }
                }
                // 輪郭パスを描くか
                const bool drawOutline =
                    !depthOnly
                    && useCustom
                    && activeCustomEffect->HasOutline()
                    && materialOverride != nullptr
                    && materialOverride
                        ->CustomParameter(3).x > 0.0f
                    && !wireframe;
                if (drawOutline)
                {
                    // 静的またはスキニング輪郭
                    const auto outlineRole = activeCustomEffect->IsSkinned()
                        ? ShaderPassRole::SkinnedOutline
                        : ShaderPassRole::Outline;
                    // 追加描画パスの数
                    const auto passCount = useManifest
                        ? activeCustomEffect->PassCount(outlineRole)
                        : std::size_t{ 1 };
                    // 描画するパスの番号
                    for (std::size_t passIndex = 0;
                        // 追加描画パスの数
                        passIndex < passCount;
                        ++passIndex)
                    {
                        if (useManifest)
                        {
                            if (activeOutlineInputLayouts == nullptr
                                || passIndex
                                    >= activeOutlineInputLayouts->size())
                            {
                                break;
                            }
                            activeCustomEffect->SelectPass(
                                outlineRole,
                                passIndex);
                            // 選択したパスの描画状態
                            const auto& renderState =
                                activeCustomEffect->SelectedPassRenderState(
                                    outlineRole);
                            if (renderState.declared)
                            {
                                applyDrawState(&renderState);
                            }
                            else
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
                            }
                            context->IASetInputLayout(
                                (*activeOutlineInputLayouts)[
                                    passIndex].Get());
                        }
                        else
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
                            context->IASetInputLayout(
                                primitive.inputLayout.Get());
                        }
                        activeCustomEffect->ApplyOutline(context);
                        context->DrawIndexed(
                            selectedIndexCount,
                            0,
                            0);
                    }
                }

                if (useManifest)
                {
                    // 影と深度は先頭パスのみ、通常色はManifestに記載した順に全パスを描く。
                    // 今回実行する通常パス数
                    const auto colorPassCount = depthOnly
                        ? std::size_t{ 1 }
                        : activeCustomEffect->ColorPassCount();
                    activeCustomEffect->SetDepthOnlyEnabled(depthOnly);
                    // 描画するパスの番号
                    for (std::size_t passIndex = 0;
                        // 今回実行する通常パス数
                        passIndex < colorPassCount;
                        ++passIndex)
                    {
                        if (passIndex
                            >= activeColorInputLayouts->size())
                        {
                            break;
                        }
                        activeCustomEffect->SelectColorPass(passIndex);
                        // 選択したパスの描画状態
                        const auto& renderState =
                            activeCustomEffect->ColorPassRenderState(
                                passIndex);
                        applyDrawState(&renderState);
                        context->IASetInputLayout(
                            (*activeColorInputLayouts)[passIndex].Get());
                        activeCustomEffect->Apply(context);
                        context->DrawIndexed(
                            selectedIndexCount,
                            0,
                            0);
                    }
                    activeCustomEffect->SetDepthOnlyEnabled(false);
                    continue;
                }

                // Manifest以外はDirectXTKの頂点処理を使い、直書きHLSLではピクセルシェーダーを切り替える。
                // 直書きHLSLの描画状態
                const ShaderRenderState* directRenderState =
                    useCustom
                            && activeCustomEffect->RenderState().declared
                        ? &activeCustomEffect->RenderState()
                        : nullptr;
                applyDrawState(directRenderState);
                context->IASetInputLayout(
                    useCutout
                        ? primitive.cutoutInputLayout.Get()
                        : primitive.inputLayout.Get());
                if (useCustom)
                {
                    primitive.effect->Apply(context);
                    if (!depthOnly)
                    {
                        activeCustomEffect->ApplyPixelOnly(context);
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
                // 深度描画では不要なピクセル処理を外し、切り抜きによる影の形を保つ場合だけ残す。
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
