# am-sidecar

A lightweight, rootless chroot launcher for running Apple Music FairPlay DRM decrypt binaries on Linux.

`am-sidecar` manages the lifecycle of an Android-based decrypt daemon inside a chroot environment, providing PTY-based output forwarding, port readiness detection, graceful restart, and user namespace isolation — all without requiring root privileges.

## Architecture

```
                         ┌──────────────────────────────────────┐
                         │           am-sidecar (39 KB)         │
                         │                                      │
  SIGUSR1 ──────────────►│  ┌─ signal handler ────────────┐     │
  SIGTERM ──────────────►│  │  forward / grace / restart  │     │
                         │  └─────────────────────────────┘     │
                         │                                      │
                         │  ┌─ user namespace ────────────────┐ │
                         │  │  unshare(NEWUSER | NEWNS)       │ │
                         │  │  uid 1000 → 0 (inside)         │ │
                         │  │  bind-mount /dev, /proc, /sys   │ │
                         │  │                                 │ │
                         │  │  ┌─ chroot ./rootfs ──────────┐ │ │
                         │  │  │  /system/bin/main           │ │ │
                         │  │  │    ├─ TCP :47010 (decrypt)  │ │ │
                         │  │  │    └─ TCP :47020 (m3u8)     │ │ │
                         │  │  └─────────────────────────────┘ │ │
                         │  └──────────────────────────────────┘ │
                         │                                      │
                         │  ┌─ PTY ──────────────────────────┐  │
                         │  │  openpty() → line-buffered     │  │
                         │  │  child stdout → parent stdout  │  │
                         │  └────────────────────────────────┘  │
                         │                                      │
                         │  ┌─ port probe ───────────────────┐  │
                         │  │  --wait-ports 47010,47020      │  │
                         │  │  non-blocking TCP connect      │  │
                         │  │  200ms tick, interleaved w/PTY │  │
                         │  └────────────────────────────────┘  │
                         └──────────────────────────────────────┘
```

## Acknowledgments

This project is inspired by and builds upon the pioneering work of the Apple Music decrypt community:

- [**zhaarey/wrapper**](https://github.com/zhaarey/wrapper) — the original wrapper binary for running FairPlay decrypt on Linux
- [**glomatico/wrapper-v2**](https://github.com/glomatico/wrapper-v2) — Rust-based supervisor with Docker isolation
- [**WorldObservationLog/AppleMusicDecrypt**](https://github.com/WorldObservationLog/AppleMusicDecrypt) — comprehensive Apple Music decrypt toolkit

`am-sidecar` addresses several limitations encountered when deploying wrapper in production environments — particularly around rootless operation, observability, and process lifecycle management.

## Advantages over wrapper

| Capability | wrapper | am-sidecar |
|---|---|---|
| **Root required** | No (user namespace) | **No** (user namespace) |
| **Source available** | Closed binary (20 KB) | **Open C source** (923 lines) |
| **Output visibility** | Fully buffered (invisible) | **PTY line-buffered** (real-time) |
| **Port readiness** | None (blind start) | **`--wait-ports`** with non-blocking TCP probe |
| **Graceful restart** | Not supported | **SIGUSR1** → TERM → grace → KILL → re-fork |
| **DNS in chroot** | Not handled | **Auto-seed** resolv.conf / hosts / nsswitch.conf |
| **Structured events** | None | **`--json-events`** for machine-parseable status |
| **Grace shutdown** | Immediate kill | **Configurable** `--grace-secs` with TERM→wait→KILL |
| **Docker required** | No | **No** |
| **Binary size** | 20 KB | **39 KB** |
| **Multi-generation** | Process dies on crash | **In-process restart loop** (same PID) |
| **Signal forwarding** | Basic | **Full** (INT/TERM/HUP/QUIT → child) |
| **Readiness notification** | None | **`--ready-fd`** for systemd / k8s probes |

### Key improvements in detail

1. **User namespace isolation** — `unshare(CLONE_NEWUSER | CLONE_NEWNS)` creates an isolated namespace where the process maps itself to uid 0. This enables `chroot()`, `mount()`, and device access without any real root privileges. The host system is never modified.

2. **PTY output forwarding** — wrapper's child stdout is fully buffered by libc (default for non-TTY pipes), making login progress and decrypt status invisible. am-sidecar allocates a PTY via `openpty()`, switching the child to line-buffered mode so all output surfaces in real time.

3. **Port readiness gate** — `--wait-ports 47010,47020` polls each port with non-blocking TCP connects on a 200ms tick, interleaved with PTY draining. This prevents the chatty child's output from filling the PTY buffer while waiting. Callers know exactly when the service is ready.

4. **In-process graceful restart** — `kill -USR1 <sidecar-pid>` sends SIGTERM to the child, waits up to `--grace-secs`, escalates to SIGKILL if needed, then re-forks with the same argv. The sidecar process itself never exits — useful for auth refresh without redeployment.

5. **DNS bootstrap** — Before chroot, copies `/etc/resolv.conf`, `/etc/hosts`, `/etc/nsswitch.conf`, and `/etc/services` into the rootfs. This ensures DNS resolution works inside the chroot — a common silent failure with wrapper deployments.

## Build

```bash
make
# or directly:
cc -O2 -Wall -Wextra -o sidecar sidecar.c -lutil
```

Requirements: Linux with `unprivileged_userns_clone` enabled (default on most distributions):
```bash
cat /proc/sys/kernel/unprivileged_userns_clone   # should be 1
```

## Usage

### Basic (code-from-file, headless)

```bash
./sidecar --rootfs ./rootfs --bin /system/bin/main \
  --wait-ports 47010,47020 --verbose \
  -- -M 47020 -D 47010 -F
```

### Login mode (initial authentication)

```bash
./sidecar --rootfs ./rootfs --bin /system/bin/main \
  --wait-ports 47010,47020 --verbose \
  -- -M 47020 -D 47010 --login=user@host:pass
```

### Multi-instance (parallel decryption)

```bash
for i in 0 1 2 3; do
  ./sidecar --rootfs ./rootfs --bin /system/bin/main \
    --wait-ports $((47010+i)),$((47020+i)) --verbose \
    -- -M $((47020+i)) -D $((47010+i)) -F &
done
```

### Real-root mode (legacy, needs sudo)

```bash
sudo ./sidecar --no-userns --rootfs ./rootfs --bin /system/bin/main \
  --wait-ports 47010,47020 -- -M 47020 -D 47010 -F
```

### Graceful restart (refresh auth without restarting sidecar)

```bash
kill -USR1 $(pgrep -f sidecar)
```

### JSON events (for monitoring integration)

```bash
./sidecar --json-events --rootfs ./rootfs --bin /system/bin/main \
  -- -M 47020 -D 47010 -F

# stderr output:
# {"t":12345,"event":"chroot_ready"}
# {"t":12350,"event":"child_forked","pid":1234}
# {"t":12360,"event":"port_probe_first","port":47010}
# {"t":12361,"event":"port_probe_first","port":47020}
# {"t":12361,"event":"port_ready_all"}
```

## CLI Reference

| Flag | Default | Description |
|------|---------|-------------|
| `--rootfs PATH` | `./rootfs` | Path to the Android rootfs directory |
| `--bin PATH` | `/system/bin/main` | Executable to run inside chroot |
| `--cwd-in-chroot PATH` | `/` | Working directory after chroot |
| `--userns` | *(on)* | Use user namespace (no root needed) |
| `--no-userns` | | Use real chroot (needs root, legacy mode) |
| `--wait-ports P[,P..]` | | TCP ports to probe for readiness |
| `--wait-timeout N` | `120` | Seconds to wait for ports before giving up |
| `--ready-fd N` | | Write `\1\n` on this fd when ports are ready |
| `--grace-secs N` | `10` | Seconds between SIGTERM and SIGKILL |
| `--pid-file PATH` | | Write child PID to this file |
| `--no-pty` | | Don't allocate PTY (legacy buffered mode) |
| `--no-dns-seed` | | Don't copy DNS files into chroot |
| `--json-events` | | Emit structured JSON events on stderr |
| `--verbose`, `-v` | | Log setup steps to stderr |
| `--help`, `-h` | | Show help |

Everything after `--` is passed through to the child binary verbatim.

## Wire Protocol

The child binary exposes two raw TCP ports:

- **Port 47010** — decrypt: pipelined stream-cipher channel for sample-by-sample FairPlay decryption
- **Port 47020** — m3u8: resolves an Apple Music `adamId` to an enhanced-HLS master playlist URL

Both are locally-unauthenticated, length-prefixed binary protocols. See [aria's PROTOCOL.md](https://github.com/overkazaf/aria) for the full byte-level specification.

## Integration

### With aria service

```python
# aria_sidecar.py manages N sidecar instances for parallel decryption
python3 aria_sidecar.py -F --instances 4
```

### With systemd

```ini
[Unit]
Description=am-sidecar FairPlay decrypt

[Service]
Type=notify
NotifyAccess=all
ExecStart=/usr/local/bin/sidecar \
  --rootfs /opt/am/rootfs \
  --bin /system/bin/main \
  --wait-ports 47010,47020 \
  --ready-fd 3 \
  --json-events \
  -- -M 47020 -D 47010 -F
ExecReload=/bin/kill -USR1 $MAINPID
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

## License

MIT — see [LICENSE](LICENSE).
