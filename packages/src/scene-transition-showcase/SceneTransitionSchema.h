#pragma once

namespace LamaPonSceneShowcase
{
    // SceneTransition.Presetの編集欄と既定値
    inline constexpr char PresetSchema[] = R"schema({
    "fields": [
        {
            "name": "effect",
            "displayName": "演出",
            "type": "string",
            "default": "fade",
            "options": [
                {
                    "value": "none",
                    "displayName": "なし（すぐ切り替え）"
                },
                {
                    "value": "fade",
                    "displayName": "フェード"
                },
                {
                    "value": "wipe",
                    "displayName": "ワイプ"
                },
                {
                    "value": "iris",
                    "displayName": "アイリス（円）"
                },
                {
                    "value": "diamond",
                    "displayName": "ひし形"
                },
                {
                    "value": "blinds",
                    "displayName": "ブラインド"
                },
                {
                    "value": "tiles",
                    "displayName": "タイル"
                },
                {
                    "value": "diamondTiles",
                    "displayName": "ひし形タイル"
                },
                {
                    "value": "dots",
                    "displayName": "ドット"
                },
                {
                    "value": "shutter",
                    "displayName": "シャッター"
                },
                {
                    "value": "shader",
                    "displayName": "シェーダー"
                }
            ]
        },
        {
            "name": "direction",
            "displayName": "向き",
            "type": "string",
            "default": "leftToRight",
            "options": [
                {
                    "value": "leftToRight",
                    "displayName": "左 → 右"
                },
                {
                    "value": "rightToLeft",
                    "displayName": "右 → 左"
                },
                {
                    "value": "topToBottom",
                    "displayName": "上 → 下"
                },
                {
                    "value": "bottomToTop",
                    "displayName": "下 → 上"
                },
                {
                    "value": "topLeftToBottomRight",
                    "displayName": "左上 → 右下"
                },
                {
                    "value": "topRightToBottomLeft",
                    "displayName": "右上 → 左下"
                },
                {
                    "value": "bottomLeftToTopRight",
                    "displayName": "左下 → 右上"
                },
                {
                    "value": "bottomRightToTopLeft",
                    "displayName": "右下 → 左上"
                }
            ]
        },
        {
            "name": "easing",
            "displayName": "動き方",
            "type": "string",
            "default": "easeInOutCubic",
            "options": [
                {
                    "value": "linear",
                    "displayName": "一定"
                },
                {
                    "value": "easeInQuad",
                    "displayName": "ゆっくり始まる（弱）"
                },
                {
                    "value": "easeOutQuad",
                    "displayName": "ゆっくり終わる（弱）"
                },
                {
                    "value": "easeInOutQuad",
                    "displayName": "ゆっくり始まって終わる（弱）"
                },
                {
                    "value": "easeInCubic",
                    "displayName": "ゆっくり始まる"
                },
                {
                    "value": "easeOutCubic",
                    "displayName": "ゆっくり終わる"
                },
                {
                    "value": "easeInOutCubic",
                    "displayName": "ゆっくり始まって終わる"
                },
                {
                    "value": "easeInOutSine",
                    "displayName": "なめらか"
                }
            ]
        },
        {
            "name": "coverDuration",
            "displayName": "覆う時間（秒）",
            "type": "float",
            "default": 0.4,
            "min": 0,
            "max": 10,
            "step": 0.05
        },
        {
            "name": "holdDuration",
            "displayName": "覆ったまま待つ時間（秒）",
            "type": "float",
            "default": 0.1,
            "min": 0,
            "max": 10,
            "step": 0.05
        },
        {
            "name": "revealDuration",
            "displayName": "開く時間（秒）",
            "type": "float",
            "default": 0.4,
            "min": 0,
            "max": 10,
            "step": 0.05
        },
        {
            "name": "color",
            "displayName": "覆いの色",
            "type": "color4",
            "default": [
                0,
                0,
                0,
                1
            ]
        },
        {
            "name": "accentColor",
            "displayName": "差し色（透明なら使わない）",
            "type": "color4",
            "default": [
                1,
                0.8,
                0.2,
                0
            ]
        },
        {
            "name": "accentWidth",
            "displayName": "差し色の太さ",
            "type": "float",
            "default": 0.03,
            "min": 0,
            "max": 0.5,
            "step": 0.005
        },
        {
            "name": "softness",
            "displayName": "境界のぼかし",
            "type": "float",
            "default": 0,
            "min": 0,
            "max": 0.5,
            "step": 0.005
        },
        {
            "name": "divisions",
            "displayName": "分割数",
            "type": "int",
            "default": 10,
            "min": 1,
            "max": 64
        },
        {
            "name": "stagger",
            "displayName": "時間差",
            "type": "float",
            "default": 0.5,
            "min": 0,
            "max": 1,
            "step": 0.05
        },
        {
            "name": "focus",
            "displayName": "中心（左上0,0／右下1,1）",
            "type": "vec2",
            "default": [
                0.5,
                0.5
            ]
        },
        {
            "name": "passThrough",
            "displayName": "開くときは通り抜ける",
            "type": "bool",
            "default": true
        },
        {
            "name": "showLoadingScreen",
            "displayName": "覆っている間は読み込み画面を重ねる",
            "type": "bool",
            "default": true
        },
        {
            "name": "blockInput",
            "displayName": "遷移中はUI Buttonを押せなくする",
            "type": "bool",
            "default": true
        },
        {
            "name": "fadeMusic",
            "displayName": "BGMも一緒にフェードする",
            "type": "bool",
            "default": true
        },
        {
            "name": "shaderPattern",
            "displayName": "シェーダーの模様",
            "type": "string",
            "default": "dissolve",
            "options": [
                {
                    "value": "ruleImage",
                    "displayName": "ルール画像"
                },
                {
                    "value": "dissolve",
                    "displayName": "ディゾルブ"
                },
                {
                    "value": "clock",
                    "displayName": "時計"
                },
                {
                    "value": "spiral",
                    "displayName": "渦巻き"
                },
                {
                    "value": "ripple",
                    "displayName": "波紋"
                },
                {
                    "value": "hexagons",
                    "displayName": "六角形"
                },
                {
                    "value": "heart",
                    "displayName": "ハート"
                }
            ]
        },
        {
            "name": "ruleTexture",
            "displayName": "ルール画像",
            "type": "asset",
            "default": "",
            "assetType": "texture"
        },
        {
            "name": "shader",
            "displayName": "独自シェーダー（空ならパッケージのシェーダー）",
            "type": "asset",
            "default": "",
            "assetType": "shader"
        }
    ]
})schema";
}
