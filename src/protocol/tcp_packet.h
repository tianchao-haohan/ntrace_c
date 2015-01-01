#ifndef __AGENT_TCP_PACKET_H__
#define __AGENT_TCP_PACKET_H__

#include <netinet/ip.h>
#include "util.h"
#include "list.h"
#include "protocol.h"

#define TCP_FIN_SENT 15
#define TCP_FIN_CONFIRMED 16

typedef struct _skbuff skbuff;
typedef skbuff *skbuffPtr;

struct _skbuff {
    u_char *data;                       /**< Skbuff data */
    u_int len;                          /**< Skbuff length */
    u_int seq;                          /**< Skbuff sequence number */
    u_int ack;                          /**< Skbuff ack number */
    u_char urg;                         /**< Skbuff urgency data flag */
    u_short urgPtr;                     /**< Skbuff urgency pointer */
    u_char psh;                         /**< Skbuff push flag */
    u_char fin;                         /**< Skbuff fin flag */
    listHead node;                      /**< Skbuff list node */
};

typedef struct _halfStream halfStream;
typedef halfStream *halfStreamPtr;

struct _halfStream {
    u_int state;                        /**< Half stream state */
    u_char *rcvBuf;                     /**< Half stream receive buffer */
    u_int bufSize;                      /**< Half stream receive buffer size */
    u_int offset;                       /**< Half stream read offset */
    u_int count;                        /**< Half stream total data received */
    u_int countNew;                     /**< Half stream new data received */
    u_int seq;                          /**< Half stream send sequence number */
    u_int ackSeq;                       /**< Half stream ack sequence number */
    u_int firstDataSeq;                 /**< Half stream first data send sequence number */
    u_int urgCount;                     /**< Half stream urg data received */
    u_int urgCountNew;                  /**< Half stream new urg data count received */
    u_char urgData;                     /**< Half stream new urg data received */
    u_char urgSeen;                     /**< Half stream has new urg data flag */
    u_short urgPtr;                     /**< Half stream urg data pointer */
    u_short window;                     /**< Half stream current window size */
    boolean tsOn;                          /**< Half stream timestamp options on flag */
    boolean wscaleOn;                      /**< Half stream window scale options on flag */
    u_int currTs;                       /**< Half stream current timestamp */
    u_short wscale;                     /**< Half stream window scale size */
    u_short mss;                        /**< Half stream MSS (Maxium Segment Size) */
    listHead head;                      /**< Half stream skbuff list head */
    u_int rmemAlloc;                    /**< Half stream memory allocated for skbuff */
};

typedef struct _tuple4 tuple4;
typedef tuple4 *tuple4Ptr;

struct _tuple4 {
    struct in_addr saddr;               /**< Source ip */
    u_short source;                     /**< Source tcp port */
    struct in_addr daddr;               /**< Dest ip */
    u_short dest;                       /**< Dest tcp port */
};

/* Tcp stream state */
typedef enum {
    STREAM_INIT,
    STREAM_CONNECTED,
    STREAM_DATA_EXCHANGING,
    STREAM_CLOSING,
    STREAM_TIME_OUT,
    STREAM_CLOSED,
    STREAM_RESET_TYPE1,                 /**< Tcp connection reset type1 (from client and before connected) */
    STREAM_RESET_TYPE2,                 /**< Tcp connection reset type2 (from server and before connected) */
    STREAM_RESET_TYPE3,                 /**< Tcp connection reset type3 (from client and after connected) */ 
    STREAM_RESET_TYPE4,                 /**< Tcp connection reset type4 (from server and after connected) */ 
} tcpStreamState;

typedef struct _tcpStream tcpStream;
typedef tcpStream *tcpStreamPtr;

/* Tcp stream */
struct _tcpStream {
    protoType proto;                    /**< Service protocol type */
    protoParserPtr parser;              /**< Protocol parser */
    tuple4 addr;                        /**< Tcp stream 4-tuple address */
    u_long_long connId;                 /**< Global tcp connection id */
    tcpStreamState state;               /**< Tcp stream state */
    halfStream client;                  /**< Tcp stream client halfStream */
    halfStream server;                  /**< Tcp stream server halfStream */
    u_long_long synTime;                /**< Syn timestamp of three handshake */
    u_int retries;                      /**< Retries count */
    u_long_long retriesTime;            /**< The last retry timestamp */
    u_int dupSynAcks;                   /**< Duplicate syn/acks of three handshake */
    u_long_long synAckTime;             /**< Syn/ack timestamp of three handshake */
    u_long_long estbTime;               /**<  timestamp */
    u_int mss;                          /**< Tcp MSS */
    u_int totalPkts;                    /**< Tcp total packets */
    u_int tinyPkts;                     /**< Tcp tiny packets */
    u_int pawsPkts;                     /**< Tcp PAWS packets */
    u_int retransmittedPkts;            /**< Tcp retransmitted packets */
    u_int outOfOrderPkts;               /**< Tcp out of order packets */
    u_int zeroWindows;                  /**< Tcp zero windows */
    u_int dupAcks;                      /**< Tcp duplicate acks */
    void *sessionDetail;                /**< Appliction session detail */
    boolean inClosingTimeout;           /**< In closing timeout list flag */
    u_long_long closeTime;              /**< Tcp close time */
    listHead node;                      /**< Tcp stream list node */
};

typedef struct _tcpStreamTimeout tcpStreamTimeout;
typedef tcpStreamTimeout *tcpStreamTimeoutPtr;

/* Tcp closing timeout */
struct _tcpStreamTimeout {
    tcpStreamPtr stream;                /**< Tcp stream to close */
    u_long_long timeout;                /**< Tcp stream timeout to close */
    listHead node;                      /**< Tcp stream timeout list node */
};

/* Tcp state for tcp breakdown */
typedef enum {
    TCP_BREAKDOWN_CONNECTED = 0,        /**< Tcp connection connected */
    TCP_BREAKDOWN_DATA_EXCHANGING,      /**< Tcp connection data exchanging */
    TCP_BREAKDOWN_CLOSED,               /**< Tcp connection closed */
    TCP_BREAKDOWN_RESET_TYPE1,          /**< Tcp connection reset type1 (from client and before connected) */
    TCP_BREAKDOWN_RESET_TYPE2,          /**< Tcp connection reset type2 (from server and before connected) */
    TCP_BREAKDOWN_RESET_TYPE3,          /**< Tcp connection reset type3 (from client and after connected) */
    TCP_BREAKDOWN_RESET_TYPE4           /**< Tcp connection reset type4 (from server and after connected) */
} tcpBreakdownState;

typedef struct _tcpBreakdown tcpBreakdown;
typedef tcpBreakdown *tcpBreakdownPtr;

struct _tcpBreakdown {
    u_long_long bkdId;                  /**< Global breakdown id */
    u_long_long timestamp;              /**< Timestamp in seconds */
    protoType proto;                    /**< Tcp application level protocol type */
    struct in_addr srcIp;               /**< Source ip */
    u_short srcPort;                    /**< Source port */
    struct in_addr svcIp;               /**< Service ip */
    u_short svcPort;                    /**< Service port */
    u_long_long connId;                 /**< Global tcp connection id */
    tcpBreakdownState state;            /**< Tcp state */
    u_int retries;                      /**< Tcp retries */
    u_int retriesLatency;               /**< Tcp retries latency in milliseconds */
    u_int dupSynAcks;                   /**< Tcp duplicate syn/ack packages */
    u_int mss;                          /**< Tcp mss (maxium segment size) */
    u_int connLatency;                  /**< Tcp connection latency in milliseconds */
    u_int totalPkts;                    /**< Tcp total packets */
    u_int tinyPkts;                     /**< Tcp tiny packets */
    u_int pawsPkts;                     /**< Tcp PAWS (Protect Against Wrapped Sequence numbers) packets */
    u_int retransmittedPkts;            /**< Tcp retransmitted packets */
    u_int outOfOrderPkts;               /**< Tcp out of order packets */
    u_int zeroWindows;                  /**< Tcp zero windows */
    u_int dupAcks;                      /**< Tcp duplicate acks */
    void *sessionBreakdown;             /**< Application level session breakdown */
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
#define COMMON_SKBD_TCP_STATE                    "tcp_state"
#define COMMON_SKBD_TCP_RETRIES                  "tcp_retries"
#define COMMON_SKBD_TCP_RETRIES_LATENCY          "tcp_retries_latency"
#define COMMON_SKBD_TCP_DUPLICATE_SYNACKS        "tcp_duplicate_synacks"
#define COMMON_SKBD_TCP_MSS                      "tcp_mss"
#define COMMON_SKBD_TCP_CONNECTION_LATENCY       "tcp_connection_latency"
#define COMMON_SKBD_TCP_TOTAL_PACKETS            "tcp_total_packets"
#define COMMON_SKBD_TCP_TINY_PACKETS             "tcp_tiny_packets"
#define COMMON_SKBD_TCP_PAWS_PACKETS             "tcp_paws_packets"
#define COMMON_SKBD_TCP_RETRANSMITTED_PACKETS    "tcp_retransmitted_packets"
#define COMMON_SKBD_TCP_OUT_OF_ORDER_PACKETS     "tcp_out_of_order_packets"
#define COMMON_SKBD_TCP_ZERO_WINDOWS             "tcp_zero_windows"
#define COMMON_SKBD_TCP_DUPLICATE_ACKS           "tcp_duplicate_acks"

/* Tcp session breakdown callback */
typedef void (*publishSessionBreakdownCB) (const char *sessionBreakdown, void *args);

/*========================Interfaces definition============================*/
void
tcpProcess (struct ip *iph, timeValPtr tm);
int
initTcp (publishSessionBreakdownCB callback, void *args);
void
destroyTcp (void);
/*=======================Interfaces definition end=========================*/

#endif /* __AGENT_TCP_PACKET_H__ */
