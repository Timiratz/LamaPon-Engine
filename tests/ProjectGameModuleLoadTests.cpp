#include "LamaPon/LamaPon.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>

int main(const int argumentCount, const char* const* argumentValues)
{
    try
    {
        if (argumentCount != 2)
        {
            throw std::invalid_argument(
                "A project Game Module path is required.");
        }
        LamaPon::GameModuleHost host;
        if (!host.Load(std::filesystem::path(argumentValues[1])))
        {
            throw std::runtime_error(host.LastError());
        }
        if (host.ModuleName() != "LamaPon Project Game Module"
            || host.RegisteredComponents().size() != 2
            || host.FindComponent("Test.ExternalScript") == nullptr
            || host.FindComponent("Game.BeginnerScript") == nullptr)
        {
            throw std::runtime_error(
                "The external project script was not registered.");
        }

        const auto* beginnerDescriptor =
            host.FindComponent("Game.BeginnerScript");
        if (beginnerDescriptor->propertiesSchemaJson == nullptr)
        {
            throw std::runtime_error(
                "The beginner Script schema was not registered.");
        }

        LamaPon::GraphicsDevice graphics;
        LamaPon::Scene scene(graphics);
        auto& object = scene.CreateGameObject("Beginner Script Test");
        object.AddComponent<LamaPon::NativeScriptComponent>(
            "Game.BeginnerScript");
        scene.Update(0.25f);
        if (object.GetTransform().position.x != 2.0f
            || object.GetTransform().position.y != 0.25f)
        {
            throw std::runtime_error(
                "The beginner Script lifecycle was not invoked.");
        }
        std::cout << "External project Game Module test passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
