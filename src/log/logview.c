#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <czmq.h>
#include <string.h>
#include "util.h"
#include "log.h"
#include "log_service.h"

/* Log server ip */
static char *logServerIp = NULL;
/* Display log with detail info */
static boolean showInDetail  = false;
/* Log level to filter */
static char *logLevel = NULL;

static zctx_t *zmqContext = NULL;
static void *subSock = NULL;

/* Check log level is valid */
static boolean
checkLogLevel (const char *logLevel) {
    if (strEqual ("ERR", logLevel) ||
        strEqual ("WARNING", logLevel) ||
        strEqual ("INFO", logLevel) ||
        strEqual ("DEBUG", logLevel))
        return true;
    else
        return false;
}

/* Logview options */
static struct option logviewOptions [] = {
    {"server", required_argument, NULL, 's'},
    {"level", required_argument, NULL, 'l'},
    {"verbose", no_argument, NULL, 'v'},
    {"help", no_argument, NULL, 'h'},
    {NULL, no_argument, NULL, 0}
};

static void
showHelp (const char *cmd) {
    const char *cmdName, *tmp;

    tmp = strrchr (cmd, '/');
    if (tmp)
        cmdName = tmp + 1;
    else
        cmdName = cmd;

    printf ("Usage: %s [options]\n"
            "       %s [-h]\n"
            "Options:\n"
            "  -s|--server <ip>, ip addr of logd server\n"
            "  -l|--level <logLevel>, optional log level: ERR, WARNING, INFO, DEBUG\n"
            "  -v|--verbose, display log in detail\n"
            "  -h|--help, help info\n",
            cmdName, cmdName);
}

/* Parse command line */
static int
parseCmdline (int argc, char *argv []) {
    int ret = 0;
    char option;

    while ((option = getopt_long (argc, argv, "s:p:l:vh?", logviewOptions, NULL)) != -1) {
        switch (option) {
            case 's':
                logServerIp = strdup (optarg);
                if (logServerIp == NULL)
                    return -1;
                break;

            case 'l':
                logLevel = strdup (optarg);
                if (logLevel == NULL)
                    return -1;
                ret = checkLogLevel (logLevel);
                if (ret < 0) {
                    fprintf (stderr, "Wrong log level.\n");
                    ret = -1;
                }
                break;

            case 'v':
                showInDetail = 1;
                break;

            case 'h':
                showHelp (argv [0]);
                exit (0);

            default:
                return -1;
        }
    }

    return ret;
}

int
main (int argc, char *argv []) {
    int ret;
    char *logMsg, *realLogMsg;

    /* Parse command line */
    ret = parseCmdline (argc, argv);
    if (ret < 0) {
        showHelp (argv [0]);
        return -1;
    }

    /* Init zmq context */
    zmqContext = zctx_new ();
    if (zmqContext == NULL)
        return -1;
    subSock = zsocket_new (zmqContext, ZMQ_SUB);
    if (subSock == NULL) {
        zctx_destroy (&zmqContext);
        return -1;
    }
    ret = zsocket_connect (subSock, "tcp://%s:%d", logServerIp ? logServerIp : "localhost", LOG_SERVICE_LOG_PUBLISH_PORT);
    if (ret < 0) {
        zctx_destroy (&zmqContext);
        return -1;
    }
    zsocket_set_subscribe (subSock, "");

    while (!zctx_interrupted) {
        logMsg = zstr_recv (subSock);
        
        if (logMsg && ((logLevel == NULL) ||
                       (logLevel && strstr (logMsg, logLevel)))) {
            if (showInDetail)
                realLogMsg = logMsg;
            else
                realLogMsg = strstr (logMsg, "): ") + strlen ("): ");
            printf ("%s", realLogMsg);
            free (logMsg);
        }
    }

    zctx_destroy (&zmqContext);
    return 0;
}
