> Current qualification: the missing compiler pack was separately downloaded
> and signature-verified after user authorization. The resolver now requires
> explicit pinned `ilc`, uses its tools directory and compiles single-threaded.
> The declared workflow completes real offline locked restore, NativeAOT build
> and native ABI execution. Previous missing-pack results below are historical
> negative evidence. Exact proof and package identity:
> `/tmp/pipelang-native-resolver-csharp-pack-20261108`.
> Set PIPELANG_NATIVE_LIBRARY_PATH only for explicitly admitted native platform
> linker inputs; ordinary development-library installations need no extra path.

# CSharp NativeAOT resolver qualification fixture

This fixture generates a readable C# class library with one `UnmanagedCallersOnly`
C export and a native C++ caller. It uses the installed
`PipeLangNativeDependencies 0.1.0` CSharp profile. No compiler code generation or
product source changes are involved.

`Prepare.cmake` writes a `.csproj`, one C# class, an exact SDK `global.json` and
NuGet configuration whose package source is **only** the selected local directory.
It first performs a real offline restore, then an actual `--locked-mode` restore.
Only two successful restores create `PinnedInputs.cmake`, containing the real
manifest, packages.lock.json, project.assets.json and toolchain digests. Failed
restore assets are never admitted. CMakeLists.txt uses the resulting pins with
`pipelang_add_native_dependency(... LANGUAGE CSharp ...)`; caller.cpp links and
checks the exported C ABI.

Inputs are absolute: OUTPUT_DIRECTORY, TOOLCHAIN_FILE, TOOLCHAIN_SHA256 and
LOCAL_PACKAGE_SOURCE. Run Prepare.cmake inside the existing contained workflow.
Set EXPECT_MISSING_PACKS=ON only for a negative test: an actual NU1101 refusal is
expected, and an unexpected successful restore fails that test. A positive test
must omit that option and then configure the generated directory with
CMAKE_PREFIX_PATH pointing to the exact installed resolver, build serially and
run CTest inside admitted containment.

The toolchain must include runtime_identifier and pinned dotnet, ilc, gcc and cxx
entries. The companion tested CSharp.json also pins cmake, make, cc, ld and
objcopy. This fixture explicitly selects /usr/bin/gcc for NativeAOT. All package
sources/caches and the .NET installation remain caller-admitted inputs; these
pins do not claim a hermetic hash of the entire SDK, compiler installation or
sysroot.

## Historical result: offline prerequisites refused

On this machine, dotnet is 10.0.114. Installed NativeAOT and ILCompiler packs are
10.0.14 for ubuntu.22.04-x64, not linux-x64. Initial linux-x64 restore refused four
missing local packages: Microsoft.NETCore.App.Runtime.linux-x64,
Microsoft.AspNetCore.App.Runtime.linux-x64,
runtime.linux-x64.Microsoft.DotNet.ILCompiler and
Microsoft.NETCore.App.Host.linux-x64.

Explicit ubuntu.22.04-x64 resolves the runtime/host packs but the SDK still
requests runtime.linux-x64.Microsoft.DotNet.ILCompiler 10.0.14 as its compiler pack.
UseRidGraph=true does not remove that missing package requirement. The same
refusal was reproduced through this reusable generator, using only the installed
SDK library-packs source.

The negative test passed and retained expected-refusal.txt. No successful locked
restore exists, PinnedInputs.cmake was not emitted, and no publish, native link or
ABI execution is claimed. No network access, installation, pack aliasing or
fabricated lockfile was used. Prepare compatible packs separately before claiming
positive NativeAOT qualification.

Evidence root: /tmp/pipelang-native-resolver-csharp-tests/proof. Preserve
restore-linux.*, restore-ubuntu.* and restore-ubuntu-ridgraph.* as negative unit
receipts. job-reusable-negative.json records the reusable negative workflow:
exit 0, about 2.07 seconds, 87,928,832 bytes aggregate peak and no memory events.
Its underlying dotnet restore failed with the expected NU1101; aggregate success
is success of the refusal test, not success of restore or compilation.
