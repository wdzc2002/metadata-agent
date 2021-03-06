/*
 * Copyright 2017 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include "oauth2.h"

#define BOOST_NETWORK_ENABLE_HTTPS
#include <boost/network/protocol/http/client.hpp>
#include <network/uri/detail/encode.hpp>
#include <chrono>
#include <cstdlib>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>

#include "base64.h"
#include "format.h"
#include "http_common.h"
#include "json.h"
#include "logging.h"
#include "time.h"

namespace http = boost::network::http;

namespace google {

namespace {

template<class T, class R, R(*D)(T*)>
struct Deleter {
  void operator()(T* p) { if (p) D(p); }
};

class PKey {
  using PKCS8_Deleter =
      Deleter<PKCS8_PRIV_KEY_INFO, void, PKCS8_PRIV_KEY_INFO_free>;
  using BIO_Deleter = Deleter<BIO, int, BIO_free>;
  using EVP_PKEY_Deleter = Deleter<EVP_PKEY, void, EVP_PKEY_free>;
 public:
  PKey() {}
  PKey(const std::string& private_key_pem) {
    std::unique_ptr<BIO, BIO_Deleter> buf(
        BIO_new_mem_buf((void*) private_key_pem.data(), -1));
    if (buf == nullptr) {
      LOG(ERROR) << "BIO_new_mem_buf failed";
      return;
    }
    std::unique_ptr<PKCS8_PRIV_KEY_INFO, PKCS8_Deleter> p8inf(
        PEM_read_bio_PKCS8_PRIV_KEY_INFO(buf.get(), NULL, NULL, NULL));
    if (p8inf == nullptr) {
      LOG(ERROR) << "PEM_read_bio_PKCS8_PRIV_KEY_INFO failed";
      return;
    }
    private_key_.reset(EVP_PKCS82PKEY(p8inf.get()));
    if (private_key_ == nullptr) {
      LOG(ERROR) << "EVP_PKCS82PKEY failed";
    }
  }

  std::string ToString() const {
    // TODO: Check for NULL and add exceptions.
    std::unique_ptr<BIO, BIO_Deleter> mem(BIO_new(BIO_s_mem()));
    EVP_PKEY_print_private(mem.get(), private_key_.get(), 0, NULL);
    char* pp;
    long len = BIO_get_mem_data(mem.get(), &pp);
    return std::string(pp, len);
  }

  EVP_PKEY* private_key() const {
    // A const_cast is fine because this is passed into functions that don't
    // modify pkey, but have silly signatures.
    return const_cast<EVP_PKEY*>(private_key_.get());
  }

 private:
  std::unique_ptr<EVP_PKEY, EVP_PKEY_Deleter> private_key_;
};

class Finally {
 public:
  Finally(std::function<void()> cleanup) : cleanup_(cleanup) {}
  ~Finally() { cleanup_(); }
 private:
  std::function<void()> cleanup_;
};

std::string Sign(const std::string& data, const PKey& pkey) {
#if 0
  LOG(ERROR) << "Signing '" << data << "' with '" << pkey.ToString() << "'";
#endif
  unsigned int capacity = EVP_PKEY_size(pkey.private_key());
  std::unique_ptr<unsigned char> result(new unsigned char[capacity]);

  char error[1024];

  EVP_MD_CTX ctx;
  EVP_SignInit(&ctx, EVP_sha256());

  Finally cleanup([&ctx, &error]() {
    if (EVP_MD_CTX_cleanup(&ctx) == 0) {
      ERR_error_string_n(ERR_get_error(), error, sizeof(error));
      LOG(ERROR) << "EVP_MD_CTX_cleanup failed: " << error;
    }
  });

  if (EVP_SignUpdate(&ctx, data.data(), data.size()) == 0) {
    ERR_error_string_n(ERR_get_error(), error, sizeof(error));
    LOG(ERROR) << "EVP_SignUpdate failed: " << error;
    return "";
  }

  unsigned int actual_result_size = 0;
  if (EVP_SignFinal(&ctx, result.get(), &actual_result_size,
                    pkey.private_key()) == 0) {
    ERR_error_string_n(ERR_get_error(), error, sizeof(error));
    LOG(ERROR) << "EVP_SignFinal failed: " << error;
    return "";
  }
#if 0
  for (int i = 0; i < actual_result_size; ++i) {
    LOG(ERROR) << "Signature char '" << static_cast<int>(result.get()[i])
               << "'";
  }
#endif
  return std::string(reinterpret_cast<char*>(result.get()), actual_result_size);
}

}

json::value OAuth2::ComputeTokenFromCredentials() const {
  const std::string service_account_email =
      environment_.CredentialsClientEmail();
  const std::string private_key_pem =
      environment_.CredentialsPrivateKey();
  if (private_key_pem.empty() || service_account_email.empty()) {
    return nullptr;
  }

  try {
    PKey private_key(private_key_pem);

    // Make a POST request to https://www.googleapis.com/oauth2/v3/token
    // with the body
    // grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer&assertion=$JWT_HEADER.$CLAIM_SET.$SIGNATURE
    //
    // The trailing part of that body has three variables that need to be
    // expanded.
    // Namely, $JWT_HEADER, $CLAIM_SET, and $SIGNATURE, separated by periods.
    //
    // $CLAIM_SET is a base64url encoding of a JSON object with five fields:
    // iss, scope, aud, exp, and iat.
    // iss: Service account email. We get this from user in the config file.
    // scope: Basically the requested scope (e.g. "permissions") for the token.
    //   For our purposes, this is the constant string
    //   "https://www.googleapis.com/auth/monitoring".
    // aud: Assertion target. Since we are asking for an access token, this is
    //   the constant string "https://www.googleapis.com/oauth2/v3/token". This
    //   is the same as the URL we are posting to.
    // iat: Time of the assertion (i.e. now) in units of "seconds from Unix
    //   epoch".
    // exp: Expiration of assertion. For us this is 'iat' + 3600 seconds.
    //
    // $SIGNATURE is the base64url encoding of the signature of the string
    // $JWT_HEADER.$CLAIM_SET
    // where $JWT_HEADER and $CLAIM_SET are defined as above. Note that they are
    // separated by the period character. The signature algorithm used should be
    // SHA-256. The private key used to sign the data comes from the user. The
    // private key to use is the one associated with the service account email
    // address (i.e. the email address specified in the 'iss' field above).

    if (environment_.config().VerboseLogging()) {
      LOG(INFO) << "Getting an OAuth2 token";
    }
    http::client client;
    http::client::request request("https://www.googleapis.com/oauth2/v3/token");
    std::string grant_type = ::network::detail::encode_fragment(
        std::string("urn:ietf:params:oauth:grant-type:jwt-bearer"));
    //std::string jwt_header = "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9";
    //std::string jwt_header =
    //    base64::Encode("{\"alg\":\"RS256\",\"typ\":\"JWT\"}");
    json::value jwt_object = json::object({
      {"alg", json::string("RS256")},
      {"typ", json::string("JWT")},
    });
    std::string jwt_header = base64::Encode(jwt_object->ToString());
    auto now = std::chrono::system_clock::now();
    auto exp = now + std::chrono::seconds(3600);
    json::value claim_set_object = json::object({
      {"iss", json::string(service_account_email)},
      {"scope", json::string("https://www.googleapis.com/auth/monitoring")},
      {"aud", json::string("https://www.googleapis.com/oauth2/v3/token")},
      {"iat", json::number(time::SecondsSinceEpoch(now))},
      {"exp", json::number(time::SecondsSinceEpoch(exp))},
    });
    if (environment_.config().VerboseLogging()) {
      LOG(INFO) << "claim_set = " << claim_set_object->ToString();
    }
    std::string claim_set = base64::Encode(claim_set_object->ToString());
    if (environment_.config().VerboseLogging()) {
      LOG(INFO) << "encoded claim_set = " << claim_set;
    }
    std::string signature = base64::Encode(
        Sign(jwt_header + "." + claim_set, private_key));
    std::string request_body =
        "grant_type=" + grant_type + "&assertion=" +
        jwt_header + "." + claim_set + "." + signature;
    // TODO: Investigate whether we need this header.
    //request << boost::network::header("Connection", "close");
    request << boost::network::header("Content-Length",
                                      std::to_string(request_body.size()));
    request << boost::network::header("Content-Type",
                                      "application/x-www-form-urlencoded");
    request << boost::network::body(request_body);
    if (environment_.config().VerboseLogging()) {
      LOG(INFO) << "About to send request: " << request.uri().string()
                << " headers: " << request.headers()
                << " body: " << request.body();
    }
    http::client::response response = client.post(request);
    if (status(response) >= 300) {
      throw boost::system::system_error(
          boost::system::errc::make_error_code(boost::system::errc::not_connected),
          format::Substitute("Server responded with '{{message}}' ({{code}})",
                             {{"message", status_message(response)},
                              {"code", format::str(status(response))}}));
    }
    if (environment_.config().VerboseLogging()) {
      LOG(INFO) << "Token response: " << body(response);
    }
    json::value parsed_token = json::Parser::FromString(body(response));
    if (environment_.config().VerboseLogging()) {
      LOG(INFO) << "Parsed token: " << *parsed_token;
    }

    return parsed_token;
  } catch (const json::Exception& e) {
    LOG(ERROR) << e.what();
    return nullptr;
  } catch (const boost::system::system_error& e) {
    LOG(ERROR) << "HTTP error: " << e.what();
    return nullptr;
  }
}

json::value OAuth2::GetMetadataToken() const {
  std::string token_response =
      environment_.GetMetadataString("instance/service-accounts/default/token");
  if (token_response.empty()) {
    return nullptr;
  }
  if (environment_.config().VerboseLogging()) {
    LOG(INFO) << "Token response: " << token_response;
  }
  json::value parsed_token = json::Parser::FromString(token_response);
  if (environment_.config().VerboseLogging()) {
    LOG(INFO) << "Parsed token: " << *parsed_token;
  }
  return parsed_token;
}

std::string OAuth2::GetAuthHeaderValue() {
  // Build in a 60 second slack to avoid timing problems (clock skew, races).
  if (auth_header_value_.empty() ||
      token_expiration_ <
          std::chrono::system_clock::now() + std::chrono::seconds(60)) {
    // Token expired; retrieve new value.
    json::value token_json = ComputeTokenFromCredentials();
    if (token_json == nullptr) {
      LOG(INFO) << "Getting auth token from metadata server";
      token_json = std::move(GetMetadataToken());
    }
    if (token_json == nullptr) {
      LOG(ERROR) << "Unable to get auth token";
      return "";
    }
    try {
      // This object should be of the form:
      // {
      //  "access_token" : $THE_ACCESS_TOKEN,
      //  "token_type" : "Bearer",
      //  "expires_in" : 3600
      // }
      const json::Object* token = token_json->As<json::Object>();

      const std::string access_token =
          token->Get<json::String>("access_token");
      const std::string token_type =
          token->Get<json::String>("token_type");
      const double expires_in =
          token->Get<json::Number>("expires_in");

      if (token_type != "Bearer") {
        LOG(ERROR) << "Token type is not 'Bearer', but '" << token_type << "'";
      }

      auth_header_value_ = token_type + " " + access_token;
      token_expiration_ =
          std::chrono::system_clock::now() +
          std::chrono::seconds(static_cast<long>(expires_in));
    } catch (const json::Exception& e) {
      LOG(ERROR) << e.what();
      return "";
    }
  }
  return auth_header_value_;
}

}  // google

