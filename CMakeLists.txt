cmake_minimum_required(VERSION 3.25)

project(LamaPon VERSION 0.1.0 LANGUAGES CXX)

include(cmake/LamaPonMsvcDependencies.cmake)
lamapon_configure_msvc_dependencies()

set(LAMAPON_BUILD_REVISION "unknown")
find_package(Git QUIET)
if(Git_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND
            "${GIT_EXECUTABLE}"
            -C "${CMAKE_CURRENT_SOURCE_DIR}"
            rev-parse --short=12 HEAD
        OUTPUT_VARIABLE LAMAPON_GIT_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE LAMAPON_GIT_REVISION_RESULT
        ERROR_QUIET
    )
    if(LAMAPON_GIT_REVISION_RESULT EQUAL 0
       AND NOT LAMAPON_GIT_REVISION STREQUAL "")
        set(LAMAPON_BUILD_REVISION "${LAMAPON_GIT_REVISION}")
        execute_process(
            COMMAND
                "${GIT_EXECUTABLE}"
                -C "${CMAKE_CURRENT_SOURCE_DIR}"
                status --porcelain
            OUTPUT_VARIABLE LAMAPON_GIT_STATUS
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE LAMAPON_GIT_STATUS_RESULT
            ERROR_QUIET
        )
        if(LAMAPON_GIT_STATUS_RESULT EQUAL 0
           AND NOT LAMAPON_GIT_STATUS STREQUAL "")
            string(APPEND LAMAPON_BUILD_REVISION "-dirty")
        endif()
    endif()
endif()

configure_file(
    cmake/Version.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/LamaPon/Core/Version.h"
    @ONLY
)
configure_file(
    cmake/BuildInfo.json.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/build-info.json"
    @ONLY
)

if(NOT WIN32)
    message(FATAL_ERROR "LamaPon currently targets Windows and DirectX 11.")
endif()

enable_language(RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# LamaPonRuntimeは失敗をC++例外で通知します。MSVCの既定フラグを
# 外部ビルドが上書きしても例外処理が強制終了へ変わらないよう、
# エンジン側の必須条件として明示します。
if(MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(
    LAMAPON_PACKAGE_SYMBOLS
    "Generate and package full PDB files for Release binaries"
    ON
)

function(lamapon_enable_release_symbols target)
    if(MSVC AND LAMAPON_PACKAGE_SYMBOLS)
        target_compile_options(
            ${target}
            PRIVATE
                $<$<CONFIG:Release>:/Zi>
        )
        get_target_property(LAMAPON_SYMBOL_TARGET_TYPE
            ${target}
            TYPE
        )
        if(NOT LAMAPON_SYMBOL_TARGET_TYPE STREQUAL "STATIC_LIBRARY"
           AND NOT LAMAPON_SYMBOL_TARGET_TYPE STREQUAL "OBJECT_LIBRARY"
           AND NOT LAMAPON_SYMBOL_TARGET_TYPE STREQUAL "INTERFACE_LIBRARY")
            target_link_options(
                ${target}
                PRIVATE
                    $<$<CONFIG:Release>:/DEBUG:FULL>
            )
        endif()
    endif()
endfunction()

include(cmake/DirectXTK.cmake)

add_library(LamaPonJson INTERFACE)
target_include_directories(LamaPonJson INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann
)

add_library(Ufbx STATIC
    third_party/ufbx/ufbx.cpp
)
target_include_directories(Ufbx PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ufbx
)
target_compile_definitions(Ufbx PUBLIC
    UFBX_REAL_IS_FLOAT
)
target_compile_options(Ufbx PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W3>
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    $<$<CXX_COMPILER_ID:MSVC>:/wd4244>
    $<$<CXX_COMPILER_ID:MSVC>:/wd4267>
    $<$<CXX_COMPILER_ID:MSVC>:/wd4996>
)

add_library(DearImGui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    # UIを記録するテストエンジンフックはimgui.objから参照されます。
    # LamaPonHubでもリンクできるよう、実装はこのターゲットに置きます。
    src/LamaPon/Editor/UiRecorder.cpp
    src/LamaPon/Editor/UiRecorder.h
)
target_include_directories(DearImGui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends
)
target_include_directories(DearImGui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(DearImGui PUBLIC
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    # ウィジェット登録時にImGuiTestEngineHook_*を呼びます。記録の有無は
    # 実行時フラグで制御します（UiRecorder.cppを参照）。
    IMGUI_ENABLE_TEST_ENGINE
)
target_compile_options(DearImGui PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W3>
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)
target_link_libraries(DearImGui PRIVATE d3d11)

add_library(ImGuizmo STATIC
    third_party/ImGuizmo/src/ImGuizmo.cpp
)
target_include_directories(ImGuizmo PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ImGuizmo/src
)
target_link_libraries(ImGuizmo PUBLIC DearImGui)
target_compile_options(ImGuizmo PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W3>
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

set(LAMAPON_RUNTIME_SOURCES
    src/LamaPon/Animation/AnimationClip.cpp
    src/LamaPon/Animation/AnimationClip.h
    src/LamaPon/Animation/AnimatorController.cpp
    src/LamaPon/Animation/AnimatorController.h
    src/LamaPon/Assets/AssetArchive.cpp
    src/LamaPon/Assets/AssetArchive.h
    src/LamaPon/Assets/AssetDatabase.cpp
    src/LamaPon/Assets/AssetDatabase.h
    src/LamaPon/Assets/AssetImporter.cpp
    src/LamaPon/Assets/AssetImporter.h
    src/LamaPon/Assets/AssetPacker.cpp
    src/LamaPon/Assets/AssetPacker.h
    src/LamaPon/Assets/DataAsset.cpp
    src/LamaPon/Assets/DataAsset.h
    src/LamaPon/Assets/GltfImporter.cpp
    src/LamaPon/Assets/GltfImporter.h
    src/LamaPon/Assets/AssetManager.cpp
    src/LamaPon/Assets/AssetManager.h
    src/LamaPon/Assets/FbxImporter.cpp
    src/LamaPon/Assets/FbxImporter.h
    src/LamaPon/Assets/CollisionMeshImporter.cpp
    src/LamaPon/Assets/CollisionMeshImporter.h
    src/LamaPon/Assets/ModelCache.cpp
    src/LamaPon/Assets/ModelCache.h
    src/LamaPon/Assets/TextureCache.cpp
    src/LamaPon/Assets/TextureCache.h
    src/LamaPon/Assets/TextureLoader.cpp
    src/LamaPon/Assets/TextureLoader.h
    src/LamaPon/Audio/AudioSystem.cpp
    src/LamaPon/Audio/AudioSystem.h
    src/LamaPon/Components/AudioListenerComponent.cpp
    src/LamaPon/Components/AudioListenerComponent.h
    src/LamaPon/Components/AudioSourceComponent.cpp
    src/LamaPon/Components/AudioSourceComponent.h
    src/LamaPon/Components/BillboardComponent.cpp
    src/LamaPon/Components/BillboardComponent.h
    src/LamaPon/Components/BoxCollider2DComponent.cpp
    src/LamaPon/Components/BoxCollider2DComponent.h
    src/LamaPon/Components/BoxCollider3DComponent.cpp
    src/LamaPon/Components/BoxCollider3DComponent.h
    src/LamaPon/Components/PolygonCollider2DComponent.cpp
    src/LamaPon/Components/PolygonCollider2DComponent.h
    src/LamaPon/Components/Light2DComponent.cpp
    src/LamaPon/Components/Light2DComponent.h
    src/LamaPon/Components/SpriteMaskComponent.cpp
    src/LamaPon/Components/SpriteMaskComponent.h
    src/LamaPon/Components/ParallaxLayerComponent.cpp
    src/LamaPon/Components/ParallaxLayerComponent.h
    src/LamaPon/Components/InputMoverComponent.cpp
    src/LamaPon/Components/InputMoverComponent.h
    src/LamaPon/Components/JointComponent.cpp
    src/LamaPon/Components/JointComponent.h
    src/LamaPon/Components/LODGroupComponent.cpp
    src/LamaPon/Components/LODGroupComponent.h
    src/LamaPon/Components/RotatorComponent.cpp
    src/LamaPon/Components/RotatorComponent.h
    src/LamaPon/Components/RigidbodyComponent.cpp
    src/LamaPon/Components/RigidbodyComponent.h
    src/LamaPon/Components/NativeScriptComponent.cpp
    src/LamaPon/Components/NativeScriptComponent.h
    src/LamaPon/Core/Application.cpp
    src/LamaPon/Core/RuntimeServices.cpp
    src/LamaPon/Core/Application.h
    src/LamaPon/Core/CrashReporter.cpp
    src/LamaPon/Core/CrashReporter.h
    src/LamaPon/Core/CrashSentinel.cpp
    src/LamaPon/Core/CrashSentinel.h
    src/LamaPon/Core/DebugOverlay.cpp
    src/LamaPon/Core/DebugOverlay.h
    src/LamaPon/Core/JobSystem.cpp
    src/LamaPon/Core/JobSystem.h
    src/LamaPon/Core/Crypto.cpp
    src/LamaPon/Core/Crypto.h
    src/LamaPon/Core/DocumentMigration.cpp
    src/LamaPon/Core/DocumentMigration.h
    src/LamaPon/Core/HttpClient.cpp
    src/LamaPon/Core/HttpClient.h
    src/LamaPon/Core/VersionCompare.cpp
    src/LamaPon/Core/VersionCompare.h
    src/LamaPon/Core/Log.cpp
    src/LamaPon/Core/Log.h
    src/LamaPon/Core/Noise.h
    src/LamaPon/Core/Profiler.cpp
    src/LamaPon/Core/Profiler.h
    src/LamaPon/Core/ApplicationLayer.h
    src/LamaPon/Core/PathUtils.h
    src/LamaPon/Core/ProjectInstance.cpp
    src/LamaPon/Core/ProjectInstance.h
    src/LamaPon/Core/ProjectMigration.cpp
    src/LamaPon/Core/ProjectMigration.h
    src/LamaPon/Core/PlayerPrefs.cpp
    src/LamaPon/Core/PlayerPrefs.h
    src/LamaPon/Core/SaveData.cpp
    src/LamaPon/Core/SaveData.h
    src/LamaPon/Core/ProjectSettings.cpp
    src/LamaPon/Core/ProjectSettings.h
    src/LamaPon/Core/Time.cpp
    src/LamaPon/Core/Time.h
    src/LamaPon/Core/Window.cpp
    src/LamaPon/Core/Window.h
    src/LamaPon/Graphics/GpuProfiler.cpp
    src/LamaPon/Graphics/GpuProfiler.h
    src/LamaPon/Graphics/GraphicsDevice.cpp
    src/LamaPon/Graphics/GraphicsDevice.h
    src/LamaPon/Graphics/Lighting.cpp
    src/LamaPon/Graphics/Lighting.h
    src/LamaPon/Graphics/LitEffect.cpp
    src/LamaPon/Graphics/LitEffect.h
    src/LamaPon/Graphics/LitMaterial.h
    src/LamaPon/Graphics/LitMaterialAsset.cpp
    src/LamaPon/Graphics/LitMaterialAsset.h
    src/LamaPon/Graphics/PbrTextures.h
    src/LamaPon/Graphics/PngWriter.cpp
    src/LamaPon/Graphics/PngWriter.h
    src/LamaPon/Graphics/DebugRenderer.cpp
    src/LamaPon/Graphics/DebugRenderer.h
    src/LamaPon/Graphics/EnvironmentCache.cpp
    src/LamaPon/Graphics/EnvironmentCache.h
    src/LamaPon/Graphics/EnvironmentRenderer.cpp
    src/LamaPon/Graphics/EnvironmentRenderer.h
    src/LamaPon/Graphics/EnvironmentSettings.h
    src/LamaPon/Graphics/GraphicsQuality.cpp
    src/LamaPon/Graphics/GraphicsQuality.h
    src/LamaPon/Components/ReflectionProbeComponent.cpp
    src/LamaPon/Components/ReflectionProbeComponent.h
    src/LamaPon/Components/RenderCullingComponent.h
    src/LamaPon/Graphics/ClusteredLights.cpp
    src/LamaPon/Graphics/ClusteredLights.h
    src/LamaPon/Graphics/ComputeEffect.cpp
    src/LamaPon/Graphics/ComputeEffect.h
    src/LamaPon/Graphics/TemporalJitter.cpp
    src/LamaPon/Graphics/TemporalJitter.h
    src/LamaPon/Graphics/RenderPipeline.cpp
    src/LamaPon/Graphics/RenderPipeline.h
    src/LamaPon/Graphics/ReflectionProbeEnvironment.h
    src/LamaPon/Graphics/RenderTarget.cpp
    src/LamaPon/Graphics/RenderTarget.h
    src/LamaPon/Graphics/ScreenEffect.cpp
    src/LamaPon/Graphics/ScreenEffect.h
    src/LamaPon/Graphics/ShaderRenderState.cpp
    src/LamaPon/Graphics/ShaderRenderState.h
    src/LamaPon/Graphics/ShadowMap.cpp
    src/LamaPon/Graphics/ShadowMap.h
    src/LamaPon/Graphics/ShaderCompiler.cpp
    src/LamaPon/Graphics/ShaderCompiler.h
    src/LamaPon/Graphics/ShaderDiagnostics.cpp
    src/LamaPon/Graphics/ShaderDiagnostics.h
    src/LamaPon/Graphics/ShaderVariants.cpp
    src/LamaPon/Graphics/ShaderVariants.h
    src/LamaPon/Graphics/SpriteEffect.cpp
    src/LamaPon/Graphics/SpriteEffect.h
    src/LamaPon/Graphics/SkeletalModel.cpp
    src/LamaPon/Graphics/SkeletalModel.h
    src/LamaPon/Graphics/TextLayout.h
    src/LamaPon/Input/InputSystem.cpp
    src/LamaPon/Input/InputSystem.h
    src/LamaPon/Physics/CollisionTypes.h
    src/LamaPon/Physics/CollisionMesh.cpp
    src/LamaPon/Physics/CollisionMesh.h
    src/LamaPon/Physics/Collision3D.cpp
    src/LamaPon/Physics/Collision3D.h
    src/LamaPon/Physics/PhysicsMaterial.h
    src/LamaPon/Physics/PhysicsQuery.h
    src/LamaPon/Physics/PhysicsSettings.cpp
    src/LamaPon/Physics/PhysicsFrameClock.cpp
    src/LamaPon/Physics/PhysicsFrameClock.h
    src/LamaPon/Physics/PhysicsSettings.h
    src/LamaPon/Physics/Raycast.cpp
    src/LamaPon/Physics/Raycast.h
    src/LamaPon/Physics/SpatialHashBroadPhase.cpp
    src/LamaPon/Physics/SpatialHashBroadPhase.h
    src/LamaPon/Reactive/Reactive.cpp
    src/LamaPon/Reactive/Reactive.h
    src/LamaPon/Scene/Component.cpp
    src/LamaPon/Scene/Component.h
    src/LamaPon/Scene/EventBus.cpp
    src/LamaPon/Scene/EventBus.h
    src/LamaPon/Scene/GameObject.cpp
    src/LamaPon/Scene/GameObject.h
    src/LamaPon/Scene/Scene.cpp
    src/LamaPon/Scene/ScenePhysics.cpp
    src/LamaPon/Scene/Scene.h
    src/LamaPon/Scene/SceneSerialization.cpp
    src/LamaPon/Scene/SceneManager.cpp
    src/LamaPon/Scene/SceneManager.h
    src/LamaPon/Scene/SceneVisibility.cpp
    src/LamaPon/Scene/RenderSpatialIndex.cpp
    src/LamaPon/Scene/RuntimeGameState.cpp
    src/LamaPon/Scene/RuntimeGameState.h
    src/LamaPon/Scene/Transform.h
    src/LamaPon/Scripting/Coroutine.h
    src/LamaPon/Scripting/GameModule.cpp
    src/LamaPon/Scripting/GameModule.h
    src/LamaPon/Scripting/GameModuleHost.cpp
    src/LamaPon/Scripting/GameModuleHost.h
    src/LamaPon/Scripting/Script.h
    src/LamaPon/Components/CameraComponent.cpp
    src/LamaPon/Components/CameraComponent.h
    src/LamaPon/Components/CapsuleCollider3DComponent.cpp
    src/LamaPon/Components/CapsuleCollider3DComponent.h
    src/LamaPon/Components/SphereCollider3DComponent.cpp
    src/LamaPon/Components/SphereCollider3DComponent.h
    src/LamaPon/Components/ConvexHullCollider3DComponent.cpp
    src/LamaPon/Components/ConvexHullCollider3DComponent.h
    src/LamaPon/Components/CharacterControllerComponent.cpp
    src/LamaPon/Components/CharacterControllerComponent.h
    src/LamaPon/Components/DirectionalLightComponent.cpp
    src/LamaPon/Components/DirectionalLightComponent.h
    src/LamaPon/Components/PointLightComponent.cpp
    src/LamaPon/Components/PointLightComponent.h
    src/LamaPon/Components/SpotLightComponent.cpp
    src/LamaPon/Components/SpotLightComponent.h
    src/LamaPon/Components/MeshRendererComponent.cpp
    src/LamaPon/Components/MeshRendererComponent.h
    src/LamaPon/Components/ModelRendererComponent.cpp
    src/LamaPon/Components/ModelRendererComponent.h
    src/LamaPon/Components/NavMeshAgentComponent.cpp
    src/LamaPon/Components/NavMeshAgentComponent.h
    src/LamaPon/Components/NavMeshComponent.cpp
    src/LamaPon/Components/NavMeshComponent.h
    src/LamaPon/Components/ParticleSystemComponent.cpp
    src/LamaPon/Components/ParticleSystemComponent.h
    src/LamaPon/Components/SpriteParticles2DComponent.cpp
    src/LamaPon/Components/SpriteParticles2DComponent.h
    src/LamaPon/Components/UICanvasComponent.cpp
    src/LamaPon/Components/UICanvasComponent.h
    src/LamaPon/Components/UIRectTransformComponent.cpp
    src/LamaPon/Components/UIRectTransformComponent.h
    src/LamaPon/Components/CircleCollider2DComponent.cpp
    src/LamaPon/Components/CircleCollider2DComponent.h
    src/LamaPon/Components/MeshCollider3DComponent.cpp
    src/LamaPon/Components/MeshCollider3DComponent.h
    src/LamaPon/Components/UIButtonComponent.cpp
    src/LamaPon/Components/UIButtonComponent.h
    src/LamaPon/Components/UIImageComponent.cpp
    src/LamaPon/Components/UIImageComponent.h
    src/LamaPon/Components/UIInputFieldComponent.cpp
    src/LamaPon/Components/UIInputFieldComponent.h
    src/LamaPon/Components/UILayoutGroupComponent.cpp
    src/LamaPon/Components/UILayoutGroupComponent.h
    src/LamaPon/Components/UIScrollViewComponent.cpp
    src/LamaPon/Components/UIScrollViewComponent.h
    src/LamaPon/Components/UISliderComponent.cpp
    src/LamaPon/Components/UISliderComponent.h
    src/LamaPon/Components/UIToggleComponent.cpp
    src/LamaPon/Components/UIToggleComponent.h
    src/LamaPon/Components/SpriteAnimatorComponent.cpp
    src/LamaPon/Components/SpriteAnimatorComponent.h
    src/LamaPon/Components/SpriteRendererComponent.cpp
    src/LamaPon/Components/SpriteRendererComponent.h
    src/LamaPon/Components/TextRendererComponent.cpp
    src/LamaPon/Components/TextRendererComponent.h
    src/LamaPon/Components/TilemapComponent.cpp
    src/LamaPon/Components/TilemapComponent.h
    src/LamaPon/Components/TransformAnimatorComponent.cpp
    src/LamaPon/Components/TransformAnimatorComponent.h
    src/LamaPon/LamaPon.h
)

set(LAMAPON_EDITOR_SOURCES
    src/LamaPon/Editor/Editor.cpp
    src/LamaPon/Editor/Editor.h
    src/LamaPon/Editor/EditorExtensionRegistry.cpp
    src/LamaPon/Editor/EditorExtensionRegistry.h
    src/LamaPon/Editor/EditorLayer.cpp
    src/LamaPon/Editor/EditorLayer.h
    src/LamaPon/Editor/EditorLayerAnimationTools.cpp
    src/LamaPon/Editor/EditorLayerAssets.cpp
    src/LamaPon/Editor/EditorLayerExtensions.cpp
    src/LamaPon/Editor/EditorLayerInspector.cpp
    src/LamaPon/Editor/UIComponentInspectors.cpp
    src/LamaPon/Editor/DataAssetSchema.cpp
    src/LamaPon/Editor/DataAssetSchema.h
    src/LamaPon/Editor/EditorLayerProject.cpp
    src/LamaPon/Editor/EditorLayerSettings.cpp
    src/LamaPon/Editor/EditorLayerShared.h
    src/LamaPon/Editor/EditorLayerViewport.cpp
    src/LamaPon/Editor/EditorLayerPackages.cpp
    src/LamaPon/Editor/ExeIconTool.cpp
    src/LamaPon/Editor/ExeIconTool.h
    src/LamaPon/Editor/BgmLoopPanel.cpp
    src/LamaPon/Editor/BgmLoopPanel.h
    src/LamaPon/Editor/GameExporter.cpp
    src/LamaPon/Editor/GameExporter.h
    src/LamaPon/Editor/GameExportDialog.cpp
    src/LamaPon/Editor/WebExportJob.cpp
    src/LamaPon/Editor/GameModuleBuilder.cpp
    src/LamaPon/Editor/GameModuleBuilder.h
    src/LamaPon/Editor/PackageManager.cpp
    src/LamaPon/Editor/PackageManager.h
    src/LamaPon/Editor/ScriptEditorDetection.cpp
    src/LamaPon/Editor/ScriptEditorDetection.h
    src/LamaPon/Editor/ShaderProperties.cpp
    src/LamaPon/Editor/ShaderProperties.h
)

add_library(LamaPonRuntime SHARED
    ${LAMAPON_RUNTIME_SOURCES}
)
add_library(LamaPon::Runtime ALIAS LamaPonRuntime)
add_library(LamaPon ALIAS LamaPonRuntime)

set_target_properties(LamaPonRuntime PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

target_include_directories(LamaPonRuntime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR}/generated
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cgltf
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
)

target_compile_definitions(LamaPonRuntime
    PUBLIC
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
        _WIN32_WINNT=0x0A00
    PRIVATE
        LAMAPON_RUNTIME_BUILD
)

target_compile_options(LamaPonRuntime
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

target_link_libraries(LamaPonRuntime
    PUBLIC
        LamaPon::DirectXTK
        LamaPon::XAudio2Redist
        LamaPonJson
        Ufbx
    PRIVATE
        LamaPon::DirectXTKAudio
        bcrypt
        dbghelp
        d3d11
        d3dcompiler
        dxgi
        dxguid
        d2d1
        dwrite
        ole32
        windowscodecs
        winhttp
)
lamapon_enable_release_symbols(LamaPonRuntime)

add_custom_command(TARGET LamaPonRuntime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${LAMAPON_XAUDIO2_REDIST_DLL}"
        "$<TARGET_FILE_DIR:LamaPonRuntime>/xaudio2_9redist.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders"
        "$<TARGET_FILE_DIR:LamaPonRuntime>/assets/shaders"
    COMMENT "Copying runtime dependencies and built-in shaders"
)

add_library(LamaPonEditor STATIC
    ${LAMAPON_EDITOR_SOURCES}
)
add_library(LamaPon::Editor ALIAS LamaPonEditor)

target_include_directories(LamaPonEditor
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(LamaPonEditor
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)

target_link_libraries(LamaPonEditor
    PUBLIC
        LamaPonRuntime
    PRIVATE
        DearImGui
        ImGuizmo
        comdlg32
        shell32
        ole32
        windowscodecs
)
lamapon_enable_release_symbols(LamaPonEditor)

set(LAMAPON_WINDOWS_RESOURCES
    src/LamaPon/Resources/LamaPon.rc
    src/LamaPon/Resources/WindowsResource.h
)

set(LAMAPON_HUB_WINDOWS_RESOURCES
    src/LamaPon/Resources/LamaPonHub.rc
    src/LamaPon/Resources/WindowsResource.h
)

add_executable(LamaPonEditorApp WIN32
    samples/Sandbox/Main.cpp
    ${LAMAPON_WINDOWS_RESOURCES}
)

set(
    LAMAPON_EDITOR_ENGINE_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}"
    CACHE PATH
    "LamaPon source root used by the editor's C++ Game Module tools"
)
cmake_path(ABSOLUTE_PATH LAMAPON_EDITOR_ENGINE_ROOT NORMALIZE)

set_target_properties(LamaPonEditorApp PROPERTIES
    OUTPUT_NAME "LamaPonEditor"
)

target_link_libraries(LamaPonEditorApp PRIVATE LamaPonEditor)
target_compile_definitions(LamaPonEditorApp PRIVATE
    LAMAPON_DEFAULT_PROJECT_ROOT="${LAMAPON_EDITOR_ENGINE_ROOT}"
    LAMAPON_BUILD_CONFIGURATION="$<CONFIG>"
)

target_compile_options(LamaPonEditorApp
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)
lamapon_enable_release_symbols(LamaPonEditorApp)

add_library(LamaPonHubCore STATIC
    src/LamaPon/Hub/LearningJourney.cpp
    src/LamaPon/Hub/LearningJourney.h
    src/LamaPon/Hub/ProjectHub.cpp
    src/LamaPon/Hub/ProjectHub.h
    src/LamaPon/Hub/UpdateChecker.cpp
    src/LamaPon/Hub/UpdateChecker.h
)
target_include_directories(LamaPonHubCore
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(LamaPonHubCore
    PUBLIC
        LamaPonRuntime
    PRIVATE
        ole32
        shell32
)
target_compile_options(LamaPonHubCore
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)
lamapon_enable_release_symbols(LamaPonHubCore)

add_executable(LamaPonHub WIN32
    samples/Hub/Main.cpp
    ${LAMAPON_HUB_WINDOWS_RESOURCES}
)
target_link_libraries(LamaPonHub
    PRIVATE
        LamaPonHubCore
        ole32
        shell32
)
target_compile_options(LamaPonHub
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)
lamapon_enable_release_symbols(LamaPonHub)
add_dependencies(LamaPonHub LamaPonEditorApp)

add_executable(LamaPonGame WIN32
    samples/Game/Main.cpp
    ${LAMAPON_WINDOWS_RESOURCES}
)

target_link_libraries(LamaPonGame PRIVATE LamaPonRuntime)

file(GLOB_RECURSE LAMAPON_GAME_SCRIPT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/*.cpp"
)

add_library(LamaPonGameModule SHARED
    samples/GameModule/SampleGameModule.cpp
    samples/GameModule/TargetRangeGame.cpp
    samples/GameModule/InterfaceProbe.cpp
    samples/GameModule/InterfaceProbe.h
    tools/ProjectGameModule/ScriptRegistry.cpp
    tools/ProjectGameModule/ScriptRegistry.h
    ${LAMAPON_GAME_SCRIPT_SOURCES}
)
target_link_libraries(LamaPonGameModule PRIVATE LamaPonRuntime)
# ABIが同じでもRuntimeの実装更新後にはモジュールを再リンクし、
# GameModuleHostのビルド時刻による互換性確認と足並みを揃えます。
set_property(TARGET LamaPonGameModule APPEND PROPERTY LINK_DEPENDS
    "$<TARGET_FILE:LamaPonRuntime>")
target_include_directories(LamaPonGameModule PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/ProjectGameModule
)
set_target_properties(LamaPonGameModule PROPERTIES
    PREFIX ""
    OUTPUT_NAME "LamaPonGameModule"
)
target_compile_options(LamaPonGameModule
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)
lamapon_enable_release_symbols(LamaPonGameModule)
add_custom_command(TARGET LamaPonGameModule POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/.lamapon/bin"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:LamaPonGameModule>"
        "${CMAKE_CURRENT_SOURCE_DIR}/.lamapon/bin/LamaPonGameModule.dll"
    COMMENT "Updating the engine project Game Module"
)

add_custom_command(TARGET LamaPonGame POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:LamaPonGame>/assets"
    COMMENT "Copying Game assets"
)

add_custom_target(LamaPonGameSettings
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:LamaPonGame>"
    COMMAND ${CMAKE_COMMAND}
        "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/.lamapon/project.json"
        "-DOUTPUT=$<TARGET_FILE_DIR:LamaPonGame>/LamaPonGame.json"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteGameSettings.cmake"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/.lamapon/project.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteGameSettings.cmake"
    COMMENT "Updating LamaPonGame project settings"
)
add_dependencies(LamaPonGame LamaPonGameSettings)

target_compile_options(LamaPonGame
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)
lamapon_enable_release_symbols(LamaPonGame)

# エディターを開かずにプロジェクトを操作するCLI。
# stdoutへJSONを返すため、WIN32を付けずコンソールアプリとして構成します。
add_executable(LamaPonCli
    tools/LamaPonCli/Main.cpp
    tools/LamaPonCli/SceneCommands.cpp
    tools/LamaPonCli/ComponentSchemas.cpp
    tools/LamaPonCli/JsonFiles.cpp
    tools/LamaPonCli/ProjectPaths.cpp
    tools/LamaPonCli/RuntimeTiming.h
)
target_link_libraries(LamaPonCli
    PRIVATE
        LamaPonRuntime
        # newはHubと同じ方法でプロジェクトを作成し、exportは
        # エディターと同じ方法でパッケージを作成します。
        LamaPonHubCore
        LamaPonEditor
        # PNG書き出し（WIC）。LamaPonRuntimeはPRIVATEでしか
        # 持っていないので、ここでも明示します。
        windowscodecs
        ole32
)
target_compile_options(LamaPonCli
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
)
# buildサブコマンドがエディターと同じ方法でエンジンルートと
# ビルド構成を解決するため（Sandbox/Main.cppと同じ定義）。
target_compile_definitions(LamaPonCli PRIVATE
    LAMAPON_DEFAULT_PROJECT_ROOT="${LAMAPON_EDITOR_ENGINE_ROOT}"
    LAMAPON_BUILD_CONFIGURATION="$<CONFIG>"
)
lamapon_enable_release_symbols(LamaPonCli)

add_dependencies(
    LamaPonEditorApp
    LamaPonGame
    LamaPonGameModule
)

set(
    LAMAPON_GAME_EXPORT_DIR
    "${CMAKE_BINARY_DIR}/export/LamaPonGame"
)
add_custom_target(LamaPonGamePackage
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        "${LAMAPON_GAME_EXPORT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${LAMAPON_GAME_EXPORT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:LamaPonGame>"
        "${LAMAPON_GAME_EXPORT_DIR}/LamaPonGame.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:LamaPonRuntime>"
        "${LAMAPON_GAME_EXPORT_DIR}/LamaPonRuntime.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:LamaPonGameModule>"
        "${LAMAPON_GAME_EXPORT_DIR}/LamaPonGameModule.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${LAMAPON_XAUDIO2_REDIST_DLL}"
        "${LAMAPON_GAME_EXPORT_DIR}/xaudio2_9redist.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "${LAMAPON_GAME_EXPORT_DIR}/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/generated/build-info.json"
        "${LAMAPON_GAME_EXPORT_DIR}/build-info.json"
    COMMAND ${CMAKE_COMMAND}
        "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/.lamapon/project.json"
        "-DOUTPUT=${LAMAPON_GAME_EXPORT_DIR}/LamaPonGame.json"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteGameSettings.cmake"
    DEPENDS
        LamaPonGame
        LamaPonGameModule
        LamaPonRuntime
    COMMENT
        "Exporting game package to ${LAMAPON_GAME_EXPORT_DIR}"
)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT LamaPonHub)

include(CTest)

if(BUILD_TESTING)
    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND CMAKE_GENERATOR MATCHES "^Ninja")
        add_test(NAME MsvcHeaderDependencies
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/MsvcDependencyTests.py"
                --cmake "${CMAKE_COMMAND}"
                --ninja "${CMAKE_MAKE_PROGRAM}"
                --compiler "${CMAKE_CXX_COMPILER}"
                --engine-root "${CMAKE_CURRENT_SOURCE_DIR}"
                --work-dir "${CMAKE_CURRENT_BINARY_DIR}/test-output/msvc-dependencies")
    endif()
    add_test(NAME WebExportTool
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/WebExportToolTests.py")
    add_test(NAME EditorWebExport
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/EditorWebExportTests.py")
    add_executable(LamaPonGameExportDialogTests tests/GameExportDialogTests.cpp)
    target_link_libraries(LamaPonGameExportDialogTests PRIVATE LamaPonEditor DearImGui)
    target_compile_options(LamaPonGameExportDialogTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    add_test(NAME GameExportDialog COMMAND LamaPonGameExportDialogTests "${Python3_EXECUTABLE}")
    add_test(NAME CliSceneCommands
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/CliSceneCommandTests.py"
            --executable "$<TARGET_FILE:LamaPonCli>"
            --work-dir "${CMAKE_CURRENT_BINARY_DIR}/test-output/cli-scene")
    add_executable(LamaPonLifecycleTests tests/LifecycleTests.cpp)
    target_link_libraries(LamaPonLifecycleTests PRIVATE LamaPonRuntime)
    target_compile_options(LamaPonLifecycleTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    add_test(NAME LifecycleRecovery COMMAND LamaPonLifecycleTests)
    add_executable(LamaPonExportedStartupTests tests/ExportedStartupTests.cpp)
    target_link_libraries(LamaPonExportedStartupTests PRIVATE LamaPonEditor)
    target_compile_options(LamaPonExportedStartupTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    add_dependencies(LamaPonExportedStartupTests LamaPonGame LamaPonGameModule)
    add_test(NAME ExportedGameStartup COMMAND LamaPonExportedStartupTests)
    set_tests_properties(ExportedGameStartup PROPERTIES TIMEOUT 90)
    add_executable(LamaPonBgmLoopPanelTests tests/BgmLoopPanelTests.cpp)
    target_link_libraries(LamaPonBgmLoopPanelTests PRIVATE LamaPonEditor DearImGui)
    target_compile_options(LamaPonBgmLoopPanelTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    add_dependencies(LamaPonBgmLoopPanelTests LamaPonGame)
    add_test(NAME BgmLoopPanelIsolation COMMAND LamaPonBgmLoopPanelTests)
    add_executable(LamaPonUIComponentInspectorTests tests/UIComponentInspectorTests.cpp)
    target_link_libraries(LamaPonUIComponentInspectorTests PRIVATE LamaPonEditor DearImGui)
    target_compile_options(LamaPonUIComponentInspectorTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    add_test(NAME UIComponentInspectorIsolation COMMAND LamaPonUIComponentInspectorTests)
    add_executable(LamaPonEditorExtensionRegistryTests
        tests/EditorExtensionRegistryTests.cpp)
    target_link_libraries(LamaPonEditorExtensionRegistryTests PRIVATE LamaPonEditor)
    target_compile_options(LamaPonEditorExtensionRegistryTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    add_test(NAME EditorExtensionRegistry
        COMMAND LamaPonEditorExtensionRegistryTests)
    foreach(test_name IN ITEMS RuntimeServices RenderSpatialIndex)
        add_executable(LamaPon${test_name}Tests tests/${test_name}Tests.cpp)
        target_link_libraries(LamaPon${test_name}Tests PRIVATE LamaPonRuntime)
        target_compile_options(LamaPon${test_name}Tests PRIVATE
            $<$<CXX_COMPILER_ID:MSVC>:/W4>
            $<$<CXX_COMPILER_ID:MSVC>:/permissive->
            $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
        add_test(NAME ${test_name}Isolation COMMAND LamaPon${test_name}Tests)
    endforeach()
    add_executable(LamaPonAudioOggProbe
        tests/AudioOggProbe.cpp
    )
    target_link_libraries(
        LamaPonAudioOggProbe
        PRIVATE
            LamaPonRuntime
            LamaPon::DirectXTKAudio
    )
    target_compile_options(LamaPonAudioOggProbe PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME AudioOggDecode
        COMMAND
            LamaPonAudioOggProbe
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/audio"
    )
    set_tests_properties(
        AudioOggDecode
        PROPERTIES
            LABELS "audio"
    )

    add_executable(LamaPonSceneTests
        tests/SceneSerializationTests.cpp
    )
    add_executable(LamaPonPhysicsTimingTests tests/PhysicsTimingTests.cpp)
    target_link_libraries(LamaPonPhysicsTimingTests PRIVATE LamaPonRuntime)
    target_compile_options(LamaPonPhysicsTimingTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(NAME PhysicsPresentationTiming COMMAND LamaPonPhysicsTimingTests)
    set_tests_properties(PhysicsPresentationTiming PROPERTIES LABELS "scene;physics;timing")
    target_link_libraries(LamaPonSceneTests PRIVATE LamaPonRuntime)
    target_compile_options(LamaPonSceneTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME SceneSerializationAndPrefab
        COMMAND LamaPonSceneTests --suite serialization
    )
    add_test(
        NAME ScenePhysicsAndNavigation
        COMMAND LamaPonSceneTests --suite simulation
    )
    add_test(
        NAME SceneManagerTransitions
        COMMAND LamaPonSceneTests --suite scene-manager
    )
    set_tests_properties(
        SceneSerializationAndPrefab
        PROPERTIES LABELS "scene;serialization;prefab"
    )
    set_tests_properties(
        ScenePhysicsAndNavigation
        PROPERTIES LABELS "scene;physics;navigation"
    )
    set_tests_properties(
        SceneManagerTransitions
        PROPERTIES LABELS "scene;scene-manager"
    )

    add_executable(LamaPonReactiveTests
        tests/ReactiveTests.cpp
        src/LamaPon/Reactive/Reactive.cpp
    )
    target_include_directories(LamaPonReactiveTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_options(LamaPonReactiveTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(NAME ReactiveStreams COMMAND LamaPonReactiveTests)
    set_tests_properties(
        ReactiveStreams
        PROPERTIES LABELS "core;reactive"
    )

    add_executable(LamaPonGameExporterTests
        tests/GameExporterTests.cpp
    )
    target_link_libraries(LamaPonGameExporterTests PRIVATE LamaPonEditor)
    target_compile_options(LamaPonGameExporterTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(NAME GameExporter COMMAND LamaPonGameExporterTests)

    add_executable(LamaPonPackageManagerTests
        tests/PackageManagerTests.cpp
    )
    target_link_libraries(LamaPonPackageManagerTests PRIVATE LamaPonEditor)
    target_compile_options(LamaPonPackageManagerTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(NAME PackageManager COMMAND LamaPonPackageManagerTests)

    add_executable(LamaPonProjectMigrationTests
        tests/ProjectMigrationTests.cpp
    )
    target_link_libraries(LamaPonProjectMigrationTests PRIVATE LamaPonRuntime)
    # 組み込みアセットのincludeの取りこぼしを、エンジン側の実物を
    # 読んで検査するため。
    target_compile_definitions(LamaPonProjectMigrationTests PRIVATE
        LAMAPON_ENGINE_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
        LAMAPON_ENGINE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_compile_options(LamaPonProjectMigrationTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(NAME ProjectMigration COMMAND LamaPonProjectMigrationTests)

    add_executable(LamaPonCrashSentinelTests
        tests/CrashSentinelTests.cpp
    )
    target_link_libraries(LamaPonCrashSentinelTests PRIVATE LamaPonRuntime)
    target_compile_options(LamaPonCrashSentinelTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(NAME CrashSentinel COMMAND LamaPonCrashSentinelTests)

    add_executable(LamaPonInputTests
        tests/InputSystemTests.cpp
    )
    target_link_libraries(LamaPonInputTests PRIVATE LamaPonRuntime)
    target_compile_options(LamaPonInputTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(NAME InputActionTransitions COMMAND LamaPonInputTests)

    add_executable(LamaPonLogTests
        tests/LogTests.cpp
    )
    target_link_libraries(LamaPonLogTests PRIVATE LamaPonRuntime)
    target_compile_options(LamaPonLogTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(NAME LoggerAndFileSink COMMAND LamaPonLogTests)

    add_executable(LamaPonNoiseTests
        tests/NoiseTests.cpp
    )
    target_link_libraries(LamaPonNoiseTests PRIVATE LamaPonRuntime)
    target_compile_options(LamaPonNoiseTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_executable(LamaPonTransformTests
        tests/TransformTests.cpp
    )
    target_link_libraries(
        LamaPonTransformTests PRIVATE LamaPonRuntime)
    target_compile_options(LamaPonTransformTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME TransformRotation
        COMMAND LamaPonTransformTests)
    set_tests_properties(
        TransformRotation
        PROPERTIES
            LABELS "core;transform"
    )

    add_executable(LamaPonBillboardTests
        tests/BillboardTests.cpp
    )
    target_link_libraries(
        LamaPonBillboardTests PRIVATE LamaPonRuntime)
    target_compile_options(LamaPonBillboardTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME BillboardOrientation
        COMMAND LamaPonBillboardTests)
    set_tests_properties(
        BillboardOrientation
        PROPERTIES
            LABELS "scene;billboard"
    )

    add_executable(LamaPonTemporalJitterTests
        tests/TemporalJitterTests.cpp
    )
    target_link_libraries(
        LamaPonTemporalJitterTests PRIVATE LamaPonRuntime)
    target_compile_options(
        LamaPonTemporalJitterTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME TemporalJitter
        COMMAND LamaPonTemporalJitterTests)
    set_tests_properties(
        TemporalJitter
        PROPERTIES
            LABELS "graphics;temporal"
    )

    add_executable(LamaPonShaderDiagnosticsTests
        tests/ShaderDiagnosticsTests.cpp
    )
    target_link_libraries(
        LamaPonShaderDiagnosticsTests PRIVATE LamaPonRuntime)
    target_compile_options(
        LamaPonShaderDiagnosticsTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME ShaderDiagnostics
        COMMAND LamaPonShaderDiagnosticsTests)
    set_tests_properties(
        ShaderDiagnostics
        PROPERTIES
            LABELS "graphics;shader"
    )

    add_executable(LamaPonShaderCacheTests
        tests/ShaderCacheTests.cpp
    )
    target_link_libraries(
        LamaPonShaderCacheTests PRIVATE LamaPonRuntime)
    target_compile_options(
        LamaPonShaderCacheTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME ShaderCache
        COMMAND LamaPonShaderCacheTests)
    set_tests_properties(
        ShaderCache
        PROPERTIES
            LABELS "graphics;shader"
    )

    add_test(NAME NoiseFunctions COMMAND LamaPonNoiseTests)
    set_tests_properties(
        NoiseFunctions
        PROPERTIES
            LABELS "core;noise"
    )

    add_executable(LamaPonDiagnosticsTests
        tests/DiagnosticsTests.cpp
    )
    target_link_libraries(
        LamaPonDiagnosticsTests
        PRIVATE
            LamaPonRuntime
    )
    target_compile_options(LamaPonDiagnosticsTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME ProfilerAndCrashReporter
        COMMAND LamaPonDiagnosticsTests
    )
    set_tests_properties(
        ProfilerAndCrashReporter
        PROPERTIES LABELS "diagnostics"
    )

    add_executable(LamaPonPersistenceTests
        tests/PersistenceTests.cpp
    )
    target_link_libraries(
        LamaPonPersistenceTests
        PRIVATE
            LamaPonRuntime
    )
    target_compile_options(LamaPonPersistenceTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME PlayerPrefsAndSaveData
        COMMAND LamaPonPersistenceTests
    )

    add_executable(LamaPonDocumentMigrationTests
        tests/DocumentMigrationTests.cpp
    )
    target_link_libraries(
        LamaPonDocumentMigrationTests
        PRIVATE
            LamaPonRuntime
    )
    target_compile_options(LamaPonDocumentMigrationTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME SerializedDocumentMigration
        COMMAND LamaPonDocumentMigrationTests
    )
    set_tests_properties(
        SerializedDocumentMigration
        PROPERTIES LABELS "serialization;migration"
    )

    add_executable(LamaPonRenderRegressionTests
        tests/RenderRegressionTests.cpp
    )
    target_link_libraries(
        LamaPonRenderRegressionTests
        PRIVATE
            d3d11
            d3dcompiler
    )
    target_compile_options(LamaPonRenderRegressionTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME RenderRegressionWarp
        COMMAND
            LamaPonRenderRegressionTests
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/render/fullscreen-triangle.ppm"
            "${CMAKE_CURRENT_BINARY_DIR}/test-output/render-regression/fullscreen-triangle-actual.ppm"
    )
    set_tests_properties(
        RenderRegressionWarp
        PROPERTIES
            LABELS "graphics;render-regression"
            TIMEOUT 60
    )

    add_executable(LamaPonRenderTests
        tests/EngineRenderRegressionTests.cpp
    )
    target_link_libraries(
        LamaPonRenderTests
        PRIVATE
            LamaPonRuntime
    )
    target_compile_definitions(
        LamaPonRenderTests
        PRIVATE
            LAMAPON_TEST_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
            LAMAPON_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    )
    target_compile_options(LamaPonRenderTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME EngineRenderPathWarp
        COMMAND LamaPonRenderTests
    )
    set_tests_properties(
        EngineRenderPathWarp
        PROPERTIES
            LABELS "graphics;render-regression"
            TIMEOUT 180
    )

    add_executable(LamaPonRenderFailureTests
        tests/RenderFailureTests.cpp
    )
    target_link_libraries(
        LamaPonRenderFailureTests
        PRIVATE
            LamaPonRuntime
    )
    target_compile_definitions(
        LamaPonRenderFailureTests
        PRIVATE
            LAMAPON_TEST_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
    )
    target_compile_options(LamaPonRenderFailureTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME RenderFailureRecovery
        COMMAND LamaPonRenderFailureTests
    )
    set_tests_properties(
        RenderFailureRecovery
        PROPERTIES
            LABELS "graphics;render-regression"
            TIMEOUT 180
    )

    add_executable(LamaPonAnimationTests
        tests/AnimationClipTests.cpp
    )
    target_link_libraries(LamaPonAnimationTests PRIVATE LamaPonRuntime)
    target_compile_options(LamaPonAnimationTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(NAME AnimationClipSampling COMMAND LamaPonAnimationTests)

    add_executable(LamaPonGameModuleTests
        tests/GameModuleTests.cpp
    )
    target_link_libraries(
        LamaPonGameModuleTests
        PRIVATE
            LamaPonRuntime
    )
    add_dependencies(
        LamaPonGameModuleTests
        LamaPonGameModule
    )
    target_compile_options(LamaPonGameModuleTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME GameModuleRegistration
        COMMAND LamaPonGameModuleTests
    )

    add_executable(LamaPonAssetDatabaseTests
        tests/AssetDatabaseTests.cpp
    )
    target_link_libraries(
        LamaPonAssetDatabaseTests
        PRIVATE
            LamaPonRuntime
    )
    target_compile_options(LamaPonAssetDatabaseTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME AssetDatabaseGuidAndDependencies
        COMMAND LamaPonAssetDatabaseTests
    )

    add_executable(LamaPonDataAssetTests
        tests/DataAssetTests.cpp
    )
    target_link_libraries(
        LamaPonDataAssetTests
        PRIVATE
            LamaPonEditor
    )
    target_compile_options(LamaPonDataAssetTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME DataAssets
        COMMAND LamaPonDataAssetTests
    )

    add_executable(LamaPonAssetImporterTests
        tests/AssetImporterTests.cpp
    )
    target_link_libraries(
        LamaPonAssetImporterTests
        PRIVATE
            LamaPonRuntime
    )
    target_compile_options(LamaPonAssetImporterTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME ExternalAssetImport
        COMMAND LamaPonAssetImporterTests
    )

    add_executable(LamaPonTextureLoaderTests
        tests/TextureLoaderTests.cpp
    )
    target_link_libraries(
        LamaPonTextureLoaderTests
        PRIVATE
            LamaPonRuntime
            d3d11
    )
    target_compile_options(LamaPonTextureLoaderTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME TextureLoaderAndAsyncPrefetch
        COMMAND LamaPonTextureLoaderTests
    )
    set_tests_properties(
        TextureLoaderAndAsyncPrefetch
        PROPERTIES
            LABELS "assets;graphics"
            TIMEOUT 120
    )

    add_executable(LamaPonScriptPrefsTests
        tests/ScriptPrefsTests.cpp
    )
    target_link_libraries(
        LamaPonScriptPrefsTests
        PRIVATE
            LamaPonRuntime
    )
    target_compile_options(LamaPonScriptPrefsTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME ScriptPlayerPrefs
        COMMAND LamaPonScriptPrefsTests
    )
    set_tests_properties(
        ScriptPlayerPrefs
        PROPERTIES
            LABELS "scripting;persistence"
            TIMEOUT 60
    )

    add_executable(LamaPonTextCacheTests
        tests/TextCacheTests.cpp
    )
    target_link_libraries(
        LamaPonTextCacheTests
        PRIVATE
            LamaPonRuntime
            d3d11
    )
    target_compile_options(LamaPonTextCacheTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME TextTextureCacheBudget
        COMMAND LamaPonTextCacheTests
    )
    set_tests_properties(
        TextTextureCacheBudget
        PROPERTIES
            LABELS "assets;graphics"
            TIMEOUT 120
    )

    add_executable(LamaPonGltfImporterTests
        tests/GltfImporterTests.cpp
    )
    target_link_libraries(
        LamaPonGltfImporterTests
        PRIVATE
            LamaPonRuntime
            d3d11
    )
    target_compile_definitions(
        LamaPonGltfImporterTests
        PRIVATE
            LAMAPON_TEST_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
    )
    target_compile_options(LamaPonGltfImporterTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME GltfSkeletalImport
        COMMAND LamaPonGltfImporterTests
    )

    add_executable(LamaPonFbxImporterTests
        tests/FbxImporterTests.cpp
    )
    target_link_libraries(
        LamaPonFbxImporterTests
        PRIVATE
            LamaPonRuntime
            d3d11
    )
    target_compile_definitions(
        LamaPonFbxImporterTests
        PRIVATE
            LAMAPON_TEST_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
    )
    target_compile_options(LamaPonFbxImporterTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME FbxSkeletalImport
        COMMAND LamaPonFbxImporterTests
    )

    add_executable(LamaPonProjectHubTests
        tests/ProjectHubTests.cpp
    )
    target_link_libraries(
        LamaPonProjectHubTests
        PRIVATE
            LamaPonHubCore
    )
    target_compile_options(LamaPonProjectHubTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_custom_command(TARGET LamaPonProjectHubTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:LamaPonProjectHubTests>/assets/textures"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/textures/LamaPonEngineLogo.png"
            "$<TARGET_FILE_DIR:LamaPonProjectHubTests>/assets/textures/LamaPonEngineLogo.png"
        COMMENT "Copying Project Hub test assets"
    )
    add_test(
        NAME ProjectHubTemplates
        COMMAND LamaPonProjectHubTests
    )

    add_executable(LamaPonRuntimeTimingTests
        tests/RuntimeTimingTests.cpp
        tools/LamaPonCli/RuntimeTiming.h
    )
    target_include_directories(LamaPonRuntimeTimingTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/LamaPonCli"
    )
    target_compile_options(LamaPonRuntimeTimingTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME RuntimeTimingAndRenderCadence
        COMMAND LamaPonRuntimeTimingTests
    )

    add_executable(LamaPonGameModuleBuilderTests
        tests/GameModuleBuilderTests.cpp
        src/LamaPon/Editor/GameModuleBuilder.cpp
        src/LamaPon/Editor/GameModuleBuilder.h
        tools/LamaPonCli/BuildDiagnostics.h
    )
    target_include_directories(LamaPonGameModuleBuilderTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/LamaPonCli"
    )
    target_link_libraries(
        LamaPonGameModuleBuilderTests
        PRIVATE
            LamaPonRuntime
    )
    target_compile_options(LamaPonGameModuleBuilderTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME GameModuleLocalBuildCache
        COMMAND LamaPonGameModuleBuilderTests
    )

    add_executable(LamaPonProceduralMeshTests
        tests/ProceduralMeshTests.cpp
    )
    target_link_libraries(
        LamaPonProceduralMeshTests
        PRIVATE
            LamaPonRuntime
    )
    target_compile_options(LamaPonProceduralMeshTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME ProceduralMeshGeometry
        COMMAND LamaPonProceduralMeshTests
    )

    add_executable(LamaPonProjectInstanceTests
        tests/ProjectInstanceTests.cpp
    )
    target_link_libraries(
        LamaPonProjectInstanceTests
        PRIVATE
            LamaPonRuntime
    )
    target_compile_options(LamaPonProjectInstanceTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME ProjectSingleInstance
        COMMAND LamaPonProjectInstanceTests
    )

    add_executable(LamaPonProjectGameModuleLoadTests
        tests/ProjectGameModuleLoadTests.cpp
    )
    target_link_libraries(
        LamaPonProjectGameModuleLoadTests
        PRIVATE
            LamaPonRuntime
    )
    target_compile_options(LamaPonProjectGameModuleLoadTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<CXX_COMPILER_ID:MSVC>:/permissive->
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    )
    add_test(
        NAME ProjectGameModuleBuildAndLoad
        COMMAND ${CMAKE_COMMAND}
            "-DENGINE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DPROJECT_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ProjectGameModule"
            "-DRUNTIME_DIR=$<TARGET_FILE_DIR:LamaPonRuntime>"
            "-DTEST_BUILD_DIR=${CMAKE_BINARY_DIR}/test-output/project-game-module-build"
            "-DTEST_OUTPUT_DIR=${CMAKE_BINARY_DIR}/test-output/project-game-module-output"
            "-DTEST_LOADER=$<TARGET_FILE:LamaPonProjectGameModuleLoadTests>"
            "-DTEST_GENERATOR=${CMAKE_GENERATOR}"
            "-DTEST_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
            "-DGENERATED_INCLUDE_DIR=${CMAKE_BINARY_DIR}/generated"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestProjectGameModule.cmake"
    )
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src PREFIX "Runtime" FILES
    ${LAMAPON_RUNTIME_SOURCES}
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src PREFIX "Editor" FILES
    ${LAMAPON_EDITOR_SOURCES}
)

# VC++ランタイムDLLを配布ZIPのルートへ同梱し、再頒布可能パッケージを
# インストールしていないPCでもHub／Editor／エクスポートしたゲームが
# そのまま起動できるようにします。
if(MSVC)
    # 同梱するVC++ランタイムDLLは、ビルドに使用したコンパイラと
    # 同じ版である必要があります。同じフォルダーのDLLが優先されるため、
    # 古いCRTが残ると起動時にアクセス違反が発生します。
    #
    # InstallRequiredSystemLibrariesの自動検出は、Visual Studioが
    # 複数入っているPCで別の版を拾ったり、新しいtoolsetの
    # ディレクトリ名（Microsoft.VC145.CRT）を当てられずに
    # DLLを検出できないことがあります。誤った配布物を作らないよう、
    # vcvarsが指すVCToolsRedistDirから実在するディレクトリを
    # 直接探し、見つからなければ構成時に止めます。
    set(LAMAPON_CRT_DLLS "")
    if(DEFINED ENV{VCToolsRedistDir})
        file(TO_CMAKE_PATH "$ENV{VCToolsRedistDir}" LAMAPON_REDIST_ROOT)
        file(GLOB LAMAPON_CRT_DIRS
            "${LAMAPON_REDIST_ROOT}/x64/Microsoft.VC*.CRT")
        foreach(dir IN LISTS LAMAPON_CRT_DIRS)
            file(GLOB dlls "${dir}/*.dll")
            list(APPEND LAMAPON_CRT_DLLS ${dlls})
        endforeach()
    endif()
    if(LAMAPON_CRT_DLLS)
        message(STATUS
            "Bundling VC++ runtime from: ${LAMAPON_REDIST_ROOT}")
        install(FILES ${LAMAPON_CRT_DLLS} DESTINATION ".")
    else()
        # 自動検出では異なる版を選ぶおそれがあるため、ここで構成を停止し、
        # vcvarsの設定が必要であることを表示します。
        message(FATAL_ERROR
            "VC++ runtime DLLs were not found. "
            "Configure from a Visual Studio developer prompt "
            "(vcvars64.bat) so VCToolsRedistDir is set.")
    endif()
endif()

# 配布ZIPにはゲーム制作へ必要なものだけを含めます。
# サンプルゲーム（LamaPonGameModule.dll・サンプルシーン等）と
# PDB（別のsymbols zipで配布）は含めません。
# LamaPonGame.exeはエクスポートの雛形として必要です。
install(
    TARGETS
        LamaPonRuntime
        LamaPonEditorApp
        LamaPonHub
        LamaPonGame
        LamaPonCli
    RUNTIME DESTINATION "."
    LIBRARY DESTINATION "."
    ARCHIVE DESTINATION "."
)
install(
    FILES "${LAMAPON_XAUDIO2_REDIST_DLL}"
    DESTINATION "."
)
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/build-info.json"
    DESTINATION "."
)
# assetsはエンジンが実行時に参照する組み込みアセットだけを
# 同梱します（shaders/materialsはHub::CreateProjectが新規プロジェクト
# へコピー、icons/filetypesはエディターのAsset Browser種別アイコン）。
install(
    DIRECTORY assets/shaders
    DESTINATION assets
)
install(
    DIRECTORY assets/icons/filetypes
    DESTINATION assets/icons
    FILES_MATCHING PATTERN "*.png"
)
install(
    DIRECTORY tools/ProjectGameModule
    DESTINATION tools
)
install(FILES cmake/LamaPonMsvcDependencies.cmake DESTINATION cmake)
# Native Game Moduleが使う公開ヘッダーを同梱します。アーカイブ作成と
# 暗号処理の内部ヘッダーはゲーム側で不要なため対象外です。
# ソース公開を前提とし、この除外をセキュリティ上の保護とは扱いません。
install(
    DIRECTORY src/LamaPon
    DESTINATION src
    FILES_MATCHING
        PATTERN "*.h"
        PATTERN "Crypto.h" EXCLUDE
        PATTERN "AssetPacker.h" EXCLUDE
        PATTERN "AssetArchive.h" EXCLUDE
)
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/generated/LamaPon/Core/Version.h"
    DESTINATION src/LamaPon/Core
)
install(
    DIRECTORY third_party/nlohmann
    DESTINATION third_party
    FILES_MATCHING PATTERN "*.hpp"
)
install(
    DIRECTORY third_party/DirectXTK/include
    DESTINATION third_party/DirectXTK
)
install(
    DIRECTORY third_party/XAudio2Redist/include
    DESTINATION third_party/XAudio2Redist
)
install(
    FILES
        LICENSE
        README.md
        CHANGELOG.md
        CONTRIBUTING.md
        SECURITY.md
        THIRD_PARTY_NOTICES.md
    DESTINATION "."
)
# READMEが案内する使い方ドキュメント（テキストのみ、約100KB）。
install(
    DIRECTORY docs
    DESTINATION "."
)

include(cmake/LamaPonDistribution.cmake)

set(CPACK_PACKAGE_NAME "LamaPon")
set(CPACK_PACKAGE_VENDOR "LamaPon contributors")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(
    CPACK_PACKAGE_FILE_NAME
    "LamaPon-${PROJECT_VERSION}-windows-x64"
)
set(CPACK_GENERATOR "ZIP")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
include(CPack)
