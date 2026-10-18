[CmdletBinding()]
param(
    [string]$QtPath,
    [string]$MingwPath,
    [string]$BuildRoot,
    [string]$InstallPrefix = "$env:LOCALAPPDATA\Programs\Swiftify",
    [ValidateRange(1, 256)]
    [int]$Jobs = [Math]::Max(1, [Math]::Min(8, [Environment]::ProcessorCount))
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

function Invoke-Native {
    param([string]$FilePath, [string[]]$Arguments = @())

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code ${LASTEXITCODE}: $($Arguments -join ' ')"
    }
}

function Find-Application {
    param([string]$Name, [string[]]$Hints = @())

    foreach ($hint in $Hints) {
        if (Test-Path -LiteralPath $hint -PathType Leaf) {
            return (Resolve-Path -LiteralPath $hint).Path
        }
    }
    $command = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) {
        return $command.Source
    }
    throw "Cannot find $Name. Install it and add its directory to PATH, then rerun this script."
}

function Find-QtKit {
    param([string]$RequestedPath)

    if ($RequestedPath) {
        return (Resolve-Path -LiteralPath $RequestedPath).Path
    }
    foreach ($candidate in @($env:QT_ROOT_DIR, $env:QT)) {
        if ($candidate -and (Test-Path -LiteralPath "$candidate\mkspecs\qconfig.pri")) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    $candidates = @(
        foreach ($root in @('C:\Qt', "$env:USERPROFILE\Qt")) {
            if (Test-Path -LiteralPath $root) {
                foreach ($versionDirectory in Get-ChildItem -LiteralPath $root -Directory) {
                    $version = $null
                    if ([version]::TryParse($versionDirectory.Name, [ref]$version) -and $version -ge [version]'6.9') {
                        foreach ($kit in @('msvc2022_64', 'mingw_64')) {
                            $kitPath = Join-Path $versionDirectory.FullName $kit
                            if (Test-Path -LiteralPath "$kitPath\mkspecs\qconfig.pri") {
                                [pscustomobject]@{ Path = $kitPath; Version = $version }
                            }
                        }
                    }
                }
            }
        }
    )
    $selected = $candidates | Sort-Object Version -Descending | Select-Object -First 1
    if (-not $selected) {
        throw 'No Qt 6.9+ x64 kit found. Install Qt, then pass -QtPath pointing to its msvc2022_64 or mingw_64 directory.'
    }
    return $selected.Path
}

function Find-MingwCompiler {
    param([string]$RequestedPath, [string]$QtTools, [version]$MinimumVersion)

    $candidates = @()
    if ($RequestedPath) {
        $directory = (Resolve-Path -LiteralPath $RequestedPath).Path
        if (Test-Path -LiteralPath "$directory\bin\g++.exe") {
            $directory = Join-Path $directory 'bin'
        }
        $candidates = @($directory)
    } else {
        if (Test-Path -LiteralPath $QtTools) {
            $candidates += @(Get-ChildItem -LiteralPath $QtTools -Directory -Filter 'mingw*' | ForEach-Object { Join-Path $_.FullName 'bin' })
        }
        $compiler = Get-Command g++.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($compiler) {
            $candidates += Split-Path -Parent $compiler.Source
        }
    }
    $compatible = @(
        foreach ($directory in ($candidates | Select-Object -Unique)) {
            $compiler = Join-Path $directory 'g++.exe'
            if (-not (Test-Path -LiteralPath $compiler) -or -not (Test-Path -LiteralPath "$directory\gcc.exe")) {
                continue
            }
            $target = (Invoke-Native $compiler @('-dumpmachine') | Out-String).Trim()
            $version = [version](Invoke-Native $compiler @('-dumpfullversion') | Out-String).Trim()
            if ($target -eq 'x86_64-w64-mingw32' -and $version -ge $MinimumVersion) {
                [pscustomobject]@{ Directory = $directory; Version = $version }
            }
        }
    )
    $selected = $compatible | Sort-Object Version | Select-Object -First 1
    if (-not $selected) {
        throw "No suitable x64 MinGW compiler found (Qt requires GCC $MinimumVersion or newer). Install Qt's matching toolchain or pass -MingwPath pointing to its bin directory."
    }
    if ($selected.Version -ne $MinimumVersion) {
        Write-Warning "Using GCC $($selected.Version) with Qt built using GCC $MinimumVersion. Qt's matching MinGW toolchain is recommended."
    }
    return $selected
}

function Initialize-Msvc {
    $vswhere = Find-Application 'vswhere.exe' @("${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe")
    $installation = (Invoke-Native $vswhere @('-latest', '-products', '*', '-version', '[17.1,18.0)', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath') | Out-String).Trim()
    if (-not $installation) {
        throw 'Install Visual Studio 2022 17.1+ (or Build Tools) with Desktop development with C++.'
    }
    & "$installation\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Host
    return Find-Application 'cl.exe'
}

function Get-DependencySource {
    param([string]$Git, [string]$Url, [string]$Destination)

    if (-not (Test-Path -LiteralPath $Destination)) {
        Invoke-Native $Git @('clone', '--depth', '1', $Url, $Destination) | Out-Host
    }
    if (-not (Test-Path -LiteralPath "$Destination\.git") -or -not (Test-Path -LiteralPath "$Destination\CMakeLists.txt")) {
        throw "Incomplete dependency checkout at $Destination. Use a different -BuildRoot or remove that incomplete checkout before retrying."
    }
}

function Invoke-WindowsBuild {
    param(
        [string]$QtPath,
        [string]$MingwPath,
        [string]$BuildRoot,
        [string]$InstallPrefix,
        [int]$Jobs
    )

    if ($env:OS -ne 'Windows_NT') {
        throw 'This build script requires Windows.'
    }
    $sourceRoot = Split-Path -Parent $PSScriptRoot
    if (-not $BuildRoot) {
        $BuildRoot = Join-Path $sourceRoot 'build-windows'
    }
    $BuildRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildRoot)
    $InstallPrefix = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($InstallPrefix)
    $QtPath = Find-QtKit $QtPath
    $qtConfiguration = Get-Content -LiteralPath "$QtPath\mkspecs\qconfig.pri" -Raw
    $qtVersion = [regex]::Match($qtConfiguration, '(?m)^QT_VERSION\s*=\s*(\S+)').Groups[1].Value
    if (-not $qtVersion -or [version]$qtVersion -lt [version]'6.9' -or $qtConfiguration -notmatch '(?m)^QT_ARCH\s*=\s*x86_64\s*$') {
        throw 'Swiftify requires a Qt 6.9+ x64 kit.'
    }
    foreach ($module in @('Qt6Core', 'Qt6Widgets', 'Qt6Quick', 'Qt6QuickControls2', 'Qt6ShaderTools')) {
        if (-not (Test-Path -LiteralPath "$QtPath\lib\cmake\$module\${module}Config.cmake")) {
            throw "The selected Qt kit is missing $module. Install Qt Base, Qt Declarative and Qt Shader Tools with the Qt installer."
        }
    }
    $qtTools = Join-Path (Split-Path -Parent (Split-Path -Parent $QtPath)) 'Tools'
    $originalEnvironment = @{}
    foreach ($entry in Get-ChildItem Env:) {
        $originalEnvironment[$entry.Name] = $entry.Value
    }
    Push-Location $sourceRoot
    try {
        $env:PATH = "$QtPath\bin;$env:USERPROFILE\.cargo\bin;$env:PATH"
        $env:CARGO_BUILD_JOBS = "$Jobs"
        $git = Find-Application 'git.exe'
        $rustup = Find-Application 'rustup.exe'
        $cmake = Find-Application 'cmake.exe' @("$qtTools\CMake_64\bin\cmake.exe")
        $ninja = Find-Application 'ninja.exe' @("$qtTools\Ninja\ninja.exe")
        $deployQt = Find-Application 'windeployqt.exe' @("$QtPath\bin\windeployqt.exe")
        $gccMajor = [regex]::Match($qtConfiguration, '(?m)^QT_GCC_MAJOR_VERSION\s*=\s*(\d+)')
        if ($gccMajor.Success) {
            $gccMinor = [regex]::Match($qtConfiguration, '(?m)^QT_GCC_MINOR_VERSION\s*=\s*(\d+)').Groups[1].Value
            $gccPatch = [regex]::Match($qtConfiguration, '(?m)^QT_GCC_PATCH_VERSION\s*=\s*(\d+)').Groups[1].Value
            $mingw = Find-MingwCompiler $MingwPath $qtTools ([version]"$($gccMajor.Groups[1].Value).$gccMinor.$gccPatch")
            $cCompiler = Join-Path $mingw.Directory 'gcc.exe'
            $cxxCompiler = Join-Path $mingw.Directory 'g++.exe'
            $compilerVersion = $mingw.Version.ToString()
            $env:PATH = "$($mingw.Directory);$env:PATH"
            $rustTarget = 'x86_64-pc-windows-gnu'
        } else {
            if ($MingwPath) {
                throw '-MingwPath cannot be used with an MSVC Qt kit.'
            }
            $cCompiler = Initialize-Msvc
            $cxxCompiler = $cCompiler
            $compilerVersion = (Get-Item -LiteralPath $cCompiler).VersionInfo.FileVersion
            $rustTarget = 'x86_64-pc-windows-msvc'
        }
        $rustToolchain = "stable-$rustTarget"
        $installedToolchains = @(Invoke-Native $rustup @('toolchain', 'list'))
        if (-not ($installedToolchains -match "^$([regex]::Escape($rustToolchain))(\s|$)")) {
            Invoke-Native $rustup @('toolchain', 'install', $rustToolchain, '--profile', 'minimal') | Out-Host
        }
        Invoke-Native $rustup @('target', 'add', $rustTarget, '--toolchain', $rustToolchain) | Out-Host

        $hashAlgorithm = [System.Security.Cryptography.SHA256]::Create()
        try {
            $identity = [Text.Encoding]::UTF8.GetBytes("$QtPath|$qtVersion|$cxxCompiler|$compilerVersion")
            $configurationId = ([BitConverter]::ToString($hashAlgorithm.ComputeHash($identity))).Replace('-', '').Substring(0, 12).ToLowerInvariant()
        } finally {
            $hashAlgorithm.Dispose()
        }
        $buildDirectory = Join-Path $BuildRoot "$rustTarget-$configurationId"
        $dependencyPrefix = Join-Path $buildDirectory 'deps'
        $sourcesDirectory = Join-Path $BuildRoot 'sources'
        New-Item -ItemType Directory -Path $sourcesDirectory, $buildDirectory -Force | Out-Null
        $env:PATH = "$dependencyPrefix\bin;$QtPath\bin;$env:PATH"
        Write-Host "Qt: $QtPath"
        Write-Host "Compiler: $cxxCompiler ($compilerVersion)"
        Write-Host "Build directory: $buildDirectory"
        Write-Host "Install directory: $InstallPrefix"

        $commonOptions = @(
            '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_TESTING=OFF',
            "-DCMAKE_MAKE_PROGRAM=$($ninja.Replace('\', '/'))",
            "-DCMAKE_C_COMPILER=$($cCompiler.Replace('\', '/'))",
            "-DCMAKE_CXX_COMPILER=$($cxxCompiler.Replace('\', '/'))",
            "-DCMAKE_PREFIX_PATH=$($QtPath.Replace('\', '/'));$($dependencyPrefix.Replace('\', '/'))",
            "-DCMAKE_INSTALL_PREFIX=$($dependencyPrefix.Replace('\', '/'))", '-DCMAKE_INSTALL_BINDIR=bin'
        )
        $dependencies = @(
            @{ Name = 'qtkeychain'; Url = 'https://github.com/frankosterfeld/qtkeychain.git'; Options = @('-DBUILD_WITH_QT5=OFF', '-DBUILD_TEST_APPLICATION=OFF', '-DBUILD_TRANSLATIONS=OFF') },
            @{ Name = 'qcoro'; Url = 'https://github.com/qcoro/qcoro.git'; Options = @('-DUSE_QT_VERSION=6', '-DQCORO_WITH_QTDBUS=OFF', '-DQCORO_WITH_QTWEBSOCKETS=OFF', '-DQCORO_WITH_QTQUICK=OFF', '-DQCORO_WITH_QML=OFF', '-DQCORO_WITH_QTTEST=OFF', '-DQCORO_BUILD_EXAMPLES=OFF') }
        )
        if ($rustTarget -eq 'x86_64-pc-windows-msvc') {
            # windeployqt takes any dll named qt6... for one of qt's own and
            # looks for it in qt's folder, where qt6keychain.dll is not. it gets
            # the lib prefix mingw already gives it
            $libraryPrefix = Join-Path $buildDirectory 'library-prefix.cmake'
            Set-Content -LiteralPath $libraryPrefix -Value 'set(CMAKE_SHARED_LIBRARY_PREFIX lib)'
            $dependencies[0].Options += "-DCMAKE_PROJECT_INCLUDE=$($libraryPrefix.Replace('\', '/'))"
        }
        foreach ($dependency in $dependencies) {
            Write-Host "Building $($dependency.Name)..."
            $dependencySource = Join-Path $sourcesDirectory $dependency.Name
            $dependencyBuild = Join-Path $buildDirectory $dependency.Name
            Get-DependencySource $git $dependency.Url $dependencySource
            Invoke-Native $cmake (@('-S', $dependencySource, '-B', $dependencyBuild) + $commonOptions + $dependency.Options) | Out-Host
            Invoke-Native $cmake @('--build', $dependencyBuild, '--parallel', "$Jobs") | Out-Host
            Invoke-Native $cmake @('--install', $dependencyBuild) | Out-Host
        }

        Write-Host 'Building Swiftify...'
        $appBuild = Join-Path $buildDirectory 'swiftify'
        Invoke-Native $cmake (@('-S', $sourceRoot, '-B', $appBuild) + $commonOptions + @("-DRust_TOOLCHAIN=$rustToolchain", "-DRust_CARGO_TARGET=$rustTarget", "-DCMAKE_INSTALL_PREFIX=$($InstallPrefix.Replace('\', '/'))")) | Out-Host
        Invoke-Native $cmake @('--build', $appBuild, '--parallel', "$Jobs") | Out-Host
        Invoke-Native $cmake @('--install', $appBuild) | Out-Host
        $installBin = Join-Path $InstallPrefix 'bin'
        $executable = Join-Path $installBin 'swiftify.exe'
        Invoke-Native $deployQt @('--release', '--force', '--compiler-runtime', '--qmldir', (Join-Path $sourceRoot 'qml'), $executable) | Out-Host
        Copy-Item -Path "$dependencyPrefix\bin\*.dll" -Destination $installBin -Force
        if ($rustTarget -eq 'x86_64-pc-windows-msvc') {
            # windeployqt only leaves msvc's runtime installer, so the runtime
            # goes beside the exe and it runs without visual c++ installed
            $runtimeDirectory = Get-ChildItem -LiteralPath (Join-Path $env:VCToolsRedistDir 'x64') -Directory -Filter 'Microsoft.VC*.CRT' | Select-Object -First 1
            if (-not $runtimeDirectory) {
                throw 'Cannot locate the Visual C++ runtime to bundle.'
            }
            Copy-Item -Path "$($runtimeDirectory.FullName)\*.dll" -Destination $installBin -Force
            Remove-Item -LiteralPath (Join-Path $installBin 'vc_redist.x64.exe') -ErrorAction SilentlyContinue
        }
        if ($rustTarget -eq 'x86_64-pc-windows-gnu') {
            foreach ($runtime in @('libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')) {
                $runtimePath = Join-Path $mingw.Directory $runtime
                if (-not (Test-Path -LiteralPath $runtimePath -PathType Leaf)) {
                    $runtimePath = (Invoke-Native $cxxCompiler @("-print-file-name=$runtime") | Out-String).Trim()
                }
                if (-not (Test-Path -LiteralPath $runtimePath -PathType Leaf)) {
                    throw "Cannot locate the compiler runtime $runtime in the selected MinGW toolchain."
                }
                Copy-Item -LiteralPath $runtimePath -Destination $installBin -Force
            }
        }
        Write-Host "Installed Swiftify. Run: & `"$executable`""
    } finally {
        foreach ($entry in Get-ChildItem Env:) {
            if (-not $originalEnvironment.ContainsKey($entry.Name)) {
                Remove-Item -LiteralPath "Env:$($entry.Name)"
            }
        }
        foreach ($name in $originalEnvironment.Keys) {
            Set-Item -LiteralPath "Env:$name" -Value $originalEnvironment[$name]
        }
        Pop-Location
    }
}

if ($MyInvocation.InvocationName -ne '.') {
    try {
        Invoke-WindowsBuild -QtPath $QtPath -MingwPath $MingwPath -BuildRoot $BuildRoot -InstallPrefix $InstallPrefix -Jobs $Jobs
    } catch {
        Write-Error $_ -ErrorAction Continue
        exit 1
    }
}
