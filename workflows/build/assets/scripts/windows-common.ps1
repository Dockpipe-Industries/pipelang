$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$script:SourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$script:PowerShell = Join-Path $PSHOME 'powershell.exe'

function Require-WindowsHost {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT -or [IntPtr]::Size -ne 8) {
        throw 'The Windows native workflow requires native x64 Windows PowerShell'
    }
}
function Get-RequiredEnvironment([string] $Name) {
    $value = [Environment]::GetEnvironmentVariable($Name)
    if ([string]::IsNullOrWhiteSpace($value)) {
        throw "Select $Name explicitly"
    }
    return $value
}
function Get-AbsolutePath([string] $Path) {
    if (![IO.Path]::IsPathRooted($Path)) {
        throw "An absolute native path is required: $Path"
    }
    return [IO.Path]::GetFullPath($Path)
}
function Get-ExternalProof([string] $Path) {
    $absolute = Get-AbsolutePath $Path
    $source = $script:SourceRoot.TrimEnd('\') + '\'
    if ($absolute.StartsWith($source, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Native proof must be outside source'
    }
    return $absolute
}
function Write-JsonFile([string] $Path, $Value) {
    $text = ($Value | ConvertTo-Json -Depth 24) + "`n"
    $temporary = Join-Path ([IO.Path]::GetDirectoryName($Path)) ('.pipelang-' + [Guid]::NewGuid().ToString('N'))
    try {
        [IO.File]::WriteAllText($temporary, $text, (New-Object Text.UTF8Encoding($false)))
        if ([IO.File]::Exists($Path)) {
            [IO.File]::Replace($temporary, $Path, $null)
        } else {
            [IO.File]::Move($temporary, $Path)
        }
    } finally {
        if ([IO.File]::Exists($temporary)) {
            [IO.File]::Delete($temporary)
        }
    }
}
function Get-Sha256([string] $Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $algorithm = [Security.Cryptography.SHA256]::Create()
        try {
            $digest = $algorithm.ComputeHash($stream)
            return [BitConverter]::ToString($digest).Replace('-', '').ToLowerInvariant()
        } finally {
            $algorithm.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}
function Get-BoundedTimeout([int] $Requested) {
    $deadline = [Environment]::GetEnvironmentVariable('PIPELANG_NATIVE_STOPWATCH_DEADLINE')
    if (!$deadline) {
        return $Requested
    }
    $ticks = [long]::Parse($deadline, [Globalization.CultureInfo]::InvariantCulture)
    $remaining = [Math]::Floor(($ticks - [Diagnostics.Stopwatch]::GetTimestamp()) / [double] [Diagnostics.Stopwatch]::Frequency)
    if ($remaining -lt 1) {
        throw 'The user-selected native execution deadline has been reached'
    }
    return [int] [Math]::Min($Requested, $remaining)
}
function Invoke-Native([string] $Executable, [string[]] $ToolArguments) {
    if (!(Test-Path -LiteralPath $Executable -PathType Leaf)) {
        throw "Native executable is missing: $Executable"
    }
    $previousPreference = $ErrorActionPreference
    try {
        # Windows PowerShell represents redirected native stderr as error
        # records. The executable's exit status owns native command failure.
        $ErrorActionPreference = 'Continue'
        $global:LASTEXITCODE = -1
        & $Executable @ToolArguments
        $result = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($result -ne 0) {
        throw "Native tool failed ($result): $Executable"
    }
}
function New-ProofDirectory([string] $Path) {
    if (Test-Path -LiteralPath $Path) {
        throw "Use a fresh proof directory: $Path"
    }
    [void] [IO.Directory]::CreateDirectory($Path)
}
function Get-WindowsTools([string] $Directory) {
    $pinsPath = Join-Path $PSScriptRoot 'windows-toolchain.json'
    $pins = Get-Content -LiteralPath $pinsPath -Raw | ConvertFrom-Json
    if ($pins.schema -ne 1 -or $pins.target -ne 'windows/amd64') {
        throw 'Unrecognized native Windows toolchain pin'
    }
    [void] [IO.Directory]::CreateDirectory($Directory)
    $archives = Join-Path $Directory 'archives'
    [void] [IO.Directory]::CreateDirectory($archives)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $selected = @{}
    foreach ($tool in $pins.tools) {
        $archive = Join-Path $archives $tool.archive
        if (!(Test-Path -LiteralPath $archive)) {
            $stagedDirectory = [Environment]::GetEnvironmentVariable('PIPELANG_WINDOWS_ARCHIVE_DIRECTORY')
            if ($stagedDirectory) {
                Copy-Item -LiteralPath (Join-Path (Get-AbsolutePath $stagedDirectory) $tool.archive) -Destination $archive
            } else {
                Invoke-WebRequest -UseBasicParsing -Uri $tool.url -OutFile $archive -TimeoutSec 600
            }
        }
        if ((Get-Item -LiteralPath $archive).Length -ne $tool.size_bytes -or (Get-Sha256 $archive) -ne $tool.sha256) {
            throw "Pinned native archive differs: $($tool.name)"
        }
        $destination = Join-Path $Directory $tool.name
        if (Test-Path -LiteralPath $destination) {
            throw "Native tool extraction must be fresh: $destination"
        }
        $zip = [IO.Compression.ZipFile]::OpenRead($archive)
        try {
            $total = 0L
            if ($zip.Entries.Count -gt 100000) {
                throw 'Native tool archive entry capacity exceeded'
            }
            foreach ($entry in $zip.Entries) {
                $name = $entry.FullName
                if ($name.Contains('\') -or $name.Contains(':') -or $name.StartsWith('/') -or
                    ($name.Split('/') -contains '..') -or (($entry.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000) {
                    throw 'Native tool archive contains a nonportable or symbolic member'
                }
                $total += $entry.Length
                if ($total -ge 4GB) {
                    throw 'Native tool archive exceeds its extraction capacity'
                }
            }
        } finally {
            $zip.Dispose()
        }
        [IO.Compression.ZipFile]::ExtractToDirectory($archive, $destination)
        $executable = Join-Path $destination $tool.executable
        if (!(Test-Path -LiteralPath $executable -PathType Leaf)) {
            throw "Pinned native executable is missing: $executable"
        }
        $selected[$tool.name] = [ordered]@{
            executable = $executable
            executable_sha256 = Get-Sha256 $executable
            archive_sha256 = $tool.sha256
            root = $destination
        }
    }
    $compilerRoot = Join-Path $selected.llvm.root 'llvm-mingw-20260616-ucrt-x86_64'
    return [ordered]@{
        pin_sha256 = Get-Sha256 $pinsPath
        cmake = $selected.cmake.executable
        ninja = $selected.ninja.executable
        cxx = $selected.llvm.executable
        rc = Join-Path $compilerRoot 'bin/x86_64-w64-mingw32-windres.exe'
        runtime = Join-Path $compilerRoot 'x86_64-w64-mingw32/bin'
        identities = $selected
    }
}
function Set-WindowsTools($Tools) {
    $pinsPath = Join-Path $PSScriptRoot 'windows-toolchain.json'
    if ((Get-Sha256 $pinsPath) -ne $Tools.pin_sha256) {
        throw 'Windows native toolchain pins changed after build'
    }
    foreach ($name in @('cmake', 'ninja', 'llvm')) {
        $identity = $Tools.identities.$name
        if ((Get-Sha256 $identity.executable) -ne $identity.executable_sha256) {
            throw "Windows native tool identity changed: $name"
        }
    }
    $env:PIPELANG_CMAKE = $Tools.cmake
    $env:PIPELANG_CXX = $Tools.cxx
    # Nested CMake consumers use the same pinned toolchain even when they do
    # not supply generator/compiler cache arguments themselves.
    $env:CMAKE_GENERATOR = 'Ninja'
    $env:CXX = '"' + $Tools.cxx.Replace('\', '/') + '"'
    foreach ($name in @('CMAKE_GENERATOR_PLATFORM', 'CMAKE_GENERATOR_TOOLSET', 'CMAKE_GENERATOR_INSTANCE')) {
        Remove-Item -LiteralPath ('Env:' + $name) -ErrorAction SilentlyContinue
    }

    $env:RC = $Tools.rc.Replace('\', '/')
    $env:PATH = @([IO.Path]::GetDirectoryName($Tools.cmake), [IO.Path]::GetDirectoryName($Tools.ninja),
        (Join-Path $env:SystemRoot 'System32'), $PSHOME) -join ';'
    foreach ($name in @('go', 'python', 'python3', 'py')) {
        if (Get-Command $name -ErrorAction SilentlyContinue) {
            throw "Retired tool is reachable on native build PATH: $name"
        }
    }
}
function Get-WindowsBuildContext {
    $proof = [Environment]::GetEnvironmentVariable('PIPELANG_NATIVE_TOOLCHAIN_PROOF')
    if (!$proof) {
        $pointer = Join-Path $script:SourceRoot 'bin/native-current.json'
        $proof = (Get-Content -LiteralPath $pointer -Raw | ConvertFrom-Json).proof
    }
    $proof = Get-ExternalProof $proof
    $tools = Get-Content -LiteralPath (Join-Path $proof 'windows-tools.json') -Raw | ConvertFrom-Json
    Set-WindowsTools $tools
    $build = Join-Path $proof 'build'
    $runner = Join-Path $build 'containedexec/pipelang-contained.exe'
    $probe = Join-Path $build 'containedexec/pipelang-containment-probe.exe'
    $acceptance = Join-Path $proof 'installed-containment/acceptance.json'
    Invoke-Native $probe @('--admit', $runner, $acceptance, (Get-Sha256 $acceptance))
    return [ordered]@{ proof = $proof; build = $build; sdk = (Join-Path $proof 'sdk'); runner = $runner; tools = $tools }
}
function Invoke-Unit([string] $Name, [string] $Executable, [string[]] $ToolArguments, [int] $Timeout = 900) {
    $Timeout = Get-BoundedTimeout $Timeout
    $prefix = Join-Path $script:Proof "units/$Name"
    $arguments = @('unit', '--output', $prefix, '--estate', $script:Proof, '--timeout', "$Timeout", '--', $Executable) + $ToolArguments
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $global:LASTEXITCODE = -1
        & $script:Runner @arguments *> "$prefix.launcher.log"
        $result = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($result -ne 0) {
        Get-Content -LiteralPath "$prefix.output" -Tail 30 -ErrorAction SilentlyContinue
        Get-Content -LiteralPath "$prefix.launcher.log" -Tail 20 -ErrorAction SilentlyContinue
        throw "Native unit failed: $Name"
    }
    Write-Host ('{0}: passed' -f $Name)
}
function Invoke-WindowsJob([string] $Script, [string] $Proof, [string] $Runner, [int] $Timeout) {
    $Timeout = Get-BoundedTimeout $Timeout
    $arguments = @('job', '--output', (Join-Path $Proof 'job'), '--estate', $Proof, '--timeout', "$Timeout", '--',
        $script:PowerShell, '-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $Script, '-Inside')
    # ExecutionPolicy Bypass is process-local for repository scripts, not a
    # machine/user policy change and not a broker or credential bypass.
    Invoke-Native $Runner $arguments
}
function Initialize-Units([string] $Proof, [string] $Runner) {
    $script:Proof = $Proof
    $script:Runner = $Runner
    [void] [IO.Directory]::CreateDirectory((Join-Path $Proof 'units'))
    $env:DOCKPIPE_EVENT_LOG = Join-Path $Proof 'operation-events.jsonl'
}
function Initialize-CxxResourceProfile([string] $Sdk, $Tools) {
    $selected = [Environment]::GetEnvironmentVariable('PIPELANG_CXX_RESOURCE_PROFILE')
    if ($null -eq $selected) {
        $selected = 'system'
    }
    if ($selected -cne 'system' -and $selected -cne 'gnu-conservative-gc') {
        throw 'PIPELANG_CXX_RESOURCE_PROFILE must be system or gnu-conservative-gc'
    }
    $script:CxxResourceProfile = $selected
    $script:CxxResourceArguments = @()
    $response = $null
    $responseHash = $null
    $macroReceipt = $null
    $versionReceipt = $null
    if ($selected -ceq 'gnu-conservative-gc') {
        $response = Join-Path $Sdk 'share/pipelang-model/compiler-profiles/gnu-conservative-gc.rsp'
        if (!(Test-Path -LiteralPath $response -PathType Leaf)) {
            throw 'Selected installed SDK is missing the GNU compiler resource profile'
        }
        $responseInfo = Get-Item -LiteralPath $response
        if (($responseInfo.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw 'Selected installed SDK compiler resource profile cannot be a reparse entry'
        }
        $probeSource = Join-Path $script:Proof 'cxx-resource-profile-probe.cpp'
        if (Test-Path -LiteralPath $probeSource) {
            throw 'Compiler resource profile probe requires a fresh proof file'
        }
        [IO.File]::WriteAllText($probeSource, '', (New-Object Text.UTF8Encoding($false)))
        Invoke-Unit 'cxx-resource-profile-probe' $Tools.cxx @('-dM', '-E', '-x', 'c++', $probeSource)
        $macroReceipt = Join-Path $script:Proof 'units/cxx-resource-profile-probe.output'
        $macros = Get-Content -LiteralPath $macroReceipt -Raw
        $compatibility = '(?m)^#define (__clang__|__INTEL_COMPILER|__INTEL_LLVM_COMPILER|__NVCOMPILER|__PGI|__PGIC__|__ibmxl__|__IBMCPP__|__SUNPRO_CC|__CUDACC__|__COMO__|__TINYC__|__DMC__)( |$)'
        if ($macros -cnotmatch '(?m)^#define __GNUC__ [0-9]+' -or
            $macros -cnotmatch '(?m)^#define __GNUG__ [0-9]+' -or $macros -cmatch $compatibility) {
            throw 'gnu-conservative-gc requires the actual GNU C++ compiler, not a compatibility frontend'
        }
        Invoke-Unit 'cxx-resource-profile-version' $Tools.cxx @('--version')
        $versionReceipt = Join-Path $script:Proof 'units/cxx-resource-profile-version.output'
        $responseHash = Get-Sha256 $response
        $script:CxxResourceArguments = @("@$response")
    }
    $env:PIPELANG_CXX_RESOURCE_PROFILE = $selected
    Write-JsonFile (Join-Path $script:Proof 'cxx-resource-profile.json') ([ordered]@{
        profile = $selected; compiler = $Tools.cxx; response = $response
        response_sha256 = $responseHash; macro_receipt = $macroReceipt; version_receipt = $versionReceipt
    })
}
function Invoke-CxxUnit([string] $Name, $Tools, [string[]] $ToolArguments) {
    if (!(Get-Variable -Name CxxResourceArguments -Scope Script -ErrorAction SilentlyContinue)) {
        throw 'Initialize the compiler resource profile before compiling native fixtures'
    }
    Invoke-Unit $Name $Tools.cxx (@($script:CxxResourceArguments) + $ToolArguments)
}
function Compile-Fixture([string] $Name, [string] $Source, [string] $Generated, [string] $Sdk, $Tools, [string[]] $ExtraArguments = @()) {
    $runtime = Join-Path $script:Proof "$Name-runtime"
    [void] [IO.Directory]::CreateDirectory($runtime)
    $base = @('-std=c++20', '-pthread', '-Wall', '-Wextra', '-Werror', "-I$Sdk/include", "-I$Generated", $Source) + $ExtraArguments
    $normal = Join-Path $runtime 'test.exe'
    Invoke-CxxUnit "$Name-build" $Tools ($base + @('-O2', '-static', '-o', $normal))
    Invoke-Unit "$Name-run" $normal @()
    $sanitized = Join-Path $runtime 'test-sanitized.exe'
    Invoke-CxxUnit "$Name-sanitizer-build" $Tools ($base + @('-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-o', $sanitized))
    # Dynamic sanitizer/runtime DLLs stay beside this isolated test executable.
    # They are not copied into the installed SDK or placed on the producer PATH.
    foreach ($library in Get-ChildItem -LiteralPath $Tools.runtime -Filter '*.dll' -File) {
        Copy-Item -LiteralPath $library.FullName -Destination $runtime -Force
    }
    $env:ASAN_OPTIONS = 'detect_leaks=0:halt_on_error=1'
    $env:UBSAN_OPTIONS = 'halt_on_error=1:print_stacktrace=1'
    Invoke-Unit "$Name-sanitizer-run" $sanitized @()
}
Require-WindowsHost
