#include "LamaPon/Animation/AnimationClip.h"
#include "LamaPon/Animation/AnimatorController.h"

#include <DirectXMath.h>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unordered_set>

namespace
{
    void Require(
        const bool condition,
        const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    bool NearlyEqual(
        const float left,
        const float right,
        const float tolerance = 0.0001f) noexcept
    {
        return std::abs(left - right)
            < tolerance;
    }
}

int main()
{
    try
    {
        const auto clip =
            LamaPon::AnimationClip::FromJson(
                R"({
                    "format":"LamaPonAnimationClip",
                    "version":1,
                    "name":"補間テスト",
                    "duration":2.0,
                    "loop":true,
                    "keyframes":[
                        {
                            "time":0.0,
                            "position":[0,0,0],
                            "rotation":[0,2.96705973,0],
                            "scale":[1,1,1]
                        },
                        {
                            "time":2.0,
                            "position":[4,2,-2],
                            "rotation":[0,-2.96705973,0],
                            "scale":[2,3,4]
                        }
                    ]
                })");

        Require(
            clip.Name() == "補間テスト"
                && NearlyEqual(
                    clip.Duration(),
                    2.0f)
                && clip.Loop()
                && clip.Keyframes().size() == 2,
            "Animation clip metadata was not parsed.");

        const auto middle =
            clip.Sample(1.0f);
        Require(
            NearlyEqual(middle.position.x, 2.0f)
                && NearlyEqual(middle.position.y, 1.0f)
                && NearlyEqual(middle.position.z, -1.0f)
                && NearlyEqual(middle.scale.x, 1.5f)
                && NearlyEqual(middle.scale.y, 2.0f)
                && NearlyEqual(middle.scale.z, 2.5f),
            "Transform keyframes were not linearly interpolated.");
        Require(
            NearlyEqual(
                std::abs(middle.rotation.y),
                DirectX::XM_PI,
                0.001f),
            "Rotation did not use the shortest angular path.");
        const auto middleRotation =
            clip.SampleRotationQuaternion(1.0f);
        Require(
            NearlyEqual(
                std::abs(
                    DirectX::XMVectorGetY(
                        DirectX::XMLoadFloat4(
                            &middleRotation))),
                1.0f,
                0.001f),
            "Quaternion rotation sampling did not use the"
            " shortest path.");

        const auto before =
            clip.Sample(-1.0f);
        const auto after =
            clip.Sample(10.0f);
        Require(
            NearlyEqual(before.position.x, 0.0f)
                && NearlyEqual(after.position.x, 4.0f),
            "Animation sampling did not clamp to endpoint keyframes.");

        bool invalidTimesRejected = false;
        try
        {
            static_cast<void>(
                LamaPon::AnimationClip::FromJson(
                    R"({
                        "format":"LamaPonAnimationClip",
                        "version":1,
                        "duration":1,
                        "keyframes":[
                            {"time":0,"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]},
                            {"time":0,"position":[1,0,0],"rotation":[0,0,0],"scale":[1,1,1]}
                        ]
                    })"));
        }
        catch (const std::exception&)
        {
            invalidTimesRejected = true;
        }
        Require(
            invalidTimesRejected,
            "Duplicate Animation keyframe times were accepted.");

        const auto editableClip =
            LamaPon::AnimationClip::Create(
                "Timeline保存",
                2.0f,
                false,
                clip.Keyframes());
        const auto serializedClip =
            LamaPon::AnimationClip::FromJson(
                editableClip.SerializeToJson());
        Require(
            serializedClip.Name()
                    == "Timeline保存"
                && !serializedClip.Loop()
                && serializedClip.Keyframes().size()
                    == 2,
            "Editable Animation Clip did not survive JSON serialization.");

        const auto outputPath =
            std::filesystem::current_path()
            / "test-output"
            / std::filesystem::path(
                L"タイムライン.animation.json");
        editableClip.SaveToFile(outputPath);
        const auto fileClip =
            LamaPon::AnimationClip::LoadFromFile(
                outputPath);
        Require(
            fileClip.Name() == "Timeline保存"
                && NearlyEqual(
                    fileClip.Duration(),
                    2.0f),
            "Animation Clip file save/load failed.");
        Require(
            !std::filesystem::exists(
                outputPath.wstring()
                    + L".tmp"),
            "Animation Clip temporary file remained after save.");

        const auto controller =
            LamaPon::AnimatorController::FromJson(
                R"({
                    "format":"LamaPonAnimatorController",
                    "version":1,
                    "entry":"Idle",
                    "states":[
                        {"name":"Idle","clip":"animations/idle.animation.json","speed":1,"loop":true},
                        {"name":"Run","clip":"animations/run.animation.json","modelClip":"Skeleton|Run","speed":1.5,"loop":true}
                    ],
                    "transitions":[
                        {"from":"Idle","to":"Run","trigger":"Run","exitTime":0.5,"duration":0.25},
                        {"from":"Run","to":"Idle","trigger":"","exitTime":0.9,"duration":0.4}
                    ]
                })");
        Require(
            controller.EntryState() == "Idle"
                && controller.States().size() == 2
                && controller.Transitions().size() == 2
                && controller.FindState("Run") != nullptr
                && controller.FindState("Run")->modelClip
                    == "Skeleton|Run",
            "Animator Controller metadata was not parsed.");
        const auto modelController =
            LamaPon::AnimatorController::FromJson(
                R"({
                    "format":"LamaPonAnimatorController",
                    "version":1,
                    "entry":"Idle",
                    "parameters":[
                        {"name":"Speed","type":"Float","default":0.25}
                    ],
                    "states":[
                        {
                            "name":"Idle",
                            "speed":1,
                            "loop":true,
                            "blendTree":{
                                "parameter":"Speed",
                                "children":[
                                    {"modelClip":"Skeleton|Run","threshold":1},
                                    {"modelClip":"Skeleton|Idle","threshold":0}
                                ]
                            },
                            "events":[
                                {"name":"Land","payload":"heavy","time":0.8},
                                {"name":"Footstep","payload":"left","time":0.25}
                            ]
                        }
                    ]
                })");
        Require(
            modelController.FindState("Idle") != nullptr
                && modelController.FloatParameters().size()
                    == 1
                && NearlyEqual(
                    modelController.FloatParameters()
                        .front().defaultValue,
                    0.25f)
                && modelController.FindState("Idle")
                    ->blendChildren.size() == 2
                && modelController.FindState("Idle")
                    ->blendChildren.front().modelClip
                    == "Skeleton|Idle"
                && modelController.FindState("Idle")
                    ->events.size() == 2
                && modelController.FindState("Idle")
                    ->events.front().name
                    == "Footstep",
            "1D Blend Tree or Animation Event metadata was not accepted or sorted.");
        const auto twoDimensionalController =
            LamaPon::AnimatorController::FromJson(
                R"({
                    "format":"LamaPonAnimatorController",
                    "version":1,
                    "entry":"Move",
                    "parameters":[
                        {"name":"MoveX","type":"Float","default":0},
                        {"name":"MoveY","type":"Float","default":0}
                    ],
                    "states":[
                        {
                            "name":"Move",
                            "blendTree":{
                                "type":"2D",
                                "parameterX":"MoveX",
                                "parameterY":"MoveY",
                                "children":[
                                    {"modelClip":"Idle","position":[0,0]},
                                    {"modelClip":"Forward","position":[0,1]},
                                    {"modelClip":"Right","position":[1,0]}
                                ]
                            }
                        }
                    ]
                })");
        const auto* moveState =
            twoDimensionalController.FindState(
                "Move");
        Require(
            moveState != nullptr
                && moveState->blendTreeType
                    == LamaPon::AnimatorBlendTreeType::
                        TwoDimensional
                && moveState->blendParameter
                    == "MoveX"
                && moveState->blendParameterY
                    == "MoveY"
                && moveState->blendChildren.size()
                    == 3
                && NearlyEqual(
                    moveState->blendChildren[1]
                        .positionY,
                    1.0f),
            "2D Blend Tree metadata was not accepted.");
        const auto exactWeights =
            LamaPon::AnimatorController::
                Calculate2DBlendWeights(
                    moveState->blendChildren,
                    1.0f,
                    0.0f);
        Require(
            exactWeights.size() == 3
                && NearlyEqual(
                    exactWeights[2],
                    1.0f)
                && NearlyEqual(
                    exactWeights[0]
                        + exactWeights[1],
                    0.0f),
            "2D Blend Tree exact-child weighting failed.");
        const auto mixedWeights =
            LamaPon::AnimatorController::
                Calculate2DBlendWeights(
                    moveState->blendChildren,
                    0.4f,
                    0.3f);
        Require(
            mixedWeights.size() == 3
                && NearlyEqual(
                    mixedWeights[0]
                        + mixedWeights[1]
                        + mixedWeights[2],
                    1.0f)
                && mixedWeights[0] > 0.0f
                && mixedWeights[1] > 0.0f
                && mixedWeights[2] > 0.0f,
            "2D Blend Tree weights were not normalized.");
        const std::unordered_set<std::string>
            runTrigger{ "Run" };
        Require(
            controller.FindTransition(
                "Idle",
                0.4f,
                runTrigger) == nullptr,
            "Animator transition ignored Exit Time.");
        const auto* runTransition =
            controller.FindTransition(
                "Idle",
                0.6f,
                runTrigger);
        Require(
            runTransition != nullptr
                && runTransition->to == "Run"
                && NearlyEqual(
                    runTransition->duration,
                    0.25f),
            "Animator Trigger transition was not selected.");
        Require(
            controller.FindTransition(
                "Run",
                0.95f,
                {}) != nullptr,
            "Animator automatic Exit Time transition was not selected.");

        bool invalidControllerRejected = false;
        try
        {
            static_cast<void>(
                LamaPon::AnimatorController::FromJson(
                    R"({
                        "format":"LamaPonAnimatorController",
                        "version":1,
                        "entry":"Missing",
                        "states":[
                            {"name":"Idle","clip":"idle.animation.json"}
                        ]
                    })"));
        }
        catch (const std::exception&)
        {
            invalidControllerRejected = true;
        }
        Require(
            invalidControllerRejected,
            "Animator Controller accepted a missing Entry State.");

        std::cout
            << "Animation clip tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr
            << "Animation clip tests failed: "
            << exception.what()
            << '\n';
        return 1;
    }
}
