#pragma once

#include <array>
#include <cstdint>
#include <random>
#include <sstream>
#include <string>

namespace reservations {

/// Generates UUID version-4 strings (schema format: uuid).
class UuidGenerator {
 public:
  UuidGenerator() : rng_(std::random_device{}()) {}

  [[nodiscard]] std::string Next() {
    std::uniform_int_distribution<int> dist(0, 255);
    std::array<std::uint8_t, 16> bytes{};
    for (auto& b : bytes) {
      b = static_cast<std::uint8_t>(dist(rng_));
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80);

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(36);
    auto put = [&](std::size_t pos, std::uint8_t value) {
      out[pos] = kHex[(value >> 4) & 0xF];
      out[pos + 1] = kHex[value & 0xF];
    };
    put(0, bytes[0]);
    put(2, bytes[1]);
    put(4, bytes[2]);
    put(6, bytes[3]);
    out[8] = '-';
    put(9, bytes[4]);
    put(11, bytes[5]);
    out[13] = '-';
    put(14, bytes[6]);
    put(16, bytes[7]);
    out[18] = '-';
    put(19, bytes[8]);
    put(21, bytes[9]);
    out[23] = '-';
    put(24, bytes[10]);
    put(26, bytes[11]);
    put(28, bytes[12]);
    put(30, bytes[13]);
    put(32, bytes[14]);
    put(34, bytes[15]);
    return out;
  }

 private:
  std::mt19937 rng_;
};

/// Non-sequential record locators avoiding ambiguous characters (I, O, 0, 1).
class RecordLocatorGenerator {
 public:
  RecordLocatorGenerator() : rng_(std::random_device{}()) {}

  [[nodiscard]] std::string Next() {
    static constexpr char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::uniform_int_distribution<std::size_t> dist(0, sizeof(kAlphabet) - 2);
    std::string out(6, 'A');
    for (char& c : out) {
      c = kAlphabet[dist(rng_)];
    }
    return out;
  }

 private:
  std::mt19937 rng_;
};

}  // namespace reservations
