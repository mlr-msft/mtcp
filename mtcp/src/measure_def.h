
#ifndef _MTCP_MEASURE_DEF_H_
#define _MTCP_MEASURE_DEF_H_

//#define _MTCP_MEASURE_SERVER_OVERALL_
//#define _MTCP_MEASURE_MTCP_READ_2_DPDK_READ_
//#define _MTCP_MEASURE_MTCP_WRITE_2_DPDK_WRITE_
//#define _MTCP_MEASURE_READ_
//#define _MTCP_MEASURE_WRITE_

//#define _MTCP_MEASURE_DPDK_SEND_
//#define _MTCP_MEASURE_DPDK_RECV_


//#define _MTCP_MEASURE_ALL_ONCE_

typedef struct _mtcp_ticks {
    uint64_t dpdk_recv_start_tick;
    uint64_t dpdk_recv_end_tick;
    uint64_t mtcp_read_start_tick;
    uint64_t mtcp_read_end_tick;
    uint64_t mtcp_write_start_tick;
    uint64_t mtcp_write_end_tick;
    uint64_t dpdk_send_start_tick;
    uint64_t dpdk_send_end_tick;
} mtcp_ticks;

#endif
