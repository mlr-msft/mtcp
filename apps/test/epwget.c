#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/queue.h>
#include <assert.h>
#include <limits.h>

#include <mtcp_api.h>
#include <mtcp_epoll.h>
#include "cpu.h"
#include "rss.h"
#include "http_parsing.h"
#include "netlib.h"
#include "debug.h"

#define MAX_URL_LEN 128
#define MAX_FILE_LEN 128
#define HTTP_HEADER_LEN 1024

#define IP_RANGE 1
#define MAX_IP_STR_LEN 16

#define BUF_SIZE (8*1024)

#define CALC_MD5SUM FALSE

#define TIMEVAL_TO_MSEC(t)		((t.tv_sec * 1000) + (t.tv_usec / 1000))
#define TIMEVAL_TO_USEC(t)		((t.tv_sec * 1000000) + (t.tv_usec))
#define TS_GT(a,b)				((int64_t)((a)-(b)) > 0)

#ifndef TRUE
#define TRUE (1)
#endif

#ifndef FALSE
#define FALSE (0)
#endif

#ifndef ERROR
#define ERROR (-1)
#endif

#ifndef MAX_CPUS
#define MAX_CPUS		16
#endif
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
static int num_cores = 1;
static int core_limit = 1;
static int max_fds = 3;

static char host[MAX_IP_STR_LEN + 1] = {'\0'};
static char url[MAX_URL_LEN + 1] = {'\0'};
static int concurrency = 1;
static int total_concurrency = 1;
//static in_addr_t daddr;
//static in_port_t dport;
//static in_addr_t saddr;

int 
main(int argc, char **argv)
{
	struct mtcp_conf mcfg;
	char *conf_file;
    struct sockaddr_in saddr;
	int ret;
    int o;
	//int i, o;
	//int process_cpu;

	if (argc < 3) {
		TRACE_CONFIG("Too few arguments!\n");
		TRACE_CONFIG("Usage: %s url #flows [output]\n", argv[0]);
		return FALSE;
	}

	if (strlen(argv[1]) > MAX_URL_LEN) {
		TRACE_CONFIG("Length of URL should be smaller than %d!\n", MAX_URL_LEN);
		return FALSE;
	}

	char* slash_p = strchr(argv[1], '/');
	if (slash_p) {
		strncpy(host, argv[1], slash_p - argv[1]);
		strncpy(url, strchr(argv[1], '/'), MAX_URL_LEN);
	} else {
		strncpy(host, argv[1], MAX_IP_STR_LEN);
		strncpy(url, "/", 2);
	}

	conf_file = NULL;
	saddr.sin_port = htons(10001);
	saddr.sin_addr.s_addr = inet_addr(host);
    saddr.sin_family = AF_INET;


	//num_cores = GetNumCPUs();
	//core_limit = num_cores;
	//concurrency = 100;

	while (-1 != (o = getopt(argc, argv, "N:c:o:n:f:"))) {
		switch(o) {
		case 'N':
			core_limit = mystrtol(optarg, 10);
			if (core_limit > num_cores) {
				TRACE_CONFIG("CPU limit should be smaller than the "
					     "number of CPUS: %d\n", num_cores);
				return FALSE;
			} else if (core_limit < 1) {
				TRACE_CONFIG("CPU limit should be greater than 0\n");
				return FALSE;
			}
			/** 
			 * it is important that core limit is set 
			 * before mtcp_init() is called. You can
			 * not set core_limit after mtcp_init()
			 */
			mtcp_getconf(&mcfg);
			mcfg.num_cores = core_limit;
			mtcp_setconf(&mcfg);
			break;
		case 'c':
			total_concurrency = mystrtol(optarg, 10);
			break;
		case 'o':
				break;
		case 'n':
			break;
		case 'f':
			conf_file = optarg;
			break;
		}
	}


	/* per-core concurrency = total_concurrency / # cores */
	if (total_concurrency > 0)
		concurrency = total_concurrency / core_limit;
    concurrency = 1;

	/* set the max number of fds 3x larger than concurrency */
	max_fds = concurrency * 3;

	TRACE_CONFIG("Application configuration:\n");
	TRACE_CONFIG("URL: %s\n", url);
	TRACE_CONFIG("# of cores: %d\n", core_limit);
	TRACE_CONFIG("Concurrency: %d\n", total_concurrency);

	if (conf_file == NULL) {
		TRACE_ERROR("mTCP configuration file is not set!\n");
		exit(EXIT_FAILURE);
	}

    ret = libos_mtcp_init(conf_file);
	if (ret) {
		TRACE_ERROR("Failed to initialize mtcp.\n");
		exit(EXIT_FAILURE);
	}
	mtcp_getconf(&mcfg);
	mcfg.max_concurrency = max_fds;
	mcfg.max_num_buffers = max_fds;
	mtcp_setconf(&mcfg);

	int qd = libos_mtcp_queue(AF_INET, SOCK_STREAM, 0);
	if(qd < 0){
		TRACE_ERROR("Failed to call libot_mtcp_queue()\n");
		return -1;
	}

    ret = libos_mtcp_connect(qd, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
    zeus_sgarray sga;
    sga.num_bufs = 1;
    char *hi_str = "hello\n";
    sga.bufs[0].buf = (void*)hi_str;
    sga.bufs[0].len = strlen(hi_str);

    libos_mtcp_push(qd, &sga);

	mtcp_destroy();
	return 0;
}
/*----------------------------------------------------------------------------*/
