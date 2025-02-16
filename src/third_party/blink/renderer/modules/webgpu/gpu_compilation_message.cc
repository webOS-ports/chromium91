// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/webgpu/gpu_compilation_message.h"

namespace blink {

GPUCompilationMessage::GPUCompilationMessage(String message,
                                             WGPUCompilationMessageType type,
                                             uint64_t line_num,
                                             uint64_t line_pos)
    : message_(message), line_num_(line_num), line_pos_(line_pos) {
  switch (type) {
    case WGPUCompilationMessageType_Error:
      type_string_ = "error";
      break;
    case WGPUCompilationMessageType_Warning:
      type_string_ = "warning";
      break;
    case WGPUCompilationMessageType_Info:
      type_string_ = "info";
      break;
    default:
      NOTREACHED();
      break;
  }
}

}  // namespace blink
