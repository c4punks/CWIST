/* Real loopback TCP against production classic/C1M senders. No mocks. */
#include <cwist/sys/app/app.h>
#include "../src/sys/app/public_fixed_cache.h"
#include <cwist/sys/app/shutdown.h>
#include <cwist/core/mem/gc.h>
#include <cwist/core/mem/alloc.h>
#include <cwist/sys/io/reactor.h>
#include <cwist/net/http/async.h>
#include <cwist/net/http/session.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s (errno=%d)\n",__FILE__,__LINE__,#x,errno); exit(1); } } while(0)
extern void cwist_app_http_handler(int,void *);
extern cwist_async_action_t cwist_app_http_handler_async(int,cwist_http_async_conn_t *);
static atomic_int calls, releases, middleware_calls;
static unsigned char payload[65536];
static size_t payload_len=2048; /* Coalesces; 64 KiB boundary tested separately. */
static atomic_int parks, hits, reconstruction_failures, body_calls, chunked_calls;
static atomic_bool fail_reconstruction;
static int shutdown_after;
static bool require_park, complete_deferred, expect_post, inline_abort_case;
static const char *raw_requests;
static atomic_int file_calls, deferred_calls, post_calls;
static int last_file_fd=-1;
static _Atomic(cwist_async *) pending_deferred;
void cwist_pfc_test_parked(void){atomic_fetch_add(&parks,1);}
void cwist_pfc_test_hit(void){int n=atomic_fetch_add(&hits,1)+1;if(shutdown_after&&n==shutdown_after)atomic_store(&g_cwist_running,false);}
bool cwist_pfc_test_reconstruction_failed(void){bool b=atomic_exchange(&fail_reconstruction,false);if(b)atomic_fetch_add(&reconstruction_failures,1);return b;}
static bool c1m;
static uint64_t cache_time=100;
static bool cache_oom;
static uint64_t test_clock(void){return cache_time;}
static void *test_alloc(size_t n){return cache_oom?NULL:malloc(n);}
static _Atomic(cwist_reactor_t *) reactor;
static void cleanup(const void *p,size_t n,void *ctx){(void)n;(void)ctx;cwist_free((void*)p);atomic_fetch_add(&releases,1);}
static void fixed(cwist_http_request *q,cwist_http_response *r){
    atomic_fetch_add(&calls,1);
    if(q&&q->body&&q->body->size){CHECK(q->body->size==3&&!memcmp(q->body->data,"abc",3));atomic_fetch_add(&body_calls,1);if(q->te_chunked_seen)atomic_fetch_add(&chunked_calls,1);}
    void *p=cwist_alloc(payload_len);CHECK(p);memcpy(p,payload,payload_len);
    cwist_http_response_set_body_ptr_managed(r,p,payload_len,cleanup,NULL);
    cwist_http_header_add(&r->headers,"Content-Type","application/octet-stream");
}
static void private_response(cwist_http_request *q,cwist_http_response *r){fixed(q,r);cwist_http_header_add(&r->headers,"Set-Cookie","private=1");}
static void private_context(cwist_http_request *q,cwist_http_response *r){static int context;fixed(q,r);q->private_data=&context;}
static void error_response(cwist_http_request *q,cwist_http_response *r){fixed(q,r);r->status_code=500;}
static void after_handler(cwist_http_request *q,cwist_http_response *r,cwist_handler_func next){atomic_fetch_add(&middleware_calls,1);next(q,r);atomic_fetch_add(&post_calls,1);cwist_http_header_add(&r->headers,"X-Post","ran");}
static void deferred_response(cwist_http_request *q,cwist_http_response *r){
    atomic_fetch_add(&deferred_calls,1);
    cwist_error_t e=cwist_http_header_add(&r->headers,"X-Before-Defer","v");CHECK(cwist_error_is_ok(&e));
    e=cwist_http_header_add(&q->headers,"X-Owned-Request","v");CHECK(cwist_error_is_ok(&e));
    if(!q->query_params){q->query_params=cwist_query_map_create();}CHECK(q->query_params);
    cwist_query_map_set(q->query_params,"owned","query");
    if(!q->path_params){q->path_params=cwist_query_map_create();}CHECK(q->path_params);
    cwist_query_map_set(q->path_params,"owned","path");
    if(!q->flash){q->flash=cwist_query_map_create();}CHECK(q->flash);
    cwist_query_map_set(q->flash,"owned","flash");
    cwist_session_t *session=cwist_session_start(q->app,q,r);CHECK(session);
    CHECK(cwist_session_set(session,"owned","session")==0);
    CHECK(!q->csrf_token);q->csrf_token=cwist_alloc(4);CHECK(q->csrf_token);memcpy(q->csrf_token,"abc",4);
    /* Exercise real heap-backed sstrings, including adopted cwist_alloc data. */
    cwist_sstring_destroy(q->body);q->body=cwist_sstring_create();CHECK(q->body);
    char *body=cwist_alloc(4);CHECK(body);memcpy(body,"abc",4);
    cwist_sstring_adopt_len(q->body,body,3);
    cwist_sstring_destroy(r->body);r->body=cwist_sstring_create();CHECK(r->body);
    cwist_async *a=cwist_async_defer(q,r);CHECK(a);atomic_store(&pending_deferred,a);
}
static void inline_abort_response(cwist_http_request *q,cwist_http_response *r){cwist_async *a=cwist_async_defer(q,r);CHECK(a);CHECK(cwist_async_abort(a,CWIST_HTTP_INTERNAL_ERROR));}
static void file_response(cwist_http_request *q,cwist_http_response *r){
    (void)q;atomic_fetch_add(&file_calls,1);
    char path[]="/tmp/cwist-pfc-XXXXXX";int fd=mkstemp(path);CHECK(fd>=0);CHECK(!unlink(path));
    size_t off=0;while(off<payload_len){ssize_t n=write(fd,payload+off,payload_len-off);CHECK(n>0);off+=(size_t)n;}
    CHECK(lseek(fd,0,SEEK_SET)==0);last_file_fd=fd;
    r->use_file_stream=true;r->file_stream_fd=fd;r->file_stream_len=payload_len;r->file_stream_auto_close=true;
}
static void deny(cwist_http_request *q,cwist_http_response *r,cwist_handler_func next){
    (void)q;(void)next;atomic_fetch_add(&middleware_calls,1);r->status_code=401;
    cwist_sstring_assign(r->body,"DENIED");
}
static cwist_async_action_t dispatch(int fd,cwist_http_async_conn_t *conn){
    atomic_store(&reactor,conn->reactor);return cwist_app_http_handler_async(fd,conn);
}
static void wake(void *ctx){(void)ctx;}
struct completion_gate{atomic_bool entered,release;};
static void hold_reactor(void *ctx){
    struct completion_gate *g=ctx;atomic_store(&g->entered,true);
    struct timespec pause={.tv_nsec=1000000};
    for(int i=0;i<5000&&!atomic_load(&g->release);i++)nanosleep(&pause,NULL);
    CHECK(atomic_load(&g->release));
}
static void *produce_response(void *ctx){
    size_t pending=cwist_gc_scope_pending_count();
    cwist_http_response *r=cwist_http_response_create();CHECK(r);fixed(NULL,r);
    cwist_error_t e=cwist_http_header_add(&r->headers,"X-Producer","owned");CHECK(cwist_error_is_ok(&e));
    CHECK(cwist_async_respond_with(ctx,r));
    /* The reactor has not consumed it: producer TLS must no longer own it. */
    if(cwist_full_gc_enabled())CHECK(cwist_gc_scope_pending_count()==pending);
    return NULL;
}
struct job{int fd;cwist_app *app;};
static void *classic(void *p){struct job *j=p;cwist_app_http_handler(j->fd,j->app);return NULL;}
static void write_all(int fd,const char *p,size_t n){while(n){ssize_t k=send(fd,p,n,0);if(k<0&&errno==EINTR)continue;CHECK(k>0);p+=k;n-=(size_t)k;}}
/* Each exchange joins its workers: cache entries must survive worker-GC teardown,
 * and any following reconfiguration is genuinely quiescent, not a route race. */
static void exchange(cwist_app *app,const char *path,const char *host,const char *extra,int count,int status){
    struct completion_gate gate={0};
    cwist_reactor_post_t gate_post={.cb=hold_reactor,.ctx=&gate};
    atomic_store(&g_cwist_running,true);atomic_store(&reactor,NULL);
    atomic_store(&parks,0);atomic_store(&hits,0);
    int listener=socket(AF_INET,SOCK_STREAM,0);CHECK(listener>=0);
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    CHECK(bind(listener,(struct sockaddr*)&addr,sizeof(addr))==0);CHECK(listen(listener,4)==0);
    socklen_t alen=sizeof(addr);CHECK(getsockname(listener,(struct sockaddr*)&addr,&alen)==0);
    int client=socket(AF_INET,SOCK_STREAM,0);CHECK(client>=0);
    CHECK(connect(client,(struct sockaddr*)&addr,sizeof(addr))==0);
    int server=accept(listener,NULL,NULL);CHECK(server>=0);close(listener);
    int small=4096;CHECK(setsockopt(server,SOL_SOCKET,SO_SNDBUF,&small,sizeof(small))==0);
    CHECK(setsockopt(client,SOL_SOCKET,SO_RCVBUF,&small,sizeof(small))==0);
    struct timeval timeout={.tv_sec=15};CHECK(setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))==0);
    char requests[16000];size_t used=0;
    for(int i=0;i<count;i++){
        int n=snprintf(requests+used,sizeof(requests)-used,"GET %s HTTP/1.1\r\nHost: %s\r\n%sConnection: %s\r\n\r\n",path,host,extra,i==count-1&&!inline_abort_case?"close":"keep-alive");
        CHECK(n>0&&(size_t)n<sizeof(requests)-used);used+=(size_t)n;
    }
    if(raw_requests)write_all(client,raw_requests,strlen(raw_requests));else write_all(client,requests,used);
    struct job job={server,app};pthread_t thread;bool joined=false;
    if(c1m){CHECK(cwist_http_pool_init()==0);CHECK(cwist_http_pool_submit_async(server,dispatch,app));}
    else CHECK(pthread_create(&thread,NULL,classic,&job)==0);
    struct timespec pause={.tv_nsec=1000000};
    if(complete_deferred){
        cwist_async *a=NULL;for(int i=0;i<2000&&!a;i++){a=atomic_exchange(&pending_deferred,NULL);if(!a)nanosleep(&pause,NULL);}CHECK(a);
        /* Completion must survive creator-thread TLS/GC destruction. */
        if(!c1m){CHECK(pthread_join(thread,NULL)==0);joined=true;}
        if(c1m){
            cwist_reactor_t *owner=atomic_load(&reactor);CHECK(owner);
            CHECK(cwist_reactor_post(owner,&gate_post));
            for(int i=0;i<2000&&!atomic_load(&gate.entered);i++)nanosleep(&pause,NULL);
            CHECK(atomic_load(&gate.entered));
            pthread_t producer;CHECK(pthread_create(&producer,NULL,produce_response,a)==0);
            CHECK(pthread_join(producer,NULL)==0);
            atomic_store(&gate.release,true);
        }else{
            cwist_http_response *r=cwist_http_response_create();CHECK(r);fixed(NULL,r);CHECK(cwist_async_respond_with(a,r));
        }
    }
    /* Positive proof, not a sleep asserted to be a partial write. */
    if(require_park&&c1m){for(int i=0;i<2000&&!atomic_load(&parks);i++)nanosleep(&pause,NULL);CHECK(atomic_load(&parks)>0);}
    if(shutdown_after)count=shutdown_after;
    size_t capacity=(size_t)count*(sizeof(payload)+4096);unsigned char *wire=malloc(capacity);CHECK(wire);
    used=0;
    for(;;){
        CHECK(used<capacity);
        ssize_t n=recv(client,wire+used,capacity-used,0);
        if(n<0&&errno==EINTR)continue;
        if(n<0)fprintf(stderr,"TCP recv failed: errno=%d mode=%s gc=%d path=%s count=%d bytes=%zu payload_len=%zu raw=%d extra_len=%zu park=%d deferred=%d inline_abort=%d calls=%d hits=%d parks=%d\n",
            errno,c1m?"c1m":"classic",cwist_full_gc_enabled(),path,count,used,payload_len,raw_requests!=NULL,strlen(extra),require_park,complete_deferred,inline_abort_case,atomic_load(&calls),atomic_load(&hits),atomic_load(&parks));
        CHECK(n>=0);if(!n)break;used+=(size_t)n;
    }
    close(client);
    if(c1m){atomic_store(&g_cwist_running,false);cwist_reactor_t *r=atomic_load(&reactor);CHECK(r);cwist_reactor_post_t p={.cb=wake};CHECK(cwist_reactor_post(r,&p));cwist_http_pool_destroy();}
    else if(!joined)CHECK(pthread_join(thread,NULL)==0);
    size_t offset=0;
    for(int i=0;i<count;i++){
        size_t end=offset;while(end+3<used&&memcmp(wire+end,"\r\n\r\n",4))end++;
        CHECK(end+3<used&&end-offset<4096);char headers[4096];memcpy(headers,wire+offset,end-offset);headers[end-offset]=0;
        char expected[32];snprintf(expected,sizeof(expected),"HTTP/1.1 %d ",status);CHECK(!strncmp(headers,expected,strlen(expected)));
        char *cl=strstr(headers,"Content-Length: ");CHECK(cl);size_t length=(size_t)strtoul(cl+16,NULL,10);
        if(expect_post)CHECK(strstr(headers,"X-Post: ran"));
        if(complete_deferred&&c1m)CHECK(strstr(headers,"X-Producer: owned"));
        CHECK(strstr(headers,i==count-1?"Connection: close":"Connection: keep-alive"));
        offset=end+4;CHECK(length<=used-offset);
        if(inline_abort_case){const char *error="Internal Server Error";CHECK(length==strlen(error)&&!memcmp(wire+offset,error,length));}
        else if(status!=401){CHECK(length==payload_len);CHECK(!memcmp(wire+offset,payload,length));}
        else {CHECK(length==6&&!memcmp(wire+offset,"DENIED",6));}
        offset+=length;
    }
    CHECK(offset==used);free(wire);
}
int main(int argc,char **argv){
    CHECK(argc==3);c1m=!strcmp(argv[1],"c1m");cwist_full_gc(!strcmp(argv[2],"gc"));
    alarm(300);signal(SIGPIPE,SIG_IGN);setenv("CWIST_C1M_MODE",c1m?"1":"0",1);setenv("CWIST_WORKER_THREADS","1",1);setenv("CWIST_WORKERS","1",1);
    for(size_t i=0;i<sizeof(payload);i++)payload[i]=(unsigned char)(i%251); /* includes NUL */
    cwist_app *app=cwist_app_create();CHECK(app);
    CHECK(cwist_app_use_session(app,NULL)==0); /* Ephemeral test config, never logged. */
    cwist_pfc_destroy(app->public_fixed_cache);
    app->public_fixed_cache=cwist_pfc_create_with(test_clock,test_alloc);CHECK(app->public_fixed_cache);
    cwist_app_get_opt(app,"/public",fixed,CWIST_ENDPOINT_PUBLIC_FIXED);
    cwist_app_get_opt(app,"/inline-abort",inline_abort_response,CWIST_ENDPOINT_PUBLIC_FIXED);
    cwist_app_get_opt(app,"/bare",fixed,CWIST_ENDPOINT_FIXED);cwist_app_get(app,"/dynamic",fixed);
    cwist_app_get_opt(app,"/private",private_response,CWIST_ENDPOINT_PUBLIC_FIXED);
    cwist_app_get_opt(app,"/context",private_context,CWIST_ENDPOINT_PUBLIC_FIXED);
    cwist_app_get_opt(app,"/error",error_response,CWIST_ENDPOINT_PUBLIC_FIXED);
    cwist_app_get_opt(app,"/file",file_response,CWIST_ENDPOINT_PUBLIC_FIXED);
    cwist_app_get_opt(app,"/deferred",deferred_response,CWIST_ENDPOINT_PUBLIC_FIXED);
    const char poison[]="HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nPOISON";
    cwist_bdr_put_fixed(app->bdr_ctx,"GET","/public",poison,sizeof(poison)-1);
    cwist_bdr_put_fixed(app->bdr_ctx,"GET","/bare",poison,sizeof(poison)-1);
    exchange(app,"/public","one.test","",1,200);CHECK(atomic_load(&calls)==1&&atomic_load(&releases)==1);
    require_park=true;exchange(app,"/public","one.test","",32,200);require_park=false;CHECK(atomic_load(&calls)==1&&atomic_load(&releases)==1);
    exchange(app,"/public","two.test","",2,200);CHECK(atomic_load(&calls)==2);
    exchange(app,"/bare","one.test","",2,200);CHECK(atomic_load(&calls)==4);
    exchange(app,"/dynamic","one.test","",2,200);CHECK(atomic_load(&calls)==6);
    exchange(app,"/private","one.test","",2,200);CHECK(atomic_load(&calls)==8);
    const char *denied[]={"Authorization: Bearer private\r\n","Cookie: private=1\r\n","Accept-Encoding: gzip\r\n","Range: bytes=0-1\r\n","Cache-Control: no-cache\r\n","X-Unknown: x\r\n","Content-Length: 0\r\n","User-Agent: a\r\nUser-Agent: b\r\n"};
    for(size_t i=0;i<sizeof(denied)/sizeof(*denied);i++){int before=atomic_load(&calls);exchange(app,"/public","one.test",denied[i],2,200);CHECK(atomic_load(&calls)==before+2);}
    int before=atomic_load(&calls);exchange(app,"/public?q=1","one.test","",2,200);CHECK(atomic_load(&calls)==before+2);
    before=atomic_load(&calls);exchange(app,"/public?","one.test","",2,200);CHECK(atomic_load(&calls)==before);
    /* Deterministic lifetime/failure injection, real TCP and real senders. */
    cache_time+=60;before=atomic_load(&calls);exchange(app,"/public","one.test","",2,200);CHECK(atomic_load(&calls)==before+1);
    cache_oom=true;before=atomic_load(&calls);exchange(app,"/public","one.test","",2,200);CHECK(atomic_load(&calls)==before+2);cache_oom=false;
    for(int i=0;i<260;i++){
        char host[32];snprintf(host,sizeof(host),"capacity%d.test",i);
        before=atomic_load(&calls);exchange(app,"/public",host,"",1,200);CHECK(atomic_load(&calls)==before+1);
    }
    CHECK(cwist_pfc_count(app->public_fixed_cache)<=256);CHECK(cwist_pfc_bytes(app->public_fixed_cache)<=16u*1024*1024);
    before=atomic_load(&calls);exchange(app,"/public","capacity0.test","",2,200);CHECK(atomic_load(&calls)==before+1);
    before=atomic_load(&calls);cwist_app_clear_public_fixed_cache(app);exchange(app,"/public","one.test","",2,200);CHECK(atomic_load(&calls)==before+1);
    before=atomic_load(&calls);cwist_app_get_opt(app,"/public",fixed,CWIST_ENDPOINT_PUBLIC_FIXED);exchange(app,"/public","one.test","",2,200);CHECK(atomic_load(&calls)==before+1);
    /* R2: a handler's private state must survive as an admission decision,
     * even though execute_chain clears its temporary executor slot. */
    size_t entries=cwist_pfc_count(app->public_fixed_cache);
    before=atomic_load(&calls);exchange(app,"/context","one.test","",2,200);exchange(app,"/context","one.test","",1,200);
    CHECK(atomic_load(&calls)==before+3&&cwist_pfc_count(app->public_fixed_cache)==entries);
    before=atomic_load(&calls);exchange(app,"/error","one.test","",2,500);CHECK(atomic_load(&calls)==before+2);
    for(int i=0;i<2;i++){exchange(app,"/file","one.test","",1,200);CHECK(fcntl(last_file_fd,F_GETFD)==-1&&errno==EBADF);}CHECK(atomic_load(&file_calls)==2);
    inline_abort_case=true;exchange(app,"/inline-abort","one.test","",1,500);inline_abort_case=false;
    complete_deferred=true;
    for(int i=0;i<2;i++)exchange(app,"/deferred","one.test","",1,200);
    complete_deferred=false;CHECK(atomic_load(&deferred_calls)==2);CHECK(cwist_pfc_count(app->public_fixed_cache)==entries);
    /* Fully framed requests in the real parser, immediately followed by a
     * warmed cacheable request: the body cannot become a phantom request. */
    const char *framed[]={
        "GET /public HTTP/1.1\r\nHost: one.test\r\nContent-Length: 3\r\n\r\nabcGET /public HTTP/1.1\r\nHost: one.test\r\nConnection: close\r\n\r\n",
        "GET /public HTTP/1.1\r\nHost: one.test\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\nGET /public HTTP/1.1\r\nHost: one.test\r\nConnection: close\r\n\r\n"};
    for(int i=0;i<2;i++){raw_requests=framed[i];before=atomic_load(&calls);exchange(app,"/public","one.test","",2,200);CHECK(atomic_load(&calls)==before+1);CHECK(atomic_load(&hits)==1);}raw_requests=NULL;
    CHECK(atomic_load(&body_calls)==2&&atomic_load(&chunked_calls)==1);
    const char *protocol_miss[]={"Expect: 100-continue\r\n","Upgrade: websocket\r\n"};
    for(int i=0;i<2;i++){before=atomic_load(&calls);exchange(app,"/public","one.test",protocol_miss[i],2,200);CHECK(atomic_load(&calls)==before+2);}
    /* Fail after snapshot lookup and header reconstruction, then verify one
     * ordinary dispatch, intact framing, and a later successful hit. */
    atomic_store(&fail_reconstruction,true);before=atomic_load(&calls);exchange(app,"/public","one.test","",2,200);
    CHECK(atomic_load(&reconstruction_failures)==1&&atomic_load(&calls)==before+1&&atomic_load(&hits)==1);
    shutdown_after=3;before=atomic_load(&calls);exchange(app,"/public","one.test","",8,200);shutdown_after=0;
    CHECK(atomic_load(&hits)==3&&atomic_load(&calls)==before);
    payload_len=sizeof(payload);cwist_app_clear_public_fixed_cache(app);
    before=atomic_load(&calls);exchange(app,"/public","boundary.test","",2,200);CHECK(atomic_load(&calls)==before+1);
    payload_len=2048;cwist_app_clear_public_fixed_cache(app);
    cwist_app *post=cwist_app_create();CHECK(post);cwist_app_get_opt(post,"/public",fixed,CWIST_ENDPOINT_PUBLIC_FIXED);cwist_app_use(post,after_handler);
    expect_post=true;before=atomic_load(&calls);exchange(post,"/public","one.test","",2,200);expect_post=false;CHECK(atomic_load(&calls)==before+2&&atomic_load(&post_calls)==2&&cwist_pfc_count(post->public_fixed_cache)==0);
    CHECK(cwist_app_use_session(post,NULL)==0);
    cwist_app_get_opt(post,"/defer",deferred_response,CWIST_ENDPOINT_PUBLIC_FIXED);
    complete_deferred=true;exchange(post,"/defer","one.test","",1,200);complete_deferred=false;
    CHECK(atomic_load(&post_calls)==3);
    cwist_app_get_opt(post,"/inline-abort",inline_abort_response,CWIST_ENDPOINT_PUBLIC_FIXED);
    inline_abort_case=true;expect_post=true;exchange(post,"/inline-abort","one.test","",1,500);inline_abort_case=false;expect_post=false;
    CHECK(atomic_load(&post_calls)==4);cwist_app_destroy(post);
    atomic_store(&middleware_calls,0);
    cwist_app_use(app,deny);before=atomic_load(&calls);exchange(app,"/public","one.test","",2,401);CHECK(atomic_load(&calls)==before&&atomic_load(&middleware_calls)==2);
    CHECK(atomic_load(&calls)==atomic_load(&releases));cwist_app_destroy(app);
    printf("public FIXED real TCP %s %s: PASS\n",argv[1],argv[2]);return 0;
}
