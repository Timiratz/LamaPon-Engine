#pragma once

#include "LamaPon/Scene/Component.h"

#include <string>
#include <string_view>

namespace LamaPon
{
    class GameModuleHost;
    struct NativeScriptTypeDescriptor;

    class NativeScriptComponent final : public Component
    {
    public:
        explicit NativeScriptComponent(
            std::string scriptType,
            std::string propertiesJson = "{}");
        ~NativeScriptComponent() override;

        [[nodiscard]] std::string_view TypeName() const noexcept override
        {
            return "NativeScript";
        }
        [[nodiscard]] const std::string&
            ScriptType() const noexcept
        {
            return m_scriptType;
        }
        [[nodiscard]] std::string DisplayName() const;
        [[nodiscard]] const std::string&
            PropertiesJson() const noexcept
        {
            return m_propertiesJson;
        }
        [[nodiscard]] std::string SerializedProperties() const;
        [[nodiscard]] std::string_view
            PropertiesSchemaJson() const noexcept;
        void SetPropertiesJson(std::string propertiesJson);
        [[nodiscard]] bool IsResolved() const noexcept;
        // スクリプト実体をScript*として返します。GameObjectの
        // GetScript<T>()経由で自作インターフェースへdynamic_castできます。
        [[nodiscard]] Script* ScriptInstance() const noexcept override;
        [[nodiscard]] const std::string&
            LastError() const noexcept
        {
            return m_lastError;
        }

    protected:
        void OnInitialize(GraphicsDevice& graphics) override;
        void OnUpdate(float deltaTime) override;
        void OnLateUpdate(float deltaTime) override;
        void OnFixedUpdate(float fixedDeltaTime) override;
        void OnCollisionEnter(const CollisionEvent& event) override;
        void OnCollisionStay(const CollisionEvent& event) override;
        void OnCollisionExit(const CollisionEvent& event) override;
        void OnTriggerEnter(const CollisionEvent& event) override;
        void OnTriggerStay(const CollisionEvent& event) override;
        void OnTriggerExit(const CollisionEvent& event) override;
        void OnActiveStateChanged(bool active) override;

    private:
        friend class GameModuleHost;

        static void ValidateProperties(
            std::string_view propertiesJson);
        void EnsureInstance();
        void DestroyInstance() noexcept;
        void BeforeModuleUnload();
        void AfterModuleLoad();
        void NotifyInstanceActive(bool active) noexcept;

        std::string m_scriptType;
        std::string m_propertiesJson;
        std::string m_lastError;
        GraphicsDevice* m_graphics{};
        GameModuleHost* m_host{};
        const NativeScriptTypeDescriptor* m_descriptor{};
        void* m_instance{};
        bool m_started{};
        bool m_instanceActive{};
    };
}
