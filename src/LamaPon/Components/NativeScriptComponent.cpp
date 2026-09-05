#include "LamaPon/Components/NativeScriptComponent.h"

#include "LamaPon/Core/Log.h"
#include "LamaPon/Scripting/GameModuleHost.h"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <cstdio>
#include <utility>

namespace
{
    constexpr std::size_t MaximumPropertiesBytes =
        64 * 1024;
    constexpr std::string_view CallbackFailurePrefix =
        "Native Script callback failed: ";

    [[nodiscard]] bool HasCallbackFailure(
        const std::string_view error) noexcept
    {
        return error.starts_with(CallbackFailurePrefix);
    }

    template <typename Callback>
    [[nodiscard]] bool InvokeScriptCallback(
        std::string& lastError,
        const std::string_view scriptType,
        const std::string_view callbackName,
        Callback&& callback)
    {
        try
        {
            std::forward<Callback>(callback)();
            return true;
        }
        catch (const std::exception& exception)
        {
            // Game Moduleが読み込まれている間にwhat()を文字列へ
            // コピーします。DLL解放後まで例外を外へ逃がすと、例外の
            // vtableも消えてクラッシュするため、ここが安全境界です。
            lastError = std::string{ CallbackFailurePrefix }
                + std::string{ scriptType }
                + "." + std::string{ callbackName }
                + " - " + exception.what();
        }
        catch (...)
        {
            lastError = std::string{ CallbackFailurePrefix }
                + std::string{ scriptType }
                + "." + std::string{ callbackName }
                + " - unknown exception";
        }
        LamaPon::Logger::Instance().Error(lastError);
        return false;
    }

    // 破棄・無効化はnoexcept境界です。通常の診断を残しつつ、
    // 診断文字列やログの確保に失敗しても後続の解放を続けます。
    template <typename Callback>
    void InvokeCleanupCallback(
        std::string& lastError,
        const std::string_view scriptType,
        const std::string_view callbackName,
        Callback&& callback) noexcept
    {
        try
        {
            static_cast<void>(InvokeScriptCallback(
                lastError, scriptType, callbackName,
                std::forward<Callback>(callback)));
        }
        catch (...)
        {
            std::fputs("Native Script cleanup diagnostic failed.\n", stderr);
        }
    }
}

namespace LamaPon
{
    NativeScriptComponent::NativeScriptComponent(
        std::string scriptType,
        std::string propertiesJson)
        : m_scriptType(std::move(scriptType))
        , m_propertiesJson(std::move(propertiesJson))
    {
        if (m_scriptType.empty()
            || m_scriptType.size() > 128)
        {
            throw std::invalid_argument(
                "Native Script type must contain 1 to 128 bytes.");
        }
        ValidateProperties(m_propertiesJson);
        if (auto* host = GameModuleHost::Current())
        {
            host->RegisterInstance(*this);
        }
    }

    NativeScriptComponent::~NativeScriptComponent()
    {
        DestroyInstance();
        if (m_host != nullptr)
        {
            m_host->UnregisterInstance(*this);
        }
    }

    std::string NativeScriptComponent::DisplayName() const
    {
        if (m_host != nullptr)
        {
            if (const auto* descriptor =
                    m_host->FindComponent(m_scriptType);
                descriptor != nullptr
                && descriptor->displayName != nullptr)
            {
                return descriptor->displayName;
            }
        }
        return m_scriptType;
    }

    std::string
        NativeScriptComponent::SerializedProperties() const
    {
        if (m_instance == nullptr
            || m_descriptor == nullptr
            || m_descriptor->serialize == nullptr)
        {
            return m_propertiesJson;
        }

        try
        {
            const char* serialized =
                m_descriptor->serialize(m_instance);
            if (serialized == nullptr)
            {
                return m_propertiesJson;
            }
            const std::string result(serialized);
            ValidateProperties(result);
            return result;
        }
        catch (...)
        {
            return m_propertiesJson;
        }
    }

    std::string_view
        NativeScriptComponent::PropertiesSchemaJson() const noexcept
    {
        if (m_host == nullptr)
        {
            return {};
        }
        const auto* descriptor =
            m_host->FindComponent(m_scriptType);
        return descriptor != nullptr
                && descriptor->propertiesSchemaJson != nullptr
            ? std::string_view{ descriptor->propertiesSchemaJson }
            : std::string_view{};
    }

    void NativeScriptComponent::SetPropertiesJson(
        std::string propertiesJson)
    {
        ValidateProperties(propertiesJson);
        DestroyInstance();
        m_propertiesJson = std::move(propertiesJson);
        EnsureInstance();
    }

    Script* NativeScriptComponent::ScriptInstance() const noexcept
    {
        // 実体はvoid*で持っているため、上位変換はGame Module側の
        // asScriptに任せます。ここでstatic_cast<Script*>すると、
        // Scriptを先頭以外に継承した型（インターフェースとの多重
        // 継承）でポインタ調整が入らず静かに壊れます。
        if (m_instance == nullptr
            || m_descriptor == nullptr
            || m_descriptor->asScript == nullptr)
        {
            return nullptr;
        }
        return m_descriptor->asScript(m_instance);
    }

    bool NativeScriptComponent::IsResolved() const noexcept
    {
        return m_host != nullptr
            && m_host->FindComponent(m_scriptType) != nullptr;
    }

    void NativeScriptComponent::OnInitialize(
        GraphicsDevice& graphics)
    {
        m_graphics = &graphics;
        if (m_host == nullptr)
        {
            if (auto* host = GameModuleHost::Current())
            {
                host->RegisterInstance(*this);
            }
        }
        EnsureInstance();
    }

    void NativeScriptComponent::OnUpdate(
        const float deltaTime)
    {
        EnsureInstance();
        if (m_instance == nullptr
            || m_descriptor == nullptr
            || HasCallbackFailure(m_lastError))
        {
            return;
        }
        if (!m_started)
        {
            if (m_descriptor->start != nullptr)
            {
                if (!InvokeScriptCallback(
                    m_lastError,
                    m_scriptType,
                    "Start",
                    [&]
                    {
                        m_descriptor->start(m_instance);
                    }))
                {
                    return;
                }
            }
            m_started = true;
        }
        if (m_descriptor->update != nullptr)
        {
            static_cast<void>(InvokeScriptCallback(
                m_lastError,
                m_scriptType,
                "Update",
                [&]
                {
                    m_descriptor->update(
                        m_instance,
                        deltaTime);
                }));
        }
    }

    void NativeScriptComponent::OnLateUpdate(
        const float deltaTime)
    {
        if (m_instance != nullptr
            && m_descriptor != nullptr
            && !HasCallbackFailure(m_lastError)
            && m_descriptor->lateUpdate != nullptr)
        {
            static_cast<void>(InvokeScriptCallback(
                m_lastError,
                m_scriptType,
                "LateUpdate",
                [&]
                {
                    m_descriptor->lateUpdate(
                        m_instance,
                        deltaTime);
                }));
        }
    }

    void NativeScriptComponent::OnFixedUpdate(
        const float fixedDeltaTime)
    {
        EnsureInstance();
        if (m_instance != nullptr
            && m_descriptor != nullptr
            && !HasCallbackFailure(m_lastError)
            && m_descriptor->fixedUpdate != nullptr)
        {
            static_cast<void>(InvokeScriptCallback(
                m_lastError,
                m_scriptType,
                "FixedUpdate",
                [&]
                {
                    m_descriptor->fixedUpdate(
                        m_instance,
                        fixedDeltaTime);
                }));
        }
    }

    void NativeScriptComponent::OnCollisionEnter(
        const CollisionEvent& event)
    {
        if (m_instance != nullptr
            && m_descriptor != nullptr
            && !HasCallbackFailure(m_lastError)
            && m_descriptor->collisionEnter != nullptr)
        {
            static_cast<void>(InvokeScriptCallback(
                m_lastError, m_scriptType, "OnCollisionEnter",
                [&] { m_descriptor->collisionEnter(m_instance, &event); }));
        }
    }

    void NativeScriptComponent::OnCollisionStay(
        const CollisionEvent& event)
    {
        if (m_instance != nullptr
            && m_descriptor != nullptr
            && !HasCallbackFailure(m_lastError)
            && m_descriptor->collisionStay != nullptr)
        {
            static_cast<void>(InvokeScriptCallback(
                m_lastError, m_scriptType, "OnCollisionStay",
                [&] { m_descriptor->collisionStay(m_instance, &event); }));
        }
    }

    void NativeScriptComponent::OnCollisionExit(
        const CollisionEvent& event)
    {
        if (m_instance != nullptr
            && m_descriptor != nullptr
            && !HasCallbackFailure(m_lastError)
            && m_descriptor->collisionExit != nullptr)
        {
            static_cast<void>(InvokeScriptCallback(
                m_lastError, m_scriptType, "OnCollisionExit",
                [&] { m_descriptor->collisionExit(m_instance, &event); }));
        }
    }

    void NativeScriptComponent::OnTriggerEnter(
        const CollisionEvent& event)
    {
        if (m_instance != nullptr
            && m_descriptor != nullptr
            && !HasCallbackFailure(m_lastError)
            && m_descriptor->triggerEnter != nullptr)
        {
            static_cast<void>(InvokeScriptCallback(
                m_lastError, m_scriptType, "OnTriggerEnter",
                [&] { m_descriptor->triggerEnter(m_instance, &event); }));
        }
    }

    void NativeScriptComponent::OnTriggerStay(
        const CollisionEvent& event)
    {
        if (m_instance != nullptr
            && m_descriptor != nullptr
            && !HasCallbackFailure(m_lastError)
            && m_descriptor->triggerStay != nullptr)
        {
            static_cast<void>(InvokeScriptCallback(
                m_lastError, m_scriptType, "OnTriggerStay",
                [&] { m_descriptor->triggerStay(m_instance, &event); }));
        }
    }

    void NativeScriptComponent::OnTriggerExit(
        const CollisionEvent& event)
    {
        if (m_instance != nullptr
            && m_descriptor != nullptr
            && !HasCallbackFailure(m_lastError)
            && m_descriptor->triggerExit != nullptr)
        {
            static_cast<void>(InvokeScriptCallback(
                m_lastError, m_scriptType, "OnTriggerExit",
                [&] { m_descriptor->triggerExit(m_instance, &event); }));
        }
    }

    void NativeScriptComponent::OnActiveStateChanged(
        const bool active)
    {
        NotifyInstanceActive(active);
    }

    void NativeScriptComponent::NotifyInstanceActive(
        const bool active) noexcept
    {
        if (m_instance == nullptr
            || m_descriptor == nullptr
            || m_instanceActive == active)
        {
            return;
        }
        m_instanceActive = active;
        if (m_descriptor->setActive != nullptr)
        {
            InvokeCleanupCallback(
                m_lastError, m_scriptType,
                active ? "OnEnable" : "OnDisable",
                [&] { m_descriptor->setActive(m_instance, active); });
        }
    }

    void NativeScriptComponent::ValidateProperties(
        const std::string_view propertiesJson)
    {
        if (propertiesJson.empty()
            || propertiesJson.size()
                > MaximumPropertiesBytes)
        {
            throw std::invalid_argument(
                "Native Script properties must contain a JSON object up to 64 KiB.");
        }
        const auto value = nlohmann::json::parse(
            propertiesJson.begin(),
            propertiesJson.end());
        if (!value.is_object())
        {
            throw std::invalid_argument(
                "Native Script properties must be a JSON object.");
        }
    }

    void NativeScriptComponent::EnsureInstance()
    {
        if (m_instance != nullptr
            || m_graphics == nullptr)
        {
            return;
        }
        if (m_host == nullptr)
        {
            // Game Module Hostが無い環境（DLLを読まないツールなど）で
            // 黙って何もしないと、「画面が空なのに理由がどこにも
            // 出ない」ことになります。理由を残して追えるようにします
            // （CLIのproblemsはここのLastErrorを拾います）。
            if (m_lastError.empty())
            {
                m_lastError =
                    "Game Moduleが読み込まれていないため、"
                    "C++ Scriptを動かせません: "
                    + m_scriptType;
            }
            return;
        }

        m_descriptor =
            m_host->FindComponent(m_scriptType);
        if (m_descriptor == nullptr)
        {
            m_lastError =
                "Game Moduleに型が登録されていません: "
                + m_scriptType;
            return;
        }

        try
        {
            m_instance = m_descriptor->create(
                &Owner(),
                m_graphics,
                m_propertiesJson.c_str());
            if (m_instance == nullptr)
            {
                m_lastError =
                    "Native Scriptの生成に失敗しました: "
                    + m_scriptType;
            }
            else
            {
                m_lastError.clear();
                m_started = false;
                m_instanceActive = false;
                if (IsActiveAndEnabled())
                {
                    NotifyInstanceActive(true);
                }
            }
        }
        catch (const std::exception& exception)
        {
            m_instance = nullptr;
            m_lastError = exception.what();
        }
        catch (...)
        {
            m_instance = nullptr;
            m_lastError =
                "Native Scriptの生成中に不明な例外が発生しました。";
        }
    }

    void NativeScriptComponent::DestroyInstance() noexcept
    {
        NotifyInstanceActive(false);
        if (m_instance != nullptr
            && m_descriptor != nullptr
            && m_descriptor->destroy != nullptr)
        {
            InvokeCleanupCallback(
                m_lastError, m_scriptType, "OnDestroy",
                [&] { m_descriptor->destroy(m_instance); });
        }
        m_instance = nullptr;
        m_descriptor = nullptr;
        m_started = false;
        m_instanceActive = false;
    }

    void NativeScriptComponent::BeforeModuleUnload()
    {
        m_propertiesJson = SerializedProperties();
        DestroyInstance();
    }

    void NativeScriptComponent::AfterModuleLoad()
    {
        EnsureInstance();
    }
}
