// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/cert/internal/system_trust_store.h"

#if defined(USE_NSS_CERTS)
#include "net/cert/internal/system_trust_store_nss.h"
#endif  // defined(USE_NSS_CERTS)

#if defined(USE_NSS_CERTS)
#include <cert.h>
#include <pk11pub.h>
#elif defined(OS_MAC)
#include <Security/Security.h>
#endif

#include <array>
#include <memory>
#include <vector>

#include "base/environment.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/string_split.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "net/cert/internal/cert_errors.h"
#include "net/cert/internal/parsed_certificate.h"
#include "net/cert/internal/trust_store_collection.h"
#include "net/cert/internal/trust_store_in_memory.h"
#include "net/cert/test_root_certs.h"
#include "net/cert/x509_certificate.h"
#include "net/cert/x509_util.h"

#if defined(USE_NSS_CERTS)
#include "crypto/nss_util.h"
#include "net/cert/internal/trust_store_nss.h"
#include "net/cert/known_roots_nss.h"
#include "net/cert/scoped_nss_types.h"
#elif defined(OS_MAC)
#include "net/cert/internal/trust_store_mac.h"
#include "net/cert/x509_util_mac.h"
#elif defined(OS_FUCHSIA)
#include "third_party/boringssl/src/include/openssl/pool.h"
#endif

namespace net {

namespace {

// Abstract implementation of SystemTrustStore to be used as a base class.
// Handles the addition of additional trust anchors.
class BaseSystemTrustStore : public SystemTrustStore {
 public:
  BaseSystemTrustStore() {
    trust_store_.AddTrustStore(&additional_trust_store_);
  }

  void AddTrustAnchor(
      const scoped_refptr<ParsedCertificate>& trust_anchor) override {
    additional_trust_store_.AddTrustAnchor(trust_anchor);
  }

  TrustStore* GetTrustStore() override { return &trust_store_; }

  bool IsAdditionalTrustAnchor(
      const ParsedCertificate* trust_anchor) const override {
    return additional_trust_store_.Contains(trust_anchor);
  }

 protected:
  TrustStoreCollection trust_store_;
  TrustStoreInMemory additional_trust_store_;
};

class DummySystemTrustStore : public BaseSystemTrustStore {
 public:
  bool UsesSystemTrustStore() const override { return false; }

  bool IsKnownRoot(const ParsedCertificate* trust_anchor) const override {
    return false;
  }
};

}  // namespace

#if defined(USE_NSS_CERTS)
namespace {

class SystemTrustStoreNSS : public BaseSystemTrustStore {
 public:
  explicit SystemTrustStoreNSS(std::unique_ptr<TrustStoreNSS> trust_store_nss)
      : trust_store_nss_(std::move(trust_store_nss)) {
    trust_store_.AddTrustStore(trust_store_nss_.get());

    // When running in test mode, also layer in the test-only root certificates.
    //
    // Note that this integration requires TestRootCerts::HasInstance() to be
    // true by the time SystemTrustStoreNSS is created - a limitation which is
    // acceptable for the test-only code that consumes this.
    if (TestRootCerts::HasInstance()) {
      trust_store_.AddTrustStore(
          TestRootCerts::GetInstance()->test_trust_store());
    }
  }

  bool UsesSystemTrustStore() const override { return true; }

  // IsKnownRoot returns true if the given trust anchor is a standard one (as
  // opposed to a user-installed root)
  bool IsKnownRoot(const ParsedCertificate* trust_anchor) const override {
    // TODO(eroman): The overall approach of IsKnownRoot() is inefficient -- it
    // requires searching for the trust anchor by DER in NSS, however path
    // building already had a handle to it.
    SECItem der_cert;
    der_cert.data = const_cast<uint8_t*>(trust_anchor->der_cert().UnsafeData());
    der_cert.len = trust_anchor->der_cert().Length();
    der_cert.type = siDERCertBuffer;
    ScopedCERTCertificate nss_cert(
        CERT_FindCertByDERCert(CERT_GetDefaultCertDB(), &der_cert));
    if (!nss_cert)
      return false;

    if (!net::IsKnownRoot(nss_cert.get()))
      return false;

    return trust_anchor->der_cert() ==
           der::Input(nss_cert->derCert.data, nss_cert->derCert.len);
  }

 private:
  std::unique_ptr<TrustStoreNSS> trust_store_nss_;
};

}  // namespace

std::unique_ptr<SystemTrustStore> CreateSslSystemTrustStore() {
  return std::make_unique<SystemTrustStoreNSS>(
      std::make_unique<TrustStoreNSS>(trustSSL));
}

std::unique_ptr<SystemTrustStore>
CreateSslSystemTrustStoreNSSWithUserSlotRestriction(
    crypto::ScopedPK11Slot user_slot) {
  return std::make_unique<SystemTrustStoreNSS>(
      std::make_unique<TrustStoreNSS>(trustSSL, std::move(user_slot)));
}

std::unique_ptr<SystemTrustStore>
CreateSslSystemTrustStoreNSSWithNoUserSlots() {
  return std::make_unique<SystemTrustStoreNSS>(std::make_unique<TrustStoreNSS>(
      trustSSL, TrustStoreNSS::DisallowTrustForCertsOnUserSlots()));
}

#elif defined(OS_MAC)

class SystemTrustStoreMac : public BaseSystemTrustStore {
 public:
  SystemTrustStoreMac() {
    trust_store_.AddTrustStore(GetGlobalTrustStoreMac());

    // When running in test mode, also layer in the test-only root certificates.
    //
    // Note that this integration requires TestRootCerts::HasInstance() to be
    // true by the time SystemTrustStoreMac is created - a limitation which is
    // acceptable for the test-only code that consumes this.
    if (TestRootCerts::HasInstance()) {
      trust_store_.AddTrustStore(
          TestRootCerts::GetInstance()->test_trust_store());
    }
  }

  bool UsesSystemTrustStore() const override { return true; }

  // IsKnownRoot returns true if the given trust anchor is a standard one (as
  // opposed to a user-installed root)
  bool IsKnownRoot(const ParsedCertificate* trust_anchor) const override {
    return GetGlobalTrustStoreMac()->IsKnownRoot(trust_anchor);
  }

  static void InitializeTrustCacheOnWorkerThread() {
    GetGlobalTrustStoreMac()->InitializeTrustCache();
  }

 private:
  static TrustStoreMac* GetGlobalTrustStoreMac() {
    static base::NoDestructor<TrustStoreMac> static_trust_store_mac(
        kSecPolicyAppleSSL);
    return static_trust_store_mac.get();
  }
};

std::unique_ptr<SystemTrustStore> CreateSslSystemTrustStore() {
  return std::make_unique<SystemTrustStoreMac>();
}

void InitializeTrustStoreMacCache() {
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&SystemTrustStoreMac::InitializeTrustCacheOnWorkerThread));
}

#elif defined(OS_FUCHSIA)

namespace {

constexpr char kRootCertsFileFuchsia[] = "/config/ssl/cert.pem";

class FuchsiaSystemCerts {
 public:
  FuchsiaSystemCerts() {
    base::FilePath filename(kRootCertsFileFuchsia);
    std::string certs_file;
    if (!base::ReadFileToString(filename, &certs_file)) {
      LOG(ERROR) << "Can't load root certificates from " << filename;
      return;
    }

    CertificateList certs = X509Certificate::CreateCertificateListFromBytes(
        certs_file.data(), certs_file.length(), X509Certificate::FORMAT_AUTO);

    for (const auto& cert : certs) {
      CertErrors errors;
      auto parsed = ParsedCertificate::Create(
          bssl::UpRef(cert->cert_buffer()),
          x509_util::DefaultParseCertificateOptions(), &errors);
      CHECK(parsed) << errors.ToDebugString();
      system_trust_store_.AddTrustAnchor(parsed);
    }
  }

  TrustStoreInMemory* system_trust_store() { return &system_trust_store_; }

 private:
  TrustStoreInMemory system_trust_store_;
};

base::LazyInstance<FuchsiaSystemCerts>::Leaky g_root_certs_fuchsia =
    LAZY_INSTANCE_INITIALIZER;

}  // namespace

class SystemTrustStoreFuchsia : public BaseSystemTrustStore {
 public:
  SystemTrustStoreFuchsia() {
    trust_store_.AddTrustStore(g_root_certs_fuchsia.Get().system_trust_store());
    if (TestRootCerts::HasInstance()) {
      trust_store_.AddTrustStore(
          TestRootCerts::GetInstance()->test_trust_store());
    }
  }

  bool UsesSystemTrustStore() const override { return true; }

  bool IsKnownRoot(const ParsedCertificate* trust_anchor) const override {
    return g_root_certs_fuchsia.Get().system_trust_store()->Contains(
        trust_anchor);
  }
};

std::unique_ptr<SystemTrustStore> CreateSslSystemTrustStore() {
  return std::make_unique<SystemTrustStoreFuchsia>();
}

#elif defined(OS_LINUX) || defined(OS_ANDROID)

namespace {

// Copied from https://golang.org/src/crypto/x509/root_linux.go
// Possible certificate files; stop after finding one.
constexpr std::array<const char*, 6> kStaticRootCertFiles = {
    "/etc/ssl/certs/ca-certificates.crt",  // Debian/Ubuntu/Gentoo etc.
    "/etc/pki/tls/certs/ca-bundle.crt",    // Fedora/RHEL 6
    "/etc/ssl/ca-bundle.pem",              // OpenSUSE
    "/etc/pki/tls/cacert.pem",             // OpenELEC
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  // CentOS/RHEL 7
    "/etc/ssl/cert.pem",                                  // Alpine Linux
};

// Possible directories with certificate files; stop after successfully
// reading at least one file from a directory.
constexpr std::array<const char*, 3> kStaticRootCertDirs = {
    "/etc/ssl/certs",      // SLES10/SLES11, https://golang.org/issue/12139
    "/etc/pki/tls/certs",  // Fedora/RHEL
    "/system/etc/security/cacerts",  // Android
};

// The environment variable which identifies where to locate the SSL
// certificate file. If set this overrides the system default.
constexpr char kStaticCertFileEnv[] = "SSL_CERT_FILE";

// The environment variable which identifies which directory to check for SSL
// certificate files. If set this overrides the system default. It is a colon
// separated list of directories.
// See https://www.openssl.org/docs/man1.0.2/man1/c_rehash.html.
constexpr char kStaticCertDirsEnv[] = "SSL_CERT_DIR";

class StaticUnixSystemCerts {
 public:
  StaticUnixSystemCerts() {
    auto env = base::Environment::Create();
    std::string env_value;

    std::vector<std::string> cert_filenames(kStaticRootCertFiles.begin(),
                                            kStaticRootCertFiles.end());
    if (env->GetVar(kStaticCertFileEnv, &env_value) && !env_value.empty()) {
      cert_filenames = {env_value};
    }

    bool cert_file_ok = false;
    for (const auto& filename : cert_filenames) {
      std::string file;
      if (!base::ReadFileToString(base::FilePath(filename), &file))
        continue;
      if (AddCertificatesFromBytes(file.data(), file.size())) {
        cert_file_ok = true;
        break;
      }
    }

    std::vector<std::string> cert_dirnames(kStaticRootCertDirs.begin(),
                                           kStaticRootCertDirs.end());
    if (env->GetVar(kStaticCertDirsEnv, &env_value) && !env_value.empty()) {
      cert_dirnames = base::SplitString(env_value, ":", base::TRIM_WHITESPACE,
                                        base::SPLIT_WANT_NONEMPTY);
    }

    bool cert_dir_ok = false;
    for (const auto& dir : cert_dirnames) {
      base::FileEnumerator e(base::FilePath(dir),
                             /*recursive=*/true, base::FileEnumerator::FILES);
      for (auto filename = e.Next(); !filename.empty(); filename = e.Next()) {
        std::string file;
        if (!base::ReadFileToString(filename, &file)) {
          continue;
        }
        if (AddCertificatesFromBytes(file.data(), file.size())) {
          cert_dir_ok = true;
        }
      }
      if (cert_dir_ok)
        break;
    }

    if (!cert_file_ok && !cert_dir_ok) {
      LOG(ERROR) << "No CA certificates were found. Try using environment "
                    "variable SSL_CERT_FILE or SSL_CERT_DIR";
    }
  }

  TrustStoreInMemory* system_trust_store() { return &system_trust_store_; }

 private:
  bool AddCertificatesFromBytes(const char* data, size_t length) {
    auto certs = X509Certificate::CreateCertificateListFromBytes(
        data, length, X509Certificate::FORMAT_AUTO);
    bool certs_ok = false;
    for (const auto& cert : certs) {
      CertErrors errors;
      auto parsed = ParsedCertificate::Create(
          bssl::UpRef(cert->cert_buffer()),
          x509_util::DefaultParseCertificateOptions(), &errors);
      if (parsed) {
        if (!system_trust_store_.Contains(parsed.get())) {
          system_trust_store_.AddTrustAnchor(parsed);
        }
        certs_ok = true;
      } else {
        LOG(ERROR) << errors.ToDebugString();
      }
    }
    return certs_ok;
  }

  TrustStoreInMemory system_trust_store_;
};

base::LazyInstance<StaticUnixSystemCerts>::Leaky g_root_certs_static_unix =
    LAZY_INSTANCE_INITIALIZER;

}  // namespace

class SystemTrustStoreStaticUnix : public BaseSystemTrustStore {
 public:
  SystemTrustStoreStaticUnix() {
    trust_store_.AddTrustStore(
        g_root_certs_static_unix.Get().system_trust_store());
  }

  bool UsesSystemTrustStore() const override { return true; }

  bool IsKnownRoot(const ParsedCertificate* trust_anchor) const override {
    return g_root_certs_static_unix.Get().system_trust_store()->Contains(
        trust_anchor);
  }
};

std::unique_ptr<SystemTrustStore> CreateSslSystemTrustStore() {
  return std::make_unique<SystemTrustStoreStaticUnix>();
}

#else

std::unique_ptr<SystemTrustStore> CreateSslSystemTrustStore() {
  return std::make_unique<DummySystemTrustStore>();
}

#endif

std::unique_ptr<SystemTrustStore> CreateEmptySystemTrustStore() {
  return std::make_unique<DummySystemTrustStore>();
}

}  // namespace net
