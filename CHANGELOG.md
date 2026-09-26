# Changelog

## Unreleased

### プレイヤーホストのオンライン通信

- Windows x64へ2〜4人のホスト主導P2P通信を追加。SDKなしで同じPC・LAN接続を利用でき、任意のEOS SDK対応ビルドでは端末ログイン、接続支援、中継、部屋ID共有に対応する。EOS SDKと製品設定は別途必要。SDK 1.19.2.1でWindows Debug・ReleaseのビルドとSDK初期化・終了試験を確認。異なる回線の2台でのEOS実通信は未検証。
- `NetworkSession`、`NetworkSceneBridge`、`NetworkIdentityComponent`を追加。位置・回転・拡縮・有効状態・ゲーム用データ、登録Prefabの生成／削除、所有権付き操作、途中参加の全状態取得、退出とタイムアウトを扱う。参加者側の物理とゲームScriptを抑制し、停止時にSceneを復元する。
- プロジェクト設定へ接続方式・ゲーム識別・同期Prefab登録、再生中の部屋作成／参加／終了と接続状態表示を追加。Windows書き出しへEOS DLLの配布確認を追加。既存プロジェクトは接続を開始しなければ従来の動作を維持する。
- C++ Scriptの通信API、協力プレイ例、利用ガイド、実TCP通信・不正データ・Scene同期の回帰試験を追加。公開型の変更に伴いGame Module APIを77へ更新し、ゲーム用DLLの再ビルドが必要。

### ウィンドウサイズ

- Windowsゲームの実行中にスクリプトからクライアント領域を変更・取得できる`SetWindowSize`／`WindowSize`を追加。エディター再生中はゲームビューへ反映し、停止時に元の表示設定へ戻す。サイズ変更時の描画バッファ再作成を次の描画前にまとめ、最小化中の描画を停止。

### デバッグ・解析ツール

- エディターの「ウィンドウ」へ「解析」サブメニューを追加し、UnityのWindow > Analysisに相当するパネルをまとめた。どのパネルも既定では閉じており、開いている間だけ記録する。
- 「プロファイラー」を追加。フレームごとのCPU時間を最上位区間で色分けしたタイムラインから過去フレームを選び、呼び出し階層（合計・自己時間・割合・呼び出し回数）と自己時間順の一覧を表示する。記録の一時停止、1フレーム送り、履歴数（240〜6000）の変更、`.lamapon/profiles/`への保存に対応。
- CPUプロファイラーを階層計測へ拡張。`LAMAPON_PROFILE_SCOPE`は開いている区間の子として記録され（スレッドごとに管理）、`ProfileSample`へ`parent`／`depth`を追加。Sceneの`Update`／`FixedUpdate`／`Physics`／`LateUpdate`／`Visibility`と、GPU計測区間（シャドウ、3D描画、ポスト処理など）もCPU区間として並ぶ。`profile.json`は`version: 2`（`parent`／`depth`付き）になり、version 1も引き続き読める。CLIとエディターのruntime状態JSONの`profiler.samples`にも`depth`／`parent`を追加。
- 「プロファイル分析」を追加。記録を区間ごとに中央値・平均・最大・95%・自己時間で集計し、2つの記録（現在の記録または保存済みJSON）を中央値の差で比較する。フレーム範囲の指定、区間ごとのフレーム時間グラフに対応。
- 「メモリプロファイラー」を追加。テクスチャ・モデル・文字テクスチャ・レンダーテクスチャ・オーディオ・アニメーション・データアセット・先読みファイルを資源ごとに記録し、分類別の合計、並べ替え、スナップショットの保存（`.lamapon/memory/`）と比較（追加・解放・変化）を表示する。`TextureAsset`へ読み込み時のGPU量`gpuBytes`を追加。
- 「フレームデバッガー」を追加。描画イベント（Mesh／Model Renderer、パーティクル、スプライト、UI、インスタンス描画）をGPU区間ごとに一覧にし、選んだイベントより後ろの描画を飛ばした途中の絵をScene View／Game Viewに表示する。詳細欄に描画パス、マテリアル・Shader・テクスチャ、合成方式・カリング、頂点数・三角形数・インスタンス数を表示する。`Component::DescribeDrawEvent`で描画内容を報告する。
- 描画パスの区間名をPIX／RenderDoc向けのイベントとして記録。D3D11は`ID3DUserDefinedAnnotation`（キャプチャツール接続時のみ）、D3D12はコマンドリストのイベント（PIXの旧形式）で、途中でコマンドリストを閉じるフレームでも入れ子を閉じる。エディターのScene View／Game View／カメラプレビューもGPU区間に分けた。
- 「物理デバッガー」を追加。Scene Viewへ接触点・法線（衝突／トリガー）・速度・角速度を描き、ボディ（種類・質量・速さ・スリープ・接触数）と接触（組・位置・法線・めり込み）を一覧にする。`Scene::SetPhysicsDebugCaptureEnabled`／`PhysicsDebugContacts`を追加（既定は無効で負荷なし）。
- 「ImGuiデバッガー」（Dear ImGuiのMetrics/Debugger）を追加。
- CLIへ`profile analyze`／`profile compare`／`memory summary`／`memory compare`を追加し、保存した記録をエディターと同じ計算でJSONに出力する。
- `EditorPanelDefinition`へ「ウィンドウ」メニューのサブメニュー名`windowMenuGroup`を追加。新規プロジェクトの`.gitignore`へ`.lamapon/profiles/`と`.lamapon/memory/`を追加し、既存プロジェクトでも保存先フォルダーへ除外用の`.gitignore`を自動で置く。
- 公開構造体のレイアウト変更（`Component`、`Scene`、`ProfileScope`／`ProfileSample`、`TextureAsset`、`GpuProfilerBackend`）に伴いGame Module APIを75へ更新。ゲーム用DLLの再ビルドが必要。

### エンジンの表示バージョン

- エンジンの表示を `0.1.0` のような番号から「ブランチ名 @ コミット」（例: `community/main @ 932b08c3a1b2`）へ変更。LamaPon Hub右下、エディターのウィンドウタイトル、ヘルプとサポート、サポート情報のコピー、クラッシュレポート、起動ログに表示し、未コミットの変更を含むビルドには `-dirty` を付ける。
- ブランチ名・コミット・コミットメッセージを構成時だけでなくビルドのたびにGitから取り直す `LamaPonBuildInfo` ターゲットを追加。CMakeを再構成しなくても最新のコミットが表示され、変更が無ければ再コンパイルしない。CIのdetached HEADでは `GITHUB_HEAD_REF` / `GITHUB_REF_NAME`、明示指定には `LAMAPON_BUILD_BRANCH` を使う。
- `LamaPon/Core/BuildInfo.h`（`GetBuildInfo` / `FormatBuildLabel` / `FormatBuildDetails`）と `LamaPonCli version` を追加。`build-info.json` へ `branch` / `commit` / `commitShort` / `commitSubject` / `dirty` を追加（`buildRevision` は互換のため残す）。
- `Version.h` の `BuildRevision` を削除し、`MAJOR.MINOR.PATCH` はパッケージの必要エンジン版・プロジェクト移行・更新確認の互換判定専用とした。ビルド識別子が必要なコードは `GetBuildInfo()` を使う。

### セキュリティ

- パッケージ一覧（`packages/index.json`）の各エントリへZip全体のSHA-256（`sha256`）を追加し、エディターはダウンロードしたZipが一致した場合だけ展開するようにした。`sha256`の無いエントリは一覧に表示しない。「パッケージを作成...」と`build_package.py`が出力する一覧用JSONにも`sha256`を含める。
- CIのActionsをコミットSHAで固定し、checkout／setup-pythonの版をワークフロー間でv7へ揃えた。Web CIのemsdkもタグとコミットで固定し、checkoutは`persist-credentials: false`でビルドやテストへトークンを残さない。Release作成の書き込み権限はジョブ単位へ限定し、固定したActionsはDependabotで更新を確認する。

### 描画API設定

- Material Asset Inspectorへ、編集中の標準Lit／custom shader／texture／parameterを即時反映する専用プレビュー球を追加。Shader compileエラーには「エラー箇所を開く」を表示し、D3DCompilerのfile・line・columnをVS Code／Visual Studioへ渡して該当位置へ移動できるようにした。
- Shader作成へTint／Emission／Rim Light／UV Scroll／Mask Texture／Alpha Clipの簡易ノード生成を追加。生成HLSLからGPU register宣言を共通includeへ隠し、Shader Manifestのtarget省略時はCustomParametersとt7〜t10の空き領域へ自動バインドする。
- DirectX 12の組み込みLit描画でMesh Rendererのカリング指定を反映。D3D11のGeometricPrimitive::Drawと同じく既定で裏面を捨て、`SetCullMode`の上書きを通常・深度・影・インスタンス描画へ適用する。D3D12自前のCube／Sphere／Cylinderの三角形の向きをD3D11に揃え、PlaneもD3D11と同じ厚さ0.05の箱にした。D3D11に無かったShadow map用のrasterizer biasも外し、閉じていないProcedural Meshの見え方と影をD3D11と一致させる。
- DirectX 12の利用者向け対応状況をruntime feature parity到達として整理し、Experimental継続理由を低レベルAPI設計差、GPU／driver差、全環境未検証に更新。Project Settingsの警告とbootstrap期の古いコメントも現状へ揃えた。
- DX10 DDSのtypeless storageをDirectXTK互換のshader-readable viewへ正規化し、R8／R16 SNORM、R11G11B10 float、RGB9E5 shared exponentをD3D11／D3D12共通ローダーへ追加。公開texture formatの末尾追加に伴いGame Module APIを74へ更新。
- DirectXTK11が認識するlegacy DDS形式（10:10:10:2、16-bit UNORM、B5／B4、alpha、signed bump map、packed RGB／YUY2、D3D9 numeric FourCC）をD3D11／D3D12共通ローダーへ追加。driver依存のYUY2はRGBA8へ安全にCPU変換し、公開texture formatの末尾追加に伴いGame Module APIを73へ更新。
- `DirectX12Experimental`を標準の`Application`／`GraphicsDevice`初期化入口から直接起動できるようにし、bootstrap期の`GraphicsStartupProfile`によるopt-in制限を解除。旧profile列挙値とoverloadはGame Moduleのソース互換用に残し、D3D12初期化失敗時だけD3D11へ安全にフォールバックする。
- D3D11／D3D12共通のDDS読み込みを、sRGB、BGRX8、R8／RG8、BC2／BC4／signed BC4・BC5／BC6H／BC7、16／32-bit floatへ拡張。DX10 headerに加えてDXT2～DXT4、ATI1／BC4、signed BC、D3D9 float FourCCも解釈し、全形式のrow／block layoutとD3D12 native SRV生成をWARPで検証する。公開`GraphicsTextureFormat`の末尾追加に伴いGame Module APIを72へ更新。
- DirectX 12 BackendでAPI非依存の動的頂点buffer更新と入力assemblerへのbindを実装。D3D11の`WRITE_DISCARD`相当として更新ごとにupload resourceをrenameし、記録済みdrawと旧handleの寿命をGPU完了まで分離する。空更新、slot／offset検証、Backend再初期化後のstale handle拒否をWARPとdebug layerで検証する。D3D11 Effect専用のpixel shader resource直接bindは、D3D12ではroot signature固有の描画サービスを使う設計であることも共通契約とガイドへ明記した。
- DirectX 12でDDSの2D array、cube array、volume textureを読み込む。D3D11／D3D12共通のDDS解析でresource次元（2D／2D array／cube／cube array／3D）とarray数・depthを保ち、D3D12はTexture2DArray／TextureCubeArray／Texture3Dのnative resourceと同じ次元のSRVで公開する（従来の2Dとcubeの入口も同じ解析を使う）。2D arrayの2枚目、volumeの奥側の層、cube arrayの2個目の+Y面をMaterial custom shaderの自由枠から読んだ画像がD3D11と一致することをWARPで検証する。あわせて、D3D11でDirectXTKが読んだDDSのvolume textureを取り込むときに2D textureとして扱って読み込みに失敗していた問題を修正した。
- CLIのruntime／screenshot描画もプロジェクトのDirectX 12 Experimental設定を使用し、D3D12初期化失敗時だけD3D11へフォールバックするように変更。Project Settingsのフォールバック表示も実際の起動結果に合わせた。
- DirectX 12へtimestamp queryとpipeline statistics queryによるGPU Profiler backendを追加。Editor／Debug Overlayでフレーム・入れ子区間・GPU投入頂点やshader invocationを表示し、スクリーンショット等でcommand listを途中送信するフレームも安全に処理する。
- DirectX 12 ExperimentalでLamaPon Editorを起動できるようにし、Dear ImGui、Viewport／アセットのテクスチャ表示、モデルプレビュー、grid／bounds／light gizmoのデバッグラインをD3D12へ対応。D3D12初期化に失敗した場合は従来どおりD3D11へ安全にフォールバックする。
- DirectX 12 Experimentalの有効化、起動時の選択、対応範囲、D3D11互換の考え方、既知の差、フォールバックと切り分け手順をまとめた利用者向けガイドを追加。
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
- DirectX 12のSpot Lightへ最大4灯の配列ShadowMapを接続し、ライトごとの射影、3x3 PCF、深度／法線bias、shadow strengthを直接光へ反映する。casterを無効にした画像との画素比較で遮蔽を検証する。
- DirectX 12のPoint LightへキューブShadowMapを接続し、6面の深度から復元した比較値と5タップPCF、深度bias、shadow strengthを直接光へ反映する。影の有無による画素差をWARP回帰テストで検証する。
- DirectX 12でAPI-neutralな`RenderTarget`のLDRカラー、ping-pongカラー、表示用カラー、深度資源とSRVを生成。offscreenへのclear／Sprite／3D描画、表示用資源への確定、再サンプリングの経路をWARPとdebug layerで検証する。HDRとpost-processは後続段階で対応する。
- DirectX 12の現在の描画先（バックバッファ／offscreenカラー／深度のみ）を`GraphicsOutputState`で退避・復元する。Cameraの名前付きRenderTextureをLDRで描画・公開し、元の出力へ戻ってSpriteから参照できる経路をWARPで検証する。D3D11専用post-processはD3D12で安全にスキップする。
- DirectX 12のゲームメインカメラを直接バックバッファへ描く暫定経路から、Scene用LDR RenderTargetへ描画してSprite pipelineで画面サイズへ合成する経路へ移行。UIは従来どおり3D合成後のバックバッファへ重ねる。
- DirectX 12のoffscreen深度専用パスとshader-readableな深度コピーを追加。DSVからコピー用stateを経てSRVへ戻し、Sprite pipelineから再サンプリングできることをWARPとdebug layerで検証する。SSAO／SSR本体は未移植のため安全に無効化する。
- DirectX 12 RenderTargetへSSR用カラー履歴とTAA用時系列履歴を追加。現在カラーを独立したSRVへコピーし、履歴の有効性と再投影行列をAPI-neutralなfacadeから公開する。両履歴の再サンプリングをWARPで検証する。
- DirectX 12のoffscreenカラーと履歴をRGBA16FのHDR資源へ移行。Sprite／Particle／Primitive pipelineは現在のRGBA8バックバッファまたはRGBA16F offscreen形式に応じたPSOを選び、深度専用pipelineもD24プリパスとD32 ShadowMapを分離する。
- DirectX 12のHDR Scene合成へACES近似の最終fullscreen passを追加。1.0を超えるRGBA16Fの色をRGBA8バックバッファへトーンマップし、単純クリップではない出力をWARP画素テストで検証する。
- DirectX 12の最終合成へ既存のカラーグレーディング設定を接続。露出、コントラスト、彩度、色温度、Tint、VignetteとTone Mapping無効化をD3D11のPSToneMapと同じ式でroot constantsから適用し、D3D11に無いgamma変換を外して両APIの出力を揃えた。自動露出の測光はD3D12では未対応のため、補正0段で合成する。
- DirectX 12のpost-processへBloomを移植。D3D11の`PSBloom`と同じしきい値・9tap・強さ・半径で、offscreenのcurrent colorを読んでpost colorへ書き、両者を交換する。Scene合成とCameraのRenderTextureで品質設定のBloom有効／無効も反映し、D3D11とD3D12の画素比較をWARPで検証する。
- DirectX 12のpost-processへFXAAを移植し、トーンマップも最終合成からD3D11と同じpost-process列（Bloom → トーンマップ → FXAA）へ移した。最終合成はD3D11と同じ単純な転写になり、CameraのRenderTextureにもトーンマップが掛かる。トーンマップの彩度とFXAAの縁検出はD3D11の`LamaPonEnvironment.hlsl`と同じRec.601の輝度係数を使い、Bloom・トーンマップ・FXAAのD3D11／D3D12画素比較をWARPで検証する。
- DirectX 12へ自動露出を移植。D3D11と同じく1/4解像度で対数輝度を測って2x2平均で1x1まで縮め、readback bufferをGPUを待たずに次フレームで読んで幾何平均輝度から順応する。順応の式はD3D11と共有し、左右で明るさの異なる画面の露出補正が幾何平均輝度の期待値になることをWARPで検証する。あわせて、破棄したDirectX 12 RenderTargetの深度などviewを持たない資源を即時解放せず、実行中のframeが終わるまで保持するよう修正した。
- DirectX 12へTAAの再投影・近傍クランプ・履歴合成passを移植。main pass直後の深度コピー、前フレームのHDRカラーとビュー射影行列を使い、解決済みカラーをBloomより前に生成して次フレームの履歴へ確定する。
- DirectX 12へScreen Outlineを移植。main passの深度から距離差とビュー空間法線を再構成し、既存の色・太さ・深度／法線しきい値でトーンマップ後のSceneへ輪郭を重ねてからFXAAへ渡す。
- DirectX 12へカメラMotion Blurを移植。RenderTargetごとの前フレーム行列とmain passの深度から画面上の移動量を求め、既存の強度・最大半径・品質別サンプル数でHDR Sceneをブラーする。
- DirectX 12へDepth of Fieldを移植。main passの深度から既存と同じ薄レンズ近似のCoCを求め、焦点帯の外側を深度対応の黄金角サンプリングでぼかしてからBloomへ渡す。
- DirectX 12へSSAOを移植。深度プリパスのコピーからD3D11の`PSAmbientOcclusion`／`PSAmbientOcclusionBlur`と同じ再構成法線・黄金角サンプリング・深度対応ブラーで半解像度の遮蔽を求め、Mesh／Modelの基本3D pipelineで環境光項だけへ掛ける。同じSceneから求めた遮蔽textureのD3D11／D3D12画素一致と、接地部だけが暗くなり環境光を0にすると画像が変わらないことをWARPで検証する。`PrimitiveDrawRequest`の公開レイアウト変更に伴いGame Module APIを69へ更新する。
- DirectX 12へSSR（画面空間反射）を移植。深度プリパスのコピーからD3D11の`PSReflectionDepthLinearize`／`PSReflectionDepthDownsample`と同じHi-Z深度ピラミッドを作り、基本3D pipelineでD3D11の`EvaluateScreenSpaceReflection`と同じHi-Zトラバーサル、前フレームカラーへの再投影、縁／距離／向き／粗さのフェードで反射を求め、キューブマップが無いときのD3D11と同じくF0の重みで環境光へ足す。環境光にもD3D11と同じ`1 - metallic * 0.5`を掛け、頂点変換もD3D11と同じくWorldの後にViewProjectionを掛ける順へ揃える。三角形の頂点順を揃えたProcedural MeshのSceneで合成画像のD3D11／D3D12画素一致と、SSRを切ると元の画像へ戻ることをWARPで検証する。`PrimitiveDrawRequest`の公開レイアウト変更に伴いGame Module APIを70へ更新する。
- DirectX 12へScreen Space Lens Flareを移植。D3D11と同じ1/4解像度RGBA16Fの2枚へ3段のストリークをping-pongし、高輝度抽出、色収差、ゴースト、ハローと合わせてBloom後のHDR Sceneへ合成する。設定した方向数・角度・長さを反映し、D3D11／D3D12の画素比較をWARPで検証する。
- DirectX 12へVolumetric Lightを移植。画面深度から復元したカメラレイに沿って既存のDirectional Lightカスケードシャドウをサンプルし、D3D11と同じHenyey-Greenstein位相関数、ディザ、距離正規化でHDR Sceneへ散乱光を足す。行列群はroot signature上限を避けるupload constant bufferで渡し、Scene Compositionで有効化・無効化した画素差をWARPとdebug layerで検証する。
- DirectX 12でRGBA8／BGRA8／BC1／BC3／BC5の2D DDSとミップ列を読み込み、共通texture upload経路からSprite／Modelへ渡せるようにした。DirectX 11はDirectXTKの従来DDS対応を維持し、未対応のarray／cube／volume／formatは誤った2D textureとして公開せず安全に拒否する。WARPで非圧縮と各BC形式の実サンプリングを検証する。
- DirectX 12でCMO Modelを描画。Visual Studio 3D Starter KitのCMOをD3D11 DeviceなしでCPU幾何・skin・内蔵albedo／normal textureへ読み込む。Material上書きが無いときはDirectXTK Effectと同じDiffuse／Emissive ColorとSpecular Powerから近似した粗さで、上書き中はD3D11の共通Lit経路と同じくコンポーネントのMaterialへ内蔵textureを補って描く（内蔵Diffuse ColorはPreserve Embedded Material Colorのときだけ掛ける）。DirectX 11は従来のDirectXTK loaderを維持し、内蔵textureの適用とMaterial上書き時のD3D11／D3D12合成画像一致をWARPで検証する。
- DirectX 12でSDKMESH Modelを描画。DirectX SDKのSDKMESH（version 101／200）をD3D11 DeviceなしでCPU幾何・内蔵albedo／normal textureへ読み込み、DirectXTKと同じ頂点宣言、16／32bit index、subsetのVertexStart、三角形strip、Materialの既定値で解釈して、CMOと同じくMaterial上書きの有無に応じた規則で描く。合成SDKMESHをメモリから読み、subsetごとの頂点範囲とMaterialを一時ファイルなしで検証する。
- DirectX 12でVBO Modelを描画。Windows 8 ResourceLoading sample由来の単純なposition／normal／UVと16bit indexをD3D11 DeviceなしでCPU幾何へ読み込み、境界を計算して共通Model描画へ渡す。DirectX 11はDirectXTKの従来loaderとBasicEffectを維持する。
- DirectX 12でScreenEffectのcustom shaderを描画。プロジェクトの`VSMain`／`PSMain`をD3D11と同じ`b0`、`t0`〜`t3`、`s0`の契約と4つの差し込み地点で実行し、保存の監視、compile失敗時の診断と直前の正常版の保持もD3D11に揃える。bright-dot、深度（depth-probe）、補助texture（auxiliary-probe）の合成画像がD3D11と一致することと、壊れたShaderを拒否することをWARPで検証する。
- DirectX 12でComputeEffectのcustom shaderを実行。プロジェクトの`CSMain`をD3D11と同じ`b0`、`t0/t1`、`s0`、`u0`の契約で名前付きRenderTextureのUAVへ書き、入出力のresource stateを切り替えて、保存の監視とcompile失敗時の診断もD3D11に揃える。compute-probeと入力textureを読むcompute-input-probeの結果をSpriteで表示した画像がD3D11と一致することと、壊れたShaderや寸法の無い要求を拒否することをWARPで検証する。
- DirectX 12でMesh RendererのMaterial custom shaderを描画。プロジェクトの`VSMain`／`PSMain`／`GSMain`をD3D11のLitEffectと同じ`b0`〜`b3`、`t0`〜`t25`、`s0`／`s1`の契約で実行し、描画状態の宣言、keyword、影や深度プリパス、保存の監視、compile失敗時のマゼンタ表示と診断もD3D11に揃える。未対応の入力は同じ次元のnull SRVで無効として渡す。雛形、光源とcustom texture・vectorを読む半透明の検査Shader、keyword付きvariant、壊れたShaderを並べた合成画像がD3D11と一致することをWARPで検証する。
- DirectX 12でglTF／GLB／FBXのModel RendererのMaterial custom shaderを描画。D3D11と同じくDirectXTK SkinnedEffectのper-pixel lighting 4 bone頂点シェーダーと同じ計算の内蔵頂点シェーダーで骨を変形し、`PSSkinnedMain`をMaterial上書きの有無・半透明pass・両面描画・カリング宣言の規則もD3D11に揃えて描く。骨を動かしたglTFへ雛形と壊れたShaderを割り当てた合成画像がD3D11と一致することをWARPで検証する。
- DirectX 12でSceneのSkyを描画。D3D11の`PSSky`と同じ天頂・地平線・地面のグラデーション、明るさ、朝昼夜モードの太陽円盤とにじみを、Sprite pipelineのfullscreen passで深度を読まず書かずに3Dの背後へ描く。太陽を正面に置いた朝昼夜モードの合成画像がD3D11と一致することをWARPで検証する。
- DirectX 12でDDSのcube texture（6面、RGBA8／BGRA8／BC1／BC3／BC5とミップ列）を読み込み、TextureCubeのSRVとして公開。SkyのcubemapをD3D11と同じ向きと明るさで描き、`IsSampleableCubeView`もD3D12のTextureCubeを判定する。メモリ上で組み立てたcube DDSを一時ファイルなしで両APIへ取り込み、Skyの合成画像が一致すること、2Dとcube、面の欠けたDDSを取り違えないことをWARPで検証する。
- DirectX 12でSkyのcubemapによる環境光（IBL）を描画。D3D11と同じGGXスペキュラ（128px、8ミップ）と放射照度（16px）の事前畳み込みをCompute Shaderで作ってSkyごとに使い回し、基本3D描画はsplit-sum近似の環境反射と環境拡散を、Material custom shaderは`t3`／`t6`と`EnvironmentParameters`をD3D11と同じ規則で受け取る。メモリ上のcubeから両APIで有効な事前畳み込み済みTextureCubeを生成できることと、同じ畳み込み経路を使うリフレクションプローブの合成画像がD3D11と一致することをWARPで検証する。
- DirectX 12の基本3D描画へSceneの霧を追加。Mesh Renderer、glTF／FBX、Material上書き中のModelはLitEffectと同じカメラ距離の範囲霧と指数霧を発光の後に掛け、Material上書きの無いCMO／SDKMESH／VBOはDirectXTK Effectと同じビュー深度の線形霧を掛ける。距離の違うCubeと上書き中のCMOに霧を掛けた合成画像がD3D11と一致すること、上書きを外したCMOにも霧が掛かることをWARPで検証する。
- DirectX 12の基本3D描画の直接光を、D3D11のLamaPonLit.hlslと同じCook-Torrance GGXへ置き換え。Directional Lightは太陽の見かけの大きさで鏡面の代表点を寄せ、Point／Spot Lightは同じ距離とコーンの減衰を掛ける。法線マップの強さもD3D11と同じくxyへ掛けてからzを復元し、法線は逆転置行列で変換する。金属、法線マップ、粗い材質のCubeを3種類の光源で照らした合成画像がD3D11と一致することをWARPで検証する。
- DirectX 12でForward+のクラスタライトを描画。D3D11と同じLamaPonLightCulling.hlslのCompute Shaderで視錐台16×9×24のクラスタごとにライト番号表を作り、基本3D描画はD3D11と同じクラスタ経路でPoint／Spot Light（最大256灯、影付き）を計算し、Material custom shaderは`t16`〜`t18`とクラスタの係数を受け取る。固定配列の16灯を超える24灯のPoint Lightと10灯のSpot Lightで照らした合成画像がD3D11と一致することをWARPで検証する。
- DirectX 12でベイクした間接光（照度ボリューム）を表示。D3D12 BackendでTexture3Dとその3D Shader Resource Viewを作れるようにし、Sceneに保存されたL1球面調和の係数を、基本3D描画ではD3D11と同じ`EvaluateBakedAmbient`（テクセル中心への補正、負値の切り捨て、ボリューム縁5%のフェード）で環境光へ反映し、Material custom shaderは`t23`〜`t25`と`BakedGi*`の係数を受け取る。Texture3Dの転送検証はD3D11と共通化した。2×2×2のボリュームで照らした合成画像がD3D11と一致することをWARPで検証する。
- DirectX 12でリフレクションプローブをベイクして描画に使う。Scene読み込み後のベイクはD3D11と同じく128pxのHDR描画先へ6面を描き、Compute Shaderで左右反転してcubeへ写してから、Skyと同じGGX事前畳み込みを掛ける（D3D12ではディスクキャッシュへ保存せず、読み込みごとにGPUで焼く）。基本3D描画はD3D11と同じ検査で範囲に入ったプローブへIBLを差し替え、ボックス射影と2個目のプローブとのブレンドを行い、Material custom shaderは`t3`／`t6`／`t19`／`t20`と`Reflection*`の係数を受け取る。発光壁に囲まれた2つのプローブで照らした合成画像がD3D11と一致することをWARPで検証する。
- DirectX 12のCMO／SDKMESH／VBOのModel RendererでMaterial custom shaderを描く。D3D11の`DrawCommonLit`と同じくMaterial上書き中だけ`VSMain`／`PSMain`で描き、DirectXTK `ModelMesh::PrepareForRendering`の既定（不透明はOpaque／DepthDefault、半透明passは非プレマルチプライド合成／DepthRead、DirectXTKのModelLoader既定に合わせてCMOはCullCounterClockwise、SDKMESH／VBOはCullClockwise）へShaderの宣言を重ね、宣言の加算はDirectXTKのAdditiveにする。内蔵DiffuseColorのTintとpartの半透明判定は既定Lit経路と揃える。あわせて、D3D12のCMO読み込みでDirectXTKのCMO loaderと同じくテクスチャ座標のVを反転するよう修正した（内蔵textureが黒い既存のテストでは差が見えていなかった）。描画状態を宣言しロゴを貼ったShaderと、宣言の無い表裏判定のShaderで描いたCMOの合成画像がD3D11と一致することをWARPで検証する。
- DirectX 12のMesh RendererでMaterial custom shaderのテセレーション（`HSMain`／`DSMain`）を描く。D3D11と同じPlane（1枚）／Cube（6面）の4制御点パッチを索引なしで流し、宣言の無いShaderはカリングなし（半透明は非プレマルチプライド合成／DepthRead）、深度パスはOpaque／DepthDefault／カリングなし、World Overlayは深度テストだけを外す規則もD3D11と揃える。Procedural Mesh、Sphere／CylinderとModel RendererではD3D11と同じ説明付きの代替表示にする。制御点の生成はD3D11と共通化した。`LamaPonTessellatedTerrain.hlsl`で波打たせたPlane／Cubeと代替表示のProcedural Meshの合成画像がD3D11と一致することをWARPで検証する。
- DirectX 12のModel Rendererで、Material上書き中のCMO／SDKMESH／VBOのcustom shaderの輪郭（`VSOutline`／`PSOutline`）と遮蔽表示（`PSOccluded`）を描く。D3D11の`DrawCommonLit`と同じく、輪郭は`CustomParameters[3].x > 0`のとき各partの通常描画の直前にNonPremultiplied／DepthRead／CullClockwiseで、遮蔽表示は`CustomParameters[4].w > 0`のとき連続する対象の通常描画より先にNonPremultiplied／CullCounterClockwiseと奥だけを通す深度（GREATER、書き込みなし）で重ねる。輪郭は`GSMain`を束ねず、遮蔽表示は`VSMain`（と`GSMain`）の出力を`PSOccluded`へ渡す。passのpipelineを作れないときは通常の描画を残してそのpassだけを止め、Shaderの説明に出す。手前の箱に隠れたCMOの遮蔽表示と輪郭の合成画像がD3D11と一致することをWARPで検証する。
- DirectX 12のModel Rendererでワイヤーフレーム表示を描く。D3D11のDirectXTK `CommonStates::Wireframe`と同じくカリングなし・四角形の線（`MultisampleEnable`）で、Material上書きの無いCMO／SDKMESH／VBO、上書き中のLit、glTF／GLB／FBXの既定Litとcustom shader（宣言より優先）、宣言の無いCMO／SDKMESH／VBOのcustom shaderを辺だけで描き、宣言付きのCMO／SDKMESH／VBOのcustom shaderはD3D11と同じく宣言の塗りつぶしへ戻す。影と深度のパスも同じ形で書く。6種類のModelを並べたワイヤーフレーム画像がD3D11と一致し、塗りつぶしの画像から変わることをWARPで検証する。
- DirectX 12のglTF／GLB／FBX Model RendererでMaterial custom shaderの輪郭（`VSSkinnedOutline`／`PSOutline`）と遮蔽表示（`PSSkinnedOccluded`、既存Shaderは`PSOccluded`へフォールバック）を描く。通常描画用b4だけでなくShader公開契約のb2にも同じ骨パレットを渡し、D3D11のLitEffectにも追加pass用の骨パレットを接続した。追加passを無効にした通常描画のD3D11／D3D12画素一致と、D3D12で輪郭・遮蔽が実画素を描くことをWARPで検証する。
- DirectX 12のMesh RendererでMaterial custom shaderのインスタンス描画を行う。D3D11の`RenderInstancedBatch`と同じく、`VSInstancedMain`を持つShaderで同じ形状・Material（色を含む）のRendererを1回の`DrawIndexedInstanced`へまとめ、slot 1へworld行列と色の80 bytesを並べて、`World`は単位行列、Material・光源・プローブは代表のRendererのものを使う。`VSInstancedMain`のpipelineだけを作れないときはまとめ描きを止めて個別描画へ戻し、説明を出す。テンプレート`LamaPonCustomMaterial.hlsl`へ、非一様スケールでも`VSMain`と同じ法線になる余因子行列の`VSInstancedMain`を追加した。非一様スケールのCube 2個が両APIで1 batchになり、個別描画と同じ画像でD3D11とも一致することをWARPで検証する。
- DirectX 12の組み込みLitでインスタンス描画を行う。D3D11と同じく、同じ形状とMaterial（色を除く）のMesh Rendererと、Material上書き・custom shader・skin・アニメーション・半透明の無い同じglTF／GLB／FBXのModel Rendererを（Modelは自動LODの段ごとに）1回の`DrawIndexedInstanced`へまとめ、`LamaPonLit.hlsl`の`VSInstancedMain`と同じくslot 1のworld行列で位置と（逆転置の代わりに正規化した）法線を、instanceの色でTintを決める。Model Rendererのまとめ描きはD3D11と同じく初期姿勢・primitiveの色とPBR map・SkyのIBLで描く。基本3D pipelineは1個ずつの描画でもD3D11と同じくTintを頂点シェーダーから渡す。色と非一様スケールの違うCube 2個と、テスト用の静的glTFの箱2個が両APIでそれぞれ1 batchになり、D3D11と同じ画像になることをWARPで検証する。
- DirectX 12でParticleSystemのcustom pixel shaderを描く。D3D11の`ApplyCustomPixelShader`と同じく、`PSMain`を`b0`の8本のfloat4、`b1`のLight2D、`t0`のparticle texture、`t1`の補助texture、`s0`の線形wrapで実行し、頂点出力はDirectXTK BasicEffectと同じCOLOR0／TEXCOORD0／SV_Positionの順で渡す。保存の250ミリ秒ごとの監視、compile失敗時の説明、`LamaPonSpriteError.hlsl`のマゼンタ表示もD3D11に揃え、Shaderも代替表示も使えないときは既定のpipelineで描く。既定のparticle、頂点色・UV・t0／t1・カスタム値を読むparticle-probe、壊れたShaderのparticleを並べた合成画像がD3D11と一致することをWARPで検証する。
- XAudio2 Redistが直前の`AudioEngine`破棄直後に新しいengineを一時的に拒否した場合だけ、250ミリ秒待って初期化を1回再試行する。描画APIの回帰検査や`GraphicsDevice`再生成で、音声voiceの非同期終了と競合しないようにする。
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
### パッケージのネイティブライブラリ対応

- `package.json`へ`native`（`includeDirectories` / `libraries` / `runtimeFiles` / `defines`）を書けるようにし、外部SDKを含むパッケージを配れるようにした。Game Moduleのビルド、編集中のプレイ、ゲームの書き出しへ自動で反映される。
- 指定できるのはこの4項目だけで、コンパイル／リンクオプションは渡せない。パッケージフォルダーの外を指すパス、拡張子違い、未知のキー、不正なマクロ名はインストール時に拒否する。
- エンジン自身のDLLと同じ名前、および複数パッケージで重複するDLL名は同梱できない。
- SDKの配置忘れは、リンカーのエラーではなく「どのパッケージの何が無いか」を名指しする警告で知らせる。SDK本体が未配置のパッケージはGame Moduleのビルドと書き出しを止めず、そのパッケージの`native`（`defines`を含む）を外してSDKなしでビルドする。
- 「パッケージを作成...」で`package.json`を作り直しても、手で書いた`native`は残る。

### Discord Rich Presence

- プレイ中の状況をDiscordへ表示する汎用APIを追加。`DiscordActivity`の`details`、`state`、大小の画像、開始／終了タイムスタンプをゲーム制作者が自由に設定でき、ジャンルを前提にしない。
- Rich PresenceはDiscordアカウント連携・クラウドセーブから完全に独立。ログインなしでも使え、ログインしていてもRich Presenceだけを切れる。
- Discord固有APIを`DiscordPresenceBackend`アダプターへ閉じ込め、ゲーム側コードからDiscord SDKの型を使わせない。LamaPonはSDKを同梱しないため、アダプター未登録・Discord未起動でも警告ログだけを出してゲームは通常どおり動く。
- プロジェクト設定「オンライン」にRich Presence設定（Application ID、既定の大画像キーとテキスト）とエディターからの動作確認を追加。`project.json`の`online.discordPresence`へ公開情報だけを保存し、`discordPresence`が無い古いプロジェクトは無効として読み込む。
- C++ ScriptへDiscord Rich Presence APIを追加したため、Game Module APIを23へ更新。
- 拡張機能へ公式パッケージ`discord-presence-sdk`を追加。Discord公式のSocial SDKでRich Presenceを実際に表示するアダプターで、Game Module読み込み時に自動登録するためゲーム側のコード変更は要らない。ライセンス上SDK本体は同梱できないため、利用者がDiscordから入手して`sdk/`へ置く。
- 配布パッケージのソースを`packages/src/`へ置き、`build_package.py`でZipと一覧を作れるようにした。改行をLFへ固定し、どのOSで作っても利用者が受け取る中身が変わらないようにした。

### オンラインアカウントとクラウドセーブ

- Windows x64ゲームに、自前バックエンドを経由するDiscordログイン、保護したセッション復元、ログアウトを追加。
- PlayerPrefsとJSONセーブスロットを内部プレイヤーIDごとに分離し、オフライン対応の永続ジャーナル、クラウド同期、ETag競合解決、強制終了後の復旧操作を追加。
- プロジェクトのオンライン設定、エディターのアカウント／同期／復旧UI、C++ Script API、通信契約と安全な運用ガイドを追加。Game Module APIを22へ更新。

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
