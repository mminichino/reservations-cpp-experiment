# reservations-cpp-experiment

C++ airline reservation experiment that backs domain classes with **Couchbase 8.x** using **[reflect-cpp](https://github.com/munich-data-engineering/reflect-cpp)** for JSON serialization—standing in for a legacy object database on a scale-out Linux deployment.

---

## Download the Linux zip (latest release)

Each version tag (for example `v0.1.0`) publishes a GitHub Release with a Linux x86_64 zip (pre-built `load` / `exercise` plus sources). The movable **`latest`** tag/release always points at the newest versioned release and exposes a **stable asset name**.

```bash
curl -OLs https://github.com/mminichino/reservations-cpp-experiment/releases/download/latest/reservations-cpp-experiment-latest-linux-x86_64.zip
```
```bash
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
gh release download latest -R mminichino/reservations-cpp-experiment \
  -p 'reservations-cpp-experiment-latest-linux-x86_64.zip'
```

Pinned/reproducible downloads should use a version tag instead, for example:

`https://github.com/mminichino/reservations-cpp-experiment/releases/download/v0.1.0/reservations-cpp-experiment-v0.1.0-linux-x86_64.zip`

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

**macOS** (Homebrew — uses `brew --prefix` for `CMAKE_PREFIX_PATH`):

```bash
make            # Release → build/load, build/exercise
make debug      # Debug build
make clean      # remove build/
```

Or invoke CMake directly:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build -j
```

**Ubuntu 24.04+** (system packages; no Homebrew prefix needed):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

reflect-cpp is fetched automatically via CMake `FetchContent` (pinned to v0.26.2).

Binaries:

- `build/load` — ensure topology, bump epoch, **fully materialize** inventory/seatmap/flight/flight_index
- `build/exercise` — availability (read) + booking (CAS update) load test

### Run the experiment

**1. Materialize the catalog (required before `exercise`)**

```bash
./build/load .env -j 64          # parallel seed (default -j = hardware concurrency)
./build/load .env -d -j 64       # drop+recreate bucket, then full seed
./build/load .env --subset 10000 -j 32   # smoke seed only (not enough for full exercise)
```

`load` advances `catalog_meta.resetEpoch`, then upserts per-flight docs for the whole catalog (or `--subset` / `--start` range): empty inventory (`sold=0`), open seatmaps, flight docs, and `flight_index`. Progress lines print every **10s**; a telemetry summary prints when finished. Full catalog seed is large and can take a long time—that is expected.

**2. Exercise the reservation path**

```bash
./build/exercise .env [threads]
```

Default threads: `4`. Example:

```bash
./build/exercise .env 32
```

Workload mix:

- **99%** seat availability checks (KV **Get** only — no lazy create)
- **1%** ticket purchases (`BookFlight` — inventory **Get** + CAS **Replace**)

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

### Catalog materialization

The catalog is large (see below). `load` writes inventory, seatmap, flight, and `flight_index` documents for every flight (or a `--subset`) up front. `exercise` only reads those docs (and CAS-updates inventory on book)—it does not create missing documents.

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

`load` bumps `catalog_meta.resetEpoch`, then rematerializes per-flight docs with that epoch (empty/open). `exercise` requires docs present at the current epoch—missing or stale docs are hard errors (run `load` again). Use `-d` to drop and recreate the bucket for a clean slate.

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
apps/load/          Epoch bump + parallel full materialization (-j, -d, --subset)
apps/exercise/      99/1 availability (read) vs book (CAS) load test
src/common/         Result/error model, env, clock, ids
src/domain/         Reflected document + message types
src/platform/       Couchbase C++ SDK client, DocumentStore<T>
src/schedule/       Catalog scale and deterministic flight ids
src/seatmap/        ReservationService, synthetic layout, seed
schema/             airline-reservation.schema.json
```

### Agent / contributor notes

Project conventions for airline domain modeling, safety, and C++ style live in `AGENTS.md` and `.cursor/rules/`. Synthetic fixtures must stay obviously fake and schema-valid (see the synthetic-data rule).
