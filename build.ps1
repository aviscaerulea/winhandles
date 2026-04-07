# vim: set ft=ps1 fenc=utf-8 ff=unix ts=4 sw=4 et :
# ==================================================
# winhandles ビルドスクリプト
# VS 開発環境をロードして cl.exe でビルドする
#
# 使用方法：
#   pwsh -File build.ps1 [-OutDir <出力ディレクトリ>] [-Version <バージョン>]
# ==================================================
param(
    [string]$OutDir  = "out",
    [string]$Version = "dev"
)

# VS 開発環境をロード（公式 DLL モジュール方式、Build Tools 対応）
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -products '*' -latest -property installationPath  # Build Tools 単体環境にも対応
if (-not $vsPath) { Write-Error "Visual Studio / Build Tools が見つからない"; exit 1 }

$devShellDll = Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
if (-not (Test-Path $devShellDll)) { Write-Error "DevShell.dll が見つからない: $devShellDll"; exit 1 }
Import-Module $devShellDll
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64"

# 出力ディレクトリを作成
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$outExe = Join-Path $OutDir "winhandles.exe"

$clArgs = @(
    "/utf-8", "/EHsc", "/O2", "/W4", "/WX", "/nologo", "/std:c++17",
    "winhandles.cpp",
    "/Fe:$outExe",
    "/link",
    "/SUBSYSTEM:WINDOWS", "/ENTRY:mainCRTStartup",
    "psapi.lib", "advapi32.lib"
)

Write-Host "[ビルド] cl.exe $($clArgs -join ' ')"
& cl.exe @clArgs

if ($LASTEXITCODE -ne 0) {
    Write-Error "ビルド失敗（終了コード：$LASTEXITCODE）"
    exit $LASTEXITCODE
}

Write-Host "[完了] $outExe"
