#include "../include/shylock-sdk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef PROBE_MODE
#define PROBE_MODE 0
#endif
static const shy_sdk_host *host;
static void mark(const char *event)
{
    const char *path=getenv("SHY_SDK_PROBE_LOG");
    FILE *f=path ? fopen(path,"a") : NULL;
    if (f) { fprintf(f,"%s\n",event); fclose(f); }
}
static int load(const shy_sdk_host *h,const char *config,void **state)
{
    (void)config; host=h; *state=NULL; mark("load");
    return PROBE_MODE==6 ? 0 : 1;
}
static int extract(void *state,const shy_sdk_capture *capture,shy_sdk_sink *sink)
{
    (void)state; mark("extract");
    shy_sdk_candidate c={sizeof c,"probe","1.00","EUR","nightly","unspecified",0,capture->length,1,100,NULL};
    int ok=host->candidate(sink,&c);
    if (PROBE_MODE==3) return 0; /* Discard valid partial output. */
    if (PROBE_MODE==4) { c.byte_length=capture->length+1; (void)host->candidate(sink,&c); return 1; }
    if (PROBE_MODE==5) { c.minor=101; (void)host->candidate(sink,&c); return 1; }
    return ok && host->diagnostic(sink,"probe_ran","copied diagnostic");
}
static void unload(void *state) { (void)state; mark("unload"); }
static const shy_sdk_plugin plugin={PROBE_MODE==1 ? 99 : SHY_SDK_ABI,
    PROBE_MODE==2 ? 1 : sizeof plugin,"sdk-probe","sdk-probe/v1",0,load,extract,unload};
SHY_SDK_EXPORT const shy_sdk_plugin *shylock_plugin_query(uint32_t abi)
{ return abi==SHY_SDK_ABI ? &plugin : NULL; }
