// Copyright 2017-2020 The Verible Authors.
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

#ifndef VERIBLE_VERILOG_ANALYSIS_CHECKERS_BANNED_DECLARED_NAME_PATTERNS_RULE_H_
#define VERIBLE_VERILOG_ANALYSIS_CHECKERS_BANNED_DECLARED_NAME_PATTERNS_RULE_H_

#include <set>

#include "verible/common/analysis/lint-rule-status.h"
#include "verible/common/analysis/syntax-tree-lint-rule.h"
#include "verible/common/text/concrete-syntax-tree.h"
#include "verible/common/text/syntax-tree-context.h"
#include "verible/verilog/CST/verilog-matchers.h"  // IWYU pragma: keep
#include "verible/verilog/analysis/descriptions.h"

namespace verilog {
namespace analysis {

// checks declared identifier with set of unwanted patterns
class BannedDeclaredNamePatternsRule : public verible::SyntaxTreeLintRule {
 public:
  using rule_type = verible::SyntaxTreeLintRule;

  static const LintRuleDescriptor &GetDescriptor();

  void HandleNode(const verible::SyntaxTreeNode &node,
                  const verible::SyntaxTreeContext &context) final;

  verible::LintRuleStatus Report() const final;

 private:
  std::set<verible::LintViolation> violations_;
};

}  // namespace analysis
}  // namespace verilog

#endif  // VERIBLE_VERILOG_ANALYSIS_CHECKERS_BANNED_DECLARED_NAME_PATTERNS_RULE_H_