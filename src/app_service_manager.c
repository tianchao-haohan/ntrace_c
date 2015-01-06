#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <jansson.h>
#include <pthread.h>
#include "util.h"
#include "hash.h"
#include "logger.h"
#include "runtime_context.h"
#include "app_service_manager.h"

/* Application service BPF ip fragment filter */
#define BPF_IP_FRAGMENT_FILTER "(tcp and (ip[6] & 0x20 != 0 or (ip[6] & 0x20 = 0 and ip[6:2] & 0x1fff != 0)))"
/* Application service BPF filter */
#define APP_SERVICE_BPF_FILTER "(ip host %s and (tcp port %u or %s)) or "
/* Application service BPF filter length */
#define APP_SERVICE_BPF_FILTER_LENGTH 256

static pthread_rwlock_t appServiceHashTableMasterRWLock;
static hashTablePtr appServiceHashTableMaster = NULL;
static hashTablePtr appServiceHashTableSlave = NULL;

protoAnalyzerPtr
getAppServiceProtoAnalyzer (const char *key) {
    appServicePtr svc;
    protoAnalyzerPtr analyzer;

    if (key ==  NULL)
        return NULL;

    pthread_rwlock_rdlock (&appServiceHashTableMasterRWLock);
    svc = (appServicePtr) hashLookup (appServiceHashTableMaster, key);
    if (svc == NULL)
        analyzer = NULL;
    else
        analyzer = svc->analyzer;
    pthread_rwlock_unlock (&appServiceHashTableMasterRWLock);

    return analyzer;
}

static int
generateFilterFromEachAppService (void *data, void *args) {
    u_int len;
    appServicePtr svc = (appServicePtr) data;
    char *filter = (char *) args;

    len = strlen (filter);
    snprintf (filter + len, APP_SERVICE_BPF_FILTER_LENGTH, APP_SERVICE_BPF_FILTER, svc->ip, svc->port, BPF_IP_FRAGMENT_FILTER);
    return 0;
}

char *
getAppServicesFilter (void) {
    int ret;
    u_int svcNum;
    char *filter;
    u_int filterLen;

    pthread_rwlock_rdlock (&appServiceHashTableMasterRWLock);
    svcNum = hashSize (appServiceHashTableMaster);
    filterLen = APP_SERVICE_BPF_FILTER_LENGTH * (svcNum + 1);
    filter = (char *) malloc (filterLen);
    if (filter == NULL) {
        LOGE ("Alloc filter buffer error: %s.\n", strerror (errno));
        pthread_rwlock_unlock (&appServiceHashTableMasterRWLock);
        return NULL;
    }
    memset (filter, 0, filterLen);

    ret = hashForEachItemDo (appServiceHashTableMaster, generateFilterFromEachAppService, filter);
    if (ret < 0) {
        LOGE ("Generate BPF filter from each application service error.\n");
        free (filter);
        pthread_rwlock_unlock (&appServiceHashTableMasterRWLock);
        return NULL;
    }

    strcat (filter, "icmp");
    pthread_rwlock_unlock (&appServiceHashTableMasterRWLock);
    return filter;
}

static void
swapAppServiceMap (void) {
    hashTablePtr tmp;

    tmp = appServiceHashTableMaster;
    pthread_rwlock_wrlock (&appServiceHashTableMasterRWLock);
    appServiceHashTableMaster = appServiceHashTableSlave;
    pthread_rwlock_unlock (&appServiceHashTableMasterRWLock);
    appServiceHashTableSlave = tmp;
}

static int
addAppService (appServicePtr svc) {
    int ret;
    char key [32];

    snprintf (key, sizeof (key), "%s:%d", svc->ip, svc->port);
    ret = hashInsert (appServiceHashTableSlave, key, svc, freeAppServiceForHash);
    if (ret < 0) {
        LOGE ("Insert new appService: %u error\n", svc->id);
        return -1;
    }

    return 0;
}

int
updateAppServiceManager (void) {
    int ret;
    u_int i, appServicesCount;
    appServicePtr tmp, svc, *appServiceArray;

    /* Cleanup slave application service hash table */
    hashClean (appServiceHashTableSlave);

    appServiceArray = getAppServices ();
    appServicesCount = getAppServicesCount ();
    for (i = 0; i <  appServicesCount; i ++) {
        tmp = appServiceArray [i];
        svc = copyAppService (tmp);
        if (svc == NULL) {
            LOGE ("Copy appService error.\n");
            return -1;
        }

        ret = addAppService (svc);
        if (ret < 0) {
            LOGE ("Add appService error.\n");
            return -1;
        }
    }
    swapAppServiceMap ();

    return 0;
}

void
resetAppServiceManager (void) {
    hashClean (appServiceHashTableSlave);
    swapAppServiceMap ();
    hashClean (appServiceHashTableSlave);
}

int
initAppServiceManager (void) {
    int ret;

    ret = pthread_rwlock_init (&appServiceHashTableMasterRWLock, NULL);
    if (ret) {
        LOGE ("Init appServiceHashTableMasterRWLock error.\n");
        return -1;
    }
    
    appServiceHashTableMaster = hashNew (0);
    if (appServiceHashTableMaster == NULL) {
        LOGE ("Create appServiceHashTableMaster error.\n");
        goto destroyAppServiceHashTableMasterRWLock;
    }

    appServiceHashTableSlave = hashNew (0);
    if (appServiceHashTableSlave == NULL) {
        LOGE ("Create appServiceHashTableSlave error.\n");
        goto destroyAppServiceHashTableMaster;
    }

    ret = updateAppServiceManager ();
    if (ret < 0) {
        LOGE ("Update application service manager error.\n");
        goto destroyAppServiceHashTableSlave;
    }

    return 0;
    
destroyAppServiceHashTableSlave:
    hashDestroy (appServiceHashTableSlave);
    appServiceHashTableSlave = NULL;
destroyAppServiceHashTableMaster:
    hashDestroy (appServiceHashTableMaster);
    appServiceHashTableMaster = NULL;
destroyAppServiceHashTableMasterRWLock:
    pthread_rwlock_destroy (&appServiceHashTableMasterRWLock);
    return -1;
}

void
destroyAppServiceManager (void) {
    pthread_rwlock_destroy (&appServiceHashTableMasterRWLock);
    hashDestroy (appServiceHashTableMaster);
    appServiceHashTableMaster = NULL;
    hashDestroy (appServiceHashTableSlave);
    appServiceHashTableSlave = NULL;
}
