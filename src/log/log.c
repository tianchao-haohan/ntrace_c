#include <stdio.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <czmq.h>
#include "util.h"
#include "log_service.h"
#include "zmq_hub.h"
#include "log.h"

#define MAX_LOG_MESSAGE_LENGTH 4096

typedef struct _logContext logContext;
typedef logContext *logContextPtr;

struct _logContext {
    zctx_t *zmqCtxt;                    /**< Zmq context */
    void *logSock;                      /**< Log zmq sock */
    u_int logLevel;                     /**< Log level */
};

/* Thread local log context */
static __thread logContextPtr logCtxtInstance = NULL;

/*
 * @brief Format log message and send log message to log service.
 *
 * @param file Source file name
 * @param line Line number
 * @param func Function name
 * @param msg Real log message
 */
void
doLog (u_char logLevel, const char *file, u_int line, const char *func, char *msg, ...) {
    int ret;
    time_t seconds;
    struct tm *localTime;
    char timeStr [32];
    va_list va;
    char flag;
    char *fileName;
    /* Thread local message buffer */
    static __thread char tmp [MAX_LOG_MESSAGE_LENGTH];
    static __thread char buf [MAX_LOG_MESSAGE_LENGTH];
    static __thread char logLevelStr [16];
    zframe_t *frame;

    if (logCtxtInstance == NULL) {
        fprintf (stderr, "Log context has not been initialized.\n");
        return;
    }

    seconds = time (NULL);
    localTime = localtime (&seconds);
    snprintf (timeStr, sizeof (timeStr), "%04d-%02d-%02d %02d:%02d:%02d",
              (localTime->tm_year + 1900), localTime->tm_mon + 1, localTime->tm_mday,
              localTime->tm_hour, localTime->tm_min, localTime->tm_sec);

    va_start (va, msg);
    vsnprintf (tmp, sizeof (tmp), msg, va);
    va_end (va);

    switch (logLevel) {
        case LOG_ERR_LEVEL:
            snprintf (logLevelStr, sizeof (logLevelStr), "ERROR");
            break;

        case LOG_WARNING_LEVEL:
            snprintf (logLevelStr, sizeof (logLevelStr), "WARNING");
            break;

        case LOG_INFO_LEVEL:
            snprintf (logLevelStr, sizeof (logLevelStr), "INFO");
            break;

        case LOG_DEBUG_LEVEL:
            snprintf (logLevelStr, sizeof (logLevelStr), "DEBUG");
            break;

        default:
            fprintf (stderr, "Unknown log level!\n");
            return;
    }

    if (logLevel <= logCtxtInstance->logLevel)
        flag = 'a';
    else
        flag = 'n';

    fileName = strrchr (file, '/') + 1;
    snprintf (buf, sizeof (buf), "%c%s [thread:%u] %s file=%s (line=%u, func=%s): %s",
              flag, timeStr, gettid (), logLevelStr, fileName, line, func, tmp);
    buf [MAX_LOG_MESSAGE_LENGTH - 1] = 0;

    frame = zframe_new ((void *) buf, strlen (buf));
    if (frame == NULL) {
        fprintf (stderr, "Create zframe for log message error.\n");
        return;
    }
    ret = zframe_send (&frame, logCtxtInstance->logSock, 0);
    if (ret < 0) {
        fprintf (stderr, "Send log message error.\n");
        zframe_destroy (&frame);
        return;
    }
}

/*
 * @brief Init log context.
 *        It will create a thread local log context, every thread want to
 *        use log function must init log context first.
 * @param logLevel Log level
 *
 * @return 0 if success else -1
 */
int
initLogContext (u_int logLevel) {
    int ret;

    logCtxtInstance = (logContextPtr) malloc (sizeof (logContext));
    if (logCtxtInstance == NULL)
        return -1;

    logCtxtInstance->zmqCtxt = zctx_new ();
    if (logCtxtInstance->zmqCtxt == NULL) {
        free (logCtxtInstance);
        return -1;
    }
    zctx_set_linger (logCtxtInstance->zmqCtxt, 0);

    logCtxtInstance->logSock = zsocket_new (logCtxtInstance->zmqCtxt, ZMQ_PUSH);
    if (logCtxtInstance->logSock == NULL) {
        zctx_destroy (&logCtxtInstance->zmqCtxt);
        free (logCtxtInstance);
        return -1;
    }

    ret = zsocket_connect (logCtxtInstance->logSock,
                           "tcp://localhost:%u", LOG_SERVICE_LOG_RECV_PORT);
    if (ret < 0) {
        zctx_destroy (&logCtxtInstance->zmqCtxt);
        free (logCtxtInstance);
        return -1;
    }

    if (logLevel > LOG_DEBUG_LEVEL || logLevel < LOG_ERR_LEVEL)
        logCtxtInstance->logLevel = LOG_DEBUG_LEVEL;
    else
        logCtxtInstance->logLevel = logLevel;

    return 0;
}

/* Destroy log context */
void
destroyLogContext (void) {
    /* Wait for log send out completely */
    usleep (100000);
    zctx_destroy (&logCtxtInstance->zmqCtxt);
    free (logCtxtInstance);
    logCtxtInstance = NULL;
}
