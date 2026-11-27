param([switch] $Inside)
. (Join-Path $PSScriptRoot '../../../build/assets/scripts/windows-common.ps1')
$context = Get-WindowsBuildContext
$proof = [Environment]::GetEnvironmentVariable('PIPELANG_NATIVE_CHECK_PROOF')
if (!$proof) {
    $proof = Join-Path ([IO.Path]::GetTempPath()) ('pipelang-native-check-' + [Guid]::NewGuid().ToString('N'))
}
$proof = Get-ExternalProof $proof
if (!$Inside) {
    New-ProofDirectory $proof
    $env:PIPELANG_NATIVE_TOOLCHAIN_PROOF = $context.proof
    $env:PIPELANG_NATIVE_CHECK_PROOF = $proof
    if (!$env:DOCKPIPE_BIN) {
        $env:DOCKPIPE_BIN = (Get-Command dockpipe.exe -ErrorAction Stop).Source
    }
    Invoke-WindowsJob $PSCommandPath $proof $context.runner 1800
    exit 0
}
Initialize-Units $proof $context.runner
$dockpipe = Get-AbsolutePath (Get-RequiredEnvironment 'DOCKPIPE_BIN')
$parsed = 0
foreach ($script in Get-ChildItem -LiteralPath (Join-Path $script:SourceRoot 'workflows') -Filter '*.ps1' -File -Recurse) {
    $tokens = $null
    $failures = $null
    [void] [Management.Automation.Language.Parser]::ParseFile($script.FullName, [ref] $tokens, [ref] $failures)
    if ($failures.Count -ne 0) {
        throw "Native PowerShell syntax failed: $($script.FullName): $($failures | Out-String)"
    }
    $parsed++
}
Invoke-Unit 'repository-check' (Join-Path $context.build 'sdk-tools/pipelang-repository-check.exe') @('--source-root', $script:SourceRoot,
    '--dockpipe', $dockpipe, '--output', (Join-Path $proof 'contracts'))
Write-JsonFile (Join-Path $proof 'windows-syntax.json') ([ordered]@{
    status = 'passed'; powershell_scripts = $parsed; unix_shell_syntax = 'not run on Windows; requires Linux/macOS root check'
})
