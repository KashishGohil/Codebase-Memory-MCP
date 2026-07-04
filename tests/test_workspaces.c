/*
 * test_workspaces.c -- Tests for npm-workspace member resolution (#271).
 *
 * Covers the collect-then-finalize detector (cbm_workspace_try_detect,
 * cbm_workspaces_finalize): `workspaces` array vs object form, member-candidate
 * recording, glob matching (single-star, doublestar, negation), first-wins
 * collision, NULL-safety, and the end-to-end filesystem walk via
 * cbm_pkgmap_scan_repo. The resolver hook (Strategy 1c) is exercised by the
 * cross-package IMPORTS contract tests in test_lang_contract.c.
 */
#include "test_framework.h"
#include "test_helpers.h"
#include "../src/foundation/compat.h"

#include "pipeline/pipeline_internal.h"
#include "foundation/hash_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Feed one package.json manifest to the detector. */
static bool detect(cbm_workspaces_t *ws, const char *rel_path, const char *json) {
    return cbm_workspace_try_detect("package.json", rel_path, json, (int)strlen(json), ws);
}

/* Look up a finalized member's directory (borrowed), or NULL. */
static const char *member_dir(cbm_workspaces_t *ws, const char *name) {
    return ws->members ? (const char *)cbm_ht_get(ws->members, name) : NULL;
}

/* ── Detection ─────────────────────────────────────────────────── */

TEST(workspaces_detect_array) {
    cbm_workspaces_t *ws = cbm_workspaces_new();
    ASSERT_NOT_NULL(ws);
    /* Private root (no "name") declaring an array of globs. */
    ASSERT_TRUE(detect(ws, "package.json", "{\"private\":true,\"workspaces\":[\"packages/*\"]}"));
    ASSERT_EQ(ws->root_count, 1);
    ASSERT_EQ(ws->cand_count, 0); /* no "name" → no member candidate */
    cbm_workspaces_free(ws);
    PASS();
}

TEST(workspaces_detect_object_packages) {
    cbm_workspaces_t *ws = cbm_workspaces_new();
    ASSERT_NOT_NULL(ws);
    /* Yarn object form: {"workspaces":{"packages":[...]}}. */
    ASSERT_TRUE(
        detect(ws, "package.json", "{\"workspaces\":{\"packages\":[\"libs/*\",\"apps/*\"]}}"));
    ASSERT_EQ(ws->root_count, 1);
    /* Both globs must resolve members under their respective roots. Member
     * manifests declare no `workspaces`, so detect() returns false for them. */
    detect(ws, "libs/x/package.json", "{\"name\":\"@org/x\"}");
    detect(ws, "apps/y/package.json", "{\"name\":\"@org/y\"}");
    cbm_workspaces_finalize(ws);
    ASSERT_STR_EQ(member_dir(ws, "@org/x"), "libs/x");
    ASSERT_STR_EQ(member_dir(ws, "@org/y"), "apps/y");
    cbm_workspaces_free(ws);
    PASS();
}

TEST(workspaces_member_candidate) {
    cbm_workspaces_t *ws = cbm_workspaces_new();
    ASSERT_NOT_NULL(ws);
    /* A named manifest with no `workspaces` is a member candidate, not a root. */
    ASSERT_FALSE(
        detect(ws, "packages/a/package.json", "{\"name\":\"@org/a\",\"version\":\"1.0.0\"}"));
    ASSERT_EQ(ws->root_count, 0);
    ASSERT_EQ(ws->cand_count, 1);
    ASSERT_STR_EQ(ws->cands[0].name, "@org/a");
    ASSERT_STR_EQ(ws->cands[0].dir, "packages/a");
    cbm_workspaces_free(ws);
    PASS();
}

/* ── Finalize / glob matching ──────────────────────────────────── */

TEST(workspaces_finalize_glob_match) {
    cbm_workspaces_t *ws = cbm_workspaces_new();
    ASSERT_NOT_NULL(ws);
    detect(ws, "package.json", "{\"workspaces\":[\"packages/*\"]}");
    detect(ws, "packages/a/package.json", "{\"name\":\"@org/a\"}");
    detect(ws, "tools/x/package.json", "{\"name\":\"@org/x\"}"); /* outside the glob */
    cbm_workspaces_finalize(ws);
    ASSERT_STR_EQ(member_dir(ws, "@org/a"), "packages/a");
    ASSERT_NULL(member_dir(ws, "@org/x")); /* tools/x is not under packages/ */
    cbm_workspaces_free(ws);
    PASS();
}

TEST(workspaces_doublestar) {
    cbm_workspaces_t *ws = cbm_workspaces_new();
    ASSERT_NOT_NULL(ws);
    detect(ws, "package.json", "{\"workspaces\":[\"packages/**\"]}");
    /* Single-star would NOT reach a nested member; doublestar must. */
    detect(ws, "packages/group/a/package.json", "{\"name\":\"@org/deep\"}");
    cbm_workspaces_finalize(ws);
    ASSERT_STR_EQ(member_dir(ws, "@org/deep"), "packages/group/a");
    cbm_workspaces_free(ws);
    PASS();
}

TEST(workspaces_single_star_not_nested) {
    cbm_workspaces_t *ws = cbm_workspaces_new();
    ASSERT_NOT_NULL(ws);
    detect(ws, "package.json", "{\"workspaces\":[\"packages/*\"]}");
    detect(ws, "packages/a/package.json", "{\"name\":\"@org/a\"}");
    detect(ws, "packages/group/nested/package.json", "{\"name\":\"@org/nested\"}");
    cbm_workspaces_finalize(ws);
    ASSERT_STR_EQ(member_dir(ws, "@org/a"), "packages/a");
    ASSERT_NULL(member_dir(ws, "@org/nested")); /* single-star matches one level only */
    cbm_workspaces_free(ws);
    PASS();
}

/* npm workspace globs are root-relative: a slash-less pattern like "*" must
 * match a top-level member but NOT a candidate nested deeper (regression guard
 * for the unrooted-pattern bug — without the leading-'/' anchor, the gitignore
 * engine matched "*" against a dir at any depth). */
TEST(workspaces_root_relative_no_nested) {
    cbm_workspaces_t *ws = cbm_workspaces_new();
    ASSERT_NOT_NULL(ws);
    detect(ws, "package.json", "{\"workspaces\":[\"*\"]}");
    detect(ws, "toplevel/package.json", "{\"name\":\"@org/top\"}");
    detect(ws, "deep/nest/package.json", "{\"name\":\"@org/nested\"}");
    cbm_workspaces_finalize(ws);
    ASSERT_STR_EQ(member_dir(ws, "@org/top"), "toplevel");
    ASSERT_NULL(member_dir(ws, "@org/nested")); /* "*" is root-relative, one level */
    cbm_workspaces_free(ws);
    PASS();
}

TEST(workspaces_negation) {
    cbm_workspaces_t *ws = cbm_workspaces_new();
    ASSERT_NOT_NULL(ws);
    detect(ws, "package.json", "{\"workspaces\":[\"packages/*\",\"!packages/skip\"]}");
    detect(ws, "packages/a/package.json", "{\"name\":\"@org/a\"}");
    detect(ws, "packages/skip/package.json", "{\"name\":\"@org/skip\"}");
    cbm_workspaces_finalize(ws);
    ASSERT_STR_EQ(member_dir(ws, "@org/a"), "packages/a");
    ASSERT_NULL(member_dir(ws, "@org/skip")); /* negation excludes it */
    cbm_workspaces_free(ws);
    PASS();
}

TEST(workspaces_first_wins_collision) {
    cbm_workspaces_t *ws = cbm_workspaces_new();
    ASSERT_NOT_NULL(ws);
    detect(ws, "package.json", "{\"workspaces\":[\"packages/*\"]}");
    /* Two members declaring the same name; the first to be recorded wins. */
    detect(ws, "packages/a/package.json", "{\"name\":\"@org/dup\"}");
    detect(ws, "packages/a2/package.json", "{\"name\":\"@org/dup\"}");
    cbm_workspaces_finalize(ws);
    ASSERT_STR_EQ(member_dir(ws, "@org/dup"), "packages/a");
    cbm_workspaces_free(ws);
    PASS();
}

TEST(workspaces_no_workspaces) {
    cbm_workspaces_t *ws = cbm_workspaces_new();
    ASSERT_NOT_NULL(ws);
    /* Named members but no root declaring `workspaces` → no members. */
    detect(ws, "packages/a/package.json", "{\"name\":\"@org/a\"}");
    detect(ws, "packages/b/package.json", "{\"name\":\"@org/b\"}");
    cbm_workspaces_finalize(ws);
    ASSERT_EQ((int)cbm_ht_count(ws->members), 0);
    cbm_workspaces_free(ws);
    PASS();
}

/* ── NULL / edge safety ────────────────────────────────────────── */

TEST(workspaces_null_safety) {
    /* All entry points must be NULL-safe (mirror path_alias_null_safety). */
    ASSERT_FALSE(cbm_workspace_try_detect("package.json", "package.json", "{}", 2, NULL));
    cbm_workspaces_finalize(NULL);
    cbm_workspaces_free(NULL);

    cbm_workspaces_t *ws = cbm_workspaces_new();
    ASSERT_NOT_NULL(ws);
    ASSERT_FALSE(cbm_workspace_try_detect(NULL, "p/package.json", "{}", 2, ws));
    ASSERT_FALSE(
        cbm_workspace_try_detect("go.mod", "go.mod", "module x", 8, ws)); /* wrong basename */
    ASSERT_FALSE(cbm_workspace_try_detect("package.json", "p/package.json", "not json{", 9, ws));
    ASSERT_EQ(ws->root_count, 0);
    /* finalize on an empty collection yields an empty (non-NULL) member map. */
    cbm_workspaces_finalize(ws);
    ASSERT_NOT_NULL(ws->members);
    ASSERT_EQ((int)cbm_ht_count(ws->members), 0);
    cbm_workspaces_free(ws);
    PASS();
}

/* ── End-to-end: real filesystem walk via cbm_pkgmap_scan_repo ──── */

TEST(workspaces_scan_repo_e2e) {
    char tmpl[256];
    snprintf(tmpl, sizeof(tmpl), "/tmp/cbm_ws_XXXXXX");
    char *root = cbm_mkdtemp(tmpl);
    ASSERT_NOT_NULL(root);

    ASSERT_EQ(th_write_file(TH_PATH(root, "package.json"),
                            "{\"private\":true,\"workspaces\":[\"packages/*\"]}\n"),
              0);
    /* Member A's declared entry is an un-built artifact (dist/index.js). */
    ASSERT_EQ(th_write_file(TH_PATH(root, "packages/a/package.json"),
                            "{\"name\":\"@org/a\",\"main\":\"dist/index.js\"}\n"),
              0);
    ASSERT_EQ(th_write_file(TH_PATH(root, "packages/a/src/index.ts"),
                            "export function fromA() { return 1; }\n"),
              0);

    cbm_workspaces_t *ws = cbm_workspaces_new();
    ASSERT_NOT_NULL(ws);
    cbm_pkg_entries_t entries;
    cbm_pkg_entries_init(&entries);
    /* The same walk that feeds pkgmap also populates workspace state. */
    cbm_pkgmap_scan_repo(root, &entries, NULL, 0, ws);
    cbm_workspaces_finalize(ws);

    ASSERT_STR_EQ(member_dir(ws, "@org/a"), "packages/a");

    cbm_pkg_entries_free(&entries);
    cbm_workspaces_free(ws);
    th_rmtree(root);
    PASS();
}

/* ── Suite ─────────────────────────────────────────────────────── */

SUITE(workspaces) {
    RUN_TEST(workspaces_detect_array);
    RUN_TEST(workspaces_detect_object_packages);
    RUN_TEST(workspaces_member_candidate);
    RUN_TEST(workspaces_finalize_glob_match);
    RUN_TEST(workspaces_doublestar);
    RUN_TEST(workspaces_single_star_not_nested);
    RUN_TEST(workspaces_root_relative_no_nested);
    RUN_TEST(workspaces_negation);
    RUN_TEST(workspaces_first_wins_collision);
    RUN_TEST(workspaces_no_workspaces);
    RUN_TEST(workspaces_null_safety);
    RUN_TEST(workspaces_scan_repo_e2e);
}
