# Changelog

## Unreleased

### 描画API設定

- Project SettingsのGraphicsへ`Auto` / `DirectX 11` / `DirectX 12 Experimental`の選択を追加。既定は従来どおりDirectX 11で、変更は次回起動時に反映する。
- Editor / CLIなど完全なrendererが必要な起動経路では、DirectX 12 Experimentalを選んでも安全にDirectX 11へフォールバックする。将来のD3D11 / D3D12バックエンド分離に備えて起動時の選択経路を追加。
- 書き出したゲームでDirectX 12 Experimentalを選べるD3D12 Backendのbootstrap起動を追加。D3D12の初期化に失敗した場合は資源をすべて解放してDirectX 11の通常起動へフォールバックする。起動経路の描画要件を表す`GraphicsStartupProfile`を追加し、既定の`FullRenderer`では従来どおりDirectX 11を選ぶ。
- DirectX 12 Backendへtexture資源（2D texture生成・mip単位更新・Shader Resource View）と、GPUの完了までresourceとdescriptorを保持する遅延解放を追加。`BeginSpritePass`は実効APIがDirectX 12の場合にD3D12の既定Sprite pipelineへ送り、DirectXTK SpriteBatchと同じ座標・UV・blend・scissor規則で描画する。D3D12 bootstrapのゲームは起動ロゴを表示でき、custom sprite shaderは既定pipelineへフォールバックする。
- DirectX 12 Experimentalのゲームでも起動Sceneを読み込み、Game Module・入力・音声・Simulationを通常どおり更新して、Sprite／Text／Imageなどの2D/UI Componentを実画面へ描画する。D3D11固有GPU資源しか持たないModel Componentは初期化時に安全に保留し、対応pipeline完成まで描画をスキップする。
- DirectX 12 ExperimentalへAPI-neutralな最小3D描画要求と専用pipelineを追加。起動SceneのCube／Sphere／Cylinder／Plane／Procedural Meshを、Transform・ベースカラー・albedo texture・深度・半透明を反映して2D/UIの背後へ描画する。Model、影、HDR/post-process、custom shaderは引き続き安全にスキップする。
- DirectX 12の最小3D pipelineへSceneのAmbient Lightと最大4灯のDirectional Lightを接続。描画定数は今後Point／Spot LightやMaterial情報を追加できるconstant buffer経路へ移行した。
- DirectX 12の最小3D pipelineへSceneのPoint Light（最大16灯）とSpot Light（最大8灯）を接続。D3D11の従来経路と同じ距離減衰とコーン減衰を、Lambert拡散へ掛けて描画する。
- DirectX 12で3D ParticleSystemのBillboard／Horizontal quadを描画。既定particle texture、深度読み取り、通常アルファ／加算ブレンドに対応し、custom particle shaderは既定pipelineへ安全にフォールバックする。
- DirectX 12の基本3D pipelineへroughness／metallic／normal／occlusion／emissiveのMaterial係数とtextureを接続。未指定mapは中立値へフォールバックし、既存Sceneの表示を維持する。
- glTF／FBXのモデルprimitiveへAPI非依存なCPU頂点・index・LOD mirrorを保持。D3D11のimmutable bufferを維持したまま、D3D12など別Backendが同じインポート結果からGPU資源を構築できる境界を追加した。
- DirectX 12のModelRendererでglTF／GLBを読み込み、ノード姿勢・アニメーション・スキニング・LOD・primitive別Material係数を反映して実描画する。CPU skinningによる移行実装で、D3D11の既存GPU skinningは変更しない。
- DirectX 12のglTF／GLB Modelで、モデル内蔵（GLB bufferView・base64 data URI）と外部ファイルのPNG／JPEG textureを読み込み、albedo／normal／metallicRoughness／occlusion／emissiveを描画へ接続。D3D11 SRVとmodel cacheを介さずBackend-neutralなhandleとして取り込み、Material上書き時のalbedo／normal継承もD3D11と同じ規則に揃えた。DDS textureは引き続き未対応。
- FBX ImporterをD3D11 DeviceなしでもCPU幾何・LOD・skin・animationとBackend-neutralなPNG／JPEG textureを生成できる構造へ分離し、DirectX 12のModelRendererでFBXを実描画する。D3D11では従来どおりGPU資源とmodel cacheを生成する。
- DirectX 12で方向光カスケード・スポット配列・ポイントキューブ用のShadowMap資源を生成し、共通Scene traversalからMesh／Modelを深度専用pipelineへ描画する。通常描画先は各shadow pass後に復元し、半透明Particleはcasterから除外する。
- DirectX 12の基本3D pipelineで方向光カスケードShadowMapを3x3 PCFサンプリングし、カスケード境界のブレンド、深度／法線bias、shadow strengthを反映する。影を無効にしたSceneとの画素比較をWARP回帰テストへ追加した。
- `GraphicsDevice`のAPI固有資源所有を抽象interface化し、D3D11のEffect・Shader・Sprite・Shadow・render serviceを単一の具象世代へ集約。停止・再生成・破棄を共通境界から呼ぶ構造へ移行。
- Effect・Shader cache・Shadow・Environment・Clustered LightsなどDevice世代に属する高水準資源をD3D11内部所有へ分離。非同期Shader workerをAssetManagerより先に停止し、再初期化時に旧Device資源と未消費Screen Effect queueを安全に破棄する。
- `GraphicsDevice`のnative D3D11 Device / Context / CommonStates / view resolverを非公開化し、D3D11描画島とテストだけがSDK非公開bridgeから利用する境界へ移行。API 64 Game Module向けの旧public binary symbolを維持し、Game Module APIを65へ更新。
- `GraphicsDevice`の全instance状態をSDK非公開の固定opaque stateへ集約。公開layoutを1ポインタに固定し、将来のBackend追加や内部cache変更でGame Module ABIを繰り返し壊さない境界を追加。公開layout変更に伴いGame Module APIを64へ更新。
- `RenderTarget`のDirectX 11 texture / view / viewportをSDK非公開のopaque Backend stateへ分離。resizeはnative資源とneutral handleが全て完成してから一括反映し、失敗時は直前の有効な描画先を維持する。公開layout変更に伴いGame Module APIを63へ更新。
- Device / Context / SwapChainとバックバッファ資源の所有を`D3D11Backend`へ分離し、`GraphicsDevice`をBackend選択の窓口に変更。API固有型を含まない共通Backend契約と、選択結果・フォールバック理由を追加。
- 最終合成時のバックバッファbindを共通Backend契約へ移し、RendererがD3D11のバックバッファRTVを直接参照しない経路へ変更。
- Dear ImGuiの描画API固有処理を`EditorGuiRenderer`へ分離し、実効APIに応じてD3D11実装を生成するEditor側の選択経路を追加。
- Editor UIのテクスチャ参照生成を`EditorGuiRenderer`へ集約し、Assetと表示用RenderTargetのD3D11 SRV変換をD3D11実装内へ分離。
- Editorのオフスクリーン描画先操作を`GraphicsDevice`の共通入口へ集約し、ViewportとプレビューからD3D11 Device / Contextの受け渡しを削減。
- オフスクリーン描画先の作成・bind・clear・表示用確定を共通Backend契約へ移し、`RenderTarget`のD3D11直呼びAPIをBackend内部へ限定。
- 深度プリパスのbindとSSR用深度コピーを共通Backend契約へ移し、呼び出し側からD3D11 Contextの受け渡しを削減。
- SSRとTAAのカラー履歴コピーを共通Backend契約へ移し、シーンとポスト処理からD3D11 Contextの受け渡しを削減。
- 自動露出の非同期輝度readbackと次回用転送を共通Backend契約へ移し、ポスト処理からD3D11 Contextの受け渡しを削減。
- 方向・スポット・ポイント影の資源作成と描画先操作を共通Backend契約へ移し、シーンからD3D11 Contextの受け渡しを削減。
- Forward+のクラスタライト更新を共通Backend契約へ移し、通常のシーン描画からD3D11 Contextの受け渡しを削減。
- primary描画先とviewportの退避・復元をopaqueな共通Backend契約へ移し、レンダーテクスチャとベイク処理からD3D11 output stateの操作を削減。
- リフレクションプローブとGIの6面ベイクを`EnvironmentRenderer`の同期workflowへ集約し、SceneからD3D11の面ターゲット生成・clear・反転コピー・畳み込み・SH readbackを分離。
- 環境キャッシュ復元と焼き込みGIの3D texture生成を`GraphicsDevice`のD3D11互換facadeへ集約し、SceneからDirectX 11 Deviceの直接利用を除去。不正なGI形状はbase64 decode前に安全に拒否する。
- 一時描画先・UI viewport・GPU計測区間をscope guardで復元し、レンダーテクスチャ／reflection probe／GI bakeの描画例外後もprimary outputと再入状態を維持する。
- GPUメモリ容量・予算のDXGI adapter照会を共通Backend契約へ移し、`GraphicsDevice`の性能統計からD3D11 Deviceへの直接依存を除去。
- Collider・grid・gizmo等の形状生成を共通`DebugRenderer`へ残し、線分のGPU送信だけをBackend生成のD3D11 sinkへ分離。
- エディター補助表示・UI・最終画面転送・Computeに残っていたGPU計測区間もscope guard化し、例外後のprofiler stackを維持。
- `GpuProfiler`をAPI非依存のfacadeとし、D3D11 timestamp / pipeline queryをBackend所有driverへ分離。未接続・非対応時は安全なno-opと空結果へフォールバックする。
- `GraphicsDevice`の再初期化時に自身が所有する旧Device由来のShader・State・Texture等を破棄してからBackendを作り直し、途中の初期化失敗も空状態へ戻す寿命境界を追加。
- API非依存の強所有`GraphicsTextureHandle` / `GraphicsBufferHandle` / `GraphicsViewHandle`とBackend世代境界を追加し、白テクスチャと共有インスタンスバッファをD3D11 Backend生成へ移行。従来のD3D11ポインターAPIは互換facadeとして維持。
- API非依存の2D texture記述・subresource転送・mip範囲View生成をBackend契約へ追加。built-in／WIC／文字／DDS assetをneutral handle所有へ移し、DirectX 11 SRVは同じhandleから解決する互換mirrorとして維持。
- 段階的texture uploadをtransactionalにし、View生成失敗時に進捗だけが完了する問題を修正。個別Invalidateは全usage variantを破棄し、読み込み中の旧世代結果がcacheへ復活しないようにした。
- 車両パラメータープレビューのモデル送信を`EditorModelPreviewRenderer`へ分離し、DirectXTK11のContext / CommonStates / BasicEffect操作をD3D11実装内へ隔離。
- `GraphicsSettings`、`GraphicsDevice`、`Scene`、`EnvironmentRenderer`、`GpuProfiler`、`DebugRenderer`のABI変更、および共通Backend契約の拡張に伴い、Game Module APIを32へ更新。
- 描画resource handle契約と`GraphicsDevice` / `GraphicsBackend`のABI変更に伴い、Game Module APIを33へ更新。SDK反映後はゲーム用DLLの再ビルドが必要。
- texture resource契約と`TextureAsset` / `AssetManager` / `RuntimeServices`のABI変更に伴い、Game Module APIを34へ更新。SDK反映後はゲーム用DLLの再ビルドが必要。
- Texture/Viewを単一の不変snapshotとしてatomicに公開し、段階upload中も同じ世代のresourceだけを描画する。ImGuiとSpriteBatchは遅延描画完了までresourceを保持する。
- `TextureAsset` / `TextTextureAsset`のresource公開ABI変更に伴い、Game Module APIを35へ更新。SDK反映後はゲーム用DLLの再ビルドが必要。
- Sceneと外部renderer向けの`GraphicsDeviceResourceLease`を追加。SceneやEditor GUI等の旧Device資源所有者が生きている再初期化は、現在のBackendやAssetManagerを破棄する前に拒否する。
- Scene破棄時は非同期Scene読み込みのcancelとjoinを明示し、AssetManagerより先にworkerを終了する。
- `Scene` / `GraphicsDevice`の寿命契約変更に伴い、Game Module APIを36へ更新。SDK反映後はゲーム用DLLの再ビルドが必要。
- Graphics Backend停止前に、AssetManagerの非同期モデル準備とmaterial shaderの暖機を明示的に完了させる。再初期化失敗時もAudioは保持する。
- モデル準備の新規受付を終了境界で閉じ、進行中のworkerと結果回収をjoinする。再初期化後はAsset root・upload/cache予算・Input action設定を復元する。`AssetManager` / `RuntimeServices`のABI変更に伴い、Game Module APIを37へ更新。
- Device / ContextやD3D11 resource resolverなどの互換facade実装を`GraphicsDeviceD3D11.cpp`へ分離し、共通の`GraphicsDevice`実装からD3D11 Backend型への直接依存を除去。
- オフスクリーン描画先、名前付きRenderTexture、Scene最終合成の実装を`GraphicsDeviceComposition.cpp`へ分離し、Backend選択・寿命管理と画面合成の責務を切り分け。
- resource生成、frame、shadow、cluster、描画先状態のAPI中立な委譲処理を`GraphicsDeviceBackendOps.cpp`へ分離し、Backend操作境界を集約。
- SpriteBatch、CommonStates、UIシザー等のDirectX 11固有資源をprivateなopaque stateへ集約し、将来の描画APIごとの資源所有境界を追加。
- SpriteBatchの基本開始・終了、UIシザー、起動画面描画を`GraphicsDeviceSpriteD3D11.cpp`へ分離し、共通Device実装からD3D11 Spriteフロントエンドの責務を切り出し。
- 環境・クラスタ・Litとエラー代替用の組み込みEffect生成を`GraphicsDeviceBuiltInEffectsD3D11.cpp`へ分離し、D3D11 Shader資源の生成責務を集約。
- Material、Sprite、Screen、Compute Shaderキャッシュの内部状態型をSDK非公開の`GraphicsDeviceShaderState.h`へ集約し、Device寿命管理とShader実装を別翻訳単位へ分けられる境界を追加。
- カスタムSprite、Material、Screen、Compute Shaderの読み込み・再読み込み・実行を`GraphicsDeviceShaderEffectsD3D11.cpp`へ分離し、共通Device本体をBackend寿命と設定管理へ縮小。
- 名前付きRenderTextureの安定表示面をAPI非依存`GraphicsViewHandle`として公開し、同サイズではidentityを維持、resize・解放・再初期化では安全に世代を更新するsidecar cacheを追加。RenderTargetの途中初期化は無効状態として再試行する。
- Light2DとSprite描画request・pass設定をD3D11非依存の`SpriteRendering.h`へ分離し、次段階のBackend中立Sprite描画経路で共有できる公開型を追加。
- move-onlyの`SpriteRenderPass`と非所有`SpriteDrawContext`を追加し、API非依存handleからSpriteを送信できる経路をD3D11実装へ接続。pass中は再初期化をleaseで拒否し、legacyとの二重Beginやstale viewを安全に拒否する。
- Sprite passのシザー切り替えでもblendとカスタムShaderを維持し、Deferred描画が完了するまでtexture viewを強所有するowner/token境界を追加。
- `GraphicsDevice`の公開レイアウト変更に伴い、Game Module APIを38へ更新。SDK反映後はゲーム用DLLの再ビルドが必要。
- `GraphicsBackend`の表示handle契約、`GraphicsDevice`の公開API、`RenderTarget`の完成状態追加に伴い、Game Module APIを39へ更新。SDK反映後はゲーム用DLLの再ビルドが必要。
- API非依存Sprite passと描画requestの公開に伴い、Game Module APIを40へ更新。SDK反映後はゲーム用DLLの再ビルドが必要。
- ローディング画面と起動ロゴをAPI非依存Sprite passへ移行し、高レベル描画手順をD3D11実装ファイルから分離。
- デバッグオーバーレイをAPI非依存Sprite passへ移行し、文字textureの取得から描画完了までBackend世代を安全に固定。
- Sceneと全2D / UI Componentの描画入口を`SpriteDrawContext`へ移行し、通常描画・カスタムShader・Sprite Mask・Light2D・UIシザーからDirectX 11のSpriteBatch受け渡しを除去。
- `Component::OnRender2D`を含む公開描画ABIの変更に伴い、Game Module APIを41へ更新。SDK反映後はゲーム用DLLの再ビルドが必要。
- 旧D3D11 `SpriteBatch`公開facadeを非公開化し、新規コードの2D描画入口を`SpriteRenderPass`へ統一。旧facadeに依存するAPI 41 Game Moduleが読み込み時に明確なAPI不一致案内へ到達できるよう、旧バイナリシンボルは1互換期間だけprivate shimとして維持。
- 旧SpriteBatch facadeの公開API変更に伴い、Game Module APIを42へ更新。SDK反映後はゲーム用DLLの再ビルドが必要。
- 名前付きRenderTextureのraw D3D11 SRV入口を非公開化し、表示resourceの取得を強所有の`GraphicsViewHandle`へ一本化。API 42 Game Module用の旧バイナリシンボルは1互換期間だけprivate shimとして維持。
- raw RenderTexture view入口の公開API変更に伴い、Game Module APIを43へ更新。SDK反映後はゲーム用DLLの再ビルドが必要。
- 共有instance vertex bufferの更新とbindを`GraphicsBufferHandle`経由のBackend共通契約へ移し、Mesh / Model rendererからraw `ID3D11Buffer`の受け渡しを除去。
- raw instance buffer取得・resolver入口を非公開化し、API 43 Game Module用の旧バイナリシンボルを1互換期間だけprivate shimとして維持。公開Backend / Device契約の変更に伴い、Game Module APIを44へ更新。
- 白テクスチャのraw D3D11 SRV参照をneutral view handle経由へ移行し、公開`WhiteTexture()`を非公開化。API 44 Game Module用の旧バイナリシンボルは1互換期間だけprivate shimとして維持し、Game Module APIを45へ更新。
- 例外を投げるraw D3D11 SRV resolverを非公開化し、描画時の解決をstale handleを安全に拒否する非throwing経路へ統一。API 45 Game Module用の旧バイナリシンボルをprivate shimとして維持し、Game Module APIを46へ更新。
- 外部callerのない`Clusters()`と`SpriteErrorPlaceholder()`を非公開化し、API 46 Game Module用の旧バイナリシンボルをprivate shimとして維持。Game Module APIを47へ更新。
- pixel shaderの連続SRV bindを`GraphicsViewHandle`とfallbackを受けるBackend共通契約へ移し、Particle描画からraw D3D11 SRV解決とbindを除去。公開Backend / Device契約の変更に伴い、Game Module APIを48へ更新。
- Lit描画のtexture一式を`LitTextureRequest`で強所有するneutral契約へまとめ、Mesh rendererからraw D3D11 SRV解決を除去。公開Device契約の追加に伴い、Game Module APIを49へ更新。
- Meshのinstance batch keyへPBR / custom texture、発光・遮蔽値、custom vector、shader keywordを含め、異なるmaterialが代表Meshのtexture requestで描かれる誤batchを防止。可変長fieldの長さもhashして境界を保持。
- 非スキニングModelの共通Lit経路を`LitTextureRequest`へ移行し、外部PBR / custom textureとDirectXTK11モデル内蔵textureをBackend世代付きhandleで描画完了まで保持。raw SRVはモデル読込時のprivate import境界に限定。
- glTF / FBXスキニングモデルの内蔵textureを読込時にneutral handleへ取り込み、外部texture上書きも`LitTextureRequest`へ統一。公開`SkeletalModel::Draw`からraw D3D11 texture引数を除き、`SkeletalPrimitive` / Draw契約変更に伴いGame Module APIを50へ更新。
- Particle Systemのquad描画をAPI非依存requestと描画サービスへ分離し、ComponentからDirectXTK11のEffect / Batch / InputLayout所有とDevice / Context / CommonStates操作を除去。公開Component layout変更に伴いGame Module APIを51へ更新。
- 既存D3D11 Shader Resource Viewのneutral handle取り込みをBuffer / StructuredBufferとTexture3Dへ拡張し、Clustered LightingとBaked GIの後続移行に必要な世代・強所有境界を整備。
- API非依存のimmutable Texture3D生成とRGBA16F formatを追加し、Baked GI係数を3個の世代付き`GraphicsViewHandle`としてアップロードできる新経路を追加。旧raw戻り値は互換shimとして維持し、公開Backend / Device契約の変更に伴いGame Module APIを52へ更新。
- Sceneと`LightingState`のBaked GI所有をneutral viewへ移し、3枚の世代・Texture3D形式を描画直前にまとめて検証するLit bridgeを追加。旧raw uploadはprivate互換shimへ移し、公開layout / API変更に伴いGame Module APIを53へ更新。
- Forward+の3本のStructuredBuffer viewを`LightingState`でも世代付きneutral handleとして強所有し、Lit bridgeで形式・要素数・Backend世代を一括検証してから反映する構造へ移行。公開layout変更に伴いGame Module APIを54へ更新。
- RenderTargetのSSAO結果・SSRカラー履歴・Hi-Z深度を世代付きneutral handleとして公開し、`LightingState`からraw D3D11 viewを除去。Resize時の3本一括更新とLit bridgeの形式・寸法・mip検証を追加し、旧raw getterをprivate互換shimへ移行。公開layout変更に伴いGame Module APIを55へ更新。
- 平行光・スポット・ポイントのShadowMapとボリューメトリック光の深度入力を世代付きneutral handleへ移行。array/cube形状・解像度・ライト番号をGPU反映前に一括検証し、旧raw getterをprivate互換shimへ移行。公開layout変更に伴いGame Module APIを56へ更新。
- 共通Sky IBLのsource・スペキュラ・放射照度viewを世代付きneutral handleへ移行。事前フィルタ結果の一括取込・再利用とcube形状検証を追加し、source/key/cache更新を例外安全にした。旧raw入口をprivate互換shimへ移行し、公開layout変更に伴いGame Module APIを57へ更新。
- Reflection Probeのベイク結果とprimary／secondary IBL viewを世代付きneutral handleへ移行。cache・ベイク結果の一括取込、Lit bridgeのcube形状検証、Backend再初期化後のstale検出と再ベイクを追加。旧raw入口をprivate互換shimへ移行し、公開layout／Backend契約変更に伴いGame Module APIを58へ更新。
- TAAの履歴カラーと深度入力を世代付きneutral handleへ移行。RenderTarget生成時に他の画面空間viewと一括取込し、TAA描画前にBackend世代・RGBA16F履歴・R24深度の形式と寸法を検証。公開layout変更に伴いGame Module APIを59へ更新。
- Sky描画のcubemapをneutral viewで受けるGraphicsDevice facadeを追加し、stale・foreign・2D viewはprocedural Skyへ安全にフォールバック。SSR深度ピラミッドとGI probeベイクもDevice境界へ集約し、Sceneからraw D3D11参照と`EnvironmentRenderer`直接依存を除去。旧raw入口はprivate互換shimとし、Game Module APIを60へ更新。
- TAAとボリュメトリックライトのScene入力をAPI非依存型へ移し、target固有の履歴・深度・前フレーム状態は`RenderTarget`内部で注入。全ポスト処理を専用D3D11 island内の`GraphicsDevice` facade経由に統一し、`RenderPipeline`から`EnvironmentRenderer`直接依存を除去。Deviceのraw renderer取得口とRenderTargetの旧post-process入口はprivate互換shimへ移行し、公開frame layout変更に伴いGame Module APIを61へ更新。
- `RenderTarget`の現在カラーと表示確定面を世代付きneutral handleとして公開し、全ポスト処理のping-pongでnative資源と同時に更新。名前付きRenderTextureとEditor GUIも同一handleを使う経路へ統一し、旧raw getterはprivate互換shimへ移行。公開layout変更に伴いGame Module APIを62へ更新。

### ファイル名の統一

- サンプルのアニメーションとPrefabを英語ファイル名へ変更し、`.meta`のGUIDを維持して参照を更新。
- 新規Scene・Material・データ・フォルダーの既定名を英語に統一。日本語のGameObjectからPrefabを作る場合も英語の保存名を提案する。
- 開発規約に英語／ローマ字のファイル命名を明記。日本語の表示・コメントとUTF-8パスの対応は維持する。

### 物理と描画の時刻同期

- Windows Runtimeに`Scene::PhysicsTiming()`を追加。固定更新で進むゴーストやリプレイを、LateUpdateでRigidbodyと同じ描画時刻へ合わせられる。
- 物理時計を専用クラスへ分離し、追いつき上限で捨てた時間をCLIの実行状態へ出力。一時停止時の補間位置を保持し、固定更新中に刻み幅が混在する問題を修正。
- 更新順序のドキュメントを修正。FPS変動・停止再開・時間制限・Scene初期化・設定変更の回帰テストを追加。
- Game Module APIを16へ更新。SDK反映時に既存のゲーム用DLLの再ビルドが必要。

### エクスポート画面

- Windows（EXE）とWeb（HTML）の出力形式を選択可能に。Webは互換性検査とビルドを別プロセスで実行し、進行状況・失敗理由・ログ・完成した出力先を画面から確認できる。
- Webビルド環境はPC内に保存し、完成したHTMLパッケージだけを出力先へ反映する。出力ダイアログとWebジョブの状態をEditorLayerから分離。

### 品質・保守性の改善

- 配布ゲームの起動判定、Script終了時の解放、Scene設定のリセット、EventBusの例外復帰を修正。
- BGMパネル、UIコンポーネントのInspector、描画用空間索引、実行時サービス、CLIのScene／Prefabコマンドを分離。
- 責務分割に伴う公開クラスのレイアウト変更でGame Module APIを14から15へ更新し、
  物理と描画の時刻同期で16、描画API設定で17、Backend分離で18、
  バックバッファ契約の追加で19、オフスクリーン描画先契約の追加で20、
  オフスクリーン深度操作の追加で21、カラー履歴操作の追加で22、
  自動露出の輝度readback契約の追加で23、影マップ操作の追加で24、
  クラスタライト更新契約の追加で25、描画先状態契約の追加で26、
  環境ベイクworkflowの集約で27、環境資源facadeの追加で28、
  GPU計測scopeの追加で29、GPUメモリ統計契約の追加で30、
  Debug描画sinkの分離で31、GPU計測driverの分離で32、
  描画resource handle契約の追加で33、texture resource境界の追加で34、
  texture snapshot公開の追加で現在の35へ更新。
- MSVC／Ninjaのヘッダー依存検出を修正。ローカライズされた出力と既存ビルドの依存情報再取得に対応。
- Web入力の登録解除・例外処理・ログを改善し、SDK構成、ライセンス同梱、Windows／Webの回帰検査を整備。

### LamaPon 0.1.0 開発版

- Windows・C++20・DirectX 11によるゲームエンジン。
- LamaPon Hub、Editor、Runtime、Game、CLIの開発環境。
- Scene・Prefab・C++ Script・リアクティブAPI・2D／3D描画・物理・音声。
- 学習用プロジェクト、サンプルゲーム、日本語ドキュメント。
- Windows向けゲームパッケージとWebGLエクスポート。
- MITライセンス、コントリビューションガイド、第三者ライセンス表記。
