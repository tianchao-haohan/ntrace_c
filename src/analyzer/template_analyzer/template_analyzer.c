#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <jansson.h>
#include <wda/util.h>
#include <wda/logger.h>
#include <wda/proto_analyzer.h>
#include "template_analyzer.h"

static int
initTemplateAnalyzer (void) {
    LOGD ("Init template analyzer.\n");
    return 0;
}

static void
destroyTemplateAnalyzer (void) {
    LOGD ("Destroy template analyzer.\n");
    return;
}

static void *
newTemplateSessionDetail (void) {
    templateSessionDetailPtr dsd;

    dsd = (templateSessionDetailPtr) malloc (sizeof (templateSessionDetail));
    if (dsd == NULL)
        return NULL;

    dsd->exchangeSize = 0;
    dsd->serverTimeBegin = 0;
    dsd->serverTimeEnd = 0;
    return dsd;
}

static void
freeTemplateSessionDetail (void *sd) {
    if (sd == NULL)
        return;

    free (sd);
}

static void *
newTemplateSessionBreakdown (void) {
    templateSessionBreakdownPtr dsbd;

    dsbd = (templateSessionBreakdownPtr) malloc (sizeof (templateSessionBreakdown));
    if (dsbd == NULL) {
        LOGE ("Alloc templateSessionBreakdown error.\n");
        return NULL;
    }

    dsbd->exchangeSize = 0;
    dsbd->serverLatency = 0;
    return dsbd;
}

static void
freeTemplateSessionBreakdown (void *sbd) {
    if (sbd == NULL)
        return;

    free (sbd);
}

static int
generateTemplateSessionBreakdown (void *sd, void *sbd) {
    templateSessionDetailPtr dsd = (templateSessionDetailPtr) sd;
    templateSessionBreakdownPtr dsbd = (templateSessionBreakdownPtr) sbd;

    dsbd->exchangeSize = dsd->exchangeSize;
    dsbd->serverLatency = (u_int) (dsd->serverTimeEnd - dsd->serverTimeBegin);

    return 0;
}

static void
templateSessionBreakdown2Json (json_t *root, void *sd, void *sbd) {
    templateSessionBreakdownPtr dsbd = (templateSessionBreakdownPtr) sbd;

    json_object_set_new (root, TEMPLATE_SBKD_EXCHANGE_SIZE, json_integer (dsbd->exchangeSize));
    json_object_set_new (root, TEMPLATE_SBKD_SERVER_LATENCY, json_integer (dsbd->serverLatency));
}

static void
templateSessionProcessEstb (void *sd, timeValPtr tm) {
    templateSessionDetailPtr dsd = (templateSessionDetailPtr) sd;

    dsd->serverTimeBegin = timeVal2MilliSecond (tm);
}

static void
templateSessionProcessUrgData (streamDirection direction, char urgData, void *sd, timeValPtr tm) {
    return;
}

static u_int
templateSessionProcessData (streamDirection direction, u_char *data, u_int dataLen, void *sd,
                           timeValPtr tm, sessionState *state) {
    templateSessionDetailPtr dsd = (templateSessionDetailPtr) sd;

    dsd->exchangeSize += dataLen;
    return dataLen;
}

static void
templateSessionProcessReset (streamDirection direction, void *sd, timeValPtr tm) {
    templateSessionDetailPtr dsd = (templateSessionDetailPtr) sd;

    dsd->serverTimeEnd = timeVal2MilliSecond (tm);
}

static void
templateSessionProcessFin (streamDirection direction, void *sd, timeValPtr tm, sessionState *state) {
    templateSessionDetailPtr dsd = (templateSessionDetailPtr) sd;

    if (dsd->serverTimeEnd == 0)
        dsd->serverTimeEnd = timeVal2MilliSecond (tm);
    else {
        dsd->serverTimeEnd = timeVal2MilliSecond (tm);
        *state = SESSION_DONE;
    }
}

protoAnalyzer analyzer = {
    .proto = "TEMPLATE",
    .initProtoAnalyzer = initTemplateAnalyzer,
    .destroyProtoAnalyzer = destroyTemplateAnalyzer,
    .newSessionDetail = newTemplateSessionDetail,
    .freeSessionDetail = freeTemplateSessionDetail,
    .newSessionBreakdown = newTemplateSessionBreakdown,
    .freeSessionBreakdown = freeTemplateSessionBreakdown,
    .generateSessionBreakdown = generateTemplateSessionBreakdown,
    .sessionBreakdown2Json = templateSessionBreakdown2Json,
    .sessionProcessEstb = templateSessionProcessEstb,
    .sessionProcessUrgData = templateSessionProcessUrgData,
    .sessionProcessData = templateSessionProcessData,
    .sessionProcessReset = templateSessionProcessReset,
    .sessionProcessFin = templateSessionProcessFin
};