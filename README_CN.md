# sidecar

轻量级、无需 root 的 chroot 启动器，用于在 Linux 上运行 Apple Music FairPlay DRM 解密服务。

`sidecar` 管理基于 Android rootfs 的解密守护进程的完整生命周期，提供 PTY 输出转发、端口就绪检测、优雅重启和用户命名空间隔离——**无需 root 权限**。

## 架构

```
                     ┌──────────────────────────────────────┐
                     │          sidecar (39 KB)             │
                     │                                      │
  SIGUSR1 ──────────►│  ┌─ 信号处理 ─────────────────┐     │
  SIGTERM ──────────►│  │  转发 / 优雅退出 / 重启    │     │
                     │  └─────────────────────────────┘     │
                     │                                      │
                     │  ┌─ 用户命名空间 ─────────────────┐  │
                     │  │  unshare(NEWUSER | NEWNS)       │  │
                     │  │  uid 1000 → 0 (命名空间内)     │  │
                     │  │  bind-mount /dev, /proc, /sys   │  │
                     │  │                                 │  │
                     │  │  ┌─ chroot ./rootfs ──────────┐ │  │
                     │  │  │  /system/bin/main           │ │  │
                     │  │  │    ├─ TCP :47010 (解密)     │ │  │
                     │  │  │    └─ TCP :47020 (m3u8)     │ │  │
                     │  │  └─────────────────────────────┘ │  │
                     │  └──────────────────────────────────┘  │
                     │                                      │
                     │  ┌─ PTY ──────────────────────────┐  │
                     │  │  openpty() → 行缓冲模式        │  │
                     │  │  子进程输出 → 父进程实时显示    │  │
                     │  └────────────────────────────────┘  │
                     │                                      │
                     │  ┌─ 端口探测 ─────────────────────┐  │
                     │  │  --wait-ports 47010,47020       │  │
                     │  │  非阻塞 TCP 连接检测            │  │
                     │  │  200ms 间隔, 与 PTY 交替执行    │  │
                     │  └────────────────────────────────┘  │
                     └──────────────────────────────────────┘
```

## 致谢

本项目受到 Apple Music 解密社区先驱工作的启发：

- [**zhaarey/wrapper**](https://github.com/zhaarey/wrapper) — 最早的 Linux FairPlay 解密启动器
- [**glomatico/wrapper-v2**](https://github.com/glomatico/wrapper-v2) — 基于 Rust 的 Docker 隔离方案
- [**WorldObservationLog/AppleMusicDecrypt**](https://github.com/WorldObservationLog/AppleMusicDecrypt) — 全面的 Apple Music 解密工具集

`sidecar` 解决了在生产环境部署 wrapper 时遇到的若干限制——尤其在无 root 运行、可观测性和进程生命周期管理方面。

## 相比 wrapper 的优势

| 能力 | wrapper | sidecar |
|------|---------|---------|
| **需要 Root** | 否（user namespace） | **否**（user namespace） |
| **源码可用** | 闭源二进制（20 KB） | **开源 C 代码**（923 行） |
| **输出可见性** | 全缓冲（不可见） | **PTY 行缓冲**（实时可见） |
| **端口就绪检测** | 无（盲启动） | **`--wait-ports`** 非阻塞 TCP 探测 |
| **优雅重启** | 不支持 | **SIGUSR1** → TERM → 等待 → KILL → 重新 fork |
| **chroot DNS** | 未处理 | **自动同步** resolv.conf / hosts |
| **结构化事件** | 无 | **`--json-events`** 机器可解析状态 |
| **优雅关闭** | 立即杀死 | **可配置** `--grace-secs` |
| **崩溃恢复** | 进程退出 | **进程内重启循环**（PID 不变） |
| **信号转发** | 基础 | **完整**（INT/TERM/HUP/QUIT → 子进程） |

### 核心改进详解

1. **用户命名空间隔离** — `unshare(CLONE_NEWUSER | CLONE_NEWNS)` 创建隔离命名空间，进程在命名空间内映射为 uid 0。这使得 `chroot()`、`mount()` 和设备访问无需任何真实 root 权限。宿主系统不会被修改。

2. **PTY 输出转发** — wrapper 的子进程 stdout 被 libc 全缓冲（非 TTY 管道的默认行为），导致登录进度和解密状态不可见。sidecar 通过 `openpty()` 分配 PTY，将子进程切换为行缓冲模式，所有输出实时可见。

3. **端口就绪门控** — `--wait-ports 47010,47020` 以 200ms 间隔用非阻塞 TCP 连接探测每个端口，与 PTY 缓冲区排空交替执行。调用方可以精确知道服务何时就绪。

4. **进程内优雅重启** — `kill -USR1 <sidecar-pid>` 向子进程发送 SIGTERM，等待 `--grace-secs`，必要时升级为 SIGKILL，然后用相同参数重新 fork。sidecar 进程本身不会退出——适用于刷新认证而无需重新部署。

5. **DNS 引导** — chroot 前自动将 `/etc/resolv.conf`、`/etc/hosts`、`/etc/nsswitch.conf` 和 `/etc/services` 复制到 rootfs 中，确保 chroot 内 DNS 解析正常。

## 编译

```bash
make
# 或直接编译:
cc -O2 -Wall -Wextra -o sidecar sidecar.c -lutil
```

前提：Linux 系统启用了 `unprivileged_userns_clone`（大多数发行版默认启用）：
```bash
cat /proc/sys/kernel/unprivileged_userns_clone   # 应为 1
```

## 使用方法

### 基本用法（从文件读取认证，无需登录）

```bash
./sidecar --rootfs ./rootfs --bin /system/bin/main \
  --wait-ports 47010,47020 --verbose \
  -- -M 47020 -D 47010 -F
```

### 登录模式（首次认证）

```bash
./sidecar --rootfs ./rootfs --bin /system/bin/main \
  --wait-ports 47010,47020 --verbose \
  -- -M 47020 -D 47010 --login=user@host:pass
```

### 多实例（并行解密）

```bash
for i in 0 1 2 3; do
  ./sidecar --rootfs ./rootfs --bin /system/bin/main \
    --wait-ports $((47010+i)),$((47020+i)) --verbose \
    -- -M $((47020+i)) -D $((47010+i)) -F &
done
```

### 优雅重启（刷新认证无需重启 sidecar）

```bash
kill -USR1 $(pgrep -f sidecar)
```

## 命令行参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--rootfs PATH` | `./rootfs` | Android rootfs 目录路径 |
| `--bin PATH` | `/system/bin/main` | chroot 内要执行的二进制 |
| `--userns` | *(开启)* | 使用用户命名空间（无需 root） |
| `--no-userns` | | 使用真实 chroot（需要 root，兼容模式） |
| `--wait-ports P[,P..]` | | 探测就绪的 TCP 端口 |
| `--wait-timeout N` | `120` | 等待端口就绪的超时秒数 |
| `--grace-secs N` | `10` | SIGTERM 到 SIGKILL 之间的等待秒数 |
| `--json-events` | | 在 stderr 输出结构化 JSON 事件 |
| `--verbose`, `-v` | | 输出详细设置步骤 |

`--` 后的所有参数原样传递给子进程。

## 通信协议

子进程暴露两个原始 TCP 端口：

- **端口 47010** — 解密：逐 sample 的 FairPlay 流密码解密管道
- **端口 47020** — m3u8：将 Apple Music `adamId` 解析为 HLS 主播放列表 URL

两个端口均为本地无认证、长度前缀的二进制协议。

## 集成

### 与 aria 服务集成

```python
# aria_sidecar.py 管理 N 个 sidecar 实例实现并行解密
python3 aria_sidecar.py -F --instances 4
```

### 与 systemd 集成

```ini
[Unit]
Description=sidecar FairPlay 解密服务

[Service]
Type=notify
ExecStart=/usr/local/bin/sidecar \
  --rootfs /opt/am/rootfs \
  --bin /system/bin/main \
  --wait-ports 47010,47020 \
  --ready-fd 3 \
  -- -M 47020 -D 47010 -F
ExecReload=/bin/kill -USR1 $MAINPID
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

## 性能评测

测试环境：Dell PowerEdge R730（80 核，94 GB 内存，Ubuntu 22.04）。

| 指标 | wrapper_new | sidecar v4 | 差异 |
|------|-------------|------------|------|
| **冷启动** | 2338 ms | **2163 ms** | **sidecar 快 7.5%** |
| **流水线解密** | 3.18s (15.0 MB/s) | **3.12s** (15.3 MB/s) | 持平 |
| **内存** | 53.8 MB | **52.3 MB** | **sidecar 省 3%** |
| **二进制** | 20 KB | 35 KB | wrapper 更小（闭源） |
| **5 track 稳定性** | 5/5 | 5/5 | 持平 |
| **源码** | 闭源 | **935 行 C** | sidecar 开源 |
| **PTY 输出** | 无 | **有** | sidecar 独有 |
| **端口就绪** | 无 | **--wait-ports** | sidecar 独有 |
| **优雅重启** | 无 | **SIGUSR1** | sidecar 独有 |

v4 关键优化：
- 命名空间精简为 `CLONE_NEWUSER | CLONE_NEWPID`（去掉 `CLONE_NEWNS`）
- 端口探测间隔 200ms → 50ms
- userns 模式跳过 DNS 种子和 bind-mount
- 二进制 strip

详细数据：[docs/BENCHMARK.md](docs/BENCHMARK.md)

## 许可证

MIT — 见 [LICENSE](LICENSE)。
