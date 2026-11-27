# SPDX-License-Identifier: Apache-2.0
if(NOT ACTION STREQUAL "build")
    message(FATAL_ERROR "CSharp profile supports NativeAOT shared library builds")
endif()
string(JSON runtime_identifier GET "${toolchain}" runtime_identifier)
if(NOT runtime_identifier MATCHES "^(linux-x64|ubuntu[.]22[.]04-x64)$")
    message(FATAL_ERROR "Unqualified NativeAOT runtime identifier")
endif()
get_filename_component(ilc_tools "${tool_ilc}" DIRECTORY)
pipelang_execute("DOTNET_PROCESSOR_COUNT=1" "DOTNET_GENERATE_ASPNET_CERTIFICATE=false" "DOTNET_CLI_HOME=${BUILD_DIRECTORY}/home" "DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1"
    "DOTNET_CLI_TELEMETRY_OPTOUT=1" "DOTNET_NOLOGO=1" "NUGET_PACKAGES=${CACHE_DIRECTORY}"
    "${tool_dotnet}" publish "${MANIFEST}" --configuration Release --runtime "${runtime_identifier}"
    --self-contained true --no-restore --output "${BUILD_DIRECTORY}/publish"
    "-p:AssemblyName=${LIBRARY_NAME}" "-p:IlcToolsPath=${ilc_tools}" -p:IlcSingleThreaded=true -p:BuildInParallel=false -p:PublishAot=true -p:NativeLib=Shared -p:RestoreLockedMode=true
    "-p:ProjectAssetsFile=${RESTORE_ASSETS}")
