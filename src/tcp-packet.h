#ifndef __WDM_AGENT_TCP_PACKET_H__
#define __WDM_AGENT_TCP_PACKET_H__

#include <stdint.h>
#include "list.h"
#include "protocol.h"

#define TCP_FIN_SENT 15
#define TCP_FIN_CONFIRMED 16

typedef struct _skbuff skbuff;
typedef skbuff *skbuffPtr;

struct _skbuff {
    u_char *data;               /**< skbuff data */
    uint32_t len;               /**< skbuff length */
    uint32_t truesize;          /**< skbuff true size */
    uint32_t seq;               /**< skbuff sequence number */
    uint32_t ack;               /**< skbuff ack number */
    char urg;                   /**< skbuff urgency data flag */
    uint32_t urgPtr;            /**< skbuff urgency pointer */
    char psh;                   /**< skbuff push flag */
    char fin;                   /**< skbuff fin flag */
    listHead node;              /**< skbuff list node */
};

typedef struct _halfStream halfStream;
typedef halfStream *halfStreamPtr;

struct _halfStream {
    int state;                  /**< half stream state */
    u_char *rcvBuf;             /**< half stream receive buffer */
    int bufSize;                /**< half stream receive buffer size */
    int offset;                 /**< half stream read offset */
    int count;                  /**< half stream total data received */
    int countNew;               /**< half stream new data received */
    u_int seq;                  /**< half stream send sequence number */
    u_int ackSeq;               /**< half stream ack sequence number */
    u_int firstDataSeq;         /**< half stream first data send sequence number */
    int urgCount;               /**< half stream urg data received */
    int urgCountNew;            /**< half stream new urg data count received */
    u_char urgData;             /**< half stream new urg data received */
    u_char urgSeen;             /**< half stream has new urg data flag */
    u_int urgPtr;               /**< half stream urg data pointer */
    u_short window;             /**< half stream current window size */
    u_char tsOn;                /**< half stream timestamp options on flag */
    u_char wscaleOn;            /**< half stream window scale options on flag */
    u_int currTs;               /**< half stream current timestamp */
    u_int wscale;               /**< half stream window scale size */
    listHead head;              /**< half stream skbuff list head */
    int rmemAlloc;              /**< half stream memory allocated for skbuff */
};

typedef struct _tuple4 tuple4;
typedef tuple4 *tuple4Ptr;

struct _tuple4 {
    struct in_addr saddr;       /**< source ip */
    uint16_t source;            /**< source tcp port */
    struct in_addr daddr;       /**< dest ip */
    uint16_t dest;              /**< dest tcp port */
};

/* Tcp stream state */
typedef enum {
    STREAM_INIT,
    STREAM_JUST_EST,
    STREAM_DATA,
    STREAM_RESET,
    STREAM_CLOSING,
    STREAM_TIMED_OUT,
    STREAM_CLOSE
} streamState;

typedef struct _tcpStream tcpStream;
typedef tcpStream *tcpStreamPtr;

/* Tcp stream */
struct _tcpStream {
    protoType proto;            /**< Service protocol type */
    protoParserPtr parser;      /**< Protocol parser */
    tuple4 addr;                /**< Tcp stream 4-tuple address */
    streamState state;          /**< Tcp stream state */
    halfStream client;          /**< Tcp stream client halfStream */
    halfStream server;          /**< Tcp stream server halfStream */
    uint64_t synTime;           /**< First syn timestamp */
    uint64_t retryTime;         /**< Last retry timestamp */
    uint64_t retryNum;          /**< Retry counts */
    uint64_t synAckTime;        /**< Syn/ack timestamp of three handshake */
    uint64_t dupSynAcks;        /**< Duplicate syn/ack Packets */
    uint64_t estbTime;          /**< Tcp connection success timestamp */
    uint8_t connectSuccess;     /**< Tcp connect success flag */
    uint64_t pktsRetransmit;    /**< Packets retransmit */
    uint64_t pktsOutOfOrder;    /**< Packets out of order */
    void *sessionDetail;        /**< Appliction session detail */
    uint64_t closeTime;         /**< Tcp close time */
    int inClosingTimeout;       /**< In closing timeout list */
    listHead node;              /**< Tcp stream list node */
};

typedef struct _tcpTimeout tcpTimeout;
typedef tcpTimeout *tcpTimeoutPtr;

/* Tcp closing timeout */
struct _tcpTimeout {
    tcpStreamPtr stream;        /**< tcp stream to close */
    uint64_t timeout;           /**< tcp stream close timeout */
    listHead node;              /**< tcp stream timeout list node */
};

/* Tcp state for tcp breakdown */
typedef enum {
    TCP_CONNECTED = 0,                  /**< Tcp connection connected */
    TCP_DATA_EXCHANGING,                /**< Tcp connection data exchanging */
    TCP_CLOSED,                         /**< Tcp connection closed */
    TCP_RESET_TYPE1,                    /**< Tcp connection reset type1 (from client and before connected) */
    TCP_RESET_TYPE2,                    /**< Tcp connection reset type2 (from server and before connected) */
    TCP_RESET_TYPE3,                    /**< Tcp connection reset type3 (from client and after connected) */
    TCP_RESET_TYPE4                     /**< Tcp connection reset type4 (from server and after connected) */
} tcpState;

typedef struct _tcpBreakdown tcpBreakdown;
typedef tcpBreakdown *tcpBreakdownPtr;

struct _tcpBreakdown {
    uint64_t bkdId;             /**< Global breakdown id */
    uint64_t timestamp;         /**< Timestamp in seconds */
    uint16_t proto;             /**< Tcp application level protocol type */
    struct in_addr srcIp;       /**< Source ip */
    uint16_t srcPort;           /**< Source port */
    struct in_addr svcIp;       /**< Service ip */
    uint16_t svcPort;           /**< Service port */
    uint64_t tcpConnId;         /**< Global tcp connection id */
    uint64_t retries;           /**< Tcp retries */
    uint64_t retriesLatency;    /**< Tcp retries latency in milliseconds */
    uint64_t dupSynAcks;        /**< Tcp duplicate syn/ack packages */
    uint64_t rtt;               /**< Tcp round trip latency */
    uint8_t state;              /**< Tcp state */
    uint64_t connLatency;       /**< Tcp connection latency in milliseconds */
    uint64_t totalPkts;         /**< Tcp total packets */
    uint64_t tinyPkts;          /**< Tcp tiny packets */
    uint64_t pawsPkts;          /**< Tcp PAWS (Protect Against Wrapped Sequence numbers) packets */
    uint64_t retransmittedPkts; /**< Tcp retransmitted packets */
    uint64_t outOfOrderPkts;    /**< Tcp out of order packets */
    uint64_t zeroWindows;       /**< Tcp zero windows */
    uint64_t dupAcks;           /**< Tcp duplicate acks */
    uint64_t mss;               /**< Tcp mss (maxium segment size) */
    void *sessionBreakdown;     /**< Application level session breakdown */
};

/* Common session breakdown json key definitions */
#define COMMON_SKBD_BREAKDOWN_ID                 "breakdown_id"
#define COMMON_SKBD_TIMESTAMP                    "timestamp"
#define COMMON_SKBD_PROTOCOL                     "protocol"
#define COMMON_SKBD_SOURCE_IP                    "source_ip"
#define COMMON_SKBD_SOURCE_PORT                  "source_port"
#define COMMON_SKBD_SERVICE_IP                   "service_ip"
#define COMMON_SKBD_SERVICE_PORT                 "service_port"
#define COMMON_SKBD_TCP_CONNECTION_ID            "tcp_connection_id"
#define COMMON_SKBD_TCP_RETRIES                  "tcp_retries"
#define COMMON_SKBD_TCP_RETRIES_LATENCY          "tcp_retries_latency"
#define COMMON_SKBD_TCP_DUPLICATE_SYNACKS        "tcp_duplicate_synacks"
#define COMMON_SKBD_TCP_RTT                      "tcp_rtt"
#define COMMON_SKBD_TCP_STATE                    "tcp_state"
#define COMMON_SKBD_TCP_CONNECTION_LATENCY       "tcp_connection_latency"
#define COMMON_SKBD_TCP_TOTAL_PACKETS            "tcp_total_packets"
#define COMMON_SKBD_TCP_TINY_PACKETS             "tcp_tiny_packets"
#define COMMON_SKBD_TCP_PAWS_PACKETS             "tcp_paws_packets"
#define COMMON_SKBD_TCP_RETRANSMITTED_PACKETS    "tcp_retransmitted_packets"
#define COMMON_SKBD_TCP_OUT_OF_ORDER_PACKETS     "tcp_out_of_order_packets"
#define COMMON_SKBD_TCP_ZERO_WINDOWS             "tcp_zero_windows"
#define COMMON_SKBD_TCP_DUPLICATE_ACKS           "tcp_duplicate_acks"
#define COMMON_SKBD_TCP_MSS                      "tcp_mss"

/* Tcp session breakdown callback */
typedef void * (*publishTcpBreakdownCB) (const char *tcpBreakdown, void *args);

/*========================Interfaces definition============================*/
void
tcpProcess (u_char *data, int skbLen, timeValPtr tm);
int
initTcp (publishTcpBreakdownCB publishTcpBreakdown, void *args);
void
destroyTcp (void);
/*=======================Interfaces definition end=========================*/

#endif /* __WDM_AGENT_TCP_PACKET_H__ */
