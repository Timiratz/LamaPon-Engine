#include "LamaPon/LamaPon.h"
#include "LamaPon/Web/WebRenderer3D.h"
#include "../EncodedImageFixture.h"
#include <algorithm>

#include <stdexcept>
#include <emscripten.h>

void LamaPonWebTextureProbe(LamaPon::Web::Renderer3D& renderer)
{
    auto bytes = LamaPonTest::EncodedImage();
    const auto texture = renderer.CreateTextureEncoded(bytes);
    if (!texture || renderer.CreateTextureEncoded({}))
        throw std::runtime_error("Web encoded image creation failed");
    std::fill(bytes.begin(), bytes.end(), 0);
    EM_ASM({
        const id = $0;
        window.__encodedImageProbe = 'pending';
        window.__encodedImageTextureId = id;
        let attempts = 0;
        const check = () => {
            const slot = globalThis.__lamaponTextures[id];
            if (!slot || !slot.ready) {
                if (++attempts < 200) { setTimeout(check, 5); return; }
                window.__encodedImageProbe = 'failed'; return;
            }
            let pixel;
            if (slot.texture) {
                const gl = globalThis.__lamaponWebGl;
                const previous = gl.getParameter(gl.FRAMEBUFFER_BINDING);
                const framebuffer = gl.createFramebuffer();
                gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
                gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, slot.texture, 0);
                pixel = new Uint8Array(4);
                gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, pixel);
                gl.bindFramebuffer(gl.FRAMEBUFFER, previous);
                gl.deleteFramebuffer(framebuffer);
            } else {
                pixel = slot.softwareLevels[0].pixels;
            }
            window.__encodedImageProbe = pixel[0] === 220 && pixel[1] === 30
                && pixel[2] === 80 && pixel[3] === 255 ? 'passed' : 'failed';
        };
        setTimeout(check, 0);
    }, texture);
}

namespace
{
    struct PortableLifecycleCounts final
    {
        int awake{};
        int start{};
        int enable{};
        int disable{};
        int destroy{};
        int eventAfterDestroy{};
    };
    PortableLifecycleCounts portableOwnerLifecycle;

    struct PortablePrefabLifecycleCounts final
    {
        int awake{};
        int start{};
        int enable{};
    };
    PortablePrefabLifecycleCounts portablePrefabLifecycle;

    class PortableHierarchyMarker final : public LamaPon::Script
    {
    public:
        bool ValidateHierarchyQueries()
        {
            return GetComponentInParent<LamaPon::UICanvasComponent>()
                    == Owner().Parent()->GetComponent<LamaPon::UICanvasComponent>()
                && GetComponentsInParent<LamaPon::UICanvasComponent>().size() == 1
                && GetComponentInChildren<LamaPon::UIButtonComponent>()
                    == Owner().GetComponent<LamaPon::UIButtonComponent>()
                && GetComponentsInChildren<
                    LamaPon::UIRectTransformComponent>().size() == 1
                && GetScriptInChildren<PortableHierarchyMarker>() == this;
        }
    };

    class PortableOwnerHierarchyMarker final : public LamaPon::Script
    {
    public:
        void Awake() override { ++portableOwnerLifecycle.awake; }
        void Start() override
        {
            ++portableOwnerLifecycle.start;
            On("PortableSmoke.DestroyedScript", []
            {
                ++portableOwnerLifecycle.eventAfterDestroy;
            });
        }
        void OnEnable() override { ++portableOwnerLifecycle.enable; }
        void OnDisable() override { ++portableOwnerLifecycle.disable; }
        void OnDestroy() override { ++portableOwnerLifecycle.destroy; }
    };

    class PortablePrefabProbe final : public LamaPon::Script
    {
    public:
        void Awake() override { ++portablePrefabLifecycle.awake; }
        void Start() override { ++portablePrefabLifecycle.start; }
        void OnEnable() override { ++portablePrefabLifecycle.enable; }
    };

    class PortableContactProbe final : public LamaPon::Script
    {
    public:
        void OnCollisionEnter(const LamaPon::CollisionEvent& event) override
        {
            ++collisionEnter;
            lastOther = event.other.Name();
            lastTrigger = event.isTrigger;
        }
        void OnCollisionStay(const LamaPon::CollisionEvent& event) override
        {
            ++collisionStay;
            lastOther = event.other.Name();
            lastTrigger = event.isTrigger;
        }
        void OnCollisionExit(const LamaPon::CollisionEvent& event) override
        {
            ++collisionExit;
            lastOther = event.other.Name();
            lastTrigger = event.isTrigger;
        }
        void OnTriggerEnter(const LamaPon::CollisionEvent& event) override
        {
            ++triggerEnter;
            lastOther = event.other.Name();
            lastTrigger = event.isTrigger;
        }
        void OnTriggerStay(const LamaPon::CollisionEvent& event) override
        {
            ++triggerStay;
            lastOther = event.other.Name();
            lastTrigger = event.isTrigger;
        }
        void OnTriggerExit(const LamaPon::CollisionEvent& event) override
        {
            ++triggerExit;
            lastOther = event.other.Name();
            lastTrigger = event.isTrigger;
        }

        int collisionEnter{};
        int collisionStay{};
        int collisionExit{};
        int triggerEnter{};
        int triggerStay{};
        int triggerExit{};
        std::string lastOther;
        bool lastTrigger{};
    };

    class PortableStartupProbe final : public LamaPon::Script
    {
    public:
        void Awake() override { ++m_awakeCalls; }
        void OnEnable() override { ++m_enableCalls; }
        void OnDisable() override { ++m_disableCalls; }
        void OnDestroy() override
        {
            if (!m_transitionPending) return;
            const auto* serializedButtonObject =
                Find("Serialized target button");
            const auto* serializedButton = serializedButtonObject
                ? serializedButtonObject->GetComponent<LamaPon::UIButtonComponent>()
                : nullptr;
            const bool passed =
                GetScene().Scenes().CurrentScenePath().generic_u8string()
                    == u8"/assets/scenes/UIButtonTarget.scene.json"
                && Find("UIButton target marker") != nullptr
                && serializedButton != nullptr
                && serializedButton->TargetScene().generic_u8string()
                    == u8"scenes/UIButtonFinal.scene.json"
                && m_buttonEventCount == 4
                && m_lastButtonEventSender == "Test Button";
            EM_ASM({
                document.body.dataset.testStatus = $0 ? 'passed' : 'failed';
                document.body.dataset.sceneTransition = $0 ? 'passed' : 'failed';
            }, passed);
        }
        // 初期化中の例外を捕捉してPortableの例外有効化を検査する。
        void Start() override
        {
            if (m_awakeCalls != 1 || m_enableCalls != 1)
                throw std::runtime_error("Portable Awake and OnEnable must run before Start.");
            m_buttonEventSubscription = On(
                "PortableSmoke.ButtonClicked",
                [this](const LamaPon::EventArgs& eventArgs)
                {
                    ++m_buttonEventCount;
                    m_lastButtonEventSender = eventArgs.sender
                        ? eventArgs.sender->Name() : std::string{};
                });
            if (m_buttonEventSubscription == 0)
                throw std::runtime_error("Portable Script::On did not subscribe to a named event.");
            const auto scriptEventSubscription = On(
                "PortableSmoke.ScriptEvent",
                [this](const LamaPon::EventArgs& eventArgs)
                {
                    ++m_scriptEventCount;
                    m_scriptEventSender = eventArgs.sender;
                });
            Emit("PortableSmoke.ScriptEvent");
            if (scriptEventSubscription == 0 || m_scriptEventCount != 1
                || m_scriptEventSender != &Owner())
                throw std::runtime_error("Portable Script::Emit did not synchronously publish its owner as the sender.");
            Off(scriptEventSubscription);
            Emit("PortableSmoke.ScriptEvent");
            if (m_scriptEventCount != 1)
                throw std::runtime_error("Portable Script::Off did not remove its event subscription.");
            const auto payloadSubscription = On(
                "PortableSmoke.PayloadEvent",
                [this](const LamaPon::EventArgs& eventArgs)
                {
                    ++m_payloadEventCount;
                    m_lastPayloadEvent = eventArgs;
                });
            LamaPon::EventArgs payload;
            payload.number = 1.25f;
            payload.text = "portable payload";
            Emit("PortableSmoke.PayloadEvent", payload);
            if (payloadSubscription == 0 || m_payloadEventCount != 1
                || m_lastPayloadEvent.sender != &Owner()
                || m_lastPayloadEvent.number != 1.25f
                || m_lastPayloadEvent.text != "portable payload")
                throw std::runtime_error("Portable Script::Emit did not preserve its payload or default the sender to its owner.");
            auto* explicitEventSender = Find("Test Button");
            if (explicitEventSender == nullptr)
                throw std::runtime_error("Portable payload sender probe could not find the test button.");
            payload.sender = explicitEventSender;
            payload.number = 2.5f;
            payload.text = "explicit sender";
            Emit("PortableSmoke.PayloadEvent", payload);
            if (m_payloadEventCount != 2
                || m_lastPayloadEvent.sender != explicitEventSender
                || m_lastPayloadEvent.number != 2.5f
                || m_lastPayloadEvent.text != "explicit sender")
                throw std::runtime_error("Portable Script::Emit replaced an explicit sender or changed the event payload.");
            Off(payloadSubscription);
            const auto noArgumentSubscription = On(
                "PortableSmoke.NoArgumentEvent",
                [this] { ++m_noArgumentEventCount; });
            Emit("PortableSmoke.NoArgumentEvent");
            if (noArgumentSubscription == 0 || m_noArgumentEventCount != 1)
                throw std::runtime_error("Portable Script::On did not support a no-argument event handler.");
            Off(noArgumentSubscription);
            auto& prefabParent = GetScene().CreateGameObject(
                "Portable runtime prefab parent");
            auto& scriptPrefab = Instantiate(
                "prefabs/PortableProbe.prefab.json", &prefabParent);
            auto& scenePrefab = GetScene().InstantiatePrefab(
                "assets/prefabs/PortableProbe.prefab.json", &prefabParent);
            if (scriptPrefab.Id() == 5001 || scenePrefab.Id() == 5001
                || scriptPrefab.Id() == scenePrefab.Id()
                || scriptPrefab.Parent() != &prefabParent
                || scenePrefab.Parent() != &prefabParent
                || scriptPrefab.Children().size() != 1
                || scenePrefab.Children().size() != 1
                || scriptPrefab.Children().front()->Parent() != &scriptPrefab
                || scenePrefab.Children().front()->Parent() != &scenePrefab
                || scriptPrefab.Tag() != "runtime-prefab"
                || scriptPrefab.GetTransform().position.x != 1.0f
                || prefabParent.Children().size() != 2
                || portablePrefabLifecycle.awake != 2
                || portablePrefabLifecycle.enable != 2
                || portablePrefabLifecycle.start != 0)
                throw std::runtime_error("Portable runtime Prefab did not preserve its hierarchy, values, unique IDs, or immediate lifecycle callbacks.");
            bool malformedPrefabRejected{};
            try
            {
                static_cast<void>(GetScene().InstantiatePrefab(
                    "prefabs/MalformedHierarchy.prefab.json", &prefabParent));
            }
            catch (const std::runtime_error&)
            {
                malformedPrefabRejected = true;
            }
            if (!malformedPrefabRejected || prefabParent.Children().size() != 2
                || GetScene().FindGameObjectByName("Malformed prefab root")
                    != nullptr)
                throw std::runtime_error("Portable runtime Prefab accepted a cyclic hierarchy or left partial objects behind.");
            auto* existingButton = GetScene().FindGameObjectByName("Test Button");
            auto* canvasObject = GetScene().FindGameObjectByName("UI Canvas");
            auto* canvas = canvasObject
                ? canvasObject->GetComponent<LamaPon::UICanvasComponent>()
                : nullptr;
            const auto* legacyCulling = Owner().GetComponent<LamaPon::RenderCullingComponent>();
            auto* hierarchyMarker = existingButton
                ? &existingButton->AddComponent<LamaPon::NativeScriptComponent>(
                    "Test.PortableHierarchyMarker")
                : nullptr;
            auto* markerInstance = hierarchyMarker
                ? dynamic_cast<PortableHierarchyMarker*>(
                    hierarchyMarker->Instance())
                : nullptr;
            auto& scriptApiChild = GetScene().CreateGameObject(
                "Portable script API child");
            scriptApiChild.SetParent(&Owner());
            auto& scriptApiCulling =
                scriptApiChild.AddComponent<LamaPon::RenderCullingComponent>();
            auto* ownerChildComponent =
                &scriptApiChild.AddComponent<LamaPon::NativeScriptComponent>(
                    "Test.PortableOwnerHierarchyMarker");
            auto* ownerChildMarker = dynamic_cast<
                PortableOwnerHierarchyMarker*>(ownerChildComponent->Instance());
            auto& apiGrandchild = GetScene().CreateGameObject(
                "Portable API leaf");
            apiGrandchild.SetParent(&scriptApiChild);
            apiGrandchild.SetName("Renamed Portable API leaf");
            const bool reorderedBefore = scriptApiChild.ReorderComponent(
                *ownerChildComponent, scriptApiCulling, false);
            const bool componentMovedBeforeReference =
                scriptApiChild.Components().size() == 2
                && scriptApiChild.Components()[0].get() == ownerChildComponent
                && scriptApiChild.Components()[1].get() == &scriptApiCulling;
            const bool reorderedAfter = scriptApiChild.ReorderComponent(
                *ownerChildComponent, scriptApiCulling, true);
            const bool componentMovedAfterReference =
                scriptApiChild.Components().size() == 2
                && scriptApiChild.Components()[0].get() == &scriptApiCulling
                && scriptApiChild.Components()[1].get() == ownerChildComponent;
            const bool componentAccessorsWork = scriptApiCulling.IsEnabled()
                && scriptApiCulling.IsActiveAndEnabled()
                && &scriptApiCulling.Owner() == &scriptApiChild
                && &scriptApiCulling.GetTransform()
                    == &scriptApiChild.GetTransform()
                && scriptApiCulling.ScriptInstance() == nullptr
                && ownerChildComponent->ScriptInstance() == ownerChildMarker;
            const auto ownerWorld = Owner().WorldMatrix();
            LamaPon::TextRendererComponent textRenderer;
            const bool textDefaultsMatch = textRenderer.Text() == "日本語テキスト"
                && textRenderer.FontFamily() == "Yu Gothic UI"
                && textRenderer.FontSize() == 32.0f
                && textRenderer.LayoutSize().x == 0.0f
                && textRenderer.LayoutSize().y == 0.0f
                && !textRenderer.WordWrap()
                && textRenderer.HorizontalAlignment()
                    == LamaPon::TextHorizontalAlignment::Left
                && textRenderer.VerticalAlignment()
                    == LamaPon::TextVerticalAlignment::Top;
            textRenderer.SetText("Portable text API");
            textRenderer.SetFontFamily("Portable Sans");
            textRenderer.SetFontSize(0.5f);
            textRenderer.SetColor({0.2f, 0.3f, 0.4f, 0.5f});
            textRenderer.SetLayoutSize({8192.0f, -4.0f});
            textRenderer.SetWordWrap(true);
            textRenderer.SetHorizontalAlignment(
                LamaPon::TextHorizontalAlignment::Center);
            textRenderer.SetVerticalAlignment(
                LamaPon::TextVerticalAlignment::Bottom);
            textRenderer.SetSortOrder(17);
            LamaPon::CameraComponent cameraApi;
            const bool cameraDefaultsMatch =
                cameraApi.VerticalFieldOfView() == DirectX::XM_PI / 4.0f
                && cameraApi.NearPlane() == 0.1f
                && cameraApi.FarPlane() == 1000.0f;
            cameraApi.SetVerticalFieldOfView(0.8f);
            cameraApi.SetNearPlane(0.25f);
            cameraApi.SetFarPlane(250.0f);
            const bool cameraSettingsMatch =
                cameraApi.VerticalFieldOfView() == 0.8f
                && cameraApi.NearPlane() == 0.25f
                && cameraApi.FarPlane() == 250.0f;
            LamaPon::UIRectTransformComponent rectApi;
            const bool rectDefaultsMatch = rectApi.AnchorMin().x == 0.5f
                && rectApi.AnchorMax().y == 0.5f && rectApi.Pivot().x == 0.5f
                && rectApi.AnchoredPosition().x == 0.0f
                && rectApi.SizeDelta().x == 220.0f
                && rectApi.TypeName() == "UIRectTransform";
            rectApi.SetAnchorMin({-1.0f, 0.4f});
            rectApi.SetAnchorMax({0.75f, 2.0f});
            rectApi.SetPivot({1.5f, -0.5f});
            rectApi.SetAnchoredPosition({12.0f, -8.0f});
            rectApi.SetSizeDelta({100.0f, 50.0f});
            const bool rectSettingsMatch = rectApi.AnchorMin().x == 0.0f
                && rectApi.AnchorMin().y == 0.4f
                && rectApi.AnchorMax().x == 0.75f
                && rectApi.AnchorMax().y == 1.0f
                && rectApi.Pivot().x == 1.0f && rectApi.Pivot().y == 0.0f
                && rectApi.AnchoredPosition().x == 12.0f
                && rectApi.AnchoredPosition().y == -8.0f
                && rectApi.SizeDelta().x == 100.0f
                && rectApi.SizeDelta().y == 50.0f;
            LamaPon::BoxCollider3DComponent boxApi(
                {2.0f, 3.0f, 4.0f}, {0.25f, -0.5f, 1.0f}, true, 33u, 0x5u);
            const bool boxDefaultsMatch = boxApi.Size().x == 2.0f
                && boxApi.Size().y == 3.0f && boxApi.Size().z == 4.0f
                && boxApi.Offset().x == 0.25f
                && boxApi.Offset().y == -0.5f
                && boxApi.Offset().z == 1.0f
                && boxApi.IsTrigger() && boxApi.Layer() == 1u
                && boxApi.CollisionMask() == 0x5u
                && boxApi.TypeName() == "BoxCollider3D";
            boxApi.SetSize({5.0f, 6.0f, 7.0f});
            boxApi.SetOffset({-1.0f, 2.0f, -3.0f});
            boxApi.SetTrigger(false);
            boxApi.SetLayer(34u);
            boxApi.SetCollisionMask(0xau);
            const bool boxSettingsMatch = boxApi.Size().z == 7.0f
                && boxApi.Offset().y == 2.0f && !boxApi.IsTrigger()
                && boxApi.Layer() == 2u && boxApi.CollisionMask() == 0xau;
            LamaPon::RigidbodyComponent bodyApi;
            const bool bodyDefaultsMatch = !bodyApi.IsKinematic()
                && bodyApi.UsesGravity() && bodyApi.Velocity().x == 0.0f
                && bodyApi.TypeName() == "Rigidbody";
            bodyApi.SetKinematic(true);
            bodyApi.SetUseGravity(false);
            bodyApi.SetVelocity({1.0f, 2.0f, 3.0f});
            const bool bodySettingsMatch = bodyApi.IsKinematic()
                && !bodyApi.UsesGravity() && bodyApi.Velocity().x == 1.0f
                && bodyApi.Velocity().y == 2.0f
                && bodyApi.Velocity().z == 3.0f;
            LamaPon::AudioSourceComponent audioApi;
            const bool audioDefaultsMatch = audioApi.AudioPath().empty()
                && audioApi.Volume() == 1.0f && audioApi.Pitch() == 0.0f
                && audioApi.Pan() == 0.0f && !audioApi.Loop()
                && !audioApi.PlayOnStart() && !audioApi.IsSpatial()
                && audioApi.MinimumDistance() == 1.0f
                && audioApi.MaximumDistance() == 20.0f
                && audioApi.Bus() == LamaPon::AudioBus::Effects
                && audioApi.TypeName() == "AudioSource";
            const LamaPon::AudioSourceComponent configuredAudio(
                {}, 2.0f, -2.0f, 2.0f, false, true, true,
                -5.0f, -5.0f);
            const bool audioConstructorClamps =
                configuredAudio.Volume() == 1.0f
                && configuredAudio.Pitch() == -1.0f
                && configuredAudio.Pan() == 1.0f
                && configuredAudio.MinimumDistance() == 0.01f
                && configuredAudio.MaximumDistance() == 0.02f;
            audioApi.SetAudioPath({});
            audioApi.SetVolume(2.0f);
            audioApi.SetPitch(-2.0f);
            audioApi.SetPan(2.0f);
            audioApi.SetLoop(true);
            audioApi.SetPlayOnStart(true);
            audioApi.SetSpatial(true);
            audioApi.SetMinimumDistance(3.0f);
            audioApi.SetMaximumDistance(1.0f);
            audioApi.SetBus(LamaPon::AudioBus::Music);
            const bool audioSettingsMatch =
                audioApi.AudioPath().empty()
                && audioApi.Volume() == 1.0f && audioApi.Pitch() == -1.0f
                && audioApi.Pan() == 1.0f && audioApi.Loop()
                && audioApi.PlayOnStart() && audioApi.IsSpatial()
                && audioApi.MinimumDistance() == 3.0f
                && audioApi.MaximumDistance() >= 3.01f
                && audioApi.Bus() == LamaPon::AudioBus::Music;
            const bool textSettingsMatch = textRenderer.Text()
                    == "Portable text API"
                && textRenderer.FontFamily() == "Portable Sans"
                && textRenderer.FontSize() == 1.0f
                && textRenderer.Color().x == 0.2f
                && textRenderer.Color().w == 0.5f
                && textRenderer.LayoutSize().x == 4096.0f
                && textRenderer.LayoutSize().y == 0.0f
                && textRenderer.WordWrap()
                && textRenderer.HorizontalAlignment()
                    == LamaPon::TextHorizontalAlignment::Center
                && textRenderer.VerticalAlignment()
                    == LamaPon::TextVerticalAlignment::Bottom
                && textRenderer.SortOrder() == 17;
            const auto rectTransforms = canvasObject
                ? canvasObject->GetComponentsInChildren<
                    LamaPon::UIRectTransformComponent>()
                : std::vector<LamaPon::UIRectTransformComponent*>{};
            const auto* ownerCamera =
                Owner().GetComponent<LamaPon::CameraComponent>();
            const bool loadedCameraDefaultsMatch = ownerCamera != nullptr
                && ownerCamera->VerticalFieldOfView() == DirectX::XM_PI / 4.0f
                && ownerCamera->NearPlane() == 0.1f
                && ownerCamera->FarPlane() == 1000.0f;
            if (!Owner().CompareTag("portable-probe")
                || FindWithTag("portable-ui") != existingButton
                || FindObjectsWithTag("portable-ui").size() != 2
                || GetComponent<LamaPon::CameraComponent>()
                    != Owner().GetComponent<LamaPon::CameraComponent>()
                || GetComponentInParent<LamaPon::CameraComponent>()
                    != GetComponent<LamaPon::CameraComponent>()
                || &GetTransform() != &Owner().GetTransform()
                || canvas == nullptr || existingButton == nullptr
                || existingButton->GetComponentInParent<LamaPon::UICanvasComponent>() != canvas
                || canvasObject->GetComponentInChildren<LamaPon::UIButtonComponent>()
                    != existingButton->GetComponent<LamaPon::UIButtonComponent>()
                || rectTransforms.size() != 2
                || canvasObject->GetScriptInChildren<PortableHierarchyMarker>()
                    != markerInstance
                || ownerChildMarker == nullptr
                || Owner().FindChild("Portable script API child/"
                    "Renamed Portable API leaf") != &apiGrandchild
                || Owner().FindChild("") != nullptr
                || !reorderedBefore || !componentMovedBeforeReference
                || !reorderedAfter || !componentMovedAfterReference
                || !Owner().IsAlwaysVisible()
                || Owner().CullingMargin() != 4.0f
                || ownerWorld._43 != 5.0f
                || !componentAccessorsWork
                || !textDefaultsMatch || !textSettingsMatch
                || !cameraDefaultsMatch || !cameraSettingsMatch
                || !rectDefaultsMatch || !rectSettingsMatch
                || !boxDefaultsMatch || !boxSettingsMatch
                || !bodyDefaultsMatch || !bodySettingsMatch
                || !audioDefaultsMatch || !audioConstructorClamps
                || !audioSettingsMatch
                || !loadedCameraDefaultsMatch
                || GetComponentInChildren<LamaPon::RenderCullingComponent>()
                    != Owner().GetComponent<LamaPon::RenderCullingComponent>()
                || GetComponentsInChildren<
                    LamaPon::RenderCullingComponent>().size() != 2
                || GetScriptInChildren<PortableOwnerHierarchyMarker>()
                    != ownerChildMarker
                || markerInstance == nullptr
                || !markerInstance->ValidateHierarchyQueries()
                || legacyCulling == nullptr
                || !legacyCulling->AlwaysVisible()
                || legacyCulling->CullingMargin() != 4.0f)
                throw std::runtime_error("Portable scene tags or legacy object culling settings were not restored.");
            bool rejectedParentCycle{};
            try
            {
                canvasObject->SetParent(existingButton);
            }
            catch (const std::invalid_argument&)
            {
                rejectedParentCycle = true;
            }
            if (!rejectedParentCycle || canvasObject->Parent() != nullptr
                || existingButton->Parent() != canvasObject)
                throw std::runtime_error("Portable hierarchy accepted a parent cycle or changed the hierarchy after rejection.");
            canvasObject->SetEnabled(false);
            const bool inactiveParentIsSkipped =
                existingButton->GetComponentInParent<LamaPon::UICanvasComponent>() == nullptr;
            const bool inactiveParentCanBeIncluded =
                existingButton->GetComponentInParent<LamaPon::UICanvasComponent>(true) == canvas;
            const bool inactiveChildrenAreSkipped =
                canvasObject->GetComponentInChildren<LamaPon::UIButtonComponent>() == nullptr
                && canvasObject->GetComponentsInChildren<
                    LamaPon::UIRectTransformComponent>().empty()
                && canvasObject->GetScriptInChildren<PortableHierarchyMarker>() == nullptr;
            const bool inactiveChildrenCanBeIncluded =
                canvasObject->GetComponentInChildren<
                    LamaPon::UIButtonComponent>(true)
                    == existingButton->GetComponent<LamaPon::UIButtonComponent>()
                && canvasObject->GetComponentsInChildren<
                    LamaPon::UIRectTransformComponent>(true).size() == 2
                && canvasObject->GetScriptInChildren<
                    PortableHierarchyMarker>(true) == markerInstance;
            scriptApiChild.SetEnabled(false);
            const bool inactiveComponentIsDisabled =
                !scriptApiCulling.IsActiveAndEnabled();
            const bool inactiveScriptChildIsSkipped =
                GetScriptInChildren<PortableOwnerHierarchyMarker>() == nullptr;
            const bool inactiveScriptChildCanBeIncluded =
                GetScriptInChildren<PortableOwnerHierarchyMarker>(true)
                    == ownerChildMarker;
            scriptApiChild.SetEnabled(true);
            const bool activeComponentIsReenabled =
                scriptApiCulling.IsActiveAndEnabled();
            canvasObject->SetEnabled(true);
            if (!inactiveParentIsSkipped || !inactiveParentCanBeIncluded
                || !inactiveChildrenAreSkipped || !inactiveChildrenCanBeIncluded
                || !inactiveScriptChildIsSkipped
                || !inactiveScriptChildCanBeIncluded
                || !inactiveComponentIsDisabled
                || !activeComponentIsReenabled)
                throw std::runtime_error("Portable parent component search ignored hierarchy activation.");
            auto& triggerA = GetScene().CreateGameObject(
                "Portable trigger A");
            auto& triggerB = GetScene().CreateGameObject(
                "Portable trigger B");
            triggerA.GetTransform().position = {20.0f, 0.0f, 0.0f};
            triggerB.GetTransform().position = {20.5f, 0.0f, 0.0f};
            auto& triggerCollider =
                triggerA.AddComponent<LamaPon::BoxCollider3DComponent>(
                    DirectX::XMFLOAT3{1, 1, 1}, DirectX::XMFLOAT3{});
            triggerCollider.SetTrigger(true);
            triggerB.AddComponent<LamaPon::BoxCollider3DComponent>();
            auto* contactA = dynamic_cast<PortableContactProbe*>(
                triggerA.AddComponent<LamaPon::NativeScriptComponent>(
                    "Test.PortableContactProbe").Instance());
            auto* contactB = dynamic_cast<PortableContactProbe*>(
                triggerB.AddComponent<LamaPon::NativeScriptComponent>(
                    "Test.PortableContactProbe").Instance());
            if (contactA == nullptr || contactB == nullptr)
                throw std::runtime_error("Portable contact probes were not created.");
            GetScene().FixedUpdate(0.0f);
            if (contactA->triggerEnter != 1 || contactB->triggerEnter != 1
                || contactA->collisionEnter != 0 || contactB->collisionEnter != 0
                || contactA->lastOther != "Portable trigger B"
                || contactB->lastOther != "Portable trigger A"
                || !contactA->lastTrigger || !contactB->lastTrigger)
                throw std::runtime_error("Portable trigger enter callbacks lost their phase, type, or other GameObject.");
            GetScene().FixedUpdate(0.0f);
            if (contactA->triggerStay != 1 || contactB->triggerStay != 1
                || contactA->triggerEnter != 1 || contactB->triggerEnter != 1)
                throw std::runtime_error("Portable trigger stay callbacks did not follow the enter phase.");
            triggerB.GetTransform().position.x = 25.5f;
            GetScene().FixedUpdate(0.0f);
            if (contactA->triggerExit != 1 || contactB->triggerExit != 1
                || contactA->lastOther != "Portable trigger B"
                || contactB->lastOther != "Portable trigger A"
                || !contactA->lastTrigger || !contactB->lastTrigger)
                throw std::runtime_error("Portable trigger exit callbacks lost their other GameObject or trigger type.");
            triggerCollider.SetTrigger(false);
            triggerB.GetTransform().position.x = 20.5f;
            GetScene().FixedUpdate(0.0f);
            if (contactA->collisionEnter != 1 || contactB->collisionEnter != 1
                || contactA->triggerEnter != 1 || contactB->triggerEnter != 1
                || contactA->lastTrigger || contactB->lastTrigger)
                throw std::runtime_error("Portable collision enter callbacks were routed as triggers or lost their other GameObject.");
            GetScene().FixedUpdate(0.0f);
            if (contactA->collisionStay != 1 || contactB->collisionStay != 1)
                throw std::runtime_error("Portable collision stay callbacks did not follow the enter phase.");
            triggerB.GetTransform().position.x = 25.5f;
            GetScene().FixedUpdate(0.0f);
            if (contactA->collisionExit != 1 || contactB->collisionExit != 1
                || contactA->lastOther != "Portable trigger B"
                || contactB->lastOther != "Portable trigger A"
                || contactA->lastTrigger || contactB->lastTrigger)
                throw std::runtime_error("Portable collision exit callbacks lost their other GameObject or collision type.");
            auto& hierarchyRemovalProbe = GetScene().CreateGameObject(
                "Portable hierarchy removal probe");
            hierarchyRemovalProbe.SetParent(&Owner());
            if (!GetScene().DestroyGameObject(hierarchyRemovalProbe))
                throw std::runtime_error("Portable hierarchy removal probe could not schedule child destruction.");
            m_hierarchyRemovalPending = true;
            if (!existingButton
                || GetScene().Load("/assets/scenes/MalformedType.scene.json")
                || GetScene().FindGameObjectByName("Test Button") != existingButton
                || GetScene().FindGameObjectByName("Malformed type probe") != nullptr)
                throw std::runtime_error("Malformed scene values must fail without changing the active scene.");
            const auto activeScenePath = GetScene().Scenes().CurrentScenePath();
            const auto activeSceneRevision = GetScene().Scenes().LoadRevision();
            if (GetScene().Load(
                    "/assets/scenes/MalformedEnvironment.scene.json")
                || GetScene().Scenes().CurrentScenePath() != activeScenePath
                || GetScene().Scenes().LoadRevision() != activeSceneRevision
                || GetScene().Scenes().LastError().empty())
                throw std::runtime_error("Malformed scene environment changed the active scene camera, path, or revision.");
            for (int index = 0; index < 2; ++index)
            {
                auto& model = GetScene().CreateGameObject("Embedded texture probe");
                model.GetTransform().position = {index ? 1.5f : -1.5f, -1.5f, 0};
                model.AddComponent<LamaPon::ModelRendererComponent>(index
                    ? "models/embedded-buffer.glb" : "models/embedded-data.gltf");
            }
            SaveText("portable-save-empty", "");
            if (!LoadText("portable-save-empty", "fallback").empty()
                || LoadText("portable-save-missing", "fallback") != "fallback")
                throw std::runtime_error("Web save must distinguish an empty value from a missing key.");
            const int persistenceProbe = EM_ASM_INT({
                const step = new URLSearchParams(location.search).get('lamaponSaveProbe');
                return step === 'write' ? 1 : step === 'verify' ? 2 : 0;
            });
            if (persistenceProbe == 1)
            {
                SaveText("browser-restart", "saved-on-first-launch");
                if (LoadText("browser-restart") != "saved-on-first-launch")
                    throw std::runtime_error("Web save did not read back during the writing launch.");
            }
            else if (persistenceProbe == 2
                && LoadText("browser-restart", "missing") != "saved-on-first-launch")
                throw std::runtime_error("Web localStorage did not preserve a save across browser launches.");
            SaveText("portable-save-failure", "before");
            EM_ASM({
                window.__probeSetItem = Storage.prototype.setItem;
                Storage.prototype.setItem = function() { throw new DOMException('Probe storage refusal', 'QuotaExceededError'); };
            });
            bool rejected = false;
            try { SaveText("portable-save-failure", "after"); }
            catch (const std::runtime_error&) { rejected = true; }
            EM_ASM({ Storage.prototype.setItem = window.__probeSetItem; delete window.__probeSetItem; });
            if (!rejected || LoadText("portable-save-failure") != "before"
                || !EM_ASM_INT({ return Boolean(document.body.dataset.lamaponSaveError)
                    && document.body.dataset.lamaponSavedValue === 'before'; }))
                throw std::runtime_error("Rejected storage must preserve the old value and must not report save success.");
            const auto* point = Find("Point Light")->GetComponent<LamaPon::PointLightComponent>();
            const auto* spot = Find("Spot Light")->GetComponent<LamaPon::SpotLightComponent>();
            if (Find("Spot Light")->GetComponent<LamaPon::PointLightComponent>()
                || Find("Test Button")->GetComponent<LamaPon::UIImageComponent>())
                throw std::runtime_error("Portable component queries must keep native UI and light types distinct.");
            const auto position = point->WorldPosition();
            if (position.x != 1 || position.y != 2 || position.z != 3 || point->Intensity() != 4 || spot->Intensity() != 5)
                throw std::runtime_error("Loaded local light settings differ.");
            const auto area = Find("Test Button")->GetComponent<LamaPon::UIRectTransformComponent>()->Resolve(1280,720);
            if (area.Size().x != 440 || area.Size().y != 112)
                throw std::runtime_error("Canvas scaling must apply to child UI rectangles.");
            // 例外捕捉が無効なBuildではabortし、running状態へ到達できない。
            try
            {
                throw std::runtime_error("expected portable exception");
            }
            // Portable runtimeが捕捉した初期化例外を記録します。
            catch (const std::runtime_error&)
            {
                LamaPon::Logger::Instance().Info("Portable startup recovery passed.");
            }
        }
        void Update(float) override
        {
            const bool ownerDestroyWasPending = m_ownerDestroyPending;
            if (!m_prefabLifecycleChecked)
            {
                if (portablePrefabLifecycle.start != 2)
                    throw std::runtime_error("Portable runtime Prefab scripts did not receive Start exactly once after Awake and OnEnable.");
                m_prefabLifecycleChecked = true;
            }
            if (m_updateFrames != 0 && !m_lateUpdateObserved)
                throw std::runtime_error("Portable LateUpdate was not called after the previous Update.");
            ++m_updateFrames;
            m_lateUpdateObserved = false;
            if (m_hierarchyRemovalPending)
            {
                if (GetScene().FindGameObjectByName(
                        "Portable hierarchy removal probe") != nullptr
                    || Owner().Children().size() != 1
                    || Owner().Children().front()->Name()
                        != "Portable script API child")
                    throw std::runtime_error("Portable object destruction left a dangling child in its parent's hierarchy.");
                m_hierarchyRemovalPending = false;
            }
            if (!m_ownerLifecycleChecked)
            {
                auto* child = GetScene().FindGameObjectByName(
                    "Portable script API child");
                auto* marker = child != nullptr
                    ? child->GetScript<PortableOwnerHierarchyMarker>()
                    : nullptr;
                if (marker == nullptr || portableOwnerLifecycle.awake != 1
                    || portableOwnerLifecycle.start != 1
                    || portableOwnerLifecycle.enable != 1
                    || portableOwnerLifecycle.disable != 0)
                    throw std::runtime_error("Portable Awake, OnEnable, or Start lifecycle order differs.");
                child->SetEnabled(false);
                if (portableOwnerLifecycle.disable != 1
                    || GetScriptInChildren<PortableOwnerHierarchyMarker>() != nullptr
                    || GetScriptInChildren<PortableOwnerHierarchyMarker>(true)
                        != marker)
                    throw std::runtime_error("Portable OnDisable or inactive script search did not update immediately.");
                child->SetEnabled(true);
                if (portableOwnerLifecycle.enable != 2
                    || portableOwnerLifecycle.start != 1)
                    throw std::runtime_error("Portable reactivation must call OnEnable without repeating Start.");
                auto* scriptComponent =
                    child->GetComponent<LamaPon::NativeScriptComponent>();
                if (scriptComponent == nullptr)
                    throw std::runtime_error("Portable script component was not found for active-state testing.");
                scriptComponent->SetEnabled(false);
                if (portableOwnerLifecycle.disable != 2)
                    throw std::runtime_error("Disabling a Portable script component did not call OnDisable.");
                scriptComponent->SetEnabled(true);
                if (portableOwnerLifecycle.enable != 3
                    || portableOwnerLifecycle.start != 1)
                    throw std::runtime_error("Re-enabling a Portable script component repeated Start or missed OnEnable.");
                if (!GetScene().DestroyGameObject(*child)
                    || portableOwnerLifecycle.disable != 3)
                    throw std::runtime_error("Portable destruction did not disable the active script.");
                m_ownerLifecycleChecked = true;
                m_ownerDestroyPending = true;
            }
            if (ownerDestroyWasPending)
            {
                if (GetScene().FindGameObjectByName(
                        "Portable script API child") != nullptr
                    || portableOwnerLifecycle.destroy != 1
                    || !Owner().Children().empty())
                    throw std::runtime_error("Portable OnDestroy did not run before removing the child script.");
                GetScene().Events().Publish("PortableSmoke.DestroyedScript");
                if (portableOwnerLifecycle.eventAfterDestroy != 0)
                    throw std::runtime_error("Destroying a Portable Script did not remove its event subscriptions.");
                m_ownerDestroyPending = false;
            }
            if (m_transitionPending)
            {
                if (++m_transitionFrames > 300)
                    throw std::runtime_error("Web UIButton targetScene did not complete within 300 frames.");
                return;
            }
            auto* button = Find("Test Button")->GetComponent<LamaPon::UIButtonComponent>();
            if (m_frame == 0)
            {
                // Headless virtual-time capture may deliver only two RAFs. Timers
                // let this multi-frame keyboard/mouse probe complete deterministically.
                if (emscripten_set_main_loop_timing(EM_TIMING_SETTIMEOUT, 16) != 0)
                    throw std::runtime_error("Cannot set Web probe frame timing.");
                ++m_frame;
                EM_ASM({ setTimeout(() => {
                    window.dispatchEvent(new KeyboardEvent('keydown', {key:'Tab', code:'Tab', bubbles:true}));
                    window.dispatchEvent(new KeyboardEvent('keyup', {key:'Tab', code:'Tab', bubbles:true}));
                    window.__probeFocused = true;
                }, 0); });
            }
            else if (m_frame == 1 && EM_ASM_INT({ return Boolean(window.__probeFocused); }))
            {
                if (!button->IsFocused()) throw std::runtime_error("Web Tab did not focus the UI button.");
                ++m_frame;
                EM_ASM({ setTimeout(() => {
                    window.dispatchEvent(new KeyboardEvent('keydown', {key:'Enter', code:'Enter', bubbles:true}));
                    window.dispatchEvent(new KeyboardEvent('keyup', {key:'Enter', code:'Enter', bubbles:true}));
                    window.__probeConfirmed = true;
                }, 0); });
            }
            else if (m_frame == 2 && EM_ASM_INT({ return Boolean(window.__probeConfirmed); }))
            {
                if (!button->IsFocused() || !button->ConsumeClick() || button->ConsumeClick())
                    throw std::runtime_error("Web Enter did not confirm the focused UI button once.");
                if (m_buttonEventCount != 1 || m_lastButtonEventSender != "Test Button")
                    throw std::runtime_error("Web UIButton clickEvent did not emit its sender to Script::On.");
                ++m_frame;
                EM_ASM({
                    setTimeout(() => {
                    const canvas = document.body.dataset.lamaponRenderer === 'canvas2d'
                        ? document.getElementById('software-canvas') : document.getElementById('canvas');
                    const rect = canvas.getBoundingClientRect();
                    const event = ({button: 0, clientX: rect.left+rect.width/2, clientY: rect.top+rect.height/2, bubbles: true});
                    canvas.dispatchEvent(new MouseEvent('mousedown', event));
                    window.dispatchEvent(new MouseEvent('mouseup', event));
                    window.__probeClicked = true;
                    }, 0);
                });
            }
            else if (m_frame == 3 && EM_ASM_INT({ return Boolean(window.__probeClicked)
                && window.__encodedImageProbe === 'passed'; }))
            {
                if (!button->ConsumeClick() || button->ConsumeClick())
                    throw std::runtime_error("Web button click must be consumable once.");
                if (m_buttonEventCount != 2 || m_lastButtonEventSender != "Test Button")
                    throw std::runtime_error("Web pointer clickEvent did not emit exactly once to Script::On.");
                if (!EM_ASM_INT({
                    const button = document.getElementById('lamapon-portable-sprite-3');
                    const label = document.getElementById('lamapon-portable-text-3');
                    const image = document.getElementById('lamapon-portable-sprite-4');
                    return button && label && image && button.style.width === '440px' && label.textContent === 'Web Button';
                })) throw std::runtime_error("Loaded UI image and button must render.");
                button->SetInteractable(false);
                if (button->IsHovered() || button->IsPressed() || button->IsFocused())
                    throw std::runtime_error("Disabled button must clear interaction state.");
                m_frame = 4;
            }
            else if (m_frame == 4)
            {
                const bool rendered = EmbeddedModelsRendered();
                if (!rendered)
                {
                    if (++m_modelFrames > 200)
                        throw std::runtime_error("Embedded glTF/GLB images did not reach the Web model framebuffer.");
                    return;
                }
                button->SetInteractable(true);
                auto& next = GetScene().CreateGameObject("Stick navigation button");
                next.AddComponent<LamaPon::UIRectTransformComponent>(
                    DirectX::XMFLOAT2{0.5f,0.5f}, DirectX::XMFLOAT2{0.5f,0.5f},
                    DirectX::XMFLOAT2{0.5f,0.5f}, DirectX::XMFLOAT2{250,0}, DirectX::XMFLOAT2{60,40});
                auto& stickButton = next.AddComponent<LamaPon::UIButtonComponent>("Stick");
                stickButton.SetClickEventName("PortableSmoke.ButtonClicked");
                EM_ASM({
                    window.__probeGamepadsDescriptor = Object.getOwnPropertyDescriptor(navigator, 'getGamepads');
                    window.__probePad = ({id:'Portable navigation probe', index:0, connected:true,
                        mapping:'standard', timestamp:0, axes:[0.8,0,0,0],
                        buttons:Array.from({length:32}, () => ({pressed:false, touched:false, value:0}))});
                    Object.defineProperty(navigator, 'getGamepads', {configurable:true,
                        value:() => window.__probePad ? [window.__probePad] : []});
                });
                m_frame = 5;
            }
            else if (m_frame == 5 || m_frame == 6)
            {
                if (!button->IsFocused()) throw std::runtime_error("Web left stick failed to focus or repeated while held.");
                if (m_frame == 6) EM_ASM({ window.__probePad.axes[0] = 0; });
                ++m_frame;
            }
            else if (m_frame == 7)
            {
                EM_ASM({ window.__probePad.axes[0] = 0.8; });
                ++m_frame;
            }
            else if (m_frame == 8)
            {
                auto* next = Find("Stick navigation button")->GetComponent<LamaPon::UIButtonComponent>();
                if (!next->IsFocused() || button->IsFocused())
                    throw std::runtime_error("Web re-armed left stick did not move UI focus.");
                EM_ASM({ window.__probePad.buttons[0] = ({pressed:true, touched:true, value:1}); });
                ++m_frame;
            }
            else if (m_frame == 9)
            {
                auto* next = Find("Stick navigation button")->GetComponent<LamaPon::UIButtonComponent>();
                if (!next->ConsumeClick() || next->ConsumeClick())
                    throw std::runtime_error("Web gamepad confirm did not click the focused button once.");
                if (m_buttonEventCount != 3
                    || m_lastButtonEventSender != "Stick navigation button")
                    throw std::runtime_error("Web gamepad clickEvent did not publish exactly once with its button sender.");
                EM_ASM({ window.__probePad = null; });
                ++m_frame;
            }
            else if (m_frame == 10)
            {
                if (Find("Stick navigation button")->GetComponent<LamaPon::UIButtonComponent>()->WasClicked())
                    throw std::runtime_error("Disconnected Web gamepad retained a click.");
                EM_ASM({
                    if (window.__probeGamepadsDescriptor)
                        Object.defineProperty(navigator, 'getGamepads', window.__probeGamepadsDescriptor);
                    else delete navigator.getGamepads;
                    delete window.__probeGamepadsDescriptor; delete window.__probePad;
                });
                if (EM_ASM_INT({ return document.body.dataset.lamaponRenderer === 'canvas2d'; }))
                    Finish();
                else
                {
                    BeginContextProbe();
                }
            }
            else if (m_frame == 11)
            {
                if (EM_ASM_INT({ return document.body.dataset.lamaponGraphicsRecovery === 'failed'; }))
                    throw std::runtime_error("Web graphics resource restoration failed.");
                if (EM_ASM_INT({ return Boolean(window.__probeContextLost)
                    && document.body.dataset.lamaponGraphicsRecovery === 'restored'; }))
                    m_frame = 12;
                else if (++m_modelFrames > 200)
                    throw std::runtime_error("Web context did not restore.");
            }
            else if (m_frame == 12)
            {
                const bool encodedRestored = EM_ASM_INT({
                    const gl = globalThis.__lamaponWebGl;
                    const slot = globalThis.__lamaponTextures[window.__encodedImageTextureId];
                    if (!slot || !slot.ready || !gl.isTexture(slot.texture)) return false;
                    const previous = gl.getParameter(gl.FRAMEBUFFER_BINDING);
                    const framebuffer = gl.createFramebuffer();
                    gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
                    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, slot.texture, 0);
                    const pixel = new Uint8Array(4);
                    gl.readPixels(0,0,1,1,gl.RGBA,gl.UNSIGNED_BYTE,pixel);
                    gl.bindFramebuffer(gl.FRAMEBUFFER, previous);
                    gl.deleteFramebuffer(framebuffer);
                    return pixel[0] === 220 && pixel[1] === 30 && pixel[2] === 80 && pixel[3] === 255;
                });
                if (encodedRestored && EmbeddedModelsRendered())
                {
                    if (++m_recoveries == 2) Finish();
                    else BeginContextProbe();
                }
                else if (++m_modelFrames > 200)
                    throw std::runtime_error("Web mesh or embedded image pixels did not recover.");
            }
        }
        void LateUpdate(float) override
        {
            m_lateUpdateObserved = true;
        }
    private:
        bool EmbeddedModelsRendered() const
        {
        return EM_ASM_INT({
                    const gl = globalThis.__lamaponWebGl;
                    const left = new Uint8Array(4);
                    const right = new Uint8Array(4);
                    if (document.body.dataset.lamaponRenderer !== 'canvas2d') {
                        const canvas = document.getElementById('canvas');
                        gl.readPixels(Math.floor(canvas.width*.35), Math.floor(canvas.height*.24), 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, left);
                        gl.readPixels(Math.floor(canvas.width*.65), Math.floor(canvas.height*.24), 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, right);
                    } else {
                        const runtime = globalThis.__lamaponCanvas2D;
                        left.set(runtime.context.getImageData(Math.floor(runtime.canvas.width*.35), Math.floor(runtime.canvas.height*.76), 1, 1).data);
                        right.set(runtime.context.getImageData(Math.floor(runtime.canvas.width*.65), Math.floor(runtime.canvas.height*.76), 1, 1).data);
                    }
                    return left[0] > 180 && left[1] < 60 && left[2] < 110
                        && right[1] > 180 && right[0] < 90 && right[2] < 90;
                });
        }
        void BeginContextProbe()
        {
            EM_ASM({
                window.__probeContextLost = false;
                const gl = globalThis.__lamaponWebGl;
                const extension = gl.getExtension('WEBGL_lose_context');
                if (!extension) throw new Error('Context-loss test extension unavailable');
                const canvas = document.getElementById('canvas');
                canvas.addEventListener('webglcontextlost', () => {
                    window.__probeContextLost = true;
                    setTimeout(() => extension.restoreContext(), 100);
                }, {once:true});
                extension.loseContext();
            });
            m_modelFrames = 0;
            m_frame = 11;
        }
        void Finish()
        {
            auto* buttonObject = Find("Test Button");
            auto* button = buttonObject
                ? buttonObject->GetComponent<LamaPon::UIButtonComponent>()
                : nullptr;
            if (button == nullptr)
                throw std::runtime_error("Web UIButton scene transition probe could not find its button.");
            button->SetTargetScene("scenes/UIButtonTarget.scene.json");
            m_transitionPending = true;
            EM_ASM({
                setTimeout(() => {
                    const canvas = document.body.dataset.lamaponRenderer === 'canvas2d'
                        ? document.getElementById('software-canvas') : document.getElementById('canvas');
                    const rect = canvas.getBoundingClientRect();
                    const event = ({button:0, clientX:rect.left+rect.width/2,
                        clientY:rect.top+rect.height/2, bubbles:true});
                    canvas.dispatchEvent(new MouseEvent('mousedown', event));
                    window.dispatchEvent(new MouseEvent('mouseup', event));
                }, 0);
            });
        }
        int m_frame{};
        int m_modelFrames{};
        int m_recoveries{};
        int m_updateFrames{};
        bool m_hierarchyRemovalPending{};
        bool m_ownerLifecycleChecked{};
        bool m_ownerDestroyPending{};
        bool m_prefabLifecycleChecked{};
        bool m_lateUpdateObserved{};
        std::uint64_t m_buttonEventSubscription{};
        int m_buttonEventCount{};
        std::string m_lastButtonEventSender;
        int m_scriptEventCount{};
        LamaPon::GameObject* m_scriptEventSender{};
        int m_payloadEventCount{};
        LamaPon::EventArgs m_lastPayloadEvent;
        int m_noArgumentEventCount{};
        bool m_transitionPending{};
        int m_transitionFrames{};
        int m_awakeCalls{};
        int m_enableCalls{};
        int m_disableCalls{};
    };
}

LAMAPON_SCRIPT_NAMED(PortableStartupProbe, "Test.PortableStartup", "Portable startup probe");
LAMAPON_SCRIPT_NAMED(PortableHierarchyMarker, "Test.PortableHierarchyMarker", "Portable hierarchy marker");
LAMAPON_SCRIPT_NAMED(PortableOwnerHierarchyMarker, "Test.PortableOwnerHierarchyMarker", "Portable owner hierarchy marker");
LAMAPON_SCRIPT_NAMED(PortablePrefabProbe, "Test.PortablePrefabProbe", "Portable prefab lifecycle probe");
LAMAPON_SCRIPT_NAMED(PortableContactProbe, "Test.PortableContactProbe", "Portable contact probe");
