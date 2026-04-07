# winhandles

Windows のシステム全体のハンドル消費量をプロセス別に調査・レポートする CLI ツール。
タスクマネージャの「ハンドル数」が異常に増加している場合に、原因プロセスを特定する目的で使用する。

## 機能

- システム全体のハンドルをプロセス別に集計し、消費量の多い順に表示
- ハンドルの型別内訳（File、Key、Event、Thread 等）を表示
- 指定 PID のハンドルをオブジェクト名つきで詳細表示
- System プロセス（PID 4）のハンドルをパス別に分析してカーネルの起因を調査

## 動作要件

- Windows 10/11（x64）
- 管理者権限推奨（SeDebugPrivilege が必要）

## インストール方法

ビルド済みの `winhandles.exe` を任意のディレクトリに配置し、PATH に追加する。

## 使用方法

```
winhandles [オプション]

オプション：
  --top N    上位 N プロセスを表示（デフォルト：20）
  --all      全プロセスを表示
  --pid PID  指定 PID のハンドル詳細を表示
             （PID 4 指定時はドライバ別内訳も表示）
  --help     ヘルプを表示
```

### 基本的な使用例

```
# 上位 20 プロセスを表示
winhandles

# 上位 50 プロセスを表示
winhandles --top 50

# 全プロセスを表示
winhandles --all

# PID 1234 の詳細を表示
winhandles --pid 1234

# System プロセスのドライバ別内訳を表示
winhandles --pid 4
```

## ビルド方法

Visual Studio または Build Tools（x64）が必要。

```
task build
```

ビルド成果物は `out/winhandles.exe` に出力される。

## 技術仕様

- C++17（MSVC）
- `NtQuerySystemInformation(SystemExtendedHandleInformation)` でシステム全ハンドルを列挙
- `NtQueryObject` でハンドルの型名・オブジェクト名を解決
- 名前付きパイプ等でのデッドロック対策としてワーカースレッド + タイムアウト方式を採用
- psapi.lib（EnumDeviceDrivers）でドライバ情報を取得
