#ifndef SHYLOCK_NAIVE_H
#define SHYLOCK_NAIVE_H
#include "../../src/shylock.h"
#include "mulib-html-treebuilder.h"
#include "mulib-regex.h"
#include "mulib-shylock-core.h"
enum { NF_NODES=8192, NF_DEPTH=96, NF_TEXT=8192, NF_HITS=64, NF_RULE=65536 };
typedef struct {
    const mulib_html_tree_node_t *node;
    size_t end, parent;
    int hidden;
} nf_node;
typedef struct {
    char amount[129], currency[4], symbol[8];
    size_t start, length, offset, span;
    int exact;
} nf_hit;
typedef struct {
    mulib_json_t *snapshot, *legacy, *expected, *result;
    char *rule, rule_hash[65], host[256];
    mulib_html_tree_node_t *tree;
    nf_node *nodes;
    size_t count;
    uint64_t generation;
    mulib_shylock_buffer_t text;
    size_t *starts, *ends;
    unsigned char *exact;
    nf_hit hits[NF_HITS];
    size_t hits_count, work, query_work;
    char *scratch;
    int failed, replayed;
} nf_session;
/* Single lexical, non-reentrant scalar-ABI session, like document.c. Node
   handles encode generation and preorder index; zero is the document root.
   Strings are borrowed until the next scratch/text operation or close. */
extern nf_session shy_nf;
int nf_fail(void);
size_t nf_index(int64_t handle);
int64_t nf_handle(size_t index);
int nf_dom_open(void);
int nf_text(size_t index, const char *attribute);
int nf_scan(const char *pattern);
int nf_token(const char *text, size_t length, nf_hit *hit);
int64_t shy_naive_close(void);
int64_t shy_naive_begin(const char *source, const char *rule);
int shy_naive_begin_record(const mulib_json_t *record);
int64_t shy_naive_replay_begin(const char *path);
const char *shy_naive_rule(void);
const char *shy_naive_meta(const char *key);
int64_t shy_naive_query(int64_t scope, const char *selector, int64_t after);
int64_t shy_naive_owner(int64_t offset, int64_t scope);
int64_t shy_naive_visible(int64_t node);
int64_t shy_naive_structured_visible(int64_t node);
const char *shy_naive_claim_field(int64_t index, const char *key);
int64_t shy_naive_offset(int64_t node);
int64_t shy_naive_scan(int64_t node, const char *attribute, const char *pattern);
const char *shy_naive_text(int64_t node, const char *attribute);
const char *shy_naive_hit_text(int64_t index, const char *key);
int64_t shy_naive_hit_number(int64_t index, const char *key);
int64_t shy_naive_money(const char *amount, const char *currency, const char *decimal, const char *group);
int64_t shy_naive_regex_valid(const char *pattern);
const char *shy_naive_span_digest(int64_t offset, int64_t length);
const char *shy_naive_identify(const char *json, const char *key);
const char *shy_naive_quote(const char *text);
const char *shy_naive_decode(const char *text);
int64_t shy_naive_finish(const char *json);
int64_t shy_naive_emit(const char *mode, const char *profile, const char *target, int64_t call);
/* Borrowed, only after full equality with a replayed artifact. */
const mulib_json_t *shy_naive_verified(void);
/* Borrow the finalized result for composition; not a replay-verification claim. */
const mulib_json_t *shy_naive_result(void);
/* Composed collection scalar ABI. Ownership is documented in collection.h. */
int64_t shy_collection_begin(const char *expected);
int64_t shy_collection_ready(void);
int64_t shy_collection_close(void);
int64_t shy_collection_size(void);
int64_t shy_collection_entry_hit(int64_t);
const char *shy_collection_target(void);
int64_t shy_collection_source(int64_t,const char *,const char *,const char *);
const char *shy_collection_discovery_field(const char *);
const char *shy_collection_hit(int64_t);
int64_t shy_collection_check(const char *,const char *);
const char *shy_collection_view(const char *);
int64_t shy_collection_append(const char *);
int64_t shy_collection_finish(const char *,int64_t);
int64_t shy_collection_review_begin(int64_t);
#endif
