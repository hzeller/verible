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

#include <iomanip>  // Only needed for json debugging right now
#include <iostream>

#include "absl/flags/flag.h"
#include "common/lsp/json-rpc-dispatcher.h"
#include "common/lsp/lsp-protocol-operators.h"
#include "common/lsp/lsp-protocol.h"
#include "common/lsp/lsp-text-buffer.h"
#include "common/lsp/message-stream-splitter.h"
#include "common/util/init_command_line.h"
#include "verilog/analysis/verilog_analyzer.h"
#include "verilog/analysis/verilog_linter.h"
#include "verilog/parser/verilog_token_classifications.h"
#include "verilog/tools/ls/document-symbol-filler.h"
#include "verilog/tools/ls/lsp-parse-buffer.h"

// Windows specific implementation of read()
#ifndef _WIN32
#include <unistd.h>
#else
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
// Windows doesn't have Posix read(), but something called _read
#define read(fd, buf, size) _read(fd, buf, size)
#endif

using nlohmann::json;
using verible::lsp::BufferCollection;
using verible::lsp::DocumentSymbol;
using verible::lsp::DocumentSymbolParams;
using verible::lsp::EditTextBuffer;
using verible::lsp::InitializeResult;
using verible::lsp::JsonRpcDispatcher;
using verible::lsp::MessageStreamSplitter;
using verible::lsp::Range;
using verilog::VerilogAnalyzer;

ABSL_FLAG(bool, kate_workaround, false, "Work around some kate issues");

static std::string GetVersionNumber() {
  return "0.0 alpha";  // TODO(hzeller): once ready, extract from build version
}

// The "initialize" method requests server capabilities.
InitializeResult InitializeServer(const nlohmann::json &params) {
  // Ignore passed client capabilities from params right now,
  // just announce what we do.
  InitializeResult result;
  result.serverInfo = {
      .name = "Verible Verilog language server.",
      .version = GetVersionNumber(),
  };
  result.capabilities = {
      {
          "textDocumentSync",
          {
              {"openClose", true},  // Want open/close events
              {"change", 2},        // Incremental updates
          },
      },
      {"codeActionProvider", true},      // Autofixes for lint errors
      {"documentSymbolProvider", true},  // Outline of file
  };

  return result;
}

nlohmann::json HandleDocumentSymbols(const VerilogAnalyzer &parser,
                                     const DocumentSymbolParams &p) {
  DocumentSymbol toplevel;
  const absl::string_view base = parser.Data().Contents();

  verilog::DocumentSymbolFiller filler(absl::GetFlag(FLAGS_kate_workaround),
                                       base, &toplevel);
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

void ConsiderSendDiagnostics(const std::string &uri,
                             const verilog::ParsedBuffer *buffer,
                             JsonRpcDispatcher *dispatcher) {
  verible::lsp::PublishDiagnosticsParams params;
  params.uri = uri;
  params.diagnostics =
      CreateDiagnostics(buffer->parser(), buffer->lint_result());
  dispatcher->SendNotification("textDocument/publishDiagnostics", params);
}

int main(int argc, char *argv[]) {
  const auto file_args = verible::InitCommandLine(argv[0], &argc, &argv);

#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
#endif

  std::cerr << "Verible Alpha Language Server " << GetVersionNumber()
            << std::endl;

  // Input and output is stdin and stdout
  static constexpr int in_fd = 0;  // STDIN_FILENO
  JsonRpcDispatcher::WriteFun write_fun = [](absl::string_view reply) {
    // Output formatting as header/body chunk as required by LSP spec.
    std::cout << "Content-Length: " << reply.size() << "\r\n\r\n";
    std::cout << reply << std::flush;
  };

  // We want the buffer size to be the largest message we could
  // receive. It typically would be in the order of largest file to
  // be opened (as it is sent verbatim in didOpen).
  // Should be chosen accordingly.
  MessageStreamSplitter stream_splitter;
  JsonRpcDispatcher dispatcher(write_fun);

  // All bodies the stream splitter extracts are pushed to the json dispatcher
  stream_splitter.SetMessageProcessor(
      [&dispatcher](absl::string_view /*header*/, absl::string_view body) {
        return dispatcher.DispatchMessage(body);
      });

  // The buffer collection keeps track of all the buffers opened in the editor.
  // It registers callbacks to receive the relevant events on the dispatcher.
  BufferCollection buffers(&dispatcher);
  verilog::BufferTrackerContainer parsed_buffers;

  // Exchange of capabilities.
  dispatcher.AddRequestHandler("initialize", InitializeServer);

  dispatcher.AddRequestHandler(
      "textDocument/codeAction",
      [&parsed_buffers](const verible::lsp::CodeActionParams &p)
          -> std::vector<verible::lsp::CodeAction> {
        const auto analyzed = parsed_buffers.GetCurrent(p.textDocument.uri);
        if (analyzed) {
          return GenerateLinterCodeActions(analyzed->parser(),
                                           analyzed->lint_result(), p);
        }
        return {};
      });
  dispatcher.AddRequestHandler(
      "textDocument/documentSymbol",
      [&parsed_buffers](const DocumentSymbolParams &p) -> nlohmann::json {
        const auto analyzed = parsed_buffers.GetLastGood(p.textDocument.uri);
        if (analyzed) return HandleDocumentSymbols(analyzed->parser(), p);
        return nlohmann::json::array();
      });

  // The client sends a request to shut down. Use that to exit our loop.
  bool shutdown_requested = false;
  dispatcher.AddRequestHandler("shutdown",
                               [&shutdown_requested](const nlohmann::json &) {
                                 shutdown_requested = true;
                                 return nullptr;
                               });

  buffers.SetChangeCallback([&parsed_buffers, &dispatcher](
                                const std::string &uri,
                                const EditTextBuffer *txt) {
    if (txt) {
      verilog::BufferTracker *const buffer = parsed_buffers.Update(uri, *txt);
      ConsiderSendDiagnostics(uri, buffer->current(), &dispatcher);
    } else {
      parsed_buffers.Remove(uri);
    }
  });

  absl::Status status = absl::OkStatus();
  while (status.ok() && !shutdown_requested) {
    status = stream_splitter.PullFrom([](char *buf, int size) -> int {  //
      return read(in_fd, buf, size);
    });
  }

  std::cerr << status.message() << std::endl;

  if (shutdown_requested) {
    std::cerr << "Shutting down due to shutdown request." << std::endl;
  }

  std::cerr << "Statistics" << std::endl;
  std::cerr << "Largest message seen: "
            << stream_splitter.StatLargestBodySeen() / 1024 << " kiB "
            << std::endl;
  for (const auto &stats : dispatcher.GetStatCounters()) {
    fprintf(stderr, "%30s %9d\n", stats.first.c_str(), stats.second);
  }
}
