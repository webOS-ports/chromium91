// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "chromeos/dbus/dlp/dlp_client.h"

#include <utility>

#include "base/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/optional.h"
#include "base/strings/strcat.h"
#include "base/threading/thread_task_runner_handle.h"
#include "chromeos/dbus/dlp/fake_dlp_client.h"
#include "dbus/bus.h"
#include "dbus/message.h"
#include "dbus/object_proxy.h"
#include "third_party/cros_system_api/dbus/dlp/dbus-constants.h"

namespace chromeos {
namespace {

DlpClient* g_instance = nullptr;

const char kDbusCallFailure[] = "Failed to call dlp.";
const char kProtoMessageParsingFailure[] =
    "Failed to parse response message from dlp.";

// Tries to parse a proto message from |response| into |proto| and returns null
// if successful. If |response| is nullptr or the message cannot be parsed it
// will return an appropriate error message.
const char* DeserializeProto(dbus::Response* response,
                             google::protobuf::MessageLite* proto) {
  if (!response)
    return kDbusCallFailure;

  dbus::MessageReader reader(response);
  if (!reader.PopArrayOfBytesAsProto(proto))
    return kProtoMessageParsingFailure;

  return nullptr;
}

// "Real" implementation of DlpClient talking to the Dlp daemon
// on the Chrome OS side.
class DlpClientImpl : public DlpClient {
 public:
  DlpClientImpl() = default;
  DlpClientImpl(const DlpClientImpl&) = delete;
  DlpClientImpl& operator=(const DlpClientImpl&) = delete;
  ~DlpClientImpl() override = default;

  void Init(dbus::Bus* bus) {
    proxy_ = bus->GetObjectProxy(dlp::kDlpServiceName,
                                 dbus::ObjectPath(dlp::kDlpServicePath));
  }

  void SetDlpFilesPolicy(const dlp::SetDlpFilesPolicyRequest request,
                         SetDlpFilesPolicyCallback callback) override {
    dbus::MethodCall method_call(dlp::kDlpInterface,
                                 dlp::kSetDlpFilesPolicyMethod);
    dbus::MessageWriter writer(&method_call);

    if (!writer.AppendProtoAsArrayOfBytes(request)) {
      dlp::SetDlpFilesPolicyResponse response;
      response.set_error_message(base::StrCat(
          {"Failure to call d-bus method: ", dlp::kSetDlpFilesPolicyMethod}));
      base::ThreadTaskRunnerHandle::Get()->PostTask(
          FROM_HERE, base::BindOnce(std::move(callback), response));
      return;
    }

    proxy_->CallMethod(
        &method_call, dbus::ObjectProxy::TIMEOUT_USE_DEFAULT,
        base::BindOnce(&DlpClientImpl::HandleSetDlpFilesPolicyResponse,
                       weak_factory_.GetWeakPtr(), std::move(callback)));
  }

 private:
  TestInterface* GetTestInterface() override { return nullptr; }

  void HandleSetDlpFilesPolicyResponse(SetDlpFilesPolicyCallback callback,
                                       dbus::Response* response) {
    dlp::SetDlpFilesPolicyResponse response_proto;
    const char* error_message = DeserializeProto(response, &response_proto);
    if (error_message) {
      response_proto.set_error_message(error_message);
    }
    std::move(callback).Run(response_proto);
  }

  // D-Bus proxy for the Dlp daemon, not owned.
  dbus::ObjectProxy* proxy_ = nullptr;

  base::WeakPtrFactory<DlpClientImpl> weak_factory_{this};
};

}  // namespace

DlpClient::DlpClient() {
  CHECK(!g_instance);
  g_instance = this;
}

DlpClient::~DlpClient() {
  CHECK_EQ(this, g_instance);
  g_instance = nullptr;
}

// static
void DlpClient::Initialize(dbus::Bus* bus) {
  CHECK(bus);
  (new DlpClientImpl())->Init(bus);
}

// static
void DlpClient::InitializeFake() {
  new FakeDlpClient();
}

// static
void DlpClient::Shutdown() {
  CHECK(g_instance);
  delete g_instance;
}

// static
DlpClient* DlpClient::Get() {
  return g_instance;
}

}  // namespace chromeos
