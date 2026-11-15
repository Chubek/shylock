#include "../../include/shylock-sdk.h"
#include <string.h>

/* Build as a shared library with ONLY this public header. No host/MuLIB link. */
static int load(const shy_sdk_host *host,const char *configuration,void **state)
{
    (void)configuration;
    if (!host || host->abi!=SHY_SDK_ABI || host->size<sizeof *host) return 0;
    const shy_sdk_host **copy=host->allocate(sizeof *copy);
    if (!copy) return 0;
    *copy=host; *state=copy; return 1;
}
static int extract(void *state,const shy_sdk_capture *capture,shy_sdk_sink *sink)
{
    const shy_sdk_host *host=*(const shy_sdk_host **)state;
    if (capture->abi!=SHY_SDK_ABI || capture->size<sizeof *capture) return 0;
    const char quote[]="EUR 1.00 per night";
    for (size_t i=0;i+sizeof quote-1<=capture->length;++i) {
        if (memcmp(capture->bytes+i,quote,sizeof quote-1)) continue;
        shy_sdk_candidate c={sizeof c,"fixed-example","1.00","EUR","nightly","unspecified",i,sizeof quote-1,1,100,NULL};
        if (!host->candidate(sink,&c)) return 0;
    }
    return 1;
}
static void unload(void *state)
{
    const shy_sdk_host *host=*(const shy_sdk_host **)state;
    host->release(state);
}
static const shy_sdk_plugin plugin={SHY_SDK_ABI,sizeof plugin,"fixed-price","fixed-price/v1",0,load,extract,unload};
SHY_SDK_EXPORT const shy_sdk_plugin *shylock_plugin_query(uint32_t abi)
{ return abi==SHY_SDK_ABI ? &plugin : NULL; }
