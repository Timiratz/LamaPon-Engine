# TARGET RANGE

マウス照準・Tag検索・`Time::SetTimeScale`ポーズ・UIウィジェット・
metallicマテリアル（PBR）・スポットライト影・GPUインスタンシング・
手続き生成Mesh Collider・スイープCCDを1本で使う射撃レンジです。

45秒以内に金色の的をできるだけ多くクリックで撃ち抜いてください。
命中した的は別の場所に湧き直します。右側では金属ボールが
Mesh Colliderのスロープを転がり続けます。

## 操作

- マウス移動: 照準
- 左クリック: 射撃（終了後は再挑戦）
- `P`: 一時停止（SE音量スライダー、スポット影のON/OFF、再開ボタン）

## 起動

エディターで`assets/scenes/target-range.scene.json`を開いて再生するか、
`.lamapon/project.json`の起動シーンを`scenes/target-range.scene.json`へ
変更して`LamaPonGame.exe`を実行してください。
