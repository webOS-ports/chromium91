// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/services/ime/mock_input_channel.h"

namespace chromeos {
namespace ime {

MockInputChannel::MockInputChannel() : receiver_(this) {}

MockInputChannel::~MockInputChannel() = default;

mojo::PendingRemote<mojom::InputChannel>
MockInputChannel::CreatePendingRemote() {
  return receiver_.BindNewPipeAndPassRemote();
}

bool MockInputChannel::IsBound() const {
  return receiver_.is_bound();
}

void MockInputChannel::FlushForTesting() {
  return receiver_.FlushForTesting();
}

}  // namespace ime
}  // namespace chromeos
