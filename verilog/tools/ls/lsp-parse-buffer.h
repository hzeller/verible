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

#ifndef VERILOG_TOOLS_LS_LSP_PARSE_BUFFER_H
#define VERILOG_TOOLS_LS_LSP_PARSE_BUFFER_H

#include <cstdint>

#include "absl/status/status.h"
#include "common/lsp/lsp-text-buffer.h"
#include "verilog/analysis/verilog_analyzer.h"
#include "verilog/analysis/verilog_linter.h"

namespace verilog {
// A parsed buffer collects all the artifacts generated from a text buffer
// from parsing or running the linter.
////
// Right now, the ParsedBuffer is synchronously filling its internal structure
// on construction, but plan is to do that on-demand and possibly with
// std::future<>s evaluated in separate threads.
class ParsedBuffer {
 public:
  ParsedBuffer(int64_t version, absl::string_view uri,
               absl::string_view content)
      : version_(version),
        parser_(verilog::VerilogAnalyzer::AnalyzeAutomaticMode(content, uri)) {
    std::cerr << "Analyzed " << uri << " lex:" << parser_->LexStatus()
              << "; parser:" << parser_->ParseStatus() << std::endl;
    // TODO: we should use a filename not URI
    if (const auto &lint_result = RunLinter(uri, *parser_); lint_result.ok()) {
      lint_statuses_ = lint_result.value();
    }
  }

  bool parsed_successfully() const {
    return parser_->LexStatus().ok() && parser_->ParseStatus().ok();
  }

  const verilog::VerilogAnalyzer &parser() const { return *parser_; }
  const std::vector<verible::LintRuleStatus> &lint_result() const {
    return lint_statuses_;
  }

  int64_t version() const { return version_; }

 private:
  static absl::StatusOr<std::vector<verible::LintRuleStatus>> RunLinter(
      absl::string_view filename, const verilog::VerilogAnalyzer &parser) {
    const auto &text_structure = parser.Data();
    verilog::LinterConfiguration config;  // TODO: read from project context
    verilog::RuleBundle bundle;
    auto status = config.ConfigureFromOptions(verilog::LinterOptions{
        .ruleset = verilog::RuleSet::kAll,
        .rules = bundle,
    });
    if (!status.ok()) {
      std::cerr << "Got an issue with the lint configuration" << std::endl;
      return status;
    }
    return VerilogLintTextStructure(filename, config, text_structure);
  }

  const int64_t version_;
  std::unique_ptr<verilog::VerilogAnalyzer> parser_;
  std::vector<verible::LintRuleStatus> lint_statuses_;
};

// A buffer tracker tracks the EditTextBuffer content and keeps up to
// two versions of ParsedBuffers - the latest, that might have parse errors
// and the last good (if available).
class BufferTracker {
 public:
  void Update(const std::string &filename,
              const verible::lsp::EditTextBuffer &txt) {
    // TODO: remove file:// prefix.
    if (current_ && current_->version() == txt.last_global_version())
      return;  // Nothing to do (we don't really expect this to happen)
    txt.RequestContent([&txt, &filename, this](absl::string_view content) {
      current_ = std::make_shared<ParsedBuffer>(txt.last_global_version(),
                                                filename, content);
    });
    if (current_->parsed_successfully()) {
      last_good_ = current_;
    }
  }

  const ParsedBuffer *current() const { return current_.get(); }
  const ParsedBuffer *last_good() const { return last_good_.get(); }

 private:
  std::shared_ptr<ParsedBuffer> current_;
  std::shared_ptr<ParsedBuffer> last_good_;
};

// Container holding all buffer trackers keyed by file uri.
// This is the correspondent to verible::lsp::BufferCollection that
// internally stores
class BufferTrackerContainer {
 public:
  // Return a callback that allows to subscribe to an lsp::BufferCollection
  verible::lsp::BufferCollection::UriBufferCallback GetSubscriptionCallback() {
    return [this](const std::string &uri,
                  const verible::lsp::EditTextBuffer *txt) {
      if (txt) {
        const BufferTracker *tracker = Update(uri, *txt);
        // Now inform our listeners.
        if (change_listener_) change_listener_(uri, *tracker);
      } else {
        Remove(uri);
      }
    };
  }

  using ChangeCallback =
      std::function<void(const std::string &uri, const BufferTracker &tracker)>;
  void SetChangeListener(const ChangeCallback &cb) { change_listener_ = cb; }

  // Get the current ParsedBuffer for a given "uri" if available, i.e.
  // if the "uri" had been registered with the container.
  const ParsedBuffer *GetCurrent(const std::string &uri) {
    if (const auto buffer = FindBufferTrackerOrNull(uri); buffer != nullptr) {
      return buffer->current();
    }
    return nullptr;
  }

  // Get the last good ParsedBuffer for the given "uri" if available, i.e.
  // if the "uri" has been registered with the container _and_ at least
  // one of the updates contained a valid parseable content.
  const ParsedBuffer *GetLastGood(const std::string &uri) {
    if (const auto buffer = FindBufferTrackerOrNull(uri); buffer != nullptr) {
      return buffer->last_good();
    }
    return nullptr;
  }

 private:
  // Update internal state of the given "uri" with the content of the text
  // buffer. Return the buffer tracker.
  BufferTracker *Update(const std::string &uri,
                        const verible::lsp::EditTextBuffer &txt) {
    auto inserted = buffers_.insert({uri, nullptr});
    if (inserted.second) {
      inserted.first->second.reset(new BufferTracker());
    }
    inserted.first->second->Update(uri, txt);
    return inserted.first->second.get();
  }

  // Remove the buffer tracker for the given "uri".
  void Remove(const std::string &uri) { buffers_.erase(uri); }

  BufferTracker *FindBufferTrackerOrNull(const std::string &uri) {
    auto found = buffers_.find(uri);
    if (found == buffers_.end()) {
      std::cerr << "Did not find " << uri << std::endl;
      return nullptr;
    }
    return found->second.get();
  }

  ChangeCallback change_listener_ = nullptr;
  std::unordered_map<std::string, std::unique_ptr<BufferTracker>> buffers_;
};
}  // namespace verilog
#endif  // VERILOG_TOOLS_LS_LSP_PARSE_BUFFER_H
