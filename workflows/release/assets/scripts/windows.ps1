param([switch] $Inside)
. (Join-Path $PSScriptRoot '../../../build/assets/scripts/windows-common.ps1')
$context = Get-WindowsBuildContext
$proof = [Environment]::GetEnvironmentVariable('PIPELANG_NATIVE_PACKAGE_PROOF')
if (!$proof) {
    $proof = Join-Path ([IO.Path]::GetTempPath()) ('pipelang-native-package-' + [Guid]::NewGuid().ToString('N'))
}
$proof = Get-ExternalProof $proof
if (!$Inside) {
    New-ProofDirectory $proof
    $env:PIPELANG_NATIVE_TOOLCHAIN_PROOF = $context.proof
    $env:PIPELANG_NATIVE_PACKAGE_PROOF = $proof
    Invoke-WindowsJob $PSCommandPath $proof $context.runner 7200
    exit 0
}
Initialize-Units $proof $context.runner
$sdk = $context.sdk
$build = $context.build
$tools = $context.tools
$tool = Join-Path $sdk 'bin/pipelang-sdk-tool.exe'
$manifest = Join-Path $sdk 'share/pipelang-model/sdk-manifest.json'
$expected = Get-Sha256 $manifest
$package = Join-Path $proof 'package'
Invoke-Unit 'archive' $tool @('sdk-package', '--prefix', $sdk, '--manifest', $manifest, '--sha256', $expected, '--output', $package)
Invoke-Unit 'independent-archive-reader' $tools.cmake @('-E', 'tar', 'tf', (Join-Path $package 'pipelang-native-sdk.tar'))
$packageManifest = Join-Path $package 'sdk-package.json'
$relocated = Join-Path $proof 'relocated-sdk'
Invoke-Unit 'extract' $tool @('sdk-extract', '--manifest', $packageManifest, '--sha256', (Get-Sha256 $packageManifest), '--output', $relocated)
Invoke-Unit 'relocated-sdk-consumer' (Join-Path $build 'sdk-tools/pipelang-sdk-tool-tests.exe') @($script:SourceRoot, $relocated, (Join-Path $proof 'consumer'))
$legacy = Join-Path $relocated 'bin/pipelang-native.exe'
Invoke-Unit 'relocated-legacy-cli' (Join-Path $build 'legacy-compiler/pipelang-legacy-compiler-tests.exe') @($script:SourceRoot, (Join-Path $proof 'legacy-cli'), $legacy)
Invoke-Unit 'relocated-materializer' (Join-Path $build 'legacy-compiler/pipelang-materialize-tests.exe') @((Join-Path $proof 'materializer'), $legacy)
Invoke-Unit 'relocated-diagnostics' (Join-Path $build 'legacy-compiler/pipelang-diagnostic-tests.exe') @((Join-Path $proof 'diagnostics'), $legacy)
Invoke-Unit 'relocated-operations' (Join-Path $build 'legacy-compiler/pipelang-operation-tests.exe') @((Join-Path $proof 'operations'), $legacy)
$scalars = Join-Path $proof 'scalars'
Invoke-Unit 'relocated-scalars' (Join-Path $build 'scalar-compiler/pipelang-scalar-tests.exe') @($script:SourceRoot, $relocated, $scalars)
$scalarTest = Join-Path $scalars 'generated/test.exe'
Invoke-Unit 'relocated-scalar-build' $tools.cxx @('-std=c++17', '-Wall', '-Wextra', '-Werror', '-O2', '-static',
    (Join-Path $scalars 'generated/runtime.cpp'), '-o', $scalarTest)
Invoke-Unit 'relocated-scalar-run' $scalarTest @()
Invoke-Unit 'relocated-extended' (Join-Path $build 'sdk-tools/pipelang-extended-tests.exe') @($script:SourceRoot, $relocated, (Join-Path $proof 'extended'))
Invoke-Unit 'relocated-sdk-verify' (Join-Path $relocated 'bin/pipelang-sdk-tool.exe') @('verify', '--prefix', $relocated,
    '--manifest', (Join-Path $relocated 'share/pipelang-model/sdk-manifest.json'), '--sha256', $expected)
Write-JsonFile (Join-Path $proof 'workflow-acceptance.json') ([ordered]@{
    status = 'passed'; platform = 'windows/amd64'; sdk_manifest_sha256 = $expected; relocated_sdk = $relocated
})
