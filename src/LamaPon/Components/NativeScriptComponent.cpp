#include "LamaPon/Components/NativeScriptComponent.h"

#include "LamaPon/Core/Log.h"
#include "LamaPon/Scripting/GameModuleHost.h"
#include "LamaPon/Scripting/Script.h"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <cstdio>
#include <utility>

namespace
{
    // プロパティJSONの最大バイト数
    constexpr std::size_t MaximumPropertiesBytes =
        64 * 1024;
    // 更新抑止を判定する失敗接頭辞
    constexpr std::string_view CallbackFailurePrefix =
        "Native Script callback failed: ";

    // 更新を抑止するコールバック失敗が記録されているか確認する(error: 記録済みの失敗説明)。
    [[nodiscard]] bool HasCallbackFailure(
        const std::string_view error) noexcept
    {
        return error.starts_with(CallbackFailurePrefix);
    }

    // モジュール内で例外を説明文字列へ変換して失敗を返す(lastError: 失敗説明の保存先, scriptType: 登録型名, callbackName: 呼び出す処理名, callback: 引数なしの実体呼び出し)。
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
        // モジュール内で受け取った例外
        catch (const std::exception& exception)
        {
            // 例外の型情報がDLL解放で失われるため、読込中にwhat()をコピーして例外を外へ持ち出さない。
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


    // 解放処理と診断の例外を吸収し、後続の解放を妨げない(lastError: 失敗説明の保存先, scriptType: 登録型名, callbackName: 呼び出す解放処理名, callback: 引数なしの実体呼び出し)。
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
        // 現在のGame Moduleホスト
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
            // 対象型の登録情報
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
            // 実体が返す借用JSON文字列
            const char* serialized =
                m_descriptor->serialize(m_instance);
            if (serialized == nullptr)
            {
                return m_propertiesJson;
            }
            // モジュール解放前に複製したJSON
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
        // 対象型の登録情報
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

    void NativeScriptComponent::ApplyPropertiesJsonLive(std::string propertiesJson)
    {
        ValidateProperties(propertiesJson);
        const auto schema = nlohmann::json::parse(PropertiesSchemaJson(), nullptr, false);
        if (schema.is_discarded() || !schema.is_object()
            || !schema.contains("liveEditable") || !schema["liveEditable"].is_boolean()
            || !schema["liveEditable"].get<bool>())
            throw std::invalid_argument("This script does not opt in to live property editing.");
        auto* script = ScriptInstance();
        if (script == nullptr)
            throw std::runtime_error("The script instance is not available for live editing.");
        try
        {
            script->LoadSerializedProperties(propertiesJson.c_str());
        }
        catch (...)
        {
            script->m_propertiesJson = m_propertiesJson;
            throw;
        }
        m_propertiesJson = std::move(propertiesJson);
    }

    Script* NativeScriptComponent::ScriptInstance() const noexcept
    {
        // 多重継承の基底位置を正しく調整するため、void*からの変換はGame Module側へ委ねる。
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
            // 現在のGame Moduleホスト
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
                // 実体へ初回開始を通知し、失敗を記録する。
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
            // 実体へ通常更新を渡し、失敗を記録する。
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
            // 実体へ後段更新を渡し、失敗を記録する。
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
            // 実体へ固定更新を渡し、失敗を記録する。
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
            // 実体へ衝突開始を渡し、失敗を記録する。
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
            // 実体へ衝突継続を渡し、失敗を記録する。
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
            // 実体へ衝突終了を渡し、失敗を記録する。
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
            // 実体へトリガー進入を渡し、失敗を記録する。
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
            // 実体へトリガー接触を渡し、失敗を記録する。
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
            // 実体へトリガー退出を渡し、失敗を記録する。
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
            // 実体の有効状態を通知し、診断失敗も吸収する。
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
        // 検証のために解析したJSON
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
            // ホスト未登録の原因はCLIなどから参照するLastErrorへ残す。
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
        // モジュール内で受け取った例外
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
            // 実体を破棄し、診断失敗も吸収する。
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
