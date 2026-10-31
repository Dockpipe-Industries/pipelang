param([switch] $Inside)
. (Join-Path $PSScriptRoot '../../../build/assets/scripts/windows-common.ps1')
$context = Get-WindowsBuildContext
$proof = [Environment]::GetEnvironmentVariable('PIPELANG_NATIVE_COMPAT_PROOF')
if (!$proof) {
    $proof = Join-Path ([IO.Path]::GetTempPath()) ('pipelang-native-tests-' + [Guid]::NewGuid().ToString('N'))
}
$proof = Get-ExternalProof $proof
if (!$Inside) {
    New-ProofDirectory $proof
    $env:PIPELANG_NATIVE_TOOLCHAIN_PROOF = $context.proof
    $env:PIPELANG_NATIVE_COMPAT_PROOF = $proof
    Invoke-WindowsJob $PSCommandPath $proof $context.runner 10800
    exit 0
}
Initialize-Units $proof $context.runner
$sdk = $context.sdk
$build = $context.build
$tools = $context.tools
$manifest = Join-Path $sdk 'share/pipelang-model/sdk-manifest.json'
$expected = Get-Sha256 $manifest
$tool = Join-Path $sdk 'bin/pipelang-sdk-tool.exe'
Invoke-Unit 'sdk-verify-before' $tool @('verify', '--prefix', $sdk, '--manifest', $manifest, '--sha256', $expected)
$suitesText = [Environment]::GetEnvironmentVariable('PIPELANG_NATIVE_COMPAT_SUITES')
if (!$suitesText) {
    $suitesText = 'protocol fundamentals iteration async-composition failure-cleanup'
}
$suites = $suitesText.Split(' ', [StringSplitOptions]::RemoveEmptyEntries)
$allowed = @('protocol', 'fundamentals', 'iteration', 'async-composition', 'failure-cleanup', 'none')
$oracle = Join-Path $build 'sdk-tools/pipelang-model-compat-tests.exe'
foreach ($suite in $suites) {
    if ($allowed -notcontains $suite) {
        throw "Unknown native compatibility suite: $suite"
    }
    if ($suite -eq 'none') {
        continue
    }
    Invoke-Unit "$suite-generate" $oracle @('--suite', $suite, '--compiler', (Join-Path $sdk 'bin/pipelang-modelc.exe'),
        '--source-root', $script:SourceRoot, '--output', (Join-Path $proof $suite))
}
$fixtures = [ordered]@{
    protocol = @('semantics', 'semantics.cpp')
    fundamentals = @('fundamentals', 'fundamentals.cpp')
    iteration = @('iteration', 'iteration-tests.cpp')
    'async-composition' = @('async-composition', 'async-composition.cpp')
    'failure-cleanup' = @('failure-cleanup', 'failure-cleanup.cpp')
}
foreach ($suite in $suites) {
    if ($suite -eq 'none') {
        continue
    }
    $fixture = $fixtures[$suite]
    Compile-Fixture $fixture[0] (Join-Path $script:SourceRoot "tests/native-models/$($fixture[1])") (Join-Path $proof $suite) $sdk $tools
}
if ($suites -contains 'fundamentals') {
    Invoke-Unit 'numeric-guards' (Join-Path $build 'sdk-tools/pipelang-extended-tests.exe') @('--numeric-guard', $sdk,
        (Join-Path $proof 'fundamentals/NumericDefaults.hpp'))
}
$extended = Join-Path $proof 'extended'
Invoke-Unit 'extended-generate' (Join-Path $build 'sdk-tools/pipelang-extended-tests.exe') @($script:SourceRoot, $sdk, $extended)
foreach ($fixture in @('composition', 'parallel_source', 'collections_source', 'mixed', 'concurrent_source',
    'synchronization_source', 'objects_source', 'objects_composition', 'interfaces_source')) {
    Compile-Fixture $fixture (Join-Path $script:SourceRoot "tests/native-models/$fixture.cpp") $extended $sdk $tools
}
$classOracle = Join-Path $build 'model-compiler/pipelang-class-inheritance-compiler-tests.exe'
$classProof = Join-Path $proof 'class-inheritance'
Invoke-Unit 'class-inheritance-generate' $classOracle @($script:SourceRoot, $classProof)
foreach ($fixture in @('ClassInheritance', 'ClassModules')) {
    $actual = Join-Path $classProof "$fixture.actual.json"
    Invoke-Unit "$fixture-transport" $classOracle @('--transport', (Join-Path $sdk 'bin/pipelang-modelc.exe'),
        (Join-Path $classProof "$fixture.request.json"), $actual)
    Invoke-Unit "$fixture-verify" $classOracle @('--verify', (Join-Path $classProof "$fixture.expected.json"), $actual)
}
Compile-Fixture 'class-inheritance' (Join-Path $script:SourceRoot 'tests/native-models/class_inheritance_runtime.cpp') $classProof $sdk $tools
$runtimeSelection = [Environment]::GetEnvironmentVariable('PIPELANG_NATIVE_RUNTIME_SELECTION')
if (!$runtimeSelection) {
    $runtimeSelection = 'all'
}
if ($runtimeSelection -eq 'all') {
    foreach ($fixture in @('extended_collections', 'parallel_runtime', 'concurrent_runtime', 'synchronization_runtime', 'lazy_task_scope', 'object_runtime', 'object_transaction')) {
        Compile-Fixture $fixture (Join-Path $script:SourceRoot "tests/native-models/$fixture.cpp") (Join-Path $sdk 'include') $sdk $tools
    }
} elseif ($runtimeSelection -ne 'none') {
    throw "Unknown native runtime selection: $runtimeSelection"
}
Invoke-Unit 'sdk-verify-after' $tool @('verify', '--prefix', $sdk, '--manifest', $manifest, '--sha256', $expected)
Write-JsonFile (Join-Path $proof 'workflow-acceptance.json') ([ordered]@{
    status = 'passed'; platform = 'windows/amd64'; suites = $suites; runtime_selection = $runtimeSelection
    sanitizer = 'address,undefined'; leak_sanitizer = $false; sdk_manifest_sha256 = $expected
})
