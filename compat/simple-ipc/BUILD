load("//contrib/bazel:defs.bzl", "GIT_COPTS")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "simple-ipc",
    srcs = [
        "ipc-shared.c",
        "ipc-unix-socket.c",
    ],
    copts = GIT_COPTS,
    deps = [
        "//:abspath_h",
        "//:gettext_h",
        "//:git_compat_util_h",
        "//:pkt_line_h",
        "//:simple_ipc_h",
        "//:strbuf_h",
        "//:thread_utils_h",
        "//:trace2_h",
        "//:trace_h",
        "//:unix_socket_h",
        "//:unix_stream_server_h",
    ],
)
