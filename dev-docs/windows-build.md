<!--
SPDX-FileCopyrightText: 2026 VibeCut contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Reproducible Windows build

VibeCut's supported Windows build uses KDE Craft with MSVC 2022. The build script pins both Craft and the KDE blueprint repository, installs the Kdenlive dependency graph from KDE's binary cache, builds this checkout, creates a portable archive and an NSIS installer, and validates both outputs.

## Requirements

- 64-bit Windows
- Visual Studio 2022 with **Desktop development with C++**
- Python 3.11 (64-bit)
- Git
- PowerShell 5.1 or newer

## Build and package

From the repository root:

```powershell
.\packaging\windows\build.ps1
```

The first run bootstraps an isolated Craft environment at `C:\CraftVibeCut`. Later runs reuse its dependencies but always rebuild VibeCut from the current checkout. Packages are written to `artifacts\windows`.

To compile and install into the Craft root without creating packages:

```powershell
.\packaging\windows\build.ps1 -BuildOnly
```

Use `-CraftRoot` to select another dedicated environment, `-DownloadDirectory` to keep a reusable download cache outside that environment, `-PythonPath` to select a specific Python 3.11 executable, and `-OutputDirectory` to place packages elsewhere. Do not share the Craft root with another project.

The package validation checks SHA-256 sidecars, tests the NSIS payload, extracts the portable archive, runs MLT and FFmpeg, and asks the packaged VibeCut executable to produce its component setup report.

## GitHub Actions

`.github/workflows/windows-build.yml` runs the same script on `windows-2022` for pull requests, pushes to `vibecut`, and manual dispatches. It caches only Craft downloads; the compiler environment and install tree are recreated on each runner so cached state cannot hide build failures.

Rattler is not used for this target because VibeCut depends on the KDE Frameworks, MLT, and Kdenlive packaging graph maintained by KDE Craft. Craft provides the Windows binary cache and installer logic used by this build.
