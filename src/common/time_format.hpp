#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace reservations {

inline std::string FormatRfc3339(std::chrono::system_clock::time_point tp) {
  const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(tp);
  const std::time_t tt = std::chrono::system_clock::to_time_t(seconds);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &tt);
#else
  gmtime_r(&tt, &tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

inline std::chrono::system_clock::time_point ParseRfc3339(const std::string& value) {
  std::tm tm{};
  std::istringstream iss(value);
  // Accept both ...Z and ...+/-HH:MM by parsing the core timestamp.
  iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
  if (iss.fail()) {
    return std::chrono::system_clock::time_point{};
  }
#if defined(_WIN32)
  const std::time_t tt = _mkgmtime(&tm);
#else
  const std::time_t tt = timegm(&tm);
#endif
  return std::chrono::system_clock::from_time_t(tt);
}

}  // namespace reservations
