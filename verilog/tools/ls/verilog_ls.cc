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

// A super-simple dummy LSP without functionality except responding
// to initialize and shutdown as well as tracking file contents.
// This is merely to test that the json-rpc plumbing is working.

#include "common/lsp/json-rpc-dispatcher.h"
#include "common/lsp/lsp-protocol.h"
#include "common/lsp/lsp-text-buffer.h"
#include "common/lsp/message-stream-splitter.h"
#include "common/util/init_command_line.h"
#include "common/util/value_saver.h"
#include "verilog/analysis/verilog_analyzer.h"
#include "verilog/analysis/verilog_linter.h"
#include "verilog/parser/verilog_token_classifications.h"

// temp.
#include "verilog/CST/verilog_nonterminals.h"
using verilog::NodeEnumToString;

#include "verilog/parser/verilog_token.h"
using verilog::TokenTypeToString;

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
using verible::TokenInfo;
using verible::lsp::BufferCollection;
using verible::lsp::EditTextBuffer;
using verible::lsp::InitializeResult;
using verible::lsp::JsonRpcDispatcher;
using verible::lsp::MessageStreamSplitter;

using verible::lsp::DocumentSymbol;
using verible::lsp::DocumentSymbolParams;

using verilog::VerilogAnalyzer;

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
      {"codeActionProvider", true},
      {"documentSymbolProvider", true},
  };

  return result;
}

nlohmann::json ToJson(const TokenInfo& token_info,
                      const TokenInfo::Context& context, bool include_text) {
  nlohmann::json json(nlohmann::json::object());
  std::ostringstream stream;
  context.token_enum_translator(stream, token_info.token_enum());
  json["start"] = token_info.left(context.base);
  json["end"] = token_info.right(context.base);
  json["tag"] = stream.str();

  if (include_text) {
    json["text"] = std::string(token_info.text());
  }

  return json;
}

class DocumentSymbolFiller : public verible::SymbolVisitor {
 public:
  explicit DocumentSymbolFiller(absl::string_view base,
                                DocumentSymbol *toplevel)
    : context_(base,
               [](std::ostream& stream, int e) {
                 stream << e;
               }),
      line_column_map_(base),
      current_symbol_(toplevel) {}

  void Visit(const verible::SyntaxTreeLeaf& leaf) final {
#if 0
    const verilog_tokentype tokentype =
      static_cast<verilog_tokentype>(leaf.Tag().tag);
    absl::string_view type_str = TokenTypeToString(tokentype);
    // Don't include token's text for operators, keywords, or anything that is a
    // part of Verilog syntax. For such types, TokenTypeToString() is equal to
    // token's text. Exception has to be made for identifiers, because things like
    // "PP_Identifier" or "SymbolIdentifier" (which are values returned by
    // TokenTypeToString()) could be used as Verilog identifier.
    const bool include_text =
      verilog::IsIdentifierLike(tokentype) || (leaf.get().text() != type_str);
#endif
    const auto &token = leaf.get();
    current_symbol_->name = std::string(token.text());
    current_symbol_->kind = 7;  // replaceme Property.
    verible::LineColumn start = line_column_map_(token.left(context_.base));
    verible::LineColumn end = line_column_map_(token.right(context_.base));
    current_symbol_->range = {
      .start = {.line = start.line, .character = start.column},
      .end = {.line = end.line, .character = end.column},
    };
    current_symbol_->selectionRange = current_symbol_->range;
  }

  void Visit(const verible::SyntaxTreeNode& node) final {
    DocumentSymbol node_symbol;
    node_symbol.kind = 3;  // replaceme namespace
    node_symbol.name = NodeEnumToString(static_cast<verilog::NodeEnum>(node.Tag().tag));
    const verible::ValueSaver<DocumentSymbol*> value_saver(
      &current_symbol_, nullptr);
    for (const auto& child : node.children()) {
      if (!child) continue;
      DocumentSymbol child_symbol;
      current_symbol_ = &child_symbol;
      child->Accept(this);
      if (node_symbol.children == nullptr) {  // first non-null child.
        node_symbol.range.start = child_symbol.range.start;
        node_symbol.children = json::array();
      }
      node_symbol.children.push_back(child_symbol);
      node_symbol.range.end = child_symbol.range.end;  // keep track of last.
    }
  }

 protected:
  // Range of text spanned by syntax tree, used for offset calculation.
  const verible::TokenInfo::Context context_;

  const verible::LineColumnMap line_column_map_;
  DocumentSymbol *current_symbol_;
};

bool operator<(const verible::lsp::Position &a,
               const verible::lsp::Position &b) {
  if (a.line > b.line) return false;
  if (a.line < b.line) return true;
  return a.character < b.character;
}
bool operator==(const verible::lsp::Position &a,
                const verible::lsp::Position &b) {
  return a.line == b.line && a.character == b.character;
}
bool rangeOverlap(const verible::lsp::Range &a, const verible::lsp::Range &b) {
  return a.start == b.start || a.end == b.end ||
         (a.start < b.end && b.start < a.end);
}

class VersionedAnalyzedBuffer {
 public:
  VersionedAnalyzedBuffer(int64_t version, absl::string_view uri,
                          absl::string_view content)
      : version_(version),
        parser_(VerilogAnalyzer::AnalyzeAutomaticMode(content, uri)) {
    std::cerr << "Analyzed " << uri << " lex:" << parser_->LexStatus()
              << "; parser:" << parser_->ParseStatus() << std::endl;
    RunLinter(uri);  // TODO: we should use filename here
  }

  bool is_good() const {
    return parser_->LexStatus().ok() && parser_->ParseStatus().ok();
  }

  std::vector<verible::lsp::Diagnostic> GetDiagnostics() const {
    // TODO: files that generate a lot of messages will create a huge
    // output. So we limit the messages here.
    // However, we should work towards emitting them around the last known
    // edit point in the document as this is what the user sees.
    static constexpr int kMaxMessages = 100;
    const auto &rejected_tokens = parser_->GetRejectedTokens();
    auto const &lint_violations = verilog::GetSortedViolations(lint_statuses_);
    std::vector<verible::lsp::Diagnostic> result;
    int remaining = rejected_tokens.size() + lint_violations.size();
    if (remaining > kMaxMessages) remaining = kMaxMessages;
    result.reserve(remaining);
    for (const auto &rejected_token : rejected_tokens) {
      parser_->ExtractLinterTokenErrorDetail(
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

    const absl::string_view base = parser_->Data().Contents();
    verible::LineColumnMap line_column_map(base);
    for (const auto &v : lint_violations) {
      result.emplace_back(ViolationToDiagnostic(v, base, line_column_map));
      if (--remaining <= 0) break;
    }
    return result;
  }

  std::vector<verible::lsp::CodeAction> HandleCodeAction(
      const verible::lsp::CodeActionParams &p) const {
    auto const &lint_violations = verilog::GetSortedViolations(lint_statuses_);
    if (lint_violations.empty()) return {};

    const absl::string_view base = parser_->Data().Contents();
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

  std::vector<DocumentSymbol> HandleDocumentSymbols(
    const DocumentSymbolParams &p) const {
    std::vector<DocumentSymbol> result;
    const absl::string_view base = parser_->Data().Contents();
    DocumentSymbolFiller filler(base, &result.emplace_back());
    const auto& text_structure = parser_->Data();
    const auto& syntax_tree = text_structure.SyntaxTree();
    syntax_tree->Accept(&filler);
    return result;
  }

 private:
  void RunLinter(absl::string_view filename) {
    const auto &text_structure = parser_->Data();
    verilog::LinterConfiguration config;  // TODO: read from project context
    verilog::RuleBundle bundle;
    auto status = config.ConfigureFromOptions(verilog::LinterOptions{
        .ruleset = verilog::RuleSet::kAll,
        .rules = bundle,
    });
    if (!status.ok()) {
      std::cerr << "Got an issue with the lint configuration" << std::endl;
      return;
    }
    const bool show_context = true;  // ? needed
    const auto linter_result = VerilogLintTextStructure(
        filename, config, text_structure, show_context);
    if (!linter_result.ok()) {
      return;
    }

    lint_statuses_ = linter_result.value();
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

  const int64_t version_;
  std::unique_ptr<VerilogAnalyzer> parser_;
  std::vector<verible::LintRuleStatus> lint_statuses_;
};

class BufferTracker {
 public:
  void Update(const std::string &filename, const EditTextBuffer &txt) {
    // TODO: remove file:// prefix.
    txt.RequestContent([&txt, &filename, this](absl::string_view content) {
      current_ = std::make_shared<VersionedAnalyzedBuffer>(
          txt.last_global_version(), filename, content);
    });
    if (current_->is_good()) {
      last_good_ = current_;
    }
  }

  const VersionedAnalyzedBuffer *current() const { return current_.get(); }

 private:
  std::shared_ptr<VersionedAnalyzedBuffer> current_;
  std::shared_ptr<VersionedAnalyzedBuffer> last_good_;
};

class ParsedBufferContainer {
 public:
  BufferTracker *Update(const std::string &filename,
                        const EditTextBuffer &txt) {
    auto inserted = buffers_.insert({filename, nullptr});
    if (inserted.second) {
      inserted.first->second.reset(new BufferTracker());
    }
    inserted.first->second->Update(filename, txt);
    return inserted.first->second.get();
  }

  const VersionedAnalyzedBuffer *GetCurrent(const std::string &uri) {
    auto found = buffers_.find(uri);
    if (found == buffers_.end()) {
      std::cerr << "Did not find " << uri << std::endl;
      return nullptr;
    }
    return found->second->current();
  }

 private:
  std::unordered_map<std::string, std::unique_ptr<BufferTracker>> buffers_;
};

void ConsiderSendDiagnostics(const std::string &uri,
                             const VersionedAnalyzedBuffer *buffer,
                             JsonRpcDispatcher *dispatcher) {
  verible::lsp::PublishDiagnosticsParams params;
  params.uri = uri;
  params.diagnostics = buffer->GetDiagnostics();
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
  ParsedBufferContainer parsed_buffers;

  // Exchange of capabilities.
  dispatcher.AddRequestHandler("initialize", InitializeServer);

  dispatcher.AddRequestHandler(
      "textDocument/codeAction",
      [&parsed_buffers](const verible::lsp::CodeActionParams &p)
          -> std::vector<verible::lsp::CodeAction> {
        const auto analyzed = parsed_buffers.GetCurrent(p.textDocument.uri);
        if (analyzed) return analyzed->HandleCodeAction(p);
        return {};
      });
  dispatcher.AddRequestHandler(
      "textDocument/documentSymbol",
      [&parsed_buffers](
          const DocumentSymbolParams &p) -> std::vector<DocumentSymbol> {
        const auto analyzed = parsed_buffers.GetCurrent(p.textDocument.uri);
        if (analyzed) return analyzed->HandleDocumentSymbols(p);
        return {};
      });

      // The client sends a request to shut down. Use that to exit our loop.
      bool shutdown_requested = false;
  dispatcher.AddRequestHandler("shutdown",
                               [&shutdown_requested](const nlohmann::json &) {
                                 shutdown_requested = true;
                                 return nullptr;
                               });

  int64_t last_updated_version = 0;
  buffers.SetOpenCallback(
    [&parsed_buffers, &last_updated_version](const std::string &uri,
                                             const EditTextBuffer &txt) {
      parsed_buffers.Update(uri, txt);
      last_updated_version = txt.last_global_version();
    });

  absl::Status status = absl::OkStatus();
  while (status.ok() && !shutdown_requested) {
    status = stream_splitter.PullFrom([](char *buf, int size) -> int {  //
      return read(in_fd, buf, size);
    });

    // TODO: this should work async.
    buffers.MapBuffersChangedSince(
        last_updated_version,
        [&parsed_buffers, &dispatcher](const std::string &uri,
                                       const EditTextBuffer &txt) {
          BufferTracker *const buffer = parsed_buffers.Update(uri, txt);
          ConsiderSendDiagnostics(uri, buffer->current(), &dispatcher);
        });
    last_updated_version = buffers.global_version();
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
