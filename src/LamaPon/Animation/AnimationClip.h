#pragma once

#include <DirectXMath.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace LamaPon
{
    struct TransformAnimationSample final
    {
        DirectX::XMFLOAT3 position{};
        DirectX::XMFLOAT3 rotation{};
        DirectX::XMFLOAT3 scale{
            1.0f,
            1.0f,
            1.0f
        };
    };

    struct TransformKeyframe final
    {
        float time{};
        TransformAnimationSample transform;
    };

    class AnimationClip final
    {
    public:
        static AnimationClip LoadFromFile(
            const std::filesystem::path& path);
        static AnimationClip FromJson(
            std::string_view json);
        static AnimationClip Create(
            std::string name,
            float duration,
            bool loop,
            std::vector<TransformKeyframe> keyframes);

        [[nodiscard]] std::string
            SerializeToJson() const;
        void SaveToFile(
            const std::filesystem::path& path) const;

        [[nodiscard]] TransformAnimationSample
            Sample(float time) const noexcept;
        // 通常再生・プレビュー用の回転サンプルです。JSONの回転値は
        // Euler角のまま保持しますが、実際の補間はクォータニオンSlerp
        // で行い、ジンバルロックや軸ごとの振れを避けます。
        [[nodiscard]] DirectX::XMFLOAT4
            SampleRotationQuaternion(float time) const noexcept;
        [[nodiscard]] const std::string& Name() const noexcept
        {
            return m_name;
        }
        [[nodiscard]] float Duration() const noexcept
        {
            return m_duration;
        }
        [[nodiscard]] bool Loop() const noexcept
        {
            return m_loop;
        }
        [[nodiscard]] const std::vector<TransformKeyframe>&
            Keyframes() const noexcept
        {
            return m_keyframes;
        }

    private:
        std::string m_name;
        float m_duration{};
        bool m_loop{ true };
        std::vector<TransformKeyframe> m_keyframes;
    };
}
