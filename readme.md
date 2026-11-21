# Keyboard Drop

Windowsで、コマンドラインから指定したファイルを、キーボードで選んだウィンドウにドラッグ＆ドロップするツールです。Migemoによるローマ字検索でウィンドウを絞り込み、部品や画面上の位置を指定して送信できます。

## ビルドと起動

Visual Studio 2022のC++ビルドツールとWindows SDKを用意し、**x64 Native Tools Command Prompt** でリポジトリのルートから実行します。

```bat
cl /nologo /std:c++17 /EHsc /utf-8 /W4 KeyboardDrop.cpp /link /SUBSYSTEM:WINDOWS
KeyboardDrop.exe "C:\work\画像 1.png" "C:\work\資料.txt"
```

ファイルは1件から指定できます。相対パスは起動時の作業ディレクトリを基準に解決します。ビルドと通常の文字列検索にはC/Migemoは不要です。

## ローマ字検索を有効にする

64ビット版では、C/Migemoの `migemo.dll` を `KeyboardDrop.exe` の隣に置き、CP932版の辞書と変換表を `dict\` に置きます。以下のPowerShellコマンドを実行ファイルのあるフォルダーで実行してください。使用するのはC/MigemoプロジェクトのWindows x64向け20110227版です。[C/Migemo公式サイト](https://www.kaoriya.net/software/cmigemo/)

```powershell
$work = New-Item -ItemType Directory -Path (Join-Path $env:TEMP ([guid]::NewGuid().ToString()))
$zip = Join-Path $work.FullName 'cmigemo.zip'
$url = 'https://storage.googleapis.com/google-code-archive-downloads/v2/code.google.com/cmigemo/cmigemo-default-win64-20110227.zip'
Invoke-WebRequest -Uri $url -OutFile $zip -ErrorAction Stop
if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne '80ADCF55848B46F8EB006FF4F73C5B840E7E322529D5D4E534BE235FF0BB4AD0') {
    throw 'C/MigemoのZIPのハッシュが一致しません。'
}
Expand-Archive -LiteralPath $zip -DestinationPath $work.FullName -ErrorAction Stop
$src = Join-Path $work.FullName 'cmigemo-default-win64'
Copy-Item -LiteralPath (Join-Path $src 'migemo.dll') -Destination . -ErrorAction Stop
New-Item -ItemType Directory -Force -Path .\dict | Out-Null
Copy-Item -Path (Join-Path $src 'dict\cp932\*') -Destination .\dict -ErrorAction Stop
```

配置後にKeyboard Dropを起動し、タイトルに「設定」を含むウィンドウが `settei` で見つかれば導入完了です。32ビットビルドにはWin32版のDLLを `migemo32.dll` という名前で配置します。

## 操作

| 場面 | キー | 動作 |
| --- | --- | --- |
| ウィンドウ選択 | 文字入力 | タイトルと実行ファイル名で絞り込みます。たとえば `settei` で「設定」を検索できます。 |
| ウィンドウ選択 | ← / → | 候補を切り替えます。Enterで選択中のウィンドウの中央に送信します。 |
| ウィンドウ選択 | Backspace / Ctrl+U | 検索文字の末尾を1文字削除 / 全消去します。 |
| ウィンドウ選択 | Space | 部品選択に入ります。候補がなければ座標指定に進みます。 |
| 部品選択 | 方向キー / Tab | 指定方向の候補へ移動 / 選択中の部品から座標指定に切り替えます。 |
| 座標指定 | 方向キー | 表示倍率100%で16px移動します。Ctrl併用で64px、Shift併用で倍率にかかわらず1px移動します。 |
| 部品選択・座標指定 | Enter | 十字で示した位置に送信します。 |
| 各モード | Esc | 一段戻ります。ウィンドウ選択中は検索文字を消去し、空の状態で押すと終了します。 |

独自描画のアプリでは、ドロップ可能な領域が部品候補に現れない場合があります。その場合は座標指定で十字を受付領域に合わせてください。送信には `WM_DROPFILES` またはOLEのコピー操作を使います。OLEで拒否された場合は、オーバーレイで位置を選び直せます。

## 改版履歴

| バージョン | 主な変更 |
| --- | --- |
| 0.3.0 | ウィンドウタイトル・実行ファイル名による絞り込みと、任意で導入できるC/Migemoによるローマ字検索を追加。Backspaceで1文字削除、Ctrl+Uで検索を全消去。 |
| 0.2.0 | Spaceでの部品選択と、十字による座標指定を追加。OLEの受付待ちと、タスクバーなどで拒否された後のキーボードフォーカス復帰を改善。 |
| 0.1.0 | 初版。コマンドラインで指定した複数ファイルを、オーバーレイで選んだウィンドウの中央にドロップ。 |
