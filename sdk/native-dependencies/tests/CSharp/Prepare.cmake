cmake_minimum_required(VERSION 3.21.1)

foreach(argument OUTPUT_DIRECTORY TOOLCHAIN_FILE LOCAL_PACKAGE_SOURCE)
    if(NOT IS_ABSOLUTE "${${argument}}")
        message(FATAL_ERROR "Set ${argument} to an absolute path")
    endif()
endforeach()
if(NOT TOOLCHAIN_SHA256)
    message(FATAL_ERROR "Set TOOLCHAIN_SHA256 explicitly")
endif()
file(SHA256 "${TOOLCHAIN_FILE}" actual_toolchain_hash)
if(NOT actual_toolchain_hash STREQUAL TOOLCHAIN_SHA256)
    message(FATAL_ERROR "Toolchain manifest checksum mismatch")
endif()
file(READ "${TOOLCHAIN_FILE}" toolchain)
string(JSON dotnet GET "${toolchain}" tools dotnet path)
string(JSON dotnet_hash GET "${toolchain}" tools dotnet sha256)
string(JSON expected_version GET "${toolchain}" tools dotnet version)
string(JSON runtime_identifier GET "${toolchain}" runtime_identifier)
string(JSON build_path GET "${toolchain}" path)
file(SHA256 "${dotnet}" actual_dotnet_hash)
if(NOT actual_dotnet_hash STREQUAL dotnet_hash)
    message(FATAL_ERROR "dotnet executable checksum mismatch")
endif()
execute_process(COMMAND "${dotnet}" --version OUTPUT_VARIABLE actual_version
    RESULT_VARIABLE version_status OUTPUT_STRIP_TRAILING_WHITESPACE TIMEOUT 15)
if(NOT version_status EQUAL 0 OR NOT actual_version STREQUAL expected_version)
    message(FATAL_ERROR "dotnet version mismatch")
endif()
if(NOT runtime_identifier MATCHES "^(linux-x64|ubuntu[.]22[.]04-x64)$")
    message(FATAL_ERROR "Select a supported Linux x64 runtime identifier explicitly")
endif()
if(NOT IS_DIRECTORY "${LOCAL_PACKAGE_SOURCE}" OR LOCAL_PACKAGE_SOURCE MATCHES "[<>&\"]")
    message(FATAL_ERROR "LOCAL_PACKAGE_SOURCE must be an admitted local directory")
endif()

if(EXISTS "${OUTPUT_DIRECTORY}")
    message(FATAL_ERROR "Select a fresh output directory; preserve existing evidence")
endif()
set(source "${OUTPUT_DIRECTORY}/source")
set(cache "${OUTPUT_DIRECTORY}/nuget-cache")
file(MAKE_DIRECTORY "${source}" "${cache}" "${OUTPUT_DIRECTORY}/restore-home")
file(WRITE "${source}/global.json" "{\n  \"sdk\": {\n    \"version\": \"${expected_version}\",\n    \"rollForward\": \"disable\",\n    \"allowPrerelease\": false\n  }\n}\n")
file(WRITE "${source}/ResolverNativeFixture.csproj" [=[
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net10.0</TargetFramework>
    <AssemblyName>ResolverNativeFixture</AssemblyName>
    <PublishAot>true</PublishAot>
    <NativeLib>Shared</NativeLib>
    <RestorePackagesWithLockFile>true</RestorePackagesWithLockFile>
    <CppCompilerAndLinker>/usr/bin/gcc</CppCompilerAndLinker>
    <UseRidGraph>true</UseRidGraph>
    <ImplicitUsings>disable</ImplicitUsings>
    <Nullable>enable</Nullable>
  </PropertyGroup>
</Project>
]=])
file(WRITE "${source}/EntryPoints.cs" [=[
using System.Runtime.InteropServices;

public static class EntryPoints
{
    [UnmanagedCallersOnly(EntryPoint = "pipelang_fixture_add")]
    public static int Add(int first, int second)
    {
        return first + second;
    }
}
]=])
file(WRITE "${source}/NuGet.Config" "<?xml version=\"1.0\" encoding=\"utf-8\"?>
<configuration>
  <packageSources>
    <clear />
    <add key=\"prepared-local-packs\" value=\"${LOCAL_PACKAGE_SOURCE}\" />
  </packageSources>
</configuration>
")
set(environment "PATH=${build_path}"
    "DOTNET_CLI_HOME=${OUTPUT_DIRECTORY}/restore-home"
    "DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1" "DOTNET_CLI_TELEMETRY_OPTOUT=1"
    "DOTNET_NOLOGO=1" "NUGET_PACKAGES=${cache}")
set(restore_arguments restore "${source}/ResolverNativeFixture.csproj"
    --runtime "${runtime_identifier}" --configfile "${source}/NuGet.Config" --packages "${cache}"
    -p:PublishAot=true -p:NativeLib=Shared)
execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${environment} "${dotnet}"
    ${restore_arguments} --use-lock-file --force-evaluate
    WORKING_DIRECTORY "${source}" RESULT_VARIABLE restore_status TIMEOUT 120
    OUTPUT_FILE "${OUTPUT_DIRECTORY}/restore.output" ERROR_FILE "${OUTPUT_DIRECTORY}/restore.output")
if(NOT restore_status EQUAL 0)
    file(READ "${OUTPUT_DIRECTORY}/restore.output" details)
    if(EXPECT_MISSING_PACKS AND details MATCHES "error NU1101:")
        file(WRITE "${OUTPUT_DIRECTORY}/expected-refusal.txt"
            "Offline restore refused missing local packages; no inputs admitted.\n${details}")
        message(STATUS "Expected offline missing-pack refusal retained; no publish qualification")
        return()
    endif()
    message(FATAL_ERROR "Offline restore failed; no restore inputs admitted. See ${OUTPUT_DIRECTORY}/restore.output")
endif()
if(EXPECT_MISSING_PACKS)
    message(FATAL_ERROR "Missing-pack negative fixture unexpectedly restored successfully")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${environment} "${dotnet}"
    ${restore_arguments} --locked-mode
    WORKING_DIRECTORY "${source}" RESULT_VARIABLE locked_status TIMEOUT 120
    OUTPUT_FILE "${OUTPUT_DIRECTORY}/locked-restore.output" ERROR_FILE "${OUTPUT_DIRECTORY}/locked-restore.output")
if(NOT locked_status EQUAL 0)
    message(FATAL_ERROR "Actual locked restore failed; no restore inputs admitted")
endif()

file(SHA256 "${source}/ResolverNativeFixture.csproj" manifest_hash)
file(SHA256 "${source}/packages.lock.json" lock_hash)
file(SHA256 "${source}/obj/project.assets.json" assets_hash)
file(WRITE "${OUTPUT_DIRECTORY}/PinnedInputs.cmake"
    "set(FIXTURE_SOURCE [====[${source}]====])\n"
    "set(FIXTURE_CACHE [====[${cache}]====])\n"
    "set(FIXTURE_TOOLCHAIN [====[${TOOLCHAIN_FILE}]====])\n"
    "set(FIXTURE_TOOLCHAIN_SHA256 ${TOOLCHAIN_SHA256})\n"
    "set(FIXTURE_MANIFEST_SHA256 ${manifest_hash})\n"
    "set(FIXTURE_LOCK_SHA256 ${lock_hash})\n"
    "set(FIXTURE_ASSETS_SHA256 ${assets_hash})\n")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" "${CMAKE_CURRENT_LIST_DIR}/caller.cpp"
    DESTINATION "${OUTPUT_DIRECTORY}")
message(STATUS "Prepared real lock and locked-restore assets: ${OUTPUT_DIRECTORY}")
