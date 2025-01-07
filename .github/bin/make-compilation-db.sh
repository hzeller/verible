#!/usr/bin/env bash
# Copyright 2021 The Verible Authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -u
set -e

# Which bazel and bant to use
BAZEL=${BAZEL:-bazel}
BANT=${BANT:-needs-to-be-compiled-locally}

# Bant not given, compile from bzlmod dep. We need to do that before
# we run other bazel rules below as we change the cxxopt flags. Remember
# the full realpath of the resulting binary to be immune to symbolic-link
# switcharoo by bazel.
if [ "${BANT}" = "needs-to-be-compiled-locally" ]; then
  ${BAZEL} build -c opt --cxxopt=-std=c++20 @bant//bant:bant
  BANT=$(realpath bazel-bin/external/bant*/bant/bant)
fi

# Bazel-build all targets that generate files, so that they can be
# seen in dependency analysis.
BAZEL_OPTS="-c opt"
${BAZEL} build ${BAZEL_OPTS} $(${BANT} list-targets | \
  egrep "genrule|cc_proto_library|genlex|genyacc" | awk '{print $3}')

# Some selected targets to trigger remaining dependency fetches from MODULE.bazel
# verilog-y-final to create a header, kzip creator to trigger build of any.pb.h
# and some test that triggers fetching nlohmann_json and gtest
${BAZEL} build ${BAZEL_OPTS} //verible/verilog/parser:verilog-y-final \
  //verible/verilog/tools/kythe:verible-verilog-kythe-kzip-writer \
  //verible/common/lsp:json-rpc-dispatcher_test

# bant does not distinguish the includes per file yet, so instead of
# a compile_commands.json, we can just as well create a simpler
# compile_flags.txt which is easier to digest for all kinds of tools anyway.
${BANT} compile-flags 2>/dev/null > compile_flags.txt

# Bant does not see the flex dependency inside the toolchain yet.
for d in bazel-out/../../../external/*flex*/src/FlexLexer.h ; do
  echo "-I$(dirname $d)" >> compile_flags.txt
done
