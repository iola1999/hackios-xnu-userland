/* Exercise the production compute lifecycle with instrumented Gallium callbacks.
 * Real software execution is validated separately with the same built library. */
#include "air_metal.c"
static unsigned created, deleted, reported = 768;
static bool fixed;
static void *test_create(struct pipe_context *pipe, const struct pipe_compute_state *state)
{
   (void)pipe;
   created++;
   fixed = !((nir_shader *)state->prog)->info.workgroup_size_variable;
   return (void *)state->prog;
}
static void test_delete(struct pipe_context *pipe, void *state)
{
   (void)pipe; deleted++; ralloc_free(state);
}
static void test_bind(struct pipe_context *pipe, void *state) { (void)pipe; (void)state; }
static void test_info(struct pipe_context *pipe, void *state, struct pipe_compute_state_object_info *info)
{
   (void)pipe; (void)state;
   info->max_threads=reported;info->preferred_simd_size=128;
}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"contract:%d %s: %s\n",__LINE__,#x,why);return 1;}}while(0)
int main(int argc,char **argv)
{
   char why[512]={0};CHECK(argc==3);
   FILE *f=fopen(argv[1],"rb");CHECK(f);fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);CHECK(size>0);void *bc=malloc(size);CHECK(bc&&fread(bc,1,size,f)==(size_t)size);fclose(f);
   setenv("RWA_SOFTWARE","1",1);
   struct rwa_device *d=rwa_device_open(why,sizeof(why));CHECK(d);
   struct pipe_context *pipe=d->pipe;
   __typeof__(pipe->create_compute_state) create=pipe->create_compute_state;
   __typeof__(pipe->delete_compute_state) del=pipe->delete_compute_state;
   __typeof__(pipe->bind_compute_state) bind=pipe->bind_compute_state;
   __typeof__(pipe->get_compute_state_info) info=pipe->get_compute_state_info;
   pipe->create_compute_state=test_create;pipe->delete_compute_state=test_delete;pipe->bind_compute_state=test_bind;pipe->get_compute_state_info=test_info;
   d->software=false;d->max_threads_per_group=1024;
   struct rwa_compute_info caps={0};
   struct rwa_compute *c=rwa_compute_create(d,bc,size,argv[2],&caps,why,sizeof(why));CHECK(c);
   CHECK(created==1 && !fixed && caps.max_threads_per_group==768 && caps.thread_execution_width==128);
   unsigned a[3]={32,24,1},b[3]={16,16,1},over[3]={1024,1,1},grid[3]={1,1,1};
   void *cso=c->cso;CHECK(compute_state_for(c,a,why,sizeof(why))==cso);CHECK(compute_state_for(c,b,why,sizeof(why))==cso);CHECK(created==1);
   CHECK(rwa_dispatch(c,NULL,grid,over,why,sizeof(why))!=0 && strstr(why,"at most 768"));CHECK(created==1);
   rwa_compute_destroy(c);CHECK(deleted==1);
   reported=0;c=rwa_compute_create(d,bc,size,argv[2],&caps,why,sizeof(why));CHECK(!c && created==2 && deleted==2);
   reported=2048;c=rwa_compute_create(d,bc,size,argv[2],&caps,why,sizeof(why));CHECK(c&&caps.max_threads_per_group==1024);rwa_compute_destroy(c);CHECK(deleted==3);
   pipe->create_compute_state=create;pipe->delete_compute_state=del;pipe->bind_compute_state=bind;pipe->get_compute_state_info=info;d->software=true;rwa_device_close(d);free(bc);
   puts("PASS production create/info/cache/dispatch guard/destroy: 768 cap, variant reuse, zero cap refusal, device min; pipe callbacks instrumented");return 0;
}
