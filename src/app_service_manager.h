#ifndef __AGENT_APP_SERVICE_MANAGER_H__
#define __AGENT_APP_SERVICE_MANAGER_H__

#include <stdlib.h>
#include "protocol.h"
#include "app_service.h"

/*========================Interfaces definition============================*/
protoType
lookupAppServiceProtoType (const char *key);
char *
getAppServicesFilter (void);
int
updateAppServiceManager (void);
void
resetAppServiceManager (void);
int
initAppServiceManager (void);
void
destroyAppServiceManager (void);
/*=======================Interfaces definition end=========================*/

#endif /* __AGENT_APP_SERVICE_MANAGER_H__ */
