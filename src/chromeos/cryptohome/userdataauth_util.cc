// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/cryptohome/userdataauth_util.h"

#include "base/logging.h"
#include "base/notreached.h"
#include "chromeos/cryptohome/cryptohome_util.h"
#include "components/device_event_log/device_event_log.h"

using google::protobuf::RepeatedPtrField;

namespace user_data_auth {

namespace {

template <typename ReplyType>
bool IsEmpty(const base::Optional<ReplyType>& reply) {
  if (!reply.has_value()) {
    LOGIN_LOG(ERROR) << "Cryptohome call failed with empty reply.";
    return true;
  }
  return false;
}

}  // namespace

template <typename ReplyType>
cryptohome::MountError ReplyToMountError(
    const base::Optional<ReplyType>& reply) {
  if (IsEmpty(reply)) {
    return cryptohome::MOUNT_ERROR_FATAL;
  }

  return CryptohomeErrorToMountError(reply->error());
}

// Instantiate ReplyToMountError and export them for types actually used.
template COMPONENT_EXPORT(CHROMEOS_CRYPTOHOME) cryptohome::MountError
    ReplyToMountError(const base::Optional<RemoveReply>&);
template COMPONENT_EXPORT(CHROMEOS_CRYPTOHOME) cryptohome::MountError
    ReplyToMountError(const base::Optional<UnmountReply>&);
template COMPONENT_EXPORT(CHROMEOS_CRYPTOHOME) cryptohome::MountError
    ReplyToMountError(const base::Optional<RenameReply>&);
template COMPONENT_EXPORT(CHROMEOS_CRYPTOHOME) cryptohome::MountError
    ReplyToMountError(const base::Optional<MountReply>&);
template COMPONENT_EXPORT(CHROMEOS_CRYPTOHOME) cryptohome::MountError
    ReplyToMountError(const base::Optional<MigrateKeyReply>&);
template COMPONENT_EXPORT(CHROMEOS_CRYPTOHOME) cryptohome::MountError
    ReplyToMountError(const base::Optional<GetKeyDataReply>&);

std::vector<cryptohome::KeyDefinition> GetKeyDataReplyToKeyDefinitions(
    const base::Optional<GetKeyDataReply>& reply) {
  const RepeatedPtrField<cryptohome::KeyData>& key_data = reply->key_data();
  return cryptohome::RepeatedKeyDataToKeyDefinitions(key_data);
}

int64_t AccountDiskUsageReplyToUsageSize(
    const base::Optional<GetAccountDiskUsageReply>& reply) {
  if (IsEmpty(reply))
    return -1;

  if (reply->error() != CRYPTOHOME_ERROR_NOT_SET) {
    LOGIN_LOG(ERROR) << "GetAccountDiskUsage failed with error: "
                     << reply->error();
    return -1;
  }

  return reply->size();
}

// TODO(crbug.com/797848): Finish testing this method.
cryptohome::MountError CryptohomeErrorToMountError(CryptohomeErrorCode code) {
  switch (code) {
    case CRYPTOHOME_ERROR_NOT_SET:
      return cryptohome::MOUNT_ERROR_NONE;
    case CRYPTOHOME_ERROR_ACCOUNT_NOT_FOUND:
      return cryptohome::MOUNT_ERROR_USER_DOES_NOT_EXIST;
    case CRYPTOHOME_ERROR_NOT_IMPLEMENTED:
    case CRYPTOHOME_ERROR_MOUNT_FATAL:
    case CRYPTOHOME_ERROR_KEY_QUOTA_EXCEEDED:
    case CRYPTOHOME_ERROR_BACKING_STORE_FAILURE:
    case CRYPTOHOME_ERROR_INSTALL_ATTRIBUTES_FINALIZE_FAILED:
    case CRYPTOHOME_ERROR_INSTALL_ATTRIBUTES_GET_FAILED:
    case CRYPTOHOME_ERROR_INSTALL_ATTRIBUTES_SET_FAILED:
    case CRYPTOHOME_ERROR_INVALID_ARGUMENT:
    case CRYPTOHOME_ERROR_FINGERPRINT_ERROR_INTERNAL:
    case CRYPTOHOME_ERROR_FINGERPRINT_RETRY_REQUIRED:
    case CRYPTOHOME_ERROR_FINGERPRINT_DENIED:
      return cryptohome::MOUNT_ERROR_FATAL;
    case CRYPTOHOME_ERROR_AUTHORIZATION_KEY_NOT_FOUND:
    case CRYPTOHOME_ERROR_KEY_NOT_FOUND:
    case CRYPTOHOME_ERROR_MIGRATE_KEY_FAILED:
    case CRYPTOHOME_ERROR_AUTHORIZATION_KEY_FAILED:
      return cryptohome::MOUNT_ERROR_KEY_FAILURE;
    case CRYPTOHOME_ERROR_TPM_COMM_ERROR:
      return cryptohome::MOUNT_ERROR_TPM_COMM_ERROR;
    case CRYPTOHOME_ERROR_TPM_DEFEND_LOCK:
      return cryptohome::MOUNT_ERROR_TPM_DEFEND_LOCK;
    case CRYPTOHOME_ERROR_MOUNT_MOUNT_POINT_BUSY:
      return cryptohome::MOUNT_ERROR_MOUNT_POINT_BUSY;
    case CRYPTOHOME_ERROR_TPM_NEEDS_REBOOT:
      return cryptohome::MOUNT_ERROR_TPM_NEEDS_REBOOT;
    case CRYPTOHOME_ERROR_AUTHORIZATION_KEY_DENIED:
    case CRYPTOHOME_ERROR_KEY_LABEL_EXISTS:
    case CRYPTOHOME_ERROR_UPDATE_SIGNATURE_INVALID:
      return cryptohome::MOUNT_ERROR_KEY_FAILURE;
    case CRYPTOHOME_ERROR_MOUNT_OLD_ENCRYPTION:
      return cryptohome::MOUNT_ERROR_OLD_ENCRYPTION;
    case CRYPTOHOME_ERROR_MOUNT_PREVIOUS_MIGRATION_INCOMPLETE:
      return cryptohome::MOUNT_ERROR_PREVIOUS_MIGRATION_INCOMPLETE;
    case CRYPTOHOME_ERROR_REMOVE_FAILED:
      return cryptohome::MOUNT_ERROR_REMOVE_FAILED;
    case CRYPTOHOME_ERROR_TPM_UPDATE_REQUIRED:
      return cryptohome::MOUNT_ERROR_TPM_UPDATE_REQUIRED;
    case CRYPTOHOME_ERROR_VAULT_UNRECOVERABLE:
      return cryptohome::MOUNT_ERROR_VAULT_UNRECOVERABLE;
    // TODO(crbug.com/797563): Split the error space and/or handle everything.
    case CRYPTOHOME_ERROR_LOCKBOX_SIGNATURE_INVALID:
    case CRYPTOHOME_ERROR_LOCKBOX_CANNOT_SIGN:
    case CRYPTOHOME_ERROR_BOOT_ATTRIBUTE_NOT_FOUND:
    case CRYPTOHOME_ERROR_BOOT_ATTRIBUTES_CANNOT_SIGN:
    case CRYPTOHOME_ERROR_TPM_EK_NOT_AVAILABLE:
    case CRYPTOHOME_ERROR_ATTESTATION_NOT_READY:
    case CRYPTOHOME_ERROR_CANNOT_CONNECT_TO_CA:
    case CRYPTOHOME_ERROR_CA_REFUSED_ENROLLMENT:
    case CRYPTOHOME_ERROR_CA_REFUSED_CERTIFICATE:
    case CRYPTOHOME_ERROR_INTERNAL_ATTESTATION_ERROR:
    case CRYPTOHOME_ERROR_FIRMWARE_MANAGEMENT_PARAMETERS_INVALID:
    case CRYPTOHOME_ERROR_FIRMWARE_MANAGEMENT_PARAMETERS_CANNOT_STORE:
    case CRYPTOHOME_ERROR_FIRMWARE_MANAGEMENT_PARAMETERS_CANNOT_REMOVE:
    case CRYPTOHOME_ERROR_UPDATE_USER_ACTIVITY_TIMESTAMP_FAILED:
    case CRYPTOHOME_ERROR_FAILED_TO_EXTEND_PCR:
    case CRYPTOHOME_ERROR_FAILED_TO_READ_PCR:
    case CRYPTOHOME_ERROR_PCR_ALREADY_EXTENDED:
      NOTREACHED();
      return cryptohome::MOUNT_ERROR_FATAL;
    // TODO(dlunev): remove this temporary case after rolling up system api
    // change and adding proper handling for the new enum value in
    // https://chromium-review.googlesource.com/c/chromium/src/+/2518524
    default:
      NOTREACHED();
      return cryptohome::MOUNT_ERROR_FATAL;
  }
}

}  // namespace user_data_auth
