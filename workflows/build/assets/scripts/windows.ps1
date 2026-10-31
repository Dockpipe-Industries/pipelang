param([switch] $Inside)
. (Join-Path $PSScriptRoot 'windows-common.ps1')

if (!$Inside) {
    $proof = [Environment]::GetEnvironmentVariable('PIPELANG_NATIVE_TOOLCHAIN_PROOF')
    if (!$proof) {
        $workspace = Join-Path ([IO.Path]::GetTempPath()) ('pipelang-native-build-' + [Guid]::NewGuid().ToString('N'))
        $proof = Join-Path $workspace 'proof'
    }
    $proof = Get-ExternalProof $proof
    $bootstrap = "$proof-bootstrap"
    New-ProofDirectory $bootstrap
    $toolsReceipt = [Environment]::GetEnvironmentVariable('PIPELANG_WINDOWS_TOOLS_RECEIPT')
    if ($toolsReceipt) {
        $toolsReceipt = Get-AbsolutePath $toolsReceipt
        $expectedTools = Get-RequiredEnvironment 'PIPELANG_WINDOWS_TOOLS_RECEIPT_SHA256'
        if ((Get-Sha256 $toolsReceipt) -ne $expectedTools) {
            throw 'Selected native tool-cache receipt differs'
        }
        $tools = Get-Content -LiteralPath $toolsReceipt -Raw | ConvertFrom-Json
    } else {
        $tools = Get-WindowsTools (Join-Path $bootstrap 'tools')
    }
    Set-WindowsTools $tools
    $bootstrapBuild = Join-Path $bootstrap 'build'
    $bootstrapScript = Join-Path $PSScriptRoot 'Invoke-WindowsBootstrap.ps1'
    & $bootstrapScript -Executable $tools.cmake -Arguments @('-S', (Join-Path $script:SourceRoot 'src/native/bootstrap'),
        '-B', $bootstrapBuild, '-G', 'Ninja', "-DCMAKE_CXX_COMPILER:FILEPATH=$($tools.cxx.Replace('\', '/'))",
        "-DCMAKE_MAKE_PROGRAM:FILEPATH=$($tools.ninja.Replace('\', '/'))", "-DCMAKE_RC_COMPILER:FILEPATH=$($tools.rc.Replace('\', '/'))", '-DCMAKE_BUILD_TYPE=Release') `
        -Proof (Join-Path $bootstrap 'units/configure') -Estate $bootstrap
    & $bootstrapScript -Executable $tools.cmake -Arguments @('--build', $bootstrapBuild, '--parallel', '1') `
        -Proof (Join-Path $bootstrap 'units/build') -Estate $bootstrap -TimeoutSeconds 1800
    $runner = Join-Path $bootstrapBuild 'containedexec/pipelang-contained.exe'
    $probe = Join-Path $bootstrapBuild 'containedexec/pipelang-containment-probe.exe'
    $admission = Join-Path $bootstrap 'containment'
    Invoke-Native $probe @('--suite', $runner, $admission)
    $receipt = Join-Path $admission 'acceptance.json'
    Invoke-Native $probe @('--admit', $runner, $receipt, (Get-Sha256 $receipt))
    New-ProofDirectory $proof
    Write-JsonFile (Join-Path $proof 'windows-tools.json') $tools
    $env:PIPELANG_NATIVE_TOOLCHAIN_PROOF = $proof
    $env:PIPELANG_NATIVE_RUNNER = $runner
    $env:PIPELANG_NATIVE_RUNNER_SHA256 = Get-Sha256 $runner
    Invoke-WindowsJob $PSCommandPath $proof $runner 10800
    $installedRunner = Join-Path $proof 'build/containedexec/pipelang-contained.exe'
    $installedProbe = Join-Path $proof 'build/containedexec/pipelang-containment-probe.exe'
    Invoke-Native $installedProbe @('--suite', $installedRunner, (Join-Path $proof 'installed-containment'))
    [void] [IO.Directory]::CreateDirectory((Join-Path $script:SourceRoot 'bin'))
    Write-JsonFile (Join-Path $script:SourceRoot 'bin/native-current.json') ([ordered]@{ proof = $proof; sdk = (Join-Path $proof 'sdk') })
    Write-Host ('Native SDK: {0}' -f (Join-Path $proof 'sdk'))
    exit 0
}

$proof = Get-ExternalProof (Get-RequiredEnvironment 'PIPELANG_NATIVE_TOOLCHAIN_PROOF')
$runner = Get-AbsolutePath (Get-RequiredEnvironment 'PIPELANG_NATIVE_RUNNER')
if ((Get-Sha256 $runner) -ne (Get-RequiredEnvironment 'PIPELANG_NATIVE_RUNNER_SHA256')) {
    throw 'Qualified native bootstrap runner changed'
}
$tools = Get-Content -LiteralPath (Join-Path $proof 'windows-tools.json') -Raw | ConvertFrom-Json
Set-WindowsTools $tools
Initialize-Units $proof $runner
$build = Join-Path $proof 'build'
$sdk = Join-Path $proof 'sdk'
Invoke-Unit 'configure' $tools.cmake @('-S', (Join-Path $script:SourceRoot 'sdk/pipeview/model'), '-B', $build,
    '-G', 'Ninja', "-DCMAKE_CXX_COMPILER:FILEPATH=$($tools.cxx.Replace('\', '/'))", "-DCMAKE_MAKE_PROGRAM:FILEPATH=$($tools.ninja.Replace('\', '/'))",
    "-DCMAKE_RC_COMPILER:FILEPATH=$($tools.rc.Replace('\', '/'))", '-DCMAKE_BUILD_TYPE=Release', '-DPIPELANG_BUILD_SDK_TOOL_TESTS=ON',
    '-DPIPELANG_BUILD_CLASS_INHERITANCE_TESTS=ON')
Invoke-Unit 'build' $tools.cmake @('--build', $build, '--parallel', '1') 1800
Invoke-Unit 'install' $tools.cmake @('--install', $build, '--prefix', $sdk)
$tool = Join-Path $sdk 'bin/pipelang-sdk-tool.exe'
Invoke-Unit 'inventory' $tool @('inventory', '--source-root', $script:SourceRoot, '--output', (Join-Path $proof 'inventory.json'))
Invoke-Unit 'sdk-tests' (Join-Path $build 'sdk-tools/pipelang-sdk-tool-tests.exe') @($script:SourceRoot, $sdk, (Join-Path $proof 'sdk-tests'))
Invoke-Unit 'package-tests' (Join-Path $build 'sdk-tools/pipelang-package-tests.exe') @((Join-Path $proof 'package-tests'))
$legacy = Join-Path $sdk 'bin/pipelang-native.exe'
Invoke-Unit 'legacy-compiler-tests' (Join-Path $build 'legacy-compiler/pipelang-legacy-compiler-tests.exe') @($script:SourceRoot, (Join-Path $proof 'legacy-compiler-tests'), $legacy)
Invoke-Unit 'materialize-tests' (Join-Path $build 'legacy-compiler/pipelang-materialize-tests.exe') @((Join-Path $proof 'materialize-tests'), $legacy)
Invoke-Unit 'diagnostic-tests' (Join-Path $build 'legacy-compiler/pipelang-diagnostic-tests.exe') @((Join-Path $proof 'diagnostic-tests'), $legacy)
Invoke-Unit 'operation-tests' (Join-Path $build 'legacy-compiler/pipelang-operation-tests.exe') @((Join-Path $proof 'operation-tests'), $legacy)
$companion = Join-Path $proof 'companion-tests'
Invoke-Unit 'companion-tests' (Join-Path $build 'sdk-tools/pipelang-companion-tests.exe') @($script:SourceRoot, $sdk, $companion)
Invoke-Unit 'companion-generated-build' $tools.cxx @('-std=c++17', '-Wall', '-Wextra', '-Werror', '-O2', '-static',
    "-I$companion", (Join-Path $companion 'emitted_probe.cpp'), '-o', (Join-Path $companion 'emitted-probe.exe'))
Invoke-Unit 'companion-generated-run' (Join-Path $companion 'emitted-probe.exe') @()
$scalars = Join-Path $proof 'scalar-tests'
Invoke-Unit 'scalar-tests' (Join-Path $build 'scalar-compiler/pipelang-scalar-tests.exe') @($script:SourceRoot, $sdk, $scalars)
Invoke-Unit 'scalar-generated-build' $tools.cxx @('-std=c++17', '-Wall', '-Wextra', '-Werror', '-O2', '-static',
    (Join-Path $scalars 'generated/runtime.cpp'), '-o', (Join-Path $scalars 'generated/runtime-test.exe'))
Invoke-Unit 'scalar-generated-run' (Join-Path $scalars 'generated/runtime-test.exe') @()
