#include <stdlib.h>
#include <pcap.h>
#include <czmq.h>
#include "util.h"
#include "properties.h"
#include "signals.h"
#include "log.h"
#include "zmq_hub.h"
#include "task_manager.h"
#include "app_service_manager.h"
#include "netdev.h"
#include "ip.h"
#include "raw_packet.h"
#include "raw_capture_service.h"

static pcap_t *pcapDev = NULL;
static int datalinkType = -1;

static u_long_long rawPktCaptureSize = 0;
static u_long_long rawPktCaptureStartTime = 0;
static u_long_long rawPktCaptureEndTime = 0;

static int
loopPcapDev (void) {
    int ret;
    char *filter;

    ret = loopNetDev ();
    if (ret < 0) {
        LOGE ("Loop netDev error.\n");
        return -1;
    } else if (ret == 1) {
        LOGI ("Loop netDev complete.\n");
        return 1;
    }

    filter = getAppServicesFilter ();
    if (filter == NULL) {
        LOGE ("Get application service filter error.\n");
        return -1;
    }

    ret = updateNetDevFilter (filter);
    free (filter);
    if (ret < 0) {
        LOGE ("Update net device filter error.\n");
        return -1;
    }

    pcapDev = getNetDevPcapDesc ();
    datalinkType = getNetDevDatalinkType ();
    return 0;
}

/*
 * Raw packet capture service.
 * Capture raw packet from mirror interface, then extract ip packet
 * from raw packet and send it to ip packet process service.
 */
void *
rawCaptureService (void *args) {
    int ret;
    void *ipPktSendSock;
    char *filter;
    struct pcap_pkthdr *capPktHdr;
    u_char *rawPkt;
    iphdrPtr iph;
    timeVal captureTime;
    zframe_t *frame;
    boolean loopComplete = false;

    /* Reset signals flag */
    resetSignalsFlag ();

    /* Init log context */
    ret = initLogContext (getPropertiesLogLevel ());
    if (ret < 0) {
        fprintf (stderr, "Init log context error.\n");
        goto exit;
    }

    /* Get net device pcap descriptor */
    pcapDev = getNetDevPcapDesc ();
    /* Get net device datalink type */
    datalinkType = getNetDevDatalinkType ();
    /* Get ipPktSendSock */
    ipPktSendSock = getIpPktSendSock ();

    /* Update application services filter */
    filter = getAppServicesFilter ();
    if (filter == NULL) {
        LOGE ("Get application services filter error.\n");
        goto destroyLogContext;
    }
    ret = updateNetDevFilter (filter);
    if (ret < 0) {
        LOGE ("Update application services filter error.\n");
        free (filter);
        goto destroyLogContext;
    }
    LOGI ("Update BPF filter with: %s\n", filter);
    free (filter);

    /* Init rawPktCaptureSize and rawPktCaptureStartTime */
    rawPktCaptureSize = 0;
    rawPktCaptureStartTime = getSysTime ();

    while (!SIGUSR1IsInterrupted () && !zctx_interrupted) {
        ret = pcap_next_ex (pcapDev, &capPktHdr, (const u_char **) &rawPkt);
        if (ret == 1) {
            /* Filter out incomplete raw packet */
            if (capPktHdr->caplen != capPktHdr->len)
                continue;
            rawPktCaptureSize += capPktHdr->caplen;

            /* Get ip packet */
            iph = (iphdrPtr) getIpPacket (rawPkt, datalinkType);
            if (iph == NULL)
                continue;

            /* Get packet capture timestamp */
            captureTime.tvSec = htonll (capPktHdr->ts.tv_sec);
            captureTime.tvUsec = htonll (capPktHdr->ts.tv_usec);

            /* Send capture timestamp zframe */
            frame = zframe_new (&captureTime, sizeof (timeVal));
            if (frame == NULL) {
                LOGE ("Create packet timestamp zframe error.\n");
                continue;
            }
            ret = zframe_send (&frame, ipPktSendSock, ZFRAME_MORE);
            if (ret < 0) {
                LOGE ("Send packet timestamp zframe error.\n");
                continue;
            }

            /* Send ip packet zframe */
            frame = zframe_new (iph, ntohs (iph->ipLen));
            if (frame == NULL) {
                LOGE ("Create ip packet zframe error.\n");
                continue;
            }
            ret = zframe_send (&frame, ipPktSendSock, 0);
            if (ret < 0) {
                LOGE ("Send ip packet zframe error.\n");
                continue;
            }
        } else if (ret == -1) {
            LOGE ("Capture raw packets with fatal error.\n");
            break;
        } else if (ret == -2) {
            ret = loopPcapDev ();
            if (ret) {
                loopComplete = true;
                break;
            }
        }
    }

    rawPktCaptureEndTime = getSysTime ();
    /* Show raw packets capture statistics info */
    LOGI ("==Capture raw packets complete==\n"
          "--size: %lf KB\n--interval: %llu ms\n--rate: %lf MB/s\n",
          ((double) rawPktCaptureSize / 1024),
          (rawPktCaptureEndTime - rawPktCaptureStartTime),
          (((double) rawPktCaptureSize / (128 * 1024)) /
           ((double) (rawPktCaptureEndTime - rawPktCaptureStartTime) / 1000)));

    LOGI ("RawCaptureService will exit ... .. .\n");
destroyLogContext:
    destroyLogContext ();
exit:
    if (loopComplete)
        sendTaskStatus (TASK_STATUS_EXIT_NORMALLY);
    else if (!SIGUSR1IsInterrupted ())
        sendTaskStatus (TASK_STATUS_EXIT_ABNORMALLY);

    return NULL;
}
