#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/ip.h>
#include <czmq.h>
#include <ini_config.h>
#include <jansson.h>
#include <locale.h>
#include "config.h"
#include "list.h"
#include "hash.h"
#include "log.h"
#include "util.h"
#include "service-manager.h.h"
#include "router.h"
#include "ip-packet.h"
#include "tcp-packet.h"
#include "agent.h"

#define AGENT_CONTROL_RESPONSE_SUCCESS 0
#define AGENT_CONTROL_RESPONSE_FAILURE 1

/* Global agent parameters */
static agentParams agentParameters = {
    .daemonMode = 0,
    .mirrorInterface = NULL,
    .logLevel = 0,
};

/* Agent state cache instance */
static agentStateCache agentStateCacheInstance = {
    .state = AGENT_STATE_INIT,
    .agentId = NULL,
    .pubIp = NULL,
    .pubPort = 0,
    .services = NULL
};

static int agentPidFd = -1;
/* Mirror interface pcap descriptor */
static pcap_t *mirrorPcapDev = NULL;

static inline void
freeAgentParameters (void) {
    free (agentParameters.mirrorInterface);
}

void
freeAgentStateCache (void) {
    free (agentStateCacheInstance.agentId);
    free (agentStateCacheInstance.pubIp);
    free (agentStateCacheInstance.services);
}

void
dumpAgentStateCache (void) {
    int fd;
    json_t *root;
    char *out;

    root = json_object ();
    if (root == NULL) {
        LOGE ("Create json object error.\n");
        return;
    }

    fd = open (AGENT_STATE_CACHE_FILE, O_WRONLY | O_TRUNC | O_CREAT, 0755);
    if (fd < 0) {
        LOGE ("Open %s error: %s\n", AGENT_STATE_CACHE_FILE, strerror (errno));
        return;
    }

    json_object_set_new (root, "state", json_integer (agentStateCacheInstance.state));
    json_object_set_new (root, "agentId", json_string (agentStateCacheInstance.agentId));
    json_object_set_new (root, "pubIp", json_string (agentStateCacheInstance.pubIp));
    json_object_set_new (root, "pubPort", json_integer (agentStateCacheInstance.pubPort));
    json_object_set_new (root, "services", json_string (agentStateCacheInstance.services));
    out = json_dumps (root, JSON_INDENT (4));
    json_object_clear (root);

    safeWrite (fd, dumpOut, strlen (dumpOut));
    close (fd);
}

int
initAgentStateCache (void) {
    int fd;
    json_error_t error;
    json_t *root, *tmp;

    if (!fileExist (AGENT_RUN_DIR) && (mkdir (AGENT_RUN_DIR, 0755) < 0)) {
        LOGE ("Create agent run directory error: %s.\n", strerror (errno));
        return -1;
    }

    if (!fileExist (AGENT_STATE_CACHE_FILE) || fileIsEmpty (AGENT_STATE_CACHE_FILE))
        dumpAgentStateCache ();

    fd = open (AGENT_STATE_CACHE_FILE, O_RDONLY);
    if (fd < 0) {
        LOGE ("Open %s error: %s\n", AGENT_STATE_CACHE_FILE, strerror (errno));
        return -1;
    }

    root = json_load_file (AGENT_STATE_CACHE_FILE, JSON_DISABLE_EOF_CHECK, &error);
    if ((root == NULL) ||
        (json_object_get (root, "state") == NULL) ||
        (json_object_get (root, "agentId") == NULL) ||
        (json_object_get (root, "pubIp") == NULL) ||
        (json_object_get (root, "pubPort") == NULL)) {
        agentStateCacheInstance.state = AGENT_STATE_INIT;
        agentStateCacheInstance.agentId = NULL;
        agentStateCacheInstance.pubIp = NULL;
        agentStateCacheInstance.pubPort = 0;
        agentStateCacheInstance.services = NULL;
        close (fd);
        return 0;
    }

    tmp = json_object_get (root, "state");
    agentStateCacheInstance.state = json_integer_value (tmp);
    tmp = json_object_get (root, "agentId");
    agentStateCacheInstance.agentId = strdup (json_string_value (tmp));
    tmp = json_object_get (root, "pubIp");
    agentStateCacheInstance.pubIp = strdup (json_string_value (tmp));
    tmp = json_object_get (root, "pubPort");
    agentStateCacheInstance.pubPort = json_integer_value (tmp);
    tmp = json_object_get (root, "services");
    agentStateCacheInstance.services = strdup (json_string_value (tmp));

    if ((agentStateCacheInstance.state == AGENT_STATE_INIT) || (agentStateCacheInstance.agentId == NULL) ||
        (agentStateCacheInstance.pubIp == NULL) || (agentStateCacheInstance.pubPort == 0)) {
        /* Free */
        freeAgentStateCache ();
        /* Reset */
        agentStateCacheInstance.state = AGENT_STATE_INIT;
        agentStateCacheInstance.agentId = NULL;
        agentStateCacheInstance.pubIp = NULL;
        agentStateCacheInstance.pubPort = 0;
        agentStateCacheInstance.services = NULL;
    }

    close (fd);
    return 0;
}

/*
 * @brief Build agent control response message
 *
 * @param code response code, 0 for success and 1 for error
 * @param status response status, 1 for stopped, 2 for running and 3 for error.
 *
 * @return response message in json if success else NULL
 */
static char *
buildAgentControlResponse (int code, int status) {
    char *json;
    json_ *root, *tmp;

    root = json_object ();
    if (root == NULL) {
        LOGE ("Alloc json object root error.\n");
        return  NULL;
    }

    /* Set response code */
    json_object_set_new (resp, "code", json_integer (code));

    /* Set response body:status */
    if (status != AGENT_STATE_INIT) {
        tmp = json_object ();
        if (tmp == NULL) {
            LOGE ("Alloc json object tmp error.\n");
            json_object_clear (root);
            return NULL;
        }
        json_object_set_new (tmp, "status", json_integer (status));
        json_object_set_new (root, "body", tmp);
    }
    json = json_dumps (resp, JSON_INDENT (4));
    json_object_clear (root);
    return json;
}

static int
addAgent (const char *profile) {
    json_error_t error;
    json_t *root, *tmp;

    if (agentStateCacheInstance.state != AGENT_STATE_INIT) {
        LOGE ("Add-agent error: agent already added.\n");
        return -1;
    }

    /* Free agent state cache */
    freeAgentStateCache ();

    root = json_loads (profile, JSON_DISABLE_EOF_CHECK, &error);
    if ((root == NULL) ||
        (json_object_get (root, "ip") == NULL) ||
        (json_object_get (root, "port") == NULL) ||
        (json_object_get (root, "agent-id") == NULL)) {
        LOGE ("Json parse error.\n");
        return -1;
    }

    /* Get pubIp */
    tmp = json_object_get (root, "ip");
    agentStateCacheInstance.pubIp = strdup (json_string_value (tmp));
    if (agentStateCacheInstance.pubIp == NULL) {
        LOGE ("Get pubIp error.\n");
        freeAgentStateCache ();
        json_object_clear (root);
        return -1;
    }

    /* Get pubPort */
    tmp = json_object_get (root, "port");
    agentStateCacheInstance.pubPort = json_integer_value (tmp);

    /* Get agent id */
    tmp = json_object_get (root, "agent-id");
    agentStateCacheInstance.agentId = strdup (json_string_value (tmp));
    if (agentStateCacheInstance.agentId == NULL) {
        LOGE ("Get agentId error.\n");
        freeAgentStateCache ();
        json_object_clear (root);
        return -1;
    }

    /* Set agent state */
    agentStateCacheInstance.state = AGENT_STATE_STOPPED;
    /* Free root */
    json_object_clear (root);

    return 0;
}

static int
removeAgent (const char *profile) {
    if (agentStateCacheInstance.state == AGENT_STATE_RUNNING) {
        LOGE ("Agent is running, please stop it before removing.\n");
        return -1;
    }

    
}

static int
startAgent (const char *profile) {

}

static int
stopAgent (const char *profile) {

}

static int
heartbeat (const char *profile) {

}

static int
pushProfile (const char *profile) {

}

/* Agent control message handler, this handler will always return 0. */
static int
agentControlMessageHandler (zloop_t *loop, zmq_pollitem_t *item, void *arg) {
    int ret;
    char *msg;
    char *cmd, body, out;
    json_error_t error;
    json_t *root, *tmp;

    msg = zstr_recv_nowait (item->socket);
    if (msg == NULL)
        return 0;

    root = json_loads (msg, JSON_DISABLE_EOF_CHECK, &error);
    if ((root == NULL) ||
        (json_object_get (root, "command") == NULL) ||
        (json_object_get (root, "body") == NULL)) {
        LOGE ("Json parse error.\n");
        out = buildAgentControlResponse (AGENT_CONTROL_RESPONSE_FAILURE, AGENT_STATE_ERROR);
    } else {
        tmp = json_object_get (root, "command");
        command = json_string_value (tmp);
        tmp = json_object_get (root, "body");
        body = json_string_value (tmp);

        if (strEqual ("add-agent", command)) {
            ret = addAgent (body);
            if (ret < 0)
                out = buildAgentControlResponse (AGENT_CONTROL_RESPONSE_FAILURE, AGENT_STATE_ERROR);
            else
                out = buildAgentControlResponse (AGENT_CONTROL_RESPONSE_SUCCESS, agentStateCacheInstance.state);
        } else if (strEqual("remove-agent", command)) {
            ret = removeAgent (body);
            if (ret < 0)
                out = buildAgentControlResponse (AGENT_CONTROL_RESPONSE_FAILURE, AGENT_STATE_ERROR);
            else
                out = buildAgentControlResponse (AGENT_CONTROL_RESPONSE_SUCCESS, agentStateCacheInstance.state);

        } else if (strEqual ("start-agent", command)) {
            ret = startAgent (body);
            if (ret < 0)
                out = buildAgentControlResponse (AGENT_CONTROL_RESPONSE_FAILURE, AGENT_STATE_ERROR);
            else
                out = buildAgentControlResponse (AGENT_CONTROL_RESPONSE_SUCCESS, agentStateCacheInstance.state);

        } else if (strEqual ("stop-agent", command)) {
            ret = stopAgent (body);
            if (ret < 0)
                out = buildAgentControlResponse (AGENT_CONTROL_RESPONSE_FAILURE, AGENT_STATE_ERROR);
            else
                out = buildAgentControlResponse (AGENT_CONTROL_RESPONSE_SUCCESS, agentStateCacheInstance.state);

        } else if (strEqual ("heartbeat", command)) {
            ret = heartbeat (body);
            if (ret < 0)
                out = buildAgentControlResponse (AGENT_CONTROL_RESPONSE_FAILURE, AGENT_STATE_ERROR);
            else
                out = buildAgentControlResponse (AGENT_CONTROL_RESPONSE_SUCCESS, agentStateCacheInstance.state);

        } else if (strEqual ("push-profile", command)) {
            ret = pushProfile (body);
            if (ret < 0)
                out = buildAgentControlResponse (AGENT_CONTROL_RESPONSE_FAILURE, AGENT_STATE_ERROR);
            else
                out = buildAgentControlResponse (AGENT_CONTROL_RESPONSE_SUCCESS, agentStateCacheInstance.state);
        } else {
            LOGE ("Unknown agent control command.\n");
            out = buildAgentControlResponse (AGENT_CONTROL_RESPONSE_FAILURE, AGENT_STATE_ERROR);
        }
    }

    zstr_send (item->socket, out);
    free (out);
    free (msg);
    return 0;
}

/*
 * Sub-thread status message handler, when receiving SUB_THREAD_EXIT
 * then return -1 to exit.
 */
static int
subThreadStatusMessageHandler (zloop_t *loop, zmq_pollitem_t *item, void *arg) {
    char *status;

    status = subThreadStatusRecvNonBlock ();
    if (status == NULL)
        return 0;

    if (strEqual (status, SUB_THREAD_EXIT)) {
        LOGE ("Sub-threads exit abnormally\n");
        free (status);
        return -1;
    }

    free (status);
    return 0;
}

static int
lockPidFile (void) {
    pid_t pid;
    ssize_t n;
    char buf [16] = {0};

    pid = getpid ();

    agentPidFd = open (AGENT_PID_FILE, O_CREAT | O_RDWR, 0666);
    if (agentPidFd < 0) {
        fprintf(stderr, "Open pid file %s error: %s.\n", AGENT_PID_FILE, strerror (errno));
        return -1;
    }

    if (flock (agentPidFd, LOCK_EX | LOCK_NB) == 0) {
        snprintf (buf, sizeof (buf) - 1, "%d", pid);
        n = write (agentPidFd, buf, strlen (buf));
        if (n != strlen (buf)) {
            fprintf(stderr, "Write pid to pid file error: %s.\n", strerror (errno));
            close (agentPidFd);
            remove (AGENT_PID_FILE);
            return -1;
        }
        sync ();
    } else {
        fprintf (stderr, "Agent is running.\n");
        close (agentPidFd);
        return -1;
    }

    return 0;
}

static void
unlockPidFile (void) {
    if (agentPidFd >= 0) {
        flock (agentPidFd, LOCK_UN);
        close (agentPidFd);
        agentPidFd = -1;
    }
    remove (AGENT_PID_FILE);
}

static int
agentRun (void) {
    int ret;
    void *agentControlRecvSock;
    void *subThreadStatusRecvSock;
    zloop_t *loop;
    zmq_pollitem_t pollItems [2];

    if (lockPidFile () < 0)
        return -1;

    /* Init log context */
    ret = initLog (agentParameters.logLevel);
    if (ret < 0) {
        logToConsole ("Init log context error.\n");
        ret = -1;
        goto unlockPidFile;
    }

    /* Init message channel */
    ret = initMessageChannel ();
    if (ret < 0) {
        LOGE ("Init message channel error.\n");
        ret = -1;
        goto destroyLog;
    }

    /* Init agent state cache */
    ret = initAgentStateCache ();
    if (ret < 0) {
        LOGE ("Init agent state cache error.\n");
        ret = -1;
        goto destroyMessageChannel;
    }

    /* Init agent control socket */
    agentControlRecvSock = newZSock (ZMQ_REP);
    if (agentControlRecvSock == NULL) {
        LOGE ("Create zsocket error.\n");
        ret = -1;
        goto freeAgentStateCache;
    }

    /* Get sub-thread status receive socket */
    subThreadStatusRecvSock = getStatusRecvSock ();
    if (subThreadStatusRecvSock == NULL) {
        LOGE ("Get subThreadStatusRecvSock error.\n");
        ret = -1;
        goto freeAgentStateCache;
    }

    /* Create zloop reactor */
    loop = zloop_new ();
    if (loop == Null) {
        LOGE ("Create zloop error.\n");
        ret = -1;
        goto freeAgentStateCache;
    }

    /* Init poll item 0*/
    pollItems [0].socket = agentControlRecvSock;
    pollItems [0].fd = 0;
    pollItems [0].events = ZMQ_POLLIN;

    /* Init poll item 1*/
    pollItems [1].socket = subThreadStatusRecvSock;
    pollItems [1].fd = 0;
    pollItems [1].events = ZMQ_POLLIN;

    /* Register poll item 0 */
    ret = zloop_poller(loop, &pollItems [0], agentControlMessageHandler, NULL);
    if (ret < 0) {
        LOGE ("Register poll items [0] error.\n");
        ret = -1;
        goto destroyZloop;
    }

    /* Register poll item 1 */
    ret = zloop_poller(loop, &pollItems [0], subThreadStatusMessageHandler, NULL);
    if (ret < 0) {
        LOGE ("Register poll items [1] error.\n");
        ret = -1;
        goto destroyZloop;
    }

    /* Start zloop */
    ret = zloop_start (loop);
    if (ret < 0)
        LOGE ("Stopped with error");
    else
        LOGD ("Stopped by interrupt.\n");

destroyZloop:
    zloop_destroy (&loop);
freeAgentStateCache:
    freeAgentStateCache ();
destroyMessageChannel:
    destroyMessageChannel ();
destroyLog:
    destroyLog ();
unlockPidFile:
    unlockPidFile ();
    return ret;
}

/* Parse configuration of agent */
static int
parseConf (void) {
    int ret, error;
    const char *tmp;
    struct collection_item *iniConfig = NULL;
    struct collection_item *errorSet = NULL;
    struct collection_item *item;

    ret = config_from_file ("Agent", AGENT_CONFIG_FILE,
                            &iniConfig, INI_STOP_ON_ANY, &errorSet);
    if (ret) {
        logToConsole ("Parse config file: %s error.\n", AGENT_CONFIG_FILE);
        return -1;
    }

    /* Get daemon mode */
    ret = get_config_item ("MAIN", "daemonMode", iniConfig, &item);
    if (ret) {
        logToConsole ("Get_config_item \"daemonMode\" error\n");
        ret = -1;
        goto exit;
    }
    agentParameters.daemonMode = get_int_config_value (item, 1, -1, &error);
    if (error) {
        logToConsole ("Parse \"daemonMode\" error.\n");
        ret = -1;
        goto exit;
    }

    /* Get mirror interface */
    ret = get_config_item ("MAIN", "mirrorInterface", iniConfig, &item);
    if (ret) {
        logToConsole ("Get_config_item \"mirrorInterface\" error\n");
        ret = -1;
        goto exit;
    }
    tmp = get_const_string_config_value (item, &error);
    if (error) {
        logToConsole ("Parse \"mirrorInterface\" error.\n");
        ret = -1;
        goto exit;
    }
    agentParameters.mirrorInterface = strdup (tmp);
    if (agentParameters.mirrorInterface == NULL) {
        logToConsole ("Get \"mirrorInterface\" error\n");
        ret = -1;
        goto exit;
    }

    /* Get default log level */
    ret = get_config_item ("LOG", "logLevel", iniConfig, &item);
    if (ret) {
        logToConsole ("Get_config_item \"logLevel\" error\n");
        ret = -1;
        goto exit;
    }
    agentParameters.logLevel = get_int_config_value (item, 1, -1, &error);
    if (error) {
        logToConsole ("Parse \"logLevel\" error.\n");
        ret = -1;
        goto exit;
    }

exit:
    if (iniConfig)
        free_ini_config (iniConfig);
    if (errorSet)
        free_ini_config_errors (errorSet);
    return ret;
}

/* Agent cmd options */
static struct option agentOptions [] = {
    {"daemonMode", no_argument, NULL, 'D'},
    {"mirrorInterface", required_argument, NULL, 'm'},
    {"logLevel", required_argument, NULL, 'l'},
    {"version", no_argument, NULL, 'v'},
    {"help", no_argument, NULL, 'h'},
    {NULL, no_argument, NULL, 0},
};

static void
showHelpInfo (const char *cmd) {
    const char *cmdName;

    cmdName = strrchr (cmd, '/') ? (strrchr (cmd, '/') + 1) : cmd;
    logToConsole ("Usage: %s -m <eth*> [options]\n"
                  "       %s [-vh]\n"
                  "Basic options: \n"
                  "  -D|--daemonMode, run as daemon\n"
                  "  -m|--mirrorInterface <eth*> interface to collect packets\n"
                  "  -l|--logLevel <level> log level\n"
                  "       Optional level: 0-ERR 1-WARNING 2-INFO 3-DEBUG\n"
                  "  -v|--version, version of %s\n"
                  "  -h|--help, help information\n",
                  cmdName, cmdName, cmdName);
}

/* Cmd line parser */
static int
parseCmdline (int argc, char *argv []) {
    char option;
    BOOL showVersion = FALSE;
    BOOL showHelp = FALSE;

    while ((option = getopt_long (argc, argv, "Dm:l:vh?", agentOptions, NULL)) != -1) {
        switch (option) {
            case 'D':
                agentParameters.daemonMode = 1;
                break;

            case 'm':
                agentParameters.mirrorInterface = strdup (optarg);
                if (agentParameters.mirrorInterface == NULL) {
                    logToConsole ("Get mirroring interface error!\n");
                    return -1;
                }
                break;

            case 'l':
                agentParameters.logLevel = atoi (optarg);
                break;

            case 'v':
                showVersion = TRUE;
                break;

            case 'h':
                showHelp = TRUE;
                break;

            case '?':
                logToConsole ("Unknown options.\n");
                showHelpInfo (argv [0]);
                return -1;
        }
    }

    if (showVersion || showHelp) {
        if (showVersion)
            logToConsole ("Current version: %d.%d\n", AGENT_VERSION_MAJOR, AGENT_VERSION_MINOR);
        if (showHelp)
            showHelpInfo (argv [0]);
        exit (0);
    }

    return 0;
}

static int
agentDaemon (void) {
    pid_t pid, next_pid;
    int stdinfd;
    int stdoutfd;

    if (chdir("/") < 0) {
        fprintf (stderr, "Chdir error: %s.\n", strerror (errno));
        return -1;
    }

    pid = fork ();
    switch (pid) {
        case 0:
            if ((stdinfd = open ("/dev/null", O_RDONLY)) < 0)
                return -1;

            if ((stdoutfd = open ("/dev/null", O_WRONLY)) < 0) {
                close (stdinfd);
                return -1;
            }

            if (dup2 (stdinfd, STDIN_FILENO) != STDIN_FILENO) {
                close (stdoutfd);
                close (stdinfd);
                return -1;
            }

            if (dup2 (stdoutfd, STDOUT_FILENO) != STDOUT_FILENO) {
                close (stdoutfd);
                close (stdinfd);
                return -1;
            }

            if (dup2 (stdoutfd, STDERR_FILENO) != STDERR_FILENO) {
                close (stdoutfd);
                close (stdinfd);
                return -1;
            }

            if (stdinfd > STDERR_FILENO)
                close (stdoutfd);

            if (stdoutfd > STDERR_FILENO)
                close (stdinfd);

            /* Set session id */
            if (setsid () < 0) {
                close (stdoutfd);
                close (stdinfd);
                return -1;
            }

            next_pid = fork ();
            switch (next_pid) {
                case 0:
                    return agentRun ();

                case -1:
                    return -1;

                default:
                    return 0;
            }

        case -1:
            return -1;

        default:
            return 0;
    }
}

int
main (int argc, char *argv []) {
    int ret;

    if (getuid () != 0) {
        fprintf (stderr, "Permission denied, please run as root.\n");
        return -1;
    }

    /* Set locale */
    setlocale (LC_COLLATE,"");
    /* Parse configuration file */
    ret = parseConf ();
    if (ret < 0) {
        fprintf (stderr, "Parse configuration file error.\n");
        ret = -1;
        goto exit;
    }

    /* Parse command */
    ret = parseCmdline (argc, argv);
    if (ret < 0) {
        fprintf (stderr, "Parse command line error.\n");
        ret = -1;
        goto exit;
    }

    if (agentParameters.daemonMode)
        ret = agentDaemon ();
    else
        ret = agentRun ();
exit:
    freeAgentParameters ();
    return ret;
}
