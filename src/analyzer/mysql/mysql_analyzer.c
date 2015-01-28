#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <zlib.h>
#include <pthread.h>
#include <jansson.h>
#include "util.h"
#include "log.h"
#include "mysql_analyzer.h"

#define MATCH(a, b) ((a) == (b) ? true : false)

/* Mysql response packet header */
#define MYSQL_RESPONSE_OK_HEADER 0x00
#define MYSQL_RESPONSE_END_HEADER 0xFE
#define MYSQL_RESPONSE_ERROR_HEADER 0xFF

/* Current timestamp */
static __thread timeValPtr currTime;
/* Current session state */
static __thread sessionState currSessionState;
/* Current mysql shared info */
static __thread mysqlSharedInfoPtr currSharedInfo;
/* Current mysql session detail */
static __thread mysqlSessionDetailPtr currSessionDetail;

/* Mysql state event matrix */
static mysqlEventHandleMap mysqlStateEventMatrix [MYSQL_STATE_COUNT];
/* Mysql proto init once control */
static pthread_once_t mysqlProtoInitOnceControl = PTHREAD_ONCE_INIT;
/* Mysql proto destroy once control */
static pthread_once_t mysqlProtoDestroyOnceControl = PTHREAD_ONCE_INIT;

/* Fixed-Length Integer Types */
#define FLI1(A) ((u_char) (A) [0])

#define FLI2(A) ((u_short) (((u_short) ((u_char) (A) [0])) +        \
                            ((u_short) ((u_char) (A) [1]) << 8)))

#define FLI3(A) ((u_int) (((u_int) ((u_char) (A) [0])) +            \
                          (((u_int) ((u_char) (A) [1])) << 8) +     \
                          (((u_int) ((u_char) (A) [2])) << 16)))

#define FLI4(A) ((u_int) (((u_int) ((u_char) (A) [0])) +            \
                          (((u_int) ((u_char) (A) [1])) << 8) +     \
                          (((u_int) ((u_char) (A) [2])) << 16) +    \
                          (((u_int) ((u_char) (A) [3])) << 24)))

#define FLI6(A) ((u_long_long) (((u_long_long) ((u_char) (A) [0])) +    \
                                (((u_long_long) ((u_char) (A) [1])) << 8) + \
                                (((u_long_long) ((u_char) (A) [2])) << 16) + \
                                (((u_long_long) ((u_char) (A) [3])) << 24) + \
                                (((u_long_long) ((u_char) (A) [3])) << 32) + \
                                (((u_long_long) ((u_char) (A) [3])) << 40)))

#define FLI8(A) ((u_long_long) (((u_long_long) ((u_char) (A) [0])) +    \
                                (((u_long_long) ((u_char) (A) [1])) << 8) + \
                                (((u_long_long) ((u_char) (A) [2])) << 16) + \
                                (((u_long_long) ((u_char) (A) [3])) << 24) + \
                                (((u_long_long) ((u_char) (A) [3])) << 32) + \
                                (((u_long_long) ((u_char) (A) [3])) << 40) + \
                                (((u_long_long) ((u_char) (A) [3])) << 48) + \
                                (((u_long_long) ((u_char) (A) [3])) << 56)))

/* Encoded length integer Type */
static u_long_long
encLenInt (u_char *pkt, u_int *len) {
    u_int prefix;

    prefix = (u_int) *pkt;

    if (prefix < 0xFB) {
        *len = 1;
        return (u_long_long) FLI1 (pkt);
    } else if (prefix == 0xFB) {
        *len = 1;
        return (u_long_long) 0;
    } else if (prefix == 0xFC) {
        *len = 3;
        return (u_long_long) FLI2 (pkt + 1);
    } else if (prefix == 0xFD) {
        *len = 4;
        return (u_long_long) FLI3 (pkt + 1);
    } else if (prefix == 0xFE) {
        *len = 9;
        return (u_long_long) FLI8 (pkt + 1);
    } else {
        *len = 0;
        return 0;
    }
}

char *
getFieldTypeName (mysqlFieldType type) {
    switch (type) {
        case FIELD_TYPE_DECIMAL:
            return "FIELD_TYPE_DECIMAL";

        case FIELD_TYPE_TINY:
            return "FIELD_TYPE_TINY";

        case FIELD_TYPE_SHORT:
            return "FIELD_TYPE_SHORT";

        case FIELD_TYPE_LONG:
            return "FIELD_TYPE_LONG";

        case FIELD_TYPE_FLOAT:
            return "FIELD_TYPE_FLOAT";

        case FIELD_TYPE_DOUBLE:
            return "FIELD_TYPE_DOUBLE";

        case FIELD_TYPE_NULL:
            return "FIELD_TYPE_NULL";

        case FIELD_TYPE_TIMESTAMP:
            return "FIELD_TYPE_TIMESTAMP";

        case FIELD_TYPE_LONGLONG:
            return "FIELD_TYPE_LONGLONG";

        case FIELD_TYPE_INT24:
            return "FIELD_TYPE_INT24";

        case FIELD_TYPE_DATE:
            return "FIELD_TYPE_DATE";

        case FIELD_TYPE_TIME:
            return "FIELD_TYPE_TIME";

        case FIELD_TYPE_DATETIME:
            return "FIELD_TYPE_DATETIME";

        case FIELD_TYPE_YEAR:
            return "FIELD_TYPE_YEAR";

        case FIELD_TYPE_NEWDATE:
            return "FIELD_TYPE_NEWDATE";

        case FIELD_TYPE_VARCHAR:
            return "FIELD_TYPE_VARCHAR";

        case FIELD_TYPE_BIT:
            return "FIELD_TYPE_BIT";

        case FIELD_TYPE_NEWDECIMAL:
            return "FIELD_TYPE_NEWDECIMAL";

        case FIELD_TYPE_ENUM:
            return "FIELD_TYPE_ENUM";

        case FIELD_TYPE_SET:
            return "FIELD_TYPE_SET";

        case FIELD_TYPE_TINY_BLOB:
            return "FIELD_TYPE_TINY_BLOB";

        case FIELD_TYPE_MEDIUM_BLOB:
            return "FIELD_TYPE_MEDIUM_BLOB";

        case FIELD_TYPE_LONG_BLOB:
            return "FIELD_TYPE_LONG_BLOB";

        case FIELD_TYPE_BLOB:
            return "FIELD_TYPE_BLOB";

        case FIELD_TYPE_VAR_STRING:
            return "FIELD_TYPE_VAR_STRING";

        case FIELD_TYPE_STRING:
            return "FIELD_TYPE_STRING";

        case FIELD_TYPE_GEOMETRY:
            return "FIELD_TYPE_GEOMETRY";

        default:
            return "FIELD_TYPE_UNKNOWN";
    }
}

/* =================================Handshake================================= */

static mysqlEventHandleState
pktServerHandshake (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    u_int toCopyLen;
    u_char filler;
    u_int caps;
    u_char charSet;
    u_short statusFlag;
    u_int authPluginDataLen;
    char authPluginData [256] = {0};
    u_char *pkt = payload;

    if ((direction == STREAM_FROM_CLIENT) || currSessionDetail->seqId != 0)
        return EVENT_HANDLE_ERROR;

    LOGD ("Cli <<----------------<< Server packet seqId:%u\n", currSessionDetail->seqId);
    LOGD ("Initial handshake packet\n");
    
    /* Only support v10 protocol */
    if (!MATCH (*pkt, 0x0A)) {
        LOGE ("Only support v10 protocol\n");
        return EVENT_HANDLE_ERROR;
    }

    /* Proto version */
    currSharedInfo->protoVer = (u_int) FLI1 (pkt);
    pkt += 1;

    /* Server version, example:4.1.1 ........ */
    currSharedInfo->serverVer = strdup ((const char *) pkt);
    LOGD ("Server_version:%s\n", currSharedInfo->serverVer);
    pkt += strlen ((const char *) pkt) + 1;

    /* Connection id */
    currSharedInfo->conId = FLI4 (pkt);
    LOGD ("Connection_id:%u\n", currSharedInfo->conId);
    pkt += 4;

    /* 8 bytes auth-plugin-data-part-1 0 padding */
    memcpy (authPluginData, pkt, 8);
    pkt += 8;

    /* 1 byte filler */
    filler = *pkt;
    pkt += 1;

    /* Capability flags lower 2 bytes */
    caps = FLI2 (pkt);
    if (caps & CLIENT_PROTOCOL_41)
        currSharedInfo->cliProtoIsV41 = true;
    else
        currSharedInfo->cliProtoIsV41 = false;
    LOGD ("CLIENT_PROTOCOL_41:%s\n", currSharedInfo->cliProtoIsV41 ? "YES" : "NO");
    pkt += 2;

    if (pkt < (payload + payloadLen)) {
        /* Character set */
        charSet = FLI1 (pkt);
        pkt += 1;

        /* Status flags */
        statusFlag = FLI2 (pkt);
        pkt += 2;

        /* Capability flags upper 2 bytes */
        caps = (FLI2 (pkt) << 16) + caps;
        pkt += 2;

        /* Auth plugin data len or 0 padding */
        if (caps & CLIENT_PLUGIN_AUTH) {
            authPluginDataLen = FLI1 (pkt);
        } else
            authPluginDataLen = 8;
        pkt += 1;

        /* 10 bytes for zero-byte padding */
        pkt += 10;

        /* Auth Plugin data */
        if (caps & CLIENT_SECURE_CONNECTION) {
            authPluginDataLen = MAX_NUM (13, (authPluginDataLen - 8));
            toCopyLen = MIN_NUM (authPluginDataLen, (sizeof (authPluginData) - 9));
            memcpy (authPluginData + 8, pkt, toCopyLen);
            pkt += authPluginDataLen;
            LOGD ("Auth_plugin_data:%s\n", authPluginData);
        }

        /* Auth plugin name */
        if (caps & CLIENT_PLUGIN_AUTH)
            LOGD ("Auth_plugin_name:%s\n", pkt);
    }

    return EVENT_HANDLE_OK;
}

static mysqlEventHandleState
pktClientHandshake (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    u_int encLen;
    u_long_long realLen;
    u_int toCopyLen;
    u_char charSet;
    u_char authResp [256] = {0};
    u_char *attrsEnd;
    char attrKey [256] = {0};
    char attrValue [256] = {0};
    u_char *pkt = payload;

    if ((direction == STREAM_FROM_SERVER) || (currSessionDetail->seqId != 1))
        return EVENT_HANDLE_ERROR;

    LOGD ("Cli >>---------------->> Server packet seqId:%u\n", currSessionDetail->seqId);
    LOGD ("Client handshake packet\n");

    if (currSharedInfo->cliProtoIsV41) {
        /* Capability flags */
        currSharedInfo->cliCaps = FLI4 (pkt);
        pkt += 4;

        /* Max packet size */
        currSharedInfo->maxPktSize = FLI4 (pkt);
        LOGD ("Max_packet_size:%u\n", currSharedInfo->maxPktSize);
        pkt += 4;

        /* Character set */
        charSet = FLI1 (pkt);
        pkt += 1;

        /* Reserved 23 bytes of 0 */
        pkt += 23;

        /* User name */
        currSharedInfo->userName = strdup ((const char *) pkt);
        LOGD ("User_name:%s\n", currSharedInfo->userName);
        pkt += strlen ((const char *) pkt) + 1;

        /* Auth response */
        if ((currSharedInfo->cliCaps & CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA) ||
            (currSharedInfo->cliCaps & CLIENT_SECURE_CONNECTION)) {
            if (currSharedInfo->cliCaps & CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA) {
                realLen = encLenInt (pkt, &encLen);
                pkt += encLen;
            } else {
                realLen = FLI1 (pkt);
                pkt += 1;
            }

            toCopyLen = MIN_NUM (realLen, (sizeof (authResp) - 1));
            memcpy (authResp, pkt, toCopyLen);
            pkt += realLen;
        } else {
            realLen = strlen ((const char *) pkt);
            toCopyLen = MIN_NUM (realLen, (sizeof (authResp) - 1));
            memcpy (authResp, pkt, toCopyLen);
            pkt += realLen + 1;
        }
        LOGD ("Auth_response:%s\n", authResp);

        /* Database */
        if (currSharedInfo->cliCaps & CLIENT_CONNECT_WITH_DB) {
            LOGD ("Database:%s\n", pkt);
            pkt += strlen ((const char *) pkt) + 1;
        }

        /* Auth plugin name */
        if (currSharedInfo->cliCaps & CLIENT_PLUGIN_AUTH) {
            LOGD ("Auth_plugin_name:%s\n", pkt);
            pkt += strlen ((const char *) pkt) + 1;
        }

        /* Attributes */
        if (currSharedInfo->cliCaps & CLIENT_CONNECT_ATTRS) {
            realLen = encLenInt (pkt, &encLen);
            attrsEnd = pkt + realLen;
            pkt += encLen;

            while (pkt < attrsEnd) {
                realLen = encLenInt (pkt, &encLen);
                pkt += encLen;
                toCopyLen = MIN_NUM (realLen, (sizeof (attrKey) - 1));
                memcpy (attrKey, pkt, toCopyLen);
                attrKey [toCopyLen] = 0;
                pkt += realLen;

                realLen = encLenInt (pkt, &encLen);
                pkt += encLen;
                toCopyLen = MIN_NUM (realLen, (sizeof (attrValue) - 1));
                memcpy (attrValue, pkt, toCopyLen);
                attrValue [toCopyLen] = 0;
                pkt += realLen;

                LOGD ("Attributes %s:%s\n", attrKey, attrValue);
            }
        }
    } else {
        /* Capability flags */
        currSharedInfo->cliCaps = FLI2 (pkt);
        pkt += 2;

        /* Max packet size */
        currSharedInfo->maxPktSize = FLI3 (pkt);
        LOGD ("Max_packet_size:%u\n", currSharedInfo->maxPktSize);
        pkt += 3;

        /* User name */
        currSharedInfo->userName = strdup ((const char *) pkt);
        LOGD ("User_name:%s\n", currSharedInfo->userName);
        pkt += strlen ((const char *) pkt) + 1;

        if (currSharedInfo->cliCaps & CLIENT_CONNECT_WITH_DB) {
            /* Auth response */
            realLen = strlen ((const char *) pkt);
            toCopyLen = MIN_NUM (realLen, (sizeof (authResp) - 1));
            memcpy (authResp, pkt, toCopyLen);
            LOGD ("Auth_response:%s\n", authResp);
            pkt += realLen + 1;

            /* DB name */
            LOGD ("Database:%s\n", pkt);
        } else {
            realLen = payload + payloadLen - pkt;
            toCopyLen = MIN_NUM (realLen, (sizeof (authResp) - 1));
            memcpy (authResp, pkt, toCopyLen);
            pkt += realLen;
            LOGD ("Auth_response:%s\n", authResp);
        }
    }

    currSharedInfo->doSSL = ((currSharedInfo->cliCaps & CLIENT_SSL) ? true : false);
    currSharedInfo->doCompress = ((currSharedInfo->cliCaps & CLIENT_COMPRESS) ? true : false);
    LOGD ("Connection doSSL:%s, doCompress:%s\n",
          currSharedInfo->doSSL ? "Yes" : "No", currSharedInfo->doCompress ? "Yes" : "No");

    return EVENT_HANDLE_OK;
}

static mysqlEventHandleState
pktSecureAuth (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    u_char *pkt = payload;

    if ((direction == STREAM_FROM_SERVER) &&
        (MATCH (*pkt, MYSQL_RESPONSE_OK_HEADER) ||
         MATCH (*pkt, MYSQL_RESPONSE_ERROR_HEADER)))
        return EVENT_HANDLE_ERROR;

    if (direction == STREAM_FROM_CLIENT) {
        LOGD ("Cli >>---------------->> Server packet seqId:%u\n", currSessionDetail->seqId);
        LOGD ("Secure authentication...\n");
    }
    else {
        LOGD ("Cli <<----------------<< Server packet seqId:%u\n", currSessionDetail->seqId);
        LOGD ("Secure authentication...\n");
    }

    return EVENT_HANDLE_OK;
}
/* =================================Handshake================================= */

/* ==================================Request================================== */

static mysqlEventHandleState
pktComX (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    boolean eventMatch;
    const char *cmdName;
    u_char *pkt = payload;

    if (direction != STREAM_FROM_CLIENT)
        return EVENT_HANDLE_ERROR;

    switch (event) {
        case COM_SLEEP:
            eventMatch = MATCH (*pkt, COM_SLEEP);
            break;

        case COM_QUIT:
            eventMatch = MATCH (*pkt, COM_QUIT);
            break;

        case COM_STATISTICS:
            eventMatch = MATCH (*pkt, COM_STATISTICS);
            break;

        case COM_PROCESS_INFO:
            eventMatch = MATCH (*pkt, COM_PROCESS_INFO);
            break;

        case COM_CONNECT:
            eventMatch = MATCH (*pkt, COM_CONNECT);
            break;

        case COM_DEBUG:
            eventMatch = MATCH (*pkt, COM_DEBUG);
            break;

        case COM_PING:
            eventMatch = MATCH (*pkt, COM_PING);
            break;

        case COM_TIME:
            eventMatch = MATCH (*pkt, COM_TIME);
            break;

        case COM_DELAYED_INSERT:
            eventMatch = MATCH (*pkt, COM_DELAYED_INSERT);
            break;

        case COM_CONNECT_OUT:
            eventMatch = MATCH (*pkt, COM_CONNECT_OUT);
            break;

        case COM_DAEMON:
            eventMatch = MATCH (*pkt, COM_DAEMON);
            break;

        case COM_RESET_CONNECTION:
            eventMatch = MATCH (*pkt, COM_RESET_CONNECTION);
            break;

        default:
            eventMatch = false;
            break;
    }

    if (!eventMatch)
        return EVENT_HANDLE_ERROR;
    
    currSessionDetail->cmd = *pkt;
    cmdName = mysqlCmdName [*pkt];
    pkt += 1;
    LOGD ("\nCli >>---------------->> Server packet seqId:%u\n", currSessionDetail->seqId);
    LOGD ("%s\n", cmdName);
    
    /* For COM_QUIT and COM_PING, doesn't do statistics */
    if (!MATCH (*pkt, COM_QUIT) && !MATCH (*pkt, COM_PING)) {
        currSessionDetail->reqStmt = strdup (cmdName);
        currSessionDetail->state = MYSQL_REQUEST_COMPLETE;
    }

    return EVENT_HANDLE_OK;
}

static mysqlEventHandleState
pktComDB (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    u_int realLen;
    u_int toCopyLen;
    const char *cmdName;
    char database [256] = {0};
    char com [256];
    u_char *pkt = payload;

    if ((direction != STREAM_FROM_CLIENT) ||
        !(MATCH (*pkt, COM_INIT_DB) || MATCH (*pkt, COM_CREATE_DB) || MATCH (*pkt, COM_DROP_DB)))
        return EVENT_HANDLE_ERROR;

    /* Get command name */
    currSessionDetail->cmd = *pkt;
    cmdName = mysqlCmdName [*pkt];
    pkt += 1;

    /* Get database name */
    realLen = payload + payloadLen - pkt;
    toCopyLen = MIN_NUM (realLen, (sizeof (database) - 1));
    memcpy (database, pkt, toCopyLen);
    pkt += realLen;

    /* Construct command */
    snprintf (com, sizeof (com), "%s db_name:%s", cmdName, database);
    LOGD ("\nCli >>---------------->> Server packet seqId:%u\n", currSessionDetail->seqId);
    LOGD ("%s\n", com);

    currSessionDetail->reqStmt = strdup (com);
    currSessionDetail->state = MYSQL_REQUEST_COMPLETE;

    return EVENT_HANDLE_OK;
}

static mysqlEventHandleState
pktQuery (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    u_int realLen;
    u_int toCopyLen;
    const char *cmdName;
    char schema [4096] = {0};
    char com [4096];
    u_char *pkt = payload;

    if ((direction != STREAM_FROM_CLIENT) ||
        !MATCH (*pkt, COM_QUERY))
        return EVENT_HANDLE_ERROR;

    /* Get command name */
    currSessionDetail->cmd = *pkt;
    cmdName = mysqlCmdName [*pkt];
    pkt += 1;
    
    /* Get schema */
    realLen = payload + payloadLen - pkt;
    toCopyLen = MIN_NUM (realLen, (sizeof (schema) - 1));
    memcpy (schema, pkt, toCopyLen);
    pkt += realLen;
    
    /* Construct command */
    snprintf (com, sizeof (com), "%s schema:%s", cmdName, schema);
    LOGD ("\nCli >>---------------->> Server packet seqId:%u\n", currSessionDetail->seqId);
    LOGD ("%s\n", com);
    
    currSessionDetail->reqStmt = strdup (com);
    currSessionDetail->state = MYSQL_REQUEST_COMPLETE;

    return EVENT_HANDLE_OK;
}

static mysqlEventHandleState
pktFieldList (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    u_int realLen;
    u_int toCopyLen;
    const char *cmdName;
    char *table;
    char fieldWildcard [4096] = {0};
    char com [4096];
    u_char *pkt = payload;

    if ((direction != STREAM_FROM_CLIENT) ||
        !MATCH (*pkt, COM_FIELD_LIST))
        return EVENT_HANDLE_ERROR;

    /* Get command name */
    currSessionDetail->cmd = *pkt;
    cmdName = mysqlCmdName [*pkt];
    pkt += 1;

    /* Get table */
    table = (char *) pkt;
    pkt += strlen ((const char *) pkt) + 1;

    /* Get field wildcard */
    realLen = payload + payloadLen - pkt;
    toCopyLen = MIN_NUM (realLen, (sizeof (fieldWildcard) - 1));
    memcpy (fieldWildcard, pkt, toCopyLen);
    pkt += realLen;
    
    /* Construct command */
    snprintf (com, sizeof (com), "%s table:%s, field_wildcard:%s", cmdName, table, fieldWildcard);
    LOGD ("\nCli >>---------------->> Server packet seqId:%u\n", currSessionDetail->seqId);
    LOGD ("%s\n", com);
    
    currSessionDetail->reqStmt = strdup (com);
    currSessionDetail->state = MYSQL_REQUEST_COMPLETE;

    return EVENT_HANDLE_OK;
}

static mysqlEventHandleState
pktRefresh (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    const char *cmdName;
    u_char subCmd;
    char com [256];
    u_char *pkt = payload;

    if ((direction != STREAM_FROM_CLIENT) ||
        !MATCH (*pkt, COM_REFRESH))
        return EVENT_HANDLE_ERROR;

    /* Get command name */
    currSessionDetail->cmd = *pkt;
    cmdName = mysqlCmdName [*pkt];
    pkt += 1;

    /* Get sub-command */
    subCmd = FLI1 (pkt);
    pkt += 1;

    /* Construct command */
    snprintf (com, sizeof (com), "%s sub_command:%u", cmdName, subCmd);
    LOGD ("\nCli >>---------------->> Server packet seqId:%u\n", currSessionDetail->seqId);
    LOGD ("%s\n", com);
    
    currSessionDetail->reqStmt = strdup (com);
    currSessionDetail->state = MYSQL_REQUEST_COMPLETE;

    return EVENT_HANDLE_OK;
}

static mysqlEventHandleState
pktShutdown (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    char com [256];
    u_char *pkt = payload;

    if ((direction != STREAM_FROM_CLIENT) ||
        !MATCH (*pkt, COM_SHUTDOWN))
        return EVENT_HANDLE_ERROR;

    /* Get command name and shutdown type if any */
    currSessionDetail->cmd = *pkt;
    if (payloadLen == 1) {
        snprintf (com, sizeof (com), "%s", mysqlCmdName [*pkt]);
        pkt += 1;
    } else {
        snprintf (com, sizeof (com), "%s shutdown_type:%u", mysqlCmdName [*pkt], (u_int) *(pkt + 1));
        pkt += 2;
    }
    LOGD ("\nCli >>---------------->> Server packet seqId:%u\n", currSessionDetail->seqId);
    LOGD ("%s\n", com);
    
    currSessionDetail->reqStmt = strdup (com);
    currSessionDetail->state = MYSQL_REQUEST_COMPLETE;

    return EVENT_HANDLE_OK;
}

static mysqlEventHandleState
pktProcessKill (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    const char *cmdName;
    u_int connectionId;
    char com [256];
    u_char *pkt = payload;

    if ((direction != STREAM_FROM_CLIENT) ||
        !MATCH (*pkt, COM_PROCESS_KILL))
        return EVENT_HANDLE_ERROR;

    /* Get command name */
    currSessionDetail->cmd = *pkt;
    cmdName = mysqlCmdName [*pkt];
    pkt += 1;

    /* Get connection id */
    connectionId = FLI4 (pkt);
    pkt += 4;

    /* Construct command */
    snprintf (com, sizeof (com), "%s connection_id:%u", cmdName, connectionId);
    LOGD ("\nCli >>---------------->> Server packet seqId:%u\n", currSessionDetail->seqId);
    LOGD ("%s\n", com);
    
    currSessionDetail->reqStmt = strdup (com);
    currSessionDetail->state = MYSQL_REQUEST_COMPLETE;

    return EVENT_HANDLE_OK;
}

static mysqlEventHandleState
pktChangeUser (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    u_int encLen;
    u_long_long realLen;
    u_int toCopyLen;    
    const char *cmdName;
    char *user;
    char authResp [256] = {0};
    u_short charSet;
    u_char *attrsEnd;
    char attrKey [256] = {0};
    char attrValue [256] = {0};
    char com [256];
    u_char *pkt = payload;

    if ((direction != STREAM_FROM_CLIENT) ||
        !MATCH (*pkt, COM_CHANGE_USER))
        return EVENT_HANDLE_ERROR;

    /* Get command name */
    currSessionDetail->cmd = *pkt;
    cmdName = mysqlCmdName [*pkt];
    pkt += 1;

    /* Get user */
    user = (char *) pkt;
    pkt += strlen ((const char *) pkt) + 1;
    
    /* Construct command */
    snprintf (com, sizeof (com), "%s user_name:%s", cmdName, user);
    LOGD ("\nCli >>---------------->> Server packet seqId:%u\n", currSessionDetail->seqId);
    LOGD ("%s\n", com);

    /* Auth response */
    if (currSharedInfo->cliCaps & CLIENT_SECURE_CONNECTION) {
        realLen = FLI1 (pkt);
        pkt += 1;
        toCopyLen = MIN_NUM (realLen, (sizeof (authResp) - 1));
        memcpy (authResp, pkt, toCopyLen);
        pkt += realLen;
    } else {
        realLen = strlen ((const char *) pkt);
        toCopyLen = MIN_NUM (realLen, (sizeof (authResp) - 1));
        memcpy (authResp, pkt, toCopyLen);
        pkt += realLen + 1;
    }
    LOGD ("Auth_response:%s, ", authResp);

    /* Schema name */
    LOGD ("schema_name:%s, ", pkt);
    pkt += strlen ((const char *) pkt) + 1;

    /* More data */
    if (pkt < (payload + payloadLen)) {
        /* Character set */
        charSet = FLI2 (pkt);
        pkt += 2;

        /* Plugin name */
        if (currSharedInfo->cliCaps & CLIENT_PLUGIN_AUTH) {
            LOGD ("plugin_name:%s\n", pkt);
            pkt += strlen ((const char *) pkt) + 1;
        }

        /* Attributes */
        if (currSharedInfo->cliCaps & CLIENT_CONNECT_ATTRS) {
            realLen = encLenInt (pkt, &encLen);
            attrsEnd = pkt + realLen;
            pkt += encLen;

            while (pkt < attrsEnd) {
                realLen = encLenInt (pkt, &encLen);
                pkt += encLen;
                toCopyLen = MIN_NUM (realLen, (sizeof (attrKey) - 1));
                memcpy (attrKey, pkt, toCopyLen);
                attrKey [toCopyLen] = 0;
                pkt += realLen;

                realLen = encLenInt (pkt, &encLen);
                pkt += encLen;
                toCopyLen = MIN_NUM (realLen, (sizeof (attrValue) - 1));
                memcpy (attrValue, pkt, toCopyLen);
                attrValue [toCopyLen] = 0;
                pkt += realLen;

                LOGD ("Attributes %s:%s\n", attrKey, attrValue);
            }
        }
    }

    currSessionDetail->reqStmt = strdup (com);
    currSessionDetail->state = MYSQL_REQUEST_COMPLETE;

    return EVENT_HANDLE_OK;
}

static mysqlEventHandleState
pktRegisterSlave (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    u_int len;
    u_int toCopyLen;
    const char *cmdName;
    u_int serverId;
    char hostName [256] = {0};
    char user [256] = {0};
    char passwd [256] = {0};
    u_short mysqlPort;
    u_int repRank;
    u_int masterId;
    char com [1024];
    u_char *pkt = payload;

    if ((direction != STREAM_FROM_CLIENT) ||
        !MATCH (*pkt, COM_REGISTER_SLAVE))
        return EVENT_HANDLE_ERROR;

    /* Get command name */
    currSessionDetail->cmd = *pkt;
    cmdName = mysqlCmdName [*pkt];
    pkt += 1;

    /* Get server id */
    serverId = FLI4 (pkt);
    pkt += 4;

    /* Get host name */
    len = FLI1 (pkt);
    pkt += 1;
    toCopyLen = MIN_NUM (len, (sizeof (hostName) - 1));
    memcpy (hostName, pkt, toCopyLen);
    pkt += len;

    /* Get user */
    len = FLI1 (pkt);
    pkt += 1;
    toCopyLen = MIN_NUM (len, (sizeof (user) - 1));
    memcpy (user, pkt, toCopyLen);
    pkt += len;

    /* Get password */
    len = FLI1 (pkt);
    pkt += 1;
    toCopyLen = MIN_NUM (len, (sizeof (passwd) - 1));
    memcpy (passwd, pkt, toCopyLen);
    pkt += len;

    /* Get mysql port */
    mysqlPort = FLI2 (pkt);
    pkt += 2;

    /* Get replication rank */
    repRank = FLI4 (pkt);
    pkt += 4;

    /* Get master id */
    masterId = FLI4 (pkt);
    pkt += 4;

    /* Construct command */
    snprintf (com, sizeof (com),
              "%s server_id:%u, host_name:%s, user:%s, password:%s, mysql_port:%u, replication_rank:%u, master_id:%u",
              cmdName, serverId, hostName, user, passwd, mysqlPort, repRank, masterId);
    LOGD ("\nCli >>---------------->> Server packet seqId:%u\n", currSessionDetail->seqId);
    LOGD ("%s\n", com);
    
    currSessionDetail->reqStmt = strdup (com);
    currSessionDetail->state = MYSQL_REQUEST_COMPLETE;

    return EVENT_HANDLE_OK;
}

static mysqlEventHandleState
pktStmtPrepare (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    u_int realLen;
    u_int toCopyLen;
    const char *cmdName;
    char schema [4096] = {0};
    char com [4096];
    u_char *pkt = payload;

    if ((direction != STREAM_FROM_CLIENT) ||
        !MATCH (*pkt, COM_STMT_PREPARE))
        return EVENT_HANDLE_ERROR;

    /* Get command name */
    currSessionDetail->cmd = *pkt;
    cmdName = mysqlCmdName [*pkt];
    pkt += 1;

    /* Get schema */
    realLen = payload + payloadLen - pkt;
    toCopyLen = MIN_NUM (realLen, (sizeof (schema) - 1));
    memcpy (schema, pkt, toCopyLen);
    pkt += realLen;

    /* Construct command */
    snprintf (com, sizeof (com), "%s schema:%s", cmdName, schema);
    LOGD ("\nCli >>---------------->> Server packet seqId:%u\n", currSessionDetail->seqId);
    LOGD ("%s\n", com);
    
    currSessionDetail->reqStmt = strdup (com);
    currSessionDetail->state = MYSQL_REQUEST_COMPLETE;

    return EVENT_HANDLE_OK;
}

static mysqlEventHandleState
pktStmtExec (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    const char *cmdName;
    u_int stmtId;
    u_char flags;
    u_int iterCount;
    char com [256];
    u_char *pkt = payload;

    if ((direction != STREAM_FROM_CLIENT) ||
        !MATCH (*pkt, COM_STMT_EXECUTE))
        return EVENT_HANDLE_ERROR;

    /* Get command name */
    currSessionDetail->cmd = *pkt;
    cmdName = mysqlCmdName [*pkt];
    pkt += 1;

    /* Get stmt-id */
    stmtId = FLI4 (pkt);
    pkt += 4;

    /* Get flags */
    flags = FLI1 (pkt);
    pkt += 1;

    /* Get iteration count */
    iterCount = FLI4 (pkt);
    pkt += 4;

    /* Construct command */
    snprintf (com, sizeof (com), "%s stmt_id:%d, flags:%u, iteration_count:%u",
              cmdName, stmtId, flags, iterCount);
    LOGD ("\nCli >>---------------->> Server packet seqId:%u\n", currSessionDetail->seqId);
    LOGD ("%s\n", com);
    
    currSessionDetail->reqStmt = strdup (com);
    currSessionDetail->state = MYSQL_REQUEST_COMPLETE;

    return EVENT_HANDLE_OK;
}

static mysqlEventHandleState
pktStmtCloseOrReset (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    const char *cmdName;
    u_int stmtId;
    char com [256];
    u_char *pkt = payload;

    if ((direction != STREAM_FROM_CLIENT) ||
        !(MATCH (*pkt, COM_STMT_CLOSE) || MATCH(*pkt, COM_STMT_RESET)))
        return EVENT_HANDLE_ERROR;

    /* Get command name */
    currSessionDetail->cmd = *pkt;    
    cmdName = mysqlCmdName [*pkt];
    pkt += 1;

    /* Get stmt-id */
    stmtId = FLI4 (pkt);
    pkt += 4;

    /* Construct command */
    snprintf (com, sizeof (com), "%s stmt_id:%d", cmdName, stmtId);
    LOGD ("\nCli >>---------------->> Server packet seqId:%u\n", currSessionDetail->seqId);
    LOGD ("%s\n", com);
    
    currSessionDetail->reqStmt = strdup (com);
    currSessionDetail->state = MYSQL_REQUEST_COMPLETE;

    return EVENT_HANDLE_OK;
}

static mysqlEventHandleState
pktSetOption (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    const char *cmdName;
    u_short option;
    char com [256];
    u_char *pkt = payload;

    if ((direction != STREAM_FROM_CLIENT) ||
        !MATCH (*pkt, COM_SET_OPTION))
        return EVENT_HANDLE_ERROR;

    /* Get command name */
    currSessionDetail->cmd = *pkt;
    cmdName = mysqlCmdName [*pkt];
    pkt += 1;

    /* Get option */
    option = FLI2 (pkt);
    pkt += 2;

    /* Construct command */
    snprintf (com, sizeof (com), "%s option:%u", cmdName, option);
    LOGD ("\nCli >>---------------->> Server packet seqId:%u\n", currSessionDetail->seqId);
    LOGD ("%s\n", com);

    currSessionDetail->reqStmt = strdup (com);
    currSessionDetail->state = MYSQL_REQUEST_COMPLETE;

    return EVENT_HANDLE_OK;
}

static mysqlEventHandleState
pktStmtFetch (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    const char *cmdName;
    u_int stmtId;
    u_int numRows;
    char com [256];
    u_char *pkt = payload;

    if ((direction != STREAM_FROM_CLIENT) ||
        !MATCH (*pkt, COM_STMT_FETCH))
        return EVENT_HANDLE_ERROR;

    /* Get command name */
    currSessionDetail->cmd = *pkt;
    cmdName = mysqlCmdName [*pkt];
    pkt += 1;

    /* Get stmt-id */
    stmtId = FLI4 (pkt);
    pkt += 4;

    /* Get num rows */
    numRows = FLI4 (pkt);
    pkt += 4;

    /* Construct command */
    snprintf (com, sizeof (com), "%s stmt_id:%u, num_rows:%u", cmdName, stmtId, numRows);
    LOGD ("\nCli >>---------------->> Server packet seqId:%u\n", currSessionDetail->seqId);
    LOGD ("%s\n", com);

    currSessionDetail->reqStmt = strdup (com);
    currSessionDetail->state = MYSQL_REQUEST_COMPLETE;

    return EVENT_HANDLE_OK;
}

/* ==================================Request================================== */

/* =================================Response================================== */

static mysqlEventHandleState
pktOk (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    u_int encLen;
    u_long_long realLen;
    u_int toCopyLen;
    u_long_long rows;
    u_long_long insertId;
    u_int status;
    u_int warn;
    char okInfo [512] = {0};
    u_char *pkt = payload;

    if ((direction != STREAM_FROM_SERVER) ||
        !MATCH(*pkt, MYSQL_RESPONSE_OK_HEADER))
        return EVENT_HANDLE_ERROR;

    if (currSessionDetail->showS2CTag) {
        currSessionDetail->showS2CTag = false;
        LOGD ("Cli <<----------------<< Server packet seqId:%u\n", currSessionDetail->seqId);
    }    

    LOGD ("Ok response packet\n");
    pkt += 1;

    /* Affected rows */
    rows = encLenInt (pkt, &encLen);
    pkt += encLen;
    LOGD ("Affected_rows:%llu, ", rows);

    /* Last insert id */
    insertId = encLenInt (pkt, &encLen);
    pkt += encLen;
    LOGD ("last_insert_id:%llu, ", insertId);

    /* Status and Warning */
    if (currSharedInfo->cliProtoIsV41) {
        status = FLI2 (pkt);
        pkt += 2;

        warn = FLI2 (pkt);
        pkt += 2;
    } else if (currSharedInfo->cliCaps & CLIENT_TRANSACTIONS) {
        status = FLI2 (pkt);
        pkt += 2;

        warn = 0;
    } else {
        status = 0;

        warn = 0;
    }

    /* Info */
    if (currSharedInfo->cliCaps & CLIENT_SESSION_TRACK)
        realLen = encLenInt (pkt, &encLen);
    else
        realLen = payload + payloadLen - pkt;
    toCopyLen = MIN_NUM (realLen, sizeof (okInfo) - 1);
    memcpy (okInfo, pkt, toCopyLen);
    pkt += realLen;
    LOGD ("info:%s\n", okInfo);

    /*
     * For mysql handshake, COM_QUIT and COM_PING packet, there is no request
     * statement and session breakdown.
     */
    if (currSessionDetail->reqStmt) {
        currSessionDetail->state = MYSQL_RESPONSE_OK;
        currSessionDetail->respTimeEnd = timeVal2MilliSecond (currTime);
        currSessionState = SESSION_DONE;
    }

    return EVENT_HANDLE_OK;
}

static mysqlEventHandleState
pktError (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    u_int realLen;
    u_int toCopyLen;
    u_short errCode;
    u_char sqlStateMarker;
    char sqlState [6] = {0};
    char errMsg [512] = {0};
    u_char *pkt = payload;

    if ((direction != STREAM_FROM_SERVER) ||
        !MATCH(*pkt, MYSQL_RESPONSE_ERROR_HEADER))
        return EVENT_HANDLE_ERROR;

    if (currSessionDetail->showS2CTag) {
        currSessionDetail->showS2CTag = false;
        LOGD ("Cli <<----------------<< Server packet seqId:%u\n", currSessionDetail->seqId);
    }

    LOGD ("Error response packet\n");
    pkt += 1;

    /* Error code */
    errCode = FLI2 (pkt);
    pkt += 2;
    LOGD ("Error_code:%u, ", errCode);

    if (currSharedInfo->cliProtoIsV41) {
        /* SQL State marker */
        sqlStateMarker = *pkt;
        pkt += 1;

        /* SQL State */
        memcpy (sqlState, pkt, 5);
        pkt += 5;
        LOGD ("sql_state_marker:%u, sql_state:%u\n", sqlStateMarker, atoi (sqlState));
    }

    /* Error message */
    realLen = payload + payloadLen - pkt;
    toCopyLen = MIN_NUM (realLen, sizeof (errMsg) - 1);
    memcpy (errMsg, pkt, toCopyLen);
    pkt += realLen;
    LOGD ("Error_message:%s\n", errMsg);

    /*
     * For mysql handshake, COM_QUIT and COM_PING packet, there is no request
     * statement and session breakdown.
     */
    if (currSessionDetail->reqStmt) {
        currSessionDetail->state = MYSQL_RESPONSE_ERROR;
        currSessionDetail->respTimeEnd = timeVal2MilliSecond (currTime);
        currSessionDetail->errCode = errCode;
        if (currSharedInfo->cliProtoIsV41)
            currSessionDetail->sqlState = atoi (sqlState);
        currSessionDetail->errMsg = strdup (errMsg);
        currSessionState = SESSION_DONE;
    }

    return EVENT_HANDLE_OK;
}

static mysqlEventHandleState
pktEnd (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    u_short warnsNum = 0;
    u_short statusFlags = 0;
    u_char *pkt = payload;

    if ((direction != STREAM_FROM_SERVER) ||
        !MATCH (*pkt, MYSQL_RESPONSE_END_HEADER))
        return EVENT_HANDLE_ERROR;

    pkt += 1;

    if (currSharedInfo->cliProtoIsV41) {
        /* Get number of warnings */
        warnsNum = FLI2 (pkt);
        pkt += 2;

        /* Get status flags */
        statusFlags = FLI2 (pkt);
        pkt += 2;
    }

    if (event == EVENT_END_WITH_MULTI_RESULT) {
        if (statusFlags & SERVER_MORE_RESULTS_EXISTS) {
            if (currSessionDetail->showS2CTag) {
                currSessionDetail->showS2CTag = false;
                LOGD ("Cli <<----------------<< Server packet seqId:%u\n", currSessionDetail->seqId);
            }

            LOGD ("End response packet with more results\n");
            return EVENT_HANDLE_OK;
        } else
            return EVENT_HANDLE_ERROR;
    }

    if (currSessionDetail->showS2CTag) {
        currSessionDetail->showS2CTag = false;
        LOGD ("Cli <<----------------<< Server packet seqId:%u\n", currSessionDetail->seqId);
    }

    LOGD ("End response packet\n");

    if ((currSessionDetail->mstate == STATE_END_OR_ERROR) ||
        (currSessionDetail->mstate == STATE_FIELD_LIST) ||
        (currSessionDetail->mstate == STATE_END) ||
        (currSessionDetail->mstate == STATE_TXT_ROW) ||
        (currSessionDetail->mstate == STATE_BIN_ROW)) {
        currSessionDetail->state = MYSQL_RESPONSE_OK;
        currSessionDetail->respTimeEnd = timeVal2MilliSecond (currTime);
        currSessionState = SESSION_DONE;
    }

    return EVENT_HANDLE_OK;
}

static mysqlEventHandleState
pktStatistics (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    u_int toCopyLen;
    char statistics [512] = {0};
    u_char *pkt = payload;

    if (currSessionDetail->showS2CTag) {
        currSessionDetail->showS2CTag = false;
        LOGD ("Cli <<----------------<< Server packet seqId:%u\n", currSessionDetail->seqId);
    }
    
    /* Get statistics */
    toCopyLen = MIN_NUM (payloadLen, (sizeof (statistics) - 1));
    memcpy (statistics, payload, toCopyLen);
    pkt += payloadLen;
    LOGD ("Statistics:%s\n", statistics);
    
    currSessionDetail->respTimeEnd = timeVal2MilliSecond (currTime);
    currSessionDetail->state = MYSQL_RESPONSE_OK;
    currSessionState = SESSION_DONE;

    return EVENT_HANDLE_OK;
}

static mysqlEventHandleState
pktNFields (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    u_int encLen;
    u_long_long count;
    u_char *pkt = payload;

    if ((direction != STREAM_FROM_SERVER) ||
        MATCH (*pkt, MYSQL_RESPONSE_OK_HEADER) ||
        MATCH (*pkt, MYSQL_RESPONSE_ERROR_HEADER))
        return EVENT_HANDLE_ERROR;

    if (currSessionDetail->showS2CTag) {
        currSessionDetail->showS2CTag = false;
        LOGD ("Cli <<----------------<< Server packet seqId:%u\n", currSessionDetail->seqId);
    }

    count = encLenInt (pkt, &encLen);
    currSessionDetail->cmdCtxt.fieldsCount = count;
    currSessionDetail->cmdCtxt.fieldsRecv = 0;
    pkt += encLen;
    LOGD ("Field_count:%u\n", (u_int) count);
    
    return EVENT_HANDLE_OK;
}

static mysqlEventHandleState
pktField (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    u_int encLen;
    u_long_long realLen;
    u_int toCopyLen;
    u_short charSet;
    u_int columnLen;
    u_char type;
    u_short flags;
    u_char decimals;
    char buf [512] = {0};
    u_char *pkt = payload;

    if ((direction != STREAM_FROM_SERVER) || MATCH (*pkt, MYSQL_RESPONSE_END_HEADER) ||
        ((event == EVENT_TXT_FIELD) &&
         (currSessionDetail->cmdCtxt.fieldsRecv >= currSessionDetail->cmdCtxt.fieldsCount)))
        return EVENT_HANDLE_ERROR;

    if (currSessionDetail->showS2CTag) {
        currSessionDetail->showS2CTag = false;
        LOGD ("Cli <<----------------<< Server packet seqId:%u\n", currSessionDetail->seqId);
    }
    
    currSessionDetail->cmdCtxt.fieldsRecv++;
    LOGD ("Field definition\n");
    
    if (currSharedInfo->cliProtoIsV41) {
        /* Get catalog */
        realLen = encLenInt (pkt, &encLen);
        pkt += encLen;
        toCopyLen = MIN_NUM (realLen, (sizeof (buf) - 1));
        memcpy (buf, pkt, toCopyLen);
        buf [toCopyLen] = 0;
        pkt += realLen;
        LOGD ("Catalog:%s, ", buf);

        /* Get schema */
        realLen = encLenInt (pkt, &encLen);
        pkt += encLen;
        toCopyLen = MIN_NUM (realLen, (sizeof (buf) - 1));
        memcpy (buf, pkt, toCopyLen);
        buf [toCopyLen] = 0;
        pkt += realLen;
        LOGD ("schema:%s, ", buf);
        
        /* Get table */
        realLen = encLenInt (pkt, &encLen);
        pkt += encLen;
        toCopyLen = MIN_NUM (realLen, (sizeof (buf) - 1));
        memcpy (buf, pkt, toCopyLen);
        buf [toCopyLen] = 0;
        pkt += realLen;
        LOGD ("table:%s, ", buf);

        /* Get org_table */
        realLen = encLenInt (pkt, &encLen);
        pkt += encLen;
        toCopyLen = MIN_NUM (realLen, (sizeof (buf) - 1));
        memcpy (buf, pkt, toCopyLen);
        buf [toCopyLen] = 0;
        pkt += realLen;
        LOGD ("org_table:%s, ", buf);

        /* Get name */
        realLen = encLenInt (pkt, &encLen);
        pkt += encLen;
        toCopyLen = MIN_NUM (realLen, (sizeof (buf) - 1));
        memcpy (buf, pkt, toCopyLen);
        buf [toCopyLen] = 0;
        pkt += realLen;
        LOGD ("name:%s, ", buf);

        /* Get org_name */
        realLen = encLenInt (pkt, &encLen);
        pkt += encLen;
        toCopyLen = MIN_NUM (realLen, (sizeof (buf) - 1));
        memcpy (buf, pkt, toCopyLen);
        buf [toCopyLen] = 0;
        pkt += realLen;
        LOGD ("org_name:%s, ", buf);

        /* Get length of fixed-length fields */
        realLen = encLenInt (pkt, &encLen);
        pkt += encLen;

        /* Get character set */
        charSet = FLI2 (pkt);
        pkt += 2;

        /* Get column length */
        columnLen = FLI4 (pkt);
        pkt += 4;

        /* Get type */
        type = FLI1 (pkt);
        if (currSessionDetail->cmdCtxt.fieldsRecv <= sizeof (currSessionDetail->cmdCtxt.fieldsType))
            currSessionDetail->cmdCtxt.fieldsType [currSessionDetail->cmdCtxt.fieldsRecv - 1] = type;
        LOGD ("type:%s\n", getFieldTypeName (type));
        pkt += 1;

        /* Get flags */
        flags = FLI2 (pkt);
        pkt += 2;

        /* Get decimals */
        decimals = FLI1 (pkt);
        pkt += 1;
    } else {
        /* Get table */
        realLen = encLenInt (pkt, &encLen);
        pkt += encLen;
        toCopyLen = MIN_NUM (realLen, (sizeof (buf) - 1));
        memcpy (buf, pkt, toCopyLen);
        buf [toCopyLen] = 0;
        pkt += realLen;
        LOGD ("Table:%s, ", buf);

        /* Get name */
        realLen = encLenInt (pkt, &encLen);
        pkt += encLen;
        toCopyLen = MIN_NUM (realLen, (sizeof (buf) - 1));
        memcpy (buf, pkt, toCopyLen);
        buf [toCopyLen] = 0;
        pkt += realLen;
        LOGD ("name:%s\n", buf);

        /* Get length of fixed-length fields */
        encLen = 1;
        pkt += encLen;
        columnLen = FLI3 (pkt);
        pkt += 3;

        /* Get type */
        encLen = 1;
        pkt += 1;
        type = FLI1 (pkt);
        if (currSessionDetail->cmdCtxt.fieldsRecv <= sizeof (currSessionDetail->cmdCtxt.fieldsType))
            currSessionDetail->cmdCtxt.fieldsType [currSessionDetail->cmdCtxt.fieldsRecv - 1] = type;
        LOGD ("type:%s\n", getFieldTypeName (type));
        pkt += 1;

        /* Skip length of flags+decimals fields */
        encLen = 1;
        pkt += 1;

        /* Get flags */
        if (currSharedInfo->cliCaps & CLIENT_LONG_FLAG) {
            flags = FLI2 (pkt);
            pkt += 2;
        } else {
            flags = FLI1 (pkt);
            pkt += 1;
        }

        /* Get decimals */
        decimals = FLI1 (pkt);
        pkt += 1;
    }

    if (event == EVENT_FIELD_LIST_FIELD) {
        realLen = encLenInt (pkt, &encLen);
        pkt += encLen;
        toCopyLen = MIN_NUM (realLen, (sizeof (buf) - 1));
        memcpy (buf, pkt, toCopyLen);
        buf [toCopyLen] = 0;
        pkt += realLen;
    }

    return EVENT_HANDLE_OK;
}

static mysqlEventHandleState
pktTxtRow (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    u_int encLen;
    u_long_long realLen;
    u_int toCopyLen;
    char buf [1024] = {0};
    u_char *pkt = payload;

    if ((direction != STREAM_FROM_SERVER) ||
        MATCH (*pkt, MYSQL_RESPONSE_END_HEADER) ||
        MATCH (*pkt, MYSQL_RESPONSE_OK_HEADER) ||
        MATCH (*pkt, MYSQL_RESPONSE_ERROR_HEADER))
        return EVENT_HANDLE_ERROR;

    if (currSessionDetail->showS2CTag) {
        currSessionDetail->showS2CTag = false;
        LOGD ("Cli <<----------------<< Server packet seqId:%u\n", currSessionDetail->seqId);
    }
    
    LOGD ("|");
    while (pkt < (payload + payloadLen)) {
        realLen = encLenInt (pkt, &encLen);
        pkt += encLen;

        if (realLen) {
            toCopyLen = MIN_NUM (realLen, (sizeof (buf) - 1));
            memcpy (buf, pkt, toCopyLen);
            buf [toCopyLen] = 0;
        } else
            snprintf (buf, sizeof (buf), "NULL");

        pkt += realLen;
        LOGD (" %s |", buf);
    }
    LOGD ("\n");
    
    return EVENT_HANDLE_OK;
}

static mysqlEventHandleState
pktBinRow (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    u_int encLen;
    u_long_long realLen;
    u_int toCopyLen;
    u_int column, columnEnd;
    u_int nullBitmapByte, nullBitmapByteBit;
    u_short nullBitmapSize = (currSessionDetail->cmdCtxt.fieldsCount + 2 + 7) / 8;
    u_long_long number;
    u_int sign, year, month, day, hour, min, second;
    char buf [512] = {0};
    u_char *nullBitmap = payload + 1;
    u_char *pkt = payload + 1 + nullBitmapSize;

    if ((direction != STREAM_FROM_SERVER) ||
        MATCH (*pkt, MYSQL_RESPONSE_END_HEADER) ||
        MATCH (*pkt, MYSQL_RESPONSE_ERROR_HEADER))
        return EVENT_HANDLE_ERROR;

    if (currSessionDetail->showS2CTag) {
        currSessionDetail->showS2CTag = false;
        LOGD ("Cli <<----------------<< Server packet seqId:%u\n", currSessionDetail->seqId);
    }

    columnEnd = MIN_NUM (currSessionDetail->cmdCtxt.fieldsCount,
                         sizeof (currSessionDetail->cmdCtxt.fieldsType));
    LOGD ("||");
    for (column = 0; column < columnEnd; column ++) {
        nullBitmapByte = (column + 2) / 8;
        nullBitmapByteBit = (column + 2) % 8;

        /* Colum is null */
        if (nullBitmap [nullBitmapByte] & (1 << nullBitmapByteBit)) {
            LOGD (" NULL |");
            continue;
        } else {
            switch (currSessionDetail->cmdCtxt.fieldsType [column]) {
                case FIELD_TYPE_TINY:
                    number = FLI1 (pkt);
                    LOGD (" %llu |", number);
                    pkt += 1;
                    break;

                case FIELD_TYPE_SHORT:
                    number = FLI2 (pkt);
                    LOGD (" %llu |", number);
                    pkt += 2;
                    break;

                case FIELD_TYPE_LONG:
                    number = FLI4 (pkt);
                    LOGD (" %llu |", number);
                    pkt += 4;
                    break;

                case FIELD_TYPE_LONGLONG:
                    number = FLI8 (pkt);
                    LOGD (" %llu |", number);
                    pkt += 8;                    
                    break;

                case FIELD_TYPE_DATE:
                    realLen = FLI1 (pkt);
                    pkt += 1;
                    year = FLI2 (pkt);
                    pkt += 2;
                    month = FLI1 (pkt);
                    pkt += 1;
                    day = FLI1 (pkt);
                    pkt += 1;
                    snprintf (buf, sizeof (buf), "%04u-%02u-%02u", year, month, day);
                    LOGD (" %s |", buf);
                    break;

                case FIELD_TYPE_TIME:
                    realLen = FLI1 (pkt);
                    pkt += 1;
                    sign = FLI1 (pkt);
                    pkt += 1;
                    day = FLI4 (pkt);
                    pkt += 4;
                    hour = FLI1 (pkt);
                    hour += (day * 24);
                    pkt += 1;
                    min = FLI1 (pkt);
                    pkt += 1;
                    second = FLI1 (pkt);
                    pkt += 1;
                    snprintf (buf, sizeof (buf), "%s%02u:%02u:%02u",
                              (sign ? "-" : ""), hour, min, second);
                    LOGD (" %s |", buf);
                    break;

                case FIELD_TYPE_DATETIME:
                    realLen = FLI1 (pkt);
                    pkt += 1;
                    year = FLI2 (pkt);
                    pkt += 2;
                    month = FLI1 (pkt);
                    pkt += 1;
                    day = FLI1 (pkt);
                    pkt += 1;
                    hour = FLI1 (pkt);
                    pkt += 1;
                    min = FLI1 (pkt);
                    pkt += 1;
                    second = FLI1 (pkt);
                    pkt += 1;
                    snprintf (buf, sizeof (buf), "%04u-%02u-%02u %02u:%02u:%02u",
                              year, month, day, hour, min, second);
                    LOGD (" %s |", buf);
                    break;

                case FIELD_TYPE_VARCHAR:
                case FIELD_TYPE_TINY_BLOB:
                case FIELD_TYPE_MEDIUM_BLOB:
                case FIELD_TYPE_LONG_BLOB:
                case FIELD_TYPE_BLOB:
                case FIELD_TYPE_VAR_STRING:
                case FIELD_TYPE_STRING:
                    realLen = encLenInt (pkt, &encLen);
                    pkt += encLen;
                    toCopyLen = MIN_NUM (realLen, (sizeof (buf) - 1));
                    memcpy (buf, pkt, toCopyLen);
                    buf [toCopyLen] = 0;
                    pkt += realLen;
                    LOGD (" %s |", buf);
                    break;

                default:
                    LOGD (" %s ||\n", getFieldTypeName (currSessionDetail->cmdCtxt.fieldsType [column]));
                    return EVENT_HANDLE_OK;
            }
        }
    }
    LOGD ("|\n");
    
    return EVENT_HANDLE_OK;
}

static mysqlEventHandleState
pktStmtMeta (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    u_char status;
    u_int stmtId;
    u_short numColumns;
    u_short numParams;
    u_char filler;
    u_short warnCount;
    u_char *pkt = payload;
    
    if ((direction != STREAM_FROM_SERVER) ||
        MATCH (*pkt, MYSQL_RESPONSE_ERROR_HEADER))
        return EVENT_HANDLE_ERROR;

    if (currSessionDetail->showS2CTag) {
        currSessionDetail->showS2CTag = false;
        LOGD ("Cli <<----------------<< Server packet seqId:%u\n", currSessionDetail->seqId);
    }

    /* Get status */
    status = FLI1 (pkt);
    pkt += 1;

    /* Get statement-id */
    stmtId = FLI4 (pkt);
    pkt += 4;

    /* Get number of columns */
    numColumns = FLI2 (pkt);
    pkt += 2;

    /* Get number of params */
    numParams = FLI2 (pkt);
    pkt += 2;

    /* Get filler */
    filler = FLI1 (pkt);
    pkt += 1;

    /* Get number of warnings */
    warnCount = FLI2 (pkt);
    pkt += 2;

    LOGD ("Status:%u, statement_id:%u, num_columns:%u, num_params:%u, warning_count:%u\n",
          status, stmtId, numColumns, numParams, warnCount);
    
    return EVENT_HANDLE_OK;
}

static mysqlEventHandleState
pktStmtFetchRS (mysqlEvent event, u_char *payload, u_int payloadLen, streamDirection direction) {
    u_char *pkt = payload;

    if ((direction != STREAM_FROM_SERVER) ||
        MATCH (*pkt, MYSQL_RESPONSE_OK_HEADER) ||
        MATCH (*pkt, MYSQL_RESPONSE_ERROR_HEADER))
        return EVENT_HANDLE_ERROR;

    if (currSessionDetail->showS2CTag) {
        currSessionDetail->showS2CTag = false;
        LOGD ("Cli <<----------------<< Server packet seqId:%u\n", currSessionDetail->seqId);
    }
    
    LOGD ("Statement fetch result ... .. .\n");
    
    return EVENT_HANDLE_OK;
}

/* =================================Response================================== */

static void
resetMysqlSessionDetail (mysqlSessionDetailPtr msd);

static u_int
sqlParse (u_char *data, u_int dataLen, streamDirection direction) {
    u_int parseCount = 0;
    u_int parseLeft = dataLen;
    u_char *pkt;
    u_int pktLen;
    mysqlHeaderPtr hdr;
    u_int payloadLen;
    u_char *payload;
    mysqlEvent event;
    mysqlEventHandler handler;

    while (1) {
        if (parseLeft < MYSQL_HEADER_SIZE)
            break;

        /* Next mysql packet begin */
        pkt = data + parseCount;
        hdr = (mysqlHeaderPtr) pkt;
        payloadLen = hdr->payloadLen;
        currSessionDetail->seqId = hdr->pktId;
        payload = pkt + MYSQL_HEADER_SIZE;
        pktLen = MYSQL_HEADER_SIZE + payloadLen;

        /* If packet is not complete, return and wait for further processing */
        if (parseLeft < pktLen)
            break;

        if (payloadLen) {
            for (event = 0; event < mysqlStateEventMatrix [currSessionDetail->mstate].size; event++) {
                handler = mysqlStateEventMatrix [currSessionDetail->mstate].handler [event];

                if ((*handler) (event, payload, payloadLen, direction) == EVENT_HANDLE_OK) {
                    currSessionDetail->mstate = mysqlStateEventMatrix [currSessionDetail->mstate].nextState [event];
                    break;
                } else
                    handler = NULL;
            }
            if (handler == NULL)
                LOGW ("Warning: has no proper handler.\n");
        }

        parseCount += pktLen;
        parseLeft -= pktLen;
    }

    return parseCount;
}

static u_int
mysqlParserExecute (u_char *data, u_int dataLen, streamDirection direction) {
    u_int parseCount = 0;
    u_int parseLeft = dataLen;
    u_char *compPkt;
    u_char *uncompPkt;
    u_int compPktLen;
    mysqlCompHeaderPtr compHdr;
    u_int payloadLen;
    u_int compPayloadLen;
    u_int uncompPayloadLen;
    u_char *compPayload;

    if (currSharedInfo->doSSL) {
        LOGD ("Doesn't support ssl for mysql analyzer.\n");
        return dataLen;
    }

    /* Mysql packet after handshake  */
    if ((currSessionDetail->mstate != STATE_NOT_CONNECTED) &&
        (currSessionDetail->mstate != STATE_CLIENT_HANDSHAKE) &&
        (currSessionDetail->mstate != STATE_SECURE_AUTH)) {
        /* For incomplete mysql packet, return directly */
        if ((currSharedInfo->doCompress && (parseLeft < MYSQL_COMPRESSED_HEADER_SIZE)) ||
            (parseLeft < MYSQL_HEADER_SIZE))
            return 0;

        if ((direction == STREAM_FROM_CLIENT) && (currSessionDetail->mstate == STATE_SLEEP)) {
            /* New mysql request */
            resetMysqlSessionDetail (currSessionDetail);
            currSessionDetail->state = MYSQL_REQUEST_BEGIN;
            currSessionDetail->reqTime = timeVal2MilliSecond (currTime);
        } else if ((direction == STREAM_FROM_SERVER) && (currSessionDetail->state == MYSQL_REQUEST_COMPLETE)) {
            /* New mysql response */
            currSessionDetail->state = MYSQL_RESPONSE_BEGIN;
            currSessionDetail->respTimeBegin = timeVal2MilliSecond (currTime);
        }

        /* Compressed mysql packets */
        if (currSharedInfo->doCompress) {
            while (1) {
                /* Incomplete header of compressed packet */
                if (parseLeft < MYSQL_COMPRESSED_HEADER_SIZE)
                    break;

                compPkt = data + parseCount;
                compHdr = (mysqlCompHeaderPtr) compPkt;
                payloadLen = compHdr->payloadLen;
                compPayloadLen = compHdr->compPayloadLen;
                compPayload = (u_char *) (compPkt + MYSQL_COMPRESSED_HEADER_SIZE);
                compPktLen = MYSQL_COMPRESSED_HEADER_SIZE + compPayloadLen;

                /* Incomplete compressed packet */
                if (parseLeft < compPktLen)
                    break;

                if (payloadLen) {
                    /* Compressed pkt */
                    uncompPkt = (u_char *) malloc (payloadLen);
                    if (uncompPkt == NULL) {
                        LOGE ("Alloc memory for uncompPkt error: %s.\n", strerror (errno));
                        break;
                    }

                    uncompPayloadLen = payloadLen;
                    if (uncompress ((u_char *) uncompPkt, (u_long *) &uncompPayloadLen, compPayload, compPayloadLen) != Z_OK) {
                        LOGE ("Uncompress packet error.\n");
                        free (uncompPkt);
                        uncompPkt = NULL;
                        parseCount += compPktLen;
                        parseLeft -= compPktLen;
                        continue;
                    }
                } else {
                    uncompPkt = compPayload;
                    uncompPayloadLen = compPayloadLen;
                }

                /* Real sql parse */
                sqlParse (uncompPkt, uncompPayloadLen, direction);
                /* Free uncompressed packet buffer if any */
                if (payloadLen) {
                    free (uncompPkt);
                    uncompPkt = NULL;
                }

                parseCount += compPktLen;
                parseLeft -= compPktLen;
            }
        } else  /* Non Compressed mysql packets */
            parseCount = sqlParse (data, dataLen, direction);

        if (direction == STREAM_FROM_CLIENT)
            currSessionDetail->reqSize += parseCount;
        else
            currSessionDetail->respSize += parseCount;
    } else  /* Mysql handshake packets */
        parseCount = sqlParse (data, dataLen, direction);

    return parseCount;
}

static void
initMysqlSharedInstance (void) {
    /* -------------------------------------------------------------- */
    mysqlStateEventMatrix [STATE_NOT_CONNECTED].size = 1;
    mysqlStateEventMatrix [STATE_NOT_CONNECTED].event [0] = EVENT_SERVER_HANDSHAKE;
    mysqlStateEventMatrix [STATE_NOT_CONNECTED].nextState [0] = STATE_CLIENT_HANDSHAKE;
    mysqlStateEventMatrix [STATE_NOT_CONNECTED].handler [0] = &pktServerHandshake;

    /* -------------------------------------------------------------- */
    mysqlStateEventMatrix [STATE_CLIENT_HANDSHAKE].size = 1;
    mysqlStateEventMatrix [STATE_CLIENT_HANDSHAKE].event [0] = EVENT_CLIENT_HANDSHAKE;
    mysqlStateEventMatrix [STATE_CLIENT_HANDSHAKE].nextState [0] = STATE_SECURE_AUTH;
    mysqlStateEventMatrix [STATE_CLIENT_HANDSHAKE].handler [0] = &pktClientHandshake;

    /* -------------------------------------------------------------- */
    mysqlStateEventMatrix [STATE_SECURE_AUTH].size = 3;
    mysqlStateEventMatrix [STATE_SECURE_AUTH].event [0] = EVENT_SECURE_AUTH;
    mysqlStateEventMatrix [STATE_SECURE_AUTH].nextState [0] = STATE_SECURE_AUTH;
    mysqlStateEventMatrix [STATE_SECURE_AUTH].handler [0] = &pktSecureAuth;

    mysqlStateEventMatrix [STATE_SECURE_AUTH].event [1] = EVENT_OK;
    mysqlStateEventMatrix [STATE_SECURE_AUTH].nextState [1] = STATE_SLEEP;
    mysqlStateEventMatrix [STATE_SECURE_AUTH].handler [1] = &pktOk;

    mysqlStateEventMatrix [STATE_SECURE_AUTH].event [2] = EVENT_ERROR;
    mysqlStateEventMatrix [STATE_SECURE_AUTH].nextState [2] = STATE_SLEEP;
    mysqlStateEventMatrix [STATE_SECURE_AUTH].handler [2] = &pktError;

    /* -------------------------------------------------------------- */
    mysqlStateEventMatrix [STATE_SLEEP].size = 28;
    mysqlStateEventMatrix [STATE_SLEEP].event [0] = COM_SLEEP;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [0] = STATE_OK_OR_ERROR;
    mysqlStateEventMatrix [STATE_SLEEP].handler [0] = &pktComX;

    mysqlStateEventMatrix [STATE_SLEEP].event [1] = COM_QUIT;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [1] = STATE_NOT_CONNECTED;
    mysqlStateEventMatrix [STATE_SLEEP].handler [1] = &pktComX;

    mysqlStateEventMatrix [STATE_SLEEP].event [2] = COM_INIT_DB;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [2] = STATE_OK_OR_ERROR;
    mysqlStateEventMatrix [STATE_SLEEP].handler [2] = &pktComDB;

    mysqlStateEventMatrix [STATE_SLEEP].event [3] = COM_QUERY;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [3] = STATE_TXT_RS;
    mysqlStateEventMatrix [STATE_SLEEP].handler [3] = &pktQuery;

    mysqlStateEventMatrix [STATE_SLEEP].event [4] = COM_FIELD_LIST;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [4] = STATE_FIELD_LIST;
    mysqlStateEventMatrix [STATE_SLEEP].handler [4] = &pktFieldList;

    mysqlStateEventMatrix [STATE_SLEEP].event [5] = COM_CREATE_DB;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [5] = STATE_OK_OR_ERROR;
    mysqlStateEventMatrix [STATE_SLEEP].handler [5] = &pktComDB;

    mysqlStateEventMatrix [STATE_SLEEP].event [6] = COM_DROP_DB;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [6] = STATE_OK_OR_ERROR;
    mysqlStateEventMatrix [STATE_SLEEP].handler [6] = &pktComDB;

    mysqlStateEventMatrix [STATE_SLEEP].event [7] = COM_REFRESH;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [7] = STATE_OK_OR_ERROR;
    mysqlStateEventMatrix [STATE_SLEEP].handler [7] = &pktRefresh;

    mysqlStateEventMatrix [STATE_SLEEP].event [8] = COM_SHUTDOWN;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [8] = STATE_END_OR_ERROR;
    mysqlStateEventMatrix [STATE_SLEEP].handler [8] = &pktShutdown;

    mysqlStateEventMatrix [STATE_SLEEP].event [9] = COM_STATISTICS;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [9] = STATE_STATISTICS;
    mysqlStateEventMatrix [STATE_SLEEP].handler [9] = &pktComX;

    mysqlStateEventMatrix [STATE_SLEEP].event [10] = COM_PROCESS_INFO;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [10] = STATE_TXT_RS;
    mysqlStateEventMatrix [STATE_SLEEP].handler [10] = &pktComX;

    mysqlStateEventMatrix [STATE_SLEEP].event [11] = COM_CONNECT;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [11] = STATE_OK_OR_ERROR;
    mysqlStateEventMatrix [STATE_SLEEP].handler [11] = &pktComX;

    mysqlStateEventMatrix [STATE_SLEEP].event [12] = COM_PROCESS_KILL;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [12] = STATE_OK_OR_ERROR;
    mysqlStateEventMatrix [STATE_SLEEP].handler [12] = &pktProcessKill;

    mysqlStateEventMatrix [STATE_SLEEP].event [13] = COM_DEBUG;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [13] = STATE_END_OR_ERROR;
    mysqlStateEventMatrix [STATE_SLEEP].handler [13] = &pktComX;

    mysqlStateEventMatrix [STATE_SLEEP].event [14] = COM_PING;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [14] = STATE_PONG;
    mysqlStateEventMatrix [STATE_SLEEP].handler [14] = &pktComX;

    mysqlStateEventMatrix [STATE_SLEEP].event [15] = COM_TIME;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [15] = STATE_OK_OR_ERROR;
    mysqlStateEventMatrix [STATE_SLEEP].handler [15] = &pktComX;

    mysqlStateEventMatrix [STATE_SLEEP].event [16] = COM_DELAYED_INSERT;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [16] = STATE_OK_OR_ERROR;
    mysqlStateEventMatrix [STATE_SLEEP].handler [16] = &pktComX;

    mysqlStateEventMatrix [STATE_SLEEP].event [17] = COM_CHANGE_USER;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [17] = STATE_SECURE_AUTH;
    mysqlStateEventMatrix [STATE_SLEEP].handler [17] = &pktChangeUser;

    mysqlStateEventMatrix [STATE_SLEEP].event [18] = COM_CONNECT_OUT;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [18] = STATE_OK_OR_ERROR;
    mysqlStateEventMatrix [STATE_SLEEP].handler [18] = &pktComX;

    mysqlStateEventMatrix [STATE_SLEEP].event [19] = COM_REGISTER_SLAVE;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [19] = STATE_OK_OR_ERROR;
    mysqlStateEventMatrix [STATE_SLEEP].handler [19] = &pktRegisterSlave;

    mysqlStateEventMatrix [STATE_SLEEP].event [20] = COM_STMT_PREPARE;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [20] = STATE_STMT_META;
    mysqlStateEventMatrix [STATE_SLEEP].handler [20] = &pktStmtPrepare;

    mysqlStateEventMatrix [STATE_SLEEP].event [21] = COM_STMT_EXECUTE;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [21] = STATE_BIN_RS;
    mysqlStateEventMatrix [STATE_SLEEP].handler [21] = &pktStmtExec;

    mysqlStateEventMatrix [STATE_SLEEP].event [22] = COM_STMT_CLOSE;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [22] = STATE_SLEEP;
    mysqlStateEventMatrix [STATE_SLEEP].handler [22] = &pktStmtCloseOrReset;

    mysqlStateEventMatrix [STATE_SLEEP].event [23] = COM_STMT_RESET;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [23] = STATE_OK_OR_ERROR;
    mysqlStateEventMatrix [STATE_SLEEP].handler [23] = &pktStmtCloseOrReset;

    mysqlStateEventMatrix [STATE_SLEEP].event [24] = COM_SET_OPTION;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [24] = STATE_END_OR_ERROR;
    mysqlStateEventMatrix [STATE_SLEEP].handler [24] = &pktSetOption;

    mysqlStateEventMatrix [STATE_SLEEP].event [25] = COM_STMT_FETCH;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [25] = STATE_STMT_FETCH_RS;
    mysqlStateEventMatrix [STATE_SLEEP].handler [25] = &pktStmtFetch;

    mysqlStateEventMatrix [STATE_SLEEP].event [26] = COM_DAEMON;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [26] = STATE_OK_OR_ERROR;
    mysqlStateEventMatrix [STATE_SLEEP].handler [26] = &pktComX;

    mysqlStateEventMatrix [STATE_SLEEP].event [27] = COM_RESET_CONNECTION;
    mysqlStateEventMatrix [STATE_SLEEP].nextState [27] = STATE_OK_OR_ERROR;
    mysqlStateEventMatrix [STATE_SLEEP].handler [27] = &pktComX;

    /* -------------------------------------------------------------- */
    mysqlStateEventMatrix [STATE_PONG].size = 1;
    mysqlStateEventMatrix [STATE_PONG].event [0] = EVENT_OK;
    mysqlStateEventMatrix [STATE_PONG].nextState [0] = STATE_SLEEP;
    mysqlStateEventMatrix [STATE_PONG].handler [0] = &pktOk;
    
    /* -------------------------------------------------------------- */
    mysqlStateEventMatrix [STATE_OK_OR_ERROR].size = 2;
    mysqlStateEventMatrix [STATE_OK_OR_ERROR].event [0] = EVENT_OK;
    mysqlStateEventMatrix [STATE_OK_OR_ERROR].nextState [0] = STATE_SLEEP;
    mysqlStateEventMatrix [STATE_OK_OR_ERROR].handler [0] = &pktOk;

    mysqlStateEventMatrix [STATE_OK_OR_ERROR].event [1] = EVENT_ERROR;
    mysqlStateEventMatrix [STATE_OK_OR_ERROR].nextState [1] = STATE_SLEEP;
    mysqlStateEventMatrix [STATE_OK_OR_ERROR].handler [1] = &pktError;
    
    /* -------------------------------------------------------------- */
    mysqlStateEventMatrix [STATE_END_OR_ERROR].size = 2;
    mysqlStateEventMatrix [STATE_END_OR_ERROR].event [0] = EVENT_END;
    mysqlStateEventMatrix [STATE_END_OR_ERROR].nextState [0] = STATE_SLEEP;
    mysqlStateEventMatrix [STATE_END_OR_ERROR].handler [0] = &pktEnd;

    mysqlStateEventMatrix [STATE_END_OR_ERROR].event [1] = EVENT_ERROR;
    mysqlStateEventMatrix [STATE_END_OR_ERROR].nextState [1] = STATE_SLEEP;
    mysqlStateEventMatrix [STATE_END_OR_ERROR].handler [1] = &pktError;

    /* -------------------------------------------------------------- */
    mysqlStateEventMatrix [STATE_END].size = 1;
    mysqlStateEventMatrix [STATE_END].event [0] = EVENT_END;
    mysqlStateEventMatrix [STATE_END].nextState [0] = STATE_SLEEP;
    mysqlStateEventMatrix [STATE_END].handler [0] = &pktEnd;

    /* -------------------------------------------------------------- */
    mysqlStateEventMatrix [STATE_STATISTICS].size = 1;
    mysqlStateEventMatrix [STATE_STATISTICS].event [0] = EVENT_STATISTICS;
    mysqlStateEventMatrix [STATE_STATISTICS].nextState [0] = STATE_SLEEP;
    mysqlStateEventMatrix [STATE_STATISTICS].handler [0] = &pktStatistics;

    /* -------------------------------------------------------------- */
    mysqlStateEventMatrix [STATE_FIELD_LIST].size = 2;
    mysqlStateEventMatrix [STATE_FIELD_LIST].event [0] = EVENT_FIELD_LIST_FIELD;
    mysqlStateEventMatrix [STATE_FIELD_LIST].nextState [0] = STATE_FIELD_LIST;
    mysqlStateEventMatrix [STATE_FIELD_LIST].handler [0] = &pktField;

    mysqlStateEventMatrix [STATE_FIELD_LIST].event [1] = EVENT_END;
    mysqlStateEventMatrix [STATE_FIELD_LIST].nextState [1] = STATE_SLEEP;
    mysqlStateEventMatrix [STATE_FIELD_LIST].handler [1] = &pktEnd;
    
    /* -------------------------------------------------------------- */
    mysqlStateEventMatrix [STATE_TXT_RS].size = 3;
    mysqlStateEventMatrix [STATE_TXT_RS].event [0] = EVENT_NUM_FIELDS;
    mysqlStateEventMatrix [STATE_TXT_RS].nextState [0] = STATE_TXT_FIELD;
    mysqlStateEventMatrix [STATE_TXT_RS].handler [0] = &pktNFields;

    mysqlStateEventMatrix [STATE_TXT_RS].event [1] = EVENT_OK;
    mysqlStateEventMatrix [STATE_TXT_RS].nextState [1] = STATE_SLEEP;
    mysqlStateEventMatrix [STATE_TXT_RS].handler [1] = &pktOk;

    mysqlStateEventMatrix [STATE_TXT_RS].event [2] = EVENT_ERROR;
    mysqlStateEventMatrix [STATE_TXT_RS].nextState [2] = STATE_SLEEP;
    mysqlStateEventMatrix [STATE_TXT_RS].handler [2] = &pktError;
    
    /* -------------------------------------------------------------- */    
    mysqlStateEventMatrix [STATE_TXT_FIELD].size = 3;
    mysqlStateEventMatrix [STATE_TXT_FIELD].event [0] = EVENT_TXT_FIELD ;
    mysqlStateEventMatrix [STATE_TXT_FIELD].nextState [0] = STATE_TXT_FIELD;
    mysqlStateEventMatrix [STATE_TXT_FIELD].handler [0] = &pktField;

    mysqlStateEventMatrix [STATE_TXT_FIELD].event [1] = EVENT_END;
    mysqlStateEventMatrix [STATE_TXT_FIELD].nextState [1] = STATE_TXT_ROW;
    mysqlStateEventMatrix [STATE_TXT_FIELD].handler [1] = &pktEnd;

    mysqlStateEventMatrix [STATE_TXT_FIELD].event [2] = EVENT_TXT_ROW;
    mysqlStateEventMatrix [STATE_TXT_FIELD].nextState [2] = STATE_TXT_ROW;
    mysqlStateEventMatrix [STATE_TXT_FIELD].handler [2] = &pktTxtRow;

    /* -------------------------------------------------------------- */
    mysqlStateEventMatrix [STATE_TXT_ROW].size = 5;
    mysqlStateEventMatrix [STATE_TXT_ROW].event [0] = EVENT_TXT_ROW;
    mysqlStateEventMatrix [STATE_TXT_ROW].nextState [0] = STATE_TXT_ROW;
    mysqlStateEventMatrix [STATE_TXT_ROW].handler [0] = &pktTxtRow;

    mysqlStateEventMatrix [STATE_TXT_ROW].event [1] = EVENT_END;
    mysqlStateEventMatrix [STATE_TXT_ROW].nextState [1] = STATE_SLEEP;
    mysqlStateEventMatrix [STATE_TXT_ROW].handler [1] = &pktEnd;

    mysqlStateEventMatrix [STATE_TXT_ROW].event [2] = EVENT_END_WITH_MULTI_RESULT;
    mysqlStateEventMatrix [STATE_TXT_ROW].nextState [2] = STATE_TXT_RS;
    mysqlStateEventMatrix [STATE_TXT_ROW].handler [2] = &pktEnd;

    mysqlStateEventMatrix [STATE_TXT_ROW].event [3] = EVENT_OK;
    mysqlStateEventMatrix [STATE_TXT_ROW].nextState [3] = STATE_SLEEP;
    mysqlStateEventMatrix [STATE_TXT_ROW].handler [3] = &pktOk;
    
    mysqlStateEventMatrix [STATE_TXT_ROW].event [4] = EVENT_ERROR;
    mysqlStateEventMatrix [STATE_TXT_ROW].nextState [4] = STATE_SLEEP;
    mysqlStateEventMatrix [STATE_TXT_ROW].handler [4] = &pktError;

    /* -------------------------------------------------------------- */
    mysqlStateEventMatrix [STATE_BIN_RS].size = 3;
    mysqlStateEventMatrix [STATE_BIN_RS].event [0] = EVENT_NUM_FIELDS;
    mysqlStateEventMatrix [STATE_BIN_RS].nextState [0] = STATE_BIN_FIELD;
    mysqlStateEventMatrix [STATE_BIN_RS].handler [0] = &pktNFields;

    mysqlStateEventMatrix [STATE_BIN_RS].event [1] = EVENT_OK;
    mysqlStateEventMatrix [STATE_BIN_RS].nextState [1] = STATE_SLEEP;
    mysqlStateEventMatrix [STATE_BIN_RS].handler [1] = &pktOk;

    mysqlStateEventMatrix [STATE_BIN_RS].event [2] = EVENT_ERROR;
    mysqlStateEventMatrix [STATE_BIN_RS].nextState [2] = STATE_SLEEP;
    mysqlStateEventMatrix [STATE_BIN_RS].handler [2] = &pktError;
    
    /* -------------------------------------------------------------- */
    mysqlStateEventMatrix [STATE_BIN_FIELD].size = 2;
    mysqlStateEventMatrix [STATE_BIN_FIELD].event [0] = EVENT_BIN_FIELD;
    mysqlStateEventMatrix [STATE_BIN_FIELD].nextState [0] = STATE_BIN_FIELD;
    mysqlStateEventMatrix [STATE_BIN_FIELD].handler [0] = &pktField;

    mysqlStateEventMatrix [STATE_BIN_FIELD].event [1] = EVENT_END;
    mysqlStateEventMatrix [STATE_BIN_FIELD].nextState [1] = STATE_BIN_ROW;
    mysqlStateEventMatrix [STATE_BIN_FIELD].handler [1] = &pktEnd;

    /* -------------------------------------------------------------- */
    mysqlStateEventMatrix [STATE_BIN_ROW].size = 3;
    mysqlStateEventMatrix [STATE_BIN_ROW].event [0] = EVENT_BIN_ROW;
    mysqlStateEventMatrix [STATE_BIN_ROW].nextState [0] = STATE_BIN_ROW;
    mysqlStateEventMatrix [STATE_BIN_ROW].handler [0] = &pktBinRow;

    mysqlStateEventMatrix [STATE_BIN_ROW].event [1] = EVENT_END;
    mysqlStateEventMatrix [STATE_BIN_ROW].nextState [1] = STATE_SLEEP;
    mysqlStateEventMatrix [STATE_BIN_ROW].handler [1] = &pktEnd;

    mysqlStateEventMatrix [STATE_BIN_ROW].event [2] = EVENT_ERROR;
    mysqlStateEventMatrix [STATE_BIN_ROW].nextState [2] = STATE_SLEEP;
    mysqlStateEventMatrix [STATE_BIN_ROW].handler [2] = &pktError;

    /* -------------------------------------------------------------- */
    mysqlStateEventMatrix [STATE_STMT_META].size = 2;
    mysqlStateEventMatrix [STATE_STMT_META].event [0] = EVENT_STMT_META;
    mysqlStateEventMatrix [STATE_STMT_META].nextState [0] = STATE_STMT_PARAM;
    mysqlStateEventMatrix [STATE_STMT_META].handler [0] = &pktStmtMeta;

    mysqlStateEventMatrix [STATE_STMT_META].event [1] = EVENT_ERROR;
    mysqlStateEventMatrix [STATE_STMT_META].nextState [1] = STATE_SLEEP;
    mysqlStateEventMatrix [STATE_STMT_META].handler [1] = &pktError;

    /* -------------------------------------------------------------- */
    mysqlStateEventMatrix [STATE_STMT_PARAM].size = 2;
    mysqlStateEventMatrix [STATE_STMT_PARAM].event [0] = EVENT_STMT_PARAM;
    mysqlStateEventMatrix [STATE_STMT_PARAM].nextState [0] = STATE_STMT_PARAM;
    mysqlStateEventMatrix [STATE_STMT_PARAM].handler [0] = &pktField;

    mysqlStateEventMatrix [STATE_STMT_PARAM].event [1] = EVENT_END;
    mysqlStateEventMatrix [STATE_STMT_PARAM].nextState [1] = STATE_FIELD_LIST;
    mysqlStateEventMatrix [STATE_STMT_PARAM].handler [1] = &pktEnd;

    /* -------------------------------------------------------------- */
    mysqlStateEventMatrix [STATE_STMT_FETCH_RS].size = 3;
    mysqlStateEventMatrix [STATE_STMT_FETCH_RS].event [0] = EVENT_STMT_FETCH_RESULT;
    mysqlStateEventMatrix [STATE_STMT_FETCH_RS].nextState [0] = STATE_STMT_FETCH_RS;
    mysqlStateEventMatrix [STATE_STMT_FETCH_RS].handler [0] = &pktStmtFetchRS;

    mysqlStateEventMatrix [STATE_STMT_FETCH_RS].event [1] = EVENT_OK;
    mysqlStateEventMatrix [STATE_STMT_FETCH_RS].nextState [1] = STATE_SLEEP;
    mysqlStateEventMatrix [STATE_STMT_FETCH_RS].handler [1] = &pktOk;

    mysqlStateEventMatrix [STATE_STMT_FETCH_RS].event [2] = EVENT_ERROR;
    mysqlStateEventMatrix [STATE_STMT_FETCH_RS].nextState [2] = STATE_SLEEP;
    mysqlStateEventMatrix [STATE_STMT_FETCH_RS].handler [2] = &pktError;

    mysqlProtoDestroyOnceControl = PTHREAD_ONCE_INIT;
}

static void
destroyMysqlSharedInstance (void) {
    mysqlProtoInitOnceControl = PTHREAD_ONCE_INIT;
}

static int
initMysqlAnalyzer (void) {
    pthread_once (&mysqlProtoInitOnceControl, initMysqlSharedInstance);
    return 0;
}

static void
destroyMysqlAnalyzer (void) {
    pthread_once (&mysqlProtoDestroyOnceControl, destroyMysqlSharedInstance);
}

static int
initMysqlSharedinfo (mysqlSharedInfoPtr sharedInfo) {
    sharedInfo->protoVer = 0;
    sharedInfo->serverVer = NULL;
    sharedInfo->cliCaps = 0;
    sharedInfo->cliProtoIsV41 = false;
    sharedInfo->conId = 0;
    sharedInfo->maxPktSize = 0;
    sharedInfo->doCompress = false;
    sharedInfo->doSSL = false;
    sharedInfo->userName = NULL;

    return 0;
}

static void
destroyMysqlSharedinfo (mysqlSharedInfoPtr sharedInfo) {
    if (sharedInfo == NULL)
        return;

    free (sharedInfo->serverVer);
    free (sharedInfo->userName);
}

static void *
newMysqlSessionDetail (void) {
    int ret;
    mysqlSessionDetailPtr msd;

    msd = (mysqlSessionDetailPtr) malloc (sizeof (mysqlSessionDetail));
    if (msd == NULL)
        return NULL;

    ret = initMysqlSharedinfo (&msd->sharedInfo);
    if (ret < 0) {
        free (msd);
        return NULL;
    }
    msd->cmd = COM_UNKNOWN;
    msd->cmdCtxt.fieldsCount = 0;
    msd->cmdCtxt.fieldsRecv = 0;
    msd->showS2CTag = true;
    msd->mstate = STATE_NOT_CONNECTED;
    msd->seqId = 0;
    msd->reqStmt = NULL;
    msd->state = MYSQL_INIT;
    msd->errCode = 0;
    msd->sqlState = 0;
    msd->errMsg = NULL;
    msd->reqSize = 0;
    msd->respSize = 0;
    msd->reqTime = 0;
    msd->respTimeBegin = 0;
    msd->respTimeEnd = 0;
    return msd;
}

/* Reset mysql session detail */
static void
resetMysqlSessionDetail (mysqlSessionDetailPtr msd) {
    msd->cmd = COM_UNKNOWN;
    msd->cmdCtxt.fieldsCount = 0;
    msd->cmdCtxt.fieldsRecv = 0;
    msd->showS2CTag = true;
    free (msd->reqStmt);
    msd->reqStmt = NULL;
    msd->state = MYSQL_INIT;
    msd->errCode = 0;
    msd->sqlState = 0;
    free (msd->errMsg);
    msd->errMsg = NULL;
    msd->reqSize = 0;
    msd->respSize = 0;
    msd->reqTime = 0;
    msd->respTimeBegin = 0;
    msd->respTimeEnd = 0;
}

static void
freeMysqlSessionDetail (void *sd) {
    mysqlSessionDetailPtr msd;

    if (sd == NULL)
        return;

    msd = (mysqlSessionDetailPtr) sd;
    /* Destroy mysql sharedInfo state */
    destroyMysqlSharedinfo (&msd->sharedInfo);
    free (msd->reqStmt);
    free (msd->errMsg);

    free (msd);
}

static void *
newMysqlSessionBreakdown (void) {
    mysqlSessionBreakdownPtr msbd;

    msbd = (mysqlSessionBreakdownPtr) malloc (sizeof (mysqlSessionBreakdown));
    if (msbd == NULL)
        return NULL;

    msbd->serverVer = NULL;
    msbd->userName = NULL;
    msbd->conId = 0;
    msbd->reqStmt = NULL;
    msbd->state = MYSQL_BREAKDOWN_ERROR;
    msbd->errCode = 0;
    msbd->sqlState = 0;
    msbd->errMsg = NULL;
    msbd->reqSize = 0;
    msbd->respSize = 0;
    msbd->respLatency = 0;
    msbd->downloadLatency = 0;
    return msbd;
}

static void
freeMysqlSessionBreakdown (void *sbd) {
    mysqlSessionBreakdownPtr msbd;

    if (sbd == NULL)
        return;

    msbd = (mysqlSessionBreakdownPtr) sbd;
    free (msbd->serverVer);
    free (msbd->userName);
    free (msbd->reqStmt);
    free (msbd->errMsg);

    free (msbd);
}

static int
generateMysqlSessionBreakdown (void *sd, void *sbd) {
    mysqlSessionDetailPtr msd = (mysqlSessionDetailPtr) sd;
    mysqlSessionBreakdownPtr msbd = (mysqlSessionBreakdownPtr) sbd;

    if (currSharedInfo->serverVer) {
        msbd->serverVer = strdup (currSharedInfo->serverVer);
        if (msbd->serverVer == NULL) {
            LOGE ("Strdup mysql server version error: %s.\n", strerror (errno));
            return -1;
        }
    } else {
        LOGE ("Mysql server version is NULL.\n");
        return -1;
    }

    if (currSharedInfo->userName) {
        msbd->userName = strdup (currSharedInfo->userName);
        if (msbd->userName == NULL) {
            LOGE ("Strdup mysql userName error: %s.\n", strerror (errno));
            return -1;
        }
    } else {
        LOGE ("Mysql user name is NULL.\n");
        return -1;
    }

    msbd->conId = currSharedInfo->conId;

    /* For MYSQL_BREAKDOWN_RESET_TYPE4 case, reqStmt is NULL */
    if (msd->reqStmt && (msbd->state != MYSQL_BREAKDOWN_RESET_TYPE4)) {
        msbd->reqStmt = strdup (msd->reqStmt);
        if (msbd->reqStmt == NULL) {
            LOGE ("Strdup mysql request error: %s.\n", strerror (errno));
            return -1;
        }
    }

    switch (msd->state) {
        case MYSQL_RESPONSE_OK:
        case MYSQL_RESPONSE_ERROR:
            if (msd->state == MYSQL_RESPONSE_OK) {
                msbd->state = MYSQL_BREAKDOWN_OK;
                msbd->errCode = 0;
                msbd->sqlState = 0;
                msbd->errMsg = NULL;
            } else {
                msbd->state = MYSQL_BREAKDOWN_ERROR;
                msbd->errCode = msd->errCode;
                msbd->sqlState = msd->sqlState;
                if (msd->errMsg) {
                    msbd->errMsg = strdup (msd->errMsg);
                    if (msbd->errMsg == NULL) {
                        LOGE ("Strdup mysql error message error: %s.\n", strerror (errno));
                        return -1;
                    }
                } else {
                    LOGE ("Mysql errMsg is NULL.\n");
                    return -1;
                }
            }
            msbd->reqSize = msd->reqSize;
            msbd->respSize = msd->respSize;
            msbd->respLatency = (u_int) (msd->respTimeBegin - msd->reqTime);
            msbd->downloadLatency = (u_int) (msd->respTimeEnd - msd->respTimeBegin);
            break;

        case MYSQL_RESET_TYPE1:
        case MYSQL_RESET_TYPE2:
            if (msd->state == MYSQL_RESET_TYPE1)
                msbd->state = MYSQL_BREAKDOWN_RESET_TYPE1;
            else
                msbd->state = MYSQL_BREAKDOWN_RESET_TYPE2;
            msbd->errCode = 0;
            msbd->sqlState = 0;
            msbd->errMsg = NULL;
            msbd->reqSize = msd->reqSize;
            msbd->respSize = 0;
            msbd->respLatency = 0;
            msbd->downloadLatency = 0;
            break;

        case MYSQL_RESET_TYPE3:
            msbd->state = MYSQL_BREAKDOWN_RESET_TYPE3;
            msbd->errCode = 0;
            msbd->sqlState = 0;
            msbd->errMsg = NULL;
            msbd->reqSize = msd->reqSize;
            msbd->respSize = msd->respSize;
            msbd->respLatency = (u_int) (msd->respTimeBegin - msd->reqTime);
            msbd->downloadLatency = 0;
            break;

        case MYSQL_RESET_TYPE4:
            msbd->state = MYSQL_BREAKDOWN_RESET_TYPE4;
            msbd->errCode = 0;
            msbd->sqlState = 0;
            msbd->errMsg = NULL;
            msbd->reqSize = 0;
            msbd->respSize = 0;
            msbd->respLatency = 0;
            msbd->downloadLatency = 0;
            break;

        default:
            LOGE ("Wrong mysql state for breakdown.\n");
            return -1;
    }

    return 0;
}

static void
mysqlSessionBreakdown2Json (json_t *root, void *sd, void *sbd) {
    mysqlSessionBreakdownPtr msbd = (mysqlSessionBreakdownPtr) sbd;

    /* Mysql server version */
    json_object_set_new (root, MYSQL_SBKD_SERVER_VERSION, json_string (msbd->serverVer));
    /* Mysql user name */
    json_object_set_new (root, MYSQL_SBKD_USER_NAME, json_string (msbd->userName));
    /* Mysql connection id */
    json_object_set_new (root, MYSQL_SBKD_CONNECTION_ID, json_integer (msbd->conId));
    /* Mysql request statement */
    if (msbd->reqStmt)
        json_object_set_new (root, MYSQL_SBKD_REQUEST_STATEMENT, json_string (msbd->reqStmt));
    else
        json_object_set_new (root, MYSQL_SBKD_REQUEST_STATEMENT, json_string (""));
    /* Mysql state */
    json_object_set_new (root, MYSQL_SBKD_STATE, json_integer (msbd->state));
    /* Mysql error code */
    json_object_set_new (root, MYSQL_SBKD_ERROR_CODE, json_integer (msbd->errCode));
    /* Mysql sql state */
    json_object_set_new (root, MYSQL_SBKD_SQL_STATE, json_integer (msbd->sqlState));
    /* Mysql error message */
    if (msbd->errMsg)
        json_object_set_new (root, MYSQL_SBKD_ERROR_MESSAGE, json_string (msbd->errMsg));
    else
        json_object_set_new (root, MYSQL_SBKD_ERROR_MESSAGE, json_string (""));
    /* Mysql request size */
    json_object_set_new (root, MYSQL_SBKD_REQUEST_SIZE, json_integer (msbd->reqSize));
    /* Mysql response size */
    json_object_set_new (root, MYSQL_SBKD_RESPONSE_SIZE, json_integer (msbd->respSize));
    /* Mysql response latency */
    json_object_set_new (root, MYSQL_SBKD_RESPONSE_LATENCY, json_integer (msbd->respLatency));
    /* Mysql download latency */
    json_object_set_new (root, MYSQL_SBKD_DOWNLOAD_LATENCY, json_integer (msbd->downloadLatency));
}

static void
mysqlSessionProcessEstb (timeValPtr tm, void *sd) {
    return;
}

static void
mysqlSessionProcessUrgData (streamDirection direction, char urgData, timeValPtr tm, void *sd) {
    return;
}

static u_int
mysqlSessionProcessData (streamDirection direction, u_char *data, u_int dataLen,
                         timeValPtr tm, void *sd, sessionState *state) {
    u_int parseCount;

    currTime = tm;
    currSessionState = SESSION_ACTIVE;
    currSharedInfo = &((mysqlSessionDetailPtr) sd)->sharedInfo;
    currSessionDetail = (mysqlSessionDetailPtr) sd;

    parseCount = mysqlParserExecute (data, dataLen, direction);
    *state = currSessionState;

    return parseCount;
}

static void
mysqlSessionProcessReset (streamDirection direction, timeValPtr tm, void *sd) {
    mysqlSessionDetailPtr msd = (mysqlSessionDetailPtr) sd;

    if (msd->state == MYSQL_REQUEST_BEGIN)
        msd->state = MYSQL_RESET_TYPE1;
    else if (msd->state == MYSQL_REQUEST_COMPLETE)
        msd->state = MYSQL_RESET_TYPE2;
    else if (msd->state == MYSQL_RESPONSE_BEGIN)
        msd->state = MYSQL_RESET_TYPE3;
    else if (msd->state == MYSQL_INIT)
        msd->state = MYSQL_RESET_TYPE4;

    return;
}

static void
mysqlSessionProcessFin (streamDirection direction, timeValPtr tm, void *sd, sessionState *state) {
    return;
}

protoAnalyzer mysqlAnalyzer = {
    .proto = "MYSQL",
    .initProtoAnalyzer = initMysqlAnalyzer,
    .destroyProtoAnalyzer = destroyMysqlAnalyzer,
    .newSessionDetail = newMysqlSessionDetail,
    .freeSessionDetail = freeMysqlSessionDetail,
    .newSessionBreakdown = newMysqlSessionBreakdown,
    .freeSessionBreakdown = freeMysqlSessionBreakdown,
    .generateSessionBreakdown = generateMysqlSessionBreakdown,
    .sessionBreakdown2Json = mysqlSessionBreakdown2Json,
    .sessionProcessEstb = mysqlSessionProcessEstb,
    .sessionProcessUrgData = mysqlSessionProcessUrgData,
    .sessionProcessData = mysqlSessionProcessData,
    .sessionProcessReset = mysqlSessionProcessReset,
    .sessionProcessFin = mysqlSessionProcessFin
};
