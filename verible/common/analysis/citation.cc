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

#include "verible/common/analysis/citation.h"

#include <string>
#include <string_view>

#include "absl/strings/str_cat.h"

namespace verible {
std::string GetStyleGuideCitation(std::string_view topic) {
  return absl::StrCat("[Style: ", topic, "]");
}
}  // namespace verible
