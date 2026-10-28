#pragma once

#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>

#include "result.hpp"

namespace reservations {

inline std::string Trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
    value.remove_suffix(1);
  }
  return std::string(value);
}

/// Loads KEY=VALUE pairs from a .env file into the process environment (does not override
/// variables that are already set).
inline Result<void> LoadDotEnv(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    return Result<void>::Failure(Error::Make(ErrorCode::ConfigError, "Unable to open " + path));
  }

  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const auto eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    auto key = Trim(line.substr(0, eq));
    auto value = Trim(line.substr(eq + 1));
    if (!key.empty() && std::getenv(key.c_str()) == nullptr) {
#if defined(_WIN32)
      _putenv_s(key.c_str(), value.c_str());
#else
      setenv(key.c_str(), value.c_str(), 0);
#endif
    }
  }
  return Result<void>::Success();
}

inline Result<std::string> RequireEnv(std::string_view name) {
  const char* value = std::getenv(std::string(name).c_str());
  if (value == nullptr || *value == '\0') {
    return Result<std::string>::Failure(
        Error::Make(ErrorCode::ConfigError, "Missing required environment variable: " + std::string(name)));
  }
  return Result<std::string>::Success(std::string(value));
}

struct CouchbaseConfig {
  std::string connection_string;
  std::string username;
  std::string password;
  std::string bucket{"reservations"};
  std::string scope{"airline"};
  std::string management_host;
  int management_port{8091};
};

inline Result<CouchbaseConfig> LoadCouchbaseConfig() {
  auto connection = RequireEnv("CB_CONNECTION_STRING");
  if (!connection) {
    return Result<CouchbaseConfig>::Failure(connection.error());
  }
  auto username = RequireEnv("CB_USERNAME");
  if (!username) {
    return Result<CouchbaseConfig>::Failure(username.error());
  }
  auto password = RequireEnv("CB_PASSWORD");
  if (!password) {
    return Result<CouchbaseConfig>::Failure(password.error());
  }

  CouchbaseConfig cfg;
  cfg.connection_string = connection.value();
  cfg.username = username.value();
  cfg.password = password.value();

  if (const char* bucket = std::getenv("CB_BUCKET"); bucket != nullptr && *bucket != '\0') {
    cfg.bucket = bucket;
  }
  if (const char* scope = std::getenv("CB_SCOPE"); scope != nullptr && *scope != '\0') {
    cfg.scope = scope;
  }

  // couchbase://host or couchbases://host
  std::string host = cfg.connection_string;
  const auto scheme_end = host.find("://");
  if (scheme_end != std::string::npos) {
    host = host.substr(scheme_end + 3);
  }
  const auto slash = host.find('/');
  if (slash != std::string::npos) {
    host = host.substr(0, slash);
  }
  const auto comma = host.find(',');
  if (comma != std::string::npos) {
    host = host.substr(0, comma);
  }
  const auto colon = host.find(':');
  if (colon != std::string::npos) {
    host = host.substr(0, colon);
  }
  cfg.management_host = host;
  return Result<CouchbaseConfig>::Success(std::move(cfg));
}

}  // namespace reservations
