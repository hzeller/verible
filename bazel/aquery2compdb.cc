// Copyright 2023 The Verible Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// This converts aquery JSON output into
// compilation database json https://clang.llvm.org/docs/JSONCompilationDatabase.html

/* clang-format off
   # Creating a compilation DB
   OUTPUT_BASE=$(bazel info output_base)
   bazel build -c opt bazel:aquery2compdb && bazel aquery -c opt '//...' --output=jsonproto 2>/dev/null | bazel-bin/bazel/aquery2compdb ${OUTPUT_BASE} > compile_commands.json
*/ // clang-format on

#include <cstdio>
#include <filesystem>
#include <iostream>

#include "absl/strings/match.h"
#include "common/util/file_util.h"
#include "nlohmann/json.hpp"

using nlohmann::json;

static int input_problem(const char *progname, const char *msg) {
  fprintf(stderr,
          "%s: %s.\n"
          "Note: Input needs to be a bazel aquery with --output=jsonproto\n",
          progname, msg);
  return 1;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s $(bazel info output_base)\n", argv[0]);
    return 1;
  }
  const std::string output_base = argv[1];
  const std::string project_base = std::filesystem::current_path().string();

  json aquery;
  try {
    std::cin >> aquery;
  } catch (const std::exception &e) {
    return input_problem(argv[0], e.what());
  }

  const auto actions_list = aquery["actions"];
  if (!actions_list.is_array()) {
    return input_problem(argv[0], "Expected array in 'actions' element");
  }

  auto compile_db = json::array();
  for (const auto &action : actions_list) {
    if (action["mnemonic"] != "CppCompile") continue;

    auto cc_compile_action = json::object();
    cc_compile_action["directory"] = output_base;

    auto &arguments_out = cc_compile_action["arguments"] = json::array();
    std::string cc_filename;
    for (const auto &arg : action["arguments"]) {
      if (!arg.is_string()) continue;
      const std::string &str = arg;

      // Redact unneded or not understood arguments for clang-tidy
      if (str == "-fno-canonical-system-headers" || str == "-fPIC" ||
          absl::StartsWith(str, "-frandom-seed=")) {
        continue;
      }

      // Need to extract filename from the arguments to put in "file"
      // field later.
      // We don't get it from the aquery, so we fuzzily match it and look for
      // arguments that look like cc files.
      if (absl::EndsWith(str, ".cc")) cc_filename = str;
      arguments_out.push_back(str);
    }

    if (cc_filename.empty()) continue;   // Not a rule we're interested in.
    cc_compile_action["file"] = cc_filename;

    // Augment compile arguments.
    // Also need to add our project root to the include search path
    arguments_out.push_back("-iquote");
    arguments_out.push_back(project_base);

    // Collected everything needed for that cc compile.
    compile_db.push_back(cc_compile_action);

    // HACK -------------------
    // Unfortunately, we don't get an information how a header
    // would be handled from aquery. We need that, as if we look at a header
    // individually, we want to know
    //  * if it is a c++ header (-x c++)
    //  * what -I pathes are there to resolve its dependent headers.
    // Let's fake it roughtly and just add a *.h entry for each *.cc
    // file, assuming it would share the same include paths. Also assume
    // they are c++.
    // We just re-use the cc_compile_action content and modify the
    // relevant fields.
    cc_compile_action["file"] =
        absl::StrCat(verible::file::Stem(cc_filename), ".h");
    arguments_out.push_back("-x");
    arguments_out.push_back("c++");
    compile_db.push_back(cc_compile_action);
  }

  // Unfortunately, we're out of luck with generated headers and files.
  //
  //  * common/lsp/lsp-protocol.h
  //  * common/util/generated_verible_build_version.h
  //  * third_party/proto/kythe/analysis.pb.{h,cc}
  //  * third_party/proto/kythe/storage.pb.{h,cc}
  //  * verilog/CST/verilog_nonterminals_foreach-gen.inc
  //  * verilog/parser/verilog_parse_interface.h
  //  * verilog/parser/verilog_token_enum.h
  //  * verilog/tools/kythe/verilog_extractor_indexing_fact_type_foreach-gen.inc

  std::cout << compile_db.dump(2) << std::endl;
  std::cerr << compile_db.size() << " actions emitted\n";
}
