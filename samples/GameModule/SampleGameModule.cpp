#include "LamaPon/LamaPon.h"
#include "ScriptRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    class FloatingAccent final
    {
    public:
        FloatingAccent(
            LamaPon::GameObject& owner,
            const char* propertiesJson)
            : m_owner(owner)
        {
            const auto properties = nlohmann::json::parse(
                propertiesJson != nullptr ? propertiesJson : "{}");
            m_amplitude = properties.value("amplitude", 0.35f);
            m_frequency = properties.value("frequency", 1.5f);
            m_elapsed = properties.value("elapsed", 0.0f);
            m_baseHeight = properties.value(
                "baseHeight",
                m_owner.GetTransform().position.y);
            m_fixedTicks = properties.value("fixedTicks", 0);
        }

        void Update(const float deltaTime)
        {
            m_elapsed += deltaTime;
            m_owner.GetTransform().position.y =
                m_baseHeight
                + std::sin(m_elapsed * m_frequency * DirectX::XM_2PI)
                    * m_amplitude;
            // 回転の合成はRotateで行います（回転の正本は
            // クォータニオンなので、オイラー角の成分を足す
            // 書き方はできません）。
            m_owner.GetTransform().Rotate(
                { 0.0f, 1.0f, 0.0f },
                deltaTime * 0.8f);
        }

        void FixedUpdate(float)
        {
            ++m_fixedTicks;
        }

        [[nodiscard]] const char* Serialize()
        {
            m_serialized = nlohmann::json{
                { "amplitude", m_amplitude },
                { "frequency", m_frequency },
                { "elapsed", m_elapsed },
                { "baseHeight", m_baseHeight },
                { "fixedTicks", m_fixedTicks }
            }.dump();
            return m_serialized.c_str();
        }

    private:
        LamaPon::GameObject& m_owner;
        float m_amplitude{ 0.35f };
        float m_frequency{ 1.5f };
        float m_elapsed{};
        float m_baseHeight{};
        int m_fixedTicks{};
        std::string m_serialized;
    };

    void* CreateFloatingAccent(
        LamaPon::GameObject* owner,
        LamaPon::GraphicsDevice*,
        const char* propertiesJson)
    {
        return owner != nullptr
            ? new FloatingAccent(*owner, propertiesJson)
            : nullptr;
    }

    template<typename T>
    void Destroy(void* instance)
    {
        delete static_cast<T*>(instance);
    }

    template<typename T>
    void Update(void* instance, const float deltaTime)
    {
        static_cast<T*>(instance)->Update(deltaTime);
    }

    void FixedUpdateFloatingAccent(void* instance, const float deltaTime)
    {
        static_cast<FloatingAccent*>(instance)->FixedUpdate(deltaTime);
    }

    template<typename T>
    const char* Serialize(void* instance)
    {
        return static_cast<T*>(instance)->Serialize();
    }

    constexpr char FloatingAccentSchema[] = R"({
        "fields": [
            {
                "name": "amplitude",
                "displayName": "振幅",
                "type": "float",
                "default": 0.35,
                "min": 0.0,
                "max": 5.0,
                "step": 0.05
            },
            {
                "name": "frequency",
                "displayName": "周波数",
                "type": "float",
                "default": 1.5,
                "min": 0.0,
                "max": 10.0,
                "step": 0.05
            }
        ]
    })";

    // データアセット（UnityのScriptableObject相当）のサンプルです。
    // アセットウィンドウの右クリック→「新規データアセット」から
    // `*.asset.json`を作り、Inspectorで値を編集できます。
    constexpr char SampleEnemyDataSchema[] = R"({
        "fields": [
            {
                "name": "displayName",
                "displayName": "表示名",
                "type": "string",
                "default": "スライム"
            },
            {
                "name": "hitPoints",
                "displayName": "体力",
                "type": "int",
                "default": 10,
                "min": 1,
                "max": 9999
            },
            {
                "name": "moveSpeed",
                "displayName": "移動速度",
                "type": "float",
                "default": 2.0,
                "min": 0.0,
                "max": 20.0,
                "step": 0.1
            },
            {
                "name": "tintColor",
                "displayName": "色",
                "type": "color4",
                "default": [0.4, 0.9, 0.5, 1.0]
            },
            {
                "name": "prefab",
                "displayName": "出現させるPrefab",
                "type": "asset",
                "assetType": "prefab"
            },
            {
                "name": "dropRates",
                "displayName": "ドロップ率",
                "type": "list",
                "item": {
                    "type": "float",
                    "default": 0.1,
                    "min": 0.0,
                    "max": 1.0,
                    "step": 0.01
                }
            }
        ]
    })";

    constexpr LamaPon::NativeDataAssetTypeDescriptor DataAssets[]{
        {
            "Sample.EnemyData",
            "敵データ",
            SampleEnemyDataSchema
        }
    };

    constexpr LamaPon::NativeScriptTypeDescriptor Components[]{
        {
            "Sample.FloatingAccent",
            "浮遊アクセント",
            &CreateFloatingAccent,
            &Destroy<FloatingAccent>,
            &Update<FloatingAccent>,
            nullptr,
            nullptr,
            nullptr,
            &Serialize<FloatingAccent>,
            &FixedUpdateFloatingAccent,
            FloatingAccentSchema
        }
    };

}

LAMAPON_GAME_MODULE_EXPORT
{
    static const std::vector<LamaPon::NativeScriptTypeDescriptor>
        registeredComponents = []
        {
            std::vector<LamaPon::NativeScriptTypeDescriptor> result{
                std::begin(Components),
                std::end(Components)
            };
            const auto& generated =
                LamaPon::GameModuleScripts::RegisteredScripts();
            result.insert(
                result.end(),
                generated.begin(),
                generated.end());
            return result;
        }();
    static const std::vector<
        LamaPon::NativeDataAssetTypeDescriptor>
        registeredDataAssets = []
        {
            std::vector<LamaPon::NativeDataAssetTypeDescriptor>
                result{
                    std::begin(DataAssets),
                    std::end(DataAssets)
                };
            const auto& generated =
                LamaPon::GameModuleDataAssets::
                    RegisteredDataAssets();
            result.insert(
                result.end(),
                generated.begin(),
                generated.end());
            return result;
        }();
    static const LamaPon::GameModuleDescriptor module{
        LamaPon::GameModuleApiVersion,
        "LamaPon Sample Game",
        registeredComponents.size(),
        registeredComponents.data(),
        registeredDataAssets.size(),
        registeredDataAssets.data()
    };
    return &module;
}
