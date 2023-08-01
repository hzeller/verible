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

#include "verilog/tools/kythe/kythe_proto_output.h"

#include <iostream>

#include "verilog/tools/kythe/kythe_facts.h"
#include "verilog/tools/kythe/kythe_facts_extractor.h"
#include "verilog/tools/kythe/kzip_creator.h"

namespace verilog {
namespace kythe {
namespace {

// Returns the VName representation in Kythe's storage proto format.
kythe::proto::VName ConvertVnameToProto(const VName &vname) {
  proto::VName proto_vname;
  proto_vname.signature = vname.signature.ToString();
  proto_vname.corpus = std::string{vname.corpus};
  proto_vname.root = std::string{vname.root};
  proto_vname.path = std::string{vname.path};
  proto_vname.language = std::string{vname.language};
  return proto_vname;
}

// Returns the Fact representation in Kythe's storage proto format.
proto::Entry ConvertEdgeToEntry(const Edge &edge) {
  proto::Entry entry;
  entry.fact_name = "/";
  entry.edge_kind = std::string{edge.edge_name};
  entry.source = ConvertVnameToProto(edge.source_node);
  entry.target = ConvertVnameToProto(edge.target_node);
  return entry;
}

// Returns the Fact representation in Kythe's storage proto format.
proto::Entry ConvertFactToEntry(const Fact &fact) {
  proto::Entry entry;
  entry.fact_name = std::string{fact.fact_name};
  entry.fact_value = fact.fact_value;
  entry.source = ConvertVnameToProto(fact.node_vname);
  return entry;
}

// Output entry to the stream.
void OutputProto(const proto::Entry &entry /*,some stream*/) {
  // coded_stream.WriteVarint32(entry.ByteSizeLong());
  // entry.SerializeToCodedStream(&coded_stream);
}

}  // namespace

KytheProtoOutput::KytheProtoOutput(int fd) { /* wrap with whatever encoder */
}
KytheProtoOutput::~KytheProtoOutput() { /*fd.close()*/
}

void KytheProtoOutput::Emit(const Fact &fact) {
  OutputProto(ConvertFactToEntry(fact));
}
void KytheProtoOutput::Emit(const Edge &edge) {
  OutputProto(ConvertEdgeToEntry(edge));
}

}  // namespace kythe
}  // namespace verilog
