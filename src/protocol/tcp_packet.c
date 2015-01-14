#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <jansson.h>
#include "config.h"
#include "util.h"
#include "list.h"
#include "hash.h"
#include "logger.h"
#include "atomic.h"
#include "checksum.h"
#include "app_service_manager.h"
#include "tcp_options.h"
#include "tcp_packet.h"
#include "proto_analyzer.h"

/* Default tcp stream closing timeout 30 seconds */
#define DEFAULT_TCP_STREAM_CLOSING_TIMEOUT 30
/* Default tcp stream hash table size */
#define DEFAULT_TCP_STREAM_HASH_TABLE_SIZE 65535
/* Tcp expect sequence */
#define EXP_SEQ (snd->firstDataSeq + rcv->count + rcv->urgCount)
/* Tcp stream hash key format string */
#define TCP_STREAM_HASH_KEY_FORMAT "%s:%u:%s:%u"

/* Global tcp connection id */
static u_long_long tcpConnectionId = 0;
/* Tcp connection id spin lock */
static pthread_spinlock_t tcpConnectionIdLock;
/* Global tcp breakdown id */
static u_long_long tcpBreakdownId = 0;
/* Tcp breakdown id lock */
static pthread_spinlock_t tcpBreakdownIdLock;
/* Tcp context init once control */
static pthread_once_t tcpInitOnceControl = PTHREAD_ONCE_INIT;
/* Tcp context destroy once control */
static pthread_once_t tcpDestroyOnceControl = PTHREAD_ONCE_INIT;

/* Debug statistic data */
#ifndef NDEBUG
static u_int tcpStreamsAlloc = 0;
static u_int tcpStreamsFree = 0;
#endif

/* Tcp stream list */
static __thread listHead tcpStreamList;
/* Tcp stream timeout list */
static __thread listHead tcpStreamTimoutList;
/* Tcp stream hash table */
static __thread hashTablePtr tcpStreamHashTable;
/* Tcp breakdown publish callback */
static __thread publishSessionBreakdownCB publishSessionBreakdownFunc;
/* Tcp breakdown publish callback args */
static __thread void *publishSessionBreakdownArgs;

static inline boolean
before (u_int seq1, u_int seq2) {
    int ret;

    ret = (int) (seq1 - seq2);
    if (ret < 0)
        return true;
    else
        return false;
}

static inline boolean
after (u_int seq1, u_int seq2) {
    int ret;

    ret = (int) (seq1 - seq2);
    if (ret > 0)
        return true;
    else
        return false;
}

static inline u_long_long
getTcpConnectionId (void) {
    u_long_long connId;

    pthread_spin_lock (&tcpConnectionIdLock);
    connId = tcpConnectionId;
    tcpConnectionId ++;
    pthread_spin_unlock (&tcpConnectionIdLock);

    return connId;
}

static inline u_long_long
getTcpBreakdownId (void) {
    u_long_long bkdId;

    pthread_spin_lock (&tcpConnectionIdLock);
    bkdId = tcpBreakdownId;
    tcpBreakdownId ++;
    pthread_spin_unlock (&tcpConnectionIdLock);

    return bkdId;
}

/*
 * @brief Add tcp stream to global tcp stream timeout list
 *
 * @param stream tcp stream to add
 * @param tm tcp stream closing time
 */
static void
addTcpStreamToClosingTimeoutList (tcpStreamPtr stream, timeValPtr tm) {
    tcpStreamTimeoutPtr tst;

    /* If already added, return directly */
    if (stream->inClosingTimeout)
        return;

    tst = (tcpStreamTimeoutPtr) malloc (sizeof (tcpStreamTimeout));
    if (tst == NULL) {
        LOGE ("Alloc tcp timeout error: %s.\n", strerror (errno));
        return;
    }

    stream->inClosingTimeout = true;
    tst->stream = stream;
    tst->timeout = tm->tvSec + DEFAULT_TCP_STREAM_CLOSING_TIMEOUT;
    listAddTail (&tst->node, &tcpStreamTimoutList);
}

/* Delete tcp stream from global tcp stream timeout list */
static void
delTcpStreamFromClosingTimeoutList (tcpStreamPtr stream) {
    tcpStreamTimeoutPtr pos, tmp;

    if (!stream->inClosingTimeout)
        return;

    listForEachEntrySafe (pos, tmp, &tcpStreamTimoutList, node) {
        if (pos->stream == stream) {
            listDel (&pos->node);
            free (pos);
            return;
        }
    }
}

/*
 * @brief Lookup tcp stream from global tcp stream hash
 *        table
 * @param addr tcp stream 4 tuple address
 *
 * @return Tcp stream if success else NULL
 */
static tcpStreamPtr
lookupTcpStreamFromHash (tuple4Ptr addr) {
    char key [64];

    snprintf (key, sizeof (key), TCP_STREAM_HASH_KEY_FORMAT,
             inet_ntoa (addr->saddr), addr->source,
             inet_ntoa (addr->daddr), addr->dest);
    return (tcpStreamPtr) hashLookup (tcpStreamHashTable, key);
}

/*
 * @brief Add tcp stream to global hash table
 *
 * @param stream tcp stream to add
 * @param freeFun tcp stream free function
 *
 * @return 0 if success else -1
 */
static int
addTcpStreamToHash (tcpStreamPtr stream, hashItemFreeCB freeFun) {
    int ret;
    tuple4Ptr addr;
    char key [64];

    addr = &stream->addr;
    snprintf (key, sizeof (key), TCP_STREAM_HASH_KEY_FORMAT,
             inet_ntoa (addr->saddr), addr->source,
             inet_ntoa (addr->daddr), addr->dest);
    ret = hashInsert (tcpStreamHashTable, key, stream, freeFun);
    if (ret < 0) {
        LOGE ("Insert stream to hash table error.\n");
        return -1;
    } else
        return 0;
}

/*
 * @brief Remove tcp stream from hash table
 *
 * @param stream tcp stream to remove
 */
static void
delTcpStreamFromHash (tcpStreamPtr stream) {
    int ret;
    tuple4Ptr addr;
    char key [64];

    addr = &stream->addr;
    snprintf (key, sizeof (key), TCP_STREAM_HASH_KEY_FORMAT,
             inet_ntoa (addr->saddr), addr->source,
             inet_ntoa (addr->daddr), addr->dest);
    ret = hashDel (tcpStreamHashTable, key);
    if (ret < 0)
        LOGE ("Delete stream from hash table error.\n");
    else {
#ifndef NDEBUG
        LOGD ("tcpStreamsAlloc: %u<------->tcpStreamsFree: %u\n",
              ATOMIC_ADD_AND_FETCH (&tcpStreamsAlloc, 0), ATOMIC_ADD_AND_FETCH (&tcpStreamsFree, 1));
#endif
    }
}

/*
 * @brief Find tcp stream from global hash table
 *
 * @param tcph tcp header
 * @param iph ip header
 * @param direction return stream direction
 *
 * @return Tcp stream if success else NULL
 */
static tcpStreamPtr
findTcpStream (struct tcphdr *tcph, struct ip * iph, streamDirection *direction) {
    tuple4 addr, reversed;
    tcpStreamPtr stream;

    addr.saddr = iph->ip_src;
    addr.source = ntohs (tcph->source);
    addr.daddr = iph->ip_dst;
    addr.dest = ntohs (tcph->dest);
    stream = lookupTcpStreamFromHash (&addr);
    if (stream) {
        *direction = STREAM_FROM_CLIENT;
        return stream;
    }

    reversed.saddr = iph->ip_dst;
    reversed.source = ntohs (tcph->dest);
    reversed.daddr = iph->ip_src;
    reversed.dest = ntohs (tcph->source);
    stream = lookupTcpStreamFromHash (&reversed);
    if (stream) {
        *direction = STREAM_FROM_SERVER;
        return stream;
    }

    return NULL;
}

/* Create a new tcpStream and init */
static tcpStreamPtr
newTcpStream (protoAnalyzerPtr analyzer) {
    tcpStreamPtr stream;

    stream = (tcpStreamPtr) malloc (sizeof (tcpStream));
    if (stream == NULL)
        return NULL;

    stream->proto = analyzer->proto;
    stream->analyzer = analyzer;
    /* Init 4-tuple address */
    stream->addr.saddr.s_addr = 0;
    stream->addr.source = 0;
    stream->addr.daddr.s_addr = 0;
    stream->addr.dest = 0;
    stream->connId = getTcpConnectionId ();
    stream->state = STREAM_INIT;
    /* Init client halfStream */
    stream->client.state = TCP_CLOSE;
    stream->client.rcvBuf = NULL;
    stream->client.bufSize = 0;
    stream->client.offset = 0;
    stream->client.count = 0;
    stream->client.countNew = 0;
    stream->client.seq = 0;
    stream->client.ackSeq = 0;
    stream->client.firstDataSeq = 0;
    stream->client.urgData = 0;
    stream->client.urgCount = 0;
    stream->client.urgCountNew = 0;
    stream->client.urgSeen = 0;
    stream->client.urgPtr = 0;
    stream->client.window = 0;
    stream->client.tsOn = false;
    stream->client.wscaleOn = false;
    stream->client.currTs = 0;
    stream->client.wscale = 0;
    stream->client.mss = 0;
    initListHead (&stream->client.head);
    stream->client.rmemAlloc = 0;
    /* Init client halfStream end */
    /* Init server halfStream */
    stream->server.state = TCP_CLOSE;
    stream->server.rcvBuf = NULL;
    stream->server.bufSize = 0;
    stream->server.offset = 0;
    stream->server.count = 0;
    stream->server.countNew = 0;
    stream->server.seq = 0;
    stream->server.ackSeq = 0;
    stream->server.firstDataSeq = 0;
    stream->server.urgData = 0;
    stream->server.urgCount = 0;
    stream->server.urgCountNew = 0;
    stream->server.urgSeen = 0;
    stream->server.urgPtr = 0;
    stream->server.window = 0;
    stream->server.tsOn = false;
    stream->server.wscaleOn = false;
    stream->server.currTs = 0;
    stream->server.wscale = 0;
    stream->server.mss = 0;
    initListHead (&stream->server.head);
    stream->server.rmemAlloc = 0;
    /* Init server halfStream end */
    /* Init tcp session detail */
    stream->synTime = 0;
    stream->retries = 0;
    stream->retriesTime = 0;
    stream->dupSynAcks = 0;
    stream->synAckTime = 0;
    stream->estbTime = 0;
    stream->mss = 0;
    stream->totalPkts = 0;
    stream->tinyPkts = 0;
    stream->pawsPkts = 0;
    stream->retransmittedPkts = 0;
    stream->outOfOrderPkts = 0;
    stream->zeroWindows = 0;
    stream->dupAcks = 0;
    /* Init application service session detail */
    stream->sessionDetail = (*stream->analyzer->newSessionDetail) ();
    if (stream->sessionDetail == NULL) {
        free (stream);
        return NULL;
    }
    stream->inClosingTimeout = false;
    stream->closeTime = 0;
    initListHead (&stream->node);

    return stream;
}

/* Tcp stream free function */
static void
freeTcpStream (void *data) {
    skbuffPtr pos, tmp;
    tcpStreamPtr stream = (tcpStreamPtr) data;

    /* Delete stream from global tcp stream list */
    listDel (&stream->node);
    /* Delete stream from closing timeout list */
    delTcpStreamFromClosingTimeoutList (stream);

    /* Free client halfStream */
    listForEachEntrySafe (pos, tmp, &stream->client.head, node) {
        listDel (&pos->node);
        free (pos->data);
        free (pos);
    }
    free (stream->client.rcvBuf);
    /* Free client halfStream end */

    /* Free server halfStream */
    listForEachEntrySafe (pos, tmp, &stream->server.head, node) {
        listDel (&pos->node);
        free (pos->data);
        free (pos);
    }
    free (stream->server.rcvBuf);
    /* Free server halfStream end */

    /* Free session detail */
    (*stream->analyzer->freeSessionDetail) (stream->sessionDetail);
    free (data);
}

/*
 * @brief Alloc new tcp stream and add it to global hash table
 *
 * @param tcph tcp header for current packet
 * @param iph ip header for current packet
 * @param tm timestamp for current packet
 *
 * @return TcpStream if success else NULL
 */
static tcpStreamPtr
addNewTcpStream (struct tcphdr *tcph, struct ip *iph, timeValPtr tm) {
    int ret;
    char key [64];
    protoAnalyzerPtr analyzer;
    tcpStreamPtr stream, tmp;

    snprintf (key, sizeof (key), "%s:%d", inet_ntoa (iph->ip_dst), ntohs (tcph->dest));
    analyzer = getAppServiceProtoAnalyzer (key);
    if (analyzer == NULL) {
        LOGD ("Appliction service (%s:%d) has not been registered.\n",
              inet_ntoa (iph->ip_dst), ntohs (tcph->dest));
        return NULL;
    }

    stream = newTcpStream (analyzer);
    if (stream == NULL) {
        LOGE ("Create new tcpStream error.\n");
        return NULL;
    }
    
    /* Set stream 4-tuple address */
    stream->addr.saddr = iph->ip_src;
    stream->addr.source = ntohs (tcph->source);
    stream->addr.daddr = iph->ip_dst;
    stream->addr.dest = ntohs (tcph->dest);
    /* Set client halfStream */
    stream->client.state = TCP_SYN_SENT;
    stream->client.seq = ntohl (tcph->seq) + 1;
    stream->client.firstDataSeq = stream->client.seq;
    stream->client.window = ntohs (tcph->window);
    stream->client.tsOn = getTimeStampOption (tcph, &stream->client.currTs);
    stream->client.wscaleOn = getTcpWindowScaleOption (tcph, &stream->client.wscale);
    if (!stream->client.wscaleOn)
        stream->client.wscale = 1;
    if (!getTcpMssOption (tcph, &stream->client.mss))
        LOGW ("Tcp MSS from client is null.\n");
    stream->synTime = timeVal2MilliSecond (tm);
    stream->retriesTime = timeVal2MilliSecond (tm);
    stream->totalPkts++;
    if (!stream->client.window)
        stream->zeroWindows++;

    /* Check the count of tcp streams. If the count of tcp streams exceed eighty
     * percent of tcpStreamHashTable limit size then remove the oldest tcp stream
     * from global tcp stream list.
     */
    if (hashSize (tcpStreamHashTable) >= (hashCapacityLimit (tcpStreamHashTable) * 0.8)) {
        listFirstEntry (tmp, &tcpStreamList, node);
        delTcpStreamFromHash (tmp);
    }
    /* Add to global tcp stream list */
    listAddTail (&stream->node, &tcpStreamList);
    /* Add to global tcp stream hash table */
    ret = addTcpStreamToHash (stream, freeTcpStream);
    if (ret < 0) {
        LOGE ("Add tcp stream to stream hash table error.\n");
        return NULL;
    } else {
#ifndef NDEBUG
        ATOMIC_INC (&tcpStreamsAlloc);
#endif
        return stream;
    }
}

static char *
tcpBreakdown2Json (tcpStreamPtr stream, tcpBreakdownPtr tbd) {
    char *out;
    json_t *root;

    root = json_object ();
    if (root == NULL) {
        LOGE ("Create json object error.\n");
        return NULL;
    }
    /* Tcp breakdown id */
    json_object_set_new (root, COMMON_SKBD_BREAKDOWN_ID, json_integer (tbd->bkdId));
    /* Tcp breakdown timestamp */
    json_object_set_new (root, COMMON_SKBD_TIMESTAMP, json_integer (tbd->timestamp));
    /* Tcp application layer protocol */
    json_object_set_new (root, COMMON_SKBD_PROTOCOL, json_string (tbd->proto));
    /* Tcp source ip */
    json_object_set_new (root, COMMON_SKBD_SOURCE_IP, json_string (inet_ntoa (tbd->srcIp)));
    /* Tcp source port */
    json_object_set_new (root, COMMON_SKBD_SOURCE_PORT, json_integer (tbd->srcPort));
    /* Tcp service ip */
    json_object_set_new (root, COMMON_SKBD_SERVICE_IP, json_string (inet_ntoa (tbd->svcIp)));
    /* Tcp service port */
    json_object_set_new (root, COMMON_SKBD_SERVICE_PORT, json_integer (tbd->svcPort));
    /* Tcp connection id */
    json_object_set_new (root, COMMON_SKBD_TCP_CONNECTION_ID, json_integer (tbd->connId));
    /* Tcp state */
    json_object_set_new (root, COMMON_SKBD_TCP_STATE, json_integer (tbd->state));
    /* Tcp retries */
    json_object_set_new (root, COMMON_SKBD_TCP_RETRIES, json_integer (tbd->retries));
    /* Tcp retries latency */
    json_object_set_new (root, COMMON_SKBD_TCP_RETRIES_LATENCY, json_integer (tbd->retriesLatency));
    /* Tcp duplicate syn/ack packets */
    json_object_set_new (root, COMMON_SKBD_TCP_DUPLICATE_SYNACKS, json_integer (tbd->dupSynAcks));
    /* Tcpp mss */
    json_object_set_new (root, COMMON_SKBD_TCP_MSS, json_integer (tbd->mss));
    /* Tcp connection latency */
    json_object_set_new (root, COMMON_SKBD_TCP_CONNECTION_LATENCY, json_integer (tbd->connLatency));
    /* Tcp total packets */
    json_object_set_new (root, COMMON_SKBD_TCP_TOTAL_PACKETS, json_integer (tbd->totalPkts));
    /* Tcp tiny packets */
    json_object_set_new (root, COMMON_SKBD_TCP_TINY_PACKETS, json_integer (tbd->tinyPkts));
    /* Tcp PAWS packets */
    json_object_set_new (root, COMMON_SKBD_TCP_PAWS_PACKETS, json_integer (tbd->pawsPkts));
    /* Tcp retransmitted packets */
    json_object_set_new (root, COMMON_SKBD_TCP_RETRANSMITTED_PACKETS, json_integer (tbd->retransmittedPkts));
    /* Tcp out of order packets */
    json_object_set_new (root, COMMON_SKBD_TCP_OUT_OF_ORDER_PACKETS, json_integer (tbd->outOfOrderPkts));
    /* Tcp zero windows */
    json_object_set_new (root, COMMON_SKBD_TCP_ZERO_WINDOWS, json_integer (tbd->zeroWindows));
    /* Tcp duplicate acks */
    json_object_set_new (root, COMMON_SKBD_TCP_DUPLICATE_ACKS, json_integer (tbd->dupAcks));

    if ((tbd->state == TCP_BREAKDOWN_DATA_EXCHANGING) ||
        (tbd->state == TCP_BREAKDOWN_RESET_TYPE3) ||
        (tbd->state == TCP_BREAKDOWN_RESET_TYPE4))
        (*stream->analyzer->sessionBreakdown2Json) (root, stream->sessionDetail, tbd->sessionBreakdown);

    out = json_dumps (root, JSON_INDENT (4));
    json_object_clear (root);

    return out;
}

static void
publishSessionBreakdown (tcpStreamPtr stream, timeValPtr tm) {
    int ret;
    tcpBreakdown tbd;
    char *jsonStr = NULL;

    tbd.sessionBreakdown = (*stream->analyzer->newSessionBreakdown) ();
    if (tbd.sessionBreakdown == NULL) {
        LOGE ("Create new sessionBreakdown error.\n");
        return;
    }

    tbd.bkdId = getTcpBreakdownId ();
    tbd.timestamp = tm->tvSec;
    tbd.proto = stream->proto;
    tbd.srcIp = stream->addr.saddr;
    tbd.srcPort = stream->addr.source;
    tbd.svcIp = stream->addr.daddr;
    tbd.svcPort = stream->addr.dest;
    tbd.connId = stream->connId;

    switch (stream->state) {
        case STREAM_CONNECTED:
            tbd.state = TCP_BREAKDOWN_CONNECTED;
            break;

        case STREAM_DATA_EXCHANGING:
        case STREAM_CLOSING:
            tbd.state = TCP_BREAKDOWN_DATA_EXCHANGING;
            break;

        case STREAM_TIME_OUT:
        case STREAM_CLOSED:
            tbd.state = TCP_BREAKDOWN_CLOSED;
            break;

        case STREAM_RESET_TYPE1:
            tbd.state = TCP_BREAKDOWN_RESET_TYPE1;
            break;

        case STREAM_RESET_TYPE2:
            tbd.state = TCP_BREAKDOWN_RESET_TYPE2;
            break;

        case STREAM_RESET_TYPE3:
            tbd.state = TCP_BREAKDOWN_RESET_TYPE3;
            break;

        case STREAM_RESET_TYPE4:
            tbd.state = TCP_BREAKDOWN_RESET_TYPE4;
            break;

        default:
            (*stream->analyzer->freeSessionBreakdown) (tbd.sessionBreakdown);
            LOGE ("Unsupported stream state for breakdown.\n");
            return;
    }

    switch (tbd.state) {
        case TCP_BREAKDOWN_CONNECTED:
            tbd.retries = stream->retries;
            tbd.retriesLatency = stream->retriesTime - stream->synTime;
            tbd.dupSynAcks = stream->dupSynAcks;
            tbd.mss = stream->mss;
            tbd.connLatency = stream->estbTime - stream->retriesTime;
            break;

        default:
            tbd.retries = 0;
            tbd.retriesLatency = 0;
            tbd.dupSynAcks = 0;
            tbd.mss = 0;
            tbd.connLatency = 0;
            break;
    }

    tbd.totalPkts = stream->totalPkts;
    tbd.tinyPkts = stream->tinyPkts;
    tbd.pawsPkts = stream->pawsPkts;
    tbd.retransmittedPkts = stream->retransmittedPkts;
    tbd.outOfOrderPkts = stream->outOfOrderPkts;
    tbd.zeroWindows = stream->zeroWindows;
    tbd.dupAcks = stream->dupAcks;

    /* For TCP_BREAKDOWN_DATA_EXCHANGING, TCP_BREAKDOWN_RESET_TYPE3 and TCP_BREAKDOWN_RESET_TYPE4 breakdown,
     * there is application layer breakdown */
    if ((tbd.state == TCP_BREAKDOWN_DATA_EXCHANGING) ||
        (tbd.state == TCP_BREAKDOWN_RESET_TYPE3) ||
        (tbd.state == TCP_BREAKDOWN_RESET_TYPE4)) {
        ret = (*stream->analyzer->generateSessionBreakdown) (stream->sessionDetail, tbd.sessionBreakdown);
        if (ret < 0) {
            LOGE ("GenerateSessionBreakdown error.\n");
            (*stream->analyzer->freeSessionBreakdown) (tbd.sessionBreakdown);
            return;
        }
    }

    jsonStr = tcpBreakdown2Json (stream, &tbd);
    if (jsonStr == NULL) {
        LOGE ("SessionBreakdown2Json error.\n");
        (*stream->analyzer->freeSessionBreakdown) (tbd.sessionBreakdown);
        return;
    }
    publishSessionBreakdownFunc (jsonStr, publishSessionBreakdownArgs);

    /* Free json string and application layer session breakdown */
    free (jsonStr);
    (*stream->analyzer->freeSessionBreakdown) (tbd.sessionBreakdown);

    /* Reset some statistic fields of tcp stream */
    stream->totalPkts = 0;
    stream->tinyPkts = 0;
    stream->pawsPkts = 0;
    stream->retransmittedPkts = 0;
    stream->outOfOrderPkts = 0;
    stream->zeroWindows = 0;
    stream->dupAcks = 0;
}

/*
 * @brief Check tcp stream timeout list and remove timeout
 *        tcp stream.
 *
 * @param tm timestamp for current packet
 */
static void
checkTcpStreamClosingTimeoutList (timeValPtr tm) {
    tcpStreamTimeoutPtr pos, tmp;

    listForEachEntrySafe (pos, tmp, &tcpStreamTimoutList, node) {
        if (pos->timeout > tm->tvSec)
            return;
        else {
            pos->stream->state = STREAM_TIME_OUT;
            pos->stream->closeTime = timeVal2MilliSecond (tm);
            publishSessionBreakdown (pos->stream, tm);
            delTcpStreamFromHash (pos->stream);
        }
    }
}

/* Tcp connection establishment handler callback */
static void
handleEstb (tcpStreamPtr stream, timeValPtr tm) {
    /* Set tcp state */
    stream->client.state = TCP_ESTABLISHED;
    stream->server.state = TCP_ESTABLISHED;
    stream->state = STREAM_CONNECTED;
    stream->estbTime = timeVal2MilliSecond (tm);
    stream->mss = MIN_NUM (stream->client.mss, stream->server.mss);

    (*stream->analyzer->sessionProcessEstb) (stream->sessionDetail, tm);
    /* Publish tcp connected breakdown */
    publishSessionBreakdown (stream, tm);
}

/* Tcp urgence data handler callback */
static void
handleUrgData (tcpStreamPtr stream, halfStreamPtr snd, u_char urgData, timeValPtr tm) {
    streamDirection direction;

    if (snd == &stream->client)
        direction = STREAM_FROM_CLIENT;
    else
        direction = STREAM_FROM_SERVER;

    (*stream->analyzer->sessionProcessUrgData) (direction, urgData, stream->sessionDetail, tm);
}

/* Tcp data handler callback */
static u_int
handleData (tcpStreamPtr stream, halfStreamPtr snd, u_char *data, u_int dataLen, timeValPtr tm) {
    streamDirection direction;
    u_int parseCount;
    sessionState state = SESSION_ACTIVE;

    if (snd == &stream->client)
        direction = STREAM_FROM_CLIENT;
    else
        direction = STREAM_FROM_SERVER;

    parseCount = (*stream->analyzer->sessionProcessData) (direction, data, dataLen, stream->sessionDetail, tm, &state);
    if (state == SESSION_DONE)
        publishSessionBreakdown (stream, tm);

    return parseCount;
}

/* Tcp reset handler callback */
static void
handleReset (tcpStreamPtr stream, halfStreamPtr snd, timeValPtr tm) {
    streamDirection direction;

    if (snd == &stream->client)
        direction = STREAM_FROM_CLIENT;
    else
        direction = STREAM_FROM_SERVER;

    if (stream->state == STREAM_INIT) {
        if (direction == STREAM_FROM_CLIENT)
            stream->state = STREAM_RESET_TYPE1;
        else
            stream->state = STREAM_RESET_TYPE2;
    } else {
        if (direction == STREAM_FROM_CLIENT)
            stream->state = STREAM_RESET_TYPE3;
        else
            stream->state = STREAM_RESET_TYPE4;
        (*stream->analyzer->sessionProcessReset) (direction, stream->sessionDetail, tm);
    }

    stream->closeTime = timeVal2MilliSecond (tm);
    publishSessionBreakdown (stream, tm);
    delTcpStreamFromHash (stream);
}

/* Tcp fin handler callback */
static void
handleFin (tcpStreamPtr stream, halfStreamPtr snd, timeValPtr tm) {
    streamDirection direction;
    sessionState state = SESSION_ACTIVE;

    if (snd == &stream->client)
        direction = STREAM_FROM_CLIENT;
    else
        direction = STREAM_FROM_SERVER;

    (*stream->analyzer->sessionProcessFin) (direction, stream->sessionDetail, tm, &state);
    if (state == SESSION_DONE)
        publishSessionBreakdown (stream, tm);

    snd->state = TCP_FIN_SENT;
    stream->state = STREAM_CLOSING;
    addTcpStreamToClosingTimeoutList (stream, tm);
}

/* Tcp close handler callback */
static void
handleClose (tcpStreamPtr stream, timeValPtr tm) {
    stream->state = STREAM_CLOSED;
    stream->closeTime = timeVal2MilliSecond (tm);
    publishSessionBreakdown (stream, tm);
    delTcpStreamFromHash (stream);
}

/*
 * @brief Add data to halfStream receive buffer
 *
 * @param rcv halfStream to receive
 * @param data data to add
 * @param dataLen data length to add
 */
static void
add2buf (halfStreamPtr rcv, u_char *data, u_int dataLen) {
    u_int toAlloc;

    if ((rcv->count - rcv->offset + dataLen) > rcv->bufSize) {
        if (rcv->rcvBuf == NULL) {
            if (dataLen < 2048)
                toAlloc = 4096;
            else
                toAlloc = dataLen * 2;
            rcv->rcvBuf = (u_char *) malloc (toAlloc);
            if (rcv->rcvBuf == NULL) {
                LOGE ("Alloc memory for halfStream rcvBuf error: %s.\n", strerror (errno));
                rcv->bufSize = 0;
                return;
            }
            rcv->bufSize = toAlloc;
        } else {
            if (dataLen < rcv->bufSize)
                toAlloc = 2 * rcv->bufSize;
            else
                toAlloc = rcv->bufSize + 2 * dataLen;
            rcv->rcvBuf = (u_char *) realloc (rcv->rcvBuf, toAlloc);
            if (rcv->rcvBuf == NULL) {
                LOGE ("Alloc memory for halfStream rcvBuf error: %s.\n", strerror (errno));
                rcv->bufSize = 0;
                return;
            }
            rcv->bufSize = toAlloc;
        }
    }
    memcpy (rcv->rcvBuf + rcv->count - rcv->offset, data, dataLen);
    rcv->countNew = dataLen;
    rcv->count += dataLen;
}

/*
 * @brief Tcp data defragment, merge data from skbuff to receiver's receive
 *        buffer. If data contains urgData, it needs to update receiver's urg
 *        data and pointer first else merge data directly.
 *
 * @param stream current tcp stream
 * @param snd tcp sender
 * @param rcv tcp receiver
 * @param data data to merge
 * @param dataLen data length
 * @param curSeq current send sequence
 * @param fin fin flag
 * @param urg urg flag
 * @param urgPtr urgPointer
 * @param push push flag
 * @param tm current timestamp
 */
static void
addFromSkb (tcpStreamPtr stream, halfStreamPtr snd, halfStreamPtr rcv, u_char *data, u_int dataLen,
            u_int curSeq, u_char fin, u_char urg, u_short urgPtr, u_char push, timeValPtr tm) {
    u_int parseCount;
    u_int toCopy1, toCopy2;
    u_int lost = EXP_SEQ - curSeq;

    if (urg && after (urgPtr, EXP_SEQ - 1) &&
        (!rcv->urgSeen || after (urgPtr, rcv->urgPtr))) {
        rcv->urgPtr = urgPtr;
        rcv->urgSeen = 1;
    }

    if (rcv->urgSeen && after (rcv->urgPtr + 1, curSeq + lost) &&
        before (rcv->urgPtr, curSeq + dataLen)) {
        toCopy1 = rcv->urgPtr - (curSeq + lost);
        if (toCopy1 > 0) {
            add2buf (rcv, data + lost, toCopy1);
            parseCount = handleData (stream, snd, rcv->rcvBuf, rcv->count - rcv->offset, tm);
            memmove (rcv->rcvBuf, rcv->rcvBuf + parseCount,  rcv->count - rcv->offset - parseCount);
            rcv->offset += parseCount;
            rcv->countNew = 0;
        }
        rcv->urgData = data [rcv->urgPtr - curSeq];
        rcv->urgCountNew = 1;
        handleUrgData (stream, snd, rcv->urgData, tm);
        rcv->urgCountNew = 0;
        rcv->urgSeen = 0;
        rcv->urgCount++;
        toCopy2 = curSeq + dataLen - rcv->urgPtr - 1;
        if (toCopy2 > 0) {
            add2buf (rcv, data + lost + toCopy1 + 1, toCopy2);
            parseCount = handleData (stream, snd, rcv->rcvBuf, rcv->count - rcv->offset, tm);
            memmove (rcv->rcvBuf, rcv->rcvBuf + parseCount,  rcv->count - rcv->offset - parseCount);
            rcv->offset += parseCount;
            rcv->countNew = 0;
        }
    } else {
        if (dataLen - lost > 0) {
            add2buf (rcv, data + lost, dataLen - lost);
            parseCount = handleData (stream, snd, rcv->rcvBuf, rcv->count - rcv->offset, tm);
            memmove (rcv->rcvBuf, rcv->rcvBuf + parseCount,  rcv->count - rcv->offset - parseCount);
            rcv->offset += parseCount;
            rcv->countNew = 0;
        }
    }

    if (fin)
        handleFin (stream, snd, tm);
}

/*
 * @brief Tcp queue process, for expected data merge it to receiver's
 *        receive buffer directly else store it to skbuff and link it
 *        to receiver's skbuff list.
 *
 * @param stream current tcp stream
 * @param tcph tcp header
 * @param snd tcp sender
 * @param rcv tcp receiver
 * @param data data to merge
 * @param dataLen data length
 * @param tm current timestamp
 */
static void
tcpQueue (tcpStreamPtr stream, struct tcphdr *tcph, halfStreamPtr snd,
          halfStreamPtr rcv, u_char *data, u_int dataLen, timeValPtr tm) {
    u_int curSeq;
    skbuffPtr skbuf, prev, tmp;

    curSeq = ntohl (tcph->seq);
    if (!after (curSeq, EXP_SEQ)) {
        /* Accumulate out of order packets */
        if (before (curSeq, EXP_SEQ))
            stream->retransmittedPkts++;

        if (after (curSeq + dataLen + tcph->fin, EXP_SEQ)) {
            /* The packet straddles our window end */
            getTimeStampOption (tcph, &snd->currTs);
            addFromSkb (stream, snd, rcv, (u_char *) data, dataLen, curSeq, tcph->fin,
                        tcph->urg, curSeq + ntohs (tcph->urg_ptr) - 1, tcph->psh, tm);

            listForEachEntrySafe (skbuf, tmp, &rcv->head, node) {
                if (after (skbuf->seq, EXP_SEQ))
                    break;
                listDel (&skbuf->node);
                if (after (skbuf->seq + skbuf->len + skbuf->fin, EXP_SEQ)) {
                    addFromSkb (stream, snd, rcv, skbuf->data, skbuf->len, skbuf->seq, skbuf->fin,
                                skbuf->urg, skbuf->seq + skbuf->urgPtr - 1, skbuf->psh, tm);
                }
                rcv->rmemAlloc -= skbuf->len;
                free (skbuf->data);
                free (skbuf);
            }
        } else
            return;
    } else {
        /* Accumulate out of order packets */
        stream->outOfOrderPkts++;

        /* Alloc new skbuff */
        skbuf = (skbuffPtr) malloc (sizeof (skbuff));
        if (skbuf == NULL) {
            LOGE ("Alloc memory for skbuff error: %s.\n", strerror (errno));
            return;
        }
        memset (skbuf, 0, sizeof (skbuff));
        skbuf->data = (u_char *) malloc (dataLen);
        if (skbuf->data == NULL) {
            LOGE ("Alloc memory for skbuff data error: %s.\n", strerror (errno));
            free (skbuf);
            return;
        }
        skbuf->len = dataLen;
        memcpy (skbuf->data, data, dataLen);
        skbuf->fin = tcph->fin;
        skbuf->seq = curSeq;
        skbuf->urg = tcph->urg;
        skbuf->urgPtr = ntohs (tcph->urg_ptr);
        skbuf->psh = tcph->psh;

        if (skbuf->fin) {
            snd->state = TCP_CLOSING;
            addTcpStreamToClosingTimeoutList (stream, tm);
        }
        rcv->rmemAlloc += skbuf->len;

        listForEachEntryReverseSafe (prev, tmp, &rcv->head, node) {
            if (before (prev->seq, curSeq)) {
                listAdd (&skbuf->node, &prev->node);
                return;
            }
        }
        listAdd (&skbuf->node, &rcv->head);
    }
}

/*
 * @brief Tcp process portal, it will construct tcp connection, tcp data
 *        defragment, generate tcp session breakdown, publish tcp session
 *        breakdown and tcp stream context destroy.
 *
 * @param iph ip packet header
 * @param tm current timestamp
 */
void
tcpProcess (struct ip *iph, timeValPtr tm) {
    u_int ipLen;
    struct tcphdr *tcph;
#if DO_STRICT_CHECK
    u_int tcpLen;
#endif
    u_char *tcpData;
    u_int tcpDataLen;
    u_int tmOption;
    tcpStreamPtr stream;
    halfStreamPtr snd, rcv;
    streamDirection direction;

    ipLen = ntohs (iph->ip_len);
    tcph = (struct tcphdr *) ((char *) iph + iph->ip_hl * 4);
#if DO_STRICT_CHECK
    tcpLen = ipLen - iph->ip_hl * 4;
#endif
    tcpData = (u_char *) tcph + 4 * tcph->doff;
    tcpDataLen = ipLen - (iph->ip_hl * 4) - (tcph->doff * 4);

    tm->tvSec = ntohll (tm->tvSec);
    tm->tvUsec = ntohll (tm->tvUsec);

    /* Tcp stream closing timout check */
    checkTcpStreamClosingTimeoutList (tm);

    if (ipLen < (iph->ip_hl * 4 + sizeof (struct tcphdr))) {
        LOGE ("Invalid tcp packet.\n");
        return;
    }

    if (tcpDataLen < 0) {
        LOGE ("Invalid tcp data length.\n");
        return;
    }

    if (iph->ip_src.s_addr == 0 || iph->ip_dst.s_addr == 0) {
        LOGE ("Invalid ip address.\n");
        return;
    }

#if DO_STRICT_CHECK
    /* Tcp checksum validation */
    if (tcpFastCheckSum ((u_char *) tcph, tcpLen, iph->ip_src.s_addr, iph->ip_dst.s_addr) != 0) {
        LOGE ("Tcp fast checksum error.\n");
        return;
    }
#endif

    stream = findTcpStream (tcph, iph, &direction);
    if (stream == NULL) {
        /* The first packet of tcp three handshake */
        if (tcph->syn && !tcph->ack && !tcph->rst) {
            stream = addNewTcpStream (tcph, iph, tm);
            if (stream == NULL) {
                LOGE ("Create new tcp stream error.\n");
                return;
            }
        }
        return;
    }

    if (direction == STREAM_FROM_CLIENT) {
        snd = &stream->client;
        rcv = &stream->server;
    } else {
        rcv = &stream->client;
        snd = &stream->server;
    }

    /* Accumulate totoal packets */
    stream->totalPkts++;
    /* Tcp window check */
    snd->window = ntohs (tcph->window);
    if (!snd->window)
        stream->zeroWindows++;

    if (tcph->syn) {
        if ((direction == STREAM_FROM_CLIENT) || (stream->client.state != TCP_SYN_SENT) ||
            (stream->server.state != TCP_CLOSE) || !tcph->ack) {
            /* Tcp syn retries */
            if ((direction == STREAM_FROM_CLIENT) && (stream->client.state == TCP_SYN_SENT)) {
                stream->retries++;
                stream->retriesTime = timeVal2MilliSecond (tm);
            } else if ((direction == STREAM_FROM_SERVER) && (stream->server.state == TCP_SYN_RECV)) {
                /* Tcp syn/ack retries */
                stream->dupSynAcks++;
                stream->synAckTime = timeVal2MilliSecond (tm);
                stream->dupAcks++;
            }

            stream->retransmittedPkts++;
            return;
        } else {
            /* The second packet of tcp three handshake */
            if (stream->client.seq != ntohl (tcph->ack_seq)) {
                LOGW ("Error ack sequence number of syn/ack packet.\n");
                return;
            }

            stream->server.state = TCP_SYN_RECV;
            stream->server.seq = ntohl (tcph->seq) + 1;
            stream->server.firstDataSeq = stream->server.seq;
            stream->server.ackSeq = ntohl (tcph->ack_seq);

            if (stream->client.tsOn) {
                stream->server.tsOn = getTimeStampOption (tcph, &stream->server.currTs);
                if (!stream->server.tsOn)
                    stream->client.tsOn = false;
            } else
                stream->server.tsOn = false;

            if (stream->client.wscaleOn) {
                stream->server.wscaleOn = getTcpWindowScaleOption (tcph, &stream->server.wscale);
                if (!stream->server.wscaleOn) {
                    stream->client.wscaleOn = false;
                    stream->client.wscale  = 1;
                    stream->server.wscale = 1;
                }
            } else {
                stream->server.wscaleOn = false;
                stream->server.wscale = 1;
            }

            if (!getTcpMssOption (tcph, &stream->server.mss))
                LOGW ("Tcp MSS from server is null.\n");

            stream->synAckTime = timeVal2MilliSecond (tm);
        }

        return;
    }

    if (tcph->rst) {
        handleReset (stream, snd, tm);
        return;
    }

    /* Filter retransmitted or out of window range packet */
    if (!(!tcpDataLen && (ntohl (tcph->seq) == rcv->ackSeq)) &&
        (before (ntohl (tcph->seq) + tcpDataLen, rcv->ackSeq) ||
         !before (ntohl (tcph->seq), (rcv->ackSeq + rcv->window * rcv->wscale)))) {
        /* Accumulate retransmitted packets */
        if (before (ntohl (tcph->seq) + tcpDataLen, rcv->ackSeq))
            stream->retransmittedPkts++;
        return;
    }

    /* PAWS (Protect Against Wrapped Sequence numbers) check */
    if (rcv->tsOn && getTimeStampOption (tcph, &tmOption) && before (tmOption, snd->currTs)) {
        stream->pawsPkts++;
        return;
    }

    if (tcph->ack) {
        if (direction == STREAM_FROM_CLIENT) {
            /* The last packet of tcp three handshake */
            if (stream->client.state == TCP_SYN_SENT && stream->server.state == TCP_SYN_RECV) {
                if (ntohl (tcph->ack_seq) == stream->server.seq) {
                    handleEstb (stream, tm);
                    stream->state = STREAM_DATA_EXCHANGING;
                } else
                    stream->outOfOrderPkts++;
            }
        }

        if (ntohl (tcph->ack_seq) > snd->ackSeq)
            snd->ackSeq = ntohl (tcph->ack_seq);
        else if (!tcpDataLen) {
            /* For out of order packets, if receiver doesn't receive all packets, it
             * will send a single ack packet to ackownledge the last received successive
             * packet, in that case, client will resend the dropped packet again */
            stream->dupAcks++;
        }

        if (rcv->state == TCP_FIN_SENT)
            rcv->state = TCP_FIN_CONFIRMED;
        if (rcv->state == TCP_FIN_CONFIRMED && snd->state == TCP_FIN_CONFIRMED) {
            handleClose (stream, tm);
            return;
        }
    }

    if (tcpDataLen + tcph->fin > 0) {
        if (tcpDataLen == 1)
            stream->tinyPkts++;
        tcpQueue (stream, tcph, snd, rcv, tcpData, tcpDataLen, tm);
    }
}

static void
initTcpSharedInstance (void) {
    tcpStreamsAlloc = 0;
    tcpStreamsFree = 0;
    tcpBreakdownId = 0;
    pthread_spin_init (&tcpConnectionIdLock, PTHREAD_PROCESS_PRIVATE);
    pthread_spin_init (&tcpBreakdownIdLock, PTHREAD_PROCESS_PRIVATE);
    tcpDestroyOnceControl = PTHREAD_ONCE_INIT;
}

static void
destroyTcpSharedInstance (void) {
    pthread_spin_destroy (&tcpConnectionIdLock);
    pthread_spin_destroy (&tcpBreakdownIdLock);
    tcpInitOnceControl = PTHREAD_ONCE_INIT;
}

/* Init tcp process context */
int
initTcp (publishSessionBreakdownCB callback, void *args) {
    pthread_once (&tcpInitOnceControl, initTcpSharedInstance);
    initListHead (&tcpStreamList);
    initListHead (&tcpStreamTimoutList);
    publishSessionBreakdownFunc = callback;
    publishSessionBreakdownArgs = args;
    tcpStreamHashTable = hashNew (DEFAULT_TCP_STREAM_HASH_TABLE_SIZE);
    if (tcpStreamHashTable == NULL)
        return -1;
    else
        return 0;
}

/* Destroy tcp process context */
void
destroyTcp (void) {
    pthread_once (&tcpDestroyOnceControl, destroyTcpSharedInstance);
    publishSessionBreakdownFunc = NULL;
    publishSessionBreakdownArgs = NULL;
    hashDestroy (tcpStreamHashTable);
    tcpStreamHashTable = NULL;
}