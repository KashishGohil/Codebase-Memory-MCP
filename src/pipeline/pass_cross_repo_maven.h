/*
 * pass_cross_repo_maven.h - Maven artifact matching for cross-repo links.
 */
#ifndef CBM_PASS_CROSS_REPO_MAVEN_H
#define CBM_PASS_CROSS_REPO_MAVEN_H

#include "store/store.h"

int cbm_cross_repo_match_maven_libraries(cbm_store_t *src_store, const char *src_project,
                                         cbm_store_t *tgt_store, const char *tgt_project);

#endif /* CBM_PASS_CROSS_REPO_MAVEN_H */
