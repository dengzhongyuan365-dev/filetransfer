# Technical Roadmap

This document records technical optimization directions for LAN File Transfer.
It is intentionally focused on engineering work rather than product UI ideas.

## Priorities

## Architecture Refactor Status

The latest maintainability pass split several responsibilities out of the GUI main window and scheduler hot path.
This is now the baseline for later multi-device and queue work:

- `src/gui/target_dialogs.*`: compact target-selection dialogs for multi-target sends and queued-task target changes.
- `src/gui/device_manager.*`: peer inventory, endpoint de-duplication, online/stale status, linked peers, active peer, and selected send targets.
- `src/gui/control_message.*`: UDP discovery/link JSON message encoding and decoding.
- `src/gui/transfer_list_model.*`: transfer snapshot storage, peer ownership, per-peer filtering, and dismissed transfer keys.
- `TransferScheduler::set_runner_factory`: injectable send runner factory used by tests to simulate concurrent sends without real network I/O.

The intent is to keep `MainWindow` focused on Qt widgets and user actions.
Network state, transfer-list state, and scheduler execution state should continue moving into small testable classes when touched.

### 1. Manifest Tree Upgrade

Current manifest data is file-list oriented. Upgrade it into a full filesystem tree model:

- regular files
- directories, including empty directories
- unsupported entries recorded explicitly where useful
- relative path, size, mtime, mode
- root directory name
- aggregate counters such as total files and total bytes

Expected benefits:

- preserve empty directories
- improve total progress and ETA
- make conflict handling clearer
- prepare for small-file batching and resume checkpoints

### 2. Progress Accounting

Add byte-level accounting for directory transfers:

- total directory bytes
- transferred directory bytes
- current file bytes
- processed file count
- skipped/full/delta file count
- estimated remaining time

The GUI should be able to show both file-count progress and byte progress, for example:

```text
123/3800 files, 1.2 GiB/1.9 GiB, 18 MiB/s, ETA 40s
```

### 3. Protocol Versioning And Capability Negotiation

The frame protocol and hello message already have version information, and manifest encoding has started to evolve.
Unify this into a clear protocol negotiation layer:

- protocol version
- supported features
- manifest schema version
- delta stream capability
- optional strict verification mode

This reduces compatibility risk when adding new protocol fields.

### 4. Verification Modes

Directory sync currently favors responsiveness by using quick checks and avoiding full pre-hash of every file.
Add explicit verification modes:

- fast: size and mtime quick check
- normal: verify files written during transfer
- strict: stronger hash verification for changed files or the whole tree

The CLI and GUI can expose this later as a setting.

### 5. Network Robustness

Improve behavior under real LAN failures:

- connect timeout
- read/write timeout
- heartbeat or ping frame
- idle timeout
- clearer peer-disconnect errors
- cancellation that consistently interrupts blocking network operations

This should be tested with two physical machines, sleep/resume, Wi-Fi switching, and unplugged network scenarios.

### 6. Persistent Resume

Current resume is mostly based on `.part` files and quick checks.
Add a durable transfer session model:

- transfer session id
- checkpoint file
- completed file list
- failed file list
- restart after app crash or reboot
- retry only failed files

This is especially useful for large directories.

### 7. Small-File Batching

Many small files pay too much per-file overhead.
Potential optimizations:

- batch metadata
- batch acknowledgements
- reduce per-file round trips
- group small file payloads where practical

This should come after manifest tree and progress accounting are stable.

### 8. Large-File I/O Optimization

Only optimize after measuring real bottlenecks.
Possible Linux-specific options:

- larger adaptive buffers
- `sendfile`
- `splice`
- `posix_fadvise`
- fewer allocations in the delta path

The goal is to improve throughput without making the protocol harder to reason about.

### 9. Sync Session Refactor

`sync_session` now owns negotiation, sender streaming, receiver application, progress publishing, and error mapping.
Split it gradually:

- negotiation
- sender stream
- receiver apply
- progress accounting
- protocol error handling

This should be done in small steps with tests kept green.

## Suggested Next Step

Start with **Manifest Tree Upgrade + Progress Accounting**.
Those two changes unlock empty directory sync, better UI progress, ETA, and cleaner future conflict handling.
