/*
 * mem.h — Unified memory management via mimalloc.
 *
 * Provides budget tracking based on actual RSS (not partial vmem tracking).
 * Uses mi_process_info() as the single source of truth for memory pressure.
 * Replaces the old vmem.h budget-tracked virtual memory allocator.
 */
#ifndef CBM_MEM_H
#define CBM_MEM_H

#include <stdbool.h>
#include <stddef.h>

/* Initialize memory budget = ram_fraction * total_physical_ram.
 * Thread-safe: only the first call takes effect.
 * Configures mimalloc options for reduced upfront memory. */
void cbm_mem_init(double ram_fraction);

/* Current RSS in bytes via mi_process_info().
 * Falls back to OS-specific queries when MI_OVERRIDE=0 (ASan builds). */
size_t cbm_mem_rss(void);

/* Peak RSS in bytes. */
size_t cbm_mem_peak_rss(void);

/* Total budget in bytes. */
size_t cbm_mem_budget(void);

/* Returns true if current RSS exceeds the budget. */
bool cbm_mem_over_budget(void);

/* Per-worker budget hint: budget / num_workers. */
size_t cbm_mem_worker_budget(int num_workers);

/* Return unused pages to the OS. Call between files to bound per-file peak. */
void cbm_mem_collect(void);

/* ── Hard memory ceiling (abort, not advisory) ───────────────────
 *
 * Distinct from cbm_mem_budget()/cbm_mem_over_budget() above, which are
 * ADVISORY: pass_parallel.c backpressure naps and proceeds with a soft
 * overshoot when workers can't get back under budget. The ceiling below is
 * ENFORCING: cbm_mem_abort_if_over_ceiling() hard-aborts the process
 * (abort(), SIGABRT) when exceeded, after emitting a diagnostic dump naming
 * the offending file, pipeline phase, and RSS. It is always set strictly
 * above the advisory budget (see cbm_mem_ceiling), so a repo that trips the
 * advisory backpressure but recovers never reaches the ceiling.
 *
 * Call cbm_mem_abort_if_over_ceiling() only from the in-memory
 * extract/resolve phases, BEFORE the graph buffer is dumped to SQLite
 * (pipeline.c: run_parallel_pipeline() precedes dump_and_persist_hashes()).
 * An abort there can never leave a half-written store. */

/* Hard-abort ceiling in bytes: max(CBM_MEM_CEILING_FRACTION * total_ram,
 * CBM_MEM_CEILING_FLOOR_MB), unless overridden by the CBM_MEM_CEILING_MB
 * env var (same precedence/clamp shape as CBM_WORKERS / CBM_MAX_FILE_MB).
 * Always computed fresh (env can change between calls in tests); cheap
 * (one getenv + the cached cbm_system_info()). */
size_t cbm_mem_ceiling(void);

/* Returns true if current RSS exceeds cbm_mem_ceiling(). */
bool cbm_mem_over_ceiling(void);

/* If current RSS exceeds cbm_mem_ceiling(), log a diagnostic dump (offending
 * file, phase, RSS, ceiling) at ERROR level to stderr and hard-abort the
 * process via abort(). Returns (does nothing) otherwise. `file` and `phase`
 * may be NULL (logged as "unknown"/"n/a") when the caller has no better
 * label at the call site. Not signal-safe; call only from a normal worker
 * context, never from a signal handler. */
void cbm_mem_abort_if_over_ceiling(const char *file, const char *phase);

#endif /* CBM_MEM_H */
