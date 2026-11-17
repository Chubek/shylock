#include "naive.h"
#include "naive-rules.h"
#include "mulib-readability.h"
#include "mulib-utf8.h"
#include <string.h>

/* A text unit is a UTF-8 scalar or one HTML entity. Entity decoding must be
   single-pass: &amp;#8364; is the literal &#8364;, not a euro sign. MuLIB
   readability supplies its named subset; this adapter adds bounded numeric
   references and the monetary named references absent from that subset. */
static int entity(const char *raw,size_t length,unsigned char decoded[64],size_t *used,size_t *size)
{
    if (raw[0]!='&') return 0;
    size_t end=1;
    while (end<length && end<32 && raw[end]!=';' && raw[end]!='&' && raw[end]!='<' && raw[end]!=' ') ++end;
    if (end==length || end==32 || raw[end]!=';') return 0;
    *used=end+1;
    uint32_t cp=0;
    if (raw[1]=='#') {
        size_t i=2; unsigned base=10;
        if (i<end && (raw[i]=='x' || raw[i]=='X')) { base=16; ++i; }
        if (i==end) return 0;
        for (;i<end;++i) {
            unsigned digit;
            if (raw[i]>='0' && raw[i]<='9') digit=(unsigned)(raw[i]-'0');
            else if (raw[i]>='a' && raw[i]<='f') digit=(unsigned)(raw[i]-'a'+10);
            else if (raw[i]>='A' && raw[i]<='F') digit=(unsigned)(raw[i]-'A'+10);
            else return 0;
            if (digit>=base || cp>(UINT32_C(0x10ffff)-digit)/base) return 0;
            cp=cp*base+digit;
        }
        if (!cp || (cp>=0xd800 && cp<=0xdfff)) cp=0xfffd;
        *size=mulib_utf8_encode(cp,decoded,64);
        return *size!=0;
    }
    if (end==5 && !memcmp(raw,"&euro;",6)) cp=0x20ac;
    if (end==6 && !memcmp(raw,"&pound;",7)) cp=0xa3;
    if (cp) { *size=mulib_utf8_encode(cp,decoded,64); return *size!=0; }
    char input[36]; input[0]='x'; memcpy(input+1,raw,*used); input[*used+1]='x';
    mulib_shylock_buffer_t out={0}; out.limit=64;
    if (!mulib_readability_extract(input,*used+2,&out)) { mulib_shylock_buffer_free(&out); nf_fail(); return -1; }
    int changed=out.len>=2 && (out.len-2!=*used || memcmp(out.data+1,raw,*used));
    if (changed) { *size=out.len-2; memcpy(decoded,out.data+1,*size); }
    mulib_shylock_buffer_free(&out); return changed;
}
static int space(uint32_t c)
{
    return c==9 || c==10 || c==12 || c==13 || c==32 || c==0xa0 || c==0x2007 || c==0x2009 || c==0x202f;
}
static int mapped(const unsigned char *bytes,size_t n,size_t offset,size_t consumed,int exact)
{
    uint32_t cp; size_t count;
    int whitespace=mulib_utf8_decode(bytes,n,&cp,&count) && count==n && space(cp);
    if (whitespace) {
        size_t i=shy_nf.text.len;
        if (i && shy_nf.text.data[i-1]==' ') {
            shy_nf.ends[i-1]=offset+consumed;
            shy_nf.exact[i-1]=0; return 1;
        }
        if (!mulib_shylock_append(&shy_nf.text," ",1)) return nf_fail();
        shy_nf.starts[i]=offset; shy_nf.ends[i]=offset+consumed;
        shy_nf.exact[i]=(unsigned char)(exact && n==1 && bytes[0]==' ');
        return 1;
    }
    size_t at=shy_nf.text.len;
    if (!mulib_shylock_append(&shy_nf.text,bytes,n)) return nf_fail();
    for (size_t i=0;i<n;++i) {
        shy_nf.starts[at+i]=offset+(exact ? i : 0);
        shy_nf.ends[at+i]=offset+(exact ? i+1 : consumed);
        shy_nf.exact[at+i]=(unsigned char)exact;
    }
    return 1;
}
static int append(const char *raw,size_t length,size_t offset)
{
    if (length>NF_TEXT*2) return nf_fail();
    for (size_t i=0;i<length;) {
        unsigned char decoded[64]; size_t consumed=0,n=0;
        int decoded_entity=entity(raw+i,length-i,decoded,&consumed,&n);
        if (decoded_entity<0) return 0;
        if (decoded_entity) {
            if (!mapped(decoded,n,offset+i,consumed,0)) return 0;
        } else {
            uint32_t cp;
            if (!mulib_utf8_decode((const unsigned char *)raw+i,length-i,&cp,&consumed) ||
                !mapped((const unsigned char *)raw+i,consumed,offset+i,consumed,1)) return nf_fail();
        }
        i+=consumed;
    }
    return 1;
}
static int boundary(size_t offset)
{
    if (!shy_nf.text.len || shy_nf.text.data[shy_nf.text.len-1]=='\n') return 1;
    size_t i=shy_nf.text.len;
    if (!mulib_shylock_append(&shy_nf.text,"\n",1)) return nf_fail();
    shy_nf.starts[i]=offset; shy_nf.ends[i]=offset; shy_nf.exact[i]=0; return 1;
}
static int block(const char *tag)
{
    static const char *const tags[]={"br","div","p","li","section","article","h1","h2","h3","td","tr"};
    for (size_t i=0;i<sizeof tags/sizeof *tags;++i) if (!strcmp(tag,tags[i])) return 1;
    return 0;
}
int nf_text(size_t index,const char *attribute)
{
    shy_nf.text.len=0; if (shy_nf.text.data) shy_nf.text.data[0]=0;
    shy_nf.hits_count=0;
    const mulib_html_tree_node_t *root=shy_nf.nodes[index].node;
    if (*attribute) {
        const char *value; size_t length;
        if (!mulib_html_attribute(root->attributes.data,root->attributes.len,attribute,&value,&length)) return 1;
        return append(value,length,root->offset+(size_t)(value-root->attributes.data));
    }
    for (size_t i=index;i<shy_nf.nodes[index].end;++i) {
        if (++shy_nf.query_work>NF_QUERY_WORK) return nf_fail();
        nf_node item=shy_nf.nodes[i];
        if (item.hidden) { i=item.end-1; continue; }
        const mulib_html_tree_node_t *n=item.node;
        if (n->is_text) { if (!append(n->text.data,n->text.len,n->offset)) return 0; }
        else if (block(n->tag) && !boundary(n->offset)) return 0;
        /* A block's end can precede an inline sibling. Insert a synthetic
           separator only on leaving that subtree, never between inline nodes. */
        size_t next=i+1;
        for (size_t p=i;p!=index && shy_nf.nodes[p].end==next;p=shy_nf.nodes[p].parent) {
            const mulib_html_tree_node_t *ended=shy_nf.nodes[p].node;
            if (!ended->is_text && block(ended->tag) && !boundary(ended->offset)) return 0;
        }
    }
    return 1;
}
