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

#include "src/type/access_control_type.h"
#include "src/type/test_helper.h"
#include "src/type/texture_type.h"

namespace tint {
namespace type {
namespace {

using I32Test = TestHelper;

TEST_F(I32Test, Is) {
  I32 i;
  Type* ty = &i;
  EXPECT_FALSE(ty->Is<AccessControl>());
  EXPECT_FALSE(ty->Is<Alias>());
  EXPECT_FALSE(ty->Is<Array>());
  EXPECT_FALSE(ty->Is<Bool>());
  EXPECT_FALSE(ty->Is<F32>());
  EXPECT_TRUE(ty->Is<I32>());
  EXPECT_FALSE(ty->Is<Matrix>());
  EXPECT_FALSE(ty->Is<Pointer>());
  EXPECT_FALSE(ty->Is<Sampler>());
  EXPECT_FALSE(ty->Is<Struct>());
  EXPECT_FALSE(ty->Is<Texture>());
  EXPECT_FALSE(ty->Is<U32>());
  EXPECT_FALSE(ty->Is<Vector>());
}

TEST_F(I32Test, TypeName) {
  I32 i;
  EXPECT_EQ(i.type_name(), "__i32");
}

TEST_F(I32Test, FriendlyName) {
  I32 i;
  EXPECT_EQ(i.FriendlyName(Symbols()), "i32");
}

}  // namespace
}  // namespace type
}  // namespace tint
