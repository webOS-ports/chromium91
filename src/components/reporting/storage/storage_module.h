// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_REPORTING_STORAGE_STORAGE_MODULE_H_
#define COMPONENTS_REPORTING_STORAGE_STORAGE_MODULE_H_

#include <utility>

#include "base/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "components/reporting/encryption/encryption_module_interface.h"
#include "components/reporting/proto/record.pb.h"
#include "components/reporting/proto/record_constants.pb.h"
#include "components/reporting/storage/storage.h"
#include "components/reporting/storage/storage_configuration.h"
#include "components/reporting/storage/storage_module_interface.h"
#include "components/reporting/storage/storage_uploader_interface.h"
#include "components/reporting/util/status.h"
#include "components/reporting/util/statusor.h"

namespace reporting {

class StorageModule : public StorageModuleInterface {
 public:
  // Factory method creates |StorageModule| object.
  static void Create(
      const StorageOptions& options,
      UploaderInterface::AsyncStartUploaderCb async_start_upload_cb,
      scoped_refptr<EncryptionModuleInterface> encryption_module,
      base::OnceCallback<void(StatusOr<scoped_refptr<StorageModuleInterface>>)>
          callback);

  StorageModule(const StorageModule& other) = delete;
  StorageModule& operator=(const StorageModule& other) = delete;

  // AddRecord will add |record| (taking ownership) to the |StorageModule|
  // according to the provided |priority|. On completion, |callback| will be
  // called.
  void AddRecord(Priority priority,
                 Record record,
                 base::OnceCallback<void(Status)> callback) override;

  // Initiates upload of collected records according to the priority.
  // Called usually for a queue with an infinite or very large upload period.
  // Multiple |Flush| calls can safely run in parallel.
  // Returns error if cannot start upload.
  void Flush(Priority priority,
             base::OnceCallback<void(Status)> callback) override;

  // Once a record has been successfully uploaded, the sequencing information
  // can be passed back to the StorageModule here for record deletion.
  // If |force| is false (which is used in most cases), |sequencing_information|
  // only affects Storage if no higher sequeincing was confirmed before;
  // otherwise it is accepted unconditionally.
  void ReportSuccess(SequencingInformation sequencing_information,
                     bool force) override;

  // If the server attached signed encryption key to the response, it needs to
  // be paased here.
  void UpdateEncryptionKey(SignedEncryptionInfo signed_encryption_key) override;

 protected:
  // Constructor can only be called by |Create| factory method.
  StorageModule();

  // Refcounted object must have destructor declared protected or private.
  ~StorageModule() override;

 private:
  friend base::RefCountedThreadSafe<StorageModule>;

  // Storage backend (currently only Storage).
  // TODO(b/160334561): make it a pluggable interface.
  scoped_refptr<Storage> storage_;
};

}  // namespace reporting

#endif  // COMPONENTS_REPORTING_STORAGE_STORAGE_MODULE_H_
