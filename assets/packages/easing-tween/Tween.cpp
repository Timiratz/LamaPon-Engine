// Tween（トゥイーン）— GameObjectの位置・回転・拡縮を、時間をかけて
// 目標値へ動かします。動きの緩急はイージング種別で選べます。
//
// 使い方: GameObjectへ「Tween（移動・回転・拡縮）」を追加し、
// Inspectorで目標値・時間・種別を設定します。C++からイージングだけ
// 使いたい場合はEasing.hを直接includeしてください。
#include "LamaPon/LamaPon.h"

#include "Easing.h"

#include <nlohmann/json.hpp>

#include <string>

namespace
{
    // Inspectorに出す入力欄の定義です。種別は文字列で持ち、
    // Easing.hのTypeFromNameで解釈します。
    constexpr char TweenSchema[] = R"({
        "fields": [
            {
                "name": "easing",
                "displayName": "動きの緩急",
                "type": "string",
                "default": "OutCubic"
            },
            {
                "name": "duration",
                "displayName": "所要時間（秒）",
                "type": "float",
                "default": 1.0,
                "min": 0.01,
                "max": 60.0,
                "step": 0.05
            },
            {
                "name": "delay",
                "displayName": "開始までの待ち（秒）",
                "type": "float",
                "default": 0.0,
                "min": 0.0,
                "max": 60.0,
                "step": 0.05
            },
            {
                "name": "loop",
                "displayName": "繰り返す",
                "type": "bool",
                "default": false
            },
            {
                "name": "pingPong",
                "displayName": "往復する",
                "type": "bool",
                "default": true
            },
            {
                "name": "playOnStart",
                "displayName": "開始時に再生",
                "type": "bool",
                "default": true
            },
            {
                "name": "movePosition",
                "displayName": "位置を動かす",
                "type": "bool",
                "default": true
            },
            {
                "name": "offsetX",
                "displayName": "移動量 X",
                "type": "float",
                "default": 0.0,
                "min": -100.0,
                "max": 100.0,
                "step": 0.1
            },
            {
                "name": "offsetY",
                "displayName": "移動量 Y",
                "type": "float",
                "default": 1.0,
                "min": -100.0,
                "max": 100.0,
                "step": 0.1
            },
            {
                "name": "offsetZ",
                "displayName": "移動量 Z",
                "type": "float",
                "default": 0.0,
                "min": -100.0,
                "max": 100.0,
                "step": 0.1
            },
            {
                "name": "rotate",
                "displayName": "回転させる",
                "type": "bool",
                "default": false
            },
            {
                "name": "rotationX",
                "displayName": "回転量 X（度）",
                "type": "float",
                "default": 0.0,
                "min": -3600.0,
                "max": 3600.0,
                "step": 5.0
            },
            {
                "name": "rotationY",
                "displayName": "回転量 Y（度）",
                "type": "float",
                "default": 180.0,
                "min": -3600.0,
                "max": 3600.0,
                "step": 5.0
            },
            {
                "name": "rotationZ",
                "displayName": "回転量 Z（度）",
                "type": "float",
                "default": 0.0,
                "min": -3600.0,
                "max": 3600.0,
                "step": 5.0
            },
            {
                "name": "scale",
                "displayName": "拡縮させる",
                "type": "bool",
                "default": false
            },
            {
                "name": "scaleMultiplier",
                "displayName": "拡縮の倍率",
                "type": "float",
                "default": 1.5,
                "min": 0.01,
                "max": 10.0,
                "step": 0.05
            }
        ]
    })";
}

class Tween final : public LamaPon::Script
{
public:
    void Start() override
    {
        // 開始時のTransformを基準にして、そこからの相対で動かします。
        const auto& transform = GetTransform();
        m_startPosition = transform.position;
        m_startRotation = transform.EulerAngles();
        m_startScale = transform.scale;
        m_elapsed = 0.0f;
        m_forward = true;
        m_finished = false;
        m_playing = m_playOnStart;
    }

    void Update(const float deltaTime) override
    {
        if (!m_playing || m_finished)
        {
            return;
        }

        m_elapsed += deltaTime;
        if (m_elapsed < m_delay)
        {
            return;
        }

        const float active = m_elapsed - m_delay;
        const float duration = m_duration > 0.01f
            ? m_duration
            : 0.01f;
        float progress = active / duration;
        if (progress >= 1.0f)
        {
            if (m_loop)
            {
                // 往復なら向きを変え、そうでなければ最初へ戻します。
                progress = 1.0f;
                m_elapsed = m_delay;
                if (m_pingPong)
                {
                    m_forward = !m_forward;
                }
            }
            else
            {
                progress = 1.0f;
                m_finished = true;
            }
        }

        const float eased = LamaPonEasing::Ease(
            m_easing,
            m_forward ? progress : 1.0f - progress);
        Apply(eased);
    }

    // 進み具合を保存しておくと、再生中にホットリロードしても
    // 途中から続きます。
    void LoadProperties(
        const std::string_view propertiesJson) override
    {
        const auto properties = nlohmann::json::parse(
            propertiesJson.empty()
                ? std::string{ "{}" }
                : std::string{ propertiesJson },
            nullptr,
            false);
        if (properties.is_discarded()
            || !properties.is_object())
        {
            return;
        }

        m_easing = LamaPonEasing::TypeFromName(
            properties.value(
                "easing",
                std::string{ "OutCubic" }));
        m_duration = properties.value("duration", 1.0f);
        m_delay = properties.value("delay", 0.0f);
        m_loop = properties.value("loop", false);
        m_pingPong = properties.value("pingPong", true);
        m_playOnStart =
            properties.value("playOnStart", true);
        m_movePosition =
            properties.value("movePosition", true);
        m_offset = {
            properties.value("offsetX", 0.0f),
            properties.value("offsetY", 1.0f),
            properties.value("offsetZ", 0.0f)
        };
        m_rotate = properties.value("rotate", false);
        m_rotation = {
            properties.value("rotationX", 0.0f),
            properties.value("rotationY", 180.0f),
            properties.value("rotationZ", 0.0f)
        };
        m_scale = properties.value("scale", false);
        m_scaleMultiplier =
            properties.value("scaleMultiplier", 1.5f);
        m_elapsed = properties.value("elapsed", 0.0f);
        m_forward = properties.value("forward", true);
    }

    [[nodiscard]] std::string SaveProperties() const override
    {
        return nlohmann::json{
            {
                "easing",
                std::string{
                    LamaPonEasing::TypeName(m_easing)
                }
            },
            { "duration", m_duration },
            { "delay", m_delay },
            { "loop", m_loop },
            { "pingPong", m_pingPong },
            { "playOnStart", m_playOnStart },
            { "movePosition", m_movePosition },
            { "offsetX", m_offset.x },
            { "offsetY", m_offset.y },
            { "offsetZ", m_offset.z },
            { "rotate", m_rotate },
            { "rotationX", m_rotation.x },
            { "rotationY", m_rotation.y },
            { "rotationZ", m_rotation.z },
            { "scale", m_scale },
            { "scaleMultiplier", m_scaleMultiplier },
            { "elapsed", m_elapsed },
            { "forward", m_forward }
        }.dump();
    }

private:
    void Apply(const float amount)
    {
        auto& transform = GetTransform();
        if (m_movePosition)
        {
            transform.position = {
                m_startPosition.x + m_offset.x * amount,
                m_startPosition.y + m_offset.y * amount,
                m_startPosition.z + m_offset.z * amount
            };
        }
        if (m_rotate)
        {
            // Transformの回転はラジアンなので度から変換します。
            constexpr float toRadians =
                3.14159265358979323846f / 180.0f;
            transform.SetEulerAngles({
                m_startRotation.x
                    + m_rotation.x * toRadians * amount,
                m_startRotation.y
                    + m_rotation.y * toRadians * amount,
                m_startRotation.z
                    + m_rotation.z * toRadians * amount
            });
        }
        if (m_scale)
        {
            const float factor = 1.0f
                + (m_scaleMultiplier - 1.0f) * amount;
            transform.scale = {
                m_startScale.x * factor,
                m_startScale.y * factor,
                m_startScale.z * factor
            };
        }
    }

    LamaPonEasing::Type m_easing{
        LamaPonEasing::Type::OutCubic
    };
    float m_duration{ 1.0f };
    float m_delay{};
    bool m_loop{};
    bool m_pingPong{ true };
    bool m_playOnStart{ true };
    bool m_movePosition{ true };
    DirectX::XMFLOAT3 m_offset{ 0.0f, 1.0f, 0.0f };
    bool m_rotate{};
    DirectX::XMFLOAT3 m_rotation{ 0.0f, 180.0f, 0.0f };
    bool m_scale{};
    float m_scaleMultiplier{ 1.5f };

    DirectX::XMFLOAT3 m_startPosition{};
    DirectX::XMFLOAT3 m_startRotation{};
    DirectX::XMFLOAT3 m_startScale{ 1.0f, 1.0f, 1.0f };
    float m_elapsed{};
    bool m_forward{ true };
    bool m_playing{ true };
    bool m_finished{};
};

LAMAPON_SCRIPT_WITH_SCHEMA(
    Tween,
    "Easing.Tween",
    "Tween（移動・回転・拡縮）",
    TweenSchema);
