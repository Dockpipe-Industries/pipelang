#!/bin/bash
# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
set -euo pipefail

compiler=$1
sdk_tool=$2
llc=$3
linker=$4
proof=$5
arch=$6
assets=$(cd "$(dirname "$0")/.." && pwd -P)
llvm=$(dirname "$llc")

case "$arch" in
    arm64)
        target=macos-arm64
        format='Mach-O arm64'
        assembly_flags=()
        ;;
    x86_64)
        target=macos-x64
        format='Mach-O 64-bit x86-64'
        assembly_flags=(--defsym=X64=1)
        ;;
    *)
        printf 'Unsupported macOS test architecture: %s\n' "$arch" >&2
        exit 1
        ;;
esac

# A Linux worker has no Apple sysroot. Use a synthetic TAPI fixture for
# link and symbol publication checks. The application is not executed here.
mkdir -p "$proof/sysroot/usr/lib" "$proof/repeat"
"$llvm/llvm-mc" "-triple=$arch-apple-macosx11.0" -filetype=obj "${assembly_flags[@]}" \
    "$assets/macos-application-link.s" -o "$proof/fixture.o"
"$llvm/llvm-ar" rcs "$proof/runtime.a" "$proof/fixture.o"
for library in libSystem 'libc++'; do
    cat >"$proof/sysroot/usr/lib/$library.tbd" <<TBD
--- !tapi-tbd
tbd-version: 4
targets: [ $arch-macos ]
install-name: '/usr/lib/$library.dylib'
exports:
  - targets: [ $arch-macos ]
    symbols: [ dyld_stub_binder ]
...
TBD
done

/bin/bash "$assets/scripts/stage-macos-link-sdk.sh" "$proof/runtime.a" "$sdk_tool" "$proof/sdk"
arguments=(--target "$target" --executable --llc "$llc" --linker "$linker"
    --link-sdk "$proof/sdk" --link-sdk-sha256 "$(<"$proof/sdk/link-sdk.sha256")"
    --macos-sysroot "$proof/sysroot" --macos-sdk-version 11.0 --source "$assets/MachOApplicationLink.pipe"
    --entry NativeProof.MachOApplicationLink.App.Run)
dwarf=app.dSYM/Contents/Resources/DWARF/app

for profile in debug release; do
    mkdir "$proof/$profile" "$proof/repeat/$profile"
    "$compiler" "${arguments[@]}" --profile "$profile" --dsymutil "$llvm/dsymutil" --output "$proof/$profile/app"
    "$compiler" "${arguments[@]}" --profile "$profile" --dsymutil "$llvm/dsymutil" --output "$proof/repeat/$profile/app"
    cmp "$proof/$profile/app" "$proof/repeat/$profile/app"
    cmp "$proof/$profile/$dwarf" "$proof/repeat/$profile/$dwarf"
    "$llvm/llvm-readobj" --file-headers "$proof/$profile/app" > "$proof/$profile/headers.txt"
    grep -Fx "Format: $format" "$proof/$profile/headers.txt"
    "$llvm/llvm-dwarfdump" --debug-line "$proof/$profile/$dwarf" > "$proof/$profile/lines.txt"
    grep -F 'MachOApplicationLink.pipe' "$proof/$profile/lines.txt"
    executable_uuid=$("$llvm/llvm-dwarfdump" --uuid "$proof/$profile/app" | cut -d ' ' -f 2)
    symbols_uuid=$("$llvm/llvm-dwarfdump" --uuid "$proof/$profile/$dwarf" | cut -d ' ' -f 2)
    test -n "$executable_uuid" && test "$executable_uuid" = "$symbols_uuid"
done

rm "$proof/debug/$dwarf"
"$compiler" "${arguments[@]}" --profile debug --dsymutil "$llvm/dsymutil" --output "$proof/debug/app"
cmp "$proof/debug/$dwarf" "$proof/repeat/debug/$dwarf"
rm "$proof/debug/$dwarf" "$proof/debug/app.dSYM/Contents/Info.plist"
"$compiler" "${arguments[@]}" --profile debug --dsymutil "$llvm/dsymutil" --output "$proof/debug/app"
cmp "$proof/debug/$dwarf" "$proof/repeat/debug/$dwarf"

mkdir -p "$proof/unrelated/app.dSYM"
printf '%s\n' 'preserve this file' > "$proof/unrelated/app.dSYM/keep.txt"
if "$compiler" "${arguments[@]}" --profile debug --dsymutil "$llvm/dsymutil" \
    --output "$proof/unrelated/app" 2>"$proof/unrelated-symbols.stderr"; then
    exit 1
fi
grep -F 'native dSYM contains an unexpected file' "$proof/unrelated-symbols.stderr"
test "$(<"$proof/unrelated/app.dSYM/keep.txt")" = 'preserve this file'

report="$proof/debug/app.dSYM/Contents/Resources/report.txt"
printf '%s\n' 'preserve this report' > "$report"
cp "$proof/debug/app.dSYM/Contents/Info.plist" "$proof/prior-info.plist"
if "$compiler" "${arguments[@]}" --profile release --dsymutil "$llvm/dsymutil" \
    --output "$proof/debug/app" 2>"$proof/populated-symbols.stderr"; then
    exit 1
fi
grep -Fx "pipelang-objectc: native dSYM contains an unexpected file: $report" "$proof/populated-symbols.stderr"
test "$(<"$report")" = 'preserve this report'
cmp "$proof/debug/app" "$proof/repeat/debug/app"
cmp "$proof/debug/$dwarf" "$proof/repeat/debug/$dwarf"
cmp "$proof/debug/app.dSYM/Contents/Info.plist" "$proof/prior-info.plist"
rm "$report"

if "$compiler" "${arguments[@]}" --profile release --dsymutil /usr/bin/false \
    --output "$proof/debug/app" 2>"$proof/failed-symbols.stderr"; then
    exit 1
fi
grep -F 'native dSYM generation failed' "$proof/failed-symbols.stderr"
cmp "$proof/debug/app" "$proof/repeat/debug/app"
cmp "$proof/debug/$dwarf" "$proof/repeat/debug/$dwarf"

"$compiler" "${arguments[@]}" --profile release --dsymutil "$llvm/dsymutil" --output "$proof/debug/app"
cmp "$proof/debug/app" "$proof/release/app"
cmp "$proof/debug/$dwarf" "$proof/release/$dwarf"

# A symbol tool can emit companion metadata alongside the DWARF member.
cat > "$proof/dsymutil-with-metadata" <<EOF
#!/bin/bash
set -euo pipefail
"$llvm/dsymutil" "\$@"
bundle=\${!#}
/bin/mkdir -p "\$bundle/Contents/Resources/Metadata"
printf '%s\n' 'generated metadata' > "\$bundle/Contents/Resources/Metadata/app.txt"
EOF
chmod +x "$proof/dsymutil-with-metadata"
"$compiler" "${arguments[@]}" --profile debug --dsymutil "$proof/dsymutil-with-metadata" --output "$proof/debug/app"
metadata="$proof/debug/app.dSYM/Contents/Resources/Metadata/app.txt"
printf '%s\n' 'previous metadata' > "$metadata"
"$compiler" "${arguments[@]}" --profile release --dsymutil "$proof/dsymutil-with-metadata" --output "$proof/debug/app"
test "$(<"$metadata")" = 'generated metadata'
cmp "$proof/debug/app" "$proof/release/app"
cmp "$proof/debug/$dwarf" "$proof/release/$dwarf"
test -z "$(find "$proof" -name '*.pipelang-publish' -print)"
