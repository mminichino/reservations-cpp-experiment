#pragma once

#include <string>

namespace reservations::platform {

/// Tiny Couchbase document used for a cheap currency check before serving cached
/// price or schedule data. A future dedicated price/schedule load process can
/// Upsert these without rewriting the full catalog.
struct DataSnapshotDocument {
  std::string type;  // "price_snapshot" | "schedule_snapshot"
  std::string snapshotId;
  std::string snapshotDate;  // RFC 3339 when the snapshot became current
};

inline constexpr const char* kPriceSnapshotKey = "snapshot::prices";
inline constexpr const char* kScheduleSnapshotKey = "snapshot::schedule";

[[nodiscard]] inline DataSnapshotDocument MakePriceSnapshot(std::string id, std::string date) {
  return DataSnapshotDocument{.type = "price_snapshot",
                              .snapshotId = std::move(id),
                              .snapshotDate = std::move(date)};
}

[[nodiscard]] inline DataSnapshotDocument MakeScheduleSnapshot(std::string id, std::string date) {
  return DataSnapshotDocument{.type = "schedule_snapshot",
                              .snapshotId = std::move(id),
                              .snapshotDate = std::move(date)};
}

}  // namespace reservations::platform
