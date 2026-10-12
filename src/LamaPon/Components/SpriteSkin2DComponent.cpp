#include "LamaPon/Components/SpriteSkin2DComponent.h"

#include "LamaPon/Components/SpriteRendererComponent.h"
#include "LamaPon/Components/Sway2DComponent.h"
#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <string>
#include <utility>

namespace LamaPon
{
    namespace
    {
        // 逆行列を求められないとみなす行列式の大きさ
        constexpr float MinimumDeterminant = 1.0e-12f;
        // 距離による重みで使う最小距離
        constexpr float MinimumWeightDistance = 0.5f;

        // 行列が有限かつ可逆か返します(matrix: 確認する行列)。
        [[nodiscard]] bool IsInvertible(
            DirectX::FXMMATRIX matrix) noexcept
        {
            // 行列式
            const float determinant = DirectX::XMVectorGetX(
                DirectX::XMMatrixDeterminant(matrix));
            return std::isfinite(determinant)
                && std::abs(determinant) > MinimumDeterminant;
        }

        // 行列を保存形式へ変換します(matrix: 変換する行列)。
        [[nodiscard]] DirectX::XMFLOAT4X4 ToFloat4x4(
            DirectX::FXMMATRIX matrix) noexcept
        {
            // 保存形式の行列
            DirectX::XMFLOAT4X4 result{};
            DirectX::XMStoreFloat4x4(&result, matrix);
            return result;
        }

        // 行列の平行移動のXYを返します(matrix: 姿勢の行列)。
        [[nodiscard]] DirectX::XMFLOAT2 Translation(
            const DirectX::XMFLOAT4X4& matrix) noexcept
        {
            return { matrix._41, matrix._42 };
        }

        // 点から線分までの距離を返します(point: 測る点, start: 線分の始点, end: 線分の終点)。
        [[nodiscard]] float DistanceToSegment(
            const DirectX::XMFLOAT2& point,
            const DirectX::XMFLOAT2& start,
            const DirectX::XMFLOAT2& end) noexcept
        {
            // 線分の方向X
            const float segmentX = end.x - start.x;
            // 線分の方向Y
            const float segmentY = end.y - start.y;
            // 線分の長さの二乗
            const float lengthSquared =
                segmentX * segmentX + segmentY * segmentY;
            // 線分上の最近点の割合
            const float amount = lengthSquared > 1.0e-8f
                ? std::clamp(
                    ((point.x - start.x) * segmentX
                        + (point.y - start.y) * segmentY)
                        / lengthSquared,
                    0.0f,
                    1.0f)
                : 0.0f;
            // 最近点までのX差
            const float deltaX =
                point.x - (start.x + segmentX * amount);
            // 最近点までのY差
            const float deltaY =
                point.y - (start.y + segmentY * amount);
            return std::sqrt(deltaX * deltaX + deltaY * deltaY);
        }

        // 重みを正規化し、番号と値が有効か返します(weight: 補正する重み, boneCount: ボーン数)。
        [[nodiscard]] bool NormalizeWeight(
            SpriteSkinWeight& weight,
            const std::size_t boneCount) noexcept
        {
            // 重みの合計
            float total{};
            // 確認する影響の番号
            for (std::size_t index = 0;
                index < SpriteSkinWeight::MaximumInfluences;
                ++index)
            {
                if (!std::isfinite(weight.weights[index])
                    || weight.weights[index] < 0.0f
                    || (weight.weights[index] > 0.0f
                        && weight.bones[index] >= boneCount))
                {
                    return false;
                }
                total += weight.weights[index];
            }
            if (total <= 1.0e-6f)
            {
                return false;
            }
            // 正規化する重み
            for (auto& value : weight.weights)
            {
                value /= total;
            }
            return true;
        }
    }

    SpriteSkin2DComponent::SpriteSkin2DComponent(
        std::vector<std::uint64_t> bones)
        : m_bones(std::move(bones))
    {
    }

    void SpriteSkin2DComponent::SetBones(
        std::vector<std::uint64_t> bones)
    {
        m_bones = std::move(bones);
        m_weights.clear();
        Unbind();
    }

    bool SpriteSkin2DComponent::RemapBones(
        std::vector<std::uint64_t> bones)
    {
        if (bones.size() != m_bones.size())
        {
            return false;
        }
        m_bones = std::move(bones);
        return true;
    }

    void SpriteSkin2DComponent::SetWeightFalloff(
        const float falloff) noexcept
    {
        m_weightFalloff = std::isfinite(falloff)
            ? std::clamp(falloff, 0.5f, 16.0f)
            : DefaultWeightFalloff;
    }

    bool SpriteSkin2DComponent::Bind()
    {
        // 格子を持つ同じGameObjectのSprite Renderer
        const auto* sprite =
            Owner().GetComponent<SpriteRendererComponent>();
        if (sprite == nullptr
            || m_bones.empty()
            || m_bones.size()
                > std::numeric_limits<std::uint16_t>::max())
        {
            return false;
        }
        // ボーンを探すシーン
        const auto& scene = Owner().GetScene();
        // 今回記録するボーンの姿勢
        std::vector<DirectX::XMFLOAT4X4> bonePoses;
        bonePoses.reserve(m_bones.size());
        // 姿勢を記録するボーンID
        for (const auto id : m_bones)
        {
            // ボーンにするGameObject
            const auto* bone = scene.FindGameObject(id);
            if (bone == nullptr)
            {
                return false;
            }
            // ボーンのワールド行列
            const auto world = bone->WorldMatrix();
            if (!IsInvertible(world))
            {
                return false;
            }
            bonePoses.push_back(ToFloat4x4(world));
        }
        // Spriteのワールド行列
        const auto spriteWorld = Owner().WorldMatrix();
        if (!IsInvertible(spriteWorld))
        {
            return false;
        }

        m_boneBindPoses = std::move(bonePoses);
        m_spriteBindPose = ToFloat4x4(spriteWorld);
        m_boundColumns = sprite->MeshColumns();
        m_boundRows = sprite->MeshRows();
        m_bound = true;
        if (m_weights.size() != sprite->MeshVertexCount())
        {
            static_cast<void>(ComputeAutomaticWeights());
        }
        return true;
    }

    bool SpriteSkin2DComponent::ComputeAutomaticWeights()
    {
        // 格子を持つ同じGameObjectのSprite Renderer
        const auto* sprite =
            Owner().GetComponent<SpriteRendererComponent>();
        if (!m_bound
            || sprite == nullptr
            || sprite->MeshColumns() != m_boundColumns
            || sprite->MeshRows() != m_boundRows)
        {
            return false;
        }
        // ボーンの親子を調べるシーン
        const auto& scene = Owner().GetScene();
        // 各ボーンの影響範囲の始点
        std::vector<DirectX::XMFLOAT2> starts;
        // 各ボーンの影響範囲の終点
        std::vector<DirectX::XMFLOAT2> ends;
        starts.reserve(m_bones.size());
        ends.reserve(m_bones.size());
        // 始点を求めるボーン番号
        for (const auto& pose : m_boneBindPoses)
        {
            starts.push_back(Translation(pose));
            ends.push_back(Translation(pose));
        }
        // 子のボーンへ向かう線分を影響範囲にします。
        // 子を探すボーン番号
        for (std::size_t child = 0; child < m_bones.size(); ++child)
        {
            // 子にあたるGameObject
            const auto* childObject = scene.FindGameObject(m_bones[child]);
            // 子の親のGameObject
            const auto* parentObject = childObject != nullptr
                ? childObject->Parent()
                : nullptr;
            // 親にあたるボーン番号
            for (std::size_t parent = 0; parent < m_bones.size(); ++parent)
            {
                if (parentObject != nullptr
                    && parentObject->Id() == m_bones[parent]
                    && ends[parent].x == starts[parent].x
                    && ends[parent].y == starts[parent].y)
                {
                    ends[parent] = starts[child];
                }
            }
        }
        // 子のない末端のボーンは、親のボーンからの向きと長さで先へ延ばします。
        // 延ばす末端のボーン番号
        for (std::size_t tip = 0; tip < m_bones.size(); ++tip)
        {
            if (ends[tip].x != starts[tip].x
                || ends[tip].y != starts[tip].y)
            {
                continue;
            }
            // 末端のGameObject
            const auto* tipObject = scene.FindGameObject(m_bones[tip]);
            // 末端の親のGameObject
            const auto* parentObject = tipObject != nullptr
                ? tipObject->Parent()
                : nullptr;
            // 親にあたるボーン番号
            for (std::size_t parent = 0; parent < m_bones.size(); ++parent)
            {
                if (parentObject != nullptr
                    && parentObject->Id() == m_bones[parent])
                {
                    ends[tip] = {
                        starts[tip].x * 2.0f - starts[parent].x,
                        starts[tip].y * 2.0f - starts[parent].y };
                    break;
                }
            }
        }

        // バインド時のSpriteのワールド行列
        const auto spriteBind = DirectX::XMLoadFloat4x4(&m_spriteBindPose);
        // 重みを付ける静止頂点
        const auto rest = sprite->MeshRestPositions();
        // 各ボーンの距離による重み
        std::vector<float> scores(m_bones.size());
        // 重みの大きい順のボーン番号
        std::vector<std::uint16_t> order(m_bones.size());
        // 新しい頂点ごとの重み
        std::vector<SpriteSkinWeight> weights;
        weights.reserve(rest.size());
        // 重みを付ける頂点
        for (const auto& local : rest)
        {
            // 頂点のバインド時のワールド位置
            DirectX::XMFLOAT3 world{};
            DirectX::XMStoreFloat3(
                &world,
                DirectX::XMVector3TransformCoord(
                    DirectX::XMVectorSet(local.x, local.y, 0.0f, 1.0f),
                    spriteBind));
            // 距離を測るボーン番号
            for (std::size_t bone = 0; bone < m_bones.size(); ++bone)
            {
                scores[bone] = 1.0f / std::pow(
                    std::max(
                        DistanceToSegment(
                            { world.x, world.y },
                            starts[bone],
                            ends[bone]),
                        MinimumWeightDistance),
                    m_weightFalloff);
            }
            std::iota(order.begin(), order.end(), std::uint16_t{});
            // 影響として残すボーンの数
            const std::size_t kept = std::min(
                order.size(),
                SpriteSkinWeight::MaximumInfluences);
            std::partial_sort(
                order.begin(),
                order.begin() + static_cast<std::ptrdiff_t>(kept),
                order.end(),
                // 重みの大きい順に並べます(left: 一方の番号, right: 他方の番号)。
                [&scores](const std::uint16_t left, const std::uint16_t right)
                {
                    return scores[left] > scores[right];
                });
            // この頂点の重み
            SpriteSkinWeight weight{};
            weight.weights = {};
            // 残す影響の番号
            for (std::size_t index = 0; index < kept; ++index)
            {
                weight.bones[index] = order[index];
                weight.weights[index] = scores[order[index]];
            }
            if (!NormalizeWeight(weight, m_bones.size()))
            {
                weight = {};
            }
            weights.push_back(weight);
        }
        m_weights = std::move(weights);
        return true;
    }

    void SpriteSkin2DComponent::Unbind() noexcept
    {
        m_bound = false;
        m_boneBindPoses.clear();
        m_boundColumns = 0;
        m_boundRows = 0;
    }

    bool SpriteSkin2DComponent::SetWeights(
        std::vector<SpriteSkinWeight> weights)
    {
        if (!m_bound
            || weights.size()
                != static_cast<std::size_t>(m_boundColumns + 1)
                    * static_cast<std::size_t>(m_boundRows + 1))
        {
            return false;
        }
        // 正規化する頂点の重み
        for (auto& weight : weights)
        {
            if (!NormalizeWeight(weight, m_bones.size()))
            {
                return false;
            }
        }
        m_weights = std::move(weights);
        return true;
    }

    bool SpriteSkin2DComponent::RestoreBinding(
        std::vector<DirectX::XMFLOAT4X4> boneBindPoses,
        const DirectX::XMFLOAT4X4& spriteBindPose,
        std::vector<SpriteSkinWeight> weights,
        const int columns,
        const int rows)
    {
        Unbind();
        if (boneBindPoses.size() != m_bones.size()
            || m_bones.empty()
            || columns < 1
            || rows < 1
            || weights.size()
                != static_cast<std::size_t>(columns + 1)
                    * static_cast<std::size_t>(rows + 1)
            || !IsInvertible(DirectX::XMLoadFloat4x4(&spriteBindPose)))
        {
            return false;
        }
        // 確認するボーンの姿勢
        for (const auto& pose : boneBindPoses)
        {
            if (!IsInvertible(DirectX::XMLoadFloat4x4(&pose)))
            {
                return false;
            }
        }
        // 正規化する頂点の重み
        for (auto& weight : weights)
        {
            if (!NormalizeWeight(weight, m_bones.size()))
            {
                return false;
            }
        }
        m_boneBindPoses = std::move(boneBindPoses);
        m_spriteBindPose = spriteBindPose;
        m_weights = std::move(weights);
        m_boundColumns = columns;
        m_boundRows = rows;
        m_bound = true;
        return true;
    }

    std::vector<GameObject*> SpriteSkin2DComponent::CreateBoneChain(
        const int boneCount,
        const bool addSway)
    {
        // 長さを決めるSprite Renderer
        const auto* sprite =
            Owner().GetComponent<SpriteRendererComponent>();
        if (sprite == nullptr)
        {
            return {};
        }
        // 1〜16へ収めたボーン数
        const int count = std::clamp(boneCount, 1, 16);
        // Spriteの基準点比率
        const auto& pivot = sprite->Pivot();
        // Spriteの表示サイズ
        const auto& size = sprite->Size();
        // 基準点から反対側の端までのローカル位置
        DirectX::XMFLOAT2 tip{
            (1.0f - 2.0f * pivot.x) * size.x,
            (1.0f - 2.0f * pivot.y) * size.y };
        if (std::abs(tip.x) + std::abs(tip.y) < 1.0e-3f)
        {
            tip = { 0.0f, size.y * 0.5f };
        }
        // 1本のボーンの長さと向き
        const DirectX::XMFLOAT2 segment{
            tip.x / static_cast<float>(count),
            tip.y / static_cast<float>(count) };

        // ボーンを追加するシーン
        auto& scene = Owner().GetScene();
        // 作ったボーン
        std::vector<GameObject*> bones;
        // 新しいボーンの親
        GameObject* parent = &Owner();
        // 作るボーンの番号
        for (int index = 0; index < count; ++index)
        {
            // 新しいボーン
            auto& bone = scene.CreateGameObject(
                Owner().Name() + "_Bone" + std::to_string(index));
            bone.SetParent(parent);
            bone.GetTransform().position = index == 0
                ? DirectX::XMFLOAT3{}
                : DirectX::XMFLOAT3{ segment.x, segment.y, 0.0f };
            // 根元を頭などに固定したまま先だけをしならせるため、2本以上なら根元は揺らしません。
            if (addSway && (index > 0 || count == 1))
            {
                // 次のボーンへ向かう揺れ設定
                Sway2DSettings settings{};
                settings.tipOffset = segment;
                bone.AddComponent<Sway2DComponent>(settings);
            }
            bones.push_back(&bone);
            parent = &bone;
        }

        // 作ったボーンのID列
        std::vector<std::uint64_t> ids;
        ids.reserve(bones.size());
        // IDを集めるボーン
        for (const auto* bone : bones)
        {
            ids.push_back(bone->Id());
        }
        SetBones(std::move(ids));
        static_cast<void>(Bind());
        return bones;
    }

    void SpriteSkin2DComponent::DeformSpriteMesh(
        const SpriteRendererComponent& sprite,
        std::vector<DirectX::XMFLOAT2>& positions)
    {
        if (!m_bound
            || sprite.MeshColumns() != m_boundColumns
            || sprite.MeshRows() != m_boundRows
            || positions.size() != m_weights.size())
        {
            return;
        }
        using namespace DirectX;

        // 現在のSpriteのワールド行列
        const XMMATRIX spriteWorld = Owner().WorldMatrix();
        if (!IsInvertible(spriteWorld))
        {
            return;
        }
        // ワールドからSpriteのローカルへ戻す行列
        const XMMATRIX worldToSprite =
            XMMatrixInverse(nullptr, spriteWorld);
        // バインド時のSpriteのワールド行列
        const XMMATRIX spriteBind = XMLoadFloat4x4(&m_spriteBindPose);
        // 見つからないボーンはSpriteと一緒に動かします。
        // Spriteの動きだけを写す行列
        const XMMATRIX rigid =
            XMMatrixInverse(nullptr, spriteBind) * spriteWorld;
        // ボーンを探すシーン
        const auto& scene = Owner().GetScene();
        // バインド時から現在へ頂点を移すボーンごとの行列
        std::vector<XMFLOAT4X4> skinning(m_bones.size());
        // 行列を求めるボーン番号
        for (std::size_t index = 0; index < m_bones.size(); ++index)
        {
            // ボーンにするGameObject
            const auto* bone = scene.FindGameObject(m_bones[index]);
            // バインド時のボーンのワールド行列
            const XMMATRIX bindPose =
                XMLoadFloat4x4(&m_boneBindPoses[index]);
            XMStoreFloat4x4(
                &skinning[index],
                bone != nullptr
                    ? XMMatrixInverse(nullptr, bindPose)
                        * bone->WorldMatrix()
                    : rigid);
        }

        // 変形する頂点番号
        for (std::size_t vertex = 0; vertex < positions.size(); ++vertex)
        {
            // 頂点のバインド時のワールド位置
            const XMVECTOR bindWorld = XMVector3TransformCoord(
                XMVectorSet(
                    positions[vertex].x,
                    positions[vertex].y,
                    0.0f,
                    1.0f),
                spriteBind);
            // 重みで混ぜた現在のワールド位置
            XMVECTOR blended = XMVectorZero();
            // 頂点の重み
            const auto& weight = m_weights[vertex];
            // 混ぜる影響の番号
            for (std::size_t influence = 0;
                influence < SpriteSkinWeight::MaximumInfluences;
                ++influence)
            {
                if (weight.weights[influence] <= 0.0f)
                {
                    continue;
                }
                blended = XMVectorAdd(
                    blended,
                    XMVectorScale(
                        XMVector3TransformCoord(
                            bindWorld,
                            XMLoadFloat4x4(
                                &skinning[weight.bones[influence]])),
                        weight.weights[influence]));
            }
            // Spriteのローカルへ戻した位置
            XMFLOAT3 local{};
            XMStoreFloat3(
                &local,
                XMVector3TransformCoord(blended, worldToSprite));
            positions[vertex] = { local.x, local.y };
        }
    }

    void SpriteSkin2DComponent::OnInitialize(GraphicsDevice&)
    {
        if (!m_bound && !m_bones.empty())
        {
            static_cast<void>(Bind());
        }
    }
}
