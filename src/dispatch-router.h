#ifndef __AGENT_DISPATCH_ROUTER_H__
#define __AGENT_DISPATCH_ROUTER_H__

#include <sys/types.h>

typedef struct _router router;
typedef router *routerPtr;

struct _router {
    u_int id;
    void *pushSock;
};

typedef struct _dispatchRouter dispatchRouter;
typedef dispatchRouter *dispatchRouterPtr;

struct _dispatchRouter {
    u_int pktParsingThreads;
    routerPtr routers;
};

typedef void * (*dispatchRoutine) (void *args);

/*========================Interfaces definition============================*/
void
routerDispatch (struct ip *iphdr, timeValPtr tm);
int
initDispatchRouter (u_int pktParsingThreads, dispatchRoutine routine, const char *routerAddress);
void
destroyDispatchRouter (void);
/*=======================Interfaces definition end=========================*/

#endif /* __AGENT_DISPATCH_ROUTER_H__ */
