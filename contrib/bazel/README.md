# Building Git with Bazel

The initial Bazel files come from the Bazel Central Registry `git` 2.55.0
module: https://bcr.bazel.build/modules/git/2.55.0/source.json.
The overlay files were verified against the SHA-256 digests in that manifest,
then adapted to this source tree. The existing Git ignore rules are preserved.

Build and run the regression suites with:

```sh
bazel test //... --incompatible_strict_action_env --test_output=errors
```

The repository `.bazelrc` selects OpenSSL for curl, matching the BCR presubmit.
External libraries and the Perl, Python, and Rust toolchains are resolved through
`MODULE.bazel`. Shell tests also use host utilities; Git's normal prerequisite
checks skip cases whose optional tools or platform features are unavailable.
A passing shell target therefore does not imply that every optional test ran.

For a focused run, use the shell script's basename as its target, for example:

```sh
bazel test //t:t0093-verify-cache-df-gap //t/unit-tests:unit_tests //:gitcore_tests
```

The build uses the development version declared by `GIT-VERSION-GEN`, without
reading checkout-specific Git metadata inside build actions. `MODULE.bazel`
identifies this development tree separately from the BCR release.

Shell regression scripts and Clar C unit suites are discovered automatically.
`//:gitcore_tests` runs the unit tests embedded in the core Rust modules.
`//t:check-chainlint` and `//t:check-greplint` run the lint parser regression
fixtures using the declared Perl toolchain. `//t:test-greplint` also checks
the shell tests, helpers, and performance scripts, matching the upstream
`test-greplint` Makefile target.

When updating the source tree, compare the library and builtin object lists in
`Makefile` with the Bazel rules, and check the shell, C, and Rust test targets.
The regression command above covers Bazel's registered tests; the performance,
interop, fuzzing, and optional contrib suites have separate upstream workflows.
