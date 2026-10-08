# Runtime reliability fixes

This document covers logger degradation reporting, rule-state pruning and chip verification
boundaries. It adds no listeners or threads.

## Logger degradation

Monitor checks write failures and dropped bytes after sampling; the main loop also checks after
flushing. The first change produces an important SEL record and a service event. Further changes
are reported at most every five seconds; unchanged counters are not repeated. Changes inside a
reporting window are coalesced into the latest cumulative counters at the next permitted check.
Warnings never write back to the failing Logger, avoiding a feedback loop.

```sh
sudo grep '"log" "degraded"' /var/log/bmc-lite/sel.db | tail -10
```

Messages include log-write-failures and dropped-bytes. Enabled uplink receives the same service
event, with its existing best-effort delivery semantics. Local SEL is the primary observation path.
If SEL also fails, persistence cannot be guaranteed. Sampling continues during logger failures;
an unsuccessful final flush still produces exit status 1 and a stderr diagnostic.

## Rule state lifecycle

After each Monitor.poll, FaultRuleEngine removes state belonging to absent sensor IDs. No rule
reload is needed. Confirmation counts and active flags for retained sensors are preserved.
State size follows current matching rule/sensor combinations rather than historical sensor IDs.

Direct FaultRuleEngine callers must call retain_sensors after a complete sampling round with
the whole current sensor set. Calling it with only one sensor after each evaluate would incorrectly
erase other sensors. These interfaces belong to the sampling thread, not the network thread.

## Hardware verification limits

docs/chips.md distinguishes candidate datasheet sources from verified facts. Items without recorded
sections/tables are explicitly unverified. EMC2103 constants, fault codes, internal resolution and
default RANGE, plus ADM1275 unsupported-command assumptions, remain unverified against the
published manuals. Corresponding code comments state these limits.

The previous equation 3932160 = 60 × 32768 / 5 is false and is no longer presented as a derivation.
No replacement hardware constant is guessed; existing decoding behavior stays compatible.
Fake tests establish consistency with implementation assumptions, not actual electrical behavior
or measurement accuracy.

## Validate

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build -R 'RuntimeManagement|degradation_runtime' --output-on-failure
```

Tests cover /dev/full, persisted runtime warnings, reporting throttling, no feedback, independent
dropped-byte changes, 10,000 changing sensors, retained confirmation counts and Monitor wiring.
Source/release installation is unchanged; see README and the control deployment guide.
