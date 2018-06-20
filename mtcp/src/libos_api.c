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
#include "uthash.h"

#define MAX(a, b) ((a)>(b)?(a):(b))
#define MIN(a, b) ((a)<(b)?(a):(b))

#define MTCP_CPU_SUM 1
#define MTCP_MAX_FLOW_NUM (10000)
#define MTCP_MAX_EVENTS (MTCP_MAX_FLOW_NUM * 3)


#define MAGIC 91917171
#define ZEUS_IO_ERR_NO (-9)
#define MAX_EVENTS (16 * 1024)

struct mtcp_pending_req {
    int qd;             // key
    void *buf;
    size_t res;         // TODO: Q: why ssize_t, size_t in libos-posix
    size_t buf_size;
    int req_done;
    UT_hash_handle hh;
};

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

struct mtcp_pending_req *outcome_pending_req_map = NULL;
struct mtcp_pending_req *income_pending_req_map = NULL;

/******************************************************************************/
// libos_mtcp implementation
/******************************************************************************/


struct libos_thread_context *get_current_thread_context(){
    // the the current running thread (caller)'s corresponding thread context 
    printf("@@@@@@ get_current_thread_context()\n");
    int i;
    pthread_t tid = pthread_self();
    for(i = 0; i < MTCP_CPU_SUM; i++){
        printf("@@@@@@i:%d, app_threaad[i]%lu tid:%lu\n", i, app_thread[i], tid);
        if (app_thread[i] == tid){
            return &libos_threads_ctx_list[i];
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

void* init_mtcp_app_thread_ctx(int core_id) {
    // TODO: another way to pass core number in
    // Assume no HT
    int core = core_id;
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
    //ctx->appvars = (struct app_vars *) calloc(MTCP_MAX_FLOW_NUM, sizeof(struct app_vars));
    //if (!ctx->appvars) {
    //    mtcp_close(ctx->mctx, ctx->ep);
    //    mtcp_destroy_context(ctx->mctx);
    //    //free(ctx);
    //    TRACE_ERROR("Failed to create server_vars struct!\n");
    //    return NULL;
    //}

    return NULL;
}

int libos_mtcp_init(const char *config_file){
    int ret;
    int i;
    // running parameters
    num_cores = MTCP_CPU_SUM;
    core_limit = num_cores;

#ifdef LIBOS_MTCP_DEBUG
	printf("@@@@@@@@@JINGLIU:libos_mtcp_init()@@@@@@@@@\n");
#endif
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
        init_mtcp_app_thread_ctx(cores[i]);
        app_thread[i] = pthread_self();
    }

    return ret;
}


// network functions
int libos_mtcp_queue(int domain, int type, int protocol){
    int qd;
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
    ret = mtcp_setsock_nonblock(ctx->mctx, qd);
    if(ret < 0){
        TRACE_CONFIG("Failed to set noblocking");
        exit(EXIT_FAILURE);
    }
    return qd;
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
        if (child_qd >= MTCP_MAX_FLOW_NUM){
            TRACE_ERROR("Invalid socked it %d.\n", child_qd);
            return -1;
        }
        ev.events = MTCP_EPOLLIN;
        ev.data.sockid = child_qd;
        mtcp_setsock_nonblock(ctx->mctx, child_qd);
        mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_ADD, child_qd, &ev);
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
        if (errno != EINPROGRESS){
            perror("mtcp_connect\n");
            mtcp_close(ctx->mctx, qd);
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
	size_t count, total = 0;
    uint64_t magic = MAGIC;
    uint64_t num = sga->num_bufs;
    uint64_t totalLen = 0;
    struct mtcp_pending_req *req;
    struct mtcp_epoll_event ev;
    struct libos_thread_context *ctx = get_current_thread_context();
    // find the req rcd
    HASH_FIND_INT(outcome_pending_req_map, &qd, req);
    if(req == NULL){
        req = (struct mtcp_pending_req*) malloc(sizeof *req);
        req->qd = qd;
        req->res = 0;
        req->buf_size = 0;
        req->req_done = FALSE;
    }
    ////////////////////////////
    // C & P from libos-posix, let's see if anything wrong
    count = mtcp_write(ctx->mctx, qd, (char *) &magic, sizeof(uint64_t)/sizeof(char));
    if ((size_t)count < sizeof(uint64_t)) {
        fprintf(stderr, "Could not ::write magic\n");
        return -1;
    }
	for (int i = 0; i < sga->num_bufs; i++) {
        totalLen += (uint64_t)((sga->bufs[i]).len);
        totalLen += sizeof(uint64_t);
		// TODO: JL: add the calling of pin() here after integrate the Hoard lib
        //pin((void *)sga.bufs[i].buf);
    }
	totalLen += sizeof(num);
    req->buf_size = totalLen;
	count = mtcp_write(ctx->mctx, qd, (char *)&totalLen, sizeof(uint64_t)/sizeof(char));
    if (count < 0 || (size_t)count < sizeof(uint64_t)) {
        fprintf(stderr, "Could not ::write total length\n");
        return -1;
    }
	count = mtcp_write(ctx->mctx, qd, (char *)&num, sizeof(uint64_t)/sizeof(char));
    if (count < 0 || (size_t)count < sizeof(uint64_t)) {
        fprintf(stderr, "Could not ::write sga entries\n");
        return -1;
    }
    for (int i = 0; i < sga->num_bufs; i++) {
        if((sga->bufs[i]).len > MTCP_SNDBUF_SIZE){
            fprintf(stderr, "writing unit should be less than %d, real len is %lu", MTCP_SNDBUF_SIZE, (sga->bufs[i]).len);
            return -1;
        }
        // stick in size header
        count = mtcp_write(ctx->mctx, qd, (char *) &(((sga->bufs)[i]).len), sizeof(uint64_t)/sizeof(char));
        if (count < 0 || (size_t)count < sizeof((sga->bufs[i]).len)) {
            fprintf(stderr, "Could not ::write sga entry len\n");
            return -1;
        }
        // write buffer
        count = mtcp_write(ctx->mctx, qd, (char*)((sga->bufs)[i]).buf,
                      ((sga->bufs)[i]).len);
        // TODO: JL: call unpin() here
        //unpin((void *)sga.bufs[i].buf);
        if (count < 0 || (size_t)count < (sga->bufs[i]).len) {
            fprintf(stderr, "Could not ::write sga buf\n");
            return -1;
        }
        total += count;
    }
    // add write event
    ev.events = MTCP_EPOLLIN | MTCP_EPOLLOUT;
    ev.data.sockid = qd;
    mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_MOD, qd, &ev);

    // NOTE: JL: stil not sure what this staff used for
    req->res = total;
    req->req_done = TRUE;

    return total;
}

int libos_mtcp_pop(int qd, zeus_sgarray *sga){
    //if return 0, then already ready and in sga
    void *buf;
	uint8_t *ptr;
    size_t count;
    size_t total = 0;
    struct mtcp_pending_req *req;
    struct libos_thread_context *ctx = get_current_thread_context();
    size_t headerSize = sizeof(uint64_t) * 2;
    HASH_FIND_INT(income_pending_req_map, &qd, req);
    if(req == NULL){
        req = (struct mtcp_pending_req*)malloc(sizeof *req);
        req->qd = qd;
        req->res = 0;
        req->buf_size = 0;
        req->req_done = FALSE;
    }
    buf = req->buf;
    count = req->buf_size;

	// if we aren't already working on a buffer, allocate one
    if (buf == NULL) {
        buf = malloc(headerSize);
        count = 0;
    }
	// if we don't have a full header in our buffer, then get one
    if (count < headerSize) {
        ssize_t res = mtcp_read(ctx->mctx, qd, (char *)((uint8_t *)buf + count), headerSize - count);
        if(res == 0){
            return 0;
        }
        // we still don't have a header
        if ((res < 0 && errno == EAGAIN) ||
            (res >= 0 && (count + (size_t)res < headerSize))) {
            // try again later
            req->buf = buf;
            req->buf_size =
                (res > 0) ? count + res : count;
            return ZEUS_IO_ERR_NO;
        } else if (res < 0) {
            return res;
        } else {
            count += res;
        }

    }
    // go to the beginning of the buffer to check the header
    ptr = (uint8_t *)buf;
    uint64_t magic = *(uint64_t *)ptr;
    if (magic != MAGIC) {
        // not a correctly formed packet
        fprintf(stderr, "Could not find magic %lu\n", magic);
        exit(-1);
        return -1;
    }
    ptr += sizeof(magic);
    uint64_t totalLen = *(uint64_t *)ptr;
    ptr += sizeof(totalLen);
    // grab the rest of the packet
    if (count < headerSize + totalLen) {
        buf = realloc(buf, totalLen + headerSize);    
        ssize_t res = mtcp_read(ctx->mctx, qd,(char *)((uint8_t *)buf + count), totalLen + headerSize - count);
        if(res == 0) {
            return 0;
        }
        // try again later
        if ((res < 0 && errno == EAGAIN) ||
            (res >= 0 && (count + (size_t)res < totalLen + headerSize))) {
            // try again later
            req->buf = buf;
            req->buf_size = (res > 0) ? count + res : count;
            return ZEUS_IO_ERR_NO;
        } else if (res < 0) {
            return res;
        } else {
            count += res;
        }
    }

	// now we have the whole buffer, start ::reading data
    ptr = (uint8_t *)buf + headerSize;
    sga->num_bufs = *(uint64_t *)ptr;
    ptr += sizeof(uint64_t);
    for (int i = 0; i < sga->num_bufs; i++) {
        ((sga->bufs)[i]).len = *(size_t *)ptr;
        ptr += sizeof(uint64_t);
        ((sga->bufs)[i]).buf = (void *)ptr;
        ptr += ((sga->bufs)[i]).len;
        total += ((sga->bufs)[i]).len;
    }
    req->buf = NULL;
    req->buf_size = 0;
    req->req_done = TRUE;
    req->res = total;
    return total;
}

ssize_t libos_mtcp_wait(int qt, zeus_sgarray *sga){
    int qts[1] = {qt};
    int numevents;
    numevents = libos_mtcp_wait_any(qts, 1, sga);
    return numevents;
}

ssize_t libos_mtcp_wait_any(int *qts, int qsum, zeus_sgarray *sga){
    int numevents;
    struct mtcp_epoll_event *events;
    struct libos_thread_context *ctx = get_current_thread_context();
    events = (struct mtcp_epoll_event *)calloc(MAX_EVENTS, sizeof(struct mtcp_epoll_event));
    numevents = mtcp_epoll_queue_wait(ctx->mctx, ctx->ep, events, MAX_EVENTS, -1, qts, qsum, FALSE);
    return numevents;
}

ssize_t libos_mtcp_wait_all(int *qts, int qsum, zeus_sgarray *sga){
    // identical to a push, followed by a wait on the returned qtoken
    int numevents;
    struct mtcp_epoll_event *events;
    struct libos_thread_context *ctx = get_current_thread_context();
    events = (struct mtcp_epoll_event *)calloc(MAX_EVENTS, sizeof(struct mtcp_epoll_event));
    numevents = mtcp_epoll_queue_wait(ctx->mctx, ctx->ep, events, MAX_EVENTS, -1, qts, qsum, TRUE);
    return numevents;
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

