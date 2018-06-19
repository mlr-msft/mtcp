#define _LARGEFILE64_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <limits.h>

#include <mtcp_api.h>
#include <mtcp_epoll.h>

#include "cpu.h"
#include "http_parsing.h"
#include "netlib.h"
#include "debug.h"

#define MAX_FLOW_NUM  (10000)

#define RCVBUF_SIZE (2*1024)
#define SNDBUF_SIZE (8*1024)

#define MAX_EVENTS (MAX_FLOW_NUM * 3)

#define HTTP_HEADER_LEN 1024
#define URL_LEN 128

#define MAX_FILES 30

#define NAME_LIMIT 128
#define FULLNAME_LIMIT 256

#ifndef TRUE
#define TRUE (1)
#endif

#ifndef FALSE
#define FALSE (0)
#endif

#ifndef ERROR
#define ERROR (-1)
#endif

#define HT_SUPPORT FALSE

#ifndef MAX_CPUS
#define MAX_CPUS		16
#endif


static void
printHelp(const char *prog_name)
{
	TRACE_CONFIG("%s -p <path_to_www/> -f <mtcp_conf_file> "
		     "[-N num_cores] [-c <per-process core_id>] [-h]\n",
		     prog_name);
	exit(EXIT_SUCCESS);
}
/*----------------------------------------------------------------------------*/
int 
main(int argc, char **argv)
{
	DIR *dir;
	struct dirent *ent;
	int fd;
	int ret;
	uint64_t total_read;
	struct mtcp_conf mcfg;
	int process_cpu;
	int  o;  	// return value of getopt()
	int num_cores;
	int backlog;
	int core_limit;
	char *conf_file = NULL;
	struct sockaddr_in saddr;

	num_cores = GetNumCPUs();
	core_limit = num_cores;

	if (argc < 2) {
		TRACE_CONFIG("$%s directory_to_service\n", argv[0]);
		return FALSE;
	}

	while (-1 != (o = getopt(argc, argv, "N:f:p:c:b:h"))) {
		switch (o) {
		case 'p':
			/* open the directory to serve */
			www_main = optarg;
			dir = opendir(www_main);
			if (!dir) {
				TRACE_CONFIG("Failed to open %s.\n", www_main);
				perror("opendir");
				return FALSE;
			}
			break;
		case 'N':
			core_limit = mystrtol(optarg, 10);
			if (core_limit > num_cores) {
				TRACE_CONFIG("CPU limit should be smaller than the "
					     "number of CPUs: %d\n", num_cores);
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
		case 'f':
			conf_file = optarg;
			break;
		case 'c':
			process_cpu = mystrtol(optarg, 10);
			if (process_cpu > core_limit) {
				TRACE_CONFIG("Starting CPU is way off limits!\n");
				return FALSE;
			}
			break;
		case 'b':
			backlog = mystrtol(optarg, 10);
			break;
		case 'h':
			printHelp(argv[0]);
			break;
		}
	}
	

	}
	/* initialize mtcp */
	if (conf_file == NULL) {
		TRACE_CONFIG("You forgot to pass the mTCP startup config file!\n");
		exit(EXIT_FAILURE);
	}

    ret = libos_mtcp_init(conf_file);
	// TODO: check return value of libos_mtcp_init
	// libos will init the event for me
	int listen_qd = libos_mtcp_queue(AF_INET, SOCK_STERM, 0);
	if(listen_qd < 0){
		TRACE_ERROR("Failed to call libot_mtcp_queue()\n");
		return -1;
	}

	saddr.sin_family = AF_INET;
	saddr.sin_addr.s_addr = INADDR_ANY;
	saddr.sin_port = htons(10001);

	ret = libos_mtcp_bind(listen_qd, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
	if (ret < 0){
		TRACE_ERROR("Failed to call libos_mtcp_bind()\n");
		return -1;
	}
	ret = libos_mtcp_listen(listen_qd, backlog);
	if(ret < 0){
		TRACE_ERROR("Failed to call libos_mtcp_listen()\n");
		return -1;
	}

	int i = 0;
	while(i < 3){
		// do 3 accept() at max
		while(1){
			sleep(1);
			zeus_sgarray zsga;
			int pop_ret = libos_mtcp_pop(listen_qd, &zsga);
			if (pop_ret < 0){
				continue;
			}
			int child_qd = libos_mtcp_accept(listen_qd, (struct sockaddr *)&saddr, sizeof(struct sockaddr));
			// do read
			// TODO: JL: clean the sag
			// TODO: JL: I think clean the data structure should be made as a common rountine
			// or even a interface (for application)
			while (ret = libos_mtcp_pop(child_qd, &zsga) >= 0) {
				// read until nothing to read
			}
			break;
		}
		i--;
	}

	return 0;
}
