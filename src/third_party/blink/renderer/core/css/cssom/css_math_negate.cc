// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/css/cssom/css_math_negate.h"

#include "third_party/blink/renderer/core/css/css_math_expression_node.h"
#include "third_party/blink/renderer/core/css/cssom/css_numeric_sum_value.h"
#include "third_party/blink/renderer/platform/wtf/text/string_builder.h"

namespace blink {

base::Optional<CSSNumericSumValue> CSSMathNegate::SumValue() const {
  auto maybe_sum = value_->SumValue();
  if (!maybe_sum)
    return base::nullopt;

  std::for_each(maybe_sum->terms.begin(), maybe_sum->terms.end(),
                [](auto& term) { term.value *= -1; });
  return maybe_sum;
}

void CSSMathNegate::BuildCSSText(Nested nested,
                                 ParenLess paren_less,
                                 StringBuilder& result) const {
  if (paren_less == ParenLess::kNo)
    result.Append(nested == Nested::kYes ? "(" : "calc(");

  result.Append("-");
  value_->BuildCSSText(Nested::kYes, ParenLess::kNo, result);

  if (paren_less == ParenLess::kNo)
    result.Append(")");
}

CSSMathExpressionNode* CSSMathNegate::ToCalcExpressionNode() const {
  CSSMathExpressionNode* right_side = value_->ToCalcExpressionNode();
  if (!right_side)
    return nullptr;
  return CSSMathExpressionBinaryOperation::CreateSimplified(
      CSSMathExpressionNumericLiteral::Create(
          -1, CSSPrimitiveValue::UnitType::kNumber, false),
      right_side, CSSMathOperator::kMultiply);
}

}  // namespace blink
