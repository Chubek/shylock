#ifndef SHYLOCK_DRAGNET_WORKFLOW_H
#define SHYLOCK_DRAGNET_WORKFLOW_H
#include <stdint.h>
#include "../../src/shylock.h"
#define SHY_ELVI_PROFILE "dragnet.elvi/hardened-v1"
mulib_json_t *shy_snapshot_record(const char *,size_t,const char *,int64_t,const char *,unsigned,const char *);
int shy_snapshot_valid(const mulib_json_t *);
/* Synchronous owning session: prepare/replay replaces prior response. Publish
   consumes the result. Close is idempotent, including after allocation failure.
   Batch owns copied job/results independently of the current response session. */
int64_t shy_dragnet_prepare(const char *,const char *,const char *,const char *,int64_t);
int64_t shy_dragnet_count(void);
int64_t shy_dragnet_publish(const char *,const char *);
int64_t shy_dragnet_close(void);
int64_t shy_dragnet_snapshot(const char *,int64_t,const char *);
int64_t shy_dragnet_extract_snapshot(const char *,const char *,const char *,const char *);
/* Replay: 0 prepared, 1 incompatible runtime/profile, -1 invalid artifact.
   A different parser revision must never silently relabel historical results. */
int64_t shy_dragnet_replay(const char *);
/* Internal borrowed-record replay; owns a clone. Check seals/compares without
   publishing or consuming, for composed On1x workflows. */
int shy_dragnet_replay_record(const mulib_json_t *);
int shy_dragnet_check(const char *,const char *);
int64_t shy_dragnet_batch_begin(const char *);
int64_t shy_dragnet_batch_error(const char *);
int64_t shy_dragnet_batch_finish(const char *);
int64_t shy_dragnet_batch_close(void);
#endif
