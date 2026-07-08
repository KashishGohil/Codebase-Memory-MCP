/*
 * system_info_internal.h — Internal helpers exposed for testing.
 *
 * These functions are implementation details of system_info.c; they are
 * declared here only so that test_platform.c can drive them against a
 * fake cgroup filesystem. Production code outside system_info.c should
 * use the public APIs in platform.h instead.
 */
#ifndef CBM_FOUNDATION_SYSTEM_INFO_INTERNAL_H
#define CBM_FOUNDATION_SYSTEM_INFO_INTERNAL_H

#include <stddef.h>

#ifdef __linux__

/*
 * Effective CPU count for the cgroup rooted at `cgroup_root`.
 *
 * Reads (in order):
 *   1. cgroup v2: "<cgroup_root>/cpu.max"            ("<quota> <period>" or "max ...")
 *   2. cgroup v1: "<cgroup_root>/cpu/cpu.cfs_quota_us" + ".../cpu.cfs_period_us"
 *
 * Returns ceil(quota / period) (>= 1) when a valid CPU quota is in place.
 * Returns -1 when no cgroup limit is present (caller should fall back to
 * sysconf(_SC_NPROCESSORS_ONLN)).
 */
int cbm_detect_cgroup_cpus(const char *cgroup_root);

/*
 * Effective memory limit (bytes) for the cgroup rooted at `cgroup_root`.
 *
 * Reads (in order):
 *   1. cgroup v2: "<cgroup_root>/memory.max"             ("max" or integer bytes)
 *   2. cgroup v1: "<cgroup_root>/memory/memory.limit_in_bytes"
 *
 * Returns the byte count when a finite limit is in place. Returns 0 when
 * no cgroup limit is present, the limit is "max"/unlimited, or the value
 * is so large it represents the cgroup-v1 "unlimited" sentinel.
 */
size_t cbm_detect_cgroup_mem(const char *cgroup_root);

/*
 * Effective memory limit (bytes) read directly from `mem_file_path`.
 *
 * `mem_file_path` must point at either a cgroup-v2 "memory.max" file or
 * a cgroup-v1 "memory.limit_in_bytes" file — the two share a compatible
 * grammar (integer bytes, or "max" / the v1 near-ULLONG_MAX sentinel for
 * "unlimited"). Used by callers that have already resolved the process's
 * own sub-cgroup path via /proc/self/cgroup rather than blindly reading
 * the cgroup filesystem root.
 *
 * Returns the byte count when a finite limit is in place, or 0 when the
 * file is missing, the limit is "max"/unlimited, or the value trips the
 * cgroup-v1 unlimited sentinel.
 */
size_t cbm_detect_cgroup_mem_file(const char *mem_file_path);

/*
 * Effective CPU count from an already-resolved cgroup path.
 *
 * When `is_v2` is non-zero, `path` must be a "cpu.max" file (v2 grammar:
 * "<quota> <period>" or "max ..."). When `is_v2` is zero, `path` must be
 * a directory containing v1's "cpu.cfs_quota_us" and "cpu.cfs_period_us".
 *
 * Returns ceil(quota / period) when a valid CPU quota is present, or -1
 * when the limit is "max"/unlimited or the files are missing/malformed.
 */
int cbm_detect_cgroup_cpus_file(const char *path, int is_v2);

/*
 * Resolve the memory-cgroup file path for the current process.
 *
 * Parses `proc_self_cgroup_path` (typically "/proc/self/cgroup") to find
 * the process's own sub-cgroup and combines it with `cgroup_fs_root`
 * (typically "/sys/fs/cgroup") to produce the exact file the caller
 * should read. The cgroup version is auto-detected: v2 if
 * "<cgroup_fs_root>/cgroup.controllers" exists, otherwise v1.
 *
 * On success writes the resolved path into `out` (of size `out_sz`),
 * sets `*out_is_v2` to 1 (v2) or 0 (v1), and returns 0. Returns -1 on
 * any error (unreadable /proc file, malformed line, missing memory
 * controller entry in v1, buffer overflow).
 */
int cbm_resolve_process_cgroup_mem_path(const char *proc_self_cgroup_path,
                                        const char *cgroup_fs_root,
                                        char *out,
                                        size_t out_sz,
                                        int *out_is_v2);

/*
 * Resolve the cpu-cgroup path for the current process.
 *
 * For v2 the output is a "cpu.max" file path; for v1 it is the directory
 * containing "cpu.cfs_quota_us" and "cpu.cfs_period_us" (which the v1
 * controller path already gives us — no extra "cpu/" prefix is added
 * beyond the sub-cgroup path itself). Auto-detects v2/v1 the same way
 * as the memory resolver. See its docstring for arguments and return.
 */
int cbm_resolve_process_cgroup_cpu_path(const char *proc_self_cgroup_path,
                                        const char *cgroup_fs_root,
                                        char *out,
                                        size_t out_sz,
                                        int *out_is_v2);

#endif /* __linux__ */

#endif /* CBM_FOUNDATION_SYSTEM_INFO_INTERNAL_H */
