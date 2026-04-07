# CLAUDE.md

## 開発環境

- Visual Studio 2022 または Build Tools for Visual Studio 2022（x64）
- Task（タスクランナー）
- PowerShell 7（pwsh.exe）

## ビルド方法

```
task build
```

ビルド成果物：`out/winhandles.exe`

クリーンビルド：

```
task
```

## テスト方法

管理者権限のコマンドプロンプトまたは PowerShell で実行する。

```
# 基本動作確認
out/winhandles.exe

# System プロセスのドライバ別内訳確認
out/winhandles.exe --pid 4
```

## 実装上の注意点

### NtQueryObject のデッドロック
`NtQueryObject(ObjectNameInformation)` は名前付きパイプや ALPC ポートに対して呼び出すとハングする。
`SafeQueryObjectName()` 関数でワーカースレッド + 150ms タイムアウトで保護している。
タイムアウト時は `TerminateThread` で回収し、`QueryNameCtx` はリーク（解放するとクラッシュする可能性があるため）。

### ObjectTypeIndex のキャッシュ
Windows バージョンによって型インデックスが変動するため、動的解決のみ。ハードコードは禁止。

### バッファサイズ
`EnumerateHandles()` は 4MB から開始し、`STATUS_INFO_LENGTH_MISMATCH` が返るたびに 2 倍に拡張する。
ハンドル数が 50 万を超える環境では 64MB 程度必要になる場合がある。

### ウィンドウレス CLI
`/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup` でコンソールウィンドウなしのサブシステムとして
ビルドしているが、コンソールから実行した場合は通常通り stdout/stderr に出力される。

## 参考

@README.md
