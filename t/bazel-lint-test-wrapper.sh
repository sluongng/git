#!/usr/bin/env bash
# Run the check-chainlint/check-greplint recipes without writing into runfiles.
# --- begin runfiles.bash initialization ---
# Copy-pasted from Bazel's Bash runfiles library (tools/bash/runfiles/runfiles.bash).
set -euo pipefail
if [[ ! -d "${RUNFILES_DIR:-/dev/null}" && ! -f "${RUNFILES_MANIFEST_FILE:-/dev/null}" ]]; then
 if [[ -f "$0.runfiles_manifest" ]]; then
   export RUNFILES_MANIFEST_FILE="$0.runfiles_manifest"
 elif [[ -f "$0.runfiles/MANIFEST" ]]; then
   export RUNFILES_MANIFEST_FILE="$0.runfiles/MANIFEST"
 elif [[ -f "$0.runfiles/bazel_tools/tools/bash/runfiles/runfiles.bash" ]]; then
   export RUNFILES_DIR="$0.runfiles"
 fi
fi
if [[ -f "${RUNFILES_DIR:-/dev/null}/bazel_tools/tools/bash/runfiles/runfiles.bash" ]]; then
  source "${RUNFILES_DIR}/bazel_tools/tools/bash/runfiles/runfiles.bash"
elif [[ -f "${RUNFILES_MANIFEST_FILE:-/dev/null}" ]]; then
  source "$(grep -m1 "^bazel_tools/tools/bash/runfiles/runfiles.bash " \
    "$RUNFILES_MANIFEST_FILE" | cut -d ' ' -f 2-)"
else
  echo >&2 "ERROR: cannot find @bazel_tools//tools/bash/runfiles:runfiles.bash"
  exit 1
fi
# --- end runfiles.bash initialization ---

lint="$1"
script="$2"
# Perl do() requires an absolute or explicitly relative shared-parser path.
if [[ "$script" != /* ]]; then
  script="$PWD/$script"
fi
shift 2
# $(PERL) is an exec path; translate external repositories to runfiles keys.
case "${PERL:?Perl toolchain path is required}" in
  /*) perl="$PERL" ;;
  external/*) perl="$(rlocation "${PERL#external/}")" ;;
  *) perl="$(rlocation "${TEST_WORKSPACE:?}/$PERL")" ;;
esac

if [[ "$lint" == test-greplint ]]; then
  exec "$perl" "$script" "$@"
fi

cd "$(dirname "$script")"

outdir="$(mktemp -d "${TEST_TMPDIR:-${TMPDIR:-/tmp}}/git-${lint}.XXXXXX")"
trap 'rm -rf "$outdir"' EXIT
export LC_ALL=C

fixtures=("${lint}"/*.test)
names=()
for fixture in "${fixtures[@]}"; do
  name="${fixture##*/}"
  names+=("${name%.test}")
done

"$perl" "./${lint}-cat.pl" "$outdir" "${names[@]}"
case "$lint" in
  chainlint)
    "$perl" ./chainlint.pl --emit-all "$outdir/tests" >"$outdir/actual" || true
    ;;
  greplint)
    "$perl" ./greplint.pl "${fixtures[@]}" >"$outdir/actual" 2>&1 || true
    ;;
  *)
    echo "unknown lint suite: $lint" >&2
    exit 1
    ;;
esac

diff -u "$outdir/expect" "$outdir/actual"
