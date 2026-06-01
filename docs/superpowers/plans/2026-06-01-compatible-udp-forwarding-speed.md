# Compatible UDP Forwarding Speed Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Improve UDP forwarding throughput without changing the command line, protocol, default ports, or requiring Linux-only batch syscalls.

**Architecture:** Keep the single-process `select()` design, but reduce wakeups and per-packet housekeeping. Use nonblocking UDP sockets so each selected descriptor can be drained until it would block, and throttle DNS/timeout maintenance to cached time ticks.

**Tech Stack:** C99/GNU99, POSIX sockets, existing Makefile and assert-based C tests.

---

### Task 1: Lock Transform Behavior

**Files:**
- Modify: `tests/test_transform.c`
- Modify: `transform.h`

- [ ] **Step 1: Add tests for the new inline XOR helper**

Add assertions that a fixed 4-byte mask transforms bytes exactly and that applying the same mask twice restores the input.

- [ ] **Step 2: Verify the test fails before adding the helper**

Run: `make tests/test_transform && tests/test_transform`
Expected: build fails because `transformbuf` is not defined.

- [ ] **Step 3: Add `transformbuf` and route `maskbuf`/`unmaskbuf` through it**

Keep the packet format unchanged: masked payload followed by the 4-byte mask.

- [ ] **Step 4: Verify transform tests pass**

Run: `make tests/test_transform && tests/test_transform`
Expected: assertions pass.

### Task 2: Drain Ready UDP Sockets

**Files:**
- Modify: `udpmask.c`

- [ ] **Step 1: Add nonblocking socket setup**

Set `O_NONBLOCK` on the listening socket and per-client sockets immediately after creation.

- [ ] **Step 2: Drain the listening socket**

After `select()` marks the listening socket readable, repeatedly call `recvfrom()` until it returns `EAGAIN` or `EWOULDBLOCK`, forwarding each valid datagram.

- [ ] **Step 3: Drain mapped sockets**

After `select()` marks a mapped remote socket readable, repeatedly call `recv()` until it returns `EAGAIN` or `EWOULDBLOCK`, forwarding each valid datagram back to its client.

### Task 3: Throttle Housekeeping

**Files:**
- Modify: `udpmask.c`

- [ ] **Step 1: Cache current time**

Only refresh `time(NULL)` when needed for DNS refresh, last-use updates, and cleanup checks.

- [ ] **Step 2: Run cleanup at most once per second**

Keep timeout semantics within one second while avoiding a full map scan after every packet burst.

- [ ] **Step 3: Keep DNS refresh behavior**

Continue honoring `UM_HOST_TIMEOUT`, but only check it through the cached timestamp.

### Task 4: Verify

**Files:**
- Read: `Makefile`
- Read: local toolchain state

- [ ] **Step 1: Run available tests**

Run: `make test` if a POSIX C toolchain is available.

- [ ] **Step 2: Run static source checks available in this environment**

Use `git diff --check` and inspect modified C paths.

- [ ] **Step 3: Report unavailable verification**

If `make`, `gcc`, and WSL are missing, report that compilation could not be executed on this Windows host.
