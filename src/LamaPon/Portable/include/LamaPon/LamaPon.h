#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace DirectX
{
    constexpr float XM_PI = 3.14159265358979323846f;

    struct XMFLOAT2 final
    {
        float x{};
        float y{};
    };

    struct XMFLOAT3 final
    {
        float x{};
        float y{};
        float z{};
    };

    struct XMFLOAT4 final
    {
        float x{};
        float y{};
        float z{};
        float w{};
    };

    struct XMFLOAT4X4 final
    {
        float _11{ 1.0f }, _12{}, _13{}, _14{};
        float _21{}, _22{ 1.0f }, _23{}, _24{};
        float _31{}, _32{}, _33{ 1.0f }, _34{};
        float _41{}, _42{}, _43{}, _44{ 1.0f };
    };

    using XMMATRIX = XMFLOAT4X4;

    inline void XMStoreFloat4x4(
        XMFLOAT4X4* destination,
        const XMMATRIX& source) noexcept
    {
        if (destination != nullptr)
        {
            *destination = source;
        }
    }
}

namespace LamaPon::Web
{
    class Renderer3D;
    class WebAudioRuntime;
    class WebInput;
}

namespace LamaPon
{
    class Component;
    class GameObject;
    class GraphicsDevice;
    class NativeScriptComponent;
    class Scene;
    class Script;

    class Logger final
    {
    public:
        [[nodiscard]] static Logger& Instance() noexcept
        {
            static Logger logger;
            return logger;
        }

        void Info(std::string_view) const noexcept {}
        void Warning(std::string_view) const noexcept {}
        void Error(std::string_view) const noexcept {}
    };

    enum class AudioBus : std::uint8_t
    {
        Master,
        Music,
        Effects,
        Ui,
        Count
    };

    using GameObjectId = std::uint64_t;

    enum class PrimitiveShape : std::uint8_t
    {
        Plane,
        Cube,
        Sphere,
        Cylinder,
    };

    enum class ShaderCullMode : std::uint8_t
    {
        Back,
        Front,
        None,
    };

    enum class ParticleEmitterShape : std::uint8_t
    {
        Cone,
        Sphere,
        Box,
        Point = Cone,
    };

    enum class ParticleRenderMode : std::uint8_t
    {
        Billboard,
        Horizontal,
    };

    enum class SpriteMaskShape : std::uint8_t
    {
        Rectangle,
        Circle,
    };

    enum class SpriteMaskInteraction : std::uint8_t
    {
        None,
        VisibleInsideMask,
        VisibleOutsideMask,
    };

    enum class TextHorizontalAlignment : std::uint8_t
    {
        Left,
        Center,
        Right,
    };

    enum class TextVerticalAlignment : std::uint8_t
    {
        Top,
        Center,
        Bottom,
    };

    struct ProceduralMeshVertex final
    {
        DirectX::XMFLOAT3 position{};
        DirectX::XMFLOAT3 normal{ 0.0f, 1.0f, 0.0f };
        DirectX::XMFLOAT2 textureCoordinate{};
    };

    struct CollisionEvent final
    {
        DirectX::XMFLOAT3 normal{};
        DirectX::XMFLOAT3 point{};
        float penetration{};
        bool isTrigger{};
    };

    struct Ray final
    {
        DirectX::XMFLOAT3 origin{};
        DirectX::XMFLOAT3 direction{ 0.0f, -1.0f, 0.0f };
    };

    struct PhysicsQueryFilter final
    {
        std::uint32_t layerMask{ 0xffffffffu };
        GameObjectId ignoredGameObjectId{};
    };

    struct PhysicsHit final
    {
        GameObject* gameObject{};
        DirectX::XMFLOAT3 point{};
        DirectX::XMFLOAT3 normal{};
        float distance{};
    };

    class Transform final
    {
    public:
        DirectX::XMFLOAT3 position{};
        DirectX::XMFLOAT3 rotation{};
        DirectX::XMFLOAT3 scale{ 1.0f, 1.0f, 1.0f };

        void SetEulerAngles(float pitch, float yaw, float roll) noexcept
        {
            rotation = { pitch, yaw, roll };
        }

    private:
        friend class GameObject;
        GameObject* m_owner{};
    };

    class Component
    {
    public:
        virtual ~Component() = default;

        [[nodiscard]] GameObject& Owner() const noexcept { return *m_owner; }
        [[nodiscard]] bool IsEnabled() const noexcept { return m_enabled; }
        void SetEnabled(bool enabled) noexcept { m_enabled = enabled; }

    protected:
        virtual void OnAttached() {}

    private:
        friend class GameObject;
        GameObject* m_owner{};
        bool m_enabled{ true };
    };

    class RuntimeState final
    {
    public:
        void SetNumber(std::string key, double value);
        void SetInteger(std::string key, std::int64_t value);
        void SetBoolean(std::string key, bool value);
        void SetString(std::string key, std::string value);

        [[nodiscard]] double Number(
            std::string_view key,
            double fallback = 0.0) const;
        [[nodiscard]] std::int64_t Integer(
            std::string_view key,
            std::int64_t fallback = 0) const;
        [[nodiscard]] bool Boolean(
            std::string_view key,
            bool fallback = false) const;
        [[nodiscard]] std::string String(
            std::string_view key,
            std::string fallback = {}) const;

    private:
        std::unordered_map<std::string, double> m_numbers;
        std::unordered_map<std::string, std::int64_t> m_integers;
        std::unordered_map<std::string, bool> m_booleans;
        std::unordered_map<std::string, std::string> m_strings;
    };

    class SceneCollection final
    {
    public:
        [[nodiscard]] RuntimeState& State() noexcept { return m_state; }
        [[nodiscard]] const RuntimeState& State() const noexcept
        {
            return m_state;
        }

    private:
        RuntimeState m_state;
    };

    enum class PointerButton : std::uint8_t
    {
        Left,
        Right,
        Middle,
        Extra1,
        Extra2,
        Count
    };

    struct InputPointerButtonState final
    {
        bool down{};
        bool pressed{};
        bool released{};
    };

    struct InputPointerState final
    {
        DirectX::XMFLOAT2 position{};
        bool valid{};
        bool down{};
        bool pressed{};
        bool released{};
        DirectX::XMFLOAT2 delta{};
        float wheel{};
        float wheelHorizontal{};
        std::array<
            InputPointerButtonState,
            static_cast<std::size_t>(PointerButton::Count)> buttons{};

        [[nodiscard]] const InputPointerButtonState& Button(
            PointerButton button) const noexcept
        {
            return buttons[static_cast<std::size_t>(button)];
        }
    };

    struct PortableKeyboardState final
    {
        bool Space{};
        bool R{};
    };

    class InputSystem final
    {
    public:
        void Bind(Web::WebInput* input) noexcept { m_input = input; }
        [[nodiscard]] float Value(std::string_view action) const;
        [[nodiscard]] bool WasPressed(std::string_view action) const;
        [[nodiscard]] bool IsDown(
            std::string_view action,
            float threshold = 0.5f) const
        {
            return std::abs(Value(action)) >= threshold;
        }
        [[nodiscard]] bool WasReleased(
            std::string_view action,
            float threshold = 0.5f) const;
        [[nodiscard]] const InputPointerState& Pointer() const noexcept;
        [[nodiscard]] const PortableKeyboardState& KeyboardState() const noexcept;
        void SetEdgeEventsEnabled(bool value) noexcept
        {
            m_edgeEventsEnabled = value;
        }

    private:
        Web::WebInput* m_input{};
        mutable InputPointerState m_pointer;
        mutable PortableKeyboardState m_keyboardState;
        bool m_edgeEventsEnabled{ true };
    };

    class GraphicsDevice final
    {
    public:
        [[nodiscard]] InputSystem& Input() noexcept { return m_input; }
        [[nodiscard]] const InputSystem& Input() const noexcept
        {
            return m_input;
        }
        [[nodiscard]] std::uint32_t UIWidth() const noexcept { return m_width; }
        [[nodiscard]] std::uint32_t UIHeight() const noexcept { return m_height; }
        void SetUiSize(std::uint32_t width, std::uint32_t height) noexcept
        {
            m_width = width;
            m_height = height;
        }

    private:
        InputSystem m_input;
        std::uint32_t m_width{ 1280 };
        std::uint32_t m_height{ 720 };
    };

    class Script
    {
    public:
        virtual ~Script() = default;
        virtual void Start() {}
        virtual void FixedUpdate(float) {}
        virtual void Update(float) {}
        virtual void OnCollisionEnter(const CollisionEvent&) {}
        virtual void OnCollisionStay(const CollisionEvent&) {}
        virtual void LoadProperties(std::string_view) {}
        [[nodiscard]] virtual std::string SaveProperties() const
        {
            return "{}";
        }

    protected:
        [[nodiscard]] Scene& GetScene() const noexcept;
        [[nodiscard]] GraphicsDevice& Graphics() const noexcept;
        [[nodiscard]] GameObject& Owner() const noexcept;
        [[nodiscard]] GameObject* Find(std::string_view name) const noexcept;
        bool Destroy(GameObject& gameObject);
        [[nodiscard]] std::string LoadText(
            std::string_view key,
            std::string fallback = {}) const;
        void SaveText(std::string_view key, std::string_view value) const;
        [[nodiscard]] std::int64_t LoadInteger(
            std::string_view key,
            std::int64_t fallback = 0) const;
        void SaveInteger(std::string_view key, std::int64_t value) const;

    private:
        friend class NativeScriptComponent;
        friend class Scene;
        GameObject* m_owner{};
        bool m_started{};
    };

    using ScriptFactory = std::function<std::unique_ptr<Script>()>;

    bool RegisterPortableScript(
        std::string id,
        std::string displayName,
        ScriptFactory factory);
    [[nodiscard]] std::unique_ptr<Script> CreatePortableScript(
        std::string_view id);

    class GameObject final
    {
    public:
        GameObject(Scene& scene, GameObjectId id, std::string name);

        [[nodiscard]] GameObjectId Id() const noexcept { return m_id; }
        [[nodiscard]] const std::string& Name() const noexcept { return m_name; }
        [[nodiscard]] Scene& GetScene() const noexcept { return *m_scene; }
        [[nodiscard]] Transform& GetTransform() noexcept { return m_transform; }
        [[nodiscard]] const Transform& GetTransform() const noexcept
        {
            return m_transform;
        }
        void SetParent(GameObject* parent) noexcept { m_parent = parent; }
        [[nodiscard]] GameObject* Parent() const noexcept { return m_parent; }
        [[nodiscard]] bool IsEnabled() const noexcept { return m_enabled; }
        void SetEnabled(bool enabled) noexcept;
        void SetTag(std::string tag) { m_tag = std::move(tag); }
        [[nodiscard]] const std::string& Tag() const noexcept { return m_tag; }

        [[nodiscard]] DirectX::XMMATRIX InterpolatedWorldMatrix(float) const;

        template<typename T, typename... Args>
        T& AddComponent(Args&&... args)
        {
            static_assert(std::is_base_of_v<Component, T>);
            auto component = std::make_unique<T>(std::forward<Args>(args)...);
            component->m_owner = this;
            T& reference = *component;
            m_components.push_back(std::move(component));
            static_cast<Component&>(reference).OnAttached();
            return reference;
        }

        template<typename T>
        [[nodiscard]] T* GetComponent() noexcept
        {
            for (const auto& component : m_components)
            {
                if (auto* value = dynamic_cast<T*>(component.get()))
                {
                    return value;
                }
            }
            return nullptr;
        }

        template<typename T>
        [[nodiscard]] const T* GetComponent() const noexcept
        {
            for (const auto& component : m_components)
            {
                if (auto* value = dynamic_cast<const T*>(component.get()))
                {
                    return value;
                }
            }
            return nullptr;
        }

        template<typename T>
        [[nodiscard]] T* GetScript() noexcept;

        [[nodiscard]] const std::vector<std::unique_ptr<Component>>& Components()
            const noexcept
        {
            return m_components;
        }

    private:
        Scene* m_scene{};
        GameObjectId m_id{};
        std::string m_name;
        std::string m_tag;
        Transform m_transform;
        GameObject* m_parent{};
        bool m_enabled{ true };
        std::vector<std::unique_ptr<Component>> m_components;
    };

    class MeshRendererComponent final : public Component
    {
    public:
        MeshRendererComponent(
            PrimitiveShape shape,
            DirectX::XMFLOAT4 color,
            std::filesystem::path albedo = {});

        void SetCullMode(ShaderCullMode mode) noexcept { m_cullMode = mode; }
        void SetRoughness(float value) noexcept { m_roughness = value; }
        void SetMetallic(float value) noexcept { m_metallic = value; }
        void SetNormalTexturePath(std::filesystem::path path)
        {
            m_normal = std::move(path);
        }
        void SetNormalStrength(float value) noexcept { m_normalStrength = value; }
        void SetRoughnessTexturePath(std::filesystem::path path)
        {
            m_roughnessTexture = std::move(path);
        }
        void SetMetallicTexturePath(std::filesystem::path path)
        {
            m_metallicTexture = std::move(path);
        }
        void SetOcclusionTexturePath(std::filesystem::path path)
        {
            m_occlusionTexture = std::move(path);
        }
        void SetOcclusionStrength(float value) noexcept
        {
            m_occlusionStrength = value;
        }
        void SetEmissiveTexturePath(std::filesystem::path path)
        {
            m_emissiveTexture = std::move(path);
        }
        void SetEmissiveColor(DirectX::XMFLOAT3 value) noexcept
        {
            m_emissiveColor = value;
        }
        void SetProceduralMesh(
            std::vector<ProceduralMeshVertex> vertices,
            std::vector<std::uint32_t> indices,
            bool recalculateNormals);

    private:
        friend class Scene;
        DirectX::XMFLOAT4 m_color{};
        std::filesystem::path m_albedo;
        std::filesystem::path m_normal;
        std::filesystem::path m_roughnessTexture;
        std::filesystem::path m_metallicTexture;
        std::filesystem::path m_occlusionTexture;
        std::filesystem::path m_emissiveTexture;
        ShaderCullMode m_cullMode{ ShaderCullMode::Back };
        float m_roughness{ 0.65f };
        float m_metallic{};
        float m_normalStrength{ 1.0f };
        float m_occlusionStrength{ 1.0f };
        DirectX::XMFLOAT3 m_emissiveColor{};
        std::vector<ProceduralMeshVertex> m_vertices;
        std::vector<std::uint32_t> m_indices;
        std::uint32_t m_webMesh{};
        std::uint32_t m_webTexture{};
        std::uint32_t m_webNormalTexture{};
        std::uint32_t m_webRoughnessTexture{};
        std::uint32_t m_webMetallicTexture{};
        std::uint32_t m_webOcclusionTexture{};
        std::uint32_t m_webEmissiveTexture{};
        bool m_dirty{ true };
    };

    class ModelRendererComponent final : public Component
    {
    public:
        explicit ModelRendererComponent(
            std::filesystem::path modelPath = {},
            bool wireframe = false,
            bool materialOverrideEnabled = false,
            DirectX::XMFLOAT4 color = { 1.0f, 1.0f, 1.0f, 1.0f },
            std::filesystem::path albedoTexture = {},
            std::filesystem::path normalTexture = {},
            float roughness = 0.5f,
            float normalStrength = 1.0f);

        void SetModelPath(std::filesystem::path path);
        [[nodiscard]] const std::filesystem::path& ModelPath() const noexcept
        {
            return m_modelPath;
        }
        void SetColor(const DirectX::XMFLOAT4& value) noexcept { m_color = value; }
        void SetAlbedoTexturePath(std::filesystem::path path)
        {
            m_albedoTexture = std::move(path);
        }
        void SetNormalTexturePath(std::filesystem::path path)
        {
            m_normalTexture = std::move(path);
        }
        void SetRoughness(float value) noexcept { m_roughness = value; }
        void SetNormalStrength(float value) noexcept { m_normalStrength = value; }
        void SetMetallic(float value) noexcept { m_metallic = value; }
        void SetRoughnessTexturePath(std::filesystem::path path)
        {
            m_roughnessTexture = std::move(path);
        }
        void SetMetallicTexturePath(std::filesystem::path path)
        {
            m_metallicTexture = std::move(path);
        }
        void SetOcclusionTexturePath(std::filesystem::path path)
        {
            m_occlusionTexture = std::move(path);
        }
        void SetOcclusionStrength(float value) noexcept
        {
            m_occlusionStrength = value;
        }
        void SetEmissiveTexturePath(std::filesystem::path path)
        {
            m_emissiveTexture = std::move(path);
        }
        void SetEmissiveColor(DirectX::XMFLOAT3 value) noexcept
        {
            m_emissiveColor = value;
        }
        void SetMaterialOverrideEnabled(bool value) noexcept
        {
            m_materialOverrideEnabled = value;
        }
        void SetAnimationIndex(std::size_t index) noexcept;
        [[nodiscard]] std::size_t AnimationIndex() const noexcept
        {
            return m_animationIndex;
        }
        [[nodiscard]] std::size_t AnimationCount() const noexcept
        {
            return m_animations.size();
        }
        [[nodiscard]] std::string_view AnimationName(
            std::size_t index) const noexcept;
        [[nodiscard]] float AnimationDuration() const noexcept;
        void SetAnimationSpeed(float value) noexcept { m_animationSpeed = value; }
        [[nodiscard]] float AnimationSpeed() const noexcept
        {
            return m_animationSpeed;
        }
        void SetAnimationLoop(bool value) noexcept { m_animationLoop = value; }
        [[nodiscard]] bool AnimationLoop() const noexcept
        {
            return m_animationLoop;
        }
        void SetAnimationPlayOnStart(bool value) noexcept
        {
            m_animationPlayOnStart = value;
        }
        [[nodiscard]] bool AnimationPlayOnStart() const noexcept
        {
            return m_animationPlayOnStart;
        }
        void PlayAnimation() noexcept { m_animationPlaying = true; }
        void PauseAnimation() noexcept { m_animationPlaying = false; }
        void StopAnimation() noexcept;
        void SetAnimationTime(float value) noexcept;
        [[nodiscard]] float AnimationTime() const noexcept
        {
            return m_animationTime;
        }
        [[nodiscard]] bool IsAnimationPlaying() const noexcept
        {
            return m_animationPlaying;
        }

    private:
        friend class Scene;

        struct ModelNode final
        {
            int parent{ -1 };
            DirectX::XMFLOAT3 translation{};
            DirectX::XMFLOAT4 rotation{ 0.0f, 0.0f, 0.0f, 1.0f };
            DirectX::XMFLOAT3 scale{ 1.0f, 1.0f, 1.0f };
            std::array<float, 16> matrix{};
            bool hasMatrix{};
        };

        struct ModelSkin final
        {
            std::vector<std::size_t> joints;
            std::vector<std::array<float, 16>> inverseBindMatrices;
        };

        struct ModelAnimationChannel final
        {
            std::size_t nodeIndex{};
            std::uint8_t path{};
            std::uint8_t interpolation{};
            std::vector<float> times;
            std::vector<DirectX::XMFLOAT4> values;
            std::vector<DirectX::XMFLOAT4> inTangents;
            std::vector<DirectX::XMFLOAT4> outTangents;
        };

        struct ModelAnimation final
        {
            std::string name;
            float duration{};
            std::vector<ModelAnimationChannel> channels;
        };

        struct Part final
        {
            std::vector<ProceduralMeshVertex> vertices;
            std::vector<ProceduralMeshVertex> bindVertices;
            std::vector<std::uint32_t> indices;
            std::vector<std::array<std::uint16_t, 4>> joints;
            std::vector<DirectX::XMFLOAT4> weights;
            DirectX::XMFLOAT4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
            std::filesystem::path albedoTexture;
            std::filesystem::path normalTexture;
            std::filesystem::path metallicRoughnessTexture;
            std::filesystem::path roughnessTexture;
            std::filesystem::path metallicTexture;
            std::filesystem::path occlusionTexture;
            std::filesystem::path emissiveTexture;
            float roughness{ 0.5f };
            float metallic{};
            float normalStrength{ 1.0f };
            float occlusionStrength{ 1.0f };
            DirectX::XMFLOAT3 emissiveColor{};
            DirectX::XMFLOAT3 dielectricSpecular{
                0.04f, 0.04f, 0.04f };
            bool unlit{};
            bool doubleSided{};
            bool alphaBlended{};
            float alphaCutoff{ -1.0f };
            int skinIndex{ -1 };
            std::size_t meshNodeIndex{};
            std::uint32_t webMesh{};
            std::uint32_t webTexture{};
            std::uint32_t webNormalTexture{};
            std::uint32_t webMetallicRoughnessTexture{};
            std::uint32_t webRoughnessTexture{};
            std::uint32_t webMetallicTexture{};
            std::uint32_t webOcclusionTexture{};
            std::uint32_t webEmissiveTexture{};
            bool dirty{ true };
        };

        [[nodiscard]] bool LoadPortableModel();
        void AdvancePortableAnimation(float deltaTime);
        void ApplyPortablePose();

        std::filesystem::path m_modelPath;
        DirectX::XMFLOAT4 m_color{ 1.0f, 1.0f, 1.0f, 1.0f };
        std::filesystem::path m_albedoTexture;
        std::filesystem::path m_normalTexture;
        std::filesystem::path m_roughnessTexture;
        std::filesystem::path m_metallicTexture;
        std::filesystem::path m_occlusionTexture;
        std::filesystem::path m_emissiveTexture;
        float m_roughness{ 0.5f };
        float m_metallic{};
        float m_normalStrength{ 1.0f };
        float m_occlusionStrength{ 1.0f };
        DirectX::XMFLOAT3 m_emissiveColor{};
        [[maybe_unused]] bool m_wireframe{};
        bool m_materialOverrideEnabled{};
        bool m_loaded{};
        std::vector<Part> m_parts;
        std::vector<ModelNode> m_nodes;
        std::vector<ModelNode> m_poseNodes;
        std::vector<std::array<float, 16>> m_nodeWorldMatrices;
        std::vector<ModelSkin> m_skins;
        std::vector<ModelAnimation> m_animations;
        std::size_t m_animationIndex{};
        float m_animationSpeed{ 1.0f };
        float m_animationTime{};
        bool m_animationLoop{ true };
        bool m_animationPlayOnStart{ true };
        bool m_animationPlaying{};
    };

    class MeshCollider3DComponent final : public Component
    {
    public:
        void SetLayer(std::uint32_t layer) noexcept { m_layer = layer; }
        void SetMesh(
            std::vector<DirectX::XMFLOAT3> vertices,
            std::vector<std::uint32_t> indices)
        {
            m_vertices = std::move(vertices);
            m_indices = std::move(indices);
        }

    private:
        friend class Scene;
        std::uint32_t m_layer{};
        std::vector<DirectX::XMFLOAT3> m_vertices;
        std::vector<std::uint32_t> m_indices;
    };

    class BoxCollider3DComponent final : public Component
    {
    public:
        BoxCollider3DComponent(
            DirectX::XMFLOAT3 size = { 1.0f, 1.0f, 1.0f },
            DirectX::XMFLOAT3 offset = {})
            : m_size(size), m_offset(offset)
        {
        }

        void SetLayer(std::uint32_t layer) noexcept { m_layer = layer; }
        void SetCollisionMask(std::uint32_t mask) noexcept { m_mask = mask; }
        void SetTrigger(bool value) noexcept { m_trigger = value; }

    private:
        friend class Scene;
        DirectX::XMFLOAT3 m_size{};
        DirectX::XMFLOAT3 m_offset{};
        std::uint32_t m_layer{};
        std::uint32_t m_mask{ 0xffffffffu };
        bool m_trigger{};
    };

    class RigidbodyComponent final : public Component
    {
    public:
        void SetKinematic(bool value) noexcept { m_kinematic = value; }
        void SetUseGravity(bool value) noexcept { m_useGravity = value; }
        void SetVelocity(DirectX::XMFLOAT3 value) noexcept { m_velocity = value; }
        [[nodiscard]] DirectX::XMFLOAT3 Velocity() const noexcept
        {
            return m_velocity;
        }

    private:
        friend class Scene;
        bool m_kinematic{};
        bool m_useGravity{ true };
        DirectX::XMFLOAT3 m_velocity{};
    };

    class CameraComponent final : public Component
    {
    public:
        void SetVerticalFieldOfView(float value) noexcept { m_fieldOfView = value; }
        void SetNearPlane(float value) noexcept { m_nearPlane = value; }
        void SetFarPlane(float value) noexcept { m_farPlane = value; }

    private:
        friend class Scene;
        float m_fieldOfView{ 1.0471976f };
        float m_nearPlane{ 0.1f };
        float m_farPlane{ 1500.0f };
    };

    class RotatorComponent final : public Component
    {
    public:
        explicit RotatorComponent(
            DirectX::XMFLOAT3 angularVelocity = { 0.0f, 1.0f, 0.0f }) noexcept
            : m_angularVelocity(angularVelocity)
        {
        }

        [[nodiscard]] const DirectX::XMFLOAT3& AngularVelocity() const noexcept
        {
            return m_angularVelocity;
        }
        void SetAngularVelocity(DirectX::XMFLOAT3 value) noexcept
        {
            m_angularVelocity = value;
        }

    private:
        friend class Scene;
        DirectX::XMFLOAT3 m_angularVelocity{};
    };

    class InputMoverComponent final : public Component
    {
    public:
        InputMoverComponent(
            std::string horizontalAction = "MoveHorizontal",
            std::string verticalAction = "MoveVertical",
            float speed = 3.0f)
            : m_horizontalAction(std::move(horizontalAction)),
              m_verticalAction(std::move(verticalAction)),
              m_speed(std::max(speed, 0.0f))
        {
        }

        void SetHorizontalAction(std::string value)
        {
            m_horizontalAction = std::move(value);
        }
        [[nodiscard]] const std::string& HorizontalAction() const noexcept
        {
            return m_horizontalAction;
        }
        void SetVerticalAction(std::string value)
        {
            m_verticalAction = std::move(value);
        }
        [[nodiscard]] const std::string& VerticalAction() const noexcept
        {
            return m_verticalAction;
        }
        void SetSpeed(float value) noexcept { m_speed = std::max(value, 0.0f); }
        [[nodiscard]] float Speed() const noexcept { return m_speed; }

    private:
        friend class Scene;
        std::string m_horizontalAction;
        std::string m_verticalAction;
        float m_speed{ 3.0f };
    };

    class RenderCullingComponent final : public Component
    {
    public:
        RenderCullingComponent(
            bool alwaysVisible = false,
            float cullingMargin = 0.0f) noexcept
            : m_alwaysVisible(alwaysVisible),
              m_cullingMargin(std::max(cullingMargin, 0.0f))
        {
        }

        void SetAlwaysVisible(bool value) noexcept { m_alwaysVisible = value; }
        [[nodiscard]] bool AlwaysVisible() const noexcept
        {
            return m_alwaysVisible;
        }
        void SetCullingMargin(float value) noexcept
        {
            m_cullingMargin = std::max(value, 0.0f);
        }
        [[nodiscard]] float CullingMargin() const noexcept
        {
            return m_cullingMargin;
        }

    private:
        [[maybe_unused]] bool m_alwaysVisible{};
        [[maybe_unused]] float m_cullingMargin{};
    };

    class ParticleSystemComponent final : public Component
    {
    public:
        ParticleSystemComponent(
            std::uint32_t capacity,
            float emissionRate,
            DirectX::XMFLOAT2 lifetime,
            DirectX::XMFLOAT2 speed,
            DirectX::XMFLOAT2 size,
            DirectX::XMFLOAT4 startColor,
            DirectX::XMFLOAT4 endColor,
            ParticleEmitterShape shape,
            std::filesystem::path texture = {});

        void SetGravity(DirectX::XMFLOAT3 gravity) noexcept { m_gravity = gravity; }
        void SetEndSizeMultiplier(float value) noexcept
        {
            m_endSizeMultiplier = value;
        }
        void SetRenderMode(ParticleRenderMode value) noexcept
        {
            m_renderMode = value;
        }
        void SetEmitterSize(DirectX::XMFLOAT3 value) noexcept
        {
            m_emitterSize = value;
        }
        void SetConeAngle(float value) noexcept { m_coneAngle = value; }
        void SetDuration(float value) noexcept { m_duration = value; }
        void SetLooping(bool value) noexcept { m_looping = value; }
        void SetPlayOnStart(bool value) noexcept { m_playing = value; }
        void SetAdditive(bool value) noexcept { m_additive = value; }
        void Emit(int count);
        void Stop(bool clearParticles = false);

    private:
        friend class Scene;
        struct Particle final
        {
            DirectX::XMFLOAT3 position{};
            DirectX::XMFLOAT3 velocity{};
            float age{};
            float lifetime{ 1.0f };
            float size{ 0.5f };
            float rotation{};
        };

        std::uint32_t m_capacity{};
        float m_emissionRate{};
        DirectX::XMFLOAT2 m_lifetime{};
        DirectX::XMFLOAT2 m_speed{};
        DirectX::XMFLOAT2 m_size{};
        DirectX::XMFLOAT4 m_startColor{};
        DirectX::XMFLOAT4 m_endColor{};
        ParticleEmitterShape m_shape{};
        ParticleRenderMode m_renderMode{};
        std::filesystem::path m_texture;
        DirectX::XMFLOAT3 m_gravity{};
        DirectX::XMFLOAT3 m_emitterSize{ 1.0f, 1.0f, 1.0f };
        float m_endSizeMultiplier{ 1.0f };
        float m_coneAngle{ 0.4363323f };
        float m_duration{ 5.0f };
        float m_emittingTime{};
        bool m_looping{ true };
        bool m_additive{ true };
        std::vector<Particle> m_particles;
        float m_emissionAccumulator{};
        bool m_playing{ true };
        std::uint32_t m_randomState{ 0x54524944u };
        std::uint32_t m_webMesh{};
        std::uint32_t m_webTexture{};
    };

    class AudioSourceComponent final : public Component
    {
    public:
        AudioSourceComponent(
            std::filesystem::path path,
            float volume = 1.0f,
            float pitch = 0.0f,
            float pan = 0.0f,
            bool loop = false,
            bool playOnStart = false,
            bool spatial = false,
            float minimumDistance = 1.0f,
            float maximumDistance = 20.0f)
            : m_path(std::move(path)), m_volume(volume), m_pitch(pitch),
              m_pan(pan), m_loop(loop), m_playOnStart(playOnStart),
              m_spatial(spatial), m_minimumDistance(minimumDistance),
              m_maximumDistance(maximumDistance)
        {
        }

        void SetBus(AudioBus value) noexcept { m_bus = value; }
        [[nodiscard]] AudioBus Bus() const noexcept { return m_bus; }
        void SetLoop(bool value) noexcept { m_loop = value; }
        void SetPitch(float value);
        void SetVolume(float value);
        void SetPan(float value);
        void SetSpatial(bool value) noexcept { m_spatial = value; }
        void SetMinimumDistance(float value) noexcept
        {
            m_minimumDistance = value;
        }
        void SetMaximumDistance(float value) noexcept
        {
            m_maximumDistance = value;
        }
        void Play();
        void PlayOneShot();
        void Stop();

    private:
        friend class Scene;
        std::filesystem::path m_path;
        float m_volume{ 1.0f };
        float m_pitch{};
        float m_pan{};
        bool m_loop{};
        bool m_playOnStart{};
        bool m_spatial{};
        float m_minimumDistance{ 1.0f };
        float m_maximumDistance{ 20.0f };
        AudioBus m_bus{ AudioBus::Effects };
        [[maybe_unused]] std::uint32_t m_handle{};
    };

    struct UIRect final
    {
        DirectX::XMFLOAT2 minimum{};
        DirectX::XMFLOAT2 maximum{};

        [[nodiscard]] DirectX::XMFLOAT2 Size() const noexcept
        {
            return {
                maximum.x - minimum.x,
                maximum.y - minimum.y
            };
        }
    };

    class UIRectTransformComponent final : public Component
    {
    public:
        explicit UIRectTransformComponent(
            DirectX::XMFLOAT2 anchorMin = { 0.5f, 0.5f },
            DirectX::XMFLOAT2 anchorMax = { 0.5f, 0.5f },
            DirectX::XMFLOAT2 pivot = { 0.5f, 0.5f },
            DirectX::XMFLOAT2 anchoredPosition = {},
            DirectX::XMFLOAT2 sizeDelta = { 220.0f, 56.0f }) noexcept;

        void SetAnchorMin(DirectX::XMFLOAT2 value) noexcept;
        void SetAnchorMax(DirectX::XMFLOAT2 value) noexcept;
        void SetPivot(DirectX::XMFLOAT2 value) noexcept;
        void SetAnchoredPosition(DirectX::XMFLOAT2 value) noexcept
        {
            m_anchoredPosition = value;
        }
        void SetSizeDelta(DirectX::XMFLOAT2 value) noexcept
        {
            m_sizeDelta = value;
        }
        [[nodiscard]] UIRect Resolve(
            float viewportWidth,
            float viewportHeight) const noexcept;

    private:
        DirectX::XMFLOAT2 m_anchorMin;
        DirectX::XMFLOAT2 m_anchorMax;
        DirectX::XMFLOAT2 m_pivot;
        DirectX::XMFLOAT2 m_anchoredPosition;
        DirectX::XMFLOAT2 m_sizeDelta;
    };

    class TransformAnimatorComponent final : public Component
    {
    public:
        explicit TransformAnimatorComponent(
            std::filesystem::path clipPath = {},
            float speed = 1.0f,
            bool loop = true,
            bool playOnStart = true)
            : m_clipPath(std::move(clipPath)),
              m_speed(speed),
              m_loop(loop),
              m_playOnStart(playOnStart)
        {
        }

        void SetSpeed(float value) noexcept { m_speed = value; }
        [[nodiscard]] float Speed() const noexcept { return m_speed; }
        void SetLoop(bool value) noexcept { m_loop = value; }
        [[nodiscard]] bool Loop() const noexcept { return m_loop; }
        void Play() noexcept { m_playing = !m_keyframes.empty(); }
        void Pause() noexcept { m_playing = false; }
        void Stop() noexcept
        {
            m_playing = false;
            m_time = 0.0f;
        }
        void SetTime(float value) noexcept;
        [[nodiscard]] float Time() const noexcept { return m_time; }
        [[nodiscard]] float Duration() const noexcept { return m_duration; }
        [[nodiscard]] bool IsPlaying() const noexcept { return m_playing; }

    private:
        friend class Scene;
        struct Keyframe final
        {
            float time{};
            DirectX::XMFLOAT3 position{};
            DirectX::XMFLOAT3 rotation{};
            DirectX::XMFLOAT3 scale{ 1.0f, 1.0f, 1.0f };
        };

        bool LoadPortableClip();
        void AdvancePortableAnimation(float deltaTime);
        void ApplyPortableSample();

        std::filesystem::path m_clipPath;
        float m_speed{ 1.0f };
        bool m_loop{ true };
        bool m_playOnStart{ true };
        bool m_playing{};
        float m_time{};
        float m_duration{};
        std::vector<Keyframe> m_keyframes;
    };

    class TextRendererComponent final : public Component
    {
    public:
        TextRendererComponent(
            std::string text,
            std::string fontFamily,
            float fontSize,
            DirectX::XMFLOAT4 color,
            DirectX::XMFLOAT2 bounds,
            bool wordWrap,
            TextHorizontalAlignment horizontal,
            TextVerticalAlignment vertical);

        void SetSortOrder(int value) noexcept { m_sortOrder = value; }
        void SetText(std::string value) { m_text = std::move(value); }
        void SetColor(DirectX::XMFLOAT4 value) noexcept { m_color = value; }
        void SetFontSize(float value) noexcept { m_fontSize = value; }
        void SetFontAsset(std::filesystem::path value)
        {
            m_fontAsset = std::move(value);
        }
        void SetLayoutSize(DirectX::XMFLOAT2 value) noexcept { m_bounds = value; }

    private:
        friend class Scene;
        std::string m_text;
        std::string m_fontFamily;
        std::filesystem::path m_fontAsset;
        float m_fontSize{};
        DirectX::XMFLOAT4 m_color{};
        DirectX::XMFLOAT2 m_bounds{};
        bool m_wordWrap{};
        TextHorizontalAlignment m_horizontal{};
        TextVerticalAlignment m_vertical{};
        int m_sortOrder{};
    };

    class SpriteMaskComponent final : public Component
    {
    public:
        explicit SpriteMaskComponent(
            SpriteMaskShape shape = SpriteMaskShape::Rectangle,
            DirectX::XMFLOAT2 size = { 128.0f, 128.0f }) noexcept
            : m_shape(shape), m_size(size)
        {
        }

        void SetShape(SpriteMaskShape value) noexcept { m_shape = value; }
        void SetSize(DirectX::XMFLOAT2 value) noexcept { m_size = value; }

    private:
        friend class Scene;
        SpriteMaskShape m_shape{};
        DirectX::XMFLOAT2 m_size{};
    };

    class SpriteRendererComponent final : public Component
    {
    public:
        SpriteRendererComponent(
            DirectX::XMFLOAT2 size,
            DirectX::XMFLOAT4 color,
            std::filesystem::path texture = {});

        void SetSize(DirectX::XMFLOAT2 value) noexcept { m_size = value; }
        void SetColor(DirectX::XMFLOAT4 value) noexcept { m_color = value; }
        void SetPivot(DirectX::XMFLOAT2 value) noexcept { m_pivot = value; }
        void SetSortOrder(int value) noexcept { m_sortOrder = value; }
        void SetMaskInteraction(SpriteMaskInteraction value) noexcept
        {
            m_maskInteraction = value;
        }
        void SetSourceRect(DirectX::XMFLOAT4 value) noexcept
        {
            m_sourceRect = value;
        }
        void SetTexturePath(std::filesystem::path value)
        {
            m_texture = std::move(value);
        }

    private:
        friend class Scene;
        DirectX::XMFLOAT2 m_size{};
        DirectX::XMFLOAT4 m_color{};
        std::filesystem::path m_texture;
        DirectX::XMFLOAT2 m_pivot{};
        DirectX::XMFLOAT4 m_sourceRect{ 0.0f, 0.0f, 1.0f, 1.0f };
        SpriteMaskInteraction m_maskInteraction{};
        int m_sortOrder{};
    };

    struct SpriteAnimationClip final
    {
        std::string name;
        int startFrame{};
        int frameCount{ 1 };
        float framesPerSecond{ 10.0f };
        bool loop{ true };
    };

    class SpriteAnimatorComponent final : public Component
    {
    public:
        SpriteAnimatorComponent(int columns = 1, int rows = 1) noexcept;
        void SetSheetGrid(int columns, int rows) noexcept;
        [[nodiscard]] int Columns() const noexcept { return m_columns; }
        [[nodiscard]] int Rows() const noexcept { return m_rows; }
        void AddClip(SpriteAnimationClip clip);
        void RemoveClip(std::string_view name);
        [[nodiscard]] std::vector<SpriteAnimationClip>& Clips() noexcept
        {
            return m_clips;
        }
        [[nodiscard]] const std::vector<SpriteAnimationClip>& Clips() const noexcept
        {
            return m_clips;
        }
        bool Play(std::string_view clipName);
        void Stop() noexcept { m_playing = false; }
        [[nodiscard]] bool IsPlaying() const noexcept { return m_playing; }
        [[nodiscard]] const std::string& ActiveClipName() const noexcept
        {
            return m_activeClip;
        }
        [[nodiscard]] int CurrentFrame() const noexcept
        {
            return m_currentFrame;
        }
        void SetSpeed(float value) noexcept { m_speed = value; }
        [[nodiscard]] float Speed() const noexcept { return m_speed; }
        void SetDefaultClip(std::string value)
        {
            m_defaultClip = std::move(value);
        }
        [[nodiscard]] const std::string& DefaultClip() const noexcept
        {
            return m_defaultClip;
        }
        void SetPlayOnStart(bool value) noexcept { m_playOnStart = value; }
        [[nodiscard]] bool PlayOnStart() const noexcept { return m_playOnStart; }

    private:
        friend class Scene;
        [[nodiscard]] const SpriteAnimationClip* FindClip(
            std::string_view name) const noexcept;
        void ApplyFrame(int frame);
        void Advance(float deltaTime);

        int m_columns{ 1 };
        int m_rows{ 1 };
        std::vector<SpriteAnimationClip> m_clips;
        std::string m_defaultClip;
        std::string m_activeClip;
        float m_speed{ 1.0f };
        float m_time{};
        int m_currentFrame{ -1 };
        bool m_playing{};
        bool m_playOnStart{ true };
        bool m_started{};
    };

    class ParallaxLayerComponent final : public Component
    {
    public:
        ParallaxLayerComponent(
            DirectX::XMFLOAT2 factor = { 0.5f, 0.5f },
            GameObjectId referenceId = 0) noexcept
            : m_factor(factor), m_referenceSourceId(referenceId)
        {
        }

        void SetFactor(DirectX::XMFLOAT2 value) noexcept { m_factor = value; }
        [[nodiscard]] const DirectX::XMFLOAT2& Factor() const noexcept
        {
            return m_factor;
        }
        void SetReferenceId(GameObjectId value) noexcept
        {
            m_referenceSourceId = value;
            m_reference = nullptr;
            m_initialized = false;
        }
        [[nodiscard]] GameObjectId ReferenceId() const noexcept
        {
            return m_referenceSourceId;
        }

    private:
        friend class Scene;
        void Advance(GameObject* mainCamera);

        DirectX::XMFLOAT2 m_factor{ 0.5f, 0.5f };
        GameObjectId m_referenceSourceId{};
        GameObject* m_reference{};
        DirectX::XMFLOAT2 m_referenceOrigin{};
        DirectX::XMFLOAT2 m_ownOrigin{};
        bool m_initialized{};
    };

    class NativeScriptComponent final : public Component
    {
    public:
        explicit NativeScriptComponent(std::string scriptId)
            : m_scriptId(std::move(scriptId))
        {
        }

        [[nodiscard]] Script* Instance() noexcept { return m_script.get(); }
        [[nodiscard]] const Script* Instance() const noexcept
        {
            return m_script.get();
        }

    protected:
        void OnAttached() override;

    private:
        std::string m_scriptId;
        std::unique_ptr<Script> m_script;
    };

    class Scene final
    {
    public:
        Scene(
            Web::Renderer3D& renderer,
            Web::WebAudioRuntime& audio,
            Web::WebInput& input);
        ~Scene();

        [[nodiscard]] GameObject& CreateGameObject(std::string name);
        [[nodiscard]] GameObject* FindGameObjectByName(
            std::string_view name) noexcept;
        bool DestroyGameObject(GameObject& gameObject);

        template<typename T>
        [[nodiscard]] T* FindComponentOfType() noexcept
        {
            for (const auto& object : m_objects)
            {
                if (auto* component = object->GetComponent<T>())
                {
                    return component;
                }
            }
            return nullptr;
        }

        [[nodiscard]] bool Load(const std::filesystem::path& virtualPath);
        void StartScripts();
        void FixedUpdate(float deltaTime);
        void Update(float deltaTime);
        void Render();
        void SetPhysicsInterpolationAlpha(float value) noexcept
        {
            m_interpolationAlpha = value;
        }
        [[nodiscard]] float PhysicsInterpolationAlpha() const noexcept
        {
            return m_interpolationAlpha;
        }
        [[nodiscard]] bool Raycast(
            const Ray& ray,
            float maximumDistance,
            PhysicsHit& hit,
            const PhysicsQueryFilter& filter = {}) const;
        [[nodiscard]] GraphicsDevice& Graphics() noexcept { return m_graphics; }
        [[nodiscard]] Web::WebAudioRuntime& WebAudio() noexcept
        {
            return *m_audio;
        }
        [[nodiscard]] SceneCollection& Scenes() noexcept { return m_scenes; }
        [[nodiscard]] const SceneCollection& Scenes() const noexcept
        {
            return m_scenes;
        }

    private:
        void FlushDestroyedObjects();

        struct Impl;
        std::unique_ptr<Impl> m_impl;
        Web::Renderer3D* m_renderer{};
        Web::WebAudioRuntime* m_audio{};
        GraphicsDevice m_graphics;
        SceneCollection m_scenes;
        std::vector<std::unique_ptr<GameObject>> m_objects;
        std::vector<GameObjectId> m_pendingDestroy;
        GameObjectId m_nextId{ 1 };
        float m_interpolationAlpha{};
    };

    template<typename T>
    T* GameObject::GetScript() noexcept
    {
        for (const auto& component : m_components)
        {
            auto* native = dynamic_cast<NativeScriptComponent*>(component.get());
            if (native == nullptr)
            {
                continue;
            }
            if (auto* script = dynamic_cast<T*>(native->Instance()))
            {
                return script;
            }
        }
        return nullptr;
    }
}

#define LAMAPON_PORTABLE_JOIN_DETAIL(left, right) left##right
#define LAMAPON_PORTABLE_JOIN(left, right) \
    LAMAPON_PORTABLE_JOIN_DETAIL(left, right)
#define LAMAPON_SCRIPT_NAMED(type, id, displayName)                         \
    namespace                                                              \
    {                                                                      \
        const bool LAMAPON_PORTABLE_JOIN(                                  \
            LamaPonPortableScriptRegistration_, __LINE__) =                \
            ::LamaPon::RegisterPortableScript(                             \
                id,                                                        \
                displayName,                                               \
                []() -> std::unique_ptr<::LamaPon::Script>                 \
                {                                                          \
                    return std::make_unique<type>();                        \
                });                                                        \
    }
#define LAMAPON_SCRIPT(type)                                                \
    LAMAPON_SCRIPT_NAMED(type, "Game." #type, #type)
#define LAMAPON_SCRIPT_WITH_SCHEMA(type, id, displayName, schema)           \
    LAMAPON_SCRIPT_NAMED(type, id, displayName)
