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

#ifndef SRC_AST_BITCAST_EXPRESSION_H_
#define SRC_AST_BITCAST_EXPRESSION_H_

#include "src/ast/expression.h"

namespace tint {
namespace ast {

/// A bitcast expression
class BitcastExpression : public Castable<BitcastExpression, Expression> {
 public:
  /// Constructor
  /// @param source the bitcast expression source
  /// @param type the type
  /// @param expr the expr
  BitcastExpression(const Source& source, type::Type* type, Expression* expr);
  /// Move constructor
  BitcastExpression(BitcastExpression&&);
  ~BitcastExpression() override;

  /// @returns the left side expression
  type::Type* type() const { return type_; }
  /// @returns the expression
  Expression* expr() const { return expr_; }

  /// Clones this node and all transitive child nodes using the `CloneContext`
  /// `ctx`.
  /// @param ctx the clone context
  /// @return the newly cloned node
  BitcastExpression* Clone(CloneContext* ctx) const override;

  /// Writes a representation of the node to the output stream
  /// @param sem the semantic info for the program
  /// @param out the stream to write to
  /// @param indent number of spaces to indent the node when writing
  void to_str(const semantic::Info& sem,
              std::ostream& out,
              size_t indent) const override;

 private:
  BitcastExpression(const BitcastExpression&) = delete;

  type::Type* const type_;
  Expression* const expr_;
};

}  // namespace ast
}  // namespace tint

#endif  // SRC_AST_BITCAST_EXPRESSION_H_
