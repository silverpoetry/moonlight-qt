$ErrorActionPreference = 'Stop'

$Organization = "moonlight-stream"
$PrebuiltRepo = "moonlight-qt-deps"
$TargetDir = Join-Path $PSScriptRoot "libs\windows"
$Assets = @("windows-x64.zip", "windows-ARM64.zip")
$Tag = "v6"
$WindowsSdkCppPackage = @{
    Id = "Microsoft.Windows.SDK.CPP"
    Version = "10.0.28000.1839"
    Sha256 = "6428C917AE1B888F0DE4C91EF1A78B252F4358B6B2C078F947B8D3B033B16BA8"
    CppWinRtPath = "c\Include\10.0.28000.0\cppwinrt"
    TargetPath = "include\cppwinrt-10.0.28000.0"
}

function Download-FileChecked {
    param(
        [Parameter(Mandatory=$true)][string]$Url,
        [Parameter(Mandatory=$true)][string]$Destination,
        [string]$Sha256
    )

    curl.exe -s -L -f -o "$Destination" "$Url"
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    if (![string]::IsNullOrEmpty($Sha256)) {
        $ActualHash = (Get-FileHash -Algorithm SHA256 -Path "$Destination").Hash
        if ($ActualHash -ne $Sha256) {
            throw "SHA256 mismatch for $Destination. Expected $Sha256 but got $ActualHash."
        }
    }
}

if (Test-Path $TargetDir) {
    Write-Host "Cleaning target directory..." -ForegroundColor Cyan
    Remove-Item -Path "$TargetDir\*" -Recurse -Force
} else {
    New-Item -ItemType Directory -Path $TargetDir | Out-Null
}

foreach ($AssetName in $Assets) {
    $Url = "https://github.com/$Organization/$PrebuiltRepo/releases/download/$Tag/$AssetName"
    $ArchivePath = Join-Path $env:TEMP $AssetName

    Write-Host "Downloading $AssetName..." -ForegroundColor Cyan
    Download-FileChecked -Url $Url -Destination $ArchivePath

    Write-Host "Extracting $AssetName..." -ForegroundColor Cyan
    Expand-Archive -Path $ArchivePath -DestinationPath $TargetDir -Force
    Remove-Item $ArchivePath
}

$WindowsSdkPackageName = "$($WindowsSdkCppPackage.Id).$($WindowsSdkCppPackage.Version).zip"
$WindowsSdkUrl = "https://www.nuget.org/api/v2/package/$($WindowsSdkCppPackage.Id)/$($WindowsSdkCppPackage.Version)"
$WindowsSdkArchivePath = Join-Path $env:TEMP $WindowsSdkPackageName
$WindowsSdkExtractPath = Join-Path $env:TEMP "$($WindowsSdkCppPackage.Id).$($WindowsSdkCppPackage.Version)"
$WindowsSdkSourcePath = Join-Path $WindowsSdkExtractPath $WindowsSdkCppPackage.CppWinRtPath
$WindowsSdkTargetPath = Join-Path $TargetDir $WindowsSdkCppPackage.TargetPath

Write-Host "Downloading $($WindowsSdkCppPackage.Id) $($WindowsSdkCppPackage.Version)..." -ForegroundColor Cyan
Download-FileChecked -Url $WindowsSdkUrl -Destination $WindowsSdkArchivePath -Sha256 $WindowsSdkCppPackage.Sha256

if (Test-Path $WindowsSdkExtractPath) {
    Remove-Item -Path $WindowsSdkExtractPath -Recurse -Force
}

Write-Host "Extracting C++/WinRT projection headers..." -ForegroundColor Cyan
Expand-Archive -Path $WindowsSdkArchivePath -DestinationPath $WindowsSdkExtractPath -Force

if (!(Test-Path (Join-Path $WindowsSdkSourcePath "winrt\Windows.UI.Input.h"))) {
    throw "C++/WinRT projection headers were not found in $WindowsSdkArchivePath."
}

if (Test-Path $WindowsSdkTargetPath) {
    Remove-Item -Path $WindowsSdkTargetPath -Recurse -Force
}
New-Item -ItemType Directory -Path (Split-Path $WindowsSdkTargetPath) -Force | Out-Null
Copy-Item -Path $WindowsSdkSourcePath -Destination $WindowsSdkTargetPath -Recurse -Force

Remove-Item $WindowsSdkArchivePath
Remove-Item -Path $WindowsSdkExtractPath -Recurse -Force

Write-Host "Dependencies successfully deployed" -ForegroundColor Green
