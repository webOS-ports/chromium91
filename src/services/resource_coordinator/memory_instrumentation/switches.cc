// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/resource_coordinator/memory_instrumentation/switches.h"

namespace memory_instrumentation {
namespace switches {

// Disable the tracing service graph compuation while writing the trace.
const char kDisableChromeTracingComputation[] =
    "disable-chrome-tracing-computation";
const char kUseMemoryTrackingJsonWriter[] = "use-memory-tracking-json-writer";

}  // namespace switches
}  // namespace memory_instrumentation
