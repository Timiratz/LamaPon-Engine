#include "LamaPon/LamaPon.h"
#include "LamaPon/Native/NativeGL.h"
#include "LamaPon/Native/NativeInput.h"
#include "LamaPon/Native/NativeServices.h"
#include "LamaPon/Web/WebAudioRuntime.h"
#include "LamaPon/Web/WebRenderer3D.h"
#include "../EncodedImageFixture.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace
{
    struct NativeLifecycleCounts final
    {
        int awake{};
        int start{};
        int enable{};
        int disable{};
        int destroy{};
    };
    NativeLifecycleCounts nativeContactLifecycleA;
    NativeLifecycleCounts nativeContactLifecycleB;

    struct NativePrefabLifecycleCounts final
    {
        int awake{};
        int start{};
        int enable{};
    };
    NativePrefabLifecycleCounts nativePrefabLifecycle;

    class NativeContactProbe final : public LamaPon::Script
    {
    public:
        void Awake() override { ++Counts().awake; }
        void Start() override { ++Counts().start; }
        void OnEnable() override { ++Counts().enable; }
        void OnDisable() override { ++Counts().disable; }
        void OnDestroy() override { ++Counts().destroy; }
        void OnCollisionEnter(const LamaPon::CollisionEvent& event) override
        { ++collisionEnter; lastOther = event.other.Name(); lastTrigger = event.isTrigger; }
        void OnCollisionStay(const LamaPon::CollisionEvent& event) override
        { ++collisionStay; lastOther = event.other.Name(); lastTrigger = event.isTrigger; }
        void OnCollisionExit(const LamaPon::CollisionEvent& event) override
        { ++collisionExit; lastOther = event.other.Name(); lastTrigger = event.isTrigger; }
        void OnTriggerEnter(const LamaPon::CollisionEvent& event) override
        { ++triggerEnter; lastOther = event.other.Name(); lastTrigger = event.isTrigger; }
        void OnTriggerStay(const LamaPon::CollisionEvent& event) override
        { ++triggerStay; lastOther = event.other.Name(); lastTrigger = event.isTrigger; }
        void OnTriggerExit(const LamaPon::CollisionEvent& event) override
        { ++triggerExit; lastOther = event.other.Name(); lastTrigger = event.isTrigger; }

        int collisionEnter{};
        int collisionStay{};
        int collisionExit{};
        int triggerEnter{};
        int triggerStay{};
        int triggerExit{};
        std::string lastOther;
        bool lastTrigger{};

    private:
        NativeLifecycleCounts& Counts()
        {
            return Owner().Name() == "Native trigger A"
                ? nativeContactLifecycleA
                : nativeContactLifecycleB;
        }
    };

    class NativePrefabProbe final : public LamaPon::Script
    {
    public:
        void Awake() override { ++nativePrefabLifecycle.awake; }
        void Start() override { ++nativePrefabLifecycle.start; }
        void OnEnable() override { ++nativePrefabLifecycle.enable; }
    };

    bool started{};
    SDL_WindowID probeWindowId{};
    std::uint32_t probeTexture{};
    std::uint32_t encodedProbeTexture{};
    void ProbeEncodedTexture()
    {
        const auto generate = reinterpret_cast<decltype(&::glGenFramebuffers)>(SDL_GL_GetProcAddress("glGenFramebuffers"));
        const auto bind = reinterpret_cast<decltype(&::glBindFramebuffer)>(SDL_GL_GetProcAddress("glBindFramebuffer"));
        const auto attach = reinterpret_cast<decltype(&::glFramebufferTexture2D)>(SDL_GL_GetProcAddress("glFramebufferTexture2D"));
        const auto status = reinterpret_cast<decltype(&::glCheckFramebufferStatus)>(SDL_GL_GetProcAddress("glCheckFramebufferStatus"));
        const auto destroy = reinterpret_cast<decltype(&::glDeleteFramebuffers)>(SDL_GL_GetProcAddress("glDeleteFramebuffers"));
        const auto query = reinterpret_cast<decltype(&::glGetIntegerv)>(SDL_GL_GetProcAddress("glGetIntegerv"));
        if (!generate || !bind || !attach || !status || !destroy || !query || !LamaPon::Native::BindTexture(encodedProbeTexture))
            throw std::runtime_error("Cannot inspect the encoded image texture");
        GLint texture{}, previous{};
        query(GL_TEXTURE_BINDING_2D, &texture);
        query(GL_FRAMEBUFFER_BINDING, &previous);
        GLuint framebuffer{}; generate(1, &framebuffer); bind(GL_FRAMEBUFFER, framebuffer);
        attach(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, static_cast<GLuint>(texture), 0);
        const bool complete = status(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        std::array<unsigned char, 4> pixel{};
        if (complete) glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        bind(GL_FRAMEBUFFER, static_cast<GLuint>(previous)); destroy(1, &framebuffer);
        if (!complete || pixel != std::array<unsigned char, 4>{220, 30, 80, 255})
            throw std::runtime_error("Encoded image pixels changed before or after GL context recovery");
    }
    void ProbeButtonNavigation()
    {
        struct BackgroundInputHint final
        {
            std::string previous;
            bool existed{};
            BackgroundInputHint()
            {
                if (const char* value = SDL_GetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS))
                { previous = value; existed = true; }
                SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
            }
            ~BackgroundInputHint()
            {
                if (existed) SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, previous.c_str());
                else SDL_ResetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS);
            }
        } backgroundInputHint;
        struct VirtualPad final
        {
            SDL_JoystickID id{};
            SDL_Joystick* joystick{};
            ~VirtualPad() { if (joystick) SDL_CloseJoystick(joystick); if (id) SDL_DetachVirtualJoystick(id); }
        } pad;
        SDL_VirtualJoystickDesc descriptor{};
        SDL_INIT_INTERFACE(&descriptor);
        descriptor.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        descriptor.name = "LamaPon UI navigation probe";
        descriptor.naxes = SDL_GAMEPAD_AXIS_COUNT;
        descriptor.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        descriptor.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
        descriptor.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
        pad.id = SDL_AttachVirtualJoystick(&descriptor);
        pad.joystick = SDL_OpenJoystick(pad.id);
        if (!pad.id || !pad.joystick || !SDL_IsGamepad(pad.id))
            throw std::runtime_error("SDL virtual gamepad did not attach");
        LamaPon::Native::NativeInput input;
        input.Initialize(LamaPon::Native::GameWindow());
        if (!input.SelectGamepad(pad.id)) throw std::runtime_error("Cannot select SDL virtual gamepad");
        LamaPon::Web::Renderer3D renderer;
        LamaPon::Web::WebAudioRuntime audio;
        LamaPon::Scene scene(renderer, audio, input);
        scene.Graphics().SetUiSize(640, 360);
        int gamepadButtonEvents{};
        LamaPon::GameObject* gamepadButtonEventSender{};
        const auto gamepadButtonSubscription = scene.Events().Subscribe(
            "NativeSmoke.GamepadButtonClicked",
            [&](const LamaPon::EventArgs& eventArgs)
            {
                ++gamepadButtonEvents;
                gamepadButtonEventSender = eventArgs.sender;
            });
        if (gamepadButtonSubscription == 0)
            throw std::runtime_error("Native gamepad button event subscription failed");
        const auto button = [&](const char* name, float x, float y) -> LamaPon::UIButtonComponent& {
            auto& object = scene.CreateGameObject(name);
            object.AddComponent<LamaPon::UIRectTransformComponent>(
                DirectX::XMFLOAT2{0.5f,0.5f}, DirectX::XMFLOAT2{0.5f,0.5f},
                DirectX::XMFLOAT2{0.5f,0.5f}, DirectX::XMFLOAT2{x,y}, DirectX::XMFLOAT2{100,40});
            return object.AddComponent<LamaPon::UIButtonComponent>(name);
        };
        auto& first = button("First", -100, -80);
        first.SetClickEventName("NativeSmoke.GamepadButtonClicked");
        auto& disabled = button("Disabled", -100, 0); disabled.SetInteractable(false);
        auto& last = button("Last", -100, 80);
        last.SetClickEventName("NativeSmoke.GamepadButtonClicked");
        auto& right = button("Right", 100, -80);
        const auto frame = [&] {
            SDL_UpdateJoysticks(); input.BeginFrame(); scene.Update(1.0f/60.0f); input.EndFrame();
        };
        const auto press = [&](SDL_GamepadButton value, bool down) {
            if (!SDL_SetJoystickVirtualButton(pad.joystick, value, down))
                throw std::runtime_error("Cannot change SDL virtual gamepad input");
            frame();
        };
        frame(); // Open the newly connected gamepad before sending its first edge.
        press(SDL_GAMEPAD_BUTTON_SOUTH, true);
        if (first.WasClicked() || last.WasClicked() || first.IsFocused())
            throw std::runtime_error("Gameplay confirm/jump clicked UI without navigation focus");
        press(SDL_GAMEPAD_BUTTON_SOUTH, false);
        press(SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
        if (!first.IsFocused()) throw std::runtime_error("Gamepad did not select the first UI button");
        press(SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
        press(SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
        if (!last.IsFocused() || first.IsFocused() || disabled.IsFocused())
            throw std::runtime_error("Directional UI navigation did not skip the disabled button");
        press(SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
        press(SDL_GAMEPAD_BUTTON_SOUTH, true);
        if (!last.WasClicked() || first.WasClicked()
            || gamepadButtonEvents != 1
            || gamepadButtonEventSender != &last.Owner())
            throw std::runtime_error("Gamepad confirm did not click the focused button and publish its event sender");
        frame();
        if (last.WasClicked() || gamepadButtonEvents != 1)
            throw std::runtime_error("Held gamepad confirm generated a repeated click event");
        press(SDL_GAMEPAD_BUTTON_SOUTH, false);
        last.SetClickEventName({});
        press(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
        if (!right.IsFocused()) throw std::runtime_error("Spatial UI navigation did not select the right button");
        press(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, false);
        const auto stick = [&](Sint16 x, Sint16 y) {
            if (!SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_LEFTX, x)
                || !SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_LEFTY, y))
                throw std::runtime_error("Cannot change SDL virtual gamepad stick");
            frame();
        };
        stick(-26000, 0);
        if (!first.IsFocused()) throw std::runtime_error("Left stick did not navigate left");
        stick(-16000, 0); stick(-26000, 0);
        if (!first.IsFocused()) throw std::runtime_error("Held or jittering stick repeated UI navigation");
        stick(0, 0); stick(0, 26000);
        if (!last.IsFocused() || disabled.IsFocused()) throw std::runtime_error("Left stick down did not skip disabled UI");
        stick(0, 0); stick(0, -26000);
        if (!first.IsFocused()) throw std::runtime_error("Left stick did not navigate up");
        stick(0, 0); stick(26000, 0);
        if (!right.IsFocused()) throw std::runtime_error("Left stick did not navigate right");
        stick(0, 0);
        scene.DestroyGameObject(right.Owner()); frame();
        if (first.IsFocused() || last.IsFocused()) throw std::runtime_error("Destroyed UI focus was retained");
        press(SDL_GAMEPAD_BUTTON_SOUTH, true);
        if (first.WasClicked() || last.WasClicked()) throw std::runtime_error("Destroyed focus redirected confirmation to another button");
        press(SDL_GAMEPAD_BUTTON_SOUTH, false);
        press(SDL_GAMEPAD_BUTTON_DPAD_UP, true);
        press(SDL_GAMEPAD_BUTTON_DPAD_UP, false);
        press(SDL_GAMEPAD_BUTTON_SOUTH, true);
        if (!first.IsFocused() || !first.WasClicked()
            || gamepadButtonEvents != 2
            || gamepadButtonEventSender != &first.Owner())
            throw std::runtime_error("UI navigation recovery did not publish exactly one event for the focused button");
        press(SDL_GAMEPAD_BUTTON_SOUTH, false);
        first.SetClickEventName({});
        press(SDL_GAMEPAD_BUTTON_EAST, true);
        if (first.IsFocused()) throw std::runtime_error("Gamepad cancel did not clear UI focus");
        press(SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
        press(SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
        if (!first.IsFocused()) throw std::runtime_error("Cannot focus UI before SDL back probe");
        SDL_Event back{}; back.type = SDL_EVENT_KEY_DOWN; back.key.scancode = SDL_SCANCODE_AC_BACK;
        input.ProcessEvent(back); frame();
        if (first.IsFocused() || last.IsFocused() || first.WasClicked() || last.WasClicked())
            throw std::runtime_error("SDL back failed to cancel UI focus or generated a click");
        back.type = SDL_EVENT_KEY_UP; input.ProcessEvent(back); frame();
        press(SDL_GAMEPAD_BUTTON_EAST, false);
        input.KeyEvent("ShiftLeft", true); input.KeyEvent("Tab", true); frame();
        if (!last.IsFocused()) throw std::runtime_error("Shift-Tab did not select the last enabled UI button");
        input.KeyEvent("Tab", false); input.KeyEvent("ShiftLeft", false); frame();
        input.KeyEvent("Tab", true); frame();
        if (!first.IsFocused()) throw std::runtime_error("Tab did not wrap to the first enabled UI button");
        input.KeyEvent("Tab", false); frame();
        first.SetNavigationEnabled(false); frame();
        if (first.IsFocused()) throw std::runtime_error("Navigation-disabled button retained focus");
        press(SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
        press(SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
        press(SDL_GAMEPAD_BUTTON_SOUTH, true);
        if (!last.IsFocused() || first.WasClicked()) throw std::runtime_error("Navigation-disabled button accepted gamepad confirmation");
        press(SDL_GAMEPAD_BUTTON_SOUTH, false);
        input.PointerEvent(20,20); frame(); input.PointerEvent(30,20); frame();
        if (last.IsFocused()) throw std::runtime_error("Pointer movement did not restore mouse UI operation");
        input.KeyEvent("Tab", true); scene.Graphics().Input().SetEdgeEventsEnabled(false); frame();
        if (last.IsFocused()) throw std::runtime_error("UI navigation consumed a suppressed substep edge");
        scene.Graphics().Input().SetEdgeEventsEnabled(true);
        input.KeyEvent("Tab", false); frame();
        press(SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
        press(SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
        press(SDL_GAMEPAD_BUTTON_SOUTH, true);
        if (!last.WasClicked()) throw std::runtime_error("Gamepad confirm stopped before disconnection");
        SDL_CloseJoystick(pad.joystick); pad.joystick = nullptr;
        if (!SDL_DetachVirtualJoystick(pad.id)) throw std::runtime_error("SDL virtual gamepad did not disconnect");
        pad.id = 0; frame();
        if (input.ControlValue("GamePadA") != 0 || last.WasClicked())
            throw std::runtime_error("Disconnected gamepad retained held input or clicked UI");
        pad.id = SDL_AttachVirtualJoystick(&descriptor);
        pad.joystick = SDL_OpenJoystick(pad.id);
        if (!pad.id || !pad.joystick || !SDL_SetJoystickVirtualButton(pad.joystick, SDL_GAMEPAD_BUTTON_SOUTH, true))
            throw std::runtime_error("SDL replacement virtual gamepad did not attach");
        frame();
        if (input.ControlValue("GamePadA") != 0 || last.WasClicked())
            throw std::runtime_error("Selected gamepad silently switched to a different device");
        if (!input.SelectGamepad(pad.id)) throw std::runtime_error("Cannot select replacement virtual gamepad");
        frame();
        if (!last.WasClicked()) throw std::runtime_error("Explicit gamepad reselection did not restore UI input");
        if (input.SelectGamepad(static_cast<SDL_JoystickID>(-1)) || input.ControlValue("GamePadA") != 1)
            throw std::runtime_error("Invalid gamepad selection changed the current input device");
        if (!SDL_SetJoystickVirtualButton(pad.joystick, SDL_GAMEPAD_BUTTON_SOUTH, false))
            throw std::runtime_error("Cannot release the virtual gamepad before the UI scene probe");
        frame();
        scene.Events().Unsubscribe(gamepadButtonSubscription);
        input.Reset();
        auto& transitionObject = scene.CreateGameObject("Scene transition button");
        transitionObject.AddComponent<LamaPon::UIRectTransformComponent>(
            DirectX::XMFLOAT2{0.5f,0.5f}, DirectX::XMFLOAT2{0.5f,0.5f},
            DirectX::XMFLOAT2{0.5f,0.5f}, DirectX::XMFLOAT2{}, DirectX::XMFLOAT2{100,40});
        auto& transitionButton = transitionObject.AddComponent<LamaPon::UIButtonComponent>("Load target");
        transitionButton.SetTargetScene("scenes/UIButtonTarget.scene.json");
        transitionButton.SetClickEventName("NativeSmoke.UITransition");
        int transitionEvents{};
        LamaPon::GameObject* transitionEventSender{};
        const auto transitionSubscription = scene.Events().Subscribe(
            "NativeSmoke.UITransition",
            [&](const LamaPon::EventArgs& eventArgs)
            {
                ++transitionEvents;
                transitionEventSender = eventArgs.sender;
            });
        if (transitionSubscription == 0)
            throw std::runtime_error("Native portable scene event subscription failed");
        input.PointerEvent(320, 180);
        input.ButtonEvent(0, true);
        frame();
        if (!transitionButton.IsPressed())
            throw std::runtime_error("Portable UIButton did not enter its pointer-pressed state");
        input.ButtonEvent(0, false);
        frame();
        if (!transitionButton.WasClicked() || !scene.Scenes().HasPendingLoad()
            || transitionEvents != 1 || transitionEventSender != &transitionObject)
            throw std::runtime_error("Portable UIButton clickEvent/targetScene did not publish and queue the primary scene load");
        scene.Events().Unsubscribe(transitionSubscription);
        frame();
        if (scene.Scenes().LoadRevision() != 1
            || scene.Scenes().CurrentScenePath().generic_u8string()
                != u8"/assets/scenes/UIButtonTarget.scene.json"
            || scene.FindGameObjectByName("UIButton target marker") == nullptr)
            throw std::runtime_error("Portable UIButton targetScene did not replace the active scene");
        const auto* serializedTargetObject =
            scene.FindGameObjectByName("Serialized target button");
        const auto* serializedTargetButton = serializedTargetObject
            ? serializedTargetObject->GetComponent<LamaPon::UIButtonComponent>()
            : nullptr;
        if (serializedTargetButton == nullptr
            || serializedTargetButton->TargetScene().generic_u8string()
                != u8"scenes/UIButtonFinal.scene.json"
            || serializedTargetButton->Interactable())
            throw std::runtime_error("Native scene JSON did not restore the UIButton target path and disabled state");

        auto& reloadObject = scene.CreateGameObject("Scene reload button");
        reloadObject.AddComponent<LamaPon::UIRectTransformComponent>(
            DirectX::XMFLOAT2{0.5f,0.5f}, DirectX::XMFLOAT2{0.5f,0.5f},
            DirectX::XMFLOAT2{0.5f,0.5f}, DirectX::XMFLOAT2{}, DirectX::XMFLOAT2{100,40});
        auto& reloadButton = reloadObject.AddComponent<LamaPon::UIButtonComponent>("Reload current");
        reloadButton.SetReloadCurrentScene(true);
        input.ButtonEvent(0, true);
        frame();
        input.ButtonEvent(0, false);
        frame();
        if (!reloadButton.WasClicked() || !scene.Scenes().HasPendingLoad())
            throw std::runtime_error("Portable UIButton reloadCurrentScene did not queue a primary scene reload");
        frame();
        if (scene.Scenes().LoadRevision() != 2
            || scene.Scenes().CurrentScenePath().generic_u8string()
                != u8"/assets/scenes/UIButtonTarget.scene.json"
            || scene.FindGameObjectByName("UIButton target marker") == nullptr)
            throw std::runtime_error("Portable UIButton reloadCurrentScene did not reload the active scene");
        SDL_Log("LamaPon native gamepad UI navigation probe passed");
    }
    class NativeStartupProbe final : public LamaPon::Script
    {
    public:
        void Awake() override { ++m_awakeCalls; }
        void OnEnable() override { ++m_enableCalls; }
        void OnDisable() override { ++m_disableCalls; }

        void Start() override
        {
            if (m_awakeCalls != 1 || m_enableCalls != 1)
                throw std::runtime_error("Native Awake and OnEnable must run before Start");
            nativePrefabLifecycle = {};
            nativeContactLifecycleA = {};
            nativeContactLifecycleB = {};
            auto& prefabParent = GetScene().CreateGameObject(
                "Native runtime prefab parent");
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
                || nativePrefabLifecycle.awake != 2
                || nativePrefabLifecycle.enable != 2
                || nativePrefabLifecycle.start != 0)
                throw std::runtime_error("Native runtime Prefab did not preserve its hierarchy, values, unique IDs, or immediate lifecycle callbacks");
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
                throw std::runtime_error("Native runtime Prefab accepted a cyclic hierarchy or left partial objects behind");
            ProbeButtonNavigation();
            probeWindowId = SDL_GetWindowID(LamaPon::Native::GameWindow());
            LamaPon::Native::NativeInput input;
            if (!input.Initialize(LamaPon::Native::GameWindow()))
                throw std::runtime_error("Native input initialization failed");
            SDL_Event event{};
            event.type = SDL_EVENT_KEY_DOWN; event.key.scancode = SDL_SCANCODE_W;
            input.ProcessEvent(event);
            if (!input.IsDown("KeyW") || !input.WasPressed("KeyW"))
                throw std::runtime_error("SDL keyboard mapping failed");
            input.EndFrame(); input.ProcessEvent(event);
            if (input.WasPressed("KeyW")) throw std::runtime_error("SDL keyboard repeat generated a new edge");
            event.type = SDL_EVENT_WINDOW_FOCUS_LOST; input.ProcessEvent(event);
            if (input.IsDown("KeyW")) throw std::runtime_error("SDL focus loss retained held input");
            const auto verifyKeyMapping = [&](const SDL_Scancode scancode, const char* code, const char* control)
            {
                SDL_Event keyEvent{};
                keyEvent.type = SDL_EVENT_KEY_DOWN; keyEvent.key.scancode = scancode;
                input.ProcessEvent(keyEvent);
                if (!input.IsDown(code) || !input.ControlWasPressed(control))
                    throw std::runtime_error("SDL extended keyboard press mapping failed");
                input.EndFrame(); keyEvent.type = SDL_EVENT_KEY_UP; input.ProcessEvent(keyEvent);
                if (input.IsDown(code) || !input.ControlWasReleased(control))
                    throw std::runtime_error("SDL extended keyboard release mapping failed");
                input.Reset();
            };
            verifyKeyMapping(SDL_SCANCODE_F1, "F1", "KeyboardF1");
            verifyKeyMapping(SDL_SCANCODE_F12, "F12", "KeyboardF12");
            verifyKeyMapping(SDL_SCANCODE_BACKSPACE, "Backspace", "KeyboardBackspace");
            verifyKeyMapping(SDL_SCANCODE_INSERT, "Insert", "KeyboardInsert");
            verifyKeyMapping(SDL_SCANCODE_HOME, "Home", "KeyboardHome");
            verifyKeyMapping(SDL_SCANCODE_END, "End", "KeyboardEnd");
            verifyKeyMapping(SDL_SCANCODE_PAGEUP, "PageUp", "KeyboardPageUp");
            verifyKeyMapping(SDL_SCANCODE_PAGEDOWN, "PageDown", "KeyboardPageDown");
            verifyKeyMapping(SDL_SCANCODE_LALT, "AltLeft", "KeyboardLeftAlt");
            verifyKeyMapping(SDL_SCANCODE_RALT, "AltRight", "KeyboardRightAlt");
            event = {}; event.type = SDL_EVENT_FINGER_DOWN;
            event.tfinger.fingerID = 1; event.tfinger.x = 0.1f; event.tfinger.y = 0.25f;
            input.ProcessEvent(event);
            int width{}, height{};
            SDL_GetWindowSizeInPixels(LamaPon::Native::GameWindow(), &width, &height);
            if (!input.PointerButtonDown(0) || !input.PointerButtonPressed(0)
                || std::abs(input.PointerX() - width * 0.1f) > 0.01f
                || std::abs(input.PointerY() - height * 0.25f) > 0.01f
                || input.TouchHorizontalAxis() > -0.6f || input.TouchVerticalAxis() < 0.49f)
                throw std::runtime_error("SDL touch coordinates or movement mapping failed");
            input.EndFrame();
            event.type = SDL_EVENT_FINGER_CANCELED; input.ProcessEvent(event);
            if (input.PointerButtonDown(0) || input.PointerButtonReleased(0) || input.PointerValid()
                || input.TouchHorizontalAxis() != 0 || input.TouchVerticalAxis() != 0)
                throw std::runtime_error("SDL canceled touch retained input or generated a click");
            event.type = SDL_EVENT_FINGER_DOWN; input.ProcessEvent(event);
            input.EndFrame(); event.type = SDL_EVENT_FINGER_UP; input.ProcessEvent(event);
            if (input.PointerButtonDown(0) || !input.PointerButtonReleased(0))
                throw std::runtime_error("SDL touch release did not generate a click edge");
            input.Reset();
            // Two physical touch devices can report the same finger ID.
            event = {}; event.type = SDL_EVENT_FINGER_DOWN;
            event.tfinger.touchID = 11; event.tfinger.fingerID = 1;
            event.tfinger.x = 0.1f; event.tfinger.y = 0.25f;
            input.ProcessEvent(event);
            input.EndFrame();
            event.tfinger.touchID = 22; event.tfinger.x = 0.9f; event.tfinger.y = 0.5f;
            input.ProcessEvent(event);
            if (!input.PointerButtonDown(0) || input.PointerButtonPressed(0)
                || std::abs(input.PointerX() - width * 0.1f) > 0.01f
                || input.TouchHorizontalAxis() > -0.6f || input.TouchAccelerateAxis() != 1)
                throw std::runtime_error("SDL touch devices with the same finger ID were merged");
            event.type = SDL_EVENT_FINGER_CANCELED;
            input.ProcessEvent(event);
            if (!input.PointerButtonDown(0) || !input.PointerValid() || input.PointerButtonReleased(0)
                || input.TouchHorizontalAxis() > -0.6f || input.TouchAccelerateAxis() != 0)
                throw std::runtime_error("Canceling another touch device canceled the primary pointer");
            event.type = SDL_EVENT_FINGER_UP; event.tfinger.touchID = 11;
            event.tfinger.x = 0.1f; event.tfinger.y = 0.25f;
            input.ProcessEvent(event);
            if (input.PointerButtonDown(0) || !input.PointerButtonReleased(0)
                || input.TouchHorizontalAxis() != 0 || input.TouchVerticalAxis() != 0)
                throw std::runtime_error("Releasing the primary touch device retained movement or lost release");
            input.Reset();
            event = {}; event.type = SDL_EVENT_KEY_DOWN; event.key.scancode = SDL_SCANCODE_AC_BACK;
            input.ProcessEvent(event);
            if (!input.ControlWasPressed("KeyboardEscape") || !input.IsDown("Escape"))
                throw std::runtime_error("SDL Android back did not map to UI cancel");
            input.EndFrame(); input.ProcessEvent(event);
            if (input.ControlWasPressed("KeyboardEscape"))
                throw std::runtime_error("SDL Android back repeat generated another cancel edge");
            event.type = SDL_EVENT_KEY_UP; input.ProcessEvent(event);
            if (input.IsDown("Escape") || !input.ControlWasReleased("KeyboardEscape"))
                throw std::runtime_error("SDL Android back release retained a cancel key");
            input.Reset();
            auto* image = Find("Green image");
            auto* button = Find("Button");
            auto* canvasObject = GetScene().FindGameObjectByName("Canvas");
            auto* canvas = canvasObject
                ? canvasObject->GetComponent<LamaPon::UICanvasComponent>()
                : nullptr;
            const auto canvasRectTransforms = canvasObject
                ? canvasObject->GetComponentsInChildren<
                    LamaPon::UIRectTransformComponent>()
                : std::vector<LamaPon::UIRectTransformComponent*>{};
            const auto* legacyCulling = Owner().GetComponent<LamaPon::RenderCullingComponent>();
            if (!Owner().CompareTag("native-probe")
                || FindWithTag("native-ui") != image
                || FindObjectsWithTag("native-ui").size() != 2
                || GetComponent<LamaPon::CameraComponent>()
                    != Owner().GetComponent<LamaPon::CameraComponent>()
                || GetComponentInParent<LamaPon::CameraComponent>()
                    != GetComponent<LamaPon::CameraComponent>()
                || &GetTransform() != &Owner().GetTransform()
                || canvas == nullptr || button == nullptr
                || button->GetComponentInParent<LamaPon::UICanvasComponent>() != canvas
                || canvasObject->GetComponentInChildren<LamaPon::UIButtonComponent>()
                    != button->GetComponent<LamaPon::UIButtonComponent>()
                || canvasRectTransforms.size() != 2
                || canvasObject->GetComponentsInParent<
                    LamaPon::UICanvasComponent>().size() != 1
                || Owner().GetScriptInChildren<NativeStartupProbe>() != this
                || Owner().GetComponentInChildren<LamaPon::CameraComponent>()
                    != Owner().GetComponent<LamaPon::CameraComponent>()
                || GetComponentsInChildren<
                    LamaPon::CameraComponent>().size() != 1
                || GetScriptInChildren<NativeStartupProbe>() != this
                || legacyCulling == nullptr
                || !legacyCulling->AlwaysVisible()
                || legacyCulling->CullingMargin() != 4.0f)
                throw std::runtime_error("Native scene tags or legacy object culling settings were not restored");
            bool rejectedParentCycle{};
            try { canvasObject->SetParent(button); }
            catch (const std::invalid_argument&) { rejectedParentCycle = true; }
            if (!rejectedParentCycle || canvasObject->Parent() != nullptr
                || button->Parent() != canvasObject)
                throw std::runtime_error("Native hierarchy accepted a parent cycle or changed the hierarchy after rejection");
            canvasObject->SetEnabled(false);
            const bool inactiveParentIsSkipped =
                button->GetComponentInParent<LamaPon::UICanvasComponent>() == nullptr;
            const bool inactiveParentCanBeIncluded =
                button->GetComponentInParent<LamaPon::UICanvasComponent>(true) == canvas;
            const bool inactiveChildrenAreSkipped =
                canvasObject->GetComponentInChildren<LamaPon::UIButtonComponent>() == nullptr
                && canvasObject->GetComponentsInChildren<
                    LamaPon::UIRectTransformComponent>().empty()
                && canvasObject->GetScriptInChildren<NativeStartupProbe>() == nullptr;
            const bool inactiveChildrenCanBeIncluded =
                canvasObject->GetComponentInChildren<
                    LamaPon::UIButtonComponent>(true)
                    == button->GetComponent<LamaPon::UIButtonComponent>()
                && canvasObject->GetComponentsInChildren<
                    LamaPon::UIRectTransformComponent>(true).size() == 2
                && canvasObject->GetScriptInChildren<
                    NativeStartupProbe>(true) == nullptr;
            canvasObject->SetEnabled(true);
            if (!inactiveParentIsSkipped || !inactiveParentCanBeIncluded
                || !inactiveChildrenAreSkipped || !inactiveChildrenCanBeIncluded)
                throw std::runtime_error("Native parent component search ignored hierarchy activation");
            auto& triggerA = GetScene().CreateGameObject(
                "Native trigger A");
            auto& triggerB = GetScene().CreateGameObject(
                "Native trigger B");
            triggerA.GetTransform().position = {20.0f, 0.0f, 0.0f};
            triggerB.GetTransform().position = {20.5f, 0.0f, 0.0f};
            auto& triggerCollider =
                triggerA.AddComponent<LamaPon::BoxCollider3DComponent>(
                    DirectX::XMFLOAT3{1, 1, 1}, DirectX::XMFLOAT3{});
            triggerCollider.SetTrigger(true);
            triggerB.AddComponent<LamaPon::BoxCollider3DComponent>();
            auto* contactA = dynamic_cast<NativeContactProbe*>(
                triggerA.AddComponent<LamaPon::NativeScriptComponent>(
                    "Test.NativeContactProbe").Instance());
            auto* contactB = dynamic_cast<NativeContactProbe*>(
                triggerB.AddComponent<LamaPon::NativeScriptComponent>(
                    "Test.NativeContactProbe").Instance());
            if (contactA == nullptr || contactB == nullptr)
                throw std::runtime_error("Native contact probes were not created");
            GetScene().FixedUpdate(0.0f);
            if (contactA->triggerEnter != 1 || contactB->triggerEnter != 1
                || contactA->collisionEnter != 0 || contactB->collisionEnter != 0
                || contactA->lastOther != "Native trigger B"
                || contactB->lastOther != "Native trigger A"
                || !contactA->lastTrigger || !contactB->lastTrigger)
                throw std::runtime_error("Native trigger enter callbacks lost their phase, type, or other GameObject");
            GetScene().FixedUpdate(0.0f);
            if (contactA->triggerStay != 1 || contactB->triggerStay != 1
                || contactA->triggerEnter != 1 || contactB->triggerEnter != 1)
                throw std::runtime_error("Native trigger stay callbacks did not follow the enter phase");
            triggerB.GetTransform().position.x = 25.5f;
            GetScene().FixedUpdate(0.0f);
            if (contactA->triggerExit != 1 || contactB->triggerExit != 1
                || contactA->lastOther != "Native trigger B"
                || contactB->lastOther != "Native trigger A"
                || !contactA->lastTrigger || !contactB->lastTrigger)
                throw std::runtime_error("Native trigger exit callbacks lost their other GameObject or trigger type");
            triggerCollider.SetTrigger(false);
            triggerB.GetTransform().position.x = 20.5f;
            GetScene().FixedUpdate(0.0f);
            if (contactA->collisionEnter != 1 || contactB->collisionEnter != 1
                || contactA->triggerEnter != 1 || contactB->triggerEnter != 1
                || contactA->lastTrigger || contactB->lastTrigger)
                throw std::runtime_error("Native collision enter callbacks were routed as triggers or lost their other GameObject");
            GetScene().FixedUpdate(0.0f);
            if (contactA->collisionStay != 1 || contactB->collisionStay != 1)
                throw std::runtime_error("Native collision stay callbacks did not follow the enter phase");
            triggerB.GetTransform().position.x = 25.5f;
            GetScene().FixedUpdate(0.0f);
            if (contactA->collisionExit != 1 || contactB->collisionExit != 1
                || contactA->lastOther != "Native trigger B"
                || contactB->lastOther != "Native trigger A"
                || contactA->lastTrigger || contactB->lastTrigger)
                throw std::runtime_error("Native collision exit callbacks lost their other GameObject or collision type");
            if (!image || !image->GetComponent<LamaPon::UIImageComponent>()
                || !button || !button->GetComponent<LamaPon::UIButtonComponent>())
                throw std::runtime_error("Native scene components were not loaded");
            auto* existingImage = GetScene().FindGameObjectByName("Green image");
            if (!existingImage
                || GetScene().Load("/assets/scenes/MalformedType.scene.json")
                || GetScene().FindGameObjectByName("Green image") != existingImage
                || GetScene().FindGameObjectByName("Malformed type probe") != nullptr)
                throw std::runtime_error("Malformed scene values must fail without changing the active scene");
            const auto activeScenePath = GetScene().Scenes().CurrentScenePath();
            const auto activeSceneRevision = GetScene().Scenes().LoadRevision();
            if (GetScene().Load(
                    "/assets/scenes/MalformedEnvironment.scene.json")
                || GetScene().Scenes().CurrentScenePath() != activeScenePath
                || GetScene().Scenes().LoadRevision() != activeSceneRevision
                || GetScene().Scenes().LastError().empty())
                throw std::runtime_error("Malformed scene environment changed the active scene camera, path, or revision");
            if (image->GetComponent<LamaPon::UIImageComponent>()->TexturePath().generic_u8string()
                != u8"scenes/白画像.bmp")
                throw std::runtime_error("Native scene changed a UTF-8 asset filename");
            probeTexture = LamaPon::Native::AssetTexture("/assets/scenes/白画像.bmp");
            if (!probeTexture)
                throw std::runtime_error("Native Unicode BMP asset did not decode and upload");
            bool rejectedRoot{};
            try { static_cast<void>(LamaPon::Native::ReadAsset("/assets//assets/scenes/白画像.bmp")); }
            catch (const std::invalid_argument&) { rejectedRoot = true; }
            if (!rejectedRoot) throw std::runtime_error("Rooted asset path escaped the selected asset directory");
            {
                LamaPon::Web::Renderer3D renderer;
                if (!renderer.Initialize("encoded-image-probe", 640, 360))
                    throw std::runtime_error("Cannot initialize the encoded image renderer probe");
                auto bytes = LamaPonTest::EncodedImage();
                encodedProbeTexture = renderer.CreateTextureEncoded(bytes);
                if (!encodedProbeTexture || renderer.CreateTextureEncoded({})
                    || renderer.CreateTextureEncoded({1, 2, 3}))
                    throw std::runtime_error("Encoded image creation accepted invalid data or rejected a valid image");
                std::fill(bytes.begin(), bytes.end(), 0); // The renderer must retain its own data.
            }
            if (!started)
            {
                const auto previous = LoadInteger("native-probe-starts", 0);
                SaveInteger("native-probe-starts", previous + 1);
                SaveText("native-probe-text", "保存テスト / Linux Android");
                SaveText("native-probe-empty", "");
                if (!LoadText("native-probe-empty", "fallback").empty())
                    throw std::runtime_error("A saved empty string must not become the fallback");
                if (LoadInteger("native-probe-starts") != previous + 1
                    || LoadText("native-probe-text") != "保存テスト / Linux Android")
                    throw std::runtime_error("Native save round trip failed");
                started = true;
            }
            for (int index = 0; index < 2; ++index)
            {
                auto& model = GetScene().CreateGameObject("Embedded texture probe");
                model.GetTransform().position = {index ? 1.5f : -1.5f, -1.5f, 0};
                model.AddComponent<LamaPon::ModelRendererComponent>(index
                    ? "models/embedded-buffer.glb" : "models/embedded-data.gltf");
            }
            auto& scenes = GetScene().Scenes();
            if (!scenes.State().Boolean("native-smoke-scene-reload-requested"))
            {
                if (scenes.CurrentScenePath().empty()
                    || !scenes.RequestReload()
                    || !scenes.HasPendingLoad())
                throw std::runtime_error("Native deferred scene reload was not accepted");
                scenes.State().SetBoolean(
                    "native-smoke-scene-reload-requested", true);
            }
        }

        void Update(float) override
        {
            const bool contactDestroyWasPending = m_contactDestroyPending;
            if (!m_sceneReloadChecked
                && GetScene().Scenes().State().Boolean(
                    "native-smoke-scene-reload-requested"))
            {
                const auto& scenes = GetScene().Scenes();
                if (scenes.LoadRevision() < 2 || scenes.HasPendingLoad()
                    || scenes.CurrentScenePath().empty()
                    || Find("Native probe") != &Owner())
                    throw std::runtime_error("Native deferred scene reload did not replace the active scene at an update boundary");
                m_sceneReloadChecked = true;
            }
            auto& scenes = GetScene().Scenes();
            auto& state = scenes.State();
            if (m_sceneReloadChecked
                && !state.Boolean("native-smoke-missing-scene-requested"))
            {
                state.SetInteger("native-smoke-scene-revision-before-failure",
                    static_cast<std::int64_t>(scenes.LoadRevision()));
                if (!scenes.RequestLoad("scenes/Missing.scene.json")
                    || !scenes.HasPendingLoad())
                    throw std::runtime_error("Native missing-scene request was not queued");
                state.SetBoolean("native-smoke-missing-scene-requested", true);
            }
            else if (state.Boolean("native-smoke-missing-scene-requested")
                && !m_sceneFailureChecked)
            {
                const auto revisionBeforeFailure = state.Integer(
                    "native-smoke-scene-revision-before-failure", -1);
                const auto revisionAfterFailure = scenes.LoadRevision();
                const bool activeProbePreserved =
                    Find("Native probe") == &Owner();
                if (scenes.HasPendingLoad()
                    || revisionAfterFailure != static_cast<std::uint64_t>(
                        revisionBeforeFailure)
                    || scenes.LastError().empty()
                    || !activeProbePreserved)
                    throw std::runtime_error(
                        "Failed native scene load changed active state: pending="
                        + std::to_string(scenes.HasPendingLoad())
                        + ", revision=" + std::to_string(revisionAfterFailure)
                        + "/" + std::to_string(revisionBeforeFailure)
                        + ", error=" + scenes.LastError()
                        + ", active=" + std::to_string(activeProbePreserved));
                m_sceneFailureChecked = true;
            }
            if (!m_prefabLifecycleChecked)
            {
                if (nativePrefabLifecycle.start != 2)
                    throw std::runtime_error("Native runtime Prefab scripts did not receive Start exactly once after Awake and OnEnable");
                m_prefabLifecycleChecked = true;
            }
            if (!m_activeTransitionTested)
            {
                Owner().SetEnabled(false);
                if (m_disableCalls != 1)
                    throw std::runtime_error("Native object deactivation did not call OnDisable immediately");
                Owner().SetEnabled(true);
                if (m_enableCalls != 2 || m_awakeCalls != 1)
                    throw std::runtime_error("Native object reactivation did not call OnEnable exactly once");
                m_activeTransitionTested = true;
            }
            if (contactDestroyWasPending)
            {
                if (Find("Native trigger A") != nullptr
                    || nativeContactLifecycleA.destroy != 1)
                    throw std::runtime_error("Native OnDestroy did not run before removing the trigger script");
                m_contactDestroyPending = false;
            }
            if (!m_contactLifecycleTested)
            {
                auto* trigger = Find("Native trigger A");
                auto* scriptComponent = trigger != nullptr
                    ? trigger->GetComponent<LamaPon::NativeScriptComponent>()
                    : nullptr;
                if (scriptComponent == nullptr
                    || nativeContactLifecycleA.awake != 1
                    || nativeContactLifecycleA.start != 1
                    || nativeContactLifecycleA.enable != 1)
                    throw std::runtime_error("Native contact script Awake, OnEnable, and Start order differs");
                scriptComponent->SetEnabled(false);
                if (nativeContactLifecycleA.disable != 1)
                    throw std::runtime_error("Disabling a native script component did not call OnDisable");
                scriptComponent->SetEnabled(true);
                if (nativeContactLifecycleA.enable != 2
                    || nativeContactLifecycleA.start != 1)
                    throw std::runtime_error("Re-enabling a native script component repeated Start or missed OnEnable");
                if (!GetScene().DestroyGameObject(*trigger)
                    || nativeContactLifecycleA.disable != 2)
                    throw std::runtime_error("Native object destruction did not disable its active script");
                m_contactLifecycleTested = true;
                m_contactDestroyPending = true;
            }
            if (m_updateFrames != 0 && !m_lateUpdateObserved)
            {
                throw std::runtime_error(
                    "Native LateUpdate was not called after the previous Update");
            }
            ++m_updateFrames;
            m_lateUpdateObserved = false;
        }

        void LateUpdate(float) override
        {
            m_lateUpdateObserved = true;
        }

    private:
        int m_updateFrames{};
        int m_awakeCalls{};
        int m_enableCalls{};
        int m_disableCalls{};
        bool m_activeTransitionTested{};
        bool m_contactLifecycleTested{};
        bool m_contactDestroyPending{};
        bool m_prefabLifecycleChecked{};
        bool m_lateUpdateObserved{};
        bool m_sceneReloadChecked{};
        bool m_sceneFailureChecked{};
    };
}

LAMAPON_SCRIPT_NAMED(NativeStartupProbe, "Test.NativeStartup", "Native startup probe");
LAMAPON_SCRIPT_NAMED(NativeContactProbe, "Test.NativeContactProbe", "Native contact probe");
LAMAPON_SCRIPT_NAMED(NativePrefabProbe, "Test.PortablePrefabProbe", "Native prefab lifecycle probe");

void LamaPonNativeFrameProbe(const int width, const int height)
{
    if (!started) throw std::runtime_error("Native startup probe did not run");
    ProbeEncodedTexture();
    std::array<unsigned char, 4> modelPixel{};
    glReadPixels(width * 35 / 100, height * 24 / 100, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, modelPixel.data());
    if (modelPixel[0] < 180 || modelPixel[1] > 60 || modelPixel[2] > 110)
        throw std::runtime_error("glTF data URI image did not reach the model framebuffer");
    glReadPixels(width * 65 / 100, height * 24 / 100, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, modelPixel.data());
    if (modelPixel[1] < 180 || modelPixel[0] > 90 || modelPixel[2] > 90)
        throw std::runtime_error("GLB bufferView image did not reach the model framebuffer");
    // Canvas reference coordinate (170, 180) is inside the green image.
    std::array<unsigned char, 4> pixel{};
    glReadPixels(width * 170 / 640, height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    if (pixel[1] < 220 || pixel[0] > 30 || pixel[2] > 30)
        throw std::runtime_error("Native UI framebuffer pixel differs from the scene");
    glReadPixels(width / 2, height * 3 / 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    if (pixel[2] < 200 || pixel[0] > 40 || pixel[1] > 40)
        throw std::runtime_error("Native 3D emissive mesh framebuffer pixel differs from the scene");
    static int frames{};
    static Uint64 backgroundTime{};
    static std::vector<unsigned char> textBeforeReset;
    // The same scene must render at a different resolution after SDL resize.
    if (++frames == 1 && !SDL_SetWindowSize(LamaPon::Native::GameWindow(), 960, 540))
        throw std::runtime_error("SDL window resize failed");
    if (frames == 8 && (width != 960 || height != 540))
        throw std::runtime_error("Native renderer did not observe the resized window");
    if (frames == 3)
    {
        backgroundTime = SDL_GetTicks();
        SDL_Event event{}; event.type = SDL_EVENT_DID_ENTER_BACKGROUND;
        if (!SDL_PushEvent(&event)) throw std::runtime_error("Cannot post background event");
        // A window restore must not override an application still in the background.
        event.type = SDL_EVENT_WINDOW_RESTORED;
        event.window.windowID = SDL_GetWindowID(LamaPon::Native::GameWindow());
        if (!SDL_PushEvent(&event)) throw std::runtime_error("Cannot post window restore event");
        if (!SDL_AddTimer(100, [](void*, SDL_TimerID, Uint32) -> Uint32 {
            SDL_Event foreground{}; foreground.type = SDL_EVENT_DID_ENTER_FOREGROUND;
            SDL_PushEvent(&foreground);
            return 0;
        }, nullptr)) throw std::runtime_error("Cannot schedule foreground event");
    }
    if (frames == 4 && SDL_GetTicks() - backgroundTime < 90)
        throw std::runtime_error("Native runtime rendered before application foreground restoration");
    if (frames == 5)
    {
        backgroundTime = SDL_GetTicks();
        SDL_Event event{}; event.type = SDL_EVENT_WINDOW_MINIMIZED;
        event.window.windowID = SDL_GetWindowID(LamaPon::Native::GameWindow());
        if (!SDL_PushEvent(&event)) throw std::runtime_error("Cannot post window minimize event");
        event.type = SDL_EVENT_DID_ENTER_FOREGROUND;
        if (!SDL_PushEvent(&event)) throw std::runtime_error("Cannot post foreground event");
        if (!SDL_AddTimer(100, [](void*, SDL_TimerID, Uint32) -> Uint32 {
            SDL_Event restored{}; restored.type = SDL_EVENT_WINDOW_RESTORED;
            restored.window.windowID = probeWindowId;
            SDL_PushEvent(&restored);
            return 0;
        }, nullptr)) throw std::runtime_error("Cannot schedule window restore event");
    }
    if (frames == 6 && SDL_GetTicks() - backgroundTime < 90)
        throw std::runtime_error("Native runtime rendered before window restoration");
    if (frames == 6 || frames == 8)
    {
        std::vector<unsigned char> text(200 * 40 * 4);
        glReadPixels(width / 2 - 100, height / 2 - 20, 200, 40, GL_RGBA, GL_UNSIGNED_BYTE, text.data());
        if (frames == 6)
        {
            int glyphPixels{};
            for (std::size_t index = 0; index < text.size(); index += 4)
                if (text[index] > 220 && text[index + 1] > 220 && text[index + 2] > 220) ++glyphPixels;
            if (glyphPixels < 10) throw std::runtime_error("Native button text did not rasterize");
            textBeforeReset = std::move(text);
        }
        else if (text != textBeforeReset)
            throw std::runtime_error("Native UI text pixels changed after GL context replacement");
    }
    if (frames == 7)
    {
        // A real unshared replacement context has no original buffers, programs
        // or textures. This exercises resource recovery, not just a reset flag.
        const auto previous = SDL_GL_GetCurrentContext();
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
        const auto replacement = SDL_GL_CreateContext(LamaPon::Native::GameWindow());
        if (!replacement || replacement == previous
            || !SDL_GL_MakeCurrent(LamaPon::Native::GameWindow(), replacement))
            throw std::runtime_error("Cannot replace the native test GL context");
        SDL_GL_DestroyContext(previous);
        SDL_Event event{}; event.type = SDL_EVENT_DID_ENTER_BACKGROUND;
        if (!SDL_PushEvent(&event)) throw std::runtime_error("Cannot post context-loss background event");
        event.type = SDL_EVENT_DID_ENTER_FOREGROUND;
        if (!SDL_PushEvent(&event)) throw std::runtime_error("Cannot post context-loss foreground event");
        event.type = SDL_EVENT_RENDER_DEVICE_RESET; event.render.windowID = probeWindowId;
        if (!SDL_PushEvent(&event)) throw std::runtime_error("Cannot post graphics reset event");
    }
    if (frames == 8 && LamaPon::Native::AssetTexture("/assets/scenes/白画像.bmp") != probeTexture)
        throw std::runtime_error("Graphics recovery changed a live texture handle");
}
