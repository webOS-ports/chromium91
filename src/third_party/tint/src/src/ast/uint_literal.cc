// Copyright 2020 The Tint Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/ast/uint_literal.h"

#include "src/program_builder.h"

TINT_INSTANTIATE_TYPEINFO(tint::ast::UintLiteral);

namespace tint {
namespace ast {

UintLiteral::UintLiteral(const Source& source, type::Type* type, uint32_t value)
    : Base(source, type, value) {}

UintLiteral::~UintLiteral() = default;

std::string UintLiteral::to_str(const semantic::Info&) const {
  return std::to_string(value());
}

std::string UintLiteral::name() const {
  return "__uint" + std::to_string(value());
}

UintLiteral* UintLiteral::Clone(CloneContext* ctx) const {
  // Clone arguments outside of create() call to have deterministic ordering
  auto src = ctx->Clone(source());
  auto* ty = ctx->Clone(type());
  return ctx->dst->create<UintLiteral>(src, ty, value());
}

}  // namespace ast
}  // namespace tint
