// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVICE_FIDO_CABLE_CABLE_DISCOVERY_DATA_H_
#define DEVICE_FIDO_CABLE_CABLE_DISCOVERY_DATA_H_

#include <stdint.h>
#include <array>

#include "base/component_export.h"
#include "base/containers/span.h"
#include "base/optional.h"
#include "device/fido/cable/v2_constants.h"
#include "device/fido/fido_constants.h"
#include "third_party/abseil-cpp/absl/types/variant.h"

namespace cbor {
class Value;
}

namespace device {

constexpr size_t kCableEphemeralIdSize = 16;
constexpr size_t kCableSessionPreKeySize = 32;
constexpr size_t kCableNonceSize = 8;

using CableEidArray = std::array<uint8_t, kCableEphemeralIdSize>;
using CableSessionPreKeyArray = std::array<uint8_t, kCableSessionPreKeySize>;
// CableNonce is a nonce used in BLE handshaking.
using CableNonce = std::array<uint8_t, 8>;
// CableEidGeneratorKey is an AES-256 key that is used to encrypt a 64-bit nonce
// and 64-bits of zeros, resulting in a BLE-advertised EID.
using CableEidGeneratorKey = std::array<uint8_t, 32>;
// CablePskGeneratorKey is HKDF input keying material that is used to
// generate a Noise PSK given the nonce decrypted from an EID.
using CablePskGeneratorKey = std::array<uint8_t, 32>;
using CableTunnelIDGeneratorKey = std::array<uint8_t, 32>;
// CableAuthenticatorIdentityKey is a P-256 public value used to authenticate a
// paired phone.
using CableAuthenticatorIdentityKey = std::array<uint8_t, kP256X962Length>;

// Encapsulates information required to discover Cable device per single
// credential. When multiple credentials are enrolled to a single account
// (i.e. more than one phone has been enrolled to an user account as a
// security key), then FidoCableDiscovery must advertise for all of the client
// EID received from the relying party.
// TODO(hongjunchoi): Add discovery data required for MakeCredential request.
// See: https://crbug.com/837088
struct COMPONENT_EXPORT(DEVICE_FIDO) CableDiscoveryData {
  enum class Version {
    INVALID,
    V1,
    V2,
  };

  CableDiscoveryData(Version version,
                     const CableEidArray& client_eid,
                     const CableEidArray& authenticator_eid,
                     const CableSessionPreKeyArray& session_pre_key);
  CableDiscoveryData();
  CableDiscoveryData(const CableDiscoveryData& data);
  ~CableDiscoveryData();

  CableDiscoveryData& operator=(const CableDiscoveryData& other);
  bool operator==(const CableDiscoveryData& other) const;

  // MatchV1 returns true if |candidate_eid| matches this caBLE discovery
  // instance, which must be version one.
  bool MatchV1(const CableEidArray& candidate_eid) const;

  // version indicates whether v1 or v2 data is contained in this object.
  // |INVALID| is not a valid version but is set as the default to catch any
  // cases where the version hasn't been set explicitly.
  Version version = Version::INVALID;

  struct V1Data {
    CableEidArray client_eid;
    CableEidArray authenticator_eid;
    CableSessionPreKeyArray session_pre_key;
  };
  base::Optional<V1Data> v1;

  // For caBLEv2, the payload is the server-link data provided in the extension
  // as the "sessionPreKey".
  base::Optional<std::vector<uint8_t>> v2;
};

namespace cablev2 {

// Pairing represents information previously received from a caBLEv2
// authenticator that enables future interactions to skip scanning a QR code.
struct COMPONENT_EXPORT(DEVICE_FIDO) Pairing {
  Pairing();
  ~Pairing();
  Pairing(const Pairing&) = delete;
  Pairing& operator=(const Pairing&) = delete;

  // Parse builds a |Pairing| from an authenticator message. The signature
  // within the structure is validated by using |local_identity_seed| and
  // |handshake_hash|.
  static base::Optional<std::unique_ptr<Pairing>> Parse(
      const cbor::Value& cbor,
      uint32_t tunnel_server_domain,
      base::span<const uint8_t, kQRSeedSize> local_identity_seed,
      base::span<const uint8_t, 32> handshake_hash);

  // tunnel_server_domain is known to be a valid hostname as it's constructed
  // from the 22-bit value in the BLE advert rather than being parsed as a
  // string from the authenticator.
  std::string tunnel_server_domain;
  // contact_id is an opaque value that is sent to the tunnel service in order
  // to identify the caBLEv2 authenticator.
  std::vector<uint8_t> contact_id;
  // id is an opaque identifier that is sent via the tunnel service, to the
  // authenticator, to identify this specific pairing.
  std::vector<uint8_t> id;
  // secret is the shared secret that authenticates the desktop to the
  // authenticator.
  std::vector<uint8_t> secret;
  // peer_public_key_x962 is the authenticator's public key.
  std::array<uint8_t, kP256X962Length> peer_public_key_x962;
  // name is a human-friendly name for the authenticator, specified by that
  // authenticator. (For example "Pixel 3".)
  std::string name;
};

// A PairingEvent is either a new |Pairing|, learnt from a device, or else the
// public key of a pairing that has been discovered to be invalid.
using PairingEvent = absl::variant<std::unique_ptr<Pairing>,
                                   std::array<uint8_t, kP256X962Length>>;

}  // namespace cablev2

}  // namespace device

#endif  // DEVICE_FIDO_CABLE_CABLE_DISCOVERY_DATA_H_
