#!/usr/bin/env python3
"""Exercise production view ownership and writeback functions with host pixel memory.

Gallium allocation and texel I/O use host storage; view construction, dirty state,
writeback and release logic are extracted unchanged from the AIR sources.
"""
from pathlib import Path
import subprocess
import os
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SRC = Path(os.environ.get('AIR_VIEW_SOURCE', ROOT / 'tools/mesa/air'))
OUT = Path(os.environ.get('AIR_VIEW_TEST_OUT', tempfile.mkdtemp(prefix='air-view-writeback-')))
OUT.mkdir(parents=True, exist_ok=True)

def function(text, name):
    at = text.index('\n' + name + '(')
    start = text.rfind('\n', 0, at)
    if text[start:at].strip() in ('*',): raise AssertionError(name)
    brace = text.index('{', at)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start + 1:end] + '\n'

def structure(text, name):
    start = text.index('struct ' + name + ' {')
    return text[start:text.index('\n};', start) + 3] + '\n'

head = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
PRODUCTION_LOCK
#define PIPE_BIND_SAMPLER_VIEW 1
#define PIPE_BIND_RENDER_TARGET 4
#define PIPE_SWIZZLE_X 0
#define PIPE_SWIZZLE_Y 1
#define PIPE_SWIZZLE_Z 2
#define PIPE_SWIZZLE_W 3
#define PIPE_SWIZZLE_0 4
#define PIPE_SWIZZLE_1 5
#define RWA_TYPE_2D 2
#define u_minify(v,l) (((v) >> (l)) ? ((v) >> (l)) : 1)
enum pipe_format { PIPE_FORMAT_NONE, PIPE_FORMAT_RGBA8, PIPE_FORMAT_RGBA8_SRGB };
struct pipe_resource { unsigned height0, width0, last_level, bind, target, nr_samples, refs; enum pipe_format format; unsigned char bytes[256]; };
struct pipe_sampler_view { struct { struct { unsigned first_level, last_level; } tex; } u; unsigned swizzle_r, swizzle_g, swizzle_b, swizzle_a; int refs; };
struct pipe_context { struct pipe_sampler_view *(*create_sampler_view)(struct pipe_context *, struct pipe_resource *, const struct pipe_sampler_view *); };
struct rwa_texture_shape { unsigned pixel_format,type,width,height,depth,array_length,mip_levels,sample_count,usage; };
struct rwa_pixel_format { enum pipe_format pipe; const char *name; };
struct pipe_screen { bool (*is_format_supported)(struct pipe_screen *,enum pipe_format,unsigned,unsigned,unsigned,unsigned); };
struct rwa_device { struct pipe_screen *screen; struct pipe_context *pipe; struct rwa_buffer_texture *buffer_textures; unsigned written_buffer_textures, buffer_textures_count; };
struct rwa_buffer { struct rwa_device *device; void *map,*mirror; size_t size; bool mirror_hashed; };
'''
pre = r'''
static unsigned releases;
static bool supported(struct pipe_screen *s,enum pipe_format f,unsigned t,unsigned n,unsigned m,unsigned b) { (void)s;(void)f;(void)t;(void)n;(void)m;(void)b;return true; }
static void say(char *b,size_t n,const char *f,...) { va_list a;va_start(a,f);vsnprintf(b,n,f,a);va_end(a); }
static const struct rwa_pixel_format *pixel_format(unsigned f) { static const struct rwa_pixel_format formats[]={{PIPE_FORMAT_RGBA8,"rgba8"},{PIPE_FORMAT_RGBA8_SRGB,"rgba8_srgb"}};return f==70?formats:f==71?formats+1:NULL; }
static const char *pixel_format_refusal(unsigned f) { (void)f;return "unsupported"; }
static unsigned util_format_get_blocksize(enum pipe_format f) { (void)f;return 4; }
static unsigned util_format_get_blockwidth(enum pipe_format f) { (void)f;return 1; }
static unsigned util_format_get_blockheight(enum pipe_format f) { (void)f;return 1; }
static bool format_is_depth_stencil(enum pipe_format f) { (void)f;return false; }
static const char *rwa_device_name(struct rwa_device *d) { (void)d;return "host-memory"; }
static void pipe_resource_reference(struct pipe_resource **p,struct pipe_resource *q) { if(q)q->refs++;if(*p && !--(*p)->refs){free(*p);releases++;}*p=q; }
static void pipe_sampler_view_reference(struct pipe_sampler_view **p,struct pipe_sampler_view *q) { if(q)q->refs++;if(*p && !--(*p)->refs)free(*p);*p=q; }
static void pipe_sampler_view_release_ptr(struct pipe_sampler_view **p) { pipe_sampler_view_reference(p, NULL); }
static void u_sampler_view_default_template(struct pipe_sampler_view *t,const struct pipe_resource *r,enum pipe_format f) { (void)r;(void)f;memset(t,0,sizeof(*t));t->swizzle_r=PIPE_SWIZZLE_X;t->swizzle_g=PIPE_SWIZZLE_Y;t->swizzle_b=PIPE_SWIZZLE_Z;t->swizzle_a=PIPE_SWIZZLE_W; }
static struct pipe_sampler_view *make_sv(struct pipe_context *c,struct pipe_resource *r,const struct pipe_sampler_view *tmpl) { (void)c;(void)r;struct pipe_sampler_view *v=calloc(1,sizeof(*v));*v=*tmpl;v->refs=1;return v; }
static bool rgb2p_prepare_sample(struct rwa_texture *t,char *why,size_t n) { (void)t;(void)why;(void)n;return true; }
static void argbuf_scrub(struct rwa_device *d,void *b,struct rwa_texture *t,void *s) { (void)d;(void)b;(void)t;(void)s; }
static void state_texture_gone(struct rwa_device *d,struct pipe_resource *p) { (void)d;(void)p; }
static void buffer_mirror_pull(struct rwa_buffer *b) { (void)b; }
static struct { unsigned unchanged,uploads,refreshes; } upload_count;
void rwa_texture_destroy(struct rwa_texture *t);
'''
io = r'''
static int rwa_texture_read(struct rwa_texture *t,unsigned level,unsigned slice,const unsigned *origin,const unsigned *size,void *bytes,size_t row,char *why,size_t n) { (void)level;(void)slice;(void)origin;(void)size;(void)why;(void)n;for(unsigned y=0;y<t->shape.height;y++)memcpy((char *)bytes+y*row,t->res->bytes+y*t->shape.width*4,t->shape.width*4);return 0; }
static int rwa_texture_write(struct rwa_texture *t,unsigned level,unsigned slice,const unsigned *origin,const unsigned *size,const void *bytes,size_t row,char *why,size_t n) { (void)level;(void)slice;(void)origin;(void)size;(void)why;(void)n;for(unsigned y=0;y<t->shape.height;y++)memcpy(t->res->bytes+y*t->shape.width*4,(const char *)bytes+y*row,t->shape.width*4);texture_written(t);return 0; }
'''
main = r'''
static struct rwa_texture *root_texture(struct rwa_device *device,struct rwa_buffer *buffer) {
    struct rwa_texture *t=calloc(1,sizeof(*t));
    INIT_REFERENCE
    t->device=device;t->res=calloc(1,sizeof(*t->res));t->res->refs=1;t->res->width0=4;t->res->height0=4;t->res->bind=5;t->res->format=PIPE_FORMAT_RGBA8;
    t->shape=(struct rwa_texture_shape){.pixel_format=70,.type=2,.width=4,.height=4,.mip_levels=1,.sample_count=1,.usage=7};t->format_name="rgba8";t->shader_read=true;t->shader_write=true;t->layers=1;
    struct rwa_buffer_texture *b=calloc(1,sizeof(*b));b->texture=t;b->source=buffer;b->row_bytes=24;b->next=device->buffer_textures;device->buffer_textures=b;t->backing=b;return t;
}
int main(int argc,char **argv) {
    assert(argc==2);char why[256]={0};struct pipe_screen screen={supported};struct pipe_context pctx={make_sv};struct rwa_device d={.screen=&screen,.pipe=&pctx};
    unsigned char bytes[96];memset(bytes,0x33,sizeof(bytes));struct rwa_buffer buffer={.device=&d,.map=bytes,.size=sizeof(bytes)};
    struct rwa_texture *root=root_texture(&d,&buffer);
    if(!strcmp(argv[1],"swizzle_propagation")) {
        buffer_textures_forget(&buffer);
        rwa_texture_set_swizzle(root, 2, 2, 2, 3);
        assert(texture_ensure_sampler_view(&d, &pctx, root, 0, why, sizeof(why)));
        assert(root->view && root->view->swizzle_r==PIPE_SWIZZLE_X && root->view->swizzle_g==PIPE_SWIZZLE_X &&
               root->view->swizzle_b==PIPE_SWIZZLE_X && root->view->swizzle_a==PIPE_SWIZZLE_Y);
        struct rwa_texture *inh = rwa_texture_view(root, 71, 0, 1, why, sizeof(why));assert(inh);
        assert(texture_ensure_sampler_view(&d, &pctx, inh, 0, why, sizeof(why)));
        assert(inh->view && inh->view->swizzle_a==PIPE_SWIZZLE_Y);
        rwa_texture_set_swizzle(inh, 0, 0, 0, 0);
        assert(!inh->view && texture_ensure_sampler_view(&d, &pctx, inh, 0, why, sizeof(why)));
        assert(inh->view->swizzle_r==PIPE_SWIZZLE_0 && inh->view->swizzle_a==PIPE_SWIZZLE_0);
        rwa_texture_destroy(inh);rwa_texture_destroy(root);assert(releases==1);
        puts("case=swizzle_propagation ok=1");return 0;
    }
    if(!strcmp(argv[1],"mipmap_lifetime")) {
        buffer_textures_forget(&buffer);root->res->last_level=2;root->shape.mip_levels=3;
        struct rwa_texture *first=rwa_texture_view(root,71,1,2,why,sizeof(why));assert(first);
        struct rwa_texture *last=rwa_texture_view(first,70,1,1,why,sizeof(why));assert(last);
        assert(first->first_level==1 && first->shape.width==2 && last->first_level==2 && last->shape.width==1);
        rwa_texture_destroy(root);rwa_texture_destroy(first);texture_written(last);
        rwa_texture_destroy(last);assert(releases==1 && !d.buffer_textures);
        puts("case=mipmap_lifetime resource_releases=1");return 0;
    }
    struct rwa_texture *v=rwa_texture_view(root,71,0,1,why,sizeof(why)),*v2=rwa_texture_view(v,70,0,1,why,sizeof(why));assert(v && v2);
    struct rwa_texture *sibling=rwa_texture_view(root,70,0,1,why,sizeof(why));assert(sibling);
    unsigned char pixels[64];for(unsigned i=0;i<64;i++)pixels[i]=(unsigned char)(i*3+1);
    if(!strcmp(argv[1],"owner_release")){rwa_texture_destroy(root);root=NULL;}
    rwa_texture_destroy(v);v=NULL;
    if(!strcmp(argv[1],"buffer_release")){buffer_textures_forget(&buffer);assert(!d.buffer_textures);}
    assert(rwa_texture_write(v2,0,0,NULL,NULL,pixels,16,why,sizeof(why))==0);
    assert(!memcmp(sibling->res->bytes,pixels,64));
    rwa_buffer_textures_writeback(&d);
    unsigned errors=0;
    for(unsigned y=0;y<4;y++)for(unsigned x=0;x<24;x++){
       unsigned char want=(!strcmp(argv[1],"buffer_release")||x>=16)?0x33:pixels[y*16+x];
       errors+=bytes[y*24+x]!=want;
    }
    if(!strcmp(argv[1],"sibling_after_release")){rwa_texture_destroy(v2);v2=NULL;memset(pixels,0x77,sizeof(pixels));assert(rwa_texture_write(sibling,0,0,NULL,NULL,pixels,16,why,sizeof(why))==0);rwa_buffer_textures_writeback(&d);for(unsigned y=0;y<4;y++)for(unsigned x=0;x<16;x++)errors+=bytes[y*24+x]!=0x77;}
    rwa_texture_destroy(v2);rwa_texture_destroy(sibling);rwa_texture_destroy(root);
    assert(!d.buffer_textures && releases==1);
    printf("case=%s pixel_errors=%u resource_releases=%u\n",argv[1],errors,releases);return errors?1:0;
}
'''

def build(label, texts):
    alltext='\n'.join(texts)
    structs=structure(alltext,'rwa_texture')+structure(alltext,'rwa_buffer_texture')
    body='\n'.join(function(alltext,n) for n in ['buffer_texture_clear_written','rgb2p_format','texture_format','texture_levels','texture_written'])
    later='\n'.join(function(alltext,n) for n in ['buffer_texture_bytes','rows_hash','buffer_texture_download','rwa_texture_view','rwa_texture_set_swizzle','buffer_textures_forget','rwa_buffer_textures_writeback','texture_ensure_sampler_view','rwa_texture_destroy'])
    init='t->references = 1;' if 'unsigned references;' in structs else ''
    lock_start = alltext.index('static pthread_mutex_t rwa_lock =')
    lock_end = alltext.index('#include \"nir.h\"', lock_start)
    production_lock = alltext[lock_start:lock_end]
    path=OUT/(label+'.c');path.write_text(head.replace('PRODUCTION_LOCK', production_lock)+structs+pre+body+io+later+main.replace('INIT_REFERENCE',init))
    exe=OUT/label
    subprocess.run(['xcrun','clang','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-variable','-O1','-g','-fsanitize=address,undefined',str(path),'-o',str(exe)],check=True)
    results=[]
    for case in ['view','owner_release','buffer_release','sibling_after_release','mipmap_lifetime','swizzle_propagation']:
        r=subprocess.run([str(exe),case],capture_output=True,text=True,timeout=10)
        print(label,r.returncode,r.stdout.strip());results.append(r.returncode)
        (OUT/(label+'-'+case+'.log')).write_text(r.stdout+r.stderr)
    return results

names=['air_metal.c','air_metal_allocation.inc','air_metal_texture.inc','air_metal_resource_private.h','air_metal_rgb2p_private.h']
texts=[(SRC/n).read_text() for n in names]
results = build('production', texts)
assert all(x == 0 for x in results)
mutations = {
    'lost_writeback_owner': ('   if (t->storage_owner)\n      t = t->storage_owner;\n', ''),
    'missing_owner_retain': ('   view->storage_owner->references++;', ''),
    'missing_owner_release': ('   rwa_texture_destroy(owner);', '   (void)owner;'),
    'nested_owner_not_original': ('texture->storage_owner ? texture->storage_owner : texture', 'texture'),
    'lost_zero_swizzle_flag': ('   if (t->swizzled) {', '   if (t->swizzle_r || t->swizzle_g || t->swizzle_b || t->swizzle_a) {'),
    'view_drops_swizzled': ('   view->swizzled = texture->swizzled;', ''),
}
for label, (before, after) in mutations.items():
    assert sum(text.count(before) for text in texts) == 1
    changed = [text.replace(before, after) for text in texts]
    assert any(code != 0 for code in build(label, changed)), label
print('PASS: production scenarios and six rejected regression variants')
