[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $DeployDirectory,

    [string[]] $RuntimeDirectory = @(),

    [string[]] $DeferredRuntimeDirectory = @(),

    [Parameter(Mandatory = $true)]
    [string] $DumpBinPath,

    [Parameter(Mandatory = $true)]
    [ValidateSet('x86', 'x64', 'arm64')]
    [string] $TargetArchitecture,

    [switch] $Resolve
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ExpectedMachines = @{
    x86 = '14C'
    x64 = '8664'
    arm64 = 'AA64'
}

function Resolve-ExistingDirectory {
    param([Parameter(Mandatory = $true)][string] $Path)

    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    if (-not (Test-Path -LiteralPath $resolved.Path -PathType Container)) {
        throw "Runtime dependency path is not a directory: $Path"
    }
    return $resolved.Path
}

function Get-PeMachine {
    param([Parameter(Mandatory = $true)][string] $Path)

    $output = & $DumpBinPath /nologo /headers $Path 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin failed while reading PE headers: $Path`n$output"
    }
    foreach ($line in $output) {
        if ($line -match '^\s*([0-9A-Fa-f]+) machine \(') {
            return $Matches[1].ToUpperInvariant()
        }
    }
    throw "Unable to determine PE machine type: $Path"
}

function Assert-TargetMachine {
    param([Parameter(Mandatory = $true)][string] $Path)

    $actual = Get-PeMachine -Path $Path
    $expected = $ExpectedMachines[$TargetArchitecture]
    if ($actual -ne $expected) {
        throw "Wrong-architecture runtime dependency: $Path " +
                "(expected $TargetArchitecture/$expected, found $actual)"
    }
}

function Get-PeDependencies {
    param([Parameter(Mandatory = $true)][string] $Path)

    $output = & $DumpBinPath /nologo /dependents $Path 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin failed while reading dependencies: $Path`n$output"
    }
    foreach ($line in $output) {
        if ($line -match '^\s+([^\s]+\.dll)\s*$') {
            $Matches[1]
        }
    }
}

function Test-SystemDependency {
    param([Parameter(Mandatory = $true)][string] $Name)

    if ($Name -match '^(?i:api-ms-|ext-ms-)') {
        return $true
    }
    return Test-Path -LiteralPath (
            Join-Path ([Environment]::SystemDirectory) $Name) -PathType Leaf
}

function Find-RuntimeDependency {
    param([Parameter(Mandatory = $true)][string] $Name)

    foreach ($directory in $RuntimeDirectories) {
        $candidate = Join-Path $directory $Name
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return $null
}

function Test-DeferredRuntimeDependency {
    param([Parameter(Mandatory = $true)][string] $Name)

    foreach ($directory in $DeferredRuntimeDirectories) {
        $candidate = Join-Path $directory $Name
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            Assert-TargetMachine -Path $candidate
            return $true
        }
    }
    return $false
}

function Test-DeployedDependency {
    param(
        [Parameter(Mandatory = $true)][System.IO.FileInfo] $Importer,
        [Parameter(Mandatory = $true)][string] $Name
    )

    return (Test-Path -LiteralPath (
                    Join-Path $Importer.DirectoryName $Name) -PathType Leaf) -or
            (Test-Path -LiteralPath (
                    Join-Path $DeployRoot $Name) -PathType Leaf)
}

if (-not (Test-Path -LiteralPath $DumpBinPath -PathType Leaf)) {
    throw "dumpbin.exe was not found: $DumpBinPath"
}

$DeployRoot = Resolve-ExistingDirectory -Path $DeployDirectory
$RuntimeDirectories = @(
    foreach ($directory in $RuntimeDirectory) {
        Resolve-ExistingDirectory -Path $directory
    }
)
$DeferredRuntimeDirectories = @(
    foreach ($directory in $DeferredRuntimeDirectory) {
        Resolve-ExistingDirectory -Path $directory
    }
)

$queue = [System.Collections.Generic.Queue[System.IO.FileInfo]]::new()
$processed = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
$queued = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
$unresolved = @{}
$copied = [System.Collections.Generic.List[string]]::new()

Get-ChildItem -LiteralPath $DeployRoot -Recurse -File |
        Where-Object { $_.Extension -in '.exe', '.dll' } |
        ForEach-Object {
            if ($queued.Add($_.FullName)) {
                $queue.Enqueue($_)
            }
        }

while ($queue.Count -gt 0) {
    $binary = $queue.Dequeue()
    if (-not $processed.Add($binary.FullName)) {
        continue
    }

    Assert-TargetMachine -Path $binary.FullName
    foreach ($dependency in Get-PeDependencies -Path $binary.FullName) {
        if ((Test-DeployedDependency -Importer $binary -Name $dependency) -or
                (Test-SystemDependency -Name $dependency)) {
            continue
        }

        $source = $null
        if ($Resolve) {
            $source = Find-RuntimeDependency -Name $dependency
        }
        if ($null -eq $source) {
            if ($Resolve -and
                    (Test-DeferredRuntimeDependency -Name $dependency)) {
                continue
            }
            $key = $dependency.ToLowerInvariant()
            if (-not $unresolved.ContainsKey($key)) {
                $unresolved[$key] = [System.Collections.Generic.List[string]]::new()
            }
            $relativeImporter = $binary.FullName
            if ($relativeImporter.StartsWith(
                    $DeployRoot,
                    [System.StringComparison]::OrdinalIgnoreCase)) {
                $relativeImporter = $relativeImporter.Substring(
                        $DeployRoot.Length)
                $relativeImporter = $relativeImporter.TrimStart(@(
                    [System.IO.Path]::DirectorySeparatorChar,
                    [System.IO.Path]::AltDirectorySeparatorChar))
            }
            $unresolved[$key].Add($relativeImporter)
            continue
        }

        Assert-TargetMachine -Path $source
        $destination = Join-Path $DeployRoot $dependency
        Copy-Item -LiteralPath $source -Destination $destination -Force
        $copied.Add($dependency)
        $deployedFile = Get-Item -LiteralPath $destination
        if ($queued.Add($deployedFile.FullName)) {
            $queue.Enqueue($deployedFile)
        }
    }
}

if ($copied.Count -gt 0) {
    Write-Host ('Deployed transitive runtime dependencies: ' +
            (($copied | Sort-Object -Unique) -join ', '))
}

if ($unresolved.Count -gt 0) {
    $details = @(
        foreach ($entry in $unresolved.GetEnumerator() | Sort-Object Key) {
            "  $($entry.Key) <- " +
                    (($entry.Value | Sort-Object -Unique) -join ', ')
        }
    ) -join [Environment]::NewLine
    $message = "Unresolved Windows runtime dependencies:`n$details"
    throw $message
}
else {
    Write-Host "Windows runtime dependency closure is complete."
}
