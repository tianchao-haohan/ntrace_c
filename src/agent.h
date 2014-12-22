#ifndef __AGENT_AGENT_H__
#define __AGENT_AGENT_H__

#include <sys/types.h>
#include <pcap.h>
#include "util.h"

/* Agent management command */
#define AGENT_MANAGEMENT_CMD_ADD_AGENT "add_agent"
#define AGENT_MANAGEMENT_CMD_REMOVE_AGENT "remove_agent"
#define AGENT_MANAGEMENT_CMD_START_AGENT "start_agent"
#define AGENT_MANAGEMENT_CMD_STOP_AGENT "stop_agent"
#define AGENT_MANAGEMENT_CMD_HEARTBEAT "heartbeat"
#define AGENT_MANAGEMENT_CMD_PUSH_PROFILE "push_profile"

/* Agent management success response */
#define AGENT_MANAGEMENT_RESPONSE_SUCCESS 0
#define AGENT_MANAGEMENT_RESPONSE_SUCCESS_MESSAGE "{\"code\":0}"

/* Agent management error response */
#define AGENT_MANAGEMENT_RESPONSE_ERROR 1
#define AGENT_MANAGEMENT_RESPONSE_ERROR_MESSAGE "{\"code\":1}"

/* Agent management response port */
#define AGENT_MANAGEMENT_RESPONSE_PORT 59000

typedef struct _agentConfig agentConfig;
typedef agentConfig *agentConfigPtr;

/* Agent configuration */
struct _agentConfig {
    boolean daemonMode;                    /**< Run as daemon */
    char *mirrorInterface;              /**< Mirror interface */
    u_int logLevel;                     /**< Log level */
};

typedef struct _dispatchRouter dispatchRouter;
typedef dispatchRouter *dispatchRouterPtr;

struct _dispatchRouter {
    u_int dispatchThreads;              /**< Dispatch threads number */
    void **pushSocks;                   /**< Dispatch push sockets */
    void **pullSocks;                   /**< Dispatch pull sockets */
};

#endif /* __AGENT_AGENT_H__ */
