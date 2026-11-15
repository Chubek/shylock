#include "shylock.h"
#include <string.h>

enum { SHY_MAX_PLUGINS = 16 };
static const shy_plugin *registry[SHY_MAX_PLUGINS];
static size_t count;

const shy_plugin *shy_plugin_find(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(registry[i]->name, name)) return registry[i];
    return NULL;
}

const shy_plugin *shy_plugin_at(size_t index)
{
    return index < count ? registry[index] : NULL;
}

int shy_plugin_register(const shy_plugin *plugin)
{
    if (!plugin || plugin->abi != SHY_EXTRACTOR_ABI || plugin->size < sizeof *plugin ||
        !plugin->name || !*plugin->name || strlen(plugin->name) > 64 ||
        strspn(plugin->name, "abcdefghijklmnopqrstuvwxyz0123456789_-") != strlen(plugin->name) ||
        !plugin->version || !*plugin->version || strlen(plugin->version) > 64 ||
        !plugin->extract || count == SHY_MAX_PLUGINS || shy_plugin_find(plugin->name)) return 0;
    registry[count++] = plugin;
    return 1;
}
