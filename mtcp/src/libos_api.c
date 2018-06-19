#include <sys/queue.h>
#include <sys/ioctl.h>
#include <limits.h>
#include <unistd.h>
#include <assert.h>
#include <signal.h>

#include "mtcp.h"
#include "mtcp_api.h"
#include "tcp_in.h"
#include "tcp_stream.h"
#include "tcp_out.h"
#include "ip_out.h"
#include "eventpoll.h"
#include "pipe.h"
#include "fhash.h"
#include "addr_pool.h"
#include "rss.h"
#include "config.h"
#include "debug.h"

#define MAX(a, b) ((a)>(b)?(a):(b))
#define MIN(a, b) ((a)<(b)?(a):(b))

#define MTCP_CPU_SUM 1
#define MTCP_MAX_FLOW_NUM (10000)
#define MTCP_MAX_EVENTS (MTCP_MAX_FLOW_NUM * 3)


struct app_vars {
	int recv_len;
    int request_len;
};

struct libos_thread_context {
    mctx_t mctx;
    int ep;
    struct app_vars *appvars;
};


static int proc_done[MTCP_CPU_SUM];
static int num_cores;
static int core_limit;
static pthread_t app_thread[MTCP_CPU_SUM];
static int cores[MTCP_CPU_SUM];
struct libos_thread_context libos_threads_ctx_list[MTCP_CPU_SUM];


/******************************************************************************/
// libos_mtcp implementation
/******************************************************************************/


struct libos_thread_context *get_current_thread_context(){
    // the the current running thread (caller)'s corresponding thread context 
    int i;
    pthread_t tid = pthread_self();
    for(i = 0; i < MTCP_CPU_SUM; i++){
        if (app_thread[i] == tid){
            return libos_threads_ctx_list[i];
        }
    }
    TRACE_CONFIG("ERROR: cannot find corresponding thread context\n");
    return NULL;
}


void libos_signal_handler(int signum) {
    int i;

    for (i = 0; i < core_limit; i++) {
        if (app_thread[i] == pthread_self()) {
            proc_done[i] = TRUE;
        } else {
            if (!proc_done[i]) {
                pthread_kill(app_thread[i], signum);
            }
        }
    }
}

int init_mtcp_app_thread_ctx(void *arg) {
    // TODO: another way to pass core number in
    // Assume no HT
    int core = *(int *)arg;
    mtcp_core_affinitize(core);
    struct libos_thread_context *ctx = &libos_threads_ctx_list[core];
    // create mtcp context, which will spawn an mtcp thread
    if (!ctx) {
        TRACE_ERROR("Failed to create thread context!\n");
        return NULL;
    }
    /* create mtcp context: this will spawn an mtcp thread */
    ctx->mctx = mtcp_create_context(core);
    if (!ctx->mctx) {
        TRACE_ERROR("Failed to create mtcp context!\n");
        free(ctx);
        return NULL;
    }

    /* create epoll descriptor */
    ctx->ep = mtcp_epoll_create(ctx->mctx, MTCP_MAX_EVENTS);
    if (ctx->ep < 0) {
        mtcp_destroy_context(ctx->mctx);
        free(ctx);
        TRACE_ERROR("Failed to create epoll descriptor!\n");
        return NULL;
    }

    /* allocate memory for server variables */
    ctx->appvars = (struct app_vars *)
            calloc(MTCP_MAX_FLOW_NUM, sizeof(struct app_vars));
    if (!ctx->appvars) {
        mtcp_close(ctx->mctx, ctx->ep);
        mtcp_destroy_context(ctx->mctx);
        //free(ctx);
        TRACE_ERROR("Failed to create server_vars struct!\n");
        return NULL;
    }

    return 0;
}

int libos_mtcp_init(const char *config_file){
    int ret;
    int i;
    // running parameters
    num_cores = MTCP_CPU_SUM;
    core_limit = num_cores;


    if (config_file == NULL){
        TRACE_CONFIG("MTCP: not configuration file!\n");
        exit(EXIT_FAILURE);
    }
    ret = mtcp_init(config_file);
    if (ret){
        TRACE_CONFIG("Failed to initialize mtcp\n");
        exit(EXIT_FAILURE);
    }
    mtcp_register_signal(SIGINT, libos_signal_handler);
    for(i = 0; i < core_limit; i++){
        cores[i] = i;
        proc_done[i] = FALSE;
        if (pthread_create(&app_thread[i], NULL, init_mtcp_app_thread_ctx, (void *)&cores[i])) {
            perror("pthread_create");
            TRACE_CONFIG("Failed to create thread.\n");
            exit(EXIT_FAILURE);
        }
    }

    return ret;
}


// network functions
int libos_mtcp_queue(int domain, int type, int protocol){
    int listener;
    int ret;
#ifdef LIBOS_MTCP_DEBUG
	printf("@@@@@@@@@JINGLIU:libos_mtcp_queue()@@@@@@@@@\n");
#endif
    struct libos_thread_context *ctx = get_current_thread_context();
    if (ctx == NULL){
        exit(EXIT_FAILURE);
    }
    qd = mtcp_socket(ctx->mctx, AF_INET, SOCK_STREAM, 0);
    // by default, the semantics of libos is no-blocking
    ret = mtcp_setsock_nonblock(ctx->mctx, listener);
    if(ret < 0){
        TRACE_CONFIG("Failed to set noblocking");
        exit(EXIT_FAILURE);
    }
    return qid;
}

int libos_mtcp_listen(int qd, int backlog){
    int ret;
    struct mtcp_epoll_event ev;
    struct libos_thread_context *ctx = get_current_thread_context();
    ret = mtcp_listen(ctx->mctx, qd, backlog);
    ev.events = MTCP_EPOLLIN;
    ev.data.sockid = qd;
    mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_ADD, qd, &ev);
    return ret;

}

int libos_mtcp_bind(int qd, struct sockaddr *saddr, socklen_t size){
    int ret;
    struct libos_thread_context *ctx = get_current_thread_context();
    ret = mtcp_bind(ctx->mctx, qd, saddr, size);
    return ret;
}

int libos_mtcp_accept(int qd, struct sockaddr *saddr, socklen_t *size){
    struct mtcp_epoll_event ev;
    struct libos_thread_context *ctx = get_current_thread_context();
    int child_qd;
    child_qd = mtcp_accept(ctx->mctx, qd, NULL, NULL);
    if (child_qd >= 0){
        if (child_qd >= MAX_FLOW_NUM){
            TRACE_ERROR("Invalid socked it %d.\n", child_qd);
            return -1;
        }
        ev.events = MTCP_EPOLLIN;
        ev.data.sockid = child_qd;
        mtcp_setsock_nonblock(ctx->mctx, child_qd);
        mtcp_epoll_ctl(mctx, ctx->ep, MTCP_EPOLL_CTL_ADD, child_qd, &ev);
        TRACE_APP("Socket %d register\n", child_qd);
    }
    return child_qd;
}

int libos_mtcp_connect(int qd, struct sockaddr *saddr, socklen_t size){
    int ret;
    struct mtcp_epoll_event ev;
    struct libos_thread_context *ctx = get_current_thread_context();
    ret = mtcp_connect(ctx->mctx, qd, saddr, size);
    if (ret < 0){
        if (erron != EINPROGRESS){
            perror("mtcp_connect");
            mtcp_close(mctx, qd);
            return -1;
        }
    }
    ev.events = MTCP_EPOLLOUT;
    ev.data.sockid = qd;
    mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_ADD, qd, &ev);
    return ret;
}

int libos_mtcp_close(int qd){
    struct libos_thread_context *ctx = get_current_thread_context();
    mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_DEL, qd, NULL);
    mtcp_close(ctx->mctx, qd);
    return 0;
}

// other functions
int libos_mtcp_push(int qd, zeus_sgarray *sga){
    // if return 0, then already complete
    return 0;
}

int libos_mtcp_pop(int qd, zeus_sgarray *sga){
    //if return 0, then already ready and in sga
    return 0;
}

ssize_t libos_mtcp_wait(int qt, zeus_sgarray *sga){
    return 0;
}

ssize_t libos_mtcp_wait_any(int *qts, zeus_sgarray *sga){
    return 0;
}

ssize_t libos_mtcp_wait_all(int *qts, zeus_sgarray *sga){
    // identical to a push, followed by a wait on the returned qtoken
    return 0;
}

ssize_t libos_mtcp_blocking_push(int qd, zeus_sgarray *sga){
    // identical to a pop, followed by a wait on the returned qtoken
    return 0;
}

ssize_t libos_mtcp_blocking_pop(int qd, zeus_sgarray *sga){
    return 0;
}

/******************************************************************************/

