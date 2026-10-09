#include "LamaPon/Components/Keyform2DComponent.h"

#include "LamaPon/Components/Rig2DComponent.h"
#include "LamaPon/Components/SpriteRendererComponent.h"
#include "LamaPon/Scene/GameObject.h"
#include "LamaPon/Scene/Transform.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace LamaPon
{
    namespace
    {
        // 1チャンネルのキー数の上限
        constexpr std::size_t MaximumKeysPerChannel = 64;
        // チャンネル数の上限
        constexpr std::size_t MaximumChannels = 64;
        // 値の絶対値の上限
        constexpr float MaximumMagnitude = 1.0e6f;

        // 値が有限かつ上限内か返します(value: 確認する値)。
        [[nodiscard]] bool IsUsable(const float value) noexcept
        {
            return std::isfinite(value)
                && std::abs(value) <= MaximumMagnitude;
        }

        // キーの全ての値が有限か返します(key: 確認するキー)。
        [[nodiscard]] bool IsUsableKey(const Keyform2DKey& key) noexcept
        {
            if (!IsUsable(key.value)
                || !IsUsable(key.positionOffset.x)
                || !IsUsable(key.positionOffset.y)
                || !IsUsable(key.rotationDegrees)
                || !IsUsable(key.scale.x)
                || !IsUsable(key.scale.y)
                || !IsUsable(key.opacity)
                || key.vertexOffsets.size() > MaximumSpriteMeshVertices)
            {
                return false;
            }
            return std::all_of(
                key.vertexOffsets.begin(),
                key.vertexOffsets.end(),
                // 有限の移動量か判定します(offset: 確認する移動量)。
                [](const DirectX::XMFLOAT2& offset)
                {
                    return IsUsable(offset.x) && IsUsable(offset.y);
                });
        }

        // 2つの値を線形補間します(from: 始点, to: 終点, amount: 0〜1の補間率)。
        [[nodiscard]] float Lerp(
            const float from,
            const float to,
            const float amount) noexcept
        {
            return from + (to - from) * amount;
        }

        // 度を-180〜180へ収めます(degrees: 角度の度)。
        [[nodiscard]] float WrapDegrees(const float degrees) noexcept
        {
            // 360度で割った余り
            const float wrapped = std::fmod(degrees + 180.0f, 360.0f);
            return wrapped < 0.0f ? wrapped + 180.0f : wrapped - 180.0f;
        }

        // 分母が0に近ければ1を返す比を求めます(numerator: 分子, denominator: 分母)。
        [[nodiscard]] float SafeRatio(
            const float numerator,
            const float denominator) noexcept
        {
            return std::abs(denominator) > 1.0e-6f
                ? numerator / denominator
                : 1.0f;
        }

        // 値を挟む2つのキーと補間率です。
        struct KeySpan final
        {
            // 値以下で最も近いキー
            const Keyform2DKey* lower{};
            // 値以上で最も近いキー
            const Keyform2DKey* upper{};
            // lowerからupperへの補間率
            float amount{};
        };

        // 昇順のキーから値を挟む区間を求めます(keys: 空でない昇順のキー, value: パラメータ値)。
        // 範囲外は端のキーを使います。
        [[nodiscard]] KeySpan Locate(
            const std::vector<Keyform2DKey>& keys,
            const float value) noexcept
        {
            if (value <= keys.front().value)
            {
                return { &keys.front(), &keys.front(), 0.0f };
            }
            if (value >= keys.back().value)
            {
                return { &keys.back(), &keys.back(), 0.0f };
            }
            // 値より大きい最初のキー
            const auto upper = std::upper_bound(
                keys.begin(),
                keys.end(),
                value,
                // 値とキーを比べます(target: 探す値, key: 比べるキー)。
                [](const float target, const Keyform2DKey& key)
                {
                    return target < key.value;
                });
            // 値以下で最も近いキー
            const auto lower = std::prev(upper);
            // 2つのキーの値の差
            const float range = upper->value - lower->value;
            return {
                &*lower,
                &*upper,
                range > 0.0f ? (value - lower->value) / range : 0.0f };
        }

        // 回転の差のZ角度を度で返します(rest: 基準回転, current: 現在の回転)。
        [[nodiscard]] float ZRotationDegrees(
            const DirectX::XMFLOAT4& rest,
            const DirectX::XMFLOAT4& current) noexcept
        {
            // 基準の後に足された回転
            DirectX::XMFLOAT4 delta{};
            DirectX::XMStoreFloat4(
                &delta,
                DirectX::XMQuaternionMultiply(
                    DirectX::XMQuaternionInverse(
                        DirectX::XMLoadFloat4(&rest)),
                    DirectX::XMLoadFloat4(&current)));
            return WrapDegrees(
                2.0f * std::atan2(delta.z, delta.w)
                * (180.0f / std::numbers::pi_v<float>));
        }
    }

    Keyform2DComponent::Keyform2DComponent(
        std::vector<Keyform2DChannel> channels)
    {
        SetChannels(std::move(channels));
    }

    void Keyform2DComponent::SetChannels(
        std::vector<Keyform2DChannel> channels)
    {
        m_channels.clear();
        // 登録するチャンネル
        for (auto& channel : channels)
        {
            // 登録するキー
            for (auto& key : channel.keys)
            {
                static_cast<void>(
                    SetKey(channel.parameter, std::move(key)));
            }
        }
        RefreshUsage();
    }

    bool Keyform2DComponent::SetKey(
        const std::string_view parameter,
        Keyform2DKey key)
    {
        if (parameter.empty() || !IsUsableKey(key))
        {
            return false;
        }
        key.opacity = std::clamp(key.opacity, 0.0f, 1.0f);
        // キーを追加するチャンネル
        auto channel = std::find_if(
            m_channels.begin(),
            m_channels.end(),
            // 指定名のチャンネルか判定します(candidate: 判定するチャンネル)。
            [parameter](const Keyform2DChannel& candidate)
            {
                return candidate.parameter == parameter;
            });
        if (channel == m_channels.end())
        {
            if (m_channels.size() >= MaximumChannels)
            {
                return false;
            }
            m_channels.push_back({ std::string(parameter), {} });
            channel = std::prev(m_channels.end());
        }
        // 同じ値か挿入位置のキー
        const auto position = std::lower_bound(
            channel->keys.begin(),
            channel->keys.end(),
            key.value,
            // キーと値を比べます(existing: 比べるキー, target: 探す値)。
            [](const Keyform2DKey& existing, const float target)
            {
                return existing.value < target;
            });
        if (position != channel->keys.end()
            && position->value == key.value)
        {
            *position = std::move(key);
        }
        else if (channel->keys.size() >= MaximumKeysPerChannel)
        {
            return false;
        }
        else
        {
            channel->keys.insert(position, std::move(key));
        }
        RefreshUsage();
        return true;
    }

    bool Keyform2DComponent::RemoveKey(
        const std::string_view parameter,
        const float value)
    {
        // 除去したか
        bool removed{};
        // キーを探すチャンネル
        for (auto& channel : m_channels)
        {
            if (channel.parameter != parameter)
            {
                continue;
            }
            // 除去前のキー数
            const auto count = channel.keys.size();
            std::erase_if(
                channel.keys,
                // 指定値のキーか判定します(key: 判定するキー)。
                [value](const Keyform2DKey& key)
                {
                    return key.value == value;
                });
            removed = removed || channel.keys.size() != count;
        }
        std::erase_if(
            m_channels,
            // キーのないチャンネルか判定します(channel: 判定するチャンネル)。
            [](const Keyform2DChannel& channel)
            {
                return channel.keys.empty();
            });
        RefreshUsage();
        return removed;
    }

    void Keyform2DComponent::CaptureRestPose()
    {
        // 基準にする現在のローカル変換
        const auto& transform = GetTransform();
        m_restPosition = transform.position;
        m_restRotation = transform.rotationQuaternion;
        m_restScale = transform.scale;
        // 不透明度を持つSprite Renderer
        const auto* sprite =
            Owner().GetComponent<SpriteRendererComponent>();
        m_restOpacity = sprite != nullptr ? sprite->Color().w : 1.0f;
        m_hasRestPose = true;
    }

    void Keyform2DComponent::SetRestPose(
        const DirectX::XMFLOAT3& position,
        const DirectX::XMFLOAT4& rotation,
        const DirectX::XMFLOAT3& scale,
        const float opacity) noexcept
    {
        m_restPosition = position;
        // 正規化した基準回転
        Transform normalized;
        normalized.SetRotationVector(DirectX::XMLoadFloat4(&rotation));
        m_restRotation = normalized.rotationQuaternion;
        m_restScale = scale;
        m_restOpacity = std::isfinite(opacity)
            ? std::clamp(opacity, 0.0f, 1.0f)
            : 1.0f;
        m_hasRestPose = true;
    }

    bool Keyform2DComponent::RecordPoseKey(
        const std::string_view parameter,
        const float value)
    {
        if (!m_hasRestPose || !IsUsable(value))
        {
            return false;
        }
        // 記録する現在のローカル変換
        const auto& transform = GetTransform();
        // 記録するキー
        Keyform2DKey key;
        key.value = value;
        key.positionOffset = {
            transform.position.x - m_restPosition.x,
            transform.position.y - m_restPosition.y };
        key.rotationDegrees = ZRotationDegrees(
            m_restRotation,
            transform.rotationQuaternion);
        key.scale = {
            SafeRatio(transform.scale.x, m_restScale.x),
            SafeRatio(transform.scale.y, m_restScale.y) };
        // 不透明度を持つSprite Renderer
        const auto* sprite =
            Owner().GetComponent<SpriteRendererComponent>();
        key.opacity = sprite != nullptr
            ? std::clamp(
                SafeRatio(sprite->Color().w, m_restOpacity),
                0.0f,
                1.0f)
            : 1.0f;
        // 頂点移動を引き継ぐ既存のキーを探すチャンネル
        for (const auto& channel : m_channels)
        {
            if (channel.parameter != parameter)
            {
                continue;
            }
            // 同じ値か比べるキー
            for (const auto& existing : channel.keys)
            {
                if (existing.value == value)
                {
                    key.vertexOffsets = existing.vertexOffsets;
                }
            }
        }
        return SetKey(parameter, std::move(key));
    }

    bool Keyform2DComponent::RecordMeshKey(
        const std::string_view parameter,
        const float value)
    {
        // 格子を持つSprite Renderer
        const auto* sprite =
            Owner().GetComponent<SpriteRendererComponent>();
        if (sprite == nullptr || !IsUsable(value))
        {
            return false;
        }
        // 変形の起点にする頂点
        const auto base = sprite->MeshDeformation().empty()
            ? sprite->MeshRestPositions()
            : sprite->MeshDeformation();
        // この部品以外を適用した頂点
        const auto deformed = sprite->DeformedMeshPositions(this);
        if (deformed.size() != base.size())
        {
            return false;
        }
        // 記録するキー
        Keyform2DKey key;
        key.value = value;
        // 姿勢を引き継ぐ既存のキーを探すチャンネル
        for (const auto& channel : m_channels)
        {
            if (channel.parameter != parameter)
            {
                continue;
            }
            // 同じ値か比べるキー
            for (const auto& existing : channel.keys)
            {
                if (existing.value == value)
                {
                    key = existing;
                }
            }
        }
        key.vertexOffsets.resize(base.size());
        // 移動量を求める頂点番号
        for (std::size_t index = 0; index < base.size(); ++index)
        {
            key.vertexOffsets[index] = {
                deformed[index].x - base[index].x,
                deformed[index].y - base[index].y };
        }
        return SetKey(parameter, std::move(key));
    }

    Keyform2DPose Keyform2DComponent::EvaluatePose() const
    {
        // 合成した差
        Keyform2DPose pose;
        // 値を読むリグ
        const auto* rig = FindRig();
        if (rig == nullptr)
        {
            return pose;
        }
        // 合成するチャンネル
        for (const auto& channel : m_channels)
        {
            if (channel.keys.empty()
                || rig->FindParameter(channel.parameter) == nullptr)
            {
                continue;
            }
            // 現在の値を挟むキー
            const auto span = Locate(
                channel.keys,
                rig->ParameterValue(channel.parameter));
            pose.positionOffset.x += Lerp(
                span.lower->positionOffset.x,
                span.upper->positionOffset.x,
                span.amount);
            pose.positionOffset.y += Lerp(
                span.lower->positionOffset.y,
                span.upper->positionOffset.y,
                span.amount);
            pose.rotationDegrees += Lerp(
                span.lower->rotationDegrees,
                span.upper->rotationDegrees,
                span.amount);
            pose.scale.x *= Lerp(
                span.lower->scale.x,
                span.upper->scale.x,
                span.amount);
            pose.scale.y *= Lerp(
                span.lower->scale.y,
                span.upper->scale.y,
                span.amount);
            pose.opacity *= Lerp(
                span.lower->opacity,
                span.upper->opacity,
                span.amount);
        }
        pose.opacity = std::clamp(pose.opacity, 0.0f, 1.0f);
        return pose;
    }

    void Keyform2DComponent::ApplyPose()
    {
        if (!m_hasRestPose)
        {
            return;
        }
        // 基準に足す差
        const auto pose = EvaluatePose();
        if (m_usesTransform)
        {
            // 書き換えるローカル変換
            auto& transform = GetTransform();
            transform.position = {
                m_restPosition.x + pose.positionOffset.x,
                m_restPosition.y + pose.positionOffset.y,
                m_restPosition.z };
            transform.SetRotationVector(
                DirectX::XMQuaternionMultiply(
                    DirectX::XMLoadFloat4(&m_restRotation),
                    DirectX::XMQuaternionRotationAxis(
                        DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f),
                        pose.rotationDegrees
                            * (std::numbers::pi_v<float> / 180.0f))));
            transform.scale = {
                m_restScale.x * pose.scale.x,
                m_restScale.y * pose.scale.y,
                m_restScale.z };
        }
        if (m_usesOpacity)
        {
            // 不透明度を書き換えるSprite Renderer
            if (auto* sprite =
                    Owner().GetComponent<SpriteRendererComponent>())
            {
                // 不透明度だけを変えた描画色
                auto color = sprite->Color();
                color.w = m_restOpacity * pose.opacity;
                sprite->SetColor(color);
            }
        }
    }

    void Keyform2DComponent::RestoreRestPose()
    {
        if (!m_hasRestPose)
        {
            return;
        }
        if (m_usesTransform)
        {
            // 戻すローカル変換
            auto& transform = GetTransform();
            transform.position = m_restPosition;
            transform.rotationQuaternion = m_restRotation;
            transform.scale = m_restScale;
        }
        if (m_usesOpacity)
        {
            // 不透明度を戻すSprite Renderer
            if (auto* sprite =
                    Owner().GetComponent<SpriteRendererComponent>())
            {
                // 不透明度だけを戻した描画色
                auto color = sprite->Color();
                color.w = m_restOpacity;
                sprite->SetColor(color);
            }
        }
    }

    void Keyform2DComponent::DeformSpriteMesh(
        const SpriteRendererComponent&,
        std::vector<DirectX::XMFLOAT2>& positions)
    {
        // 値を読むリグ
        const auto* rig = FindRig();
        if (rig == nullptr)
        {
            return;
        }
        // 足し合わせるチャンネル
        for (const auto& channel : m_channels)
        {
            if (channel.keys.empty()
                || rig->FindParameter(channel.parameter) == nullptr)
            {
                continue;
            }
            // 現在の値を挟むキー
            const auto span = Locate(
                channel.keys,
                rig->ParameterValue(channel.parameter));
            // 下側のキーの頂点移動を使えるか
            const bool lowerUsable =
                span.lower->vertexOffsets.size() == positions.size();
            // 上側のキーの頂点移動を使えるか
            const bool upperUsable =
                span.upper->vertexOffsets.size() == positions.size();
            if (!lowerUsable && !upperUsable)
            {
                continue;
            }
            // 移動する頂点番号
            for (std::size_t index = 0; index < positions.size(); ++index)
            {
                // 下側のキーの移動量
                const DirectX::XMFLOAT2 lower = lowerUsable
                    ? span.lower->vertexOffsets[index]
                    : DirectX::XMFLOAT2{};
                // 上側のキーの移動量
                const DirectX::XMFLOAT2 upper = upperUsable
                    ? span.upper->vertexOffsets[index]
                    : DirectX::XMFLOAT2{};
                positions[index].x += Lerp(lower.x, upper.x, span.amount);
                positions[index].y += Lerp(lower.y, upper.y, span.amount);
            }
        }
    }

    bool Keyform2DComponent::DeformsSpriteMesh() const
    {
        // 頂点移動を探すチャンネル
        for (const auto& channel : m_channels)
        {
            // 頂点移動を探すキー
            for (const auto& key : channel.keys)
            {
                if (!key.vertexOffsets.empty())
                {
                    return true;
                }
            }
        }
        return false;
    }

    void Keyform2DComponent::OnInitialize(GraphicsDevice&)
    {
        if (!m_hasRestPose)
        {
            CaptureRestPose();
        }
    }

    void Keyform2DComponent::OnUpdate(float)
    {
        ApplyPose();
    }

    void Keyform2DComponent::OnActiveStateChanged(const bool active)
    {
        if (!active)
        {
            RestoreRestPose();
        }
    }

    Rig2DComponent* Keyform2DComponent::FindRig() const
    {
        return Owner().GetComponentInParent<Rig2DComponent>(true);
    }

    void Keyform2DComponent::RefreshUsage() noexcept
    {
        m_usesTransform = false;
        m_usesOpacity = false;
        // 確認するチャンネル
        for (const auto& channel : m_channels)
        {
            // 確認するキー
            for (const auto& key : channel.keys)
            {
                m_usesTransform = m_usesTransform
                    || key.positionOffset.x != 0.0f
                    || key.positionOffset.y != 0.0f
                    || key.rotationDegrees != 0.0f
                    || key.scale.x != 1.0f
                    || key.scale.y != 1.0f;
                m_usesOpacity = m_usesOpacity || key.opacity != 1.0f;
            }
        }
    }
}
