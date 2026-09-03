#include "LamaPon/Animation/AnimationClip.h"

#include "LamaPon/Core/PathUtils.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <numbers>
#include <stdexcept>

namespace
{
    using Json = nlohmann::json;

    DirectX::XMFLOAT3 ReadFloat3(
        const Json& value,
        const char* field)
    {
        if (!value.is_array() || value.size() != 3)
        {
            throw std::runtime_error(
                std::string{ field }
                + " must contain three numbers.");
        }
        DirectX::XMFLOAT3 result{
            value.at(0).get<float>(),
            value.at(1).get<float>(),
            value.at(2).get<float>()
        };
        if (!std::isfinite(result.x)
            || !std::isfinite(result.y)
            || !std::isfinite(result.z))
        {
            throw std::runtime_error(
                std::string{ field }
                + " contains a non-finite value.");
        }
        return result;
    }

    Json ToJson(
        const DirectX::XMFLOAT3& value)
    {
        return Json::array({
            value.x,
            value.y,
            value.z
        });
    }

    void WriteTextAtomically(
        const std::filesystem::path& path,
        const std::string_view text)
    {
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(
                path.parent_path());
        }
        auto temporaryPath = path;
        temporaryPath += L".tmp";
        std::ofstream output(
            temporaryPath,
            std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Could not open Animation Clip for writing: "
                + LamaPon::PathToUtf8(temporaryPath));
        }
        output << text;
        output.close();
        if (!output)
        {
            std::error_code cleanupError;
            std::filesystem::remove(
                temporaryPath,
                cleanupError);
            throw std::runtime_error(
                "Could not write Animation Clip: "
                + LamaPon::PathToUtf8(temporaryPath));
        }
        if (!MoveFileExW(
                temporaryPath.c_str(),
                path.c_str(),
                MOVEFILE_REPLACE_EXISTING
                    | MOVEFILE_WRITE_THROUGH))
        {
            std::error_code cleanupError;
            std::filesystem::remove(
                temporaryPath,
                cleanupError);
            throw std::runtime_error(
                "Could not replace Animation Clip: "
                + LamaPon::PathToUtf8(path));
        }
    }

    float Lerp(
        const float from,
        const float to,
        const float amount) noexcept
    {
        return from + (to - from) * amount;
    }

    DirectX::XMFLOAT3 LerpFloat3(
        const DirectX::XMFLOAT3& from,
        const DirectX::XMFLOAT3& to,
        const float amount) noexcept
    {
        return {
            Lerp(from.x, to.x, amount),
            Lerp(from.y, to.y, amount),
            Lerp(from.z, to.z, amount)
        };
    }

    float LerpAngle(
        const float from,
        const float to,
        const float amount) noexcept
    {
        constexpr float TwoPi =
            std::numbers::pi_v<float> * 2.0f;
        const float delta =
            std::remainder(to - from, TwoPi);
        return from + delta * amount;
    }

    DirectX::XMFLOAT3 LerpRotation(
        const DirectX::XMFLOAT3& from,
        const DirectX::XMFLOAT3& to,
        const float amount) noexcept
    {
        return {
            LerpAngle(from.x, to.x, amount),
            LerpAngle(from.y, to.y, amount),
            LerpAngle(from.z, to.z, amount)
        };
    }

}

namespace LamaPon
{
    AnimationClip AnimationClip::LoadFromFile(
        const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            throw std::runtime_error(
                "Could not open animation clip: "
                + PathToUtf8(path));
        }
        const std::string json{
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{}
        };
        return FromJson(json);
    }

    AnimationClip AnimationClip::FromJson(
        const std::string_view json)
    {
        const Json document =
            Json::parse(json.begin(), json.end());
        if (document.value(
                "format",
                std::string{})
            != "LamaPonAnimationClip"
            || document.value("version", 0) != 1)
        {
            throw std::runtime_error(
                "Unsupported LamaPon animation clip format.");
        }

        const auto keyframes =
            document.find("keyframes");
        if (keyframes == document.end()
            || !keyframes->is_array()
            || keyframes->empty()
            || keyframes->size() > 4096)
        {
            throw std::runtime_error(
                "Animation clip requires between 1 and 4096 keyframes.");
        }

        AnimationClip clip;
        clip.m_name =
            document.value(
                "name",
                std::string{ "Animation" });
        clip.m_loop =
            document.value("loop", true);
        clip.m_keyframes.reserve(
            keyframes->size());

        float previousTime = -1.0f;
        for (const auto& value : *keyframes)
        {
            const float time =
                value.at("time").get<float>();
            if (!std::isfinite(time)
                || time < 0.0f
                || time <= previousTime)
            {
                throw std::runtime_error(
                    "Animation keyframe times must be finite, non-negative, and strictly increasing.");
            }
            previousTime = time;
            clip.m_keyframes.push_back(
                TransformKeyframe{
                    time,
                    TransformAnimationSample{
                        ReadFloat3(
                            value.at("position"),
                            "position"),
                        ReadFloat3(
                            value.at("rotation"),
                            "rotation"),
                        ReadFloat3(
                            value.at("scale"),
                            "scale")
                    }
                });
        }

        const float lastKeyTime =
            clip.m_keyframes.back().time;
        clip.m_duration =
            document.value(
                "duration",
                lastKeyTime);
        if (!std::isfinite(clip.m_duration)
            || clip.m_duration < lastKeyTime
            || clip.m_duration <= 0.0f)
        {
            throw std::runtime_error(
                "Animation duration must be positive and include every keyframe.");
        }
        return clip;
    }

    AnimationClip AnimationClip::Create(
        std::string name,
        const float duration,
        const bool loop,
        std::vector<TransformKeyframe> keyframes)
    {
        AnimationClip clip;
        clip.m_name = std::move(name);
        clip.m_duration = duration;
        clip.m_loop = loop;
        clip.m_keyframes =
            std::move(keyframes);
        return FromJson(
            clip.SerializeToJson());
    }

    std::string AnimationClip::SerializeToJson() const
    {
        Json document{
            { "format", "LamaPonAnimationClip" },
            { "version", 1 },
            { "name", m_name },
            { "duration", m_duration },
            { "loop", m_loop },
            { "keyframes", Json::array() }
        };
        for (const auto& keyframe :
            m_keyframes)
        {
            document["keyframes"].push_back(
                Json{
                    { "time", keyframe.time },
                    {
                        "position",
                        ToJson(
                            keyframe.transform.position)
                    },
                    {
                        "rotation",
                        ToJson(
                            keyframe.transform.rotation)
                    },
                    {
                        "scale",
                        ToJson(
                            keyframe.transform.scale)
                    }
                });
        }
        return document.dump(2);
    }

    void AnimationClip::SaveToFile(
        const std::filesystem::path& path) const
    {
        WriteTextAtomically(
            path,
            SerializeToJson() + '\n');
    }

    TransformAnimationSample AnimationClip::Sample(
        const float time) const noexcept
    {
        if (m_keyframes.empty())
        {
            return {};
        }
        if (time <= m_keyframes.front().time)
        {
            return m_keyframes.front().transform;
        }
        if (time >= m_keyframes.back().time)
        {
            return m_keyframes.back().transform;
        }

        const auto upper = std::upper_bound(
            m_keyframes.begin(),
            m_keyframes.end(),
            time,
            [](const float value,
                const TransformKeyframe& keyframe)
            {
                return value < keyframe.time;
            });
        const auto& to = *upper;
        const auto& from = *(upper - 1);
        const float amount =
            (time - from.time)
            / (to.time - from.time);
        return {
            LerpFloat3(
                from.transform.position,
                to.transform.position,
                amount),
            LerpRotation(
                from.transform.rotation,
                to.transform.rotation,
                amount),
            LerpFloat3(
                from.transform.scale,
                to.transform.scale,
                amount)
        };
    }

    DirectX::XMFLOAT4 AnimationClip::
        SampleRotationQuaternion(const float time) const noexcept
    {
        if (m_keyframes.empty())
        {
            return { 0.0f, 0.0f, 0.0f, 1.0f };
        }

        const auto toQuaternion = [](
            const TransformAnimationSample& sample)
        {
            return DirectX::XMQuaternionRotationRollPitchYaw(
                sample.rotation.x,
                sample.rotation.y,
                sample.rotation.z);
        };

        DirectX::XMVECTOR result{};
        if (time <= m_keyframes.front().time)
        {
            result = toQuaternion(
                m_keyframes.front().transform);
        }
        else if (time >= m_keyframes.back().time)
        {
            result = toQuaternion(
                m_keyframes.back().transform);
        }
        else
        {
            const auto upper = std::upper_bound(
                m_keyframes.begin(),
                m_keyframes.end(),
                time,
                [](const float value,
                    const TransformKeyframe& keyframe)
                {
                    return value < keyframe.time;
                });
            const auto& to = *upper;
            const auto& from = *(upper - 1);
            const float amount =
                (time - from.time)
                / (to.time - from.time);
            auto fromRotation =
                toQuaternion(from.transform);
            auto toRotation =
                toQuaternion(to.transform);
            if (DirectX::XMVectorGetX(
                    DirectX::XMVector4Dot(
                        fromRotation,
                        toRotation)) < 0.0f)
            {
                toRotation =
                    DirectX::XMVectorNegate(toRotation);
            }
            result = DirectX::XMQuaternionSlerp(
                fromRotation,
                toRotation,
                amount);
        }

        DirectX::XMFLOAT4 output{};
        DirectX::XMStoreFloat4(
            &output,
            DirectX::XMQuaternionNormalize(result));
        return output;
    }
}
