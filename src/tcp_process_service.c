#include <stdlib.h>
#include "util.h"
#include "properties.h"
#include "signals.h"
#include "log.h"
#include "zmq_hub.h"
#include "task_manager.h"
#include "ip.h"
#include "tcp_packet.h"
#include "publish_session_breakdown.h"
#include "tcp_process_service.h"

/*
 * Tcp packet process service.
 * Read ip packets send by ipPktProcessService, then do tcp process and
 * send session breakdown to session breakdown sink service.
 */
void *
tcpProcessService (void *args) {
    int ret;
    u_int dispatchIndex;
    void *tcpPktRecvSock;
    void *tcpBreakdownSendSock;
    zframe_t *tmFrame = NULL;
    zframe_t *ipPktFrame = NULL;
    timeValPtr tm;
    iphdrPtr iph;

    dispatchIndex = *((u_int *) args);
    tcpPktRecvSock = getTcpPktRecvSock (dispatchIndex);
    tcpBreakdownSendSock = getTcpBreakdownSendSock (dispatchIndex);

    /* Reset signals flag */
    resetSignalsFlag ();

    /* Init log context */
    ret = initLogContext (getPropertiesLogLevel ());
    if (ret < 0) {
        fprintf (stderr, "Init log context error.\n");
        goto exit;
    }

    /* Init tcp context */
    ret = initTcp (publishSessionBreakdown, tcpBreakdownSendSock);
    if (ret < 0) {
        LOGE ("Init tcp context error.\n");
        goto destroyLogContext;
    }

    while (!SIGUSR1IsInterrupted ()) {
        /* Receive timestamp zframe */
        if (tmFrame == NULL) {
            tmFrame = zframe_recv (tcpPktRecvSock);
            if (tmFrame == NULL) {
                if (!SIGUSR1IsInterrupted ())
                    LOGE ("Receive timestamp zframe fatal error.\n");
                break;
            } else if (!zframe_more (tmFrame)) {
                zframe_destroy (&tmFrame);
                continue;
            }
        }

        /* Receive ip packet zframe */
        ipPktFrame = zframe_recv (tcpPktRecvSock);
        if (ipPktFrame == NULL) {
            if (!SIGUSR1IsInterrupted ())
                LOGE ("Receive ip packet zframe fatal error.\n");
            zframe_destroy (&tmFrame);
            break;
        } else if (zframe_more (ipPktFrame)) {
            zframe_destroy (&tmFrame);
            tmFrame = ipPktFrame;
            ipPktFrame = NULL;
            continue;
        }

        tm = (timeValPtr) zframe_data (tmFrame);
        iph = (iphdrPtr) zframe_data (ipPktFrame);

        /* Do tcp process */
        tcpProcess (iph, tm);

        /* Free zframe */
        zframe_destroy (&tmFrame);
        zframe_destroy (&ipPktFrame);
    }

    LOGI ("TcpPktProcessService will exit ... .. .\n");
    destroyTcp ();
destroyLogContext:
    destroyLogContext ();
exit:
    if (!SIGUSR1IsInterrupted ())
        sendTaskStatus (TASK_STATUS_EXIT);

    return NULL;
}
