/*
 * system_info.c — CPU core count and RAM detection.
 *
 * macOS: sysctlbyname for core counts, hw.memsize for RAM.
 * BSD: sysconf + sysctl(HW_PHYSMEM64 / HW_PHYSMEM).
 * Linux: sysconf + sysinfo(), with cgroup-aware overrides when running
 *        inside a container so the limits reflect the cgroup's effective
 *        CPU quota and memory cap rather than the host's totals.
 * Windows: GetSystemInfo + GlobalMemoryStatusEx.
 *
 * Results are cached after first call (immutable hardware properties).
 */
#include "foundation/constants.h"

enum { DEFAULT_CORES = 1, MIN_WORKERS = 1, CBM_WORKERS_MAX = 256 };
#include "foundation/log.h"
#include "foundation/platform.h"
#include "foundation/system_info_internal.h"
#include <stdint.h> // uint64_t
#include <stdlib.h> // strtol
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(__NetBSD__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <unistd.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#else /* Linux */
/* limits.h for ULLONG_MAX, stdio.h for fopen/fread, stdlib.h for strto*. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#endif

/* ── macOS detection ─────────────────────────────────────────────── */

#ifdef __APPLE__

static int sysctl_int(const char *name, int fallback) {
    int val = 0;
    size_t len = sizeof(val);
    if (sysctlbyname(name, &val, &len, NULL, 0) == 0 && val > 0) {
        return val;
    }
    return fallback;
}

static size_t sysctl_size(const char *name, size_t fallback) {
    size_t val = 0;
    size_t len = sizeof(val);
    if (sysctlbyname(name, &val, &len, NULL, 0) == 0 && val > 0) {
        return val;
    }
    /* Try CBM_SZ_64-bit variant */
    uint64_t val64 = 0;
    len = sizeof(val64);
    if (sysctlbyname(name, &val64, &len, NULL, 0) == 0 && val64 > 0) {
        return (size_t)val64;
    }
    return fallback;
}

static cbm_system_info_t detect_system_macos(void) {
    cbm_system_info_t info;
    memset(&info, 0, sizeof(info));

    info.total_cores = sysctl_int("hw.ncpu", DEFAULT_CORES);
    info.perf_cores = sysctl_int("hw.perflevel0.physicalcpu", info.total_cores);

    /* If perflevel sysctls fail (Intel Mac), perf = total */
    int eff = sysctl_int("hw.perflevel1.physicalcpu", 0);
    if (info.perf_cores + eff > info.total_cores) {
        info.perf_cores = info.total_cores;
    }

    info.total_ram = sysctl_size("hw.memsize", 0);
    return info;
}

#elif defined(__NetBSD__) || defined(__FreeBSD__) || defined(__OpenBSD__)

static cbm_system_info_t detect_system_bsd(void) {
    cbm_system_info_t info;
    memset(&info, 0, sizeof(info));

    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    info.total_cores = nprocs > 0 ? (int)nprocs : 1;
    info.perf_cores = info.total_cores;

#if defined(__OpenBSD__)
    int mib[2] = {CTL_HW, HW_PHYSMEM};
#else
    int mib[2] = {CTL_HW, HW_PHYSMEM64};
#endif
    uint64_t physmem = 0;
    size_t len = sizeof(physmem);
    if (sysctl(mib, 2, &physmem, &len, NULL, 0) == 0 && physmem > 0) {
        info.total_ram = (size_t)physmem;
    }

    return info;
}

#elif !defined(_WIN32) /* Linux */

/* Read up to (bufsz-1) bytes from `path` into `buf`, NUL-terminate, and strip
 * trailing whitespace. Returns the (stripped) byte count, or -1 if the file
 * could not be opened or read. */
static int read_small_file(const char *path, char *buf, size_t bufsz) {
    FILE *fp = fopen(path, "re");
    if (fp == NULL) {
        return -1;
    }
    size_t n = fread(buf, 1, bufsz - 1, fp);
    fclose(fp);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ' || buf[n - 1] == '\t')) {
        n--;
    }
    buf[n] = '\0';
    return (int)n;
}

/* Parse a "cpu.max" v2 payload from `buf`. Returns effective CPUs or -1. */
static int parse_cpu_max_v2(const char *buf) {
    if (strncmp(buf, "max", 3) == 0) {
        return -1; /* no quota → caller falls back to sysconf */
    }
    long quota = 0;
    long period = 0;
    if (sscanf(buf, "%ld %ld", &quota, &period) == 2 && quota > 0 && period > 0) {
        long n = (quota + period - 1) / period; /* ceil(quota/period) */
        return n > 0 ? (int)n : MIN_WORKERS;
    }
    return -1;
}

/* Read v1 quota/period from a directory that directly contains
 * "cpu.cfs_quota_us" and "cpu.cfs_period_us". Returns CPU count or -1. */
static int parse_cpu_v1_from_dir(const char *dir) {
    char path[CBM_PATH_MAX];
    char buf[CBM_SZ_64];

    snprintf(path, sizeof(path), "%s/cpu.cfs_quota_us", dir);
    if (read_small_file(path, buf, sizeof(buf)) <= 0) {
        return -1;
    }
    long quota = strtol(buf, NULL, CBM_DECIMAL_BASE);
    if (quota <= 0) {
        return -1;
    }

    snprintf(path, sizeof(path), "%s/cpu.cfs_period_us", dir);
    if (read_small_file(path, buf, sizeof(buf)) <= 0) {
        return -1;
    }
    long period = strtol(buf, NULL, CBM_DECIMAL_BASE);
    if (period <= 0) {
        return -1;
    }

    long n = (quota + period - 1) / period;
    return n > 0 ? (int)n : MIN_WORKERS;
}

/* Effective CPU count from a cgroup file tree. See header for contract. */
int cbm_detect_cgroup_cpus(const char *cgroup_root) {
    char path[CBM_PATH_MAX];
    char buf[CBM_SZ_64];

    /* cgroup v2: "<root>/cpu.max" — "<quota> <period>" or "max <period>". */
    snprintf(path, sizeof(path), "%s/cpu.max", cgroup_root);
    if (read_small_file(path, buf, sizeof(buf)) > 0) {
        return parse_cpu_max_v2(buf);
    }

    /* cgroup v1: ".../cpu/cpu.cfs_quota_us" and ".../cpu/cpu.cfs_period_us". */
    snprintf(path, sizeof(path), "%s/cpu", cgroup_root);
    return parse_cpu_v1_from_dir(path);
}

/* Effective CPU count from an already-resolved path. See header. */
int cbm_detect_cgroup_cpus_file(const char *path, int is_v2) {
    if (path == NULL) {
        return -1;
    }
    if (is_v2) {
        char buf[CBM_SZ_64];
        if (read_small_file(path, buf, sizeof(buf)) <= 0) {
            return -1;
        }
        return parse_cpu_max_v2(buf);
    }
    return parse_cpu_v1_from_dir(path);
}

/* Effective memory limit from a single file. See header for contract.
 *
 * The v2 "memory.max" and v1 "memory.limit_in_bytes" grammars are close
 * enough (integer bytes, plus v2's "max" literal and v1's near-ULLONG_MAX
 * sentinel) that one parser covers both. */
size_t cbm_detect_cgroup_mem_file(const char *mem_file_path) {
    if (mem_file_path == NULL) {
        return 0;
    }
    char buf[CBM_SZ_64];
    if (read_small_file(mem_file_path, buf, sizeof(buf)) <= 0) {
        return 0;
    }
    if (strncmp(buf, "max", 3) == 0) {
        return 0;
    }
    char *end = NULL;
    unsigned long long n = strtoull(buf, &end, CBM_DECIMAL_BASE);
    if (end == buf || n == 0 || n >= (ULLONG_MAX / 2)) {
        return 0;
    }
    return (size_t)n;
}

/* Effective memory limit from a cgroup file tree. See header for contract. */
size_t cbm_detect_cgroup_mem(const char *cgroup_root) {
    char path[CBM_PATH_MAX];

    /* cgroup v2: "<root>/memory.max". */
    snprintf(path, sizeof(path), "%s/memory.max", cgroup_root);
    size_t v2 = cbm_detect_cgroup_mem_file(path);
    if (v2 > 0) {
        return v2;
    }
    /* If the file existed but reported "max"/unlimited, we can't tell v2
     * apart from "file missing" here without a stat — but the fallback to
     * v1 below is safe: on a v2-only host the v1 path won't exist either
     * and we'll still return 0. */

    /* cgroup v1: ".../memory/memory.limit_in_bytes". */
    snprintf(path, sizeof(path), "%s/memory/memory.limit_in_bytes", cgroup_root);
    return cbm_detect_cgroup_mem_file(path);
}

/* ── /proc/self/cgroup resolvers ─────────────────────────────────────
 *
 * On Linux a process rarely lives at the cgroup filesystem root. Under
 * Docker, Kubernetes, systemd, ECS Fargate, etc. it sits in a nested
 * sub-cgroup like "/ecs/<task>/<container>" (v2) or "/docker/<id>" (v1),
 * and the effective limits live at that nested path. Reading only the
 * root files silently reports the host's totals — which is the source
 * of the OOM-in-container bug this module now guards against.
 *
 * These helpers parse /proc/self/cgroup (a tiny file: v2 has one line,
 * v1 has one line per controller) and combine the process path with the
 * cgroup filesystem root to produce a concrete file path to read. */

/* Return 1 iff `path` exists as any file/dir kind. Used to detect v2. */
static int path_exists(const char *path) {
    FILE *fp = fopen(path, "re");
    if (fp != NULL) {
        fclose(fp);
        return 1;
    }
    return 0;
}

/* Strip trailing newline in-place. */
static void chomp(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

/* Read /proc/self/cgroup into `buf`. Returns bytes read (>0) or -1. */
static int read_proc_cgroup(const char *proc_path, char *buf, size_t bufsz) {
    FILE *fp = fopen(proc_path, "re");
    if (fp == NULL) {
        return -1;
    }
    size_t n = fread(buf, 1, bufsz - 1, fp);
    fclose(fp);
    if (n == 0) {
        return -1;
    }
    buf[n] = '\0';
    return (int)n;
}

/* Copy the cgroup path portion of a v2 "0::PATH" line into `out`.
 * Returns 0 on success, -1 on malformed input or buffer overflow. */
static int extract_v2_path(const char *proc_content, char *out, size_t out_sz) {
    /* v2 grammar: single line "0::/path" (no controllers column). */
    const char *p = strstr(proc_content, "0::");
    if (p == NULL) {
        /* Some kernels emit the hierarchy id ahead of the sentinel; be
         * lenient and fall back to the first "::" occurrence. */
        p = strstr(proc_content, "::");
        if (p == NULL) {
            return -1;
        }
        p += 2;
    } else {
        p += 3;
    }
    /* Copy until newline or EOS. */
    size_t i = 0;
    while (p[i] != '\0' && p[i] != '\n' && p[i] != '\r') {
        if (i + 1 >= out_sz) {
            return -1;
        }
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    if (i == 0) {
        return -1;
    }
    return 0;
}

/* Extract the sub-cgroup path for `controller` from a v1 /proc/self/cgroup
 * dump. Matches lines "N:controllers:PATH" where `controllers` is a
 * comma-separated list containing an exact-token match for `controller`.
 * Returns 0 on success, -1 if no matching line was found. */
static int extract_v1_path(const char *proc_content, const char *controller,
                           char *out, size_t out_sz) {
    size_t clen = strlen(controller);
    const char *line = proc_content;
    while (line != NULL && *line != '\0') {
        const char *eol = strchr(line, '\n');
        size_t line_len = (eol != NULL) ? (size_t)(eol - line) : strlen(line);

        /* Skip "N:" hierarchy id */
        const char *c1 = memchr(line, ':', line_len);
        if (c1 != NULL) {
            c1++;
            size_t rest_len = line_len - (size_t)(c1 - line);
            const char *c2 = memchr(c1, ':', rest_len);
            if (c2 != NULL) {
                /* [c1, c2) is the controllers field; [c2+1, line+line_len) is path.
                 * Scan comma-separated controller tokens for an exact match. */
                const char *tok = c1;
                while (tok < c2) {
                    const char *comma = memchr(tok, ',', (size_t)(c2 - tok));
                    size_t tok_len = (comma != NULL) ? (size_t)(comma - tok)
                                                    : (size_t)(c2 - tok);
                    if (tok_len == clen && memcmp(tok, controller, clen) == 0) {
                        const char *path = c2 + 1;
                        size_t path_len = line_len - (size_t)(path - line);
                        if (path_len + 1 > out_sz) {
                            return -1;
                        }
                        memcpy(out, path, path_len);
                        out[path_len] = '\0';
                        return 0;
                    }
                    if (comma == NULL) {
                        break;
                    }
                    tok = comma + 1;
                }
            }
        }
        if (eol == NULL) {
            break;
        }
        line = eol + 1;
    }
    return -1;
}

/* Compose "<root><sub><suffix>" into `out`, collapsing the redundant slash
 * that appears when the process cgroup path is exactly "/". Returns 0 on
 * success, -1 on buffer overflow. */
static int compose_cgroup_path(const char *root, const char *sub,
                               const char *suffix, char *out, size_t out_sz) {
    int n;
    if (strcmp(sub, "/") == 0) {
        n = snprintf(out, out_sz, "%s%s", root, suffix);
    } else {
        n = snprintf(out, out_sz, "%s%s%s", root, sub, suffix);
    }
    if (n < 0 || (size_t)n >= out_sz) {
        return -1;
    }
    return 0;
}

/* Detect cgroup v2 by looking for a controllers file at the fs root. */
static int cgroup_fs_is_v2(const char *cgroup_fs_root) {
    char probe[CBM_PATH_MAX];
    int n = snprintf(probe, sizeof(probe), "%s/cgroup.controllers", cgroup_fs_root);
    if (n < 0 || (size_t)n >= sizeof(probe)) {
        return 0;
    }
    return path_exists(probe);
}

int cbm_resolve_process_cgroup_mem_path(const char *proc_self_cgroup_path,
                                        const char *cgroup_fs_root,
                                        char *out,
                                        size_t out_sz,
                                        int *out_is_v2) {
    if (proc_self_cgroup_path == NULL || cgroup_fs_root == NULL || out == NULL ||
        out_sz == 0 || out_is_v2 == NULL) {
        return -1;
    }
    char proc_buf[CBM_SZ_64K];
    if (read_proc_cgroup(proc_self_cgroup_path, proc_buf, sizeof(proc_buf)) < 0) {
        return -1;
    }

    int is_v2 = cgroup_fs_is_v2(cgroup_fs_root);
    *out_is_v2 = is_v2;

    char sub[CBM_PATH_MAX];
    if (is_v2) {
        if (extract_v2_path(proc_buf, sub, sizeof(sub)) != 0) {
            return -1;
        }
        chomp(sub);
        return compose_cgroup_path(cgroup_fs_root, sub, "/memory.max", out, out_sz);
    }

    if (extract_v1_path(proc_buf, "memory", sub, sizeof(sub)) != 0) {
        return -1;
    }
    chomp(sub);
    /* v1: "<root>/memory<sub>/memory.limit_in_bytes". "memory" here is the
     * controller mount subdir; `sub` starts with "/" already. */
    int n;
    if (strcmp(sub, "/") == 0) {
        n = snprintf(out, out_sz, "%s/memory/memory.limit_in_bytes", cgroup_fs_root);
    } else {
        n = snprintf(out, out_sz, "%s/memory%s/memory.limit_in_bytes",
                     cgroup_fs_root, sub);
    }
    if (n < 0 || (size_t)n >= out_sz) {
        return -1;
    }
    return 0;
}

int cbm_resolve_process_cgroup_cpu_path(const char *proc_self_cgroup_path,
                                        const char *cgroup_fs_root,
                                        char *out,
                                        size_t out_sz,
                                        int *out_is_v2) {
    if (proc_self_cgroup_path == NULL || cgroup_fs_root == NULL || out == NULL ||
        out_sz == 0 || out_is_v2 == NULL) {
        return -1;
    }
    char proc_buf[CBM_SZ_64K];
    if (read_proc_cgroup(proc_self_cgroup_path, proc_buf, sizeof(proc_buf)) < 0) {
        return -1;
    }

    int is_v2 = cgroup_fs_is_v2(cgroup_fs_root);
    *out_is_v2 = is_v2;

    char sub[CBM_PATH_MAX];
    if (is_v2) {
        if (extract_v2_path(proc_buf, sub, sizeof(sub)) != 0) {
            return -1;
        }
        chomp(sub);
        return compose_cgroup_path(cgroup_fs_root, sub, "/cpu.max", out, out_sz);
    }

    /* v1 cpu controller: often listed as "cpu" or "cpu,cpuacct". */
    if (extract_v1_path(proc_buf, "cpu", sub, sizeof(sub)) != 0) {
        return -1;
    }
    chomp(sub);
    int n;
    if (strcmp(sub, "/") == 0) {
        n = snprintf(out, out_sz, "%s/cpu", cgroup_fs_root);
    } else {
        n = snprintf(out, out_sz, "%s/cpu%s", cgroup_fs_root, sub);
    }
    if (n < 0 || (size_t)n >= out_sz) {
        return -1;
    }
    return 0;
}

static cbm_system_info_t detect_system_linux(void) {
    cbm_system_info_t info;
    memset(&info, 0, sizeof(info));

    /* Host fallbacks. */
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    int host_cpus = nprocs > 0 ? (int)nprocs : DEFAULT_CORES;

    size_t host_ram = 0;
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        host_ram = (size_t)si.totalram * (size_t)si.mem_unit;
    }

    /* Cgroup-aware overrides. Prefer the process's own sub-cgroup path
     * (resolved via /proc/self/cgroup) because on Docker / Kubernetes /
     * ECS Fargate the effective limits live in a nested cgroup, not at
     * the fs root. If resolving that path fails for any reason we fall
     * back to reading the root — which preserves the pre-fix behaviour
     * for environments where the process really does live at the root.
     * min(cgroup, host) defends against mis-mounted cgroups that report
     * values larger than the host. */
    int cg_cpus = -1;
    {
        char cpu_path[CBM_PATH_MAX];
        int is_v2 = 0;
        if (cbm_resolve_process_cgroup_cpu_path("/proc/self/cgroup", "/sys/fs/cgroup",
                                                cpu_path, sizeof(cpu_path), &is_v2) == 0) {
            cg_cpus = cbm_detect_cgroup_cpus_file(cpu_path, is_v2);
        }
    }
    if (cg_cpus <= 0) {
        cg_cpus = cbm_detect_cgroup_cpus("/sys/fs/cgroup");
    }
    info.total_cores = (cg_cpus > 0 && cg_cpus < host_cpus) ? cg_cpus : host_cpus;
    info.perf_cores = info.total_cores; /* Linux doesn't distinguish P/E */

    size_t cg_ram = 0;
    {
        char mem_path[CBM_PATH_MAX];
        int is_v2 = 0;
        if (cbm_resolve_process_cgroup_mem_path("/proc/self/cgroup", "/sys/fs/cgroup",
                                                mem_path, sizeof(mem_path), &is_v2) == 0) {
            cg_ram = cbm_detect_cgroup_mem_file(mem_path);
        }
    }
    if (cg_ram == 0) {
        cg_ram = cbm_detect_cgroup_mem("/sys/fs/cgroup");
    }
    info.total_ram = (cg_ram > 0 && (host_ram == 0 || cg_ram < host_ram)) ? cg_ram : host_ram;

    return info;
}

#endif /* __APPLE__ / BSD / Linux */

/* ── Windows detection ───────────────────────────────────────────── */

#ifdef _WIN32
static cbm_system_info_t detect_system_windows(void) {
    cbm_system_info_t info;
    memset(&info, 0, sizeof(info));

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    info.total_cores = (int)si.dwNumberOfProcessors;
    if (info.total_cores < 1) {
        info.total_cores = SKIP_ONE;
    }
    info.perf_cores = info.total_cores;

    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        info.total_ram = (size_t)ms.ullTotalPhys;
    }

    return info;
}
#endif

/* ── Public API ──────────────────────────────────────────────────── */

static int info_cached = 0;
static cbm_system_info_t cached_info;

cbm_system_info_t cbm_system_info(void) {
    if (!info_cached) {
#ifdef _WIN32
        cached_info = detect_system_windows();
#elif defined(__APPLE__)
        cached_info = detect_system_macos();
#elif defined(__NetBSD__) || defined(__FreeBSD__) || defined(__OpenBSD__)
        cached_info = detect_system_bsd();
#else
        cached_info = detect_system_linux();
#endif
        info_cached = SKIP_ONE;
    }
    return cached_info;
}

int cbm_default_worker_count(bool initial) {
    /* CBM_WORKERS env override (clamped to [1, CBM_WORKERS_MAX]).
     * Useful inside containers where sysconf(_SC_NPROCESSORS_ONLN)
     * reports host CPUs rather than the cgroup's effective CPU quota.
     * Same precedence shape as other CBM_* env overrides:
     * explicit override > implicit detection. */
    char buf[CBM_SZ_32];
    if (cbm_safe_getenv("CBM_WORKERS", buf, sizeof(buf), NULL) != NULL) {
        long n = strtol(buf, NULL, CBM_DECIMAL_BASE);
        if (n >= MIN_WORKERS && n <= CBM_WORKERS_MAX) {
            return (int)n;
        }
        cbm_log_warn("workers.env.invalid", "value", buf, "fallback", "sysconf");
    }

    cbm_system_info_t info = cbm_system_info();
    if (initial) {
        /* Use all cores for initial indexing — user is waiting */
        return info.total_cores;
    }
    /* Incremental: leave headroom for user's apps */
    int workers = info.perf_cores - SKIP_ONE;
    return workers > 0 ? workers : MIN_WORKERS;
}
