#!/usr/bin/env sh
set -eu

if [ "$#" -lt 2 ]; then
    echo 'Usage: trace-executions.sh ABSOLUTE_TRACE COMMAND [ARGUMENTS...]' >&2
    exit 2
fi
trace=$1
shift
case "$trace" in
    /*) ;;
    *)
        echo 'Execution trace must be absolute' >&2
        exit 2
        ;;
esac

set -- -f -e trace=execve -s 4096 -o "$trace" "$@"
tracer_help=$(/usr/bin/strace --help)
case "$tracer_help" in
    *--seccomp-bpf*)
        set -- --seccomp-bpf "$@"
        ;;
esac
# Filtering avoids ptrace stops for unselected syscalls. The execve selection,
# descendant tracing and audit remain identical. If the kernel refuses BPF,
# strace itself retains ordinary tracing instead of dropping audit coverage.
exec /usr/bin/strace "$@"
