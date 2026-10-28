#pragma once

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace reservations {

enum class ErrorCode {
  ValidationFailed,
  FlightNotFound,
  FlightClosedForSales,
  FlightFull,
  SeatNotFound,
  SeatUnavailable,
  SeatRestrictedForPassenger,
  HoldExpired,
  HoldNotFound,
  SeatMapStale,
  PriceChanged,
  PaymentDeclined,
  PaymentActionRequired,
  DuplicateRequest,
  RateLimited,
  InternalError,
  VersionConflict,
  NotFound,
  AlreadyExists,
  ConfigError,
  ConnectionError,
};

[[nodiscard]] constexpr const char* ToWireCode(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::ValidationFailed:
      return "VALIDATION_FAILED";
    case ErrorCode::FlightNotFound:
      return "FLIGHT_NOT_FOUND";
    case ErrorCode::FlightClosedForSales:
      return "FLIGHT_CLOSED_FOR_SALES";
    case ErrorCode::FlightFull:
      return "FLIGHT_CLOSED_FOR_SALES";
    case ErrorCode::SeatNotFound:
      return "SEAT_NOT_FOUND";
    case ErrorCode::SeatUnavailable:
      return "SEAT_UNAVAILABLE";
    case ErrorCode::SeatRestrictedForPassenger:
      return "SEAT_RESTRICTED_FOR_PASSENGER";
    case ErrorCode::HoldExpired:
      return "HOLD_EXPIRED";
    case ErrorCode::HoldNotFound:
      return "HOLD_NOT_FOUND";
    case ErrorCode::SeatMapStale:
      return "SEAT_MAP_STALE";
    case ErrorCode::PriceChanged:
      return "PRICE_CHANGED";
    case ErrorCode::PaymentDeclined:
      return "PAYMENT_DECLINED";
    case ErrorCode::PaymentActionRequired:
      return "PAYMENT_ACTION_REQUIRED";
    case ErrorCode::DuplicateRequest:
      return "DUPLICATE_REQUEST";
    case ErrorCode::RateLimited:
      return "RATE_LIMITED";
    case ErrorCode::InternalError:
      return "INTERNAL_ERROR";
    case ErrorCode::VersionConflict:
      return "INTERNAL_ERROR";
    case ErrorCode::NotFound:
      return "INTERNAL_ERROR";
    case ErrorCode::AlreadyExists:
      return "INTERNAL_ERROR";
    case ErrorCode::ConfigError:
      return "INTERNAL_ERROR";
    case ErrorCode::ConnectionError:
      return "INTERNAL_ERROR";
  }
  return "INTERNAL_ERROR";
}

[[nodiscard]] constexpr bool IsRetryable(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::RateLimited:
    case ErrorCode::InternalError:
    case ErrorCode::VersionConflict:
    case ErrorCode::ConnectionError:
      return true;
    case ErrorCode::ValidationFailed:
    case ErrorCode::FlightNotFound:
    case ErrorCode::FlightClosedForSales:
    case ErrorCode::FlightFull:
    case ErrorCode::SeatNotFound:
    case ErrorCode::SeatUnavailable:
    case ErrorCode::SeatRestrictedForPassenger:
    case ErrorCode::HoldExpired:
    case ErrorCode::HoldNotFound:
    case ErrorCode::SeatMapStale:
    case ErrorCode::PriceChanged:
    case ErrorCode::PaymentDeclined:
    case ErrorCode::PaymentActionRequired:
    case ErrorCode::DuplicateRequest:
    case ErrorCode::NotFound:
    case ErrorCode::AlreadyExists:
    case ErrorCode::ConfigError:
      return false;
  }
  return false;
}

struct Error {
  ErrorCode code{ErrorCode::InternalError};
  std::string message;
  bool retryable{false};

  static Error Make(ErrorCode code, std::string message) {
    return Error{.code = code, .message = std::move(message), .retryable = IsRetryable(code)};
  }
};

template <typename T>
class Result {
 public:
  static Result Success(T value) { return Result(std::move(value)); }
  static Result Failure(Error error) { return Result(std::move(error)); }

  [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(data_); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] T& value() & { return std::get<T>(data_); }
  [[nodiscard]] const T& value() const& { return std::get<T>(data_); }
  [[nodiscard]] T&& value() && { return std::get<T>(std::move(data_)); }

  [[nodiscard]] const Error& error() const { return std::get<Error>(data_); }

 private:
  explicit Result(T value) : data_(std::move(value)) {}
  explicit Result(Error error) : data_(std::move(error)) {}

  std::variant<T, Error> data_;
};

template <>
class Result<void> {
 public:
  static Result Success() { return Result(true); }
  static Result Failure(Error error) { return Result(std::move(error)); }

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] explicit operator bool() const noexcept { return ok_; }

  [[nodiscard]] const Error& error() const { return error_; }

 private:
  explicit Result(bool /*ok*/) : ok_(true) {}
  explicit Result(Error error) : ok_(false), error_(std::move(error)) {}

  bool ok_{false};
  Error error_{};
};

}  // namespace reservations
