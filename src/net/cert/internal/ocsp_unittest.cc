// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/cert/internal/ocsp.h"

#include "base/base64.h"
#include "base/strings/string_piece.h"
#include "base/strings/string_util.h"
#include "net/cert/internal/test_helpers.h"
#include "net/der/encode_values.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/boringssl/src/include/openssl/pool.h"
#include "url/gurl.h"

namespace net {

namespace {

const base::TimeDelta kOCSPAgeOneWeek = base::TimeDelta::FromDays(7);

std::string GetFilePath(const std::string& file_name) {
  return std::string("net/data/ocsp_unittest/") + file_name;
}

scoped_refptr<ParsedCertificate> ParseCertificate(base::StringPiece data) {
  CertErrors errors;
  return ParsedCertificate::Create(
      bssl::UniquePtr<CRYPTO_BUFFER>(CRYPTO_BUFFER_new(
          reinterpret_cast<const uint8_t*>(data.data()), data.size(), nullptr)),
      {}, &errors);
}

struct TestParams {
  const char* file_name;
  OCSPRevocationStatus expected_revocation_status;
  OCSPVerifyResult::ResponseStatus expected_response_status;
};

class CheckOCSPTest : public ::testing::TestWithParam<TestParams> {};

const TestParams kTestParams[] = {
    {"good_response.pem", OCSPRevocationStatus::GOOD,
     OCSPVerifyResult::PROVIDED},

    {"good_response_sha256.pem", OCSPRevocationStatus::GOOD,
     OCSPVerifyResult::PROVIDED},

    {"no_response.pem", OCSPRevocationStatus::UNKNOWN,
     OCSPVerifyResult::NO_MATCHING_RESPONSE},

    {"malformed_request.pem", OCSPRevocationStatus::UNKNOWN,
     OCSPVerifyResult::ERROR_RESPONSE},

    {"bad_status.pem", OCSPRevocationStatus::UNKNOWN,
     OCSPVerifyResult::PARSE_RESPONSE_ERROR},

    {"bad_ocsp_type.pem", OCSPRevocationStatus::UNKNOWN,
     OCSPVerifyResult::PARSE_RESPONSE_ERROR},

    {"bad_signature.pem", OCSPRevocationStatus::UNKNOWN,
     OCSPVerifyResult::PROVIDED},

    {"ocsp_sign_direct.pem", OCSPRevocationStatus::GOOD,
     OCSPVerifyResult::PROVIDED},

    {"ocsp_sign_indirect.pem", OCSPRevocationStatus::GOOD,
     OCSPVerifyResult::PROVIDED},

    {"ocsp_sign_indirect_missing.pem", OCSPRevocationStatus::UNKNOWN,
     OCSPVerifyResult::PROVIDED},

    {"ocsp_sign_bad_indirect.pem", OCSPRevocationStatus::UNKNOWN,
     OCSPVerifyResult::PROVIDED},

    {"ocsp_extra_certs.pem", OCSPRevocationStatus::GOOD,
     OCSPVerifyResult::PROVIDED},

    {"has_version.pem", OCSPRevocationStatus::GOOD, OCSPVerifyResult::PROVIDED},

    {"responder_name.pem", OCSPRevocationStatus::GOOD,
     OCSPVerifyResult::PROVIDED},

    {"responder_id.pem", OCSPRevocationStatus::GOOD,
     OCSPVerifyResult::PROVIDED},

    {"has_extension.pem", OCSPRevocationStatus::GOOD,
     OCSPVerifyResult::PROVIDED},

    {"good_response_next_update.pem", OCSPRevocationStatus::GOOD,
     OCSPVerifyResult::PROVIDED},

    {"revoke_response.pem", OCSPRevocationStatus::REVOKED,
     OCSPVerifyResult::PROVIDED},

    {"revoke_response_reason.pem", OCSPRevocationStatus::REVOKED,
     OCSPVerifyResult::PROVIDED},

    {"unknown_response.pem", OCSPRevocationStatus::UNKNOWN,
     OCSPVerifyResult::PROVIDED},

    {"multiple_response.pem", OCSPRevocationStatus::UNKNOWN,
     OCSPVerifyResult::PROVIDED},

    {"other_response.pem", OCSPRevocationStatus::UNKNOWN,
     OCSPVerifyResult::NO_MATCHING_RESPONSE},

    {"has_single_extension.pem", OCSPRevocationStatus::GOOD,
     OCSPVerifyResult::PROVIDED},

    {"has_critical_single_extension.pem", OCSPRevocationStatus::UNKNOWN,
     OCSPVerifyResult::UNHANDLED_CRITICAL_EXTENSION},

    {"has_critical_response_extension.pem", OCSPRevocationStatus::UNKNOWN,
     OCSPVerifyResult::UNHANDLED_CRITICAL_EXTENSION},

    {"has_critical_ct_extension.pem", OCSPRevocationStatus::GOOD,
     OCSPVerifyResult::PROVIDED},

    {"missing_response.pem", OCSPRevocationStatus::UNKNOWN,
     OCSPVerifyResult::NO_MATCHING_RESPONSE},
};

// Parameterised test name generator for tests depending on RenderTextBackend.
struct PrintTestName {
  std::string operator()(const testing::TestParamInfo<TestParams>& info) const {
    base::StringPiece name(info.param.file_name);
    // Strip ".pem" from the end as GTest names cannot contain period.
    name.remove_suffix(4);
    return std::string(name);
  }
};

INSTANTIATE_TEST_SUITE_P(All,
                         CheckOCSPTest,
                         ::testing::ValuesIn(kTestParams),
                         PrintTestName());

TEST_P(CheckOCSPTest, FromFile) {
  const TestParams& params = GetParam();

  std::string ocsp_data;
  std::string ca_data;
  std::string cert_data;
  std::string request_data;
  const PemBlockMapping mappings[] = {
      {"OCSP RESPONSE", &ocsp_data},
      {"CA CERTIFICATE", &ca_data},
      {"CERTIFICATE", &cert_data},
      {"OCSP REQUEST", &request_data},
  };

  ASSERT_TRUE(ReadTestDataFromPemFile(GetFilePath(params.file_name), mappings));

  // Mar 5 00:00:00 2017 GMT
  base::Time kVerifyTime =
      base::Time::UnixEpoch() + base::TimeDelta::FromSeconds(1488672000);

  // Test that CheckOCSP() works.
  OCSPVerifyResult::ResponseStatus response_status;
  OCSPRevocationStatus revocation_status =
      CheckOCSP(ocsp_data, cert_data, ca_data, kVerifyTime, kOCSPAgeOneWeek,
                &response_status);

  EXPECT_EQ(params.expected_revocation_status, revocation_status);
  EXPECT_EQ(params.expected_response_status, response_status);

  // Check that CreateOCSPRequest() works.
  scoped_refptr<ParsedCertificate> cert = ParseCertificate(cert_data);
  ASSERT_TRUE(cert);

  scoped_refptr<ParsedCertificate> issuer = ParseCertificate(ca_data);
  ASSERT_TRUE(issuer);

  std::vector<uint8_t> encoded_request;
  ASSERT_TRUE(CreateOCSPRequest(cert.get(), issuer.get(), &encoded_request));

  EXPECT_EQ(der::Input(encoded_request.data(), encoded_request.size()),
            der::Input(&request_data));
}

base::StringPiece kGetURLTestParams[] = {
    "http://www.example.com/", "http://www.example.com/path/",
    "http://www.example.com/path",
    "http://www.example.com/path?query"
    "http://user:pass@www.example.com/path?query",
};

class CreateOCSPGetURLTest
    : public ::testing::TestWithParam<base::StringPiece> {};

INSTANTIATE_TEST_SUITE_P(All,
                         CreateOCSPGetURLTest,
                         ::testing::ValuesIn(kGetURLTestParams));

TEST_P(CreateOCSPGetURLTest, Basic) {
  std::string ca_data;
  std::string cert_data;
  std::string request_data;
  const PemBlockMapping mappings[] = {
      {"CA CERTIFICATE", &ca_data},
      {"CERTIFICATE", &cert_data},
      {"OCSP REQUEST", &request_data},
  };

  // Load one of the test files. (Doesn't really matter which one as
  // constructing the DER is tested elsewhere).
  ASSERT_TRUE(
      ReadTestDataFromPemFile(GetFilePath("good_response.pem"), mappings));

  scoped_refptr<ParsedCertificate> cert = ParseCertificate(cert_data);
  ASSERT_TRUE(cert);

  scoped_refptr<ParsedCertificate> issuer = ParseCertificate(ca_data);
  ASSERT_TRUE(issuer);

  GURL url = CreateOCSPGetURL(cert.get(), issuer.get(), GetParam());

  // Try to extract the encoded data and compare against |request_data|.
  //
  // A known answer output test would be better as this just reverses the logic
  // from the implementaiton file.
  std::string b64 = url.spec().substr(GetParam().size() + 1);

  // Hex un-escape the data.
  base::ReplaceSubstringsAfterOffset(&b64, 0, "%2B", "+");
  base::ReplaceSubstringsAfterOffset(&b64, 0, "%2F", "/");
  base::ReplaceSubstringsAfterOffset(&b64, 0, "%3D", "=");

  // Base64 decode the data.
  std::string decoded;
  ASSERT_TRUE(base::Base64Decode(b64, &decoded));

  EXPECT_EQ(request_data, decoded);
}

}  // namespace

}  // namespace net
