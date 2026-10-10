#include "LamaPon/LamaPon.h"
#include <stdexcept>

// This game source uses only the public Portable API, so it can also exercise
// the compatibility scanner and the generated build project.
namespace
{
    class DefaultMacroRegistrationProbe final : public LamaPon::Script
    {
    public:
        void Start() override {}
    };

    class ExportStartupProbe final : public LamaPon::Script
    {
    public:
        void Update(float) override
        {
            const auto& pointer = Graphics().Input().Pointer();
            const auto& left = pointer.Button(LamaPon::PointerButton::Left);
            if (!left.pressed && !left.released && !left.down)
                return;

            const auto* buttonObject = Find("Button");
            const auto* button = buttonObject
                ? buttonObject->GetComponent<LamaPon::UIButtonComponent>()
                : nullptr;
            SaveText("native-pointer-diagnostic",
                "valid=" + std::to_string(pointer.valid)
                + ",position=" + std::to_string(pointer.position.x)
                + ":" + std::to_string(pointer.position.y)
                + ",viewport=" + std::to_string(Graphics().UIWidth())
                + ":" + std::to_string(Graphics().UIHeight())
                + ",left=" + std::to_string(left.pressed)
                + ":" + std::to_string(left.down)
                + ":" + std::to_string(left.released)
                + ",button=" + std::to_string(button != nullptr)
                + ":" + std::to_string(button && button->IsHovered())
                + ":" + std::to_string(button && button->IsPressed())
                + ":" + std::to_string(button && button->WasClicked()));
        }

        void Start() override
        {
            const auto buttonClickSubscription = On(
                "Native.ExportButtonClicked",
                [this](const LamaPon::EventArgs& eventArgs)
                {
                    SaveInteger(
                        "native-button-clicks",
                        LoadInteger("native-button-clicks", 0) + 1);
                    SaveText(
                        "native-button-sender",
                        eventArgs.sender ? eventArgs.sender->Name() : "<null>");
                });
            if (buttonClickSubscription == 0)
                throw std::runtime_error("Portable export button event subscription failed");
            if (!LamaPon::CreatePortableScript("Game.DefaultMacroRegistrationProbe"))
                throw std::runtime_error("Default script registration macro did not register its factory");
            const auto previous = LoadInteger("native-probe-starts", 0);
            SaveInteger("native-probe-starts", previous + 1);
            SaveText("native-probe-text", "保存テスト / Linux Android");
            if (LoadInteger("native-probe-starts") != previous + 1
                || LoadText("native-probe-text") != "保存テスト / Linux Android")
                throw std::runtime_error("Exported game save round trip failed");
            auto* image = Find("Green image");
            auto* imageRect = image
                ? image->GetComponent<LamaPon::UIRectTransformComponent>()
                : nullptr;
            auto* imageRenderer = image
                ? image->GetComponent<LamaPon::UIImageComponent>()
                : nullptr;
            if (!image || imageRect == nullptr || imageRenderer == nullptr)
                throw std::runtime_error("Exported game scene did not load");
            if (imageRenderer->TexturePath().generic_u8string()
                != u8"scenes/白画像.bmp")
                throw std::runtime_error("Exported game changed a UTF-8 asset filename");

            const auto originalName = image->Name();
            image->SetName("Portable name setter probe");
            const bool setNameWorks = image->Name() == "Portable name setter probe";
            image->SetName(originalName);
            const auto* imageParent = image->Parent();
            const bool findChildWorks = imageParent != nullptr
                && imageParent->FindChild("Green image") == image
                && imageParent->FindChild("") == nullptr;
            const bool componentAccessorsWork = imageRenderer->IsEnabled()
                && imageRenderer->IsActiveAndEnabled()
                && &imageRenderer->GetTransform() == &image->GetTransform()
                && imageRenderer->ScriptInstance() == nullptr;
            const auto ownerWorld = Owner().WorldMatrix();
            const bool cullingAccessorsWork = Owner().IsAlwaysVisible()
                && Owner().CullingMargin() == 4.0f;
            const bool orderMovedBefore = image->ReorderComponent(
                *imageRenderer, *imageRect, false);
            const bool movedBeforeReference = orderMovedBefore
                && image->Components().size() == 2
                && image->Components()[0].get() == imageRenderer
                && image->Components()[1].get() == imageRect;
            const bool orderMovedAfter = image->ReorderComponent(
                *imageRenderer, *imageRect, true);
            const bool movedAfterReference = orderMovedAfter
                && image->Components().size() == 2
                && image->Components()[0].get() == imageRect
                && image->Components()[1].get() == imageRenderer;
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
            const LamaPon::TextRendererComponent configuredText(
                "configured", "Portable Sans", 0.5f,
                {1.0f, 0.5f, 0.25f, 1.0f}, {8192.0f, -4.0f}, true,
                LamaPon::TextHorizontalAlignment::Right,
                LamaPon::TextVerticalAlignment::Center);
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
                && textRenderer.SortOrder() == 17
                && configuredText.FontSize() == 1.0f
                && configuredText.LayoutSize().x == 4096.0f
                && configuredText.LayoutSize().y == 0.0f
                && configuredText.HorizontalAlignment()
                    == LamaPon::TextHorizontalAlignment::Right
                && configuredText.VerticalAlignment()
                    == LamaPon::TextVerticalAlignment::Center;
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
            const auto* ownerCamera =
                Owner().GetComponent<LamaPon::CameraComponent>();
            const bool loadedCameraDefaultsMatch = ownerCamera != nullptr
                && ownerCamera->VerticalFieldOfView() == DirectX::XM_PI / 4.0f
                && ownerCamera->NearPlane() == 0.1f
                && ownerCamera->FarPlane() == 1000.0f;
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
            if (!rectDefaultsMatch || !rectSettingsMatch)
                throw std::runtime_error(
                    "Portable UI rectangle API differs from the Windows contract");
            SaveText("native-ui-rect-api", "passed");
            const auto* ownerAudio =
                Owner().GetComponent<LamaPon::AudioSourceComponent>();
            if (ownerAudio == nullptr || !ownerAudio->Loop()
                || !ownerAudio->PlayOnStart())
                throw std::runtime_error(
                    "Portable audio startup changed the saved PlayOnStart setting");
            SaveText("native-audio-startup-config", "passed");
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
            if (!setNameWorks || !findChildWorks || !componentAccessorsWork
                || !cullingAccessorsWork || ownerWorld._43 != 5.0f
                || !movedBeforeReference || !movedAfterReference
                || !textDefaultsMatch || !textSettingsMatch
                || !cameraDefaultsMatch || !cameraSettingsMatch
                || !loadedCameraDefaultsMatch || !boxDefaultsMatch
                || !boxSettingsMatch || !bodyDefaultsMatch
                || !bodySettingsMatch || !audioDefaultsMatch
                || !audioConstructorClamps || !audioSettingsMatch)
                throw std::runtime_error("Portable public API behavior diverged from the Windows contract");
            SaveText("native-camera-api", "passed");
            SaveText("native-physics-api", "passed");
            SaveText("native-audio-api", "passed");
        }
    };
}
LAMAPON_SCRIPT(DefaultMacroRegistrationProbe);
LAMAPON_SCRIPT_NAMED(ExportStartupProbe, "Test.NativeStartup", "Export startup probe");
