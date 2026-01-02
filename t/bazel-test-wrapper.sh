#!/usr/bin/env bash
set -euo pipefail

rlocation() {
  local path="$1"
  if [[ -n "${RUNFILES_DIR:-}" ]]; then
    echo "${RUNFILES_DIR}/${path}"
    return 0
  fi
  if [[ -n "${RUNFILES_MANIFEST_FILE:-}" ]]; then
    local entry
    entry="$(grep -m1 "^${path} " "${RUNFILES_MANIFEST_FILE}" || true)"
    if [[ -n "${entry}" ]]; then
      echo "${entry#* }"
      return 0
    fi
  fi
  return 1
}

test_script="$1"
shift

workspace="${TEST_WORKSPACE:-}"
if [[ -z "${workspace}" ]]; then
  echo "TEST_WORKSPACE not set" >&2
  exit 1
fi

git_bin="$(rlocation "${workspace}/git")"
build_opts="$(rlocation "${workspace}/GIT-BUILD-OPTIONS")"
git_web_browse="$(rlocation "${workspace}/git-web--browse")"
if [[ -z "${git_bin}" || -z "${build_opts}" ]]; then
  echo "missing runfiles: git or GIT-BUILD-OPTIONS" >&2
  exit 1
fi

export GIT_TEST_INSTALLED
GIT_TEST_INSTALLED="$(dirname "${git_bin}")"
if [[ -n "${git_web_browse}" ]]; then
  export GIT_TEST_EXEC_PATH
  GIT_TEST_EXEC_PATH="$(dirname "${git_web_browse}")"
fi
export GIT_BUILD_DIR
GIT_BUILD_DIR="$(dirname "${build_opts}")"
export TEST_DIRECTORY
TEST_DIRECTORY="$(cd "$(dirname "${test_script}")" && pwd -P)"
if [[ "$(basename "${test_script}")" == "t4018-diff-funcname.sh" ]]; then
  tmp_root="$(mktemp -d "${TEST_TMPDIR:-/tmp}/t4018-testdir.XXXXXX")"
  cp -aL "${TEST_DIRECTORY}/." "${tmp_root}/"
  TEST_DIRECTORY="${tmp_root}"
fi

cd "${TEST_DIRECTORY}"
exec bash "$(basename "${test_script}")" "$@"
