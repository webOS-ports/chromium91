// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/password_manager/core/browser/leak_detection/encryption_utils.h"

#include <climits>
#include <utility>

#include "base/optional.h"
#include "base/stl_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_piece.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "crypto/openssl_util.h"
#include "crypto/sha2.h"
#include "third_party/boringssl/src/include/openssl/evp.h"
#include "third_party/boringssl/src/include/openssl/nid.h"
#include "third_party/private-join-and-compute/src/crypto/ec_commutative_cipher.h"

namespace password_manager {

namespace {

template <typename CharT>
std::basic_string<CharT> CanonicalizeUsernameT(
    base::BasicStringPiece<CharT> username) {
  static constexpr CharT kPeriod = '.';

  std::basic_string<CharT> email_lower = base::ToLowerASCII(username);
  // |email_lower| might be an email address. Strip off the mail-address host,
  // remove periods from the username and return the result.
  std::basic_string<CharT> user_lower =
      email_lower.substr(0, email_lower.find_last_of('@'));
  base::RemoveChars(user_lower, {&kPeriod, 1}, &user_lower);
  return user_lower;
}

}  // namespace

std::string CanonicalizeUsername(base::StringPiece username) {
  return CanonicalizeUsernameT(username);
}

std::u16string CanonicalizeUsername(base::StringPiece16 username) {
  return CanonicalizeUsernameT(username);
}

std::string HashUsername(base::StringPiece canonicalized_username) {
  // Needs to stay in sync with server side constant: go/passwords-leak-salts
  static constexpr char kUsernameSalt[] = {
      -60, -108, -93, -107, -8, -64, -30, 62,   -87, 35,  4,
      120, 112,  44,  114,  24, 86,  84,  -103, -77, -23, 33,
      24,  108,  33,  26,   1,  34,  60,  69,   74,  -6};

  // Check that |canonicalized_username| is actually canonicalized.
  // Note: We can't use CanonicalizeUsername() again, since it's not idempotent
  // if multiple '@' signs are present in the initial username.
  DCHECK_EQ(base::ToLowerASCII(canonicalized_username), canonicalized_username);
  return crypto::SHA256HashString(base::StrCat(
      {canonicalized_username,
       base::StringPiece(kUsernameSalt, base::size(kUsernameSalt))}));
}

std::string BucketizeUsername(base::StringPiece canonicalized_username) {
  // Compute the number of bytes necessary to store `kUsernameHashPrefixLength`
  // bits.
  constexpr size_t kPrefixBytes =
      (kUsernameHashPrefixLength + CHAR_BIT - 1) / CHAR_BIT;
  // Compute the remainder, and construct a mask that keeps the first
  // `kPrefixRemainder` bits.
  constexpr size_t kPrefixRemainder = kUsernameHashPrefixLength % CHAR_BIT;
  constexpr size_t kPrefixMask = ((1 << kPrefixRemainder) - 1)
                                 << (CHAR_BIT - kPrefixRemainder);

  // Check that |canonicalized_username| is actually canonicalized.
  // Note: We can't use CanonicalizeUsername() again, since it's not idempotent
  // if multiple '@' signs are present in the initial username.
  DCHECK_EQ(base::ToLowerASCII(canonicalized_username), canonicalized_username);
  std::string prefix =
      HashUsername(canonicalized_username).substr(0, kPrefixBytes);
  if (kPrefixRemainder != 0)
    prefix.back() &= kPrefixMask;
  return prefix;
}

base::Optional<std::string> ScryptHashUsernameAndPassword(
    base::StringPiece canonicalized_username,
    base::StringPiece password) {
  // Constant salt added to the password hash on top of canonicalized_username.
  // Needs to stay in sync with server side constant: go/passwords-leak-salts
  static constexpr char kPasswordHashSalt[] = {
      48,   118, 42,  -46,  63,  123, -95, -101, -8,  -29, 66,
      -4,   -95, -89, -115, 6,   -26, 107, -28,  -37, -72, 79,
      -127, 83,  -59, 3,    -56, -37, -67, -34,  -91, 32};
  static constexpr size_t kHashKeyLength = 32;
  static constexpr uint64_t kScryptCost = 1 << 12;  // It must be a power of 2.
  static constexpr uint64_t kScryptBlockSize = 8;
  static constexpr uint64_t kScryptParallelization = 1;
  static constexpr size_t kScryptMaxMemory = 1024 * 1024 * 32;

  // Check that |canonicalized_username| is actually canonicalized.
  // Note: We can't use CanonicalizeUsername() again, since it's not idempotent
  // if multiple '@' signs are present in the initial username.
  DCHECK_EQ(base::ToLowerASCII(canonicalized_username), canonicalized_username);
  crypto::OpenSSLErrStackTracer err_tracer(FROM_HERE);
  std::string username_password =
      base::StrCat({canonicalized_username, password});
  std::string salt = base::StrCat(
      {canonicalized_username,
       base::StringPiece(kPasswordHashSalt, base::size(kPasswordHashSalt))});

  std::string result;
  uint8_t* key_data =
      reinterpret_cast<uint8_t*>(base::WriteInto(&result, kHashKeyLength + 1));

  int scrypt_ok =
      EVP_PBE_scrypt(username_password.data(), username_password.size(),
                     reinterpret_cast<const uint8_t*>(salt.data()), salt.size(),
                     kScryptCost, kScryptBlockSize, kScryptParallelization,
                     kScryptMaxMemory, key_data, kHashKeyLength);
  return scrypt_ok == 1 ? base::make_optional(std::move(result))
                        : base::nullopt;
}

base::Optional<std::string> CipherEncrypt(const std::string& plaintext,
                                          std::string* key) {
  using ::private_join_and_compute::ECCommutativeCipher;
  auto cipher = ECCommutativeCipher::CreateWithNewKey(
      NID_X9_62_prime256v1, ECCommutativeCipher::SHA256);
  if (cipher.ok()) {
    auto result = cipher.ValueOrDie()->Encrypt(plaintext);
    if (result.ok()) {
      *key = cipher.ValueOrDie()->GetPrivateKeyBytes();
      return std::move(result).ValueOrDie();
    }
  }
  return base::nullopt;
}

base::Optional<std::string> CipherEncryptWithKey(const std::string& plaintext,
                                                 const std::string& key) {
  using ::private_join_and_compute::ECCommutativeCipher;
  auto cipher = ECCommutativeCipher::CreateFromKey(NID_X9_62_prime256v1, key,
                                                   ECCommutativeCipher::SHA256);
  if (cipher.ok()) {
    auto result = cipher.ValueOrDie()->Encrypt(plaintext);
    if (result.ok())
      return std::move(result).ValueOrDie();
  }
  return base::nullopt;
}

base::Optional<std::string> CipherReEncrypt(
    const std::string& already_encrypted,
    std::string* key) {
  using ::private_join_and_compute::ECCommutativeCipher;
  auto cipher = ECCommutativeCipher::CreateWithNewKey(
      NID_X9_62_prime256v1, ECCommutativeCipher::SHA256);
  if (cipher.ok()) {
    auto result = cipher.ValueOrDie()->ReEncrypt(already_encrypted);
    if (result.ok()) {
      *key = cipher.ValueOrDie()->GetPrivateKeyBytes();
      return std::move(result).ValueOrDie();
    }
  }
  return base::nullopt;
}

base::Optional<std::string> CipherDecrypt(const std::string& ciphertext,
                                          const std::string& key) {
  using ::private_join_and_compute::ECCommutativeCipher;
  auto cipher = ECCommutativeCipher::CreateFromKey(NID_X9_62_prime256v1, key,
                                                   ECCommutativeCipher::SHA256);
  if (cipher.ok()) {
    auto result = cipher.ValueOrDie()->Decrypt(ciphertext);
    if (result.ok())
      return std::move(result).ValueOrDie();
  }
  return base::nullopt;
}

base::Optional<std::string> CreateNewKey() {
  using ::private_join_and_compute::ECCommutativeCipher;
  auto cipher = ECCommutativeCipher::CreateWithNewKey(
      NID_X9_62_prime256v1, ECCommutativeCipher::SHA256);
  if (cipher.ok())
    return cipher.ValueOrDie()->GetPrivateKeyBytes();
  return base::nullopt;
}

}  // namespace password_manager
