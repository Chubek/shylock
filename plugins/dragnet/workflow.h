#ifndef SHYLOCK_DRAGNET_WORKFLOW_H
#define SHYLOCK_DRAGNET_WORKFLOW_H
#include <stdint.h>
#include "../../src/shylock.h"
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
int64_t shy_dragnet_replay(const char *);
int64_t shy_dragnet_batch_begin(const char *);
int64_t shy_dragnet_batch_error(const char *);
int64_t shy_dragnet_batch_finish(const char *);
int64_t shy_dragnet_batch_close(void);
#endif
