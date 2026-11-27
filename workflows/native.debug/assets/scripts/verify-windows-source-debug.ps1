# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
param(
    [Parameter(Mandatory = $true)][string]$Debugger,
    [Parameter(Mandatory = $true)][string]$ProofDirectory
)

$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $ProofDirectory
$sourceDirectory = (Get-Location).Path.Replace('\', '/')
foreach ($inputName in @('collections-async.exe', 'collections-async.pdb', 'SuspendingCollections.pipe')) {
    if (-not (Test-Path -LiteralPath $inputName -PathType Leaf)) {
        throw "Missing PipeLang debug input: $inputName"
    }
}

@(
    "settings set target.source-map C:/pipelang-source `"$sourceDirectory`"",
    'target symbols add collections-async.pdb',
    'breakpoint set --file SuspendingCollections.pipe --line 12',
    'run 1 --require-suspension',
    'frame variable index acquired',
    'thread backtrace',
    'thread step-over',
    'breakpoint disable 1',
    'continue'
) | Set-Content -Encoding Ascii debug.lldb

$arguments = @(
    '--batch', '--no-lldbinit', '-l', 'none',
    '-O', '"settings set plugin.symbol-file.pdb.reader native"',
    '-s', 'debug.lldb', './collections-async.exe'
)
$process = Start-Process -FilePath $Debugger -ArgumentList $arguments -NoNewWindow -PassThru -Wait `
    -RedirectStandardOutput debugger.stdout -RedirectStandardError debugger.stderr
try {
    $log = Get-Content -Raw debugger.stdout
    $log
    Get-Content -Raw debugger.stderr
    if ($process.ExitCode -ne 0 -or $log -notmatch 'stop reason = breakpoint' -or
        $log -notmatch 'SuspendingCollections.App.Read[^\r\n]+SuspendingCollections\.pipe:12' -or
        $log -notmatch 'index = 1' -or $log -notmatch 'acquired = false' -or
        $log -notmatch 'SuspendingCollections\.pipe:13' -or $log -notmatch 'exited with status = 0') {
        throw 'Windows source breakpoint, locals, source step or exit was not verified.'
    }
} finally {
    $process.Dispose()
}
