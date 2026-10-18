Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\..\tools\build-windows.ps1"

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

$failed = $false
try {
    Invoke-Native $env:ComSpec @('/d', '/c', 'exit 7')
} catch {
    $failed = $_.Exception.Message -match 'exit code 7'
}
Assert-True $failed 'Native command failures must throw with the exit code.'

$fixture = Join-Path ([IO.Path]::GetTempPath()) "swiftify-build-test-$([guid]::NewGuid()) with spaces"
$originalPath = $env:PATH
$originalQtRoot = $env:QT_ROOT_DIR
$originalCargoJobs = $env:CARGO_BUILD_JOBS
$originalLocation = (Get-Location).Path
$script:invocations = New-Object 'System.Collections.Generic.List[object]'
$script:configurations = @{}
$script:failBuild = $false
$script:msvcInitializations = 0
$script:rustToolchains = @{}
$script:compilerVersion = '15.1.0'

function New-FixtureFile {
    param([string]$Path, [string]$Content = 'fixture')
    New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force | Out-Null
    Set-Content -LiteralPath $Path -Value $Content
}

function Find-Application {
    param([string]$Name, [string[]]$Hints = @())
    if ($Name -eq 'cl.exe') {
        Assert-True ($env:SWIFTIFY_TEST_VS_ENVIRONMENT -eq 'initialized') 'The MSVC developer environment must be initialized before locating cl.exe.'
        $script:msvcInitializations++
    }
    return Join-Path $fixture $Name
}

function Invoke-Native {
    param([string]$FilePath, [string[]]$Arguments = @())
    $name = Split-Path -Leaf $FilePath
    $script:invocations.Add([pscustomobject]@{ Name = $name; Arguments = $Arguments })
    switch ($name) {
        'vswhere.exe' { return Join-Path $fixture 'VS' }
        'g++.exe' {
            if ($Arguments -contains '-dumpmachine') { return 'x86_64-w64-mingw32' }
            if ($Arguments -contains '-dumpfullversion') { return $script:compilerVersion }
            throw "Unexpected compiler invocation: $Arguments"
        }
        'rustup.exe' {
            if ($Arguments[0] -eq 'toolchain' -and $Arguments[1] -eq 'list') {
                return @($script:rustToolchains.Keys)
            }
            if ($Arguments[0] -eq 'toolchain' -and $Arguments[1] -eq 'install') {
                $script:rustToolchains[$Arguments[2]] = $true
            }
        }
        'git.exe' {
            $destination = $Arguments[-1]
            New-Item -ItemType Directory -Path "$destination\.git" -Force | Out-Null
            New-FixtureFile "$destination\CMakeLists.txt"
        }
        'cmake.exe' {
            if ($Arguments -contains '-S') {
                $build = $Arguments[[array]::IndexOf($Arguments, '-B') + 1]
                $prefix = @($Arguments | Where-Object { $_ -like '-DCMAKE_INSTALL_PREFIX=*' })[-1].Substring('-DCMAKE_INSTALL_PREFIX='.Length)
                $script:configurations[$build] = $prefix
                Assert-True (@($Arguments | Where-Object { $_ -like '-DCMAKE_PREFIX_PATH=*;*' }).Count -eq 1) 'The CMake prefix list must remain a single argument.'
            } elseif ($Arguments -contains '--build') {
                if ($script:failBuild) { throw 'Simulated compile failure' }
            } elseif ($Arguments -contains '--install') {
                $build = $Arguments[1]
                $prefix = $script:configurations[$build]
                $project = Split-Path -Leaf $build
                if ($project -eq 'swiftify') {
                    New-FixtureFile "$prefix\bin\swiftify.exe"
                } else {
                    New-FixtureFile "$prefix\bin\$project.dll"
                }
            } else {
                throw "Unexpected CMake invocation: $Arguments"
            }
        }
        'windeployqt.exe' {
            Assert-True (Test-Path -LiteralPath $Arguments[-1]) 'Swiftify must be installed before deployment.'
            New-FixtureFile (Join-Path (Split-Path -Parent $Arguments[-1]) 'libstdc++-6.dll') 'outdated deployment runtime'
            New-FixtureFile (Join-Path (Split-Path -Parent $Arguments[-1]) 'vc_redist.x64.exe') 'runtime installer'
        }
        default { throw "Unexpected command: $FilePath" }
    }
}

try {
    $qtPath = Join-Path $fixture 'Qt\6.12.0\mingw_64'
    $mingwPath = Join-Path $fixture 'MinGW\bin'
    $buildRoot = Join-Path $fixture 'build'
    $installPrefix = Join-Path $fixture 'installed'
    New-FixtureFile "$qtPath\mkspecs\qconfig.pri" "QT_VERSION = 6.12.0`nQT_ARCH = x86_64`nQT_GCC_MAJOR_VERSION = 15`nQT_GCC_MINOR_VERSION = 1`nQT_GCC_PATCH_VERSION = 0"
    foreach ($module in @('Qt6Core', 'Qt6Widgets', 'Qt6Quick', 'Qt6QuickControls2', 'Qt6ShaderTools')) {
        New-FixtureFile "$qtPath\lib\cmake\$module\${module}Config.cmake"
    }
    foreach ($file in @('gcc.exe', 'g++.exe', 'libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')) {
        New-FixtureFile "$mingwPath\$file" 'selected compiler runtime'
    }
    $env:QT_ROOT_DIR = $qtPath
    Assert-True ((Find-QtKit '') -eq $qtPath) 'Qt discovery must respect QT_ROOT_DIR.'
    $parameters = @{ QtPath = $qtPath; MingwPath = $mingwPath; BuildRoot = $buildRoot; InstallPrefix = $installPrefix; Jobs = 2 }
    Invoke-WindowsBuild @parameters
    Assert-True (Test-Path -LiteralPath "$installPrefix\bin\swiftify.exe") 'Swiftify was not installed.'
    Assert-True (Test-Path -LiteralPath "$installPrefix\bin\qtkeychain.dll") 'QtKeychain runtime was not deployed.'
    Assert-True (Test-Path -LiteralPath "$installPrefix\bin\qcoro.dll") 'QCoro runtime was not deployed.'
    Assert-True ((Get-Content -LiteralPath "$installPrefix\bin\libstdc++-6.dll" -Raw).Trim() -eq 'selected compiler runtime') 'The selected compiler runtime must replace the deployment tool runtime.'
    Assert-True (@($script:invocations | Where-Object { $_.Arguments -contains '-DRust_TOOLCHAIN=stable-x86_64-pc-windows-gnu' }).Count -eq 1) 'The GNU Rust toolchain must be passed explicitly.'
    Assert-True (@($script:invocations | Where-Object { $_.Arguments -like '-DCMAKE_PROJECT_INCLUDE=*' }).Count -eq 0) 'MinGW already names QtKeychain libqt6keychain.dll.'
    $firstConfigurations = @($script:configurations.Keys | Sort-Object)
    Invoke-WindowsBuild @parameters
    Assert-True (@($script:invocations | Where-Object { $_.Name -eq 'git.exe' }).Count -eq 2) 'Rerunning must reuse dependency checkouts.'
    Assert-True (($firstConfigurations -join '|') -eq (@($script:configurations.Keys | Sort-Object) -join '|')) 'Rerunning must reuse build directories.'
    Assert-True (@($script:invocations | Where-Object { $_.Name -eq 'rustup.exe' -and $_.Arguments -contains 'install' }).Count -eq 1) 'Rerunning must reuse the installed Rust toolchain.'

    $script:compilerVersion = '13.1.0'
    $failed = $false
    try { Find-MingwCompiler $mingwPath '' ([version]'15.1.0') | Out-Null } catch { $failed = $_.Exception.Message -match 'No suitable x64 MinGW compiler' }
    Assert-True $failed 'An older MinGW compiler must be rejected.'
    $script:compilerVersion = '15.1.0'

    $script:failBuild = $true
    $script:invocations.Clear()
    $failed = $false
    try { Invoke-WindowsBuild @parameters } catch { $failed = $_.Exception.Message -eq 'Simulated compile failure' }
    Assert-True $failed 'A failed build must stop the script.'
    Assert-True (@($script:invocations | Where-Object { $_.Arguments -contains '--install' -or $_.Name -eq 'windeployqt.exe' }).Count -eq 0) 'Installation must not continue after a build failure.'
    $script:failBuild = $false

    $msvcQt = Join-Path $fixture 'Qt\6.12.0\msvc2022_64'
    New-FixtureFile "$fixture\cl.exe"
    New-FixtureFile "$fixture\VS\Common7\Tools\Launch-VsDevShell.ps1" @'
param([string]$Arch, [string]$HostArch, [switch]$SkipAutomaticLocation)
if ($Arch -ne 'amd64' -or $HostArch -ne 'amd64' -or -not $SkipAutomaticLocation) {
    throw 'The developer shell must target x64 without changing directories.'
}
$env:SWIFTIFY_TEST_VS_ENVIRONMENT = 'initialized'
$env:VCToolsRedistDir = Join-Path $PSScriptRoot '..\..\VC\Redist\MSVC\14.44'
'@
    New-FixtureFile "$fixture\VS\VC\Redist\MSVC\14.44\x64\Microsoft.VC143.CRT\vcruntime140.dll" 'msvc runtime'
    Copy-Item -LiteralPath $qtPath -Destination $msvcQt -Recurse
    New-FixtureFile "$msvcQt\mkspecs\qconfig.pri" "QT_VERSION = 6.12.0`nQT_ARCH = x86_64"
    $parameters.QtPath = $msvcQt
    $parameters.MingwPath = ''
    $script:invocations.Clear()
    Invoke-WindowsBuild @parameters
    Assert-True ($script:msvcInitializations -eq 1) 'MSVC must initialize its developer environment.'
    $prefixArgument = @($script:invocations | ForEach-Object { $_.Arguments } | Where-Object { $_ -like '-DCMAKE_PROJECT_INCLUDE=*' })
    Assert-True ($prefixArgument.Count -eq 1) 'QtKeychain must be built as libqt6keychain.dll with MSVC, or windeployqt takes it for part of Qt.'
    Assert-True ((Get-Content -LiteralPath ($prefixArgument[0] -replace '^-DCMAKE_PROJECT_INCLUDE=', '') -Raw).Trim() -eq 'set(CMAKE_SHARED_LIBRARY_PREFIX lib)') 'The MSVC QtKeychain build must use the lib prefix.'
    Assert-True ((Get-Content -LiteralPath "$installPrefix\bin\vcruntime140.dll" -Raw).Trim() -eq 'msvc runtime') 'The MSVC runtime must be bundled beside the executable.'
    Assert-True (-not (Test-Path -LiteralPath "$installPrefix\bin\vc_redist.x64.exe")) 'The MSVC runtime installer must not be shipped.'
    Assert-True (@($script:invocations | Where-Object { $_.Arguments -contains '-DRust_TOOLCHAIN=stable-x86_64-pc-windows-msvc' }).Count -eq 1) 'MSVC must select its matching Rust toolchain.'
    Assert-True ($script:configurations.Count -eq 6) 'Different Qt/compiler kits must have separate build directories.'
    Assert-True ($env:PATH -eq $originalPath) 'The original PATH must be restored.'
    Assert-True ($env:CARGO_BUILD_JOBS -eq $originalCargoJobs) 'The original Cargo parallelism must be restored.'
    Assert-True (-not (Test-Path Env:SWIFTIFY_TEST_VS_ENVIRONMENT)) 'Developer shell environment changes must not leak into the caller.'
    Assert-True ((Get-Location).Path -eq $originalLocation) 'The original working directory must be restored.'
    Write-Host 'Windows build script tests passed.'
} finally {
    $env:PATH = $originalPath
    $env:QT_ROOT_DIR = $originalQtRoot
    $env:CARGO_BUILD_JOBS = $originalCargoJobs
    $resolvedFixture = [IO.Path]::GetFullPath($fixture)
    $temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if ($resolvedFixture.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -and (Split-Path -Leaf $resolvedFixture) -like 'swiftify-build-test-*') {
        Remove-Item -LiteralPath $resolvedFixture -Recurse -Force -ErrorAction SilentlyContinue
    }
}
