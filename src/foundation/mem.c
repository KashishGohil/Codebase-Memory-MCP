/*
 * mem.c — Unified memory management via mimalloc.
 *
 * Budget tracking based on actual RSS via mi_process_info().
 * When MI_OVERRIDE=0 (ASan builds), falls back to OS-specific
 * RSS queries (task_info on macOS, /proc/self/statm on Linux,
 * GetProcessMemoryInfo on Windows).
 */
#include "mem.h"
#include "platform.h"
#include "log.h"

#include "foundation/constants.h"

#define MAX_RAM_FRACTION 1.0
#define DEFAULT_RAM_FRACTION 0.5
#include <mimalloc.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#else
#include <unistd.h>
#endif

/* ── Static state ─────────────────────────────────────────────── */

static size_t g_budget;          /* budget in bytes */
static atomic_int g_initialized; /* init guard */
static atomic_int g_was_over;    /* pressure hysteresis */

#define MB_DIVISOR ((size_t)(CBM_SZ_1K * CBM_SZ_1K))

/* ── Hard memory ceiling (enforcing — see mem.h) ─────────────────
 *
 * Fraction of cgroup-aware detected RAM (cbm_system_info().total_ram),
 * always strictly above the advisory DEFAULT_RAM_FRACTION budget so a
 * repo that trips backpressure but recovers never reaches the ceiling.
 * Absolute floor protects a legit big repo on a small-RAM CI runner from
 * spuriously aborting at a tiny fraction-derived value. */
#define CBM_MEM_CEILING_FRACTION 0.85
#define CBM_MEM_CEILING_FLOOR_MB ((size_t)2048) /* 2 GB floor */
#define CBM_MEM_CEILING_CAP_MB ((size_t)1024 * 1024) /* 1 TB env-override ceiling */

/* ── OS fallback for RSS (ASan builds where MI_OVERRIDE=0) ──── */

static size_t os_rss(void) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return (size_t)pmc.WorkingSetSize;
    }
    return 0;
#elif defined(__APPLE__)
    struct mach_task_basic_info info = {0};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) ==
        KERN_SUCCESS) {
        return (size_t)info.resident_size;
    }
    return 0;
#else
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) {
        return 0;
    }
    unsigned long pages = 0;
    unsigned long rss_pages = 0;
    if (fscanf(f, "%lu %lu", &pages, &rss_pages) != 2) {
        rss_pages = 0;
    }
    (void)fclose(f);
    long ps = sysconf(_SC_PAGESIZE);
    return rss_pages * (ps > 0 ? (size_t)ps : CBM_SZ_4K);
#endif
}

/* ── Pressure logging (hysteresis) ────────────────────────────── */

static void check_pressure(size_t rss) {
    if (g_budget == 0) {
        return;
    }

    bool over = rss > g_budget;
    int was = atomic_load(&g_was_over);

    if (over && !was) {
        atomic_store(&g_was_over, 1);
        char rss_mb[CBM_SZ_32];
        char budget_mb[CBM_SZ_32];
        char pct_str[CBM_SZ_16];
        snprintf(rss_mb, sizeof(rss_mb), "%zu", rss / MB_DIVISOR);
        snprintf(budget_mb, sizeof(budget_mb), "%zu", g_budget / MB_DIVISOR);
        snprintf(pct_str, sizeof(pct_str), "%zu",
                 g_budget > 0 ? (rss * CBM_PERCENT) / g_budget : 0);
        cbm_log_warn("mem.pressure.warn", "rss_mb", rss_mb, "budget_mb", budget_mb, "pct", pct_str);
    } else if (!over && was) {
        atomic_store(&g_was_over, 0);
        char rss_mb[CBM_SZ_32];
        char budget_mb[CBM_SZ_32];
        char pct_str[CBM_SZ_16];
        snprintf(rss_mb, sizeof(rss_mb), "%zu", rss / MB_DIVISOR);
        snprintf(budget_mb, sizeof(budget_mb), "%zu", g_budget / MB_DIVISOR);
        snprintf(pct_str, sizeof(pct_str), "%zu",
                 g_budget > 0 ? (rss * CBM_PERCENT) / g_budget : 0);
        cbm_log_info("mem.pressure.ok", "rss_mb", rss_mb, "budget_mb", budget_mb, "pct", pct_str);
    }
}

/* ── Public API ────────────────────────────────────────────────── */

void cbm_mem_init(double ram_fraction) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_initialized, &expected, 1)) {
        return;
    }

    if (ram_fraction <= 0.0 || ram_fraction > MAX_RAM_FRACTION) {
        ram_fraction = DEFAULT_RAM_FRACTION;
    }

    /* Reduce upfront memory: don't eagerly commit arenas.
     * Force decommit on purge (MADV_FREE_REUSABLE on macOS) so RSS
     * drops immediately instead of staying high until memory pressure. */
    mi_option_set(mi_option_arena_eager_commit, 0);
    mi_option_set(mi_option_purge_decommits, SKIP_ONE);
    mi_option_set(mi_option_purge_delay, 0); /* immediate purge, no 1s delay */

    cbm_system_info_t info = cbm_system_info();
    g_budget = (size_t)((double)info.total_ram * ram_fraction);

    char budget_mb[CBM_SZ_32];
    char ram_mb[CBM_SZ_32];
    snprintf(budget_mb, sizeof(budget_mb), "%zu", g_budget / MB_DIVISOR);
    snprintf(ram_mb, sizeof(ram_mb), "%zu", info.total_ram / MB_DIVISOR);
    cbm_log_info("mem.init", "budget_mb", budget_mb, "total_ram_mb", ram_mb);
}

size_t cbm_mem_rss(void) {
    size_t current_rss = 0;
    size_t peak_rss = 0;
    mi_process_info(NULL, NULL, NULL, &current_rss, &peak_rss, NULL, NULL, NULL);
    if (current_rss > 0) {
        return current_rss;
    }
    /* Fallback for ASan builds (MI_OVERRIDE=0) */
    return os_rss();
}

size_t cbm_mem_peak_rss(void) {
    size_t peak_rss = 0;
    mi_process_info(NULL, NULL, NULL, NULL, &peak_rss, NULL, NULL, NULL);
    if (peak_rss > 0) {
        return peak_rss;
    }
    /* No OS fallback for peak — return current as best approximation */
    return os_rss();
}

size_t cbm_mem_budget(void) {
    return g_budget;
}

bool cbm_mem_over_budget(void) {
    size_t rss = cbm_mem_rss();
    check_pressure(rss);
    return rss > g_budget;
}

size_t cbm_mem_worker_budget(int num_workers) {
    if (num_workers <= 0) {
        num_workers = SKIP_ONE;
    }
    return g_budget / (size_t)num_workers;
}

void cbm_mem_collect(void) {
    mi_collect(true);
}

/* ── Hard memory ceiling (enforcing) ─────────────────────────────── */

size_t cbm_mem_ceiling(void) {
    /* CBM_MEM_CEILING_MB env override (clamped to [CBM_MEM_CEILING_FLOOR_MB,
     * CBM_MEM_CEILING_CAP_MB]). Same precedence/clamp shape as
     * CBM_WORKERS / CBM_MAX_FILE_MB: unset, blank, or non-numeric all parse
     * to 0 via strtoull, which falls below the floor and is rejected the
     * same way an out-of-range value is. */
    char buf[CBM_SZ_32];
    if (cbm_safe_getenv("CBM_MEM_CEILING_MB", buf, sizeof(buf), NULL) != NULL) {
        char *end = NULL;
        unsigned long long mb = strtoull(buf, &end, CBM_DECIMAL_BASE);
        if (end != buf && mb >= CBM_MEM_CEILING_FLOOR_MB && mb <= CBM_MEM_CEILING_CAP_MB) {
            return (size_t)mb * MB_DIVISOR;
        }
        cbm_log_warn("mem_ceiling.env.invalid", "value", buf, "fallback", "fraction");
    }

    cbm_system_info_t info = cbm_system_info();
    size_t fraction_bytes = (size_t)((double)info.total_ram * CBM_MEM_CEILING_FRACTION);
    size_t floor_bytes = CBM_MEM_CEILING_FLOOR_MB * MB_DIVISOR;
    return fraction_bytes > floor_bytes ? fraction_bytes : floor_bytes;
}

bool cbm_mem_over_ceiling(void) {
    return cbm_mem_rss() > cbm_mem_ceiling();
}

void cbm_mem_abort_if_over_ceiling(const char *file, const char *phase) {
    size_t rss = cbm_mem_rss();
    size_t ceiling = cbm_mem_ceiling();
    if (rss <= ceiling) {
        return;
    }

    char rss_mb[CBM_SZ_32];
    char ceiling_mb[CBM_SZ_32];
    snprintf(rss_mb, sizeof(rss_mb), "%zu", rss / MB_DIVISOR);
    snprintf(ceiling_mb, sizeof(ceiling_mb), "%zu", ceiling / MB_DIVISOR);
    cbm_log_error("mem.ceiling.abort", "file", file ? file : "unknown", "phase",
                 phase ? phase : "n/a", "rss_mb", rss_mb, "ceiling_mb", ceiling_mb);
    /* Hard abort: SIGABRT, default handler, non-zero exit. Intentionally not
     * a graceful cancel — cbm_pipeline_cancel() already exists for that path
     * and is advisory-cooperative (workers check an atomic and unwind); RSS
     * already over the enforcing ceiling means further allocation to unwind
     * cleanly (free lists, log buffers) is itself the risk being guarded
     * against, so we terminate immediately instead. Must only be reached
     * from the in-memory extract/resolve phases, before any SQLite dump. */
    abort();
}
