# 'parser' contains mostly interfaces and adapters for working with parsers
# in Verible, e.g. those generated by bison.

package(
    default_applicable_licenses = ["//:license"],
    default_visibility = [
        "//verible/common/analysis:__subpackages__",
        "//verible/verilog/parser:__subpackages__",
    ],
    features = ["layering_check"],
)

cc_library(
    name = "bison-parser-adapter",
    hdrs = ["bison-parser-adapter.h"],
    deps = [
        ":parse",
        ":parser-param",
        "//verible/common/lexer:token-generator",
        "//verible/common/text:concrete-syntax-tree",
        "//verible/common/text:token-info",
        "//verible/common/util:logging",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings:string_view",
    ],
)

cc_library(
    name = "bison-parser-common",
    srcs = ["bison-parser-common.cc"],
    hdrs = ["bison-parser-common.h"],
    deps = [
        ":parser-param",
        "//verible/common/text:concrete-syntax-leaf",
        "//verible/common/text:concrete-syntax-tree",
        "//verible/common/text:token-info",
        "//verible/common/util:logging",
    ],
)

cc_library(
    name = "parse",
    hdrs = ["parse.h"],
    deps = [
        "//verible/common/text:concrete-syntax-tree",
        "//verible/common/text:token-info",
        "@com_google_absl//absl/status",
    ],
    alwayslink = 1,
)

cc_library(
    name = "parser-param",
    srcs = ["parser-param.cc"],
    hdrs = ["parser-param.h"],
    deps = [
        "//verible/common/lexer:token-generator",
        "//verible/common/text:concrete-syntax-leaf",
        "//verible/common/text:concrete-syntax-tree",
        "//verible/common/text:token-info",
        "//verible/common/util:casts",
        "//verible/common/util:logging",
        "@com_google_absl//absl/strings:string_view",
    ],
)

cc_library(
    name = "parser-test-util",
    testonly = 1,
    hdrs = ["parser-test-util.h"],
    deps = [
        "//verible/common/analysis/matcher:descent-path",
        "//verible/common/text:parser-verifier",
        "//verible/common/text:symbol",
        "//verible/common/text:token-info-test-util",
        "//verible/common/util:logging",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings:string_view",
        "@com_google_googletest//:gtest",  # for library testonly
    ],
)

cc_test(
    name = "bison-parser-common_test",
    srcs = ["bison-parser-common_test.cc"],
    deps = [
        ":bison-parser-common",
        ":parser-param",
        "//verible/common/lexer",
        "//verible/common/lexer:token-stream-adapter",
        "//verible/common/text:concrete-syntax-leaf",
        "//verible/common/text:concrete-syntax-tree",
        "//verible/common/text:symbol",
        "//verible/common/text:token-info",
        "//verible/common/util:casts",
        "@com_google_absl//absl/strings:string_view",
        "@com_google_googletest//:gtest",
        "@com_google_googletest//:gtest_main",
    ],
)
