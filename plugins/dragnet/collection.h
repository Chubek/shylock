#ifndef SHYLOCK_COLLECTION_H
#define SHYLOCK_COLLECTION_H
#include "naive.h"
/* One owning, synchronous collection session. Native calls only load/clone,
   compare or attach records. On1x admits links, duplicates, statuses and gates.
   View scratch is borrowed until the next view/close; no pointer crosses FFI.
   Finish compares the complete rebuilt bundle before enabling review access. */
int shy_collection_begin_record(const mulib_json_t *);
const mulib_json_t *shy_collection_verified(void);
#endif
