param(
    [Parameter(Mandatory = $true)][string] $Executable,
    [Parameter(Mandatory = $true)][string[]] $Arguments,
    [Parameter(Mandatory = $true)][string] $Proof,
    [Parameter(Mandatory = $true)][string] $Estate,
    [int] $TimeoutSeconds = 900
)
$deadline = [Environment]::GetEnvironmentVariable('PIPELANG_NATIVE_STOPWATCH_DEADLINE')
if ($deadline) {
    $remaining = [Math]::Floor(([long]::Parse($deadline, [Globalization.CultureInfo]::InvariantCulture) -
        [Diagnostics.Stopwatch]::GetTimestamp()) / [double] [Diagnostics.Stopwatch]::Frequency)
    if ($remaining -lt 1) {
        throw 'The user-selected bootstrap deadline has been reached'
    }
    $TimeoutSeconds = [int] [Math]::Min($TimeoutSeconds, $remaining)
}
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Emit only native ABI declarations. This uses the PowerShell runtime, and
# requires no C# compiler, Go, Python, shell compatibility layer or SDK install.
$assemblyName = New-Object Reflection.AssemblyName('PipeLang.Bootstrap.Native')
$assembly = [AppDomain]::CurrentDomain.DefineDynamicAssembly($assemblyName, [Reflection.Emit.AssemblyBuilderAccess]::Run)
$module = $assembly.DefineDynamicModule('Native')
$builder = $module.DefineType('PipeLang.Bootstrap.Kernel', [Reflection.TypeAttributes]'Public, Sealed, Abstract')
function Add-NativeMethod([string] $Name, [type] $Return, [type[]] $Parameters) {
    $method = $builder.DefinePInvokeMethod($Name, 'kernel32.dll',
        [Reflection.MethodAttributes]'Public, Static, PinvokeImpl', [Reflection.CallingConventions]::Standard,
        $Return, $Parameters, [Runtime.InteropServices.CallingConvention]::Winapi, [Runtime.InteropServices.CharSet]::Unicode)
    $method.SetImplementationFlags($method.GetMethodImplementationFlags() -bor [Reflection.MethodImplAttributes]::PreserveSig)
}
$pointer = [IntPtr]
$pointerRef = [IntPtr].MakeByRefType()
Add-NativeMethod CreateJobObjectW $pointer @($pointer, [string])
Add-NativeMethod SetInformationJobObject ([bool]) @($pointer, [int], $pointer, [uint32])
Add-NativeMethod QueryInformationJobObject ([bool]) @($pointer, [int], $pointer, [uint32], $pointer)
Add-NativeMethod CreateProcessW ([bool]) @([string], [Text.StringBuilder], $pointer, $pointer, [bool], [uint32], $pointer, [string], $pointer, $pointer)
Add-NativeMethod AssignProcessToJobObject ([bool]) @($pointer, $pointer)
Add-NativeMethod ResumeThread ([uint32]) @($pointer)
Add-NativeMethod TerminateJobObject ([bool]) @($pointer, [uint32])
Add-NativeMethod TerminateProcess ([bool]) @($pointer, [uint32])
Add-NativeMethod WaitForSingleObject ([uint32]) @($pointer, [uint32])
Add-NativeMethod GetExitCodeProcess ([bool]) @($pointer, [uint32].MakeByRefType())
Add-NativeMethod CloseHandle ([bool]) @($pointer)
Add-NativeMethod GetLastError ([uint32]) @()
Add-NativeMethod CreateFileW $pointer @([string], [uint32], [uint32], $pointer, [uint32], [uint32], $pointer)
Add-NativeMethod InitializeProcThreadAttributeList ([bool]) @($pointer, [int], [uint32], $pointerRef)
Add-NativeMethod UpdateProcThreadAttribute ([bool]) @($pointer, [uint32], $pointer, $pointer, $pointer, $pointer, $pointer)
Add-NativeMethod DeleteProcThreadAttributeList ([void]) @($pointer)
$kernel = $builder.CreateType()
$native = [Runtime.InteropServices.Marshal]
function Assert-Native([bool] $Condition, [string] $Message) {
    if (!$Condition) {
        throw "$Message (Win32 $($kernel::GetLastError()))"
    }
}
function Allocate-Zero([int] $Size) {
    $memory = $native::AllocHGlobal($Size)
    for ($offset = 0; $offset -lt $Size; $offset++) {
        $native::WriteByte($memory, $offset, 0)
    }
    return $memory
}
function Quote-Argument([string] $Value) {
    if ($Value.IndexOf([char]0) -ge 0) {
        throw 'Windows argument contains NUL'
    }
    $result = New-Object Text.StringBuilder
    [void] $result.Append('"')
    $slashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') {
            $slashes++
            continue
        }
        $count = $slashes
        if ($character -eq '"') {
            $count = $slashes * 2 + 1
        }
        [void] $result.Append('\', $count)
        [void] $result.Append($character)
        $slashes = 0
    }
    [void] $result.Append('\', $slashes * 2)
    [void] $result.Append('"')
    return $result.ToString()
}
if ([IntPtr]::Size -ne 8) {
    throw 'Windows bootstrap requires native x64 PowerShell'
}
if (![IO.Path]::IsPathRooted($Executable) -or !(Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw 'Bootstrap executable must be an existing absolute native tool'
}
if (Test-Path -LiteralPath "$Proof.json") {
    throw 'Bootstrap proof must be fresh'
}
[void] [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Proof))
$job = [IntPtr]::Zero
$process = [IntPtr]::Zero
$thread = [IntPtr]::Zero
$attributes = [IntPtr]::Zero
$attributeInitialized = $false
$buffers = New-Object 'Collections.Generic.List[IntPtr]'
$files = New-Object 'Collections.Generic.List[IntPtr]'
$report = [ordered]@{
    protocol = 'pipelang.bootstrap-execution.v1'; policy = 'windows.job-commit.v1'
    executable = $Executable; arguments = $Arguments
    commit_limit_bytes = 1073741824; memory_stop_bytes = 838860800
    process_limit_count = 128; sampled_thread_stop_count = 128
    swap_enforced = $false; outcome = 'starting'; exit = $null; descendants_removed = $false
}
$clock = [Diagnostics.Stopwatch]::StartNew()
try {
    $job = $kernel::CreateJobObjectW([IntPtr]::Zero, $null)
    Assert-Native ($job -ne [IntPtr]::Zero) 'Cannot create bootstrap job'
    $limits = Allocate-Zero 144
    $buffers.Add($limits)
    $native::WriteInt32($limits, 16, 0x2208)
    $native::WriteInt32($limits, 40, 128)
    $native::WriteInt64($limits, 120, 1073741824)
    Assert-Native ($kernel::SetInformationJobObject($job, 9, $limits, 144)) 'Cannot set bootstrap limits'
    $readback = Allocate-Zero 144
    $buffers.Add($readback)
    Assert-Native ($kernel::QueryInformationJobObject($job, 9, $readback, 144, [IntPtr]::Zero)) 'Cannot read bootstrap limits'
    if ($native::ReadInt32($readback, 16) -ne 0x2208 -or $native::ReadInt32($readback, 40) -ne 128 -or
        $native::ReadInt64($readback, 120) -ne 1073741824) { throw 'Bootstrap kernel limit readback differs' }
    $security = Allocate-Zero 24
    $buffers.Add($security)
    $native::WriteInt32($security, 0, 24)
    $native::WriteInt32($security, 16, 1)
    foreach ($path in @('NUL', "$Proof.stdout", "$Proof.stderr")) {
        $inputFile = $path -eq 'NUL'
        $access = [uint32] 1073741824
        $disposition = 1
        if ($inputFile) {
            $access = [uint32] 2147483648
            $disposition = 3
        }
        $file = $kernel::CreateFileW($path, $access, 3, $security, $disposition, 0x80, [IntPtr]::Zero)
        Assert-Native ($file -ne [IntPtr](-1)) 'Cannot create bootstrap protocol handle'
        $files.Add($file)
    }
    $capacity = [IntPtr]::Zero
    [void] $kernel::InitializeProcThreadAttributeList([IntPtr]::Zero, 1, 0, [ref] $capacity)
    $attributes = Allocate-Zero $capacity.ToInt32()
    $buffers.Add($attributes)
    Assert-Native ($kernel::InitializeProcThreadAttributeList($attributes, 1, 0, [ref] $capacity)) 'Cannot initialize bootstrap handle whitelist'
    $attributeInitialized = $true
    $handles = Allocate-Zero 24
    $buffers.Add($handles)
    for ($index = 0; $index -lt 3; $index++) {
        $native::WriteIntPtr($handles, $index * 8, $files[$index])
    }
    Assert-Native ($kernel::UpdateProcThreadAttribute($attributes, 0, [IntPtr] 0x20002, $handles, [IntPtr] 24,
        [IntPtr]::Zero, [IntPtr]::Zero)) 'Cannot whitelist bootstrap protocol handles'
    $startup = Allocate-Zero 112
    $buffers.Add($startup)
    $native::WriteInt32($startup, 0, 112)
    $native::WriteInt32($startup, 60, 0x100)
    for ($index = 0; $index -lt 3; $index++) {
        $native::WriteIntPtr($startup, 80 + $index * 8, $files[$index])
    }
    $native::WriteIntPtr($startup, 104, $attributes)
    $information = Allocate-Zero 24
    $buffers.Add($information)
    $command = New-Object Text.StringBuilder
    [void] $command.Append(((@($Executable) + $Arguments | ForEach-Object { Quote-Argument $_ }) -join ' '))
    Assert-Native ($kernel::CreateProcessW($Executable, $command, [IntPtr]::Zero, [IntPtr]::Zero, $true,
        0x08080004, [IntPtr]::Zero, (Get-Location).Path, $startup, $information)) 'Cannot create suspended bootstrap tool'
    $process = $native::ReadIntPtr($information, 0)
    $thread = $native::ReadIntPtr($information, 8)
    Assert-Native ($kernel::AssignProcessToJobObject($job, $process)) 'Cannot admit suspended bootstrap tool'
    Assert-Native ($kernel::ResumeThread($thread) -ne [uint32]::MaxValue) 'Cannot resume admitted bootstrap tool'
    $members = Allocate-Zero (8 + 129 * 8)
    $buffers.Add($members)
    $peak = 0L
    while ($kernel::WaitForSingleObject($process, 20) -eq 258) {
        if ($clock.Elapsed.TotalSeconds -ge $TimeoutSeconds) {
            throw 'Bootstrap deadline exceeded'
        }
        Assert-Native ($kernel::QueryInformationJobObject($job, 9, $readback, 144, [IntPtr]::Zero)) 'Cannot inspect bootstrap commit'
        $peak = [Math]::Max($peak, $native::ReadInt64($readback, 136))
        if ($peak -ge 838860800) {
            throw 'Bootstrap proactive memory stop'
        }
        Assert-Native ($kernel::QueryInformationJobObject($job, 3, $members, (8 + 129 * 8), [IntPtr]::Zero)) 'Cannot inspect bootstrap members'
        $count = $native::ReadInt32($members, 4)
        if ($count -gt 128 -or $count -ne $native::ReadInt32($members, 0)) {
            throw 'Bootstrap member inventory exceeds capacity'
        }
        $threads = 0
        for ($index = 0; $index -lt $count; $index++) {
            $identifier = $native::ReadInt64($members, 8 + 8 * $index)
            $workload = Get-Process -Id $identifier -ErrorAction SilentlyContinue
            if ($workload) {
                try {
                    if (!$workload.HasExited) {
                        $threads += @($workload.Threads).Count
                    }
                } catch {
                    $workload.Refresh()
                    if (!$workload.HasExited) {
                        throw
                    }
                } finally {
                    $workload.Dispose()
                }
            }
        }
        if ($threads -gt 128) {
            throw 'Bootstrap proactive thread stop'
        }
        $estateBytes = (Get-ChildItem -LiteralPath $Estate -File -Recurse -Force -ErrorAction Stop | Measure-Object -Property Length -Sum).Sum
        if ($estateBytes -ge 8GB) {
            throw 'Bootstrap estate logical-byte stop'
        }
        if ((Get-Item -LiteralPath "$Proof.stdout").Length -ge 64MB -or (Get-Item -LiteralPath "$Proof.stderr").Length -ge 1MB) {
            throw 'Bootstrap protocol capacity exceeded'
        }
    }
    Assert-Native ($kernel::QueryInformationJobObject($job, 9, $readback, 144, [IntPtr]::Zero)) 'Cannot inspect final bootstrap commit'
    $peak = [Math]::Max($peak, $native::ReadInt64($readback, 136))
    if ($peak -ge 838860800) {
        throw 'Bootstrap proactive memory stop'
    }
    $exitCode = [uint32] 0
    Assert-Native ($kernel::GetExitCodeProcess($process, [ref] $exitCode)) 'Cannot inspect bootstrap exit'
    $report.exit = $exitCode
    $report.outcome = 'failed'
    if ($exitCode -eq 0) {
        $report.outcome = 'completed'
    }
    $report.peak_commit_bytes = $peak
} catch {
    $report.outcome = 'failed'
    $report.error = $_.Exception.Message
} finally {
    if ($job -ne [IntPtr]::Zero) {
        [void] $kernel::TerminateJobObject($job, 1)
    }
    if ($process -ne [IntPtr]::Zero) {
        [void] $kernel::TerminateProcess($process, 1)
        [void] $kernel::WaitForSingleObject($process, 5000)
    }
    if ($job -ne [IntPtr]::Zero) {
        $accounting = Allocate-Zero 48
        $buffers.Add($accounting)
        $cleanup = [Diagnostics.Stopwatch]::StartNew()
        do {
            $queried = $kernel::QueryInformationJobObject($job, 1, $accounting, 48, [IntPtr]::Zero)
            if ($queried -and $native::ReadInt32($accounting, 40) -eq 0) {
                $report.descendants_removed = $true
                break
            }
            Start-Sleep -Milliseconds 20
        } while ($cleanup.Elapsed.TotalSeconds -lt 5)
    }
    if ($attributeInitialized) {
        $kernel::DeleteProcThreadAttributeList($attributes)
    }
    foreach ($file in $files) {
        [void] $kernel::CloseHandle($file)
    }
    foreach ($handle in @($thread, $process, $job)) {
        if ($handle -ne [IntPtr]::Zero) {
            [void] $kernel::CloseHandle($handle)
        }
    }
    foreach ($buffer in $buffers) {
        $native::FreeHGlobal($buffer)
    }
    $report.elapsed_s = $clock.Elapsed.TotalSeconds
    $report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath "$Proof.json" -Encoding UTF8
}
if ($report.outcome -ne 'completed' -or !$report.descendants_removed) {
    Get-Content -LiteralPath "$Proof.stderr" -Tail 30 -ErrorAction SilentlyContinue
    throw "Windows source bootstrap failed: $($report | ConvertTo-Json -Compress -Depth 12)"
}
