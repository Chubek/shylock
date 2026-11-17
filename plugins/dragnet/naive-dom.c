#include "naive.h"
#include "naive-rules.h"
#include "mulib-dom-query.h"
#include "mulib-scrape-rule.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int hidden(const mulib_html_tree_node_t *n)
{
    static const char *const tags[]={"head","script","style","noscript","template","nav","header","footer","aside","svg"};
    for (size_t i=0;i<sizeof tags/sizeof *tags;++i) if (!strcmp(n->tag,tags[i])) return 1;
    const char *value; size_t length;
    return mulib_html_attribute(n->attributes.data,n->attributes.len,"hidden",&value,&length) ||
        (mulib_html_attribute(n->attributes.data,n->attributes.len,"aria-hidden",&value,&length) && length==4 && !strncasecmp(value,"true",4));
}
static int index_tree(const mulib_html_tree_node_t *n,size_t parent,int excluded)
{
    for (;n;n=n->next_sibling) {
        if (shy_nf.count==NF_NODES) return nf_fail();
        size_t i=shy_nf.count++;
        shy_nf.nodes[i]=(nf_node){n,0,parent,excluded || hidden(n)};
        if (!index_tree(n->first_child,i,shy_nf.nodes[i].hidden)) return 0;
        shy_nf.nodes[i].end=shy_nf.count;
    }
    return 1;
}
int nf_dom_open(void)
{
    const char *html=shy_text(shy_nf.snapshot,"html");
    shy_nf.nodes=calloc(NF_NODES,sizeof *shy_nf.nodes);
    shy_nf.starts=calloc(NF_TEXT+1,sizeof *shy_nf.starts);
    shy_nf.ends=calloc(NF_TEXT+1,sizeof *shy_nf.ends);
    shy_nf.exact=calloc(NF_TEXT+1,1);
    shy_nf.text.limit=NF_TEXT;
    return (shy_nf.nodes && shy_nf.starts && shy_nf.ends && shy_nf.exact &&
        mulib_html_tree_parse(html,strlen(html),NF_NODES-1,NF_DEPTH,&shy_nf.tree) &&
        index_tree(shy_nf.tree,0,0)) || nf_fail();
}
size_t nf_index(int64_t handle)
{
    if (!shy_nf.tree || handle<0) { nf_fail(); return SIZE_MAX; }
    if (!handle) return 0;
    uint64_t h=(uint64_t)handle;
    size_t i=(size_t)(h%65536);
    if (h/65536!=shy_nf.generation || i>=shy_nf.count) { nf_fail(); return SIZE_MAX; }
    return i;
}
int64_t nf_handle(size_t i) { return (int64_t)(shy_nf.generation*65536+i); }
int64_t shy_naive_offset(int64_t handle)
{
    size_t i=nf_index(handle);
    return i==SIZE_MAX ? -1 : (int64_t)shy_nf.nodes[i].node->offset;
}
int64_t shy_naive_query(int64_t scope,const char *selector,int64_t after)
{
    size_t root=nf_index(scope),start=after ? nf_index(after) : root;
    mulib_scrape_rule_t rule={0};
    if (root==SIZE_MAX || start==SIZE_MAX || start<root || start>=shy_nf.nodes[root].end ||
        !mulib_scrape_rule_init(&rule,selector,NULL,1)) { nf_fail(); return -1; }
    for (size_t i=after ? start+1 : root;i<shy_nf.nodes[root].end;++i) {
        if (++shy_nf.query_work>NF_QUERY_WORK) { nf_fail(); break; }
        if (!shy_nf.nodes[i].hidden && mulib_dom_query_matches(shy_nf.nodes[i].node,rule.selector)) {
            mulib_scrape_rule_free(&rule); return nf_handle(i);
        }
    }
    mulib_scrape_rule_free(&rule); return shy_nf.failed ? -1 : 0;
}
int64_t shy_naive_owner(int64_t offset,int64_t scope)
{
    size_t root=nf_index(scope);
    if (root==SIZE_MAX || offset<0) { nf_fail(); return -1; }
    /* Exact source-token containment identifies a tree node, then ancestry is
       tested. No distance/nearest-price heuristic or fabricated closing span. */
    for (size_t i=root;i<shy_nf.nodes[root].end;++i) {
        if (++shy_nf.query_work>NF_QUERY_WORK) { nf_fail(); return -1; }
        const mulib_html_tree_node_t *n=shy_nf.nodes[i].node;
        size_t length=n->is_text ? n->text.len : n->attributes.len;
        if (length && (uint64_t)offset>=n->offset && (uint64_t)offset-n->offset<length) return nf_handle(i);
    }
    return 0;
}
int64_t shy_naive_visible(int64_t handle)
{
    size_t i=nf_index(handle);
    return i!=SIZE_MAX && !shy_nf.nodes[i].hidden;
}
int64_t shy_naive_structured_visible(int64_t handle)
{
    size_t i=nf_index(handle);
    if (i==SIZE_MAX) return 0;
    for (const mulib_html_tree_node_t *n=shy_nf.nodes[i].node;n;n=n->parent)
        if (strcmp(n->tag,"script") && hidden(n)) return 0;
    return 1;
}
const char *shy_naive_text(int64_t handle,const char *attribute)
{
    size_t i=nf_index(handle);
    if (i==SIZE_MAX || !nf_text(i,attribute)) { nf_fail(); return ""; }
    return shy_nf.text.data ? shy_nf.text.data : "";
}
int64_t shy_naive_scan(int64_t handle,const char *attribute,const char *pattern)
{
    size_t i=nf_index(handle);
    if (i==SIZE_MAX || !nf_text(i,attribute) || !nf_scan(pattern)) { nf_fail(); return -1; }
    return (int64_t)shy_nf.hits_count;
}
