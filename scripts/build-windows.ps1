[CmdletBinding()]
param(
    [ValidateSet('debug', 'release', 'signed-release')]
    [string] $Configuration = 'release',

    [ValidateSet('x64', 'arm64')]
    [string] $Architecture = 'x64',

    [string] $QtRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$sourceRoot = Split-Path $PSScriptRoot -Parent
$toolchainPath = Join-Path $PSScriptRoot 'windows-toolchain.json'
$toolchain = Get-Content -LiteralPath $toolchainPath -Raw |
        ConvertFrom-Json

$developRoot = $env:MOONLIGHT_DEVELOP_ROOT
if ([string]::IsNullOrWhiteSpace($developRoot)) {
    $developRoot = 'E:\Develop'
}

if ([string]::IsNullOrWhiteSpace($QtRoot)) {
    if (-not [string]::IsNullOrWhiteSpace($env:MOONLIGHT_QT_ROOT)) {
        $QtRoot = $env:MOONLIGHT_QT_ROOT
    }
    else {
        $QtRoot = Join-Path $developRoot 'Qt'
    }
}

$buildEnvironmentRoot = $env:MOONLIGHT_BUILD_ENV_ROOT
if ([string]::IsNullOrWhiteSpace($buildEnvironmentRoot)) {
    if (-not [string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
        $buildEnvironmentRoot = Join-Path $env:RUNNER_TEMP 'MoonlightQtBuild'
    }
    else {
        $buildEnvironmentRoot = Join-Path $developRoot 'MoonlightQtBuild'
    }
}
$buildEnvironmentRoot = [System.IO.Path]::GetFullPath(
        $buildEnvironmentRoot)

$environmentDirectories = @{
    Temp = Join-Path $buildEnvironmentRoot 'Temp'
    NuGetPackages = Join-Path $buildEnvironmentRoot 'NuGet\packages'
    NuGetHttpCache = Join-Path $buildEnvironmentRoot 'NuGet\http-cache'
    DotNetHome = Join-Path $buildEnvironmentRoot 'DotNetHome'
}
foreach ($directory in $environmentDirectories.Values) {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
}

$env:TEMP = $environmentDirectories.Temp
$env:TMP = $environmentDirectories.Temp
$env:NUGET_PACKAGES = $environmentDirectories.NuGetPackages
$env:NUGET_HTTP_CACHE_PATH = $environmentDirectories.NuGetHttpCache
$env:DOTNET_CLI_HOME = $environmentDirectories.DotNetHome

if (-not [string]::IsNullOrWhiteSpace($env:MOONLIGHT_HTTP_PROXY)) {
    $env:HTTP_PROXY = $env:MOONLIGHT_HTTP_PROXY
    $env:HTTPS_PROXY = $env:MOONLIGHT_HTTP_PROXY
}

$kitName = $toolchain.qtKits.$Architecture
if ([string]::IsNullOrWhiteSpace($kitName)) {
    throw "No Qt kit is configured for architecture '$Architecture'."
}

$qtBin = Join-Path $QtRoot (
        Join-Path $toolchain.qtVersion (
        Join-Path $kitName 'bin'))
$qtBin = [System.IO.Path]::GetFullPath($qtBin)

$qmakeCandidates = @(
    (Join-Path $qtBin 'qmake.bat'),
    (Join-Path $qtBin 'host-qmake.bat'),
    (Join-Path $qtBin 'qmake.exe')
)
if (-not ($qmakeCandidates | Where-Object {
            Test-Path -LiteralPath $_ -PathType Leaf
        } | Select-Object -First 1)) {
    throw "The configured official Qt kit is incomplete: $qtBin"
}

$env:MOONLIGHT_QT_BIN = $qtBin
$env:MOONLIGHT_QT_VERSION = $toolchain.qtVersion

Write-Host "Qt kit: $qtBin"
Write-Host "Build environment: $buildEnvironmentRoot"
Write-Host "Configuration: $Configuration"
Write-Host "Architecture: $Architecture"

$buildScript = Join-Path $PSScriptRoot 'build-arch.bat'
Push-Location $sourceRoot
try {
    & $env:ComSpec /d /c "`"$buildScript`" $Configuration $Architecture"
    if ($LASTEXITCODE -ne 0) {
        throw "Windows build failed with exit code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}
