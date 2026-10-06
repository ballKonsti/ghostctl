#include "creds.hpp"

#include <libsecret/secret.h>

namespace ghost::creds {

namespace {

constexpr const char* SERVICE = "ghostctl";
constexpr const char* USER_KEY = "__username__";

// Same attributes as Python's keyring (SecretService backend), so logins saved
// by the old Python version are still found.
const SecretSchema* schema() {
  static const SecretSchema s = {
      "org.freedesktop.Secret.Generic",
      SECRET_SCHEMA_DONT_MATCH_NAME,
      {{"service", SECRET_SCHEMA_ATTRIBUTE_STRING},
       {"username", SECRET_SCHEMA_ATTRIBUTE_STRING},
       {"application", SECRET_SCHEMA_ATTRIBUTE_STRING},
       {nullptr, SecretSchemaAttributeType(0)}},
  };
  return &s;
}

std::optional<std::string> get(const std::string& user) {
  GError* err = nullptr;
  gchar* pw = secret_password_lookup_sync(schema(), nullptr, &err, "service", SERVICE, "username",
                                          user.c_str(), nullptr);
  if (err) {
    g_error_free(err);
    return std::nullopt;
  }
  if (!pw) return std::nullopt;
  std::string out = pw;
  secret_password_free(pw);
  return out;
}

bool put(const std::string& user, const std::string& value) {
  GError* err = nullptr;
  std::string label = "Password for '" + user + "' on '" + SERVICE + "'";
  bool ok = secret_password_store_sync(schema(), SECRET_COLLECTION_DEFAULT, label.c_str(), value.c_str(), nullptr,
                                       &err, "service", SERVICE, "username", user.c_str(), "application",
                                       "Python keyring library", nullptr);
  if (err) {
    g_error_free(err);
    return false;
  }
  return ok;
}

bool del(const std::string& user) {
  GError* err = nullptr;
  bool ok = secret_password_clear_sync(schema(), nullptr, &err, "service", SERVICE, "username", user.c_str(),
                                       nullptr);
  if (err) {
    g_error_free(err);
    return false;
  }
  return ok;
}

}  // namespace

bool available() {
  GError* err = nullptr;
  SecretService* svc = secret_service_get_sync(SECRET_SERVICE_NONE, nullptr, &err);
  if (err) {
    g_error_free(err);
    return false;
  }
  if (svc) g_object_unref(svc);
  return svc != nullptr;
}

std::optional<Creds> load() {
  auto user = get(USER_KEY);
  if (!user || user->empty()) return std::nullopt;
  auto pw = get(*user);
  if (!pw || pw->empty()) return std::nullopt;
  return Creds{*user, *pw};
}

bool save(const Creds& c) { return put(USER_KEY, c.username) && put(c.username, c.password); }

bool forget() {
  auto user = get(USER_KEY);
  if (!user) return false;
  del(*user);
  return del(USER_KEY);
}

}  // namespace ghost::creds
