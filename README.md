# LoL Death MIA Pin — OBS Plugin

League of Legendsで死んだらMIAピンたかれるプラグイン

## 動作環境

- Windows 11
- OBS Studio 32.1.0

## インストール

解凍したファイルの `obs-plugins` と `data` を、OBS Studioのインストールフォルダにコピーしてください。

```
obs-studio/
├── obs-plugins/
│   └── 64bit/
│       └── lol-death-mia-pin.dll    ← コピー
└── data/
    └── obs-plugins/
        └── lol-death-mia-pin/       ← コピー
            └── locale/
                ├── en-US.ini
                └── ja-JP.ini
```

同封してあるサンプル動画（`mia.webm`）は任意の場所に移動してください。（必要なければしなくても大丈夫です）

## アンインストール

以下の2つを削除してください：

- `obs-studio/obs-plugins/64bit/lol-death-mia-pin.dll`
- `obs-studio/data/obs-plugins/lol-death-mia-pin/` フォルダ

## 使い方

OBSの「ソース」として追加して使用します。

### 設定項目

| 設定 | 説明 | 初期値 |
|------|------|--------|
| MIAピン画像/動画 | 表示するエフェクト（webmまたは画像） | なし |
| 効果音 | SE（画像エフェクトの場合に有効） | なし |
| 音量 | 効果音の音量 | 100% |
| オーバーレイ幅 | エフェクトの出現範囲（幅） | 400 |
| オーバーレイ高さ | エフェクトの出現範囲（高さ） | 400 |
| エスカレート | デスごとにピン数・速度が増加（最大9回） | OFF |
| ピン回数 | エフェクトの再生回数（非エスカレート時） | 3 |
| ピン間隔 | エフェクト間の時間差（非エスカレート時） | 0.5秒 |
| ピンサイズ | エフェクトのサイズ | 35% |

### 仕組み

[LoL Live Client Data API](https://developer.riotgames.com/docs/lol#game-client-api_live-client-data-api) を使用しています。

試合中、ローカルでAPIを取得しつづけ、デスを検知した場合にトリガーします。
トリガーされたらプロパティで設定された値に応じてエフェクト（動画）が再生されます。

### エフェクトについて

サンプルとしてMIAピンっぽい動画をつけてあります。（音無し）
ご自身で効果音は用意してください。（動画+効果音でも再生されることは確認しています）

エフェクトは指定する形なので、作成した動画を利用していただくことも可能です。（webm形式）
例えば：
- 誰かにピコハンで叩かれる
- コーチに怒られる

など用意していただければユニークなエフェクトになると思います。

## ビルド（開発者向け）

### 必要なもの

- Visual Studio 2022以降
- CMake 3.20以降
- OBS Studio ソースコード（ヘッダー用）
- obs-deps（FFmpeg・libcurl・jansson）

### 手順

```bash
# 依存ファイルを配置
# deps/obs-studio/  — OBSソースコード
# deps/obs-lib/     — obs.lib
# deps/obs-deps/    — obs-deps (FFmpeg, libcurl, jansson含む)
# deps/obs-config/  — config.h

cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## 免責事項

This product is not endorsed by Riot Games and does not reflect the views or opinions of Riot Games.

本プラグインは [LoL Live Client Data API](https://developer.riotgames.com/docs/lol#game-client-api_live-client-data-api) を使用しています。Riot Games による公式サポート対象外のAPIです。

## ライセンス

GPL-2.0 — 詳細は [LICENSE](LICENSE) を参照してください。

## 連絡先

制作者: Xil — [@Xil_7dx](https://x.com/Xil_7dx)
