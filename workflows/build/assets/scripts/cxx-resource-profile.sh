#!/bin/sh
# Shared raw-CXX consumer policy. The installed SDK owns every profile flag.
select_cxx_resource_profile() {
    cxx_resource_sdk=$1
    cxx_resource_proof=$2
    PIPELANG_CXX_RESOURCE_PROFILE=${PIPELANG_CXX_RESOURCE_PROFILE-system}
    PIPELANG_CXX_RESOURCE_ARGUMENT=''
    case "$PIPELANG_CXX_RESOURCE_PROFILE" in
        system)
            ;;
        gnu-conservative-gc)
            cxx_resource_response="$cxx_resource_sdk/share/pipelang-model/compiler-profiles/gnu-conservative-gc.rsp"
            if [ ! -f "$cxx_resource_response" ] || [ -L "$cxx_resource_response" ]; then
                echo 'Selected installed SDK is missing the GNU compiler resource profile' >&2
                return 1
            fi
            cxx_resource_probe="$cxx_resource_proof/cxx-resource-profile-probe.cpp"
            if [ -e "$cxx_resource_probe" ]; then
                echo 'Compiler resource profile probe requires a fresh proof file' >&2
                return 1
            fi
            : > "$cxx_resource_probe"
            unit cxx-resource-profile-probe "$PIPELANG_CXX" -dM -E -x c++ "$cxx_resource_probe"
            cxx_resource_macros="$cxx_resource_proof/units/cxx-resource-profile-probe.output"
            if ! grep -Eq '^#define __GNUC__ [0-9]+' "$cxx_resource_macros" ||
               ! grep -Eq '^#define __GNUG__ [0-9]+' "$cxx_resource_macros" ||
               grep -Eq '^#define (__clang__|__INTEL_COMPILER|__INTEL_LLVM_COMPILER|__NVCOMPILER|__PGI|__PGIC__|__ibmxl__|__IBMCPP__|__SUNPRO_CC|__CUDACC__|__COMO__|__TINYC__|__DMC__)( |$)' "$cxx_resource_macros"; then
                echo 'gnu-conservative-gc requires the actual GNU C++ compiler, not a compatibility frontend' >&2
                return 1
            fi
            unit cxx-resource-profile-version "$PIPELANG_CXX" --version
            PIPELANG_CXX_RESOURCE_ARGUMENT="@$cxx_resource_response"
            sha256sum "$cxx_resource_response" > "$cxx_resource_proof/cxx-resource-profile-response.sha256"
            ;;
        *)
            echo 'PIPELANG_CXX_RESOURCE_PROFILE must be system or gnu-conservative-gc' >&2
            return 1
            ;;
    esac
    export PIPELANG_CXX_RESOURCE_PROFILE
    printf '%s\n' "$PIPELANG_CXX_RESOURCE_PROFILE" > "$cxx_resource_proof/cxx-resource-profile.txt"
    printf '%s\n' "$PIPELANG_CXX" > "$cxx_resource_proof/cxx-resource-profile-compiler.txt"
}

cxx_unit() {
    cxx_unit_name=$1
    shift
    if [ -n "$PIPELANG_CXX_RESOURCE_ARGUMENT" ]; then
        unit "$cxx_unit_name" "$PIPELANG_CXX" "$PIPELANG_CXX_RESOURCE_ARGUMENT" "$@"
    else
        unit "$cxx_unit_name" "$PIPELANG_CXX" "$@"
    fi
}
