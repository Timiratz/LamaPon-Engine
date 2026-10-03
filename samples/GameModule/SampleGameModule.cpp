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
        // Scriptを所有GameObjectへ結びます(owner: 対象GameObject, propertiesJson: 保存JSON)。
        FloatingAccent(
            LamaPon::GameObject& owner,
            const char* propertiesJson)
            : m_owner(owner)
        {
            // JSONから復元したScript設定
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

        // 高さと回転を更新します(deltaTime: 経過秒)。
        void Update(const float deltaTime)
        {
            m_elapsed += deltaTime;
            m_owner.GetTransform().position.y =
                m_baseHeight
                + std::sin(m_elapsed * m_frequency * DirectX::XM_2PI)
                    * m_amplitude;
            // 回転はクォータニオンを保つRotateで合成します。
            m_owner.GetTransform().Rotate(
                { 0.0f, 1.0f, 0.0f },
                deltaTime * 0.8f);
        }

        // 固定更新回数を進めます。
        void FixedUpdate(float)
        {
            ++m_fixedTicks;
        }

        // Scriptの現在状態をJSON文字列で返します。
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
        // 更新対象のGameObject
        LamaPon::GameObject& m_owner;
        // 上下移動の振幅
        float m_amplitude{ 0.35f };
        // 上下移動の周波数
        float m_frequency{ 1.5f };
        // 起動後の経過秒
        float m_elapsed{};
        // 初期位置の高さ
        float m_baseHeight{};
        // 固定更新の累計回数
        int m_fixedTicks{};
        // Serializeが返すJSON
        std::string m_serialized;
    };

    // ownerへScriptを生成し、失敗時はnullを返します(owner: 対象GameObject, propertiesJson: 保存JSON)。
    void* CreateFloatingAccent(
        LamaPon::GameObject* owner,
        LamaPon::GraphicsDevice*,
        const char* propertiesJson)
    {
        return owner != nullptr
            ? new FloatingAccent(*owner, propertiesJson)
            : nullptr;
    }

    // Scriptの型に合わせて解放します(instance: 破棄するScript)。
    template<typename T>
    void Destroy(void* instance)
    {
        delete static_cast<T*>(instance);
    }

    // Scriptの型に合わせて更新します(instance: 更新するScript, deltaTime: 経過秒)。
    template<typename T>
    void Update(void* instance, const float deltaTime)
    {
        static_cast<T*>(instance)->Update(deltaTime);
    }

    // FloatingAccentを固定更新します(instance: 更新するScript, deltaTime: 経過秒)。
    void FixedUpdateFloatingAccent(void* instance, const float deltaTime)
    {
        static_cast<FloatingAccent*>(instance)->FixedUpdate(deltaTime);
    }

    // Scriptの型に合わせて状態を保存します(instance: 保存するScript)。
    template<typename T>
    const char* Serialize(void* instance)
    {
        return static_cast<T*>(instance)->Serialize();
    }

    // Inspectorで編集するFloatingAccentの項目
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

    // Inspectorで編集する敵データの項目
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

    // DLLに登録するデータアセット型
    constexpr LamaPon::NativeDataAssetTypeDescriptor DataAssets[]{
        {
            "Sample.EnemyData",
            "敵データ",
            SampleEnemyDataSchema
        }
    };

    // DLLに登録するNative Script型
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

// Game Moduleの型記述をDLL利用側へ公開します。
LAMAPON_GAME_MODULE_EXPORT
{
    // 手書き・生成済みScript型を保持する一覧
    static const std::vector<LamaPon::NativeScriptTypeDescriptor>
        registeredComponents = []
        {
            // DLLに登録する全Script型
            std::vector<LamaPon::NativeScriptTypeDescriptor> result{
                std::begin(Components),
                std::end(Components)
            };
            // ツールが生成したScript一覧
            const auto& generated =
                LamaPon::GameModuleScripts::RegisteredScripts();
            result.insert(
                result.end(),
                generated.begin(),
                generated.end());
            return result;
        }();
    // 手書き・生成済みData Asset型を保持する一覧
    static const std::vector<
        LamaPon::NativeDataAssetTypeDescriptor>
        registeredDataAssets = []
        {
            // DLLに登録する全Data Asset型
            std::vector<LamaPon::NativeDataAssetTypeDescriptor>
                result{
                    std::begin(DataAssets),
                    std::end(DataAssets)
                };
            // ツールが生成したData Asset一覧
            const auto& generated =
                LamaPon::GameModuleDataAssets::
                    RegisteredDataAssets();
            result.insert(
                result.end(),
                generated.begin(),
                generated.end());
            return result;
        }();
    // Game Moduleが公開するAPIと型一覧
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
