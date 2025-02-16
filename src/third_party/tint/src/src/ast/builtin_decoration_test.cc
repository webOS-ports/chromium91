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
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied->
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/ast/constant_id_decoration.h"
#include "src/ast/test_helper.h"

namespace tint {
namespace ast {
namespace {

using BuiltinDecorationTest = TestHelper;

TEST_F(BuiltinDecorationTest, Creation) {
  auto* d = create<BuiltinDecoration>(Builtin::kFragDepth);
  EXPECT_EQ(Builtin::kFragDepth, d->value());
}

TEST_F(BuiltinDecorationTest, Is) {
  Decoration* d = create<BuiltinDecoration>(Builtin::kFragDepth);
  EXPECT_FALSE(d->Is<BindingDecoration>());
  EXPECT_TRUE(d->Is<BuiltinDecoration>());
  EXPECT_FALSE(d->Is<ConstantIdDecoration>());
  EXPECT_FALSE(d->Is<LocationDecoration>());
  EXPECT_FALSE(d->Is<GroupDecoration>());
}

TEST_F(BuiltinDecorationTest, ToStr) {
  auto* d = create<BuiltinDecoration>(Builtin::kFragDepth);
  EXPECT_EQ(str(d), R"(BuiltinDecoration{frag_depth}
)");
}

}  // namespace
}  // namespace ast
}  // namespace tint
