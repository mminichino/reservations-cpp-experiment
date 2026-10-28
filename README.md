# reservations-cpp-experiment

C++ airline reservation experiment that backs domain classes with **Couchbase 8.x** using **[reflect-cpp](https://github.com/munich-data-engineering/reflect-cpp)** for JSON serialization—standing in for a legacy object database on a scale-out Linux deployment.

---

## Download the Linux zip (latest release)

Each version tag (for example `v0.1.0`) publishes a GitHub Release with a Linux x86_64 zip (pre-built `load` / `exercise` plus sources). The movable **`latest`** tag/release always points at the newest versioned release and exposes a **stable asset name**.

Replace `OWNER/REPO` with this GitHub repository (for example `my-org/reservations-cpp-experiment`):

```bash
# Stable URL via the "latest" release asset name
curl -fsSL -o reservations-cpp-experiment-linux.zip \
  "https://github.com/OWNER/REPO/releases/download/latest/reservations-cpp-experiment-latest-linux-x86_64.zip"

unzip reservations-cpp-experiment-linux.zip
cd reservations-cpp-experiment-*-linux-x86_64

# Runtime libraries (Ubuntu 24.04 / noble) — Couchbase C++ SDK + curl/fmt
sudo apt update
sudo apt install -y curl gpg
DIST_ARCH=$(. /etc/os-release; echo ${VERSION_CODENAME}/$(uname -m))
curl -fsSL "https://packages.couchbase.com/clients/cxx/repos/deb/${DIST_ARCH}/DEB-GPG-KEY.txt" \
  | sudo gpg --yes --dearmor -o /usr/share/keyrings/couchbase-archive-keyring.gpg
sudo curl -fsSL -o /etc/apt/sources.list.d/couchbase-cxx-client.sources \
  "https://packages.couchbase.com/clients/cxx/repos/deb/${DIST_ARCH}/couchbase-cxx-client.sources"
sudo apt update
sudo apt install -y couchbase-cxx-client libcurl4 libfmt9

cp .env.example .env   # edit CB_* credentials
./bin/load .env
./bin/exercise .env 4
```

Or with the GitHub CLI:

```bash
gh release download latest -R OWNER/REPO \
  -p 'reservations-cpp-experiment-latest-linux-x86_64.zip'
```

Pinned/reproducible downloads should use a version tag instead, for example:

`https://github.com/OWNER/REPO/releases/download/v0.1.0/reservations-cpp-experiment-v0.1.0-linux-x86_64.zip`

To publish a new release locally after pushing code:

```bash
git tag v0.1.0
git push origin v0.1.0
```

The [Release](.github/workflows/release.yml) workflow builds on Ubuntu 24.04, uploads the zip to the version release, force-updates the `latest` tag to that commit, and refreshes the `latest` GitHub Release.

---

## Building and running

### Prerequisites

- CMake **3.25+**
- A C++20 compiler (Apple Clang / GCC 11+ / Clang 14+)
- [Couchbase C++ SDK](https://docs.couchbase.com/cxx-sdk/current/project-docs/sdk-full-installation.html) (`couchbase-cxx-client`)
- libcurl, pkg-config, [fmt](https://fmt.dev/)
- A reachable Couchbase cluster (local or remote)

#### macOS (Homebrew)

```bash
brew install cmake curl fmt pkg-config
```

CMake will fetch the Couchbase C++ SDK via [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) when it is not already installed on the system (first configure is slower).

#### Ubuntu 24.04 (noble)

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libcurl4-openssl-dev libfmt-dev curl gpg

DIST_ARCH=$(. /etc/os-release; echo ${VERSION_CODENAME}/$(uname -m))
# e.g. noble/x86_64 or noble/aarch64
curl -fsSL "https://packages.couchbase.com/clients/cxx/repos/deb/${DIST_ARCH}/DEB-GPG-KEY.txt" \
  | sudo gpg --yes --dearmor -o /usr/share/keyrings/couchbase-archive-keyring.gpg
sudo curl -fsSL -o /etc/apt/sources.list.d/couchbase-cxx-client.sources \
  "https://packages.couchbase.com/clients/cxx/repos/deb/${DIST_ARCH}/couchbase-cxx-client.sources"
sudo apt update
sudo apt install -y couchbase-cxx-client couchbase-cxx-client-dev
```

See the [C++ SDK full installation](https://docs.couchbase.com/cxx-sdk/current/project-docs/sdk-full-installation.html) for other Debian/Ubuntu codenames.

### Configure credentials

Copy the example env file and set connection details (`.env` is git-ignored):

```bash
cp .env.example .env
```

Required variables:

| Variable | Example |
|---|---|
| `CB_CONNECTION_STRING` | `couchbase://127.0.0.1` |
| `CB_USERNAME` | `Administrator` |
| `CB_PASSWORD` | `…` |

Optional: `CB_BUCKET` (default `reservations`), `CB_SCOPE` (default `airline`).

### Build

**macOS** (Homebrew prefixes under `/opt/homebrew` on Apple Silicon):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/homebrew
cmake --build build -j
```

On Intel Macs with Homebrew under `/usr/local`, use `-DCMAKE_PREFIX_PATH=/usr/local` instead.

**Ubuntu 24.04+** (system packages; no Homebrew prefix needed):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

reflect-cpp is fetched automatically via CMake `FetchContent` (pinned to v0.26.2).

Binaries:

- `build/load` — reset catalog / ensure topology and seed meta
- `build/exercise` — availability + booking load test

### Run the experiment

**1. Reset inventory (run before a fresh test)**

```bash
./build/load .env
```

This ensures the `reservations` bucket (RAM quota = cluster Data Service `memoryQuota`), scope, and collections exist, then advances the catalog **reset epoch** so all flights are treated as fully open.

**2. Exercise the reservation path**

```bash
./build/exercise .env [threads]
```

Default threads: `4`. Example:

```bash
./build/exercise .env 32
```

Workload mix:

- **99%** seat availability checks on a random flight in the 52-week catalog
- **1%** ticket purchases (`BookFlight` against class inventory / AU)

Telemetry prints every 5 seconds. Stops on **Ctrl+C**, or when cumulative bookings reach total catalog authorization (AU).

---

## How reflect-cpp is used with Couchbase

The experiment’s persistence story is: **C++ structs are the document model**; reflect-cpp turns them into JSON; the Couchbase C++ SDK stores and loads that JSON in Couchbase collections.

### Document types as reflected structs

Domain aggregates (inventory, seat maps, PNRs, holds, idempotency records, catalog meta) are plain C++20 structs in `src/domain/`. reflect-cpp discovers their fields via reflection—no hand-written serializers.

### `DocumentStore<T>`: object-database-style persistence

`platform::DocumentStore<T>` is the adapter that replaces an object database “store/load object” API:

1. **Write** — `rfl::json::write(document)` → JSON string → Couchbase `upsert` / `insert` / `replace` (with optional CAS and TTL).
2. **Read** — Couchbase `get` → JSON bytes → `rfl::json::read<T>(…)` → typed `T` plus CAS.

```cpp
auto store = DocumentStore<InventoryDocument>(client, "inventory");
auto loaded = store.Get("inv::" + flightId);           // T + cas
store.Replace(key, updated, loaded->cas);               // optimistic concurrency
```

CAS-based `Replace` is how sell/hold paths avoid lost updates under concurrency.

### What Couchbase owns vs what reflect-cpp owns

| Layer | Responsibility |
|---|---|
| **reflect-cpp** | Struct ↔ JSON; field naming; enums; `std::optional` / vectors |
| **couchbase-cxx-client** | Connection, KV get/upsert/insert/replace/remove, collections |
| **Management REST** | Create bucket/scope/collections; read Data Service `memoryQuota` |

reflect-cpp does not speak the Couchbase protocol. It is the serialization boundary that lets application classes move to JSON documents the same way a classic object database once mapped C++ objects into its store.

### Lazy materialization

The catalog is large (see below). Flight inventory and seat maps are **not** all written at load time. On first access, if a document is missing or its `resetEpoch` is behind the catalog meta epoch, the service builds a fresh struct in memory, serializes it with reflect-cpp, and upserts it.

---

## How this project works

### Motivation

Simulate reservation software that once ran on scale-up UNIX with an object database, then moved to Linux scale-out with Couchbase. Class data that used to live in the object database is now JSON in Couchbase, with reflect-cpp providing the mapping.

Wire-level seat messages align with `schema/airline-reservation.schema.json`. Booking (class inventory) is separate from physical seat assignment.

### Catalog scale

| Dimension | Value |
|---|---|
| Airlines | 10 (`X0`–`X9`) |
| Flights / week / airline | 28,000 |
| Weeks bookable | 52 |
| **Total flights** | **14,560,000** |
| Oversell (AU) | 8% above physical capacity |

Flight identity is **deterministic** (UUID-shaped id from a global index), so any slot in the catalog can be addressed without listing every document.

### Reset epoch

`load` bumps `catalog_meta.resetEpoch`. Inventory and seatmap documents carry their own epoch. Stale or missing docs are treated as **fully open** and rewritten on next use—so a full inventory reset does not require rewriting millions of keys.

### Main APIs (`ReservationService`)

| Operation | Role |
|---|---|
| `CheckAvailability` | Seat-map view; succeeds with **zero seats** when nothing is selectable |
| `BookFlight` | Sell class inventory up to **authorization (AU)**; creates PNR without assigning a seat |
| `HoldSeats` / `PurchaseSeats` | Physical seat hold / assignment (ancillary path) |
| `ListBookableFlights` | Omit flights whose AU is exhausted |

Overselling bookings past physical seats is intentional (AU). Seat selection then simply shows an empty map rather than failing the ticket.

### Couchbase layout

Bucket: `reservations` (quota = cluster Data Service `memoryQuota`)  
Scope: `airline`

| Collection | Contents |
|---|---|
| `meta` | Catalog meta / reset epoch |
| `inventory` | Per-flight sold vs AU |
| `seatmaps` | Physical seat state |
| `flights` / `flight_index` | Optional persisted flight docs / schedule index |
| `pnrs` | Bookings |
| `holds` | Temporary seat holds (TTL) |
| `idempotency` | Booking / purchase idempotency keys |

### Repository layout

```
apps/load/          Reset epoch + ensure topology
apps/exercise/      99/1 availability vs book load test
src/common/         Result/error model, env, clock, ids
src/domain/         Reflected document + message types
src/platform/       Couchbase C++ SDK client, DocumentStore<T>
src/schedule/       Catalog scale and deterministic flight ids
src/seatmap/        ReservationService, synthetic layout, seed
schema/             airline-reservation.schema.json
```

### Agent / contributor notes

Project conventions for airline domain modeling, safety, and C++ style live in `AGENTS.md` and `.cursor/rules/`. Synthetic fixtures must stay obviously fake and schema-valid (see the synthetic-data rule).
