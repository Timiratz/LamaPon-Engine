#include "LamaPon/Input/InputSystem.h"

#include <Windows.h>
#include <objbase.h>

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    bool NearlyEqual(
        const float left,
        const float right,
        const float epsilon = 0.0001f)
    {
        return std::abs(left - right) <= epsilon;
    }
}

int main()
{
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(comResult);

    try
    {
        LamaPon::InputSystem input;
        input.SetActions(
            {
                {
                    "Move",
                    {
                        { LamaPon::InputControl::KeyboardA, -1.0f },
                        { LamaPon::InputControl::KeyboardD, 1.0f },
                        { LamaPon::InputControl::GamePadLeftX, 1.0f }
                    }
                },
                {
                    "Jump",
                    {
                        { LamaPon::InputControl::KeyboardSpace, 1.0f }
                    }
                }
            });

        LamaPon::InputSnapshot snapshot;
        snapshot.Set(
            LamaPon::InputControl::KeyboardA,
            1.0f);
        input.UpdateFromSnapshot(snapshot);
        Require(
            NearlyEqual(input.Value("Move"), -1.0f),
            "Negative binding scale was not applied.");
        Require(
            input.WasPressed("Move") && input.IsDown("Move"),
            "Action press transition was not detected.");

        input.UpdateFromSnapshot(snapshot);
        Require(
            !input.WasPressed("Move") && input.IsDown("Move"),
            "Held action state is incorrect.");

        LamaPon::InputSnapshot combined;
        combined.Set(
            LamaPon::InputControl::KeyboardD,
            1.0f);
        combined.Set(
            LamaPon::InputControl::GamePadLeftX,
            0.75f);
        input.UpdateFromSnapshot(combined);
        Require(
            NearlyEqual(input.Value("Move"), 1.0f),
            "Combined action value was not clamped.");

        input.UpdateFromSnapshot({});
        Require(
            input.WasReleased("Move")
                && !input.IsDown("Move"),
            "Action release transition was not detected.");

        input.SetPointerOverride(
            LamaPon::InputPointerState{
                { 320.0f, 180.0f },
                true,
                true });
        input.UpdateFromSnapshot({});
        Require(
            input.Pointer().valid
                && input.Pointer().pressed
                && input.Pointer().down
                && NearlyEqual(
                    input.Pointer().position.x,
                    320.0f),
            "Pointer press transition was not detected.");
        input.SetPointerOverride(
            LamaPon::InputPointerState{
                { 321.0f, 181.0f },
                true,
                true });
        input.UpdateFromSnapshot({});
        Require(
            !input.Pointer().pressed
                && input.Pointer().down,
            "Held pointer state is incorrect.");
        input.SetPointerOverride(
            LamaPon::InputPointerState{
                { 321.0f, 181.0f },
                true,
                false });
        input.UpdateFromSnapshot({});
        Require(
            input.Pointer().released
                && !input.Pointer().down,
            "Pointer release transition was not detected.");
        // 右クリックはボタン別状態（buttons配列）で検証します。
        LamaPon::InputPointerState rightPress{};
        rightPress.position = { 322.0f, 182.0f };
        rightPress.valid = true;
        rightPress.buttons[static_cast<std::size_t>(
            LamaPon::PointerButton::Right)].down = true;
        input.SetPointerOverride(rightPress);
        input.UpdateFromSnapshot({});
        Require(
            input.Pointer().Button(
                    LamaPon::PointerButton::Right)
                    .pressed
                && input.Pointer().Button(
                    LamaPon::PointerButton::Right)
                    .down,
            "Right pointer press transition was not detected.");
        LamaPon::InputPointerState rightRelease{};
        rightRelease.position = { 322.0f, 182.0f };
        rightRelease.valid = true;
        input.SetPointerOverride(rightRelease);
        input.UpdateFromSnapshot({});
        Require(
            input.Pointer().Button(
                    LamaPon::PointerButton::Right)
                    .released
                && !input.Pointer().Button(
                    LamaPon::PointerButton::Right)
                    .down,
            "Right pointer release transition was not detected.");

        input.SetActions(
            {
                {
                    "Fire",
                    {
                        { LamaPon::InputControl::MouseLeft, 1.0f }
                    }
                },
                {
                    "Zoom",
                    {
                        { LamaPon::InputControl::MouseWheelUp, 1.0f },
                        { LamaPon::InputControl::MouseWheelDown, -1.0f }
                    }
                }
            });
        LamaPon::InputSnapshot fire;
        fire.Set(
            LamaPon::InputControl::MouseLeft,
            1.0f);
        input.UpdateFromSnapshot(fire);
        Require(
            input.WasPressed("Fire")
                && input.IsDown("Fire"),
            "Mouse button binding was not applied.");
        LamaPon::InputSnapshot zoom;
        zoom.Set(
            LamaPon::InputControl::MouseWheelDown,
            1.0f);
        input.UpdateFromSnapshot(zoom);
        Require(
            NearlyEqual(input.Value("Zoom"), -1.0f),
            "Mouse wheel binding was not applied.");

        LamaPon::InputPointerState pointerOverride;
        pointerOverride.position = { 10.0f, 20.0f };
        pointerOverride.valid = true;
        pointerOverride.wheel = 1.0f;
        pointerOverride
            .buttons[static_cast<std::size_t>(
                LamaPon::PointerButton::Right)]
            .down = true;
        input.SetPointerOverride(pointerOverride);
        input.UpdateFromSnapshot({});
        Require(
            input.Pointer()
                    .Button(LamaPon::PointerButton::Right)
                    .down
                && input.Pointer()
                    .Button(LamaPon::PointerButton::Right)
                    .pressed,
            "Right button press transition was not detected.");
        Require(
            NearlyEqual(input.Pointer().wheel, 1.0f),
            "Pointer wheel value was not applied.");
        Require(
            !input.Pointer().down
                && !input.Pointer().pressed,
            "Right button state leaked into the left button.");
        pointerOverride
            .buttons[static_cast<std::size_t>(
                LamaPon::PointerButton::Right)]
            .down = false;
        pointerOverride.wheel = 0.0f;
        input.SetPointerOverride(pointerOverride);
        input.UpdateFromSnapshot({});
        Require(
            input.Pointer()
                    .Button(LamaPon::PointerButton::Right)
                    .released
                && !input.Pointer()
                    .Button(LamaPon::PointerButton::Right)
                    .down,
            "Right button release transition was not detected.");

        static_assert(
            LamaPon::IsKeyboardControl(
                LamaPon::InputControl::KeyboardB)
            && LamaPon::IsKeyboardControl(
                LamaPon::InputControl::KeyboardF12)
            && LamaPon::IsMouseControl(
                LamaPon::InputControl::MouseWheelDown)
            && !LamaPon::IsKeyboardControl(
                LamaPon::InputControl::MouseLeft)
            && LamaPon::IsGamePadControl(
                LamaPon::InputControl::GamePadRightTrigger));

        for (const auto control :
            LamaPon::AllInputControls())
        {
            Require(
                LamaPon::InputControlFromName(
                    LamaPon::InputControlName(control))
                    == control,
                "Input control name did not round-trip.");
        }

        bool duplicateRejected = false;
        try
        {
            input.SetActions(
                {
                    {
                        "Duplicate",
                        {
                            { LamaPon::InputControl::KeyboardA, 1.0f }
                        }
                    },
                    {
                        "Duplicate",
                        {
                            { LamaPon::InputControl::KeyboardD, 1.0f }
                        }
                    }
                });
        }
        catch (const std::invalid_argument&)
        {
            duplicateRejected = true;
        }
        Require(
            duplicateRejected,
            "Duplicate action names were accepted.");

        if (uninitialize)
        {
            CoUninitialize();
        }
        std::cout << "Input system tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        if (uninitialize)
        {
            CoUninitialize();
        }
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
