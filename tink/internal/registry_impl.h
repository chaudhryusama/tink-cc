// Copyright 2018 Google Inc.
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
//
///////////////////////////////////////////////////////////////////////////////

#ifndef TINK_INTERNAL_REGISTRY_IMPL_H_
#define TINK_INTERNAL_REGISTRY_IMPL_H_

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <utility>

#include "absl/base/thread_annotations.h"
#include "absl/container/flat_hash_map.h"
#include "absl/memory/memory.h"
#include "absl/meta/type_traits.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/types/optional.h"
#include "tink/core/key_manager_impl.h"
#include "tink/core/key_type_manager.h"
#include "tink/core/private_key_manager_impl.h"
#include "tink/core/private_key_type_manager.h"
#include "tink/core/template_util.h"
#include "tink/input_stream.h"
#include "tink/internal/fips_utils.h"
#include "tink/internal/keyset_wrapper.h"
#include "tink/internal/keyset_wrapper_impl.h"
#include "tink/key_manager.h"
#include "tink/monitoring/monitoring.h"
#include "tink/primitive_set.h"
#include "tink/primitive_wrapper.h"
#include "tink/util/errors.h"
#include "tink/util/status.h"
#include "tink/util/statusor.h"
#include "proto/tink.pb.h"

namespace crypto {
namespace tink {
namespace internal {

class RegistryImpl {
 public:
  static RegistryImpl& GlobalInstance() {
    static RegistryImpl* instance = new RegistryImpl();
    return *instance;
  }

  RegistryImpl() = default;
  RegistryImpl(const RegistryImpl&) = delete;
  RegistryImpl& operator=(const RegistryImpl&) = delete;

  // Registers the given 'manager' for the key type 'manager->get_key_type()'.
  // Takes ownership of 'manager', which must be non-nullptr.
  template <class P>
  crypto::tink::util::Status RegisterKeyManager(KeyManager<P>* manager,
                                                bool new_key_allowed = true)
      ABSL_LOCKS_EXCLUDED(maps_mutex_);

  // Takes ownership of 'manager', which must be non-nullptr.
  template <class KeyProto, class KeyFormatProto, class PrimitiveList>
  crypto::tink::util::Status RegisterKeyTypeManager(
      std::unique_ptr<KeyTypeManager<KeyProto, KeyFormatProto, PrimitiveList>>
          manager,
      bool new_key_allowed) ABSL_LOCKS_EXCLUDED(maps_mutex_);

  // Takes ownership of 'private_manager' and 'public_manager'. Both must be
  // non-nullptr.
  template <class PrivateKeyProto, class KeyFormatProto, class PublicKeyProto,
            class PrivatePrimitivesList, class PublicPrimitivesList>
  crypto::tink::util::Status RegisterAsymmetricKeyManagers(
      PrivateKeyTypeManager<PrivateKeyProto, KeyFormatProto, PublicKeyProto,
                            PrivatePrimitivesList>* private_manager,
      KeyTypeManager<PublicKeyProto, void, PublicPrimitivesList>*
          public_manager,
      bool new_key_allowed) ABSL_LOCKS_EXCLUDED(maps_mutex_);

  template <class P>
  crypto::tink::util::StatusOr<const KeyManager<P>*> get_key_manager(
      absl::string_view type_url) const ABSL_LOCKS_EXCLUDED(maps_mutex_);

  // Takes ownership of 'wrapper', which must be non-nullptr.
  template <class P, class Q>
  crypto::tink::util::Status RegisterPrimitiveWrapper(
      PrimitiveWrapper<P, Q>* wrapper) ABSL_LOCKS_EXCLUDED(maps_mutex_);

  template <class P>
  crypto::tink::util::StatusOr<std::unique_ptr<P>> GetPrimitive(
      const google::crypto::tink::KeyData& key_data) const
      ABSL_LOCKS_EXCLUDED(maps_mutex_);

  crypto::tink::util::StatusOr<std::unique_ptr<google::crypto::tink::KeyData>>
  NewKeyData(const google::crypto::tink::KeyTemplate& key_template) const
      ABSL_LOCKS_EXCLUDED(maps_mutex_);

  crypto::tink::util::StatusOr<std::unique_ptr<google::crypto::tink::KeyData>>
  GetPublicKeyData(absl::string_view type_url,
                   absl::string_view serialized_private_key) const
      ABSL_LOCKS_EXCLUDED(maps_mutex_);

  template <class P>
  crypto::tink::util::StatusOr<std::unique_ptr<P>> Wrap(
      std::unique_ptr<PrimitiveSet<P>> primitive_set) const
      ABSL_LOCKS_EXCLUDED(maps_mutex_);

  // Wraps a `keyset` and annotates it with `annotations`.
  template <class P>
  crypto::tink::util::StatusOr<std::unique_ptr<P>> WrapKeyset(
      const google::crypto::tink::Keyset& keyset,
      const absl::flat_hash_map<std::string, std::string>& annotations) const
      ABSL_LOCKS_EXCLUDED(maps_mutex_);

  crypto::tink::util::StatusOr<google::crypto::tink::KeyData> DeriveKey(
      const google::crypto::tink::KeyTemplate& key_template,
      InputStream* randomness) const ABSL_LOCKS_EXCLUDED(maps_mutex_);

  void Reset() ABSL_LOCKS_EXCLUDED(maps_mutex_, monitoring_factory_mutex_);

  crypto::tink::util::Status RestrictToFipsIfEmpty() const
      ABSL_LOCKS_EXCLUDED(maps_mutex_);

  // Registers a `monitoring_factory`. Only one factory can be registered,
  // subsequent calls to this method will return a kAlreadyExists error.
  crypto::tink::util::Status RegisterMonitoringClientFactory(
      std::unique_ptr<crypto::tink::MonitoringClientFactory> monitoring_factory)
      ABSL_LOCKS_EXCLUDED(monitoring_factory_mutex_);

  // Returns a pointer to the registered monitoring factory if any, and nullptr
  // otherwise.
  crypto::tink::MonitoringClientFactory* GetMonitoringClientFactory() const
      ABSL_LOCKS_EXCLUDED(monitoring_factory_mutex_) {
    absl::MutexLock lock(&monitoring_factory_mutex_);
    return monitoring_factory_.get();
  }

 private:
  // Information for a key type constructed from a KeyTypeManager or KeyManager.
  class KeyTypeInfo {
   public:
    // Takes ownership of `manager`.
    template <typename KeyProto, typename KeyFormatProto,
              typename... Primitives>
    KeyTypeInfo(
        KeyTypeManager<KeyProto, KeyFormatProto, List<Primitives...>>* manager,
        bool new_key_allowed)
        : key_manager_type_index_(std::type_index(typeid(*manager))),
          public_key_type_manager_type_index_(absl::nullopt),
          new_key_allowed_(new_key_allowed),
          key_type_manager_(absl::WrapUnique(manager)),
          internal_key_factory_(
              absl::make_unique<internal::KeyFactoryImpl<KeyTypeManager<
                  KeyProto, KeyFormatProto, List<Primitives...>>>>(manager)),
          key_factory_(internal_key_factory_.get()),
          key_deriver_(CreateDeriverFunctionFor(manager)) {
      // TODO(C++17): Replace with a fold expression.
      (void)std::initializer_list<int>{
          0, (primitive_to_manager_.emplace(
                  std::type_index(typeid(Primitives)),
                  internal::MakeKeyManager<Primitives>(manager)),
              0)...};
    }

    // Takes ownership of `private_manager`, but not of `public_manager`, which
    // must only be alive for the duration of the constructor.
    template <typename PrivateKeyProto, typename KeyFormatProto,
              typename PublicKeyProto, typename PublicPrimitivesList,
              typename... PrivatePrimitives>
    KeyTypeInfo(
        PrivateKeyTypeManager<PrivateKeyProto, KeyFormatProto, PublicKeyProto,
                              List<PrivatePrimitives...>>* private_manager,
        KeyTypeManager<PublicKeyProto, void, PublicPrimitivesList>*
            public_manager,
        bool new_key_allowed)
        : key_manager_type_index_(std::type_index(typeid(*private_manager))),
          public_key_type_manager_type_index_(
              std::type_index(typeid(*public_manager))),
          new_key_allowed_(new_key_allowed),
          key_type_manager_(absl::WrapUnique(private_manager)),
          internal_key_factory_(
              absl::make_unique<internal::PrivateKeyFactoryImpl<
                  PrivateKeyProto, KeyFormatProto, PublicKeyProto,
                  List<PrivatePrimitives...>, PublicPrimitivesList>>(
                  private_manager, public_manager)),
          key_factory_(internal_key_factory_.get()),
          key_deriver_(CreateDeriverFunctionFor(private_manager)) {
      // TODO(C++17): Replace with a fold expression.
      (void)std::initializer_list<int>{
          0, (primitive_to_manager_.emplace(
                  std::type_index(typeid(PrivatePrimitives)),
                  internal::MakePrivateKeyManager<PrivatePrimitives>(
                      private_manager, public_manager)),
              0)...};
    }

    // Takes ownership of `manager`. KeyManager is the legacy version of
    // KeyTypeManager.
    template <typename P>
    KeyTypeInfo(KeyManager<P>* manager, bool new_key_allowed)
        : key_manager_type_index_(std::type_index(typeid(*manager))),
          public_key_type_manager_type_index_(absl::nullopt),
          new_key_allowed_(new_key_allowed),
          key_type_manager_(nullptr),
          internal_key_factory_(nullptr),
          key_factory_(&manager->get_key_factory()) {
      primitive_to_manager_.emplace(std::type_index(typeid(P)),
                                    absl::WrapUnique(manager));
    }

    template <typename P>
    crypto::tink::util::StatusOr<const KeyManager<P>*> get_key_manager(
        absl::string_view requested_type_url) const {
      auto it = primitive_to_manager_.find(std::type_index(typeid(P)));
      if (it == primitive_to_manager_.end()) {
        return crypto::tink::util::Status(
            absl::StatusCode::kInvalidArgument,
            absl::StrCat(
                "Primitive type ", typeid(P).name(),
                " not among supported primitives ",
                absl::StrJoin(
                    primitive_to_manager_.begin(), primitive_to_manager_.end(),
                    ", ",
                    [](std::string* out,
                       const std::pair<const std::type_index,
                                       std::unique_ptr<KeyManagerBase>>& kv) {
                      absl::StrAppend(out, kv.first.name());
                    }),
                " for type URL ", requested_type_url));
      }
      return static_cast<const KeyManager<P>*>(it->second.get());
    }

    const std::type_index& key_manager_type_index() const {
      return key_manager_type_index_;
    }

    const absl::optional<std::type_index>& public_key_type_manager_type_index()
        const {
      return public_key_type_manager_type_index_;
    }

    bool new_key_allowed() const { return new_key_allowed_.load(); }

    void set_new_key_allowed(bool b) { new_key_allowed_.store(b); }

    const KeyFactory& key_factory() const { return *key_factory_; }

    const std::function<crypto::tink::util::StatusOr<
        google::crypto::tink::KeyData>(absl::string_view, InputStream*)>&
    key_deriver() const {
      return key_deriver_;
    }

   private:
    // Dynamic type_index of the KeyManager or KeyTypeManager for this key type.
    std::type_index key_manager_type_index_;
    // Dynamic type_index of the public KeyTypeManager for this key type when
    // inserted into the registry via RegisterAsymmetricKeyManagers. Otherwise,
    // nullopt.
    absl::optional<std::type_index> public_key_type_manager_type_index_;
    // Whether the key manager allows the creation of new keys.
    std::atomic<bool> new_key_allowed_;

    // Map from primitive type_index to KeyManager.
    absl::flat_hash_map<std::type_index, std::unique_ptr<KeyManagerBase>>
        primitive_to_manager_;
    // Key type manager. Equals nullptr if KeyTypeInfo was constructed from a
    // KeyManager.
    const std::shared_ptr<void> key_type_manager_;

    // Key factory. Equals nullptr if KeyTypeInfo was constructed from a
    // KeyManager.
    std::unique_ptr<const KeyFactory> internal_key_factory_;
    // Unowned version of `internal_key_factory_` if KeyTypeInfo was constructed
    // from a KeyTypeManager.
    // Key factory belonging to the KeyManager if KeyTypeInfo was constructed
    // from a KeyManager.
    const KeyFactory* key_factory_;

    // Derives a key if KeyTypeInfo was constructed from a KeyTypeManager with a
    // non-void KeyFormat type. Else, this function is empty and casting to a
    // bool returns false.
    std::function<crypto::tink::util::StatusOr<google::crypto::tink::KeyData>(
        absl::string_view, InputStream*)>
        key_deriver_;
  };

  class WrapperInfo {
   public:
    template <typename P, typename Q>
    explicit WrapperInfo(RegistryImpl& registry,
                         std::unique_ptr<PrimitiveWrapper<P, Q>> wrapper)
        : is_same_primitive_wrapping_(std::is_same<P, Q>::value),
          wrapper_type_index_(std::type_index(typeid(*wrapper))),
          q_type_index_(std::type_index(typeid(Q))) {
      auto keyset_wrapper_unique_ptr =
          absl::make_unique<KeysetWrapperImpl<P, Q>>(
              wrapper.get(),
              [&registry](const google::crypto::tink::KeyData& key_data) {
                return registry.GetPrimitive<P>(key_data);
              });
      keyset_wrapper_ = std::move(keyset_wrapper_unique_ptr);
      original_wrapper_ = std::move(wrapper);
    }

    template <typename Q>
    crypto::tink::util::StatusOr<const KeysetWrapper<Q>*> GetKeysetWrapper()
        const {
      if (q_type_index_ != std::type_index(typeid(Q))) {
        return crypto::tink::util::Status(
            absl::StatusCode::kInternal,
            "RegistryImpl::KeysetWrapper() called with wrong type");
      }
      return static_cast<KeysetWrapper<Q>*>(keyset_wrapper_.get());
    }

    template <typename P>
    crypto::tink::util::StatusOr<const PrimitiveWrapper<P, P>*>
    GetLegacyWrapper() const {
      if (!is_same_primitive_wrapping_) {
        // This happens if a user uses a legacy method (like Registry::Wrap)
        // directly or has a custom key manager for a primitive which has a
        // PrimitiveWrapper<P,Q> with P != Q.
        return crypto::tink::util::Status(
            absl::StatusCode::kFailedPrecondition,
            absl::StrCat("Cannot use primitive type ", typeid(P).name(),
                         " with a custom key manager."));
      }
      if (q_type_index_ != std::type_index(typeid(P))) {
        return crypto::tink::util::Status(
            absl::StatusCode::kInternal,
            "RegistryImpl::LegacyWrapper() called with wrong type");
      }
      return static_cast<const PrimitiveWrapper<P, P>*>(
          original_wrapper_.get());
    }

    // Returns true if the PrimitiveWrapper is the same class as the one used
    // to construct this WrapperInfo
    template <typename P, typename Q>
    bool HasSameType(const PrimitiveWrapper<P, Q>& wrapper) {
      return wrapper_type_index_ == std::type_index(typeid(wrapper));
    }

   private:
    bool is_same_primitive_wrapping_;
    // dynamic std::type_index of the actual PrimitiveWrapper<P,Q> class for
    // which this key was inserted.
    std::type_index wrapper_type_index_;
    // dynamic std::type_index of Q, when PrimitiveWrapper<P,Q> was inserted.
    std::type_index q_type_index_;
    // The primitive_wrapper passed in. We use a shared_ptr because
    // unique_ptr<void> is invalid.
    std::shared_ptr<void> original_wrapper_;
    // The keyset_wrapper_. We use a shared_ptr because unique_ptr<void> is
    // invalid.
    std::shared_ptr<void> keyset_wrapper_;
  };

  template <class P>
  crypto::tink::util::StatusOr<const PrimitiveWrapper<P, P>*> GetLegacyWrapper()
      const ABSL_LOCKS_EXCLUDED(maps_mutex_);

  template <class P>
  crypto::tink::util::StatusOr<const KeysetWrapper<P>*> GetKeysetWrapper() const
      ABSL_LOCKS_EXCLUDED(maps_mutex_);

  // Returns the key type info for a given type URL. Since we never replace
  // key type infos, the pointers will stay valid for the lifetime of the
  // binary.
  crypto::tink::util::StatusOr<const KeyTypeInfo*> get_key_type_info(
      absl::string_view type_url) const ABSL_LOCKS_EXCLUDED(maps_mutex_);

  // Returns OK if the key manager with the given type index can be inserted
  // for type url type_url and parameter new_key_allowed. Otherwise returns
  // an error to be returned to the user.
  crypto::tink::util::Status CheckInsertable(
      absl::string_view type_url, const std::type_index& key_manager_type_index,
      bool new_key_allowed) const ABSL_SHARED_LOCKS_REQUIRED(maps_mutex_);

  mutable absl::Mutex maps_mutex_;
  // A map from the type_url to the given KeyTypeInfo. Once emplaced KeyTypeInfo
  // objects must remain valid throughout the life time of the binary. Hence,
  // one should /never/ replace any element of the KeyTypeInfo. This is because
  // get_key_type_manager() needs to guarantee that the returned
  // key_type_manager remains valid.
  // NOTE: We require pointer stability of the value, as get_key_type_info
  // returns a pointer which needs to stay alive.
  absl::flat_hash_map<std::string, std::unique_ptr<KeyTypeInfo>>
      type_url_to_info_ ABSL_GUARDED_BY(maps_mutex_);
  // A map from the type_id to the corresponding wrapper.
  absl::flat_hash_map<std::type_index, std::unique_ptr<WrapperInfo>>
      primitive_to_wrapper_ ABSL_GUARDED_BY(maps_mutex_);

  mutable absl::Mutex monitoring_factory_mutex_;
  std::unique_ptr<crypto::tink::MonitoringClientFactory> monitoring_factory_
      ABSL_GUARDED_BY(monitoring_factory_mutex_);
};

template <class P>
crypto::tink::util::Status RegistryImpl::RegisterKeyManager(
    KeyManager<P>* manager, bool new_key_allowed) {
  auto owned_manager = absl::WrapUnique(manager);
  if (owned_manager == nullptr) {
    return crypto::tink::util::Status(absl::StatusCode::kInvalidArgument,
                                      "Parameter 'manager' must be non-null.");
  }
  std::string type_url = owned_manager->get_key_type();
  if (!manager->DoesSupport(type_url)) {
    return ToStatusF(absl::StatusCode::kInvalidArgument,
                     "The manager does not support type '%s'.", type_url);
  }
  absl::MutexLock lock(&maps_mutex_);
  crypto::tink::util::Status status = CheckInsertable(
      type_url, std::type_index(typeid(*owned_manager)), new_key_allowed);
  if (!status.ok()) return status;

  auto it = type_url_to_info_.find(type_url);
  if (it != type_url_to_info_.end()) {
    it->second->set_new_key_allowed(new_key_allowed);
  } else {
    auto key_type_info = absl::make_unique<KeyTypeInfo>(owned_manager.release(),
                                                        new_key_allowed);
    type_url_to_info_.insert({type_url, std::move(key_type_info)});
  }
  return crypto::tink::util::OkStatus();
}

template <class KeyProto, class KeyFormatProto, class PrimitiveList>
crypto::tink::util::Status RegistryImpl::RegisterKeyTypeManager(
    std::unique_ptr<KeyTypeManager<KeyProto, KeyFormatProto, PrimitiveList>>
        manager,
    bool new_key_allowed) {
  if (manager == nullptr) {
    return crypto::tink::util::Status(absl::StatusCode::kInvalidArgument,
                                      "Parameter 'manager' must be non-null.");
  }
  std::string type_url = manager->get_key_type();
  absl::MutexLock lock(&maps_mutex_);

  // Check FIPS status
  internal::FipsCompatibility fips_compatible = manager->FipsStatus();
  auto fips_status = internal::ChecksFipsCompatibility(fips_compatible);
  if (!fips_status.ok()) {
    return crypto::tink::util::Status(
        absl::StatusCode::kInternal,
        absl::StrCat("Failed registering the key manager for ",
                     typeid(*manager).name(),
                     " as it is not FIPS compatible: ", fips_status.message()));
  }

  crypto::tink::util::Status status = CheckInsertable(
      type_url, std::type_index(typeid(*manager)), new_key_allowed);
  if (!status.ok()) return status;

  auto it = type_url_to_info_.find(type_url);
  if (it != type_url_to_info_.end()) {
    it->second->set_new_key_allowed(new_key_allowed);
  } else {
    auto key_type_info =
        absl::make_unique<KeyTypeInfo>(manager.release(), new_key_allowed);
    type_url_to_info_.insert({type_url, std::move(key_type_info)});
  }
  return crypto::tink::util::OkStatus();
}

template <class PrivateKeyProto, class KeyFormatProto, class PublicKeyProto,
          class PrivatePrimitivesList, class PublicPrimitivesList>
crypto::tink::util::Status RegistryImpl::RegisterAsymmetricKeyManagers(
    PrivateKeyTypeManager<PrivateKeyProto, KeyFormatProto, PublicKeyProto,
                          PrivatePrimitivesList>* private_manager,
    KeyTypeManager<PublicKeyProto, void, PublicPrimitivesList>* public_manager,
    bool new_key_allowed) ABSL_LOCKS_EXCLUDED(maps_mutex_) {
  auto owned_private_manager = absl::WrapUnique(private_manager);
  auto owned_public_manager = absl::WrapUnique(public_manager);
  if (private_manager == nullptr) {
    return crypto::tink::util::Status(
        absl::StatusCode::kInvalidArgument,
        "Parameter 'private_manager' must be non-null.");
  }
  if (public_manager == nullptr) {
    return crypto::tink::util::Status(
        absl::StatusCode::kInvalidArgument,
        "Parameter 'public_manager' must be non-null.");
  }
  std::string private_type_url = private_manager->get_key_type();
  std::string public_type_url = public_manager->get_key_type();

  absl::MutexLock lock(&maps_mutex_);

  // Check FIPS status
  auto private_fips_status =
      internal::ChecksFipsCompatibility(private_manager->FipsStatus());

  if (!private_fips_status.ok()) {
    return crypto::tink::util::Status(
        absl::StatusCode::kInternal,
        absl::StrCat(
            "Failed registering the key manager for ",
            typeid(*private_manager).name(),
            " as it is not FIPS compatible: ", private_fips_status.message()));
  }

  auto public_fips_status =
      internal::ChecksFipsCompatibility(public_manager->FipsStatus());

  if (!public_fips_status.ok()) {
    return crypto::tink::util::Status(
        absl::StatusCode::kInternal,
        absl::StrCat(
            "Failed registering the key manager for ",
            typeid(*public_manager).name(),
            " as it is not FIPS compatible: ", public_fips_status.message()));
  }

  crypto::tink::util::Status status = CheckInsertable(
      private_type_url, std::type_index(typeid(*private_manager)),
      new_key_allowed);
  if (!status.ok()) return status;
  status =
      CheckInsertable(public_type_url, std::type_index(typeid(*public_manager)),
                      new_key_allowed);
  if (!status.ok()) return status;

  if (private_type_url == public_type_url) {
    return crypto::tink::util::Status(
        absl::StatusCode::kInvalidArgument,
        "Passed in key managers must have different get_key_type() results.");
  }

  auto private_it = type_url_to_info_.find(private_type_url);
  auto public_it = type_url_to_info_.find(public_type_url);
  bool private_found = private_it != type_url_to_info_.end();
  bool public_found = public_it != type_url_to_info_.end();

  // Only one of the private and public key type managers is found.
  if (private_found && !public_found) {
    return crypto::tink::util::Status(
        absl::StatusCode::kInvalidArgument,
        absl::StrCat(
            "Private key manager corresponding to ",
            typeid(*private_manager).name(),
            " was previously registered, but key manager corresponding to ",
            typeid(*public_manager).name(),
            " was not, so it's impossible to register them jointly"));
  }
  if (!private_found && public_found) {
    return crypto::tink::util::Status(
        absl::StatusCode::kInvalidArgument,
        absl::StrCat("Key manager corresponding to ",
                     typeid(*public_manager).name(),
                     " was previously registered, but private key manager "
                     "corresponding to ",
                     typeid(*private_manager).name(),
                     " was not, so it's impossible to register them jointly"));
  }

  // Both private and public key type managers are found.
  if (private_found) {
    // implies public_found.
    if (!private_it->second->public_key_type_manager_type_index().has_value()) {
      return crypto::tink::util::Status(
          absl::StatusCode::kInvalidArgument,
          absl::StrCat("private key manager corresponding to ",
                       typeid(*private_manager).name(),
                       " is already registered without public key manager, "
                       "cannot be re-registered with public key manager. "));
    }
    if (*private_it->second->public_key_type_manager_type_index() !=
        std::type_index(typeid(*public_manager))) {
      return crypto::tink::util::Status(
          absl::StatusCode::kInvalidArgument,
          absl::StrCat(
              "private key manager corresponding to ",
              typeid(*private_manager).name(), " is already registered with ",
              private_it->second->public_key_type_manager_type_index()->name(),
              ", cannot be re-registered with ",
              typeid(*public_manager).name()));
    }
  }

  // Both private and public key type managers are not found.
  if (!private_found) {
    // !public_found must hold.
    auto private_key_type_manager_info = absl::make_unique<KeyTypeInfo>(
        owned_private_manager.release(), owned_public_manager.get(),
        new_key_allowed);
    type_url_to_info_.insert(
        {private_type_url, std::move(private_key_type_manager_info)});
    auto public_key_type_info = absl::make_unique<KeyTypeInfo>(
        owned_public_manager.release(), new_key_allowed);
    type_url_to_info_.insert(
        {public_type_url, std::move(public_key_type_info)});
  } else {
    private_it->second->set_new_key_allowed(new_key_allowed);
  }

  return util::OkStatus();
}

template <class P, class Q>
crypto::tink::util::Status RegistryImpl::RegisterPrimitiveWrapper(
    PrimitiveWrapper<P, Q>* wrapper) {
  if (wrapper == nullptr) {
    return crypto::tink::util::Status(absl::StatusCode::kInvalidArgument,
                                      "Parameter 'wrapper' must be non-null.");
  }
  std::unique_ptr<PrimitiveWrapper<P, Q>> owned_wrapper(wrapper);

  absl::MutexLock lock(&maps_mutex_);
  auto it = primitive_to_wrapper_.find(std::type_index(typeid(Q)));
  if (it != primitive_to_wrapper_.end()) {
    if (!it->second->HasSameType(*wrapper)) {
      return util::Status(
          absl::StatusCode::kAlreadyExists,
          "A wrapper named for this primitive has already been added.");
    }
    return crypto::tink::util::OkStatus();
  }
  auto wrapper_info =
      absl::make_unique<WrapperInfo>(*this, std::move(owned_wrapper));
  primitive_to_wrapper_.insert(
      {std::type_index(typeid(Q)), std::move(wrapper_info)});
  return crypto::tink::util::OkStatus();
}

template <class P>
crypto::tink::util::StatusOr<const KeyManager<P>*>
RegistryImpl::get_key_manager(absl::string_view type_url) const {
  absl::MutexLock lock(&maps_mutex_);
  auto it = type_url_to_info_.find(type_url);
  if (it == type_url_to_info_.end()) {
    return ToStatusF(absl::StatusCode::kNotFound,
                     "No manager for type '%s' has been registered.", type_url);
  }
  return it->second->get_key_manager<P>(type_url);
}

template <class P>
crypto::tink::util::StatusOr<std::unique_ptr<P>> RegistryImpl::GetPrimitive(
    const google::crypto::tink::KeyData& key_data) const {
  auto key_manager_result = get_key_manager<P>(key_data.type_url());
  if (key_manager_result.ok()) {
    return key_manager_result.value()->GetPrimitive(key_data);
  }
  return key_manager_result.status();
}

template <class P>
crypto::tink::util::StatusOr<const PrimitiveWrapper<P, P>*>
RegistryImpl::GetLegacyWrapper() const {
  absl::MutexLock lock(&maps_mutex_);
  auto it = primitive_to_wrapper_.find(std::type_index(typeid(P)));
  if (it == primitive_to_wrapper_.end()) {
    return util::Status(
        absl::StatusCode::kNotFound,
        absl::StrCat("No wrapper registered for type ", typeid(P).name()));
  }
  return it->second->GetLegacyWrapper<P>();
}

template <class P>
crypto::tink::util::StatusOr<const KeysetWrapper<P>*>
RegistryImpl::GetKeysetWrapper() const {
  absl::MutexLock lock(&maps_mutex_);
  auto it = primitive_to_wrapper_.find(std::type_index(typeid(P)));
  if (it == primitive_to_wrapper_.end()) {
    return util::Status(
        absl::StatusCode::kNotFound,
        absl::StrCat("No wrapper registered for type ", typeid(P).name()));
  }
  return it->second->GetKeysetWrapper<P>();
}

template <class P>
crypto::tink::util::StatusOr<std::unique_ptr<P>> RegistryImpl::Wrap(
    std::unique_ptr<PrimitiveSet<P>> primitive_set) const {
  if (primitive_set == nullptr) {
    return crypto::tink::util::Status(
        absl::StatusCode::kInvalidArgument,
        "Parameter 'primitive_set' must be non-null.");
  }
  util::StatusOr<const PrimitiveWrapper<P, P>*> wrapper_result =
      GetLegacyWrapper<P>();
  if (!wrapper_result.ok()) {
    return wrapper_result.status();
  }
  return wrapper_result.value()->Wrap(std::move(primitive_set));
}

template <class P>
crypto::tink::util::StatusOr<std::unique_ptr<P>> RegistryImpl::WrapKeyset(
    const google::crypto::tink::Keyset& keyset,
    const absl::flat_hash_map<std::string, std::string>& annotations) const {
  crypto::tink::util::StatusOr<const KeysetWrapper<P>*> keyset_wrapper =
      GetKeysetWrapper<P>();
  if (!keyset_wrapper.ok()) {
    return keyset_wrapper.status();
  }
  return (*keyset_wrapper)->Wrap(keyset, annotations);
}

inline crypto::tink::util::Status RegistryImpl::RestrictToFipsIfEmpty() const {
  absl::MutexLock lock(&maps_mutex_);
  // If we are already in FIPS mode, then do nothing..
  if (IsFipsModeEnabled()) {
    return util::OkStatus();
  }
  if (type_url_to_info_.empty()) {
    SetFipsRestricted();
    return util::OkStatus();
  }
  return util::Status(absl::StatusCode::kInternal,
                      "Could not set FIPS only mode. Registry is not empty.");
}

}  // namespace internal
}  // namespace tink
}  // namespace crypto

#endif  // TINK_INTERNAL_REGISTRY_IMPL_H_
