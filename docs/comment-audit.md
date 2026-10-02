# コード内コメントの整理状況

2026年10月2日、`community/hector/comment_update`の自前コード全体694ファイルを走査し、既存コメントの共通整形と一部APIの説明追加を反映しました。**全変数・全関数への説明追加と、全コメントの意味を確認した規約適合は未完了です。**

2026年10月3日の継続作業では、`tools/export_web.py`のWeb互換性・成果物処理を読み、Python ASTで関数、引数、代入先、反復変数、制御文を再確認しました。同ファイルの単純な構文候補抽出では、コメントのない宣言・制御文は0件です。これは他ファイルやコメントの意味まで全件確認したことを示しません。

今回の基準コミットは`7535cae4c541e2d5d8971a81424864c0538f369b`です。開始時点の未コミット変更を含めたコードを比較基準とし、ゲームの描画修正など既存の処理を維持しています。プロジェクトの複製、検証用プロジェクト、一時フォルダー・一時ファイルは作成していません。

2026年9月30日の初回棚卸しは687ファイル・345,301行を対象としていました。その後の追加ソースも含め、前回のCSV走査では694ファイル・378,691行を対象にしました。初回の件数と前回走査の件数は、対象コードと抽出方法が異なるため、その差を是正件数として扱いません。

## 適用した整理

- C++・HLSLなど667ファイルを走査し、624ファイルでコメントを変更しました。
- Python・CMake・PowerShellなども走査し、16ファイルでコメントを変更しました。
- 日本語の折り返しを整理し、概要と必要な補足を原則1文・1行に分けました。
- C++・HLSLの行末・文中コメント37箇所を解消しました。
- 複数行の条件式、呼び出し、初期化、引数リスト内にあったC++コメント83行と、CMakeコメント12行を対象の文・宣言の直前へ移しました。
- `Application`、`ApplicationLayer`、`BuildInfo`、`JobSystem`、`HttpClient`、`ShaderDiagnostics`の公開API・メンバー説明を補い、必須の契約を短くまとめました。
- `Time.cpp`、`BuildInfo.cpp`、`JobSystem.cpp`、パッケージ生成スクリプト、音声生成スクリプトでは、ローカル変数・ループ変数などの役割を追加しました。
- `export_web.py`のポータブル契約・Web互換性・アセット・成果物関数に、変数、引数、分岐の説明を追加しました。SDK連携と通信サンプルの補助ソースにも、関数・変数・制御文の説明を追加しました。
- Asset Importer、Build情報、Crash Sentinel、Project Instance、Render Spatial Index、Web Lifecycleのテストで、制御文の説明を補いました。GUI RendererとGame Exporterのテストにも不足していた変数説明を追加しました。
- `AudioOggProbe.cpp`と`BillboardTests.cpp`の関数見出しを`関数(引数名: 意味): 概要`形式へ統一し、変数の役割と制御文の説明を整理しました。
- `D3D12SpriteRenderingTests.cpp`のDDS fixture、不可視window、sprite描画補助の先頭部分で、関数概要と変数の対応を整理しました。CLIが出力するgame scriptの`Start`・`Update`説明は利用者向け案内として維持しました。
- `LamaPonNoise.hlsli`の勾配関数で、hash indexごとの8方向と引数・変数の意味を明記しました。
- `CMakeLists.txt`の配布・リンク説明から重複する文をまとめ、必須の配布条件だけを残しました。
- `tools/audit_game_module.py`の条件文末尾にあったCodeView定数コメントを、条件文の直前へ移しました。
- 10ファイルのインクルード直前にあった説明コメントを除き、CONTRIBUTING.mdへ同じ配置ルールを加えました。全694ファイルを再走査し、インクルード直前のコメントがないことを確認しました。
- `SkeletalModel::Draw`の引数内にあった説明を、関数前の概要と引数名を明示した契約へまとめました。
- 開発経緯や自明な参照案内を一部整理し、ノイズ関数などの長い導入説明を概要とCPU/GPU一致の契約へ縮めました。
- Easingの使用例は[パッケージ文書](packages.md#easingを直接使う)へ移しました。

これらは全体への共通整形と、確認できた箇所の説明追加です。コメントの意味を全ファイルで人手確認し終えたものではありません。長い文章、説明のない宣言、単位・所有権などが不足する説明は、引き続き是正対象です。

## 意味を確認して整理したソース

全体の共通整形に続き、次の675ファイルで実装を読み、変数の役割、引数、関数概要、必須の契約を整理しました。初期の個別是正範囲は「適用した整理」に記載しています。

- `src/LamaPon/Core/VersionCompare.h`
- `src/LamaPon/Core/VersionCompare.cpp`
- `src/LamaPon/Core/SaveSlotValidation.h`
- `src/LamaPon/Core/SaveSlotValidation.cpp`
- `src/LamaPon/Core/PathUtils.h`
- `src/LamaPon/Core/Noise.h`
- `src/LamaPon/Core/DocumentMigration.h`
- `src/LamaPon/Core/DocumentMigration.cpp`
- `src/LamaPon/Core/Crypto.h`
- `src/LamaPon/Core/Crypto.cpp`
- `src/LamaPon/Core/Log.h`
- `src/LamaPon/Core/Log.cpp`
- `src/LamaPon/Core/CrashSentinel.h`
- `src/LamaPon/Core/CrashSentinel.cpp`
- `src/LamaPon/Core/ProjectInstance.h`
- `src/LamaPon/Core/ProjectInstance.cpp`
- `src/LamaPon/Core/MemorySnapshot.h`
- `src/LamaPon/Core/MemorySnapshot.cpp`
- `src/LamaPon/Core/RuntimeServices.h`
- `src/LamaPon/Core/RuntimeServices.cpp`
- `src/LamaPon/Core/Profiler.h`
- `src/LamaPon/Core/Profiler.cpp`
- `src/LamaPon/Core/ProfileAnalysis.h`
- `src/LamaPon/Core/ProfileAnalysis.cpp`
- `src/LamaPon/Core/CrashReporter.h`
- `src/LamaPon/Core/CrashReporter.cpp`
- `src/LamaPon/Core/Window.h`
- `src/LamaPon/Core/Window.cpp`
- `src/LamaPon/Core/DebugOverlay.h`
- `src/LamaPon/Core/DebugOverlay.cpp`
- `src/LamaPon/Core/SaveData.h`
- `src/LamaPon/Core/SaveData.cpp`
- `src/LamaPon/Core/ProjectMigration.h`
- `src/LamaPon/Core/ProjectMigration.cpp`
- `src/LamaPon/Core/Application.cpp`
- `src/LamaPon/Core/PlayerPrefs.h`
- `src/LamaPon/Core/PlayerPrefs.cpp`
- `src/LamaPon/Core/PersistenceProfiles.h`
- `src/LamaPon/Core/PersistenceProfiles.cpp`
- `assets/shaders/LamaPonNoise.hlsli`
- `src/LamaPon/Core/HttpClient.h`
- `src/LamaPon/Core/HttpClient.cpp`
- `src/LamaPon/Core/ProjectSettings.h`
- `src/LamaPon/Core/ProjectSettings.cpp`
- `src/LamaPon/Core/LocalPersistenceDocuments.h`
- `src/LamaPon/Core/LocalPersistenceDocuments.cpp`
- `src/LamaPon/Input/InputSystem.cpp`
- `src/LamaPon/Input/InputSystem.h`
- `assets/packages/easing-tween/Easing.h`
- `assets/packages/easing-tween/Tween.cpp`
- `src/LamaPon/Physics/PhysicsFrameClock.h`
- `src/LamaPon/Physics/PhysicsFrameClock.cpp`
- `src/LamaPon/Physics/PhysicsMaterial.h`
- `src/LamaPon/Physics/PhysicsSettings.h`
- `src/LamaPon/Physics/PhysicsSettings.cpp`
- `src/LamaPon/Physics/CollisionTypes.h`
- `src/LamaPon/Physics/PhysicsQuery.h`
- `src/LamaPon/Physics/CollisionMesh.h`
- `src/LamaPon/Physics/CollisionMesh.cpp`
- `src/LamaPon/Physics/Raycast.h`
- `src/LamaPon/Physics/Raycast.cpp`
- `src/LamaPon/Physics/SpatialHashBroadPhase.h`
- `src/LamaPon/Physics/SpatialHashBroadPhase.cpp`
- `src/LamaPon/Physics/Collision3D.h`
- `src/LamaPon/Physics/Collision3D.cpp`
- `src/LamaPon/Animation/AnimationClip.h`
- `src/LamaPon/Animation/AnimationClip.cpp`
- `src/LamaPon/Animation/AnimatorController.h`
- `src/LamaPon/Animation/AnimatorController.cpp`
- `src/LamaPon/Scene/EventBus.h`
- `src/LamaPon/Scene/EventBus.cpp`
- `src/LamaPon/Scene/RuntimeGameState.h`
- `src/LamaPon/Scene/RuntimeGameState.cpp`
- `src/LamaPon/Scene/Component.h`
- `src/LamaPon/Scene/Component.cpp`
- `src/LamaPon/Scene/Transform.h`
- `src/LamaPon/Scene/GameObject.h`
- `src/LamaPon/Scene/GameObject.cpp`
- `src/LamaPon/Scene/RenderSpatialIndex.h`
- `src/LamaPon/Scene/RenderSpatialIndex.cpp`
- `src/LamaPon/Scene/SceneTransition.h`
- `src/LamaPon/Scene/SceneTransition.cpp`
- `src/LamaPon/Scene/SceneManager.h`
- `src/LamaPon/Scene/SceneManager.cpp`
- `src/LamaPon/Scene/SceneVisibility.cpp`
- `src/LamaPon/Scene/Scene.cpp`
- `src/LamaPon/Scene/ScenePhysics.cpp`
- `src/LamaPon/Scene/Scene.h`
- `src/LamaPon/Scene/SceneSerialization.cpp`
- `src/LamaPon/Components/CameraComponent.h`
- `src/LamaPon/Components/CameraComponent.cpp`
- `src/LamaPon/Components/AudioListenerComponent.h`
- `src/LamaPon/Components/AudioListenerComponent.cpp`
- `src/LamaPon/Components/RotatorComponent.h`
- `src/LamaPon/Components/RotatorComponent.cpp`
- `src/LamaPon/Components/NetworkIdentityComponent.h`
- `src/LamaPon/Components/NetworkIdentityComponent.cpp`
- `src/LamaPon/Components/ParallaxLayerComponent.h`
- `src/LamaPon/Components/ParallaxLayerComponent.cpp`
- `src/LamaPon/Components/RenderCullingComponent.h`
- `src/LamaPon/Components/PointLightComponent.h`
- `src/LamaPon/Components/Light2DComponent.h`
- `src/LamaPon/Components/DirectionalLightComponent.h`
- `src/LamaPon/Components/SpotLightComponent.h`
- `src/LamaPon/Components/ReflectionProbeComponent.h`
- `src/LamaPon/Components/PointLightComponent.cpp`
- `src/LamaPon/Components/SpotLightComponent.cpp`
- `src/LamaPon/Components/DirectionalLightComponent.cpp`
- `src/LamaPon/Components/Light2DComponent.cpp`
- `src/LamaPon/Components/ReflectionProbeComponent.cpp`
- `src/LamaPon/Components/BoxCollider2DComponent.h`
- `src/LamaPon/Components/BoxCollider3DComponent.h`
- `src/LamaPon/Components/CircleCollider2DComponent.h`
- `src/LamaPon/Components/SphereCollider3DComponent.h`
- `src/LamaPon/Components/InputMoverComponent.h`
- `src/LamaPon/Components/InputMoverComponent.cpp`
- `src/LamaPon/Components/BoxCollider2DComponent.cpp`
- `src/LamaPon/Components/BoxCollider3DComponent.cpp`
- `src/LamaPon/Components/CircleCollider2DComponent.cpp`
- `src/LamaPon/Components/SphereCollider3DComponent.cpp`
- `src/LamaPon/Components/FrameDebugDescription.h`
- `src/LamaPon/Components/CapsuleCollider3DComponent.h`
- `src/LamaPon/Components/PolygonCollider2DComponent.h`
- `src/LamaPon/Components/ConvexHullCollider3DComponent.h`
- `src/LamaPon/Components/CapsuleCollider3DComponent.cpp`
- `src/LamaPon/Components/PolygonCollider2DComponent.cpp`
- `src/LamaPon/Components/ConvexHullCollider3DComponent.cpp`
- `src/LamaPon/Components/MeshCollider3DComponent.h`
- `src/LamaPon/Components/MeshCollider3DComponent.cpp`
- `src/LamaPon/Components/CharacterControllerComponent.h`
- `src/LamaPon/Components/CharacterControllerComponent.cpp`
- `src/LamaPon/Components/RigidbodyComponent.h`
- `src/LamaPon/Components/RigidbodyComponent.cpp`
- `src/LamaPon/Components/JointComponent.h`
- `src/LamaPon/Components/JointComponent.cpp`
- `src/LamaPon/Components/BillboardComponent.h`
- `src/LamaPon/Components/BillboardComponent.cpp`
- `src/LamaPon/Components/LODGroupComponent.h`
- `src/LamaPon/Components/LODGroupComponent.cpp`
- `src/LamaPon/Components/UICanvasComponent.h`
- `src/LamaPon/Components/UICanvasComponent.cpp`
- `src/LamaPon/Components/UIRectTransformComponent.h`
- `src/LamaPon/Components/UIRectTransformComponent.cpp`
- `src/LamaPon/Components/UILayoutGroupComponent.h`
- `src/LamaPon/Components/UILayoutGroupComponent.cpp`
- `src/LamaPon/Components/SpriteMaskComponent.h`
- `src/LamaPon/Components/SpriteMaskComponent.cpp`
- `src/LamaPon/Components/UIScrollViewComponent.h`
- `src/LamaPon/Components/UIScrollViewComponent.cpp`
- `assets/shaders/LamaPonSpriteMask.hlsl`
- `src/LamaPon/Components/SpriteRendererComponent.h`
- `src/LamaPon/Components/SpriteRendererComponent.cpp`
- `src/LamaPon/Components/TextRendererComponent.h`
- `src/LamaPon/Components/TextRendererComponent.cpp`
- `src/LamaPon/Graphics/TextLayout.h`
- `src/LamaPon/Components/UIImageComponent.h`
- `src/LamaPon/Components/UIImageComponent.cpp`
- `src/LamaPon/Components/UIButtonComponent.h`
- `src/LamaPon/Components/UIButtonComponent.cpp`
- `src/LamaPon/Components/UISliderComponent.h`
- `src/LamaPon/Components/UIToggleComponent.h`
- `src/LamaPon/Components/UISliderComponent.cpp`
- `src/LamaPon/Components/UIToggleComponent.cpp`
- `src/LamaPon/Components/UIInputFieldComponent.h`
- `src/LamaPon/Components/UIInputFieldComponent.cpp`
- `src/LamaPon/Components/SpriteAnimatorComponent.h`
- `src/LamaPon/Components/SpriteAnimatorComponent.cpp`
- `src/LamaPon/Components/TilemapComponent.h`
- `src/LamaPon/Components/TilemapComponent.cpp`
- `src/LamaPon/Components/NativeScriptComponent.h`
- `src/LamaPon/Components/NativeScriptComponent.cpp`
- `src/LamaPon/Components/TransformAnimatorComponent.h`
- `src/LamaPon/Components/TransformAnimatorComponent.cpp`
- `src/LamaPon/Components/NavMeshAgentComponent.h`
- `src/LamaPon/Components/NavMeshAgentComponent.cpp`
- `src/LamaPon/Components/NavMeshComponent.h`
- `src/LamaPon/Components/NavMeshComponent.cpp`
- `src/LamaPon/Components/AudioSourceComponent.h`
- `src/LamaPon/Components/AudioSourceComponent.cpp`
- `src/LamaPon/Components/SpriteParticles2DComponent.h`
- `src/LamaPon/Components/SpriteParticles2DComponent.cpp`
- `src/LamaPon/Components/ParticleSystemComponent.h`
- `src/LamaPon/Components/ParticleSystemComponent.cpp`
- `src/LamaPon/Graphics/GraphicsDeviceParticleRendering.cpp`
- `src/LamaPon/Components/MeshRendererComponent.h`
- `src/LamaPon/Components/MeshRendererComponent.cpp`
- `src/LamaPon/Graphics/LitMaterial.h`
- `src/LamaPon/Graphics/LitTextureRequest.h`
- `src/LamaPon/Components/ModelRendererComponent.h`
- `src/LamaPon/Components/ModelRendererComponent.cpp`
- `src/LamaPon/Graphics/ShaderVariants.h`
- `src/LamaPon/Graphics/ShaderVariants.cpp`
- `src/LamaPon/Graphics/ShaderRenderState.h`
- `src/LamaPon/Graphics/ShaderRenderState.cpp`
- `src/LamaPon/Graphics/ShaderDiagnostics.h`
- `src/LamaPon/Graphics/ShaderDiagnostics.cpp`
- `src/LamaPon/Graphics/ShaderManifest.h`
- `src/LamaPon/Graphics/ShaderManifest.cpp`
- `src/LamaPon/Graphics/ShaderProgram.h`
- `src/LamaPon/Graphics/ShaderProgram.cpp`
- `src/LamaPon/Graphics/ShaderCompiler.h`
- `src/LamaPon/Graphics/ShaderCompiler.cpp`
- `src/LamaPon/Graphics/ComputeEffect.h`
- `src/LamaPon/Graphics/ComputeEffect.cpp`
- `src/LamaPon/Graphics/ScreenEffect.h`
- `src/LamaPon/Graphics/ScreenEffect.cpp`
- `src/LamaPon/Graphics/SpriteEffect.h`
- `src/LamaPon/Graphics/SpriteEffect.cpp`
- `src/LamaPon/Graphics/TemporalJitter.h`
- `src/LamaPon/Graphics/TemporalJitter.cpp`
- `src/LamaPon/Graphics/AutoExposureAdaptation.h`
- `src/LamaPon/Graphics/ClusteredLights.h`
- `src/LamaPon/Graphics/ClusteredLights.cpp`
- `src/LamaPon/Graphics/ClusteredLightsBackendState.h`
- `src/LamaPon/Graphics/ShadowMap.h`
- `src/LamaPon/Graphics/ShadowMap.cpp`
- `src/LamaPon/Graphics/ShadowMapBackendState.h`
- `src/LamaPon/Graphics/PbrTextures.h`
- `src/LamaPon/Graphics/PrefilteredEnvironment.h`
- `src/LamaPon/Graphics/ReflectionProbeEnvironment.h`
- `src/LamaPon/Graphics/D3D11ShadowMapState.h`
- `src/LamaPon/Graphics/D3D11ShadowMapState.cpp`
- `src/LamaPon/Graphics/D3D11ClusteredLightsState.h`
- `src/LamaPon/Graphics/D3D11ClusteredLightsState.cpp`
- `src/LamaPon/Graphics/Lighting.h`
- `src/LamaPon/Graphics/Lighting.cpp`
- `src/LamaPon/Graphics/EnvironmentSettings.h`
- `src/LamaPon/Graphics/GraphicsResource.h`
- `src/LamaPon/Graphics/GraphicsDeviceResourceLease.h`
- `src/LamaPon/Graphics/GraphicsDeviceResourceLease.cpp`
- `src/LamaPon/Graphics/GraphicsDeviceResourceLeaseState.h`
- `src/LamaPon/Graphics/GraphicsDeviceState.h`
- `src/LamaPon/Graphics/GraphicsDeviceState.cpp`
- `src/LamaPon/Graphics/GraphicsDeviceApiResources.h`
- `src/LamaPon/Graphics/GraphicsDeviceD3D11Access.h`
- `src/LamaPon/Graphics/GraphicsDeviceD3D12Access.h`
- `src/LamaPon/Graphics/GraphicsDeviceD3D12Access.cpp`
- `src/LamaPon/Graphics/D3D12BackendProvider.cpp`
- `src/LamaPon/Graphics/GraphicsDeviceD3D12Resources.h`
- `src/LamaPon/Graphics/GraphicsDeviceD3D12Resources.cpp`
- `src/LamaPon/Graphics/FrameDebugger.h`
- `src/LamaPon/Graphics/FrameDebugger.cpp`
- `src/LamaPon/Graphics/GpuProfiler.h`
- `src/LamaPon/Graphics/GpuProfiler.cpp`
- `src/LamaPon/Graphics/GraphicsQuality.h`
- `src/LamaPon/Graphics/GraphicsQuality.cpp`
- `src/LamaPon/Graphics/MemorySnapshotCapture.h`
- `src/LamaPon/Graphics/MemorySnapshotCapture.cpp`
- `src/LamaPon/Graphics/PngWriter.h`
- `src/LamaPon/Graphics/PngWriter.cpp`
- `src/LamaPon/Graphics/RenderPipeline.h`
- `src/LamaPon/Graphics/RenderPipeline.cpp`
- `src/LamaPon/Graphics/D3D11RenderServices.cpp`
- `src/LamaPon/Graphics/GraphicsDeviceD3D11Resources.h`
- `src/LamaPon/Graphics/GraphicsDeviceShaderState.h`
- `src/LamaPon/Graphics/GraphicsDeviceD3D11.cpp`
- `src/LamaPon/Graphics/GraphicsRenderServices.h`
- `src/LamaPon/Graphics/RenderTarget.h`
- `src/LamaPon/Graphics/RenderTarget.cpp`
- `src/LamaPon/Graphics/RenderTargetBackendState.h`
- `src/LamaPon/Graphics/D3D11RenderTargetState.h`
- `src/LamaPon/Graphics/D3D11RenderTargetState.cpp`
- `src/LamaPon/Graphics/DxgiTextureLayout.h`
- `src/LamaPon/Graphics/MaterialShaderDrawRequest.h`
- `src/LamaPon/Graphics/D3D12RenderServices.h`
- `src/LamaPon/Graphics/LitMaterialAsset.h`
- `src/LamaPon/Graphics/LitMaterialAsset.cpp`
- `src/LamaPon/Graphics/D3D11GpuProfilerBackend.cpp`
- `src/LamaPon/Graphics/D3D12GpuProfilerBackend.h`
- `src/LamaPon/Graphics/D3D12GpuProfilerBackend.cpp`
- `src/LamaPon/Graphics/D3D12ComputeEffectRenderer.h`
- `src/LamaPon/Graphics/D3D12ComputeEffectRenderer.cpp`
- `src/LamaPon/Graphics/EnvironmentCache.h`
- `src/LamaPon/Graphics/EnvironmentCache.cpp`
- `src/LamaPon/Graphics/GraphicsBackendPackage.h`
- `src/LamaPon/Graphics/GraphicsBackendPackage.cpp`
- `src/LamaPon/Graphics/D3D12DebugDrawingBackend.h`
- `src/LamaPon/Graphics/D3D12DebugDrawingBackend.cpp`
- `src/LamaPon/Graphics/DebugRenderer.h`
- `src/LamaPon/Graphics/DebugRenderer.cpp`
- `src/LamaPon/Graphics/GraphicsBackend.h`
- `src/LamaPon/Graphics/GraphicsBackend.cpp`
- `src/LamaPon/Graphics/D3D12MaterialShaderRenderer.h`
- `src/LamaPon/Graphics/D3D12EnvironmentPrefilter.h`
- `src/LamaPon/Graphics/SpriteRendering.h`
- `src/LamaPon/Graphics/SpriteRendering.cpp`
- `src/LamaPon/Graphics/D3D11Backend.h`
- `src/LamaPon/Graphics/D3D12SpriteRenderer.h`
- `src/LamaPon/Graphics/D3D12EnvironmentPrefilter.cpp`
- `src/LamaPon/Graphics/GraphicsDeviceBuiltInEffectsD3D11.cpp`
- `src/LamaPon/Graphics/GraphicsDeviceBackendOps.cpp`
- `src/LamaPon/Graphics/GraphicsDeviceComposition.cpp`
- `src/LamaPon/Graphics/GraphicsDeviceSpriteFrontend.cpp`
- `src/LamaPon/Graphics/GraphicsDevicePostProcessD3D11.cpp`
- `src/LamaPon/Graphics/GraphicsDeviceSpriteD3D11.cpp`
- `src/LamaPon/Graphics/GraphicsDevice.cpp`
- `src/LamaPon/Graphics/GraphicsDevice.h`
- `src/LamaPon/Graphics/SkeletalModel.h`
- `src/LamaPon/Graphics/SkeletalModel.cpp`
- `src/LamaPon/Graphics/D3D12Backend.h`
- `src/LamaPon/Graphics/LitEffect.h`
- `src/LamaPon/Graphics/LitEffect.cpp`
- `src/LamaPon/Graphics/EnvironmentRenderer.h`
- `src/LamaPon/Graphics/EnvironmentRenderer.cpp`
- `src/LamaPon/Graphics/D3D11Backend.cpp`
- `src/LamaPon/Graphics/D3D12MaterialShaderRenderer.cpp`
- `src/LamaPon/Graphics/GraphicsDeviceShaderEffectsD3D11.cpp`
- `src/LamaPon/Graphics/D3D12RenderServices.cpp`
- `src/LamaPon/Graphics/D3D12SpriteRenderer.cpp`
- `src/LamaPon/Graphics/D3D12Backend.cpp`
- `src/LamaPon/Assets/AssetArchive.h`
- `src/LamaPon/Assets/AssetArchive.cpp`
- `src/LamaPon/Assets/AssetImporter.h`
- `src/LamaPon/Assets/AssetImporter.cpp`
- `src/LamaPon/Assets/AssetPacker.h`
- `src/LamaPon/Assets/AssetPacker.cpp`
- `src/LamaPon/Assets/VboImporter.h`
- `src/LamaPon/Assets/VboImporter.cpp`
- `src/LamaPon/Assets/TextureCache.h`
- `src/LamaPon/Assets/TextureCache.cpp`
- `src/LamaPon/Assets/CollisionMeshImporter.h`
- `src/LamaPon/Assets/CollisionMeshImporter.cpp`
- `src/LamaPon/Assets/DataAsset.h`
- `src/LamaPon/Assets/DataAsset.cpp`
- `src/LamaPon/Assets/ModelLod.h`
- `src/LamaPon/Assets/AssetDatabase.h`
- `src/LamaPon/Assets/AssetDatabase.cpp`
- `src/LamaPon/Assets/CmoImporter.h`
- `src/LamaPon/Assets/CmoImporter.cpp`
- `src/LamaPon/Assets/SdkmeshImporter.h`
- `src/LamaPon/Assets/SdkmeshImporter.cpp`
- `src/LamaPon/Assets/ModelCache.h`
- `src/LamaPon/Assets/ModelCache.cpp`
- `src/LamaPon/Assets/GltfImporter.h`
- `src/LamaPon/Assets/GltfImporter.cpp`
- `src/LamaPon/Assets/FbxImporter.h`
- `src/LamaPon/Assets/FbxImporter.cpp`
- `src/LamaPon/Assets/TextureLoader.h`
- `src/LamaPon/Assets/TextureLoader.cpp`
- `src/LamaPon/Assets/AssetManagerMemory.cpp`
- `src/LamaPon/Assets/AssetManager.h`
- `src/LamaPon/Assets/AssetManager.cpp`
- `src/LamaPon/Core/Api.h`
- `src/LamaPon/Core/Application.h`
- `src/LamaPon/Core/ApplicationLayer.h`
- `src/LamaPon/Core/BuildInfo.cpp`
- `src/LamaPon/Core/BuildInfo.h`
- `src/LamaPon/Core/JobSystem.cpp`
- `src/LamaPon/Core/JobSystem.h`
- `src/LamaPon/Core/Time.cpp`
- `src/LamaPon/Core/Time.h`
- `src/LamaPon/Resources/LamaPon.rc`
- `src/LamaPon/Resources/LamaPonHub.rc`
- `src/LamaPon/Resources/WindowsResource.h`
- `src/LamaPon/LamaPon.h`
- `src/LamaPon/Reactive/Reactive.h`
- `src/LamaPon/Reactive/Reactive.cpp`
- `src/LamaPon/Hub/UpdateChecker.h`
- `src/LamaPon/Hub/UpdateChecker.cpp`
- `src/LamaPon/Scripting/Coroutine.h`
- `src/LamaPon/Scripting/GameModule.h`
- `src/LamaPon/Scripting/GameModuleHost.cpp`
- `src/LamaPon/Scripting/GameModuleHost.h`
- `src/LamaPon/Scripting/GameModule.cpp`
- `src/LamaPon/Scripting/Script.h`
- `src/LamaPon/Audio/AudioSystem.h`
- `src/LamaPon/Audio/AudioSystem.cpp`
- `src/LamaPon/Hub/LearningJourney.h`
- `src/LamaPon/Hub/LearningJourney.cpp`
- `src/LamaPon/Hub/ProjectHub.h`
- `src/LamaPon/Hub/ProjectHub.cpp`
- `src/LamaPon/Web/WebApplication.h`
- `src/LamaPon/Web/WebApplication.cpp`
- `src/LamaPon/Portable/include/LamaPon/LamaPon.h`
- `src/LamaPon/Portable/PortableRuntime.cpp`
- `src/LamaPon/Portable/PortableLog.cpp`
- `src/LamaPon/Portable/PortableWebGame.cpp`
- `src/LamaPon/Web/WebMath.h`
- `src/LamaPon/Web/WebPhysics3D.h`
- `src/LamaPon/Web/WebPhysics3D.cpp`
- `src/LamaPon/Web/WebAudioRuntime.h`
- `src/LamaPon/Web/WebAudioRuntime.cpp`
- `src/LamaPon/Web/WebInput.h`
- `src/LamaPon/Web/WebInput.cpp`
- `src/LamaPon/Web/WebRenderer3D.h`
- `src/LamaPon/Web/WebRenderer3D.cpp`
- `assets/shaders/LamaPonSpriteError.hlsl`
- `assets/shaders/LamaPonSpriteLit.hlsl`
- `assets/shaders/LamaPonSimpleMaterialGraph.hlsli`
- `assets/shaders/LamaPonShaderError.hlsl`
- `assets/shaders/LamaPonScreenDepth.hlsli`
- `assets/shaders/LamaPonLightCulling.hlsl`
- `assets/shaders/LamaPonGeometryExplode.hlsl`
- `assets/shaders/LamaPonRetro3D.hlsl`
- `assets/shaders/LamaPonNoiseSample.hlsl`
- `assets/shaders/LamaPonCustomMaterial.hlsl`
- `assets/shaders/LamaPonTessellatedTerrain.hlsl`
- `assets/shaders/LamaPonWater.hlsl`
- `assets/shaders/LamaPonLit.hlsl`
- `assets/shaders/LamaPonEnvironment.hlsl`
- `tests/fixtures/auxiliary-probe.hlsl`
- `tests/fixtures/baked-gi-probe.hlsl`
- `tests/fixtures/bright-dot.hlsl`
- `tests/fixtures/broken-shader.hlsl`
- `tests/fixtures/compute-input-probe.hlsl`
- `tests/fixtures/compute-probe.hlsl`
- `tests/fixtures/dds-dimension-probe.hlsl`
- `tests/fixtures/depth-probe.hlsl`
- `tests/fixtures/environment-probe.hlsl`
- `tests/fixtures/facing-probe.hlsl`
- `tests/fixtures/material-lighting-probe.hlsl`
- `tests/fixtures/noise-probe.hlsl`
- `tests/fixtures/outline-probe.hlsl`
- `tests/fixtures/particle-probe.hlsl`
- `tests/fixtures/variant-probe.hlsl`
- `tests/fixtures/shader-manifest/custom-entry-compute.hlsl`
- `tests/fixtures/shader-manifest/legacy-compute.hlsl`
- `tests/fixtures/shader-manifest/custom-entry-screen-effect.hlsl`
- `tests/fixtures/shader-manifest/legacy-screen-effect.hlsl`
- `tests/fixtures/shader-manifest/legacy-material.hlsl`
- `tests/fixtures/shader-manifest/custom-entry-material.hlsl`
- `src/LamaPon/Web/default-shell.html`
- `tests/web/test-shell.html`
- `tests/web/PortableSmoke.cpp`
- `tests/web/WebLifecycleTests.cpp`
- `tests/fixtures/web-2d/main.cpp`
- `tests/fixtures/web-model/main.cpp`
- `tests/fixtures/ProjectGameModule/assets/BeginnerScript.cpp`
- `tests/fixtures/ProjectGameModule/assets/ExternalScript.cpp`
- `tests/fixtures/ProjectGameModule/assets/NetworkWorkflowPackage.cpp`
- `tests/fixtures/ProjectGameModule/assets/OllamaPackage.cpp`
- `tests/fixtures/ProjectGameModule/assets/TransitionPackage.cpp`
- `samples/Networking/P2PCooperativeController.h`
- `samples/Networking/P2PTurnBasedController.h`
- `tools/ProjectGameModule/ProjectGameModule.cpp`
- `tools/ProjectGameModule/ScriptRegistry.cpp`
- `samples/GameModule/InterfaceProbe.cpp`
- `samples/GameModule/InterfaceProbe.h`
- `tools/ProjectGameModule/ScriptRegistry.h`
- `tests/BuildInfoTests.cpp`
- `tests/CloudSaveClientTests.cpp`
- `tests/CloudSaveJournalTests.cpp`
- `tests/CloudSaveSynchronizerTests.cpp`
- `tests/DiscordPresenceTests.cpp`
- `tests/GraphicsBackendSelectionTests.cpp`
- `tests/ProjectInstanceTests.cpp`
- `tests/RuntimeTimingTests.cpp`
- `tests/RenderSpatialIndexTests.cpp`
- `tests/CrashSentinelTests.cpp`
- `tests/AssetImporterTests.cpp`
- `tests/AnimationClipTests.cpp`
- `tests/AudioOggProbe.cpp`
- `tests/LogTests.cpp`
- `tests/DiagnosticsTests.cpp`
- `tools/LamaPonCli/BuildDiagnostics.h`
- `tools/LamaPonCli/ProjectPaths.h`
- `tools/LamaPonCli/JsonFiles.h`
- `tools/LamaPonCli/RuntimeTiming.h`
- `tools/LamaPonCli/AnalysisCommands.h`
- `tools/LamaPonCli/ComponentSchemas.h`
- `tools/LamaPonCli/SceneCommands.h`
- `tools/ProjectGameModule/CMakeLists.txt`
- `tools/LamaPonCli/ProjectPaths.cpp`
- `tools/LamaPonCli/JsonFiles.cpp`
- `src/LamaPon/Online/CloudSave.h`
- `src/LamaPon/Online/NetworkTransport.h`
- `src/LamaPon/Online/NetworkRoomDirectory.h`
- `src/LamaPon/Online/NetworkRoomBrowser.h`
- `src/LamaPon/Online/NetworkRoomAdvertiser.h`
- `src/LamaPon/Online/EpicNetworkTransportSdk.h`
- `src/LamaPon/Online/EpicNetworkTransport.cpp`
- `src/LamaPon/Online/NetworkCrypto.h`
- `src/LamaPon/Online/OnlineHttpValidation.h`
- `src/LamaPon/Online/NetworkCrypto.cpp`
- `src/LamaPon/Online/OnlineHttpValidation.cpp`
- `src/LamaPon/Online/NetworkRoomBrowser.cpp`
- `src/LamaPon/Online/NetworkEndpoint.h`
- `src/LamaPon/Online/NetworkSettingsJson.h`
- `src/LamaPon/Online/NetworkPortMapping.h`
- `src/LamaPon/Online/NetworkSceneBridge.h`
- `src/LamaPon/Online/NetworkPortMapping.cpp`
- `src/LamaPon/Online/NetworkSceneBridge.cpp`
- `src/LamaPon/Online/LanNetworkTransport.cpp`
- `src/LamaPon/Online/DirectNetworkTransport.cpp`
- `src/LamaPon/Online/NetworkSession.h`
- `src/LamaPon/Online/NetworkSession.cpp`
- `src/LamaPon/Online/DiscordAuth.h`
- `src/LamaPon/Online/DiscordAuth.cpp`
- `src/LamaPon/Online/DiscordPresence.h`
- `src/LamaPon/Online/DiscordPresence.cpp`
- `src/LamaPon/Online/DiscordPresenceTesting.h`
- `src/LamaPon/Online/EpicNetworkTransportSdk.cpp`
- `src/LamaPon/Online/CloudSaveClient.h`
- `src/LamaPon/Online/CloudSaveClient.cpp`
- `src/LamaPon/Online/CloudSaveJournal.cpp`
- `src/LamaPon/Online/CloudSaveJournal.h`
- `src/LamaPon/Online/CloudSaveSynchronizer.cpp`
- `src/LamaPon/Online/CloudSaveSynchronizer.h`
- `src/LamaPon/Online/WindowsOnlinePlatform.cpp`
- `src/LamaPon/Online/WindowsOnlinePlatform.h`
- `src/LamaPon/Online/OnlineServices.cpp`
- `src/LamaPon/Online/OnlineServices.h`
- `src/LamaPon/Online/OnlineServicesTesting.h`
- `src/LamaPon/Online/OnlinePersistenceCoordinator.cpp`
- `src/LamaPon/Online/OnlinePersistenceCoordinator.h`
- `src/LamaPon/Editor/Editor.h`
- `src/LamaPon/Editor/Editor.cpp`
- `src/LamaPon/Editor/EditorGuiRenderer.h`
- `src/LamaPon/Editor/EditorGuiRenderer.cpp`
- `src/LamaPon/Editor/EditorModelPreviewRenderer.h`
- `src/LamaPon/Editor/EditorModelPreviewRenderer.cpp`
- `src/LamaPon/Editor/D3D11EditorGuiRenderer.h`
- `src/LamaPon/Editor/D3D11EditorGuiRenderer.cpp`
- `src/LamaPon/Editor/D3D11EditorModelPreviewRenderer.h`
- `src/LamaPon/Editor/D3D11EditorModelPreviewRenderer.cpp`
- `src/LamaPon/Editor/D3D12EditorGuiRenderer.h`
- `src/LamaPon/Editor/D3D12EditorGuiRenderer.cpp`
- `src/LamaPon/Editor/D3D12EditorModelPreviewRenderer.h`
- `src/LamaPon/Editor/D3D12EditorModelPreviewRenderer.cpp`
- `src/LamaPon/Editor/ScriptEditorDetection.h`
- `src/LamaPon/Editor/ScriptEditorDetection.cpp`
- `src/LamaPon/Editor/DebugCaptureFiles.h`
- `src/LamaPon/Editor/DebugCaptureFiles.cpp`
- `src/LamaPon/Editor/EditorExtensionRegistry.h`
- `src/LamaPon/Editor/EditorExtensionRegistry.cpp`
- `src/LamaPon/Editor/PersistencePanelState.h`
- `src/LamaPon/Editor/WebExportJob.h`
- `src/LamaPon/Editor/WebExportJob.cpp`
- `src/LamaPon/Editor/DataAssetSchema.h`
- `src/LamaPon/Editor/DataAssetSchema.cpp`
- `src/LamaPon/Editor/UiRecorder.h`
- `src/LamaPon/Editor/UiRecorder.cpp`
- `src/LamaPon/Editor/SimpleMaterialShaderGenerator.h`
- `src/LamaPon/Editor/SimpleMaterialShaderGenerator.cpp`
- `src/LamaPon/Editor/NetworkConnectionPanel.h`
- `src/LamaPon/Editor/NetworkConnectionPanel.cpp`
- `src/LamaPon/Editor/NetworkProjectSettingsPanel.cpp`
- `src/LamaPon/Editor/OnlineDiagnosticsPanel.h`
- `src/LamaPon/Editor/OnlineDiagnosticsPanel.cpp`
- `src/LamaPon/Editor/ServiceDiagnosticsPanel.h`
- `src/LamaPon/Editor/ServiceDiagnosticsPanel.cpp`
- `src/LamaPon/Editor/SceneLoadingScreenEditor.h`
- `src/LamaPon/Editor/SceneLoadingScreenEditor.cpp`
- `src/LamaPon/Editor/GameExportDialog.h`
- `src/LamaPon/Editor/GameExportDialog.cpp`
- `src/LamaPon/Editor/FrameDebuggerPanel.h`
- `src/LamaPon/Editor/FrameDebuggerPanel.cpp`
- `src/LamaPon/Editor/ExeIconTool.h`
- `src/LamaPon/Editor/ExeIconTool.cpp`
- `src/LamaPon/Editor/ShaderProperties.h`
- `src/LamaPon/Editor/ShaderProperties.cpp`
- `src/LamaPon/Editor/ProfilerPanel.h`
- `src/LamaPon/Editor/ProfilerPanel.cpp`
- `src/LamaPon/Editor/ProfileAnalyzerPanel.h`
- `src/LamaPon/Editor/ProfileAnalyzerPanel.cpp`
- `src/LamaPon/Editor/BgmLoopPanel.h`
- `src/LamaPon/Editor/BgmLoopPanel.cpp`
- `src/LamaPon/Editor/MemoryProfilerPanel.h`
- `src/LamaPon/Editor/MemoryProfilerPanel.cpp`
- `src/LamaPon/Editor/PhysicsDebuggerPanel.h`
- `src/LamaPon/Editor/PhysicsDebuggerPanel.cpp`
- `src/LamaPon/Editor/VehicleParametersPanel.h`
- `src/LamaPon/Editor/VehicleParametersPanel.cpp`
- `src/LamaPon/Editor/UIComponentInspectors.h`
- `src/LamaPon/Editor/UIComponentInspectors.cpp`
- `src/LamaPon/Editor/PackageNativeDependencies.h`
- `src/LamaPon/Editor/PackageNativeDependencies.cpp`
- `src/LamaPon/Editor/PackageManager.h`
- `src/LamaPon/Editor/PackageManager.cpp`
- `src/LamaPon/Editor/GameModuleBuilder.h`
- `src/LamaPon/Editor/GameModuleBuilder.cpp`
- `src/LamaPon/Editor/GameExporter.h`
- `src/LamaPon/Editor/GameExporter.cpp`
- `src/LamaPon/Editor/EditorLayerShared.h`
- `src/LamaPon/Editor/EditorLayerExtensions.cpp`
- `src/LamaPon/Editor/EditorLayerAnalysis.cpp`
- `src/LamaPon/Editor/EditorLayerPackages.cpp`
- `src/LamaPon/Editor/EditorLayerSettings.cpp`
- `src/LamaPon/Editor/EditorLayerProject.cpp`
- `src/LamaPon/Editor/EditorLayerViewport.cpp`
- `src/LamaPon/Editor/EditorLayerAnimationTools.cpp`
- `src/LamaPon/Editor/EditorLayerAssets.cpp`
- `src/LamaPon/Editor/EditorLayer.cpp`
- `src/LamaPon/Editor/EditorLayer.h`
- `src/LamaPon/Editor/EditorLayerInspector.cpp`
- `packages/src/discord-presence-sdk/DiscordSocialPresenceBackend.cpp`
- `packages/src/discord-presence-sdk/DiscordSocialPresenceBackend.h`
- `packages/src/discord-presence-sdk/DiscordSocialSdkImplementation.cpp`
- `packages/src/network-session-workflow/NetworkProfile.h`
- `packages/src/network-session-workflow/NetworkSessionController.cpp`
- `packages/src/ollama-ai/OllamaChat.cpp`
- `packages/src/ollama-ai/OllamaClient.h`
- `packages/src/ollama-ai/OllamaPolicy.h`
- `packages/src/ollama-ai/OllamaProfile.h`
- `packages/src/ollama-ai/OllamaWorker.h`
- `packages/src/scene-transition-showcase/SceneTransitionController.cpp`
- `packages/src/scene-transition-showcase/SceneTransitionOverlay.cpp`
- `packages/src/scene-transition-showcase/SceneTransitionOverlay.h`
- `packages/src/scene-transition-showcase/SceneTransitionSchema.h`
- `packages/src/scene-transition-showcase/SceneTransitionAssets.h`
- `packages/src/scene-transition-showcase/shaders/LamaPonSceneTransition.hlsl`
- `packages/src/build_package.py`
- `cmake/BuildInfoData.cpp.in`
- `cmake/Version.h.in`
- `CMakeLists.txt`
- `cmake/DirectXTK.cmake`
- `cmake/GenerateBuildInfo.cmake`
- `cmake/LamaPonDistribution.cmake`
- `cmake/LamaPonGitInfo.cmake`
- `cmake/LamaPonMsvcDependencies.cmake`
- `cmake/LamaPonNetworking.cmake`
- `cmake/LamaPonWeb.cmake`
- `cmake/TestProjectGameModule.cmake`
- `cmake/WriteGameSettings.cmake`
- `samples/Game/Main.cpp`
- `samples/GameModule/SampleGameModule.cpp`
- `samples/GameModule/TargetRangeGame.cpp`
- `samples/Hub/Main.cpp`
- `samples/Sandbox/Main.cpp`
- `tools/LamaPonCli/ComponentSchemas.cpp`
- `tools/LamaPonCli/AnalysisCommands.cpp`
- `tests/web/CMakeLists.txt`
- `tools/GenerateSampleAudio.ps1`
- `tests/MsvcDependencyTests.py`
- `tests/BgmLoopPanelTests.cpp`
- `tests/RunBrowserTests.py`
- `tools/RebuildAndInstallEditor.bat`
- `tests/CliSceneCommandTests.py`
- `tests/GameExportDialogTests.cpp`
- `tests/EditorWebExportTests.py`
- `tests/ExportedStartupTests.cpp`
- `tests/RuntimeServicesTests.cpp`
- `tests/PersistenceTests.cpp`
- `tests/NoiseTests.cpp`
- `tests/DocumentMigrationTests.cpp`
- `tests/MemorySnapshotTests.cpp`
- `tests/EditorExtensionRegistryTests.cpp`
- `tests/TemporalJitterTests.cpp`
- `tests/ReactiveTests.cpp`
- `tests/ScriptPrefsTests.cpp`
- `tests/GameModuleTests.cpp`
- `tests/ProceduralMeshTests.cpp`
- `tests/AssetDatabaseTests.cpp`
- `tests/GameModuleBuilderTests.cpp`
- `tests/FrameDebuggerTests.cpp`
- `tests/TextCacheTests.cpp`
- `tests/BillboardTests.cpp`
- `tests/InputSystemTests.cpp`
- `tests/CliAnalysisCommandTests.cpp`
- `tests/LifecycleTests.cpp`
- `tests/NetworkSceneTests.cpp`
- `tests/NetworkSessionTests.cpp`
- `tests/NetworkWorkflowPackageTests.h`
- `tests/DataAssetTests.cpp`
- `tests/PhysicsTimingTests.cpp`
- `tests/DirectNetworkTests.cpp`
- `tests/TransformTests.cpp`
- `tools/audit_game_module.py`
- `tools/editor_web_export.py`
- `tests/ProjectGameModuleLoadTests.cpp`
- `tests/UIComponentInspectorTests.cpp`
- `tests/ShaderDiagnosticsTests.cpp`
- `tests/GraphicsResourceTests.cpp`
- `tests/OnlineAuthTests.cpp`
- `tests/FbxImporterTests.cpp`
- `tests/GltfImporterTests.cpp`
- `tests/SceneTransitionTests.cpp`
- `tests/PackageNativeDependencyTests.cpp`
- `tests/PackageManagerTests.cpp`
- `tests/RenderRegressionTests.cpp`
- `tests/ProjectHubTests.cpp`
- `tests/RenderFailureTests.cpp`
- `tests/ShaderCacheTests.cpp`
- `tests/ProjectMigrationTests.cpp`
- `tools/RebuildAndInstallEditor.ps1`

## 規約

正式な規約は[CONTRIBUTING.md](../CONTRIBUTING.md#コードスタイル)を参照してください。

- コメントは必要な意味・用途・契約に絞ります。
- 引数以外の変数説明は、宣言の直前に独立した行で置き、可能な限り20文字以内にします。
- 関数の引数は概要の括弧内で`引数名: 意味`を対応付け、引数リスト内へコメントを入れません。
- `if`、`switch`、`for`、`while`、`return`、関数呼び出しなどの説明も、対象の文の直前に置きます。
- 概要と補足は原則1文・1行とし、補足は寿命、所有権、呼び出し順、座標系、失敗時の扱い、互換性など改修に必須の情報に限定します。
- ローカル変数、ループ変数、ラムダの引数、構造化束縛の各名前にも説明が必要です。
- 開発履歴、不要なログ、自明な逐語説明、装飾、コメントアウトした旧コードは残しません。
- 設定コメント、ライセンス、解析・整形ツールへの指示は保持します。

```cpp
// 次の位置を求めます(position: 現在位置, speed: 毎秒の移動量, deltaTime: 経過秒数)。
float Advance(float position, float speed, float deltaTime)
{
    // 今回の移動量
    const float distance = speed * deltaTime;
    return position + distance;
}

// フレーム提示の完了時刻
const auto presentation =
    std::chrono::steady_clock::now();

// 描画フレーム番号
for (std::uint32_t frame = 0; frame < frameCount; ++frame)
{
    // 撮影フレームだけ画像を取得します。
    if (frame + 1 == frameCount)
    {
        CaptureFrame();
    }
}
```

関数前の概要が長くなる場合も、引数名と意味の対応を失わないようにします。20文字は個々の説明の目安で、概要全体の上限ではありません。契約は宣言側へ、実装固有の注意は定義側へ置き、同じ説明を重複させません。

## 現在の棚卸し

[是正候補一覧（CSV）](comment-audit.csv)はUTF-8 BOM付きで、前回の走査時点では16,352行のデータでした。以降のコメント修正で行番号と候補数が変わっているため、CSVは現時点の完全な残件一覧ではなく、更新前の候補を探す手掛かりとして扱ってください。全件終了前に再走査して一覧を更新します。

| 分類（前回走査時点） | 候補数 | 判断する内容 |
|---|---:|---|
| 変数の説明不足 | 6,879 | 宣言直前の役割・単位など |
| ループ変数の説明不足 | 292 | 反復対象・番号の意味 |
| 引数の説明不足 | 804 | 関数前の括弧内で各名前に対応する説明 |
| 関数概要の不足 | 554 | 用途と必要な契約 |
| 変数に近接する20文字超の説明 | 1,428 | 短い役割説明と必要な補足の分離 |
| ループ変数に近接する20文字超の説明 | 120 | 同上 |
| 20文字超の引数説明 | 6 | 引数名に対応する意味の短縮 |
| 複数行のコメント群 | 2,312 | 概要と補足の必要性 |
| 複数の句点を含むコメント群 | 1,209 | 各文の必要性と配置 |
| 80文字超のコメント群 | 1,333 | 過剰な解説の短縮 |
| 履歴語・装飾の抽出候補 | 0 | 今回の抽出パターンで検出されたもの |
| C++・HLSLの未抽出構文の確認範囲 | 663 | ラムダ、複雑な宣言、マクロなど |
| 宣言抽出の対象外言語の確認範囲 | 19 | CMake、PowerShell、HTMLなど |
| 埋め込みHLSLの確認範囲 | 16 | C++文字列内のシェーダーの宣言・コメント |
| 保持対象コメント | 17 | 設定・ライセンス・ツール指示など |
| ファイル別の記録 | 694 | 走査範囲と抽出数 |

前回走査時の変数・ループ変数・引数・関数概要の不足候補は合計8,529件でした。候補は未完了の確認・是正対象であり、確定した違反件数ではありません。抽出を改善して複数行戻り値・構築子・連続する補足・HLSL属性を扱い、引数説明の長さは各引数の意味だけで判定するため、以前の抽出数との差を是正件数として扱いません。複数行・複数句点の分類には「概要1行と必須補足1行」の適切なコメント群も含まれるため、行数だけを理由に削除しません。CSV全体にはファイル記録や保持対象も含まれます。

## 一覧の使い方

CSVの列は`path,line,end_line,category,symbol,excerpt`です。行番号は前回走査時点のもので、後の編集により変わっています。現在のコード内で該当する記述を探し、周辺コードと照合してください。

| category | 意味 |
|---|---|
| `variable_comment_missing_candidate` | 変数説明の不足候補 |
| `loop_variable_comment_missing_candidate` | ループ変数説明の不足候補 |
| `parameter_comment_missing_candidate` | 引数説明の不足候補 |
| `function_summary_missing_candidate` | 関数概要の不足候補 |
| `*_comment_length_review` | 20文字を超える近接説明の確認候補 |
| `comment_multiline_review` | 複数行のコメント群 |
| `comment_multi_sentence_review` | 複数句点を含むコメント群 |
| `comment_long_review` | 80文字を超えるコメント群 |
| `manual_cpp_syntax_review` | C++・HLSLの抽出漏れを確認する範囲 |
| `embedded_hlsl_review` | C++のraw文字列内から抽出したHLSLの確認範囲 |
| `manual_declaration_review` | 宣言を機械抽出していない範囲 |
| `protected_comment_preserve` | 保持対象コメント |
| `file_inventory` | ファイルの行数・コメント所在行・宣言候補数 |

## 抽出範囲と限界

対象はGit管理下の自前コードの`.cpp`、`.h`、`.hpp`、`.c`、`.inl`、`.hlsl`、`.hlsli`、`.py`、`.ps1`、`.bat`、`.cmake`、`.html`、`.rc`、`CMakeLists.txt`、C++の`.cpp.in`・`.h.in`です。`third_party/`、配布ZIP・DLL、バイナリアセット、JSON、Markdown、ビルド生成物は機械走査の対象外です。

C++・HLSLでは文字列・文字リテラル・raw文字列をマスクし、数値の桁区切りを文字リテラルと区別します。HLSLを含むraw文字列は別途抽出し、C++の行番号に対応させて宣言とコメントを確認します。JSONや単なるコード例の文字列、設定ブロックとツール指示は維持します。

宣言抽出は構文パターンによる候補抽出で、コンパイラーによる全数解析ではありません。複数行の型、構造化束縛、関数ポインター、ラムダ、演算子、複雑なテンプレート、マクロ展開には抽出漏れや誤検出があります。候補に出ていない箇所も、規約適合を確認済みとは扱いません。

引数は関数前のコメントに`名前:`があるかを確認します。対応するヘッダーで説明を検出できた公開定義は、その宣言の説明を参照して重複候補を一部除外します。同名のオーバーロードなどを完全には識別できないため、実際の宣言・定義との対応は確認が必要です。

PythonではtokenizeとASTを使用します。代入先には再代入も含まれます。docstringは実行時のメタ情報で、CLIの説明として使うものもあるため、その文字列を維持しています。CMake・PowerShell・bat・HTMLの宣言は候補抽出しておらず、HTML内のJavaScriptやEM_JSなどの埋め込み言語も意味・宣言の確認が必要です。

## 残作業

1. 宣言抽出対象外の19ファイルは手作業で読み直しました。引き続き不足する説明を修正し、意味と契約の確認を完了します。
2. メンバー・定数・ローカル変数・ループ変数・構造化束縛の各名前に、コードの役割を示す説明を追加します。
3. ラムダ、複雑な宣言、埋め込み言語、CMake・PowerShellなどの抽出漏れを確認します。
4. 長い解説を意味で判断して短縮し、必要な使用例を文書へ移します。
5. 更新後に再走査し、候補を確認してから全件完了を判断します。

名前だけの機械的な言い換えや、一律の「値」「処理結果」といった説明では完了扱いにしません。

## 検証

- C++・HLSLなど667ファイルについて、コメント以外のトークンが開始時点と一致しています。
- 整理した埋め込みHLSLは、その実行コードの一致を確認したうえで元の文字列へ正規化し、C++側のトークンも開始時点と照合しています。
- 同じ範囲で、保持対象コメントの文字列と順序も一致しています。
- その他29ファイルでは、PythonのASTまたはコメント除去後のコードを比較し、処理の一致を確認しています。
- C++・HLSLの通常コメントには、行末・文中のコメントが残っていないことを確認しました。
- PowerShell2ファイルの構文エラーは0件です。
- 空白チェックを実施しています。
- CSVの集計・ファイル・行番号と文書の参照を照合しています。

667ファイルと29ファイルにはRC2ファイルが重複し、走査対象の合計は694ファイルです。Releaseビルドは全ターゲット成功しました。CTest全78件の初回実行では76件が成功し、失敗したPackageIndexとProjectGameModuleBuildAndLoadは修正後に個別で再実行して成功しました。全78件を修正後に一括再実行した結果は未確認です。全件の意味確認と是正は未完了です。
