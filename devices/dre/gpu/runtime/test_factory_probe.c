/* SPDX-License-Identifier: MIT */
/* CF/IO ownership and malformed-data controls for the production observer.
 * The transport is deliberately synthetic; no Metal/hardware proof is made. */
#include "factory_probe.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fake_cf { unsigned long type; long length; const char *text; };
static struct fake_cf name_key = {1,15,"MetalPluginName"};
static struct fake_cf class_key = {1,20,"MetalPluginClassName"};
static struct fake_cf plugin = {1,14,"DreAdrenoMetal"};
static struct fake_cf cls = {1,20,"DreAdrenoMetalDevice"};
static unsigned services_count, next_index, io_releases, cf_releases;
static int allocation_failure, matching_failure, lookup_failure;
static int missing_name, missing_class, string_copy_failure;

static void reset(void)
{
    services_count=1;next_index=io_releases=cf_releases=0;
    allocation_failure=matching_failure=lookup_failure=0;
    missing_name=missing_class=string_copy_failure=0;
    plugin=(struct fake_cf){1,14,"DreAdrenoMetal"};
    cls=(struct fake_cf){1,20,"DreAdrenoMetalDevice"};
}
static void *matching(const char *name)
{ assert(!strcmp(name,"IOAcceleratorES")); return matching_failure?NULL:(void *)1; }
static int services(uint32_t port,const void *dictionary,uint32_t *out)
{ assert(!port && dictionary==(void *)1);*out=lookup_failure?0:42;return lookup_failure?-99:0; }
static uint32_t next(uint32_t iterator)
{ assert(iterator==42);return next_index<services_count?100+next_index++:0; }
static int release_io(uint32_t object)
{ assert(object==42 || (object>=100 && object<100+services_count));++io_releases;return 0; }
static const void *property(uint32_t object,const void *key,const void *allocator,uint32_t flags)
{
    assert(object>=100 && object<100+services_count && !allocator && !flags);
    assert(key==&name_key || key==&class_key);
    return key==&name_key?(missing_name?NULL:&plugin):(missing_class?NULL:&cls);
}
static const void *string_create(const void *allocator,const char *text,uint32_t encoding)
{
    assert(!allocator && encoding==0x08000100);
    if(allocation_failure)return NULL;
    if(!strcmp(text,"MetalPluginName"))return &name_key;
    assert(!strcmp(text,"MetalPluginClassName"));return &class_key;
}
static unsigned long type_id(const void *p) { return ((const struct fake_cf *)p)->type; }
static unsigned long string_type_id(void) { return 1; }
static long string_length(const void *p) { return ((const struct fake_cf *)p)->length; }
static unsigned char string_copy(const void *p,char *out,long bytes,uint32_t encoding)
{
    const struct fake_cf *v=p;
    assert(bytes==DRE_FACTORY_NAME_BYTES && encoding==0x08000100);
    if(string_copy_failure)return 0;
    assert(strlen(v->text)<(size_t)bytes);strcpy(out,v->text);return 1;
}
static void release_cf(const void *p)
{ assert(p==&name_key || p==&class_key || p==&plugin || p==&cls);++cf_releases; }
static const struct dre_factory_ops ops={matching,services,next,release_io,property,
    string_create,type_id,string_type_id,string_length,string_copy,release_cf};
static struct dre_factory_observation result;

int main(void)
{
    reset();assert(dre_factory_observe(&ops,&result)==DRE_FACTORY_METADATA);
    assert(result.count==1 && result.entries[0].name_state==DRE_FACTORY_STRING);
    assert(result.entries[0].class_state==DRE_FACTORY_STRING);
    assert(!strcmp(result.entries[0].plugin,"DreAdrenoMetal"));
    assert(io_releases==2 && cf_releases==4);
    reset();services_count=0;assert(dre_factory_observe(&ops,&result)==DRE_FACTORY_NONE);
    assert(!result.count && io_releases==1 && cf_releases==2);
    reset();missing_name=1;assert(dre_factory_observe(&ops,&result)==DRE_FACTORY_METADATA);
    assert(result.entries[0].name_state==DRE_FACTORY_MISSING && cf_releases==3);
    reset();missing_class=1;assert(dre_factory_observe(&ops,&result)==DRE_FACTORY_METADATA);
    assert(result.entries[0].class_state==DRE_FACTORY_MISSING && cf_releases==3);
    reset();plugin.type=7;dre_factory_observe(&ops,&result);
    assert(result.entries[0].name_state==DRE_FACTORY_WRONG_TYPE && !result.entries[0].plugin[0]);
    assert(io_releases==2 && cf_releases==4);
    reset();cls.type=7;dre_factory_observe(&ops,&result);
    assert(result.entries[0].class_state==DRE_FACTORY_WRONG_TYPE);
    reset();plugin.length=128;dre_factory_observe(&ops,&result);
    assert(result.entries[0].name_state==DRE_FACTORY_UNREPRESENTABLE && cf_releases==4);
    reset();plugin=(struct fake_cf){1,4,"A\"B"};dre_factory_observe(&ops,&result);
    assert(result.entries[0].name_state==DRE_FACTORY_UNREPRESENTABLE && !result.entries[0].plugin[0]);
    reset();plugin=(struct fake_cf){1,6,"A\0TAIL"};dre_factory_observe(&ops,&result);
    assert(result.entries[0].name_state==DRE_FACTORY_UNREPRESENTABLE);
    reset();string_copy_failure=1;dre_factory_observe(&ops,&result);
    assert(result.entries[0].name_state==DRE_FACTORY_UNREPRESENTABLE && cf_releases==4);
    reset();allocation_failure=1;assert(dre_factory_observe(&ops,&result)==DRE_FACTORY_ALLOCATION);
    assert(!io_releases && !cf_releases);
    reset();matching_failure=1;assert(dre_factory_observe(&ops,&result)==DRE_FACTORY_ALLOCATION);
    assert(!io_releases && cf_releases==2);
    reset();lookup_failure=1;assert(dre_factory_observe(&ops,&result)==DRE_FACTORY_LOOKUP);
    assert(result.io_error==-99 && cf_releases==2);
    reset();services_count=DRE_FACTORY_LIMIT+1;
    assert(dre_factory_observe(&ops,&result)==DRE_FACTORY_LIMIT_REACHED);
    assert(result.count==DRE_FACTORY_LIMIT && io_releases==DRE_FACTORY_LIMIT+2);
    assert(cf_releases==DRE_FACTORY_LIMIT*2+2);
    reset();struct dre_factory_ops bad=ops;bad.release_io=NULL;
    assert(dre_factory_observe(&bad,&result)==DRE_FACTORY_BAD_OPS && !io_releases && !cf_releases);
    assert(dre_factory_observe(&ops,NULL)==DRE_FACTORY_BAD_OPS);
    puts("PASS: 16 factory metadata, ownership, malformed-input and refusal cases; synthetic transport only");
    return 0;
}
