# AGENTS.md: Airline Reservation System (C++)

Instructions for AI coding agents (Cursor and others) working in this repository. This codebase implements components of a passenger reservation system in the style of production airline and GDS software (Amadeus, Sabre, and carrier systems at the scale of United or American). Write code as if it will run 24/7, handle revenue-critical transactions, and be audited.

When these instructions conflict with a user request, say so and ask before proceeding. When they are silent, choose the safer, more explicit option.

The wire-level contract is `schema/airline-reservation.schema.json` (seat map hold/purchase **and** O&D `ClassAvailability` / INTERNAL `MarketPriceSet` for conditional prices). Do not change it without being asked. Domain types in C++ must map to it one to one.

Class availability is decided per request: load the **complete** candidate price set for each market on the journey, match conditions, apply precedence, then weigh the winning value against seat controls. Never open/close a class from a partial price set.

---

## 1. Non-negotiables

1. **Never lose or duplicate money or inventory.** Every state change that touches a seat, a fare, a ticket, or a payment must be atomic, idempotent and auditable.
2. **Never oversell by accident.** Overbooking is a deliberate revenue-management policy applied through authorization levels, never a side effect of a race condition.
3. **Never log or store sensitive data in the clear.** No full card numbers, CVV, passport numbers, or full dates of birth in logs, traces, metrics, crash dumps or test fixtures.
4. **Never block on the hot path.** Availability and pricing are read-heavy and latency-sensitive. Do not add synchronous network calls, unbounded locks, or allocations in loops there without measurement.
5. **Fail closed.** If you cannot prove a seat is available, a price is valid or a payment succeeded, say no and return a typed error.
6. **Everything has a time limit.** Holds, price quotes, ticketing time limits, payment authorizations and every RPC have explicit deadlines.

---

## 2. Domain model

Use industry terms. Do not invent synonyms ("booking", "order", "reservation") for existing concepts without noting the mapping.

| Concept | Meaning | Notes |
|---|---|---|
| **PNR** (Passenger Name Record) | The booking container, identified by a 6-character record locator. | Matches `BookingReference` in the schema. Holds names, itinerary segments, contacts, ticketing arrangement, SSRs, remarks and history. |
| **Segment** | One flight leg sold to the passenger: carrier, flight number, date, origin, destination, booking class, status. | Segment status codes are two letters (for example `HK` holds confirmed, `HL` waitlisted, `UC` unable to confirm, `XX` cancelled). Use an `enum class`, and keep the wire codes in one table. |
| **Booking class (RBD)** | Single-letter reservation booking designator (Y, B, M, J, ...) that determines which inventory bucket is decremented. | Not the same as cabin, and not the same as fare basis. Never conflate them. |
| **Cabin** | Physical compartment: first, business, premium economy, economy. | Maps to `CabinClass` in the schema. |
| **Inventory** | Per flight-date: physical capacity, authorization levels, and per-class seats sold, with nested or bid-price controls from revenue management. | The source of truth for sellable counts. |
| **Availability** | A *view* computed for a shop request (O&D class availability), not a stored answer. | Inventory may be slightly stale; the sell step re-checks. |
| **Market / price record** | O&D-keyed conditional values used to value a sale (INTERNAL; not customer fares). | Conditions span channel, POS, travel/booking dates, AP, trip/pax type; overlapping records need precedence. |
| **Seat map / seat inventory** | Per-seat state for assignment: available, held, occupied, blocked. | Separate from class inventory. Selling a class seat does not assign a physical seat. |
| **Fare / fare basis / fare rules** | Customer-facing price product and rules (distinct from INTERNAL market price records). | Filed and distributed externally; versioned with effective dates. |
| **Ticket / e-ticket / coupon** | Document of carriage (13-digit ticket number); one coupon per flown segment. | Coupon status (open, checked-in, flown, refunded, exchanged) is a state machine. |
| **SSR / OSI** | Special service request (meal, wheelchair, infant) and other service information. | SSRs are airline-actionable and carry status codes of their own. |
| **TTL** | Ticketing time limit. If the PNR is not ticketed by then, the system cancels it. | Enforced by a scheduler, never by a client. |
| **Hold** | Temporary reservation of a seat or inventory while payment completes. | Maps to `SeatHoldRequest` and `SeatHoldResponse`. |

### State machines are explicit

Model PNR, segment, hold, ticket coupon, payment, and seat assignment lifecycles as explicit state machines with an exhaustive transition table. Illegal transitions return an error. They never silently coerce.

```cpp
enum class HoldState : std::uint8_t { Active, Consumed, Expired, Released };

[[nodiscard]] constexpr bool CanTransition(HoldState from, HoldState to) noexcept;
```

Every `switch` over a state or status enum lists every enumerator and has no `default:` branch, so the compiler warns when a new state is added.

---

## 3. Architecture

Organize by bounded context, each behind a narrow interface. Do not reach across contexts into another context's tables or internal types.

```
src/
  common/        strong types, money, time, ids, error model, logging, tracing
  inventory/     flight-date inventory, class buckets, authorization, sell/cancel
  availability/  availability views, caching, shopping read path
  seatmap/       per-seat state, holds, assignment, restrictions
  pricing/       fare lookup, rules, taxes, quotes with validity windows
  pnr/           PNR aggregate, segments, elements, history, record locators
  ticketing/     e-ticket issuance, coupons, exchanges, refunds, TTL
  payment/       authorization, capture, refund, tokenization boundary, 3-D Secure
  messaging/     inbound and outbound adapters: gRPC/REST, EDIFACT, Type B, NDC/XML
  schedule/      flight schedules, SSIM import, aircraft configuration
  checkin/       departure control interface (DCS boundary only)
  audit/         immutable event log, regulatory data export
  platform/      config, health, metrics, feature flags, scheduler
```

Rules:
- Domain logic (`inventory`, `pnr`, `pricing`, `ticketing`) has **no dependency** on transport, database, or serialization libraries. It depends on `common` and on abstract ports (interfaces).
- Adapters (`messaging`, persistence, payment gateways) implement ports. Wire types (protobuf, JSON, EDIFACT messages) are translated to domain types at the edge and never pass inward.
- Each context owns its data. Cross-context communication is by interface call or by published event, never by shared mutable state or shared tables.

### Consistency and concurrency model

- **Inventory sell/cancel** is a single atomic compare-and-update on the flight-date and class bucket. Use the datastore's conditional write or a serializable transaction. Never read-then-write across two calls.
- **PNR updates** use optimistic concurrency with a version number. A conflicting write returns `VersionConflict`. The caller re-reads and retries a bounded number of times.
- **Multi-step workflows** (hold, price, authorize payment, sell, ticket) are sagas. Each step has a compensating action (release hold, void authorization, cancel segment). Persist saga state, so a crashed process resumes or compensates after restart.
- **Events** are published with the transactional outbox pattern. Never write to the database and publish to a broker as two independent operations.
- **Idempotency**: every mutating API accepts an idempotency key (see `idempotencyKey` in `SeatPurchaseRequest`). Store the key with the result. A retry with the same key and same payload returns the original result. The same key with a different payload is an error.
- **Time-based expiry** (hold, quote, TTL) is enforced by a server-side scheduler and re-checked at use. Never trust that an expiry job has already run.

---

## 4. C++ standards

### Language and toolchain
- **C++20** minimum. Use concepts, `std::span`, `std::chrono` calendar types, designated initializers, `[[nodiscard]]`. Use C++23 features (`std::expected`, `std::optional` monadic ops) only where the toolchain is pinned and CI covers it.
- Build with **CMake** (3.25+), targets and `target_*` commands only; no global flags. Dependencies come from the package manager lockfile (vcpkg manifest or Conan lock). Pin versions.
- Compile with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror` (or the MSVC equivalent `/W4 /WX`). Warnings are bugs.
- Formatting is enforced by `clang-format` (checked in `.clang-format`). Static analysis is enforced by `clang-tidy` (checked in `.clang-tidy`). Do not hand-format or disable checks without a justification comment.

### Types and safety
- **Strong types for everything that has meaning.** No bare `std::string` or `int` for identifiers or quantities.

  ```cpp
  struct AirportCode   { std::array<char, 3> v; };  // validated at construction
  struct RecordLocator { std::array<char, 6> v; };
  struct SeatNumber    { std::uint8_t row; char column; };
  struct FlightNumber  { std::uint16_t number; char suffix{}; };
  ```

  Use a `Parse(...)` factory that returns `std::expected<T, ParseError>` (or `tl::expected`). Constructors of validated types are private or the type cannot hold an invalid value.
- **Money is integer minor units plus a currency.** Never `float` or `double` for money. Rounding rules are explicit and named (for example half-even for tax, per-currency minor-unit exponent). The type mirrors `Money` in the schema.
- **Time**: store and compute instants as UTC (`std::chrono::sys_time`). Keep the local airport time zone and a separate local-date field where business rules need it (flight date is a local date at the origin). Never use `time_t` arithmetic for business dates, and never use `std::chrono::system_clock::now()` directly in domain code. Inject a `Clock` interface.
- **Ownership**: RAII everywhere. No raw `new`/`delete`. `std::unique_ptr` by default, `std::shared_ptr` only with a written reason. No owning raw pointers in interfaces. Use `std::span`, `std::string_view` for non-owning views, and watch lifetimes at async boundaries.
- Prefer `const`, `constexpr`, `enum class`, `final`, `noexcept` where true, and `[[nodiscard]]` on anything returning a result or error.
- No `using namespace` in headers. No macros except include guards (prefer `#pragma once`) and logging or assert wrappers.

### Errors
- Domain code returns `std::expected<T, Error>` (or an equivalent `Result<T>`). **Exceptions do not cross module boundaries.** Catch at adapters and convert.
- `Error` carries a stable machine-readable `ErrorCode` (the same vocabulary as the schema: `SEAT_UNAVAILABLE`, `PRICE_CHANGED`, `HOLD_EXPIRED`, ...), a human-readable message, a `retryable` flag, and structured details. Add codes in one place and map them to wire codes in one place.
- Programming errors (violated invariants) use `assert`-style checks that log and abort in debug, and log and fail the request in release. Never continue on corrupted state.

### Concurrency
- Prefer message passing and immutable snapshots to shared mutable state. Shared state has one owner and a documented locking order.
- No raw `std::thread` in business code. Use the project's executor / thread-pool abstraction with explicit queue bounds and back-pressure.
- Use `std::mutex` with `std::scoped_lock`; hold locks for the shortest time and never across I/O or callbacks. Use lock-free structures only with a benchmark and review. Document every use of `std::memory_order` weaker than sequentially consistent.
- Every blocking or remote call takes a deadline or a `std::stop_token`. Cancellation propagates.
- Thread-safety is stated in the class comment: "thread-safe", "externally synchronized", or "single-threaded".

### Performance
- The availability read path has a latency budget (target p99 in the low tens of milliseconds, in-process) and must not allocate on the steady-state path where avoidable. Measure first with a benchmark (Google Benchmark); do not optimize speculatively.
- Prefer contiguous containers (`std::vector`, `std::flat_map` where available) for hot data. Use `std::unordered_map` with a stable, tested hash for keyed lookups. Reserve capacity when the size is known.
- Avoid virtual dispatch and `std::function` in tight loops. Pass callables by template.
- Cache invalidation is part of the design. Every cache documents its key, TTL, invalidation event and what happens when stale data is served.

---

## 5. Component guidelines

### Inventory
- Keep counters per flight-date, per cabin, per booking class, plus authorization (overbooking) levels and any bid-price or nested-class controls. Sell and cancel adjust counters atomically and emit an inventory event.
- A sell request names the flight, date, class, and quantity, and receives `Confirmed`, `Waitlisted`, or `Unavailable`. Never infer success from absence of an error.
- Inventory is reconciled against sold segments by a periodic audit job. Drift is alerted, never silently corrected.
- Group and block holds have their own limits and release schedules.

### Availability
- Availability is derived from inventory plus schedule and may be served from a cache. It returns counts in the traditional capped form (for example 0 to 9, where 9 means 9 or more) when the consumer expects it, and the real count internally.
- Responses carry a version or timestamp (`seatMapVersion` for seat maps). Sell requests may carry it so staleness can be detected (`SEAT_MAP_STALE`).

### Seat map and seat assignment
- Seat state is separate from class inventory. Assignment checks eligibility rules (exit-row age and mobility rules, infant restrictions, bassinet positions, loyalty-tier restrictions) on the server. Never rely on the client to enforce them.
- Holds are keyed by `holdId`, owned by a session or PNR, with a hard expiry. Re-holding the same seats for the same owner is idempotent. Holding seats owned by someone else fails with `SEAT_UNAVAILABLE`.
- Seat purchase and PNR update happen in one workflow, so a paid seat is never unassigned and an assigned seat is never unpaid.

### Pricing and fares
- Fares, rules and taxes are data with effective date ranges, versioned and cached. A quote is an immutable object with an ID, a validity window (`priceValidUntil`), and the inputs used. Purchase requests reference the expected price. If the price at purchase differs, return `PRICE_CHANGED` and never charge a different amount silently.
- Tax and fee calculation is a pure function of its inputs and is covered by table-driven tests with documented source rules.
- Currency conversion uses a rate snapshot recorded on the quote.

### PNR
- The PNR is an aggregate root. All changes go through its methods, which enforce invariants (for example every segment has a passenger, infants reference an adult, no duplicate segments, no overlapping segments in time without an explicit override).
- Every change appends to an **immutable history** (who, what, when, from which channel). The history is never edited or deleted.
- Record locators are generated from a collision-checked, non-sequential, non-guessable scheme, and avoid ambiguous characters.
- Splitting, merging and cancelling a PNR are separate, tested operations with defined effects on inventory and tickets.

### Ticketing and payment
- Payment is isolated behind a port. The system sees **tokens**, never primary account numbers. Keep the PCI DSS scope as small as possible: no card data in the domain, logs, or databases.
- Authorization, capture, void and refund are distinct operations with distinct states. Authorize before selling inventory in flows that require it, and always have a compensating void or release.
- 3-D Secure or other customer-action flows return a `pending_payment_action` state and resume through a callback or webhook that is authenticated, idempotent and replay-safe.
- Ticket issuance is idempotent per PNR and passenger. Exchanges and refunds recompute value using the original fare rules and record the calculation.

### Messaging and interoperability
- Support the transports the business needs through adapters: internal gRPC or REST, plus legacy and industry formats where required (for example EDIFACT, Type B teletype messages, NDC/XML for distribution). Each adapter is a translation layer only.
- Parsers are strict on input, tolerant only where the standard demands it, and fuzz-tested. Malformed messages produce typed errors and never crashes or undefined behavior.
- Keep a **message log** of inbound and outbound messages (with sensitive fields redacted) so partner disputes can be investigated.
- Schema changes are backwards compatible by default. Add optional fields, and never reuse or repurpose field numbers or enum values.

### Regulatory and privacy
- Passenger data (API/APIS, PNR data transfer to governments, GDPR and other privacy laws) is handled by a dedicated component. Field-level classification (public, internal, personal, sensitive) is declared in the types and enforced in logging and export.
- Retention and erasure are implemented as scheduled, audited jobs and never as ad hoc deletes.
- Access to personal data is role-based and logged.

---

## 6. Testing

Every change ships with tests. A bug fix starts with a failing test.

- **Unit tests** (GoogleTest/GoogleMock or Catch2): fast, deterministic, no I/O, injected `Clock` and ID generators.
- **State machine tests**: exhaustively test the transition table (every state x every event).
- **Property-based and randomized tests** for invariants: inventory never goes negative or above authorization, money never rounds to a different total than the sum of its parts, holds never overlap, parse then serialize round-trips.
- **Concurrency tests**: hammer sell/cancel/hold from many threads and assert invariants. Run under **ThreadSanitizer**.
- **Sanitizers in CI**: ASan, UBSan, and TSan builds run the full unit suite. Memory leaks and data races fail the build.
- **Fuzzing** (libFuzzer or AFL++) for every parser and deserializer.
- **Contract tests**: validate request and response payloads against `airline-reservation.schema.json`, and maintain golden files for external formats.
- **Integration tests** use real dependencies in containers (database, broker). Do not mock the database for tests that assert transactional behavior.
- **Failure injection**: test timeouts, partial failures, duplicate deliveries, out-of-order events, and crash-and-restart of sagas.
- **Performance tests**: benchmarks for the availability path and the sell path with regression thresholds checked in CI.
- Synthetic data follows the synthetic-data rule in this repository. Never use real passenger or payment data.

Coverage is a signal, not a goal. Do not write tests that only execute code without asserting behavior.

---

## 7. Observability and operations

- **Structured logging** (key-value or JSON), with a correlation ID, request ID and PNR locator on every line where applicable. Log levels have meaning: `ERROR` means a human should look at it. Redact sensitive fields by type, not by convention.
- **Metrics** for every boundary: request rate, error rate by `ErrorCode`, latency histograms, queue depth, cache hit rate, saga states, inventory drift, expiry job lag.
- **Distributed tracing** with context propagated through every call and message.
- **Health endpoints**: separate liveness and readiness. Readiness reflects dependency health and warm caches.
- **Configuration** is typed, validated at startup, and immutable at runtime except through the audited feature-flag system. A bad config refuses to start the process.
- **Graceful shutdown**: stop accepting work, drain in-flight requests within a deadline, persist saga state.
- **Rollouts**: changes touching inventory, pricing, or payment go behind feature flags, ship dark first, and can be disabled without a deploy.
- **Runbooks**: any new alert has a runbook entry describing impact, diagnosis, and mitigation.

---

## 8. Security

- Validate **all** input at the boundary: sizes, ranges, formats, enum values. Reject unknown fields where the schema says `additionalProperties: false`.
- Use bounded buffers and `std::span`/`std::string_view`. Avoid C APIs (`strcpy`, `sprintf`, `atoi`). No uninitialized memory. Treat sanitizer findings as release blockers.
- Authenticate and authorize every request. Service-to-service calls use mutual TLS or signed tokens. Authorization checks are in the domain or application layer, not only at the gateway.
- Secrets come from the secret manager, never from source, config files or environment dumps. Never print secrets in errors.
- Constant-time comparison for tokens and signatures. Use vetted cryptographic libraries only. Never write your own cryptography.
- Rate-limit and shed load on public endpoints to protect inventory and prevent scraping and seat-hoarding abuse. Holds per session and per account are capped.
- Third-party dependencies are reviewed, pinned, scanned for vulnerabilities, and generate an SBOM.

---

## 9. Code review and change management

- Small, focused changes. One concern per commit. Commit messages explain *why*.
- Public headers are the API: document preconditions, postconditions, error codes, thread-safety and complexity. Breaking a public header requires a migration note.
- Data migrations are forward-only, backwards compatible with the previous release, tested on production-sized data, and reversible by a documented procedure.
- Never "fix" a failing test by loosening the assertion without understanding the failure.
- Comments explain *why* and cite the business rule or standard being implemented. Do not narrate the code.
- Leave TODOs only with an owner and a tracking ticket.

---

## 10. How to work in this repository (for agents)

1. **Read before writing.** Find the owning component, its public interface and existing tests. Follow its conventions.
2. **State assumptions.** If a business rule is ambiguous (class mapping, rounding, time zone, status code semantics), say so and ask. Do not invent airline rules.
3. **Design the failure paths first.** For any new operation, list what happens on timeout, duplicate request, concurrent conflict, partial failure and crash. Then implement.
4. **Write the test, then the code.** Include at least one failure-path and one concurrency or idempotency test for anything that mutates state.
5. **Keep changes in scope.** Do not reformat unrelated code, rename public symbols or upgrade dependencies as part of a feature.
6. **Run the checks** before declaring done: build with warnings as errors, `clang-format`, `clang-tidy`, unit tests, and the sanitizer builds for the touched modules.
7. **Report honestly.** Say what you ran, what passed, what you could not run, and what remains uncertain.

### Definition of done
- Compiles warning-free on all supported compilers.
- Formatted, linted and statically analyzed with no new findings.
- Unit and failure-path tests added and passing, including under sanitizers.
- Public API documented. Metrics, logs and error codes added for new behavior.
- Schema and contract tests pass. No sensitive data in logs or fixtures.
- Backwards compatibility and rollout plan stated for changes that touch persisted data or wire formats.
