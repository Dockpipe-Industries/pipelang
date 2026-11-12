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
$selectionText = [Environment]::GetEnvironmentVariable('PIPELANG_NATIVE_FIXTURE_SELECTION')
if ($null -eq $selectionText) {
    $selectionText = 'all'
}
$fixtureSelection = @($selectionText.Split([char[]]" `t`r`n", [StringSplitOptions]::RemoveEmptyEntries))
$knownFixtures = 'semantics synchronous-host initialization-budget list-reads fundamentals uint32 uint32-integer-only iteration async-composition failure-cleanup composition parallel_source collections_source mixed concurrent_source synchronization_source objects_source objects_composition interfaces_source synchronous_host_objects initialization_budget_objects list_read_objects uint32_objects foundation foundation_objects byte_buffers byte_buffer_objects result_interface result_propagation_worker result_propagation typed_host_errors byte_buffer_iteration native-numeric project compiled-project class-inheritance extended_collections parallel_runtime concurrent_runtime synchronization_runtime lazy_task_scope synchronous_host_scope initialization_budget_runtime object_runtime object_transaction'.Split(' ')
if ($fixtureSelection.Count -eq 0) {
    throw 'Native fixture selection must not be empty'
}
foreach ($selectedFixture in $fixtureSelection) {
    if ($selectedFixture -ceq 'all') {
        if ($fixtureSelection.Count -ne 1) {
            throw 'Native fixture selection all must be used alone'
        }
    } elseif ($knownFixtures -cnotcontains $selectedFixture) {
        throw "Unknown native fixture selection: $selectedFixture"
    }
}
$script:executedFixtures = @()
function Compile-SelectedFixture {
    param($Name, $Source, $Generated, $Sdk, $Tools)
    if ($fixtureSelection -cnotcontains 'all' -and $fixtureSelection -cnotcontains $Name) {
        return
    }
    $script:executedFixtures += $Name
    Compile-Fixture $Name $Source $Generated $Sdk $Tools
}
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
    Compile-SelectedFixture $fixture[0] (Join-Path $script:SourceRoot "tests/native-models/$($fixture[1])") (Join-Path $proof $suite) $sdk $tools
    if ($suite -eq 'fundamentals') {
        $generated = Join-Path $proof 'fundamentals'
        Compile-SelectedFixture 'uint32' (Join-Path $script:SourceRoot 'tests/native-models/uint32.cpp') $generated $sdk $tools
        Compile-SelectedFixture 'uint32-integer-only' (Join-Path $script:SourceRoot 'tests/native-models/uint32_integer_only.cpp') $generated $sdk $tools
        $fast = Join-Path $proof 'uint32-integer-fast.exe'
        Invoke-Unit 'uint32-integer-fast-build' $tools.cxx @('-std=c++20', '-pthread', '-Wall', '-Wextra', '-Werror',
            '-O2', '-static', '-ffast-math', "-I$sdk/include", "-I$generated",
            (Join-Path $script:SourceRoot 'tests/native-models/uint32_integer_only.cpp'), '-o', $fast)
        Invoke-Unit 'uint32-integer-fast-run' $fast @()
    }
    if ($suite -eq 'protocol') {
        Compile-SelectedFixture 'synchronous-host' (Join-Path $script:SourceRoot 'tests/native-models/synchronous_host.cpp') (Join-Path $proof $suite) $sdk $tools
        Compile-SelectedFixture 'initialization-budget' (Join-Path $script:SourceRoot 'tests/native-models/initialization_budget.cpp') (Join-Path $proof $suite) $sdk $tools
        Compile-SelectedFixture 'list-reads' (Join-Path $script:SourceRoot 'tests/native-models/list_reads.cpp') (Join-Path $proof $suite) $sdk $tools
    }
}
if ($suites -contains 'fundamentals') {
    Invoke-Unit 'numeric-guards' (Join-Path $build 'sdk-tools/pipelang-extended-tests.exe') @('--numeric-guard', $sdk,
        (Join-Path $proof 'fundamentals/NumericDefaults.hpp'))
}
$extended = Join-Path $proof 'extended'
Invoke-Unit 'extended-generate' (Join-Path $build 'sdk-tools/pipelang-extended-tests.exe') @($script:SourceRoot, $sdk, $extended)
foreach ($fixture in @('composition', 'parallel_source', 'collections_source', 'mixed', 'concurrent_source',
    'synchronization_source', 'objects_source', 'objects_composition', 'interfaces_source', 'synchronous_host_objects', 'initialization_budget_objects', 'list_read_objects', 'uint32_objects', 'foundation', 'foundation_objects', 'byte_buffers', 'byte_buffer_objects', 'result_interface', 'result_propagation_worker', 'result_propagation', 'typed_host_errors', 'byte_buffer_iteration')) {
    Compile-SelectedFixture $fixture (Join-Path $script:SourceRoot "tests/native-models/$fixture.cpp") $extended $sdk $tools
}
$numericProof = Join-Path $proof 'native-numeric'
Invoke-Unit 'native-numeric-generate' (Join-Path $build 'sdk-tools/pipelang-sdk-tool-tests.exe') @('--native-numeric-fixture', $sdk, $numericProof)
Compile-SelectedFixture 'native-numeric' (Join-Path $numericProof 'main.cpp') (Join-Path $numericProof 'generated') $sdk $tools
if ($fixtureSelection -contains 'all' -or $fixtureSelection -contains 'native-numeric') {
    $observationProject = Join-Path $numericProof 'observation'
    foreach ($observationMode in @('normal', 'sanitizer')) {
        $observationSanitizers = 'OFF'
        if ($observationMode -eq 'sanitizer') {
            $observationSanitizers = 'ON'
        }
        $observationBuild = Join-Path $observationProject $observationMode
        Invoke-Unit "ordinary-observation-$observationMode-configure" $tools.cmake @('-S', $observationProject,
            '-B', $observationBuild, '-G', 'Ninja', "-DCMAKE_PREFIX_PATH=$sdk",
            "-DCMAKE_CXX_COMPILER=$($tools.cxx)", '-DCMAKE_BUILD_TYPE=Release',
            "-DOBSERVATION_SANITIZERS=$observationSanitizers")
        Invoke-Unit "ordinary-observation-$observationMode-build" $tools.cmake @('--build', $observationBuild, '--parallel', '1')
        if ($observationMode -eq 'sanitizer') {
            foreach ($library in Get-ChildItem -LiteralPath $tools.runtime -Filter '*.dll' -File) {
                Copy-Item -LiteralPath $library.FullName -Destination $observationBuild -Force
            }
            $env:ASAN_OPTIONS = 'detect_leaks=0:halt_on_error=1'
            $env:UBSAN_OPTIONS = 'halt_on_error=1:print_stacktrace=1'
        }
        Invoke-Unit "ordinary-observation-$observationMode-run" (Join-Path $observationBuild 'ordinary_observation.exe') @()
    }
}
$projectProof = Join-Path $proof 'project'
Invoke-Unit 'project-generate' (Join-Path $build 'sdk-tools/pipelang-sdk-tool-tests.exe') @('--project-fixture', $sdk, $projectProof)
Compile-SelectedFixture 'project' (Join-Path $projectProof 'main.cpp') (Join-Path $projectProof 'generated') $sdk $tools
if ($fixtureSelection -contains 'all' -or $fixtureSelection -contains 'compiled-project') {
    $script:executedFixtures += 'compiled-project'
    $compiledProject = Join-Path $proof 'compiled-project'
    Invoke-Unit 'compiled-project-contracts' (Join-Path $build 'sdk-tools/pipelang-sdk-tool-tests.exe') @(
        '--compiled-project-contracts', $sdk, $compiledProject)
    foreach ($ownerMode in @('normal', 'sanitizer')) {
        $ownerFixture = Join-Path $proof "compiled-owner-$ownerMode"
        Invoke-Unit "compiled-owner-$ownerMode-prepare" (Join-Path $build 'sdk-tools/pipelang-sdk-tool-tests.exe') @(
            '--compiled-owner-prepare', $sdk, $ownerFixture, $ownerMode)
        $ownerConsumer = Join-Path $ownerFixture 'consumer-build'
        foreach ($library in Get-ChildItem -LiteralPath $tools.runtime -Filter '*.dll' -File) {
            Copy-Item -LiteralPath $library.FullName -Destination $ownerConsumer -Force
        }
        $env:ASAN_OPTIONS = 'detect_leaks=0:halt_on_error=1'
        $env:UBSAN_OPTIONS = 'halt_on_error=1:print_stacktrace=1'
        Invoke-Unit "compiled-owner-$ownerMode-run" (Join-Path $ownerConsumer 'owner_consumer.exe') @()
    }
    $compiledSanitizer = Join-Path $compiledProject 'sanitizer'
    Invoke-Unit 'compiled-project-sanitizer-configure' $tools.cmake @('-S', $compiledProject,
        '-B', $compiledSanitizer, '-G', 'Ninja', "-DCMAKE_PREFIX_PATH=$sdk", "-DCMAKE_CXX_COMPILER=$($tools.cxx)",
        '-DCMAKE_BUILD_TYPE=Release', '-DPIPELANG_COMPILED_FIXTURE_SANITIZERS=ON')
    Invoke-Unit 'compiled-project-sanitizer-policy' (Join-Path $build 'sdk-tools/pipelang-sdk-tool-tests.exe') @(
        '--compiled-project-policy', $compiledSanitizer, 'sanitizer')
    Invoke-Unit 'compiled-project-sanitizer-build' $tools.cmake @('--build', $compiledSanitizer, '--parallel', '1')
    foreach ($library in Get-ChildItem -LiteralPath $tools.runtime -Filter '*.dll' -File) {
        Copy-Item -LiteralPath $library.FullName -Destination $compiledSanitizer -Force
    }
    $env:ASAN_OPTIONS = 'detect_leaks=0:halt_on_error=1'
    $env:UBSAN_OPTIONS = 'halt_on_error=1:print_stacktrace=1'
    foreach ($executable in @('consumer', 'types_consumer', 'compiled_peer')) {
        Invoke-Unit "compiled-project-$executable-sanitizer-run" (Join-Path $compiledSanitizer "$executable.exe") @()
    }
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
Compile-SelectedFixture 'class-inheritance' (Join-Path $script:SourceRoot 'tests/native-models/class_inheritance_runtime.cpp') $classProof $sdk $tools
$runtimeSelection = [Environment]::GetEnvironmentVariable('PIPELANG_NATIVE_RUNTIME_SELECTION')
if (!$runtimeSelection) {
    $runtimeSelection = 'all'
}
if ($runtimeSelection -eq 'all') {
    foreach ($fixture in @('extended_collections', 'parallel_runtime', 'concurrent_runtime', 'synchronization_runtime', 'lazy_task_scope', 'synchronous_host_scope', 'initialization_budget_runtime', 'object_runtime', 'object_transaction')) {
        Compile-SelectedFixture $fixture (Join-Path $script:SourceRoot "tests/native-models/$fixture.cpp") (Join-Path $sdk 'include') $sdk $tools
    }
} elseif ($runtimeSelection -ne 'none') {
    throw "Unknown native runtime selection: $runtimeSelection"
}
foreach ($selectedFixture in $fixtureSelection) {
    if ($selectedFixture -cne 'all' -and $script:executedFixtures -cnotcontains $selectedFixture) {
        throw "Selected fixture unavailable in selected suites: $selectedFixture"
    }
}
Invoke-Unit 'sdk-verify-after' $tool @('verify', '--prefix', $sdk, '--manifest', $manifest, '--sha256', $expected)
Write-JsonFile (Join-Path $proof 'workflow-acceptance.json') ([ordered]@{
    status = 'passed'; platform = 'windows/amd64'; suites = $suites; runtime_selection = $runtimeSelection
    fixture_selection = $fixtureSelection
    sanitizer = 'address,undefined'; leak_sanitizer = $false; sdk_manifest_sha256 = $expected
})
