# SPDX-FileCopyrightText: 2026 VibeCut contributors
# SPDX-License-Identifier: GPL-3.0-or-later

#Requires -Version 5.1

[CmdletBinding()]
param(
    [string]$CraftRoot = 'C:\CraftVibeCut',
    [string]$DownloadDirectory = '',
    [string]$PythonPath = '',
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\..\artifacts\windows'),
    [switch]$BuildOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$CraftCommit = 'b80e8f8c6c4f9fc8c09464dc8b696bfa625fa12c'
$KdeBlueprintCommit = '96127a2b665ecd645058e5157b468bca721cbc65'
$PackageBaseName = 'kdenlive-vibecut-windows-cl-msvc2022-x86_64'
$LibPngArchiveUrl = 'https://codeload.github.com/pnggroup/libpng/tar.gz/refs/tags/v1.6.45'
$LibPngArchiveSha256 = '7ff6898520645716ddc3d8381d97b6e02937b03da92e6fd0d7cf9d7d2b0da780'

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(ValueFromRemainingArguments = $true)][string[]]$ArgumentList
    )

    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code $LASTEXITCODE`: $FilePath $($ArgumentList -join ' ')"
    }
}

function Resolve-Python {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return (Resolve-Path -LiteralPath $RequestedPath).Path
    }

    $python = Get-Command python.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($python) {
        return $python.Source
    }

    $launcher = Get-Command py.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($launcher) {
        $resolved = & $launcher.Source -3.11 -c 'import sys; print(sys.executable)'
        if ($LASTEXITCODE -eq 0 -and $resolved) {
            return (Resolve-Path -LiteralPath $resolved.Trim()).Path
        }
    }

    throw 'Python 3.11 was not found. Install it or pass -PythonPath.'
}

function Set-IniValue {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Section,
        [Parameter(Mandatory = $true)][string]$Key,
        [Parameter(Mandatory = $true)][string]$Value
    )

    $lines = [System.Collections.Generic.List[string]]::new()
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        $lines.AddRange([string[]][System.IO.File]::ReadAllLines($Path))
    }
    $sectionLine = -1
    $nextSectionLine = $lines.Count

    for ($index = 0; $index -lt $lines.Count; $index++) {
        if ($lines[$index] -match '^\s*\[(.+)\]\s*$') {
            if ($sectionLine -ge 0) {
                $nextSectionLine = $index
                break
            }
            if ($Matches[1] -eq $Section) {
                $sectionLine = $index
            }
        }
    }

    if ($sectionLine -lt 0) {
        $lines.Add('')
        $lines.Add("[$Section]")
        $lines.Add("$Key = $Value")
    }
    else {
        $keyPattern = '^\s*' + [regex]::Escape($Key) + '\s*='
        $replaced = $false
        for ($index = $sectionLine + 1; $index -lt $nextSectionLine; $index++) {
            if ($lines[$index] -match $keyPattern) {
                $lines[$index] = "$Key = $Value"
                $replaced = $true
                break
            }
        }
        if (-not $replaced) {
            $lines.Insert($nextSectionLine, "$Key = $Value")
        }
    }

    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllLines($Path, $lines, $utf8NoBom)
}

function Test-Checksum {
    param(
        [Parameter(Mandatory = $true)][string]$ArtifactPath,
        [Parameter(Mandatory = $true)][string]$ChecksumPath
    )

    $expected = ([System.IO.File]::ReadAllText($ChecksumPath).Trim() -split '\s+')[0].ToLowerInvariant()
    $actual = (Get-FileHash -LiteralPath $ArtifactPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $expected) {
        throw "SHA-256 mismatch for $ArtifactPath. Expected $expected, got $actual."
    }
}

function Save-VerifiedDownload {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$Sha256
    )

    $expectedHash = $Sha256.ToLowerInvariant()
    if (Test-Path -LiteralPath $Destination -PathType Leaf) {
        $existingHash = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($existingHash -eq $expectedHash) {
            Write-Host "Using verified cached download: $Destination"
            return
        }
        Write-Warning "Replacing an invalid cached download: $Destination"
    }

    $destinationDirectory = Split-Path -Parent $Destination
    New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null

    for ($attempt = 1; $attempt -le 5; $attempt++) {
        $temporaryDownload = "$Destination.partial-$([guid]::NewGuid().ToString('N'))"
        try {
            Write-Host "Downloading $Url (attempt $attempt of 5)"
            $savedProgressPreference = $ProgressPreference
            try {
                $ProgressPreference = 'SilentlyContinue'
                Invoke-WebRequest -Uri $Url -OutFile $temporaryDownload -UseBasicParsing
            }
            finally {
                $ProgressPreference = $savedProgressPreference
            }
            $actualHash = (Get-FileHash -LiteralPath $temporaryDownload -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($actualHash -ne $expectedHash) {
                throw "SHA-256 mismatch. Expected $expectedHash, got $actualHash."
            }
            Move-Item -LiteralPath $temporaryDownload -Destination $Destination -Force
            return
        }
        catch {
            if ($attempt -eq 5) {
                throw "Could not download and verify $Url after 5 attempts: $($_.Exception.Message)"
            }
            Write-Warning "Download attempt $attempt failed: $($_.Exception.Message)"
            Start-Sleep -Seconds ([math]::Pow(2, $attempt))
        }
        finally {
            if (Test-Path -LiteralPath $temporaryDownload -PathType Leaf) {
                Remove-Item -LiteralPath $temporaryDownload -Force
            }
        }
    }
}

function Use-ReliableMsgfmt {
    param(
        [Parameter(Mandatory = $true)][string]$CraftRoot,
        [Parameter(Mandatory = $true)][string]$SourceRoot
    )

    $nativeMsgfmt = Join-Path $CraftRoot 'bin\msgfmt.exe'
    $nativeMsgfmtBackup = Join-Path $CraftRoot 'bin\msgfmt-msvc.exe'
    $msysBin = Join-Path $CraftRoot 'msys\usr\bin'
    $msysMsgfmt = Join-Path $msysBin 'msgfmt.exe'
    $isolatedBin = Join-Path $CraftRoot 'dev-utils\vibecut-gettext\bin'
    $isolatedMsgfmt = Join-Path $isolatedBin 'msgfmt.exe'
    $probeCatalog = Join-Path $SourceRoot 'po\pl\kdenlive.po'

    foreach ($requiredFile in @($msysMsgfmt, $probeCatalog)) {
        if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
            throw "Required gettext file is missing: $requiredFile"
        }
    }

    New-Item -ItemType Directory -Path $isolatedBin -Force | Out-Null
    $runtimePatterns = @(
        'msgfmt.exe',
        'msys-2.0.dll',
        'msys-gettextsrc-*.dll',
        'msys-gettextlib-*.dll',
        'msys-intl-*.dll',
        'msys-iconv-*.dll'
    )
    foreach ($pattern in $runtimePatterns) {
        $runtimeFiles = @(Get-ChildItem -LiteralPath $msysBin -Filter $pattern -File)
        if ($runtimeFiles.Count -eq 0) {
            throw "The pinned MSYS gettext runtime is missing $pattern."
        }
        foreach ($runtimeFile in $runtimeFiles) {
            Copy-Item -LiteralPath $runtimeFile.FullName -Destination $isolatedBin -Force
        }
    }

    # Gettext's native MSVC msgfmt can terminate with STATUS_STACK_BUFFER_OVERRUN
    # on a clean Windows runner. Craft's pinned MSYS bootstrap already contains a
    # newer msgfmt, so verify an isolated copy against a real catalog before use.
    $probeDirectory = Join-Path (Join-Path $CraftRoot 'tmp') ("vibecut-msgfmt-probe-{0}" -f [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $probeDirectory -Force | Out-Null
    try {
        $probeOutput = Join-Path $probeDirectory 'kdenlive.mo'
        Invoke-Checked $isolatedMsgfmt '--check' '-o' $probeOutput $probeCatalog
        if (-not (Test-Path -LiteralPath $probeOutput -PathType Leaf) -or
            (Get-Item -LiteralPath $probeOutput).Length -eq 0) {
            throw 'The isolated MSYS msgfmt probe did not produce a compiled catalog.'
        }
    }
    finally {
        $resolvedProbeDirectory = [System.IO.Path]::GetFullPath($probeDirectory)
        $resolvedCraftTmp = [System.IO.Path]::GetFullPath((Join-Path $CraftRoot 'tmp')).TrimEnd('\') + '\'
        if ($resolvedProbeDirectory.StartsWith($resolvedCraftTmp, [System.StringComparison]::OrdinalIgnoreCase) -and
            ([System.IO.Path]::GetFileName($resolvedProbeDirectory) -like 'vibecut-msgfmt-probe-*')) {
            Remove-Item -LiteralPath $resolvedProbeDirectory -Recurse -Force
        }
    }

    if (Test-Path -LiteralPath $nativeMsgfmt -PathType Leaf) {
        Move-Item -LiteralPath $nativeMsgfmt -Destination $nativeMsgfmtBackup -Force
    }
    elseif (-not (Test-Path -LiteralPath $nativeMsgfmtBackup -PathType Leaf)) {
        throw "Craft's native msgfmt was not installed at $nativeMsgfmt."
    }

    $remainingPath = @($env:PATH -split ';') | Where-Object {
        $_ -and -not $_.Equals($isolatedBin, [System.StringComparison]::OrdinalIgnoreCase)
    }
    $env:PATH = (@($isolatedBin) + $remainingPath) -join ';'

    $selectedMsgfmt = Get-Command msgfmt.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1
    if (-not $selectedMsgfmt.Source.Equals($isolatedMsgfmt, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Expected the isolated MSYS msgfmt at $isolatedMsgfmt, but resolved $($selectedMsgfmt.Source)."
    }
    Write-Host "Using verified build-time msgfmt: $($selectedMsgfmt.Source)"
}

function Test-PackagedBuild {
    param(
        [Parameter(Mandatory = $true)][string]$ArtifactDirectory,
        [Parameter(Mandatory = $true)][string]$SevenZipPath
    )

    $portableArchive = Join-Path $ArtifactDirectory "$PackageBaseName.7z"
    $installer = Join-Path $ArtifactDirectory "$PackageBaseName.exe"
    Test-Checksum $portableArchive "$portableArchive.sha256"
    Test-Checksum $installer "$installer.sha256"

    Invoke-Checked $SevenZipPath 't' $installer

    $smokeRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("vibecut-windows-smoke-{0}" -f [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $smokeRoot | Out-Null
    try {
        Invoke-Checked $SevenZipPath 'x' $portableArchive "-o$smokeRoot" '-y'

        $packageBin = Join-Path $smokeRoot 'bin'
        $kdenlive = Join-Path $packageBin 'kdenlive.exe'
        $melt = Join-Path $packageBin 'melt.exe'
        $ffmpeg = Join-Path $packageBin 'ffmpeg.exe'
        foreach ($executable in @($kdenlive, $melt, $ffmpeg)) {
            if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
                throw "Packaged executable is missing: $executable"
            }
        }

        Invoke-Checked $melt '-version'
        Invoke-Checked $ffmpeg '-version'

        $reportPath = Join-Path $smokeRoot 'setup-report.json'
        $savedPath = $env:PATH
        try {
            $env:PATH = @($packageBin, "$env:SystemRoot\System32", $env:SystemRoot) -join ';'
            $process = Start-Process -FilePath $kdenlive -ArgumentList @('--setup-report', "`"$reportPath`"") -PassThru -Wait
            if ($process.ExitCode -ne 0) {
                throw "Packaged Kdenlive setup report failed with exit code $($process.ExitCode)."
            }
        }
        finally {
            $env:PATH = $savedPath
        }

        if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) {
            throw 'Packaged Kdenlive did not create its setup report.'
        }
        $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
        $componentNames = @($report.components | ForEach-Object { $_.name })
        foreach ($requiredComponent in @('Kdenlive', 'MLT', 'FFmpeg')) {
            if ($componentNames -notcontains $requiredComponent) {
                throw "Packaged setup report is missing $requiredComponent."
            }
        }
    }
    finally {
        $resolvedSmokeRoot = [System.IO.Path]::GetFullPath($smokeRoot)
        $resolvedTempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\') + '\'
        if ($resolvedSmokeRoot.StartsWith($resolvedTempRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
            ([System.IO.Path]::GetFileName($resolvedSmokeRoot) -like 'vibecut-windows-smoke-*')) {
            Remove-Item -LiteralPath $resolvedSmokeRoot -Recurse -Force
        }
    }
}

$sourceRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$CraftRoot = [System.IO.Path]::GetFullPath($CraftRoot)
$DownloadDirectory = if ($DownloadDirectory) { [System.IO.Path]::GetFullPath($DownloadDirectory) } else { '' }
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
$PythonPath = Resolve-Python $PythonPath

$pythonVersionText = & $PythonPath -c 'import sys; print(sys.version_info.major, sys.version_info.minor, sys.version_info.micro, sep=chr(46))'
if ($LASTEXITCODE -ne 0) {
    throw "Could not execute Python at $PythonPath."
}
$pythonVersion = [version]$pythonVersionText.Trim()
if ($pythonVersion.Major -ne 3 -or $pythonVersion.Minor -ne 11) {
    throw "This build is pinned to Python 3.11; found $pythonVersion at $PythonPath."
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw 'Visual Studio 2022 with the Desktop development with C++ workload is required.'
}
$visualStudio = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $visualStudio) {
    throw 'Visual Studio 2022 with the x64 C++ build tools was not found.'
}

$gitCommand = Get-Command git.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1
$savedEnvironment = @{
    Path = $env:PATH
    CraftRoot = $env:CraftRoot
    CraftPython = $env:CRAFT_PYTHON
}
$savedLocation = Get-Location

try {
    $pathParts = @(
        "$env:SystemRoot\System32",
        $env:SystemRoot,
        "$env:SystemRoot\System32\Wbem",
        "$env:SystemRoot\System32\WindowsPowerShell\v1.0",
        "$env:SystemRoot\System32\OpenSSH",
        (Split-Path -Parent $gitCommand.Source),
        (Split-Path -Parent $PythonPath)
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -Unique
    $env:PATH = $pathParts -join ';'

    $craftDirectory = Join-Path $CraftRoot 'craft'
    $craftScript = Join-Path $craftDirectory 'bin\craft.py'
    $env:CRAFT_PYTHON = $PythonPath

    if (-not (Test-Path -LiteralPath $craftScript -PathType Leaf)) {
        $env:CraftRoot = $null
        if (Test-Path -LiteralPath $CraftRoot) {
            $unexpectedEntries = @(Get-ChildItem -LiteralPath $CraftRoot -Force)
            if ($unexpectedEntries.Count -gt 0) {
                throw "$CraftRoot is not an empty Craft root. Choose a new -CraftRoot instead of mixing build environments."
            }
        }

        if (-not (Test-Path -LiteralPath $CraftRoot -PathType Container)) {
            New-Item -ItemType Directory -Path $CraftRoot | Out-Null
        }
        if ($DownloadDirectory) {
            New-Item -ItemType Directory -Path $DownloadDirectory -Force | Out-Null
            $bootstrapDownloadDirectory = Join-Path $CraftRoot 'download'
            if (-not $DownloadDirectory.Equals(
                    [System.IO.Path]::GetFullPath($bootstrapDownloadDirectory),
                    [System.StringComparison]::OrdinalIgnoreCase)) {
                New-Item -ItemType Junction -Path $bootstrapDownloadDirectory -Target $DownloadDirectory | Out-Null
            }
        }

        $bootstrapDirectory = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [System.IO.Path]::GetTempPath() }
        $craftSourceDirectory = Join-Path $bootstrapDirectory ("vibecut-craft-source-{0}-{1}" -f $CraftCommit, [guid]::NewGuid().ToString('N'))
        if (Test-Path -LiteralPath $craftSourceDirectory) {
            throw "Temporary Craft source path already exists: $craftSourceDirectory"
        }
        New-Item -ItemType Directory -Path $craftSourceDirectory | Out-Null
        Invoke-Checked $gitCommand.Source '-C' $craftSourceDirectory 'init'
        Invoke-Checked $gitCommand.Source '-C' $craftSourceDirectory 'remote' 'add' 'origin' 'https://invent.kde.org/packaging/craft.git'
        Invoke-Checked $gitCommand.Source '-C' $craftSourceDirectory 'fetch' '--depth=1' 'origin' $CraftCommit
        Invoke-Checked $gitCommand.Source '-C' $craftSourceDirectory 'checkout' '--detach' 'FETCH_HEAD'

        # A cold MSVC bootstrap builds gettext from source. Patch only that recipe
        # so its generated gettextlib link includes the already-built libiconv and
        # its MSVC runtime check is passed as a separate configure argument.
        $craftBootstrapPatch = Join-Path $PSScriptRoot 'craft-gettext-msvc-iconv.patch'
        Invoke-Checked $gitCommand.Source '-C' $craftSourceDirectory 'apply' '--check' $craftBootstrapPatch
        Invoke-Checked $gitCommand.Source '-C' $craftSourceDirectory 'apply' $craftBootstrapPatch

        # Craft installs its foundational libraries during bootstrap. Set the
        # final profile in the template first so those package images use the
        # same build type as Kdenlive and remain available to the packager.
        $craftSettingsTemplate = Join-Path $craftSourceDirectory 'CraftSettings.ini.template'
        Set-IniValue $craftSettingsTemplate 'Compile' 'BuildType' 'RelWithDebInfo'

        # The bootstrap installs Craft and its KDE blueprints as packages. Pin
        # those source checkouts before that first package transaction too.
        $bootstrapBlueprintSettings = Join-Path $craftSourceDirectory 'BootstrapBlueprintSettings.ini'
        Set-IniValue $bootstrapBlueprintSettings 'craft/craft-core' 'revision' $CraftCommit
        Set-IniValue $bootstrapBlueprintSettings 'craft/craft-blueprints-kde' 'revision' $KdeBlueprintCommit
        Set-IniValue $craftSettingsTemplate 'Blueprints' 'Settings' $bootstrapBlueprintSettings.Replace('\', '/')

        $craftBootstrap = Join-Path $craftSourceDirectory 'setup\CraftBootstrap.py'
        $bootstrapExitCode = 1
        & $PythonPath $craftBootstrap '--prefix' $CraftRoot '--branch' $CraftCommit '--localDev' $craftSourceDirectory '--use-defaults'
        $bootstrapExitCode = $LASTEXITCODE
        if ($bootstrapExitCode -ne 0 -or -not (Test-Path -LiteralPath $craftScript -PathType Leaf)) {
            throw 'KDE Craft bootstrap failed.'
        }
    }
    $env:CraftRoot = $craftDirectory

    $installedCraftCommit = (& $gitCommand.Source -C $craftDirectory rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $installedCraftCommit -ne $CraftCommit) {
        throw "Craft root is at $installedCraftCommit, but this build requires $CraftCommit. Use a new -CraftRoot."
    }

    $craftSettings = Join-Path $CraftRoot 'etc\CraftSettings.ini'
    $blueprintSettings = Join-Path $CraftRoot 'etc\BlueprintSettings.ini'
    Set-IniValue $blueprintSettings 'craft/craft-core' 'revision' $CraftCommit
    Set-IniValue $blueprintSettings 'craft/craft-blueprints-kde' 'revision' $KdeBlueprintCommit
    Set-IniValue $craftSettings 'Blueprints' 'Settings' $blueprintSettings.Replace('\', '/')
    if ($DownloadDirectory) {
        New-Item -ItemType Directory -Path $DownloadDirectory -Force | Out-Null
        Set-IniValue $craftSettings 'Paths' 'DownloadDir' $DownloadDirectory
    }
    # Match KDE's published Windows binary cache instead of inheriting a bootstrap default.
    Set-IniValue $craftSettings 'Compile' 'BuildType' 'RelWithDebInfo'
    Set-IniValue $craftSettings 'BlueprintVersions' 'EnableDailyUpdates' 'False'
    Set-IniValue $craftSettings 'Packager' 'PackageType' 'NullsoftInstallerPackager'
    Set-IniValue $craftSettings 'Packager' 'PackageDebugSymbols' 'False'
    Set-IniValue $craftSettings 'ShortPath' 'DriveLetter' 'Z:/'

    $blueprintDirectory = Join-Path $CraftRoot 'etc\blueprints\locations\craft-blueprints-kde'
    if (-not (Test-Path -LiteralPath (Join-Path $blueprintDirectory '.git'))) {
        throw "KDE Craft bootstrap did not install the KDE blueprint repository at $blueprintDirectory."
    }
    $installedBlueprintCommit = (& $gitCommand.Source -C $blueprintDirectory rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not read the KDE blueprint revision.'
    }
    if ($installedBlueprintCommit -ne $KdeBlueprintCommit) {
        Invoke-Checked $gitCommand.Source '-C' $blueprintDirectory 'fetch' 'origin' $KdeBlueprintCommit '--depth=1'
        Invoke-Checked $gitCommand.Source '-C' $blueprintDirectory 'checkout' '--detach' $KdeBlueprintCommit
    }

    # SourceForge occasionally truncates this archive on GitHub-hosted runners.
    # Seed the same tagged source from libpng's official GitHub repository and
    # verify it before Craft starts resolving the Kdenlive dependency graph.
    if ($DownloadDirectory) {
        $libPngArchive = Join-Path $DownloadDirectory 'archives\libs\libpng\libpng-1.6.45.tar.gz'
        Save-VerifiedDownload $LibPngArchiveUrl $libPngArchive $LibPngArchiveSha256
    }

    $sourceOption = 'kdenlive.srcDir=' + $sourceRoot.Replace('\', '/')
    $commonCraftArguments = @(
        '--ci-mode',
        '--output-on-failure',
        '--options', $sourceOption,
        '--options', "craft/craft-core.revision=$CraftCommit",
        '--options', "craft/craft-blueprints-kde.revision=$KdeBlueprintCommit"
    )

    # Build gettext first so its native tool can be replaced before GLib and the
    # rest of Kdenlive's dependency graph compile their translation catalogs.
    Invoke-Checked $PythonPath $craftScript @commonCraftArguments 'libs/gettext'
    Use-ReliableMsgfmt -CraftRoot $CraftRoot -SourceRoot $sourceRoot
    Invoke-Checked $PythonPath $craftScript @commonCraftArguments '--install-deps' 'kdenlive'
    Invoke-Checked $PythonPath $craftScript @commonCraftArguments '--no-cache' '--ignoreInstalled' 'kdenlive'

    $installedExecutable = Join-Path $CraftRoot 'bin\kdenlive.exe'
    if (-not (Test-Path -LiteralPath $installedExecutable -PathType Leaf)) {
        throw "Craft completed without installing $installedExecutable."
    }

    if (-not $BuildOnly) {
        Invoke-Checked $PythonPath $craftScript @commonCraftArguments 'nsis'
        Invoke-Checked $PythonPath $craftScript @commonCraftArguments '--package' 'kdenlive'

        New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
        foreach ($suffix in @('.7z', '.7z.sha256', '.exe', '.exe.sha256')) {
            $sourceArtifact = Join-Path (Join-Path $CraftRoot 'tmp') "$PackageBaseName$suffix"
            if (-not (Test-Path -LiteralPath $sourceArtifact -PathType Leaf)) {
                throw "Expected package artifact was not created: $sourceArtifact"
            }
            Copy-Item -LiteralPath $sourceArtifact -Destination $OutputDirectory -Force
        }

        $sevenZip = Join-Path $CraftRoot 'dev-utils\bin\7za.exe'
        if (-not (Test-Path -LiteralPath $sevenZip -PathType Leaf)) {
            throw "Craft 7-Zip executable was not found at $sevenZip."
        }
        Test-PackagedBuild $OutputDirectory $sevenZip
    }

    $finalBlueprintCommit = (& $gitCommand.Source -C $blueprintDirectory rev-parse HEAD).Trim()
    if ($finalBlueprintCommit -ne $KdeBlueprintCommit) {
        throw "KDE blueprint revision changed during the build: $finalBlueprintCommit"
    }

    Write-Host "VibeCut Windows build succeeded: $installedExecutable"
    if (-not $BuildOnly) {
        Write-Host "Validated packages: $OutputDirectory"
        if ($env:GITHUB_STEP_SUMMARY) {
            @(
                '## VibeCut Windows build',
                '',
                '- MSVC 2022 build: passed',
                '- Portable archive checksum and runtime smoke test: passed',
                '- NSIS installer checksum and archive validation: passed'
            ) | Out-File -LiteralPath $env:GITHUB_STEP_SUMMARY -Encoding utf8 -Append
        }
    }
}
finally {
    Set-Location $savedLocation
    $env:PATH = $savedEnvironment.Path
    $env:CraftRoot = $savedEnvironment.CraftRoot
    $env:CRAFT_PYTHON = $savedEnvironment.CraftPython
}
