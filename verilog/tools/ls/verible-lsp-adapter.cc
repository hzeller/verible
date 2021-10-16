// Copyright 2021 The Verible Authors.
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
//

#include "verilog/tools/ls/verible-lsp-adapter.h"

#include "common/lsp/lsp-protocol-operators.h"
#include "common/lsp/lsp-protocol.h"
#include "nlohmann/json.hpp"
#include "verilog/tools/ls/document-symbol-filler.h"

using verible::lsp::DocumentSymbol;
using verible::lsp::DocumentSymbolParams;
using verilog::VerilogAnalyzer;

namespace verilog {
nlohmann::json CreateDocumentSymbolOutline(const VerilogAnalyzer &parser,
                                           bool kate_compatible_tags,
                                           const DocumentSymbolParams &p) {
  DocumentSymbol toplevel;
  const absl::string_view base = parser.Data().Contents();

  verilog::DocumentSymbolFiller filler(kate_compatible_tags, base, &toplevel);
  const auto &text_structure = parser.Data();
  const auto &syntax_tree = text_structure.SyntaxTree();
  syntax_tree->Accept(&filler);
  return toplevel.children;  // We cut down one level.
}

// Convert our representation of a linter violation to a LSP-Diagnostic
static verible::lsp::Diagnostic ViolationToDiagnostic(
    const verilog::LintViolationWithStatus &v, absl::string_view base,
    const verible::LineColumnMap &lc_map) {
  const verible::LintViolation &violation = *v.violation;
  verible::LineColumn start = lc_map(violation.token.left(base));
  verible::LineColumn end = lc_map(violation.token.right(base));
  const char *fix_msg = violation.autofixes.empty() ? "" : " (fix available)";
  return verible::lsp::Diagnostic{
      .range =
          {
              .start = {.line = start.line, .character = start.column},
              .end = {.line = end.line, .character = end.column},
          },
      .message = absl::StrCat(violation.reason, " ", v.status->url, "[",
                              v.status->lint_rule_name, "]", fix_msg),
  };
}

std::vector<verible::lsp::Diagnostic> CreateDiagnostics(
    const VerilogAnalyzer &parser,
    const std::vector<verible::LintRuleStatus> &lint_statuses) {
  // TODO: files that generate a lot of messages will create a huge
  // output. So we limit the messages here.
  // However, we should work towards emitting them around the last known
  // edit point in the document as this is what the user sees.
  static constexpr int kMaxMessages = 100;
  const auto &rejected_tokens = parser.GetRejectedTokens();
  auto const &lint_violations = verilog::GetSortedViolations(lint_statuses);
  std::vector<verible::lsp::Diagnostic> result;
  int remaining = rejected_tokens.size() + lint_violations.size();
  if (remaining > kMaxMessages) remaining = kMaxMessages;
  result.reserve(remaining);
  for (const auto &rejected_token : rejected_tokens) {
    parser.ExtractLinterTokenErrorDetail(
        rejected_token,
        [&result](const std::string &filename, verible::LineColumnRange range,
                  verible::AnalysisPhase phase, absl::string_view token_text,
                  absl::string_view context_line, const std::string &msg) {
          // Note: msg is currently empty and not useful.
          const auto message = (phase == verible::AnalysisPhase::kLexPhase)
                                   ? "token error"
                                   : "syntax error";
          result.emplace_back(verible::lsp::Diagnostic{
              .range{.start{.line = range.start.line,
                            .character = range.start.column},
                     .end{.line = range.end.line,  //
                          .character = range.end.column}},
              .message = message,
          });
        });
    if (--remaining <= 0) break;
  }

  const absl::string_view base = parser.Data().Contents();
  verible::LineColumnMap line_column_map(base);
  for (const auto &v : lint_violations) {
    result.emplace_back(ViolationToDiagnostic(v, base, line_column_map));
    if (--remaining <= 0) break;
  }
  return result;
}

static std::vector<verible::lsp::TextEdit> AutofixToTextEdits(
    const verible::AutoFix &fix, absl::string_view base,
    const verible::LineColumnMap &lc_map) {
  std::vector<verible::lsp::TextEdit> result;
  // TODO(hzeller): figure out if edits are stacking or are all based
  // on the same start status.
  for (const verible::ReplacementEdit &edit : fix.Edits()) {
    verible::LineColumn start = lc_map(edit.fragment.begin() - base.begin());
    verible::LineColumn end = lc_map(edit.fragment.end() - base.begin());
    result.emplace_back(verible::lsp::TextEdit{
        .range =
            {
                .start = {.line = start.line, .character = start.column},
                .end = {.line = end.line, .character = end.column},
            },
        .newText = edit.replacement,
    });
  }
  return result;
}

std::vector<verible::lsp::CodeAction> GenerateLinterCodeActions(
    const VerilogAnalyzer &parser,
    const std::vector<verible::LintRuleStatus> &lint_statuses,
    const verible::lsp::CodeActionParams &p) {
  auto const &lint_violations = verilog::GetSortedViolations(lint_statuses);
  if (lint_violations.empty()) return {};

  const absl::string_view base = parser.Data().Contents();
  verible::LineColumnMap line_column_map(base);

  std::vector<verible::lsp::CodeAction> result;
  for (const auto &v : lint_violations) {
    const verible::LintViolation &violation = *v.violation;
    if (violation.autofixes.empty()) continue;
    auto diagnostic = ViolationToDiagnostic(v, base, line_column_map);

    // The editor usually has the cursor on a line or word, so we
    // only want to output edits that are relevant.
    if (!rangeOverlap(diagnostic.range, p.range)) continue;

    bool preferred_fix = true;
    for (const auto &fix : violation.autofixes) {
      result.emplace_back(verible::lsp::CodeAction{
          .title = fix.Description(),
          .kind = "quickfix",
          .diagnostics = {diagnostic},
          .isPreferred = preferred_fix,
          // The following is translated from json, map uri -> edits.
          // We're only sending changes for one document, the current one.
          .edit = {.changes = {{p.textDocument.uri,
                                AutofixToTextEdits(fix, base,
                                                   line_column_map)}}},
      });
      preferred_fix = false;  // only the first is preferred.
    }
  }
  return result;
}
}  // namespace verilog
