param([switch] $Inside)
$runtimeLinkage = [Environment]::GetEnvironmentVariable('PIPELANG_NATIVE_RUNTIME_LINKAGE')
if ($null -eq $runtimeLinkage) {
    $runtimeLinkage = 'header-only'
}
if ($runtimeLinkage -ceq 'linked') {
    throw 'Linked native workflow runtime is supported only on Linux'
}
if ($runtimeLinkage -cne 'header-only') {
    throw 'PIPELANG_NATIVE_RUNTIME_LINKAGE must be header-only or linked'
}
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
$knownFixtures = 'semantics synchronous-host initialization-budget list-reads fundamentals uint32 uint32-integer-only iteration async-composition failure-cleanup composition parallel_source collections_source mixed concurrent_source synchronization_source objects_source objects_composition interfaces_source synchronous_host_objects initialization_budget_objects list_read_objects uint32_objects foundation foundation_objects byte_buffers byte_buffer_objects result_interface result_propagation_worker result_propagation typed_host_errors byte_buffer_iteration generic_member_calls static_methods_objects static_methods_project static_methods_compiled var_inference_objects var_inference_project var_inference_compiled bounded_calls_objects bounded_calls_project bounded_calls_compiled bounded_calls_runtime execution_segments_objects execution_segments_project execution_segments_compiled execution_segments_di_project execution_segments_di_compiled execution_segments_runtime member_bindings_objects member_bindings_project member_bindings_compiled async_finally_source async_finally_project async_finally_compiled async_finally_probe async_finally_runtime async_finally_storage_runtime loop_flow_objects loop_flow_project loop_flow_compiled testing_library independent_tokens_source independent_tokens_project independent_tokens_compiled independent_tokens_di_project independent_tokens_di_compiled empty_interfaces_objects empty_interfaces_project empty_interfaces_compiled empty_interfaces_project_types empty_interfaces_compiled_types testing_async_library json_library native-numeric project compiled-project class-inheritance extended_collections parallel_runtime concurrent_runtime synchronization_runtime lazy_task_scope synchronous_host_scope initialization_budget_runtime object_runtime object_transaction'.Split(' ')
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
    param($Name, $Source, $Generated, $Sdk, $Tools, [string[]] $ExtraArguments = @())
    if ($fixtureSelection -cnotcontains 'all' -and $fixtureSelection -cnotcontains $Name) {
        return
    }
    $script:executedFixtures += $Name
    Compile-Fixture $Name $Source $Generated $Sdk $Tools $ExtraArguments
}
Invoke-Unit 'sdk-verify-before' $tool @('verify', '--prefix', $sdk, '--manifest', $manifest, '--sha256', $expected)
Initialize-CxxResourceProfile $sdk $tools
Invoke-Unit 'cxx-resource-profile-contracts' $tools.cmake @(
    "-DSDK_PREFIX=$sdk", "-DSOURCE_ROOT=$script:SourceRoot",
    "-DOUTPUT_ROOT=$(Join-Path $proof 'compile-resource-profile-contracts')",
    "-DCXX_COMPILER=$($tools.cxx)", '-DGENERATOR=Ninja',
    '-P', (Join-Path $script:SourceRoot 'tests/compile-resource-profile/Contracts.cmake'))
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
        Invoke-CxxUnit 'uint32-integer-fast-build' $tools @('-std=c++20', '-pthread', '-Wall', '-Wextra', '-Werror',
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
            "-DPipeLangModel_CXX_RESOURCE_PROFILE=$script:CxxResourceProfile", "-DCMAKE_CXX_COMPILER=$($tools.cxx)", '-DCMAKE_BUILD_TYPE=Release',
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
        '-B', $compiledSanitizer, '-G', 'Ninja', "-DCMAKE_PREFIX_PATH=$sdk", "-DPipeLangModel_CXX_RESOURCE_PROFILE=$script:CxxResourceProfile", "-DCMAKE_CXX_COMPILER=$($tools.cxx)",
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
Invoke-Unit 'compiler-intelligence' (Join-Path $build 'model-compiler/pipelang-compiler-intelligence-tests.exe') @()
Invoke-Unit 'source-formatting' (Join-Path $build 'model-compiler/pipelang-source-formatting-tests.exe') @()

$genericMember = Join-Path $proof 'generic-member'
Invoke-Unit 'generic-member-generate' (Join-Path $build 'model-compiler/pipelang-generic-member-compiler-tests.exe') @($script:SourceRoot, $genericMember)
Compile-SelectedFixture 'generic_member_calls' (Join-Path $script:SourceRoot 'tests/native-models/generic_member_calls_runtime.cpp') $genericMember $sdk $tools
$staticMethodsSource = Join-Path $script:SourceRoot 'tests/native-models/static_methods_source.cpp'
Compile-SelectedFixture 'static_methods_objects' $staticMethodsSource (Join-Path $genericMember 'static-objects') $sdk $tools
Compile-SelectedFixture 'static_methods_project' $staticMethodsSource (Join-Path $genericMember 'static-project') $sdk $tools @('-DPIPELANG_STATIC_PROJECT=1')
$staticMethodsCompiled = Join-Path $genericMember 'static-compiled'
$staticMethodsImplementation = @(Get-ChildItem -LiteralPath $staticMethodsCompiled -Filter 'Implementation_*.cpp' | Sort-Object Name | ForEach-Object { $_.FullName })
if ($staticMethodsImplementation.Count -eq 0) {
    throw 'Static methods compiled fixture has no implementation units'
}
Compile-SelectedFixture 'static_methods_compiled' $staticMethodsSource $staticMethodsCompiled $sdk $tools (@('-DPIPELANG_STATIC_PROJECT=1') + $staticMethodsImplementation)
$varInference = Join-Path $proof 'var-inference'
Invoke-Unit 'var-inference-generate' (Join-Path $build 'model-compiler/pipelang-var-inference-compiler-tests.exe') @($script:SourceRoot, $varInference)
$varInferenceSource = Join-Path $script:SourceRoot 'tests/native-models/var_inference_source.cpp'
Compile-SelectedFixture 'var_inference_objects' $varInferenceSource (Join-Path $varInference 'objects') $sdk $tools
Compile-SelectedFixture 'var_inference_project' $varInferenceSource (Join-Path $varInference 'project') $sdk $tools @('-DPIPELANG_VAR_PROJECT=1')
$varInferenceCompiled = Join-Path $varInference 'compiled'
$varInferenceImplementation = @(Get-ChildItem -LiteralPath $varInferenceCompiled -Filter 'Implementation_*.cpp' | Sort-Object Name | ForEach-Object { $_.FullName })
if ($varInferenceImplementation.Count -eq 0) {
    throw 'Var inference compiled fixture has no implementation units'
}
Compile-SelectedFixture 'var_inference_compiled' $varInferenceSource $varInferenceCompiled $sdk $tools (@('-DPIPELANG_VAR_PROJECT=1') + $varInferenceImplementation)
$boundedCalls = Join-Path $proof 'bounded-calls'
Invoke-Unit 'bounded-calls-generate' (Join-Path $build 'model-compiler/pipelang-bounded-calls-compiler-tests.exe') @($script:SourceRoot, $boundedCalls)
$boundedSource = Join-Path $script:SourceRoot 'tests/native-models/bounded_calls_source.cpp'
Compile-SelectedFixture 'bounded_calls_objects' $boundedSource (Join-Path $boundedCalls 'objects') $sdk $tools
Compile-SelectedFixture 'bounded_calls_project' $boundedSource (Join-Path $boundedCalls 'project') $sdk $tools @('-DPIPELANG_BOUNDED_PROJECT=1')
# The fresh oracle output contains exactly the emitted implementation units.
$boundedCompiled = Join-Path $boundedCalls 'compiled'
$boundedImplementation = @(Get-ChildItem -LiteralPath $boundedCompiled -Filter 'Implementation_*.cpp' -File |
    Sort-Object Name | ForEach-Object { $_.FullName })
if ($boundedImplementation.Count -eq 0) {
    throw 'Bounded calls compiled fixture has no implementation units'
}
Compile-SelectedFixture 'bounded_calls_compiled' $boundedSource $boundedCompiled $sdk $tools (@('-DPIPELANG_BOUNDED_PROJECT=1') + $boundedImplementation)
$memberBindings = Join-Path $proof 'member-bindings'
Invoke-Unit 'member-bindings-generate' (Join-Path $build 'model-compiler/pipelang-member-bindings-compiler-tests.exe') @($script:SourceRoot, $memberBindings)
$memberBindingsSource = Join-Path $script:SourceRoot 'tests/native-models/member_bindings_source.cpp'
Compile-SelectedFixture 'member_bindings_objects' $memberBindingsSource (Join-Path $memberBindings 'objects') $sdk $tools
Compile-SelectedFixture 'member_bindings_project' $memberBindingsSource (Join-Path $memberBindings 'project') $sdk $tools @('-DPIPELANG_BINDING_PROJECT=1')
$memberBindingsCompiled = Join-Path $memberBindings 'compiled'
$memberBindingsImplementation = @(Get-ChildItem -LiteralPath $memberBindingsCompiled -Filter 'Implementation_*.cpp' -File |
    Sort-Object Name | ForEach-Object { $_.FullName })
if ($memberBindingsImplementation.Count -eq 0) {
    throw 'Member bindings compiled fixture has no implementation units'
}
Compile-SelectedFixture 'member_bindings_compiled' $memberBindingsSource $memberBindingsCompiled $sdk $tools (@('-DPIPELANG_BINDING_PROJECT=1') + $memberBindingsImplementation)
$asyncFinally = Join-Path $proof 'async-finally'
Invoke-Unit 'async-finally-generate' (Join-Path $build 'model-compiler/pipelang-async-finally-compiler-tests.exe') @($script:SourceRoot, $asyncFinally)
$asyncFinallySource = Join-Path $script:SourceRoot 'tests/native-models/async_finally_source.cpp'
Compile-SelectedFixture 'async_finally_source' $asyncFinallySource (Join-Path $asyncFinally 'objects') $sdk $tools
Compile-SelectedFixture 'async_finally_project' $asyncFinallySource (Join-Path $asyncFinally 'project') $sdk $tools @('-DPIPELANG_ASYNC_FINALLY_PROJECT=1')
$asyncFinallyCompiled = Join-Path $asyncFinally 'compiled'
$asyncFinallyImplementation = @(Get-ChildItem -LiteralPath $asyncFinallyCompiled -Filter 'Implementation_*.cpp' -File |
    Sort-Object Name | ForEach-Object { $_.FullName })
if ($asyncFinallyImplementation.Count -eq 0) {
    throw 'Async finally compiled fixture has no implementation units'
}
Compile-SelectedFixture 'async_finally_compiled' $asyncFinallySource $asyncFinallyCompiled $sdk $tools (@('-DPIPELANG_ASYNC_FINALLY_PROJECT=1') + $asyncFinallyImplementation)
Compile-SelectedFixture 'async_finally_probe' (Join-Path $script:SourceRoot 'tests/native-models/async_finally_probe.cpp') (Join-Path $asyncFinally 'probe') $sdk $tools
foreach ($fixture in @('ClassInheritance', 'ClassModules')) {
    $actual = Join-Path $classProof "$fixture.actual.json"
    Invoke-Unit "$fixture-transport" $classOracle @('--transport', (Join-Path $sdk 'bin/pipelang-modelc.exe'),
        (Join-Path $classProof "$fixture.request.json"), $actual)
    Invoke-Unit "$fixture-verify" $classOracle @('--verify', (Join-Path $classProof "$fixture.expected.json"), $actual)
}
Compile-SelectedFixture 'class-inheritance' (Join-Path $script:SourceRoot 'tests/native-models/class_inheritance_runtime.cpp') $classProof $sdk $tools
$executionSegments = Join-Path $proof 'execution-segments'
Invoke-Unit 'execution-segments-generate' (Join-Path $build 'model-compiler/pipelang-execution-segments-compiler-tests.exe') @($script:SourceRoot, $executionSegments)
$executionSegmentsSource = Join-Path $script:SourceRoot 'tests/native-models/execution_segments_source.cpp'
$executionSegmentsDiSource = Join-Path $script:SourceRoot 'tests/native-models/execution_segments_di.cpp'
Compile-SelectedFixture 'execution_segments_objects' $executionSegmentsSource (Join-Path $executionSegments 'objects') $sdk $tools
Compile-SelectedFixture 'execution_segments_project' $executionSegmentsSource (Join-Path $executionSegments 'project') $sdk $tools @('-DPIPELANG_SEGMENTS_PROJECT=1')
$executionSegmentsCompiled = Join-Path $executionSegments 'compiled'
$executionSegmentsImplementation = @(Get-ChildItem -LiteralPath $executionSegmentsCompiled -Filter 'Implementation_*.cpp' | Sort-Object Name | ForEach-Object { $_.FullName })
if ($executionSegmentsImplementation.Count -eq 0) {
    throw 'Execution segments compiled fixture has no implementation units'
}
Compile-SelectedFixture 'execution_segments_compiled' $executionSegmentsSource $executionSegmentsCompiled $sdk $tools (@('-DPIPELANG_SEGMENTS_PROJECT=1') + $executionSegmentsImplementation)
Compile-SelectedFixture 'execution_segments_di_project' $executionSegmentsDiSource (Join-Path $executionSegments 'di_project') $sdk $tools @('-DPIPELANG_SEGMENTS_PROJECT=1')
$executionSegmentsDiCompiled = Join-Path $executionSegments 'di_compiled'
$executionSegmentsDiImplementation = @(Get-ChildItem -LiteralPath $executionSegmentsDiCompiled -Filter 'Implementation_*.cpp' | Sort-Object Name | ForEach-Object { $_.FullName })
if ($executionSegmentsDiImplementation.Count -eq 0) {
    throw 'Execution segments DI compiled fixture has no implementation units'
}
Compile-SelectedFixture 'execution_segments_di_compiled' $executionSegmentsDiSource $executionSegmentsDiCompiled $sdk $tools (@('-DPIPELANG_SEGMENTS_PROJECT=1') + $executionSegmentsDiImplementation)
$independentTokens = Join-Path $proof 'independent-tokens'
Invoke-Unit 'independent-tokens-generate' (Join-Path $build 'model-compiler/pipelang-independent-tokens-compiler-tests.exe') @($script:SourceRoot, $independentTokens)
$independentSource = Join-Path $script:SourceRoot 'tests/native-models/independent_tokens_source.cpp'
$independentDiSource = Join-Path $script:SourceRoot 'tests/native-models/independent_tokens_di.cpp'
Compile-SelectedFixture 'independent_tokens_source' $independentSource (Join-Path $independentTokens 'objects') $sdk $tools
Compile-SelectedFixture 'independent_tokens_project' $independentSource (Join-Path $independentTokens 'project') $sdk $tools @('-DPIPELANG_TOKENS_PROJECT=1')
$independentCompiled = Join-Path $independentTokens 'compiled'
$independentImplementation = @(Get-ChildItem -LiteralPath $independentCompiled -Filter 'Implementation_*.cpp' -File |
    Sort-Object Name | ForEach-Object { $_.FullName })
if ($independentImplementation.Count -eq 0) {
    throw 'Independent tokens compiled fixture has no implementation units'
}
Compile-SelectedFixture 'independent_tokens_compiled' $independentSource $independentCompiled $sdk $tools (@('-DPIPELANG_TOKENS_PROJECT=1') + $independentImplementation)
Compile-SelectedFixture 'independent_tokens_di_project' $independentDiSource (Join-Path $independentTokens 'di_project') $sdk $tools @('-DPIPELANG_TOKENS_PROJECT=1')
$independentDiCompiled = Join-Path $independentTokens 'di_compiled'
$independentDiImplementation = @(Get-ChildItem -LiteralPath $independentDiCompiled -Filter 'Implementation_*.cpp' -File |
    Sort-Object Name | ForEach-Object { $_.FullName })
if ($independentDiImplementation.Count -eq 0) {
    throw 'Independent tokens DI compiled fixture has no implementation units'
}
Compile-SelectedFixture 'independent_tokens_di_compiled' $independentDiSource $independentDiCompiled $sdk $tools (@('-DPIPELANG_TOKENS_PROJECT=1') + $independentDiImplementation)
$emptyInterfaces = Join-Path $proof 'empty-source-interfaces'
Invoke-Unit 'empty-source-interface-generate' (Join-Path $build 'model-compiler/pipelang-empty-source-interface-compiler-tests.exe') @($script:SourceRoot, $emptyInterfaces)
$emptySource = Join-Path $script:SourceRoot 'tests/native-models/empty-source-interfaces/empty_source_interface_source.cpp'
$emptyTypes = Join-Path $script:SourceRoot 'tests/native-models/empty-source-interfaces/empty_source_interface_types.cpp'
Compile-SelectedFixture 'empty_interfaces_objects' $emptySource (Join-Path $emptyInterfaces 'objects') $sdk $tools
Compile-SelectedFixture 'empty_interfaces_project' $emptySource (Join-Path $emptyInterfaces 'project') $sdk $tools
Compile-SelectedFixture 'empty_interfaces_project_types' $emptyTypes (Join-Path $emptyInterfaces 'project') $sdk $tools
$emptyCompiled = Join-Path $emptyInterfaces 'compiled'
$emptyImplementation = @(Get-ChildItem -LiteralPath $emptyCompiled -Filter 'Implementation_*.cpp' -File |
    Sort-Object Name | ForEach-Object { $_.FullName })
if ($emptyImplementation.Count -eq 0) {
    throw 'Empty source interface compiled fixture has no implementation units'
}
Compile-SelectedFixture 'empty_interfaces_compiled' $emptySource $emptyCompiled $sdk $tools $emptyImplementation
Compile-SelectedFixture 'empty_interfaces_compiled_types' $emptyTypes $emptyCompiled $sdk $tools
Invoke-Unit 'async-finally-scaling-compiler' (Join-Path $build 'model-compiler/pipelang-async-finally-scaling-compiler-tests.exe') @($script:SourceRoot, (Join-Path $proof 'async-finally-scaling'))
Invoke-Unit 'async-finally-storage' (Join-Path $build 'model-compiler/pipelang-async-finally-storage-tests.exe') @()
$loopFlow = Join-Path $proof 'loop-flow'
Invoke-Unit 'loop-flow-generate' (Join-Path $build 'model-compiler/pipelang-loop-flow-compiler-tests.exe') @($script:SourceRoot, $loopFlow)
$loopFlowSource = Join-Path $script:SourceRoot 'tests/native-models/loop_flow_source.cpp'
Compile-SelectedFixture 'loop_flow_objects' $loopFlowSource (Join-Path $loopFlow 'objects') $sdk $tools
Compile-SelectedFixture 'loop_flow_project' $loopFlowSource (Join-Path $loopFlow 'project') $sdk $tools @('-DPIPELANG_LOOP_FLOW_PROJECT=1')
$loopFlowCompiled = Join-Path $loopFlow 'compiled'
$loopFlowImplementation = @(Get-ChildItem -LiteralPath $loopFlowCompiled -Filter 'Implementation_*.cpp' -File |
    Sort-Object Name | ForEach-Object { $_.FullName })
if ($loopFlowImplementation.Count -eq 0) {
    throw 'Loop flow compiled fixture has no implementation units'
}
Compile-SelectedFixture 'loop_flow_compiled' $loopFlowSource $loopFlowCompiled $sdk $tools (@('-DPIPELANG_LOOP_FLOW_PROJECT=1') + $loopFlowImplementation)
if ($fixtureSelection -ccontains 'all' -or $fixtureSelection -ccontains 'testing_library') {
    $script:executedFixtures += 'testing_library'
    $testingPackage = Join-Path $sdk 'lib/cmake/PipeLangModel'
    if (!(Test-Path -LiteralPath (Join-Path $testingPackage 'PipeLangModelConfig.cmake') -PathType Leaf)) {
        throw 'Selected installed SDK is missing its PipeLangModel package config'
    }
    foreach ($testingMode in @('normal', 'sanitizer')) {
        $testingSanitizers = 'OFF'
        if ($testingMode -eq 'sanitizer') {
            $testingSanitizers = 'ON'
        }
        $testingBuild = Join-Path $proof "testing-library/$testingMode"
        Invoke-Unit "testing-library-$testingMode-configure" $tools.cmake @(
            '-S', (Join-Path $script:SourceRoot 'tests/testing'), '-B', $testingBuild, '-G', 'Ninja',
            "-DCMAKE_PREFIX_PATH=$sdk", "-DPipeLangModel_DIR=$testingPackage",
            "-DPipeLangModel_CXX_RESOURCE_PROFILE=$script:CxxResourceProfile", "-DCMAKE_CXX_COMPILER=$($tools.cxx)", '-DCMAKE_BUILD_TYPE=Release',
            '-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG', "-DPIPELANG_TESTING_SANITIZERS=$testingSanitizers")
        Invoke-Unit "testing-library-$testingMode-build" $tools.cmake @(
            '--build', $testingBuild, '--target', 'testing_contracts', 'testing_probe_contracts', '--parallel', '1')
        if ($testingMode -eq 'sanitizer') {
            foreach ($library in Get-ChildItem -LiteralPath $tools.runtime -Filter '*.dll' -File) {
                Copy-Item -LiteralPath $library.FullName -Destination $testingBuild -Force
            }
            $env:ASAN_OPTIONS = 'detect_leaks=0:halt_on_error=1'
            $env:UBSAN_OPTIONS = 'halt_on_error=1:print_stacktrace=1'
        }
        Invoke-Unit "testing-library-$testingMode-run" (Join-Path $testingBuild 'testing_contracts.exe') @()
        Invoke-Unit "testing-probe-library-$testingMode-run" (Join-Path $testingBuild 'testing_probe_contracts.exe') @()
    }
}
if ($fixtureSelection -ccontains 'all' -or $fixtureSelection -ccontains 'testing_async_library') {
    $script:executedFixtures += 'testing_async_library'
    $testingPackage = Join-Path $sdk 'lib/cmake/PipeLangModel'
    if (!(Test-Path -LiteralPath (Join-Path $testingPackage 'PipeLangModelConfig.cmake') -PathType Leaf)) {
        throw 'Selected installed SDK is missing its PipeLangModel package config'
    }
    foreach ($testingMode in @('normal', 'sanitizer')) {
        $testingSanitizers = 'OFF'
        if ($testingMode -eq 'sanitizer') {
            $testingSanitizers = 'ON'
        }
        $testingBuild = Join-Path $proof "testing-async-library/$testingMode"
        Invoke-Unit "testing-async-library-$testingMode-configure" $tools.cmake @(
            '-S', (Join-Path $script:SourceRoot 'tests/testing-async'), '-B', $testingBuild, '-G', 'Ninja',
            "-DCMAKE_PREFIX_PATH=$sdk", "-DPipeLangModel_DIR=$testingPackage",
            "-DPipeLangModel_CXX_RESOURCE_PROFILE=$script:CxxResourceProfile", "-DCMAKE_CXX_COMPILER=$($tools.cxx)", '-DCMAKE_BUILD_TYPE=Release',
            '-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG', "-DPIPELANG_TESTING_SANITIZERS=$testingSanitizers")
        Invoke-Unit "testing-async-library-$testingMode-build" $tools.cmake @(
            '--build', $testingBuild, '--target', 'testing_async_contracts', '--parallel', '1')
        if ($testingMode -eq 'sanitizer') {
            foreach ($library in Get-ChildItem -LiteralPath $tools.runtime -Filter '*.dll' -File) {
                Copy-Item -LiteralPath $library.FullName -Destination $testingBuild -Force
            }
            $env:ASAN_OPTIONS = 'detect_leaks=0:halt_on_error=1'
            $env:UBSAN_OPTIONS = 'halt_on_error=1:print_stacktrace=1'
        }
        Invoke-Unit "testing-async-library-$testingMode-run" (Join-Path $testingBuild 'testing_async_contracts.exe') @()
    }
}
$runtimeSelection = [Environment]::GetEnvironmentVariable('PIPELANG_NATIVE_RUNTIME_SELECTION')
if (!$runtimeSelection) {
    $runtimeSelection = 'all'
}
if ($fixtureSelection -ccontains 'all' -or $fixtureSelection -ccontains 'json_library') {
    $script:executedFixtures += 'json_library'
    foreach ($jsonMode in @('normal', 'sanitizer')) {
        $jsonSanitizers = 'OFF'
        if ($jsonMode -eq 'sanitizer') {
            $jsonSanitizers = 'ON'
        }

        $jsonBuild = Join-Path $proof "json-library/$jsonMode"
        Invoke-Unit "json-library-$jsonMode-configure" $tools.cmake @(
            '-S', (Join-Path $script:SourceRoot 'tests/json'), '-B', $jsonBuild, '-G', 'Ninja',
            "-DCMAKE_PREFIX_PATH=$sdk", "-DPipeLangModel_DIR=$sdk/lib/cmake/PipeLangModel",
            "-DPipeLangModel_CXX_RESOURCE_PROFILE=$script:CxxResourceProfile", "-DCMAKE_CXX_COMPILER=$($tools.cxx)",
            '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG',
            "-DPIPELANG_JSON_SANITIZERS=$jsonSanitizers")
        Invoke-Unit "json-library-$jsonMode-build" $tools.cmake @(
            '--build', $jsonBuild, '--parallel', '1', '--target',
            'json_contracts', 'json_native_contracts', 'json_maximum_input_contracts',
            'json_string_index_contracts', 'json_string_index_source_contracts')
        foreach ($library in Get-ChildItem -LiteralPath $tools.runtime -Filter '*.dll' -File) {
            Copy-Item -LiteralPath $library.FullName -Destination $jsonBuild -Force
        }
        $env:ASAN_OPTIONS = 'detect_leaks=0:halt_on_error=1'
        $env:UBSAN_OPTIONS = 'halt_on_error=1:print_stacktrace=1'
        foreach ($jsonTest in @(
            'json_contracts', 'json_native_contracts', 'json_maximum_input_contracts',
            'json_string_index_contracts', 'json_string_index_source_contracts')) {
            $jsonExecutable = Join-Path $jsonBuild "$jsonTest.exe"
            if ($jsonMode -eq 'sanitizer' -and $jsonTest -eq 'json_string_index_contracts') {
                # Keep earlier allocations out of the aggregate ceiling witness's sanitizer process.
                Invoke-Unit "json-library-$jsonTest-$jsonMode-run" $jsonExecutable @('operations')
                Invoke-Unit "json-library-$jsonTest-aggregate-$jsonMode-run" $jsonExecutable @('aggregate')
            } else {
                Invoke-Unit "json-library-$jsonTest-$jsonMode-run" $jsonExecutable @()
            }
        }
    }
}
if ($runtimeSelection -eq 'all') {
    foreach ($fixture in @('extended_collections', 'parallel_runtime', 'concurrent_runtime', 'synchronization_runtime', 'lazy_task_scope', 'synchronous_host_scope', 'initialization_budget_runtime', 'execution_segments_runtime', 'object_runtime', 'object_transaction', 'bounded_calls_runtime', 'async_finally_runtime')) {
        Compile-SelectedFixture $fixture (Join-Path $script:SourceRoot "tests/native-models/$fixture.cpp") (Join-Path $sdk 'include') $sdk $tools
    }
    Compile-SelectedFixture 'async_finally_storage_runtime' (Join-Path $script:SourceRoot 'tests/native-models/AsyncFinallyStorageTests.cpp') (Join-Path $sdk 'include') $sdk $tools
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
