#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <signal.h>
#include <czmq.h>
#include <errno.h>
#include <getopt.h>
#include <locale.h>
#include "util.h"
#include "list.h"
#include "log.h"

#define LOGD_PID_FILE "/var/run/logd.pid"
#define LOG_TO_FILE_MASK (1 << 0)
#define LOG_TO_NET_MASK (1 << 1)

static int logdPidFileFd = -1;
/* Log devices list */
static listHead logDevices;

typedef struct _logDev logDev;
typedef logDev *logDevPtr;
/*
 * Log backend dev, every dev has three interfaces,
 * you can add new log dev into log system with log_dev_add
 */
struct _logDev {
    listHead node;
    void *data;

    /* Operations for log dev */
    int (*init) (logDevPtr dev);
    void (*destroy) (logDevPtr dev);
    void (*write) (const char *msg, logDevPtr dev, u_int flag);
};

/* Bit test */
static inline BOOL
testBit (u_int flag, u_int bitMask) {
    if (flag & bitMask)
        return TRUE;
    else
        return FALSE;
}

/*===========================log file dev=================================*/

/* Defautl log file dir */
#define DEFAULT_LOG_FILE_DIR "/var/log/logd/"
/* Defautl log file name */
#define DEFAULT_LOG_FILE_NAME "logd.log"
/* Max log file size is 128 MB */
#define LOG_FILE_MAX_SIZE (128 << 20)
#define LOG_FILE_ROTATE_NUMBER 8
#define LOG_FILE_SIZE_CHECK_COUNT 200
#define LOG_FILE_PATH_MAX_LEN 256

static char *logFileDir = NULL;
static char *logFileName = NULL;

typedef struct _logFile logFile;
typedef logFile *logFilePtr;

struct _logFile {
    int fd;                             /**< Log file fd */
    char *filePath;                     /**< Log file path */
    u_int writeCount;                   /**< Log file write count for size checking */
};

static BOOL
logFileOversize (const char *filePath) {
    int ret;
    struct stat fileStat;

    ret = stat (filePath, &fileStat);
    if (ret < 0)
        return TRUE;

    if (fileStat.st_size >= LOG_FILE_MAX_SIZE)
        return TRUE;
    else
        return FALSE;
}

static int
logFileRotate (const char *logFileName) {
    int ret;
    int index;
    char fileNameBuf1 [LOG_FILE_PATH_MAX_LEN] = {0};
    char fileNameBuf2 [LOG_FILE_PATH_MAX_LEN] = {0};

    for (index = (LOG_FILE_ROTATE_NUMBER - 1); index > 0; index--) {
        if (index == (LOG_FILE_ROTATE_NUMBER - 1)) {
            snprintf (fileNameBuf2, sizeof (fileNameBuf2) - 1, "%s_%d", logFileName, index);
            if (fileExist (fileNameBuf2)) {
                ret = remove (fileNameBuf2);
                if (ret < 0) {
                    fprintf (stderr, "Log file rotate error.\n");
                    return -1;
                }
            }
        } else {
            snprintf (fileNameBuf1, sizeof (fileNameBuf1) - 1, "%s_%d", logFileName, index);
            snprintf (fileNameBuf2, sizeof (fileNameBuf2) - 1, "%s_%d", logFileName, index + 1);
            if (fileExist (fileNameBuf1)) {
                ret = rename (fileNameBuf1, fileNameBuf2);
                if (ret < 0) {
                    fprintf (stderr, "Log file rotate error.\n");
                    return -1;
                }
            }
        }
    }

    if (LOG_FILE_ROTATE_NUMBER == 1) {
        ret = remove (logFileName);
        if (ret < 0) {
            fprintf (stderr, "Log file rotate error.\n");
            return -1;
        }
    } else {
        snprintf (fileNameBuf2, sizeof (fileNameBuf2) - 1, "%s_%d", logFileName, 1);
        ret = rename (logFileName, fileNameBuf2);
        if (ret < 0) {
            fprintf (stderr, "Log file rotate error.\n");
            return -1;
        }
    }

    return 0;
}

static int
logFileUpdate (logDevPtr dev) {
    int ret;
    logFilePtr logfile = (logFilePtr) dev->data;

    close (logfile->fd);
    ret = logFileRotate (logfile->filePath);
    if (ret < 0)
        return -1;

    logfile->fd = open (logfile->filePath, O_WRONLY | O_APPEND | O_CREAT, 0755);
    if (logfile->fd < 0)
        return -1;

    logfile->writeCount = 0;
    return 0;
}

static int
initLogFile (logDevPtr dev) {
    char logFilePath [LOG_FILE_PATH_MAX_LEN] = {0};
    logFilePtr logfile;

    if (!fileExist (logFileDir) &&
        (mkdir (logFileDir, 0755) < 0))
        return -1;

    logfile = (logFilePtr) malloc (sizeof (logFile));
    if (logfile == NULL)
        return -1;

    snprintf (logFilePath, sizeof (logFilePath) - 1, "%s/%s", logFileDir, logFileName);
    logfile->filePath = strdup (logFilePath);
    if (logfile->filePath == NULL) {
        free (logfile);
        return -1;
    }

    logfile->fd = open (logfile->filePath, O_WRONLY | O_APPEND | O_CREAT, 0755);
    if (logfile->fd < 0) {
        free (logfile->filePath);
        free (logfile);
        return -1;
    }

    logfile->writeCount = 0;
    dev->data = logfile;

    return 0;
}

static void
destroyLogFile (logDevPtr dev) {
    logFilePtr logfile = (logFilePtr) dev->data;

    close (logfile->fd);
    free (logfile->filePath);
    free (logfile);
}

static void
writeLogFile (const char *msg, logDevPtr dev, u_int flag) {
    int ret;
    logFilePtr logfile;

    if (!testBit (flag, LOG_TO_FILE_MASK))
        return;

    logfile = (logFilePtr) dev->data;
    ret = safeWrite (logfile->fd, msg, strlen (msg));
    if (ret < 0) {
        zctx_interrupted = 1;
        fprintf (stderr, "log file write error.\n");
        return;
    }
    logfile->writeCount++;
    if ((logfile->writeCount >= LOG_FILE_SIZE_CHECK_COUNT) &&
        logFileOversize (logfile->filePath)) {
        ret = logFileUpdate (dev);
        if (ret < 0)
            zctx_interrupted = 1;
        fprintf (stderr, "log file update error.\n");
    }
}

/*===========================log net dev=================================*/

typedef struct _logNet logNet;
typedef logNet *logNetPtr;

struct _logNet {
    zctx_t *context;
    void *sock;
};

static int
initLogNet (logDevPtr dev) {
    int ret;
    logNetPtr lognet;

    if ((dev->data = malloc (sizeof (logNet))) == NULL)
        return -1;

    lognet = (logNetPtr) dev->data;
    lognet->context = zctx_new ();
    if (lognet->context == NULL) {
        free (dev->data);
        return -1;
    }

    lognet->sock = zsocket_new (lognet->context, ZMQ_PUB);
    if (lognet->sock == NULL) {
        zctx_destroy (&lognet->context);
        return -1;
    }

    ret = zsocket_bind (lognet->sock, "tcp://*:%d", LOG_SERVICE_PUBLISH_PORT);
    if (ret != LOG_SERVICE_PUBLISH_PORT) {
        zctx_destroy (&lognet->context);
        return -1;
    }

    return 0;
}

static void
writeLogNet (const char *msg, logDevPtr dev, u_int flag) {
    int ret;
    logNetPtr lognet;
    zframe_t *frame = NULL;

    if (!testBit (flag, LOG_TO_NET_MASK))
        return;

    lognet = (logNetPtr) dev->data;

    frame = zframe_new ((void *) msg, strlen (msg));
    if (frame == NULL)
        return;

    ret = zframe_send (&frame, lognet->sock, 0);
    if (ret < 0) {
        zframe_destroy (&frame);
        return;
    }
}

static void
destroyLogNet (logDevPtr dev) {
    logNetPtr lognet = (logNetPtr) dev->data;

    zctx_destroy (&lognet->context);
    free (lognet);
}

/*============================log dev================================*/

static int
logDevAdd (logDevPtr dev) {
    int ret;

    ret = dev->init (dev);
    if (ret < 0)
        return -1;

    listAdd (&dev->node, &logDevices);

    return 0;
}

static void
logDevWrite (listHeadPtr logDevices, const char *msg) {
    int ret;
    u_int flag;
    logDevPtr dev;
    const char *message;

    /* Get flag */
    ret = sscanf (msg, "%u", &flag);
    if (ret != 1)
        return;
    switch (flag) {
        case LOG_TO_ALL_TAG:
            flag = LOG_TO_FILE_MASK | LOG_TO_NET_MASK;
            break;

        case LOG_TO_NET_TAG:
            flag = LOG_TO_NET_MASK;
            break;

        default:
            return;
    }

    /* Get real log message */
    message = strstr (msg, "[pid:");
    if (message == NULL)
        return;
    listForEachEntry (dev, logDevices, node) {
        dev->write (message, dev, flag);
    }
}

static void
logDevDestroy (void) {
    logDevPtr dev, ndev;

    listForEachEntrySafe (dev, ndev, &logDevices, node) {
        dev->destroy (dev);
        listDel (&dev->node);
    }
}

static int
lockPidFile (void) {
    pid_t pid;
    ssize_t n;
    char buf [16] = {0};

    pid = getpid ();

    logdPidFileFd = open (LOGD_PID_FILE, O_CREAT | O_RDWR, 0666);
    if (logdPidFileFd < 0) {
        fprintf(stderr, "Open pid file %s error: %s.\n", LOGD_PID_FILE, strerror (errno));
        return -1;
    }

    if (flock (logdPidFileFd, LOCK_EX | LOCK_NB) == 0) {
        snprintf (buf, sizeof (buf) - 1, "%d", pid);
        n = write (logdPidFileFd, buf, strlen (buf));
        if (n != strlen (buf)) {
            fprintf(stderr, "Write pid to pid file error: %s.\n", strerror (errno));
            close (logdPidFileFd);
            remove (LOGD_PID_FILE);
            return -1;
        }
        sync ();
    } else {
        fprintf (stderr, "Logd service is running.\n");
        close (logdPidFileFd);
        return -1;
    }

    return 0;
}

static void
unlockPidFile (void) {
    if (logdPidFileFd >= 0) {
        flock (logdPidFileFd, LOCK_UN);
        close (logdPidFileFd);
        logdPidFileFd = -1;
    }
    remove (LOGD_PID_FILE);
}

static int
logdRun (void) {
    int ret;
    char *msg;
    zctx_t *context;
    void *logRcvSock;

    logDev logFileDev = {
        .init = initLogFile,
        .destroy = destroyLogFile,
        .write = writeLogFile,
    };

    logDev logNetDev = {
        .init = initLogNet,
        .destroy = destroyLogNet,
        .write = writeLogNet,
    };

    /* Lock pid file */
    if (lockPidFile () < 0)
        return -1;

    context = zctx_new ();
    if (context == NULL)
        return -1;

    logRcvSock = zsocket_new (context, ZMQ_PULL);
    if (logRcvSock == NULL) {
        zctx_destroy (&context);
        return -1;
    }

    ret = zsocket_bind (logRcvSock, "tcp://*:%d", LOG_SERVICE_SINK_PORT);
    if (ret != LOG_SERVICE_SINK_PORT) {
        zctx_destroy (&context);
        return -1;
    }

    /* Init log dev */
    initListHead (&logDevices);
    ret = logDevAdd (&logFileDev);
    if (ret < 0) {
        zctx_destroy (&context);
        return -1;
    }
    ret = logDevAdd (&logNetDev);
    if (ret < 0) {
        logDevDestroy ();
        zctx_destroy (&context);
        return -1;
    }

    while (!zctx_interrupted) {
        msg = zstr_recv (logRcvSock);
        if (msg) {
            logDevWrite (&logDevices, msg);
            free (msg);
        }
    }

    unlockPidFile ();
    logDevDestroy ();
    zctx_destroy (&context);
    return 0;
}

static int
logdDaemon (void) {
    pid_t pid, next_pid;
    int stdinfd;
    int stdoutfd;

    if (chdir("/") < 0)
        return -1;

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
                    return logdRun ();

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

/* Logd options */
static struct option logdOptions [] = {
    {"dir", required_argument, NULL, 'd'},
    {"name", required_argument, NULL, 'f'},
    {"daemon", no_argument, NULL, 'D'},
    {"help", no_argument, NULL, 'h'},
    {NULL, no_argument, NULL, 0},
};

static void
showHelpInfo (const char *cmd) {
    const char *cmdName;

    cmdName = strrchr (cmd, '/') ? (strrchr (cmd, '/') + 1) : cmd;
    fprintf (stdout,
             "Usage: %s -f <pid-file> [-d]\n"
             "Basic options: \n"
             "  -d|--dir <directory>, log file directory\n"
             "  -f|--name <fileName>, log file name\n"
             "  -D|--daemon, run as daemon\n"
             "  -h|--help, show help message\n",
             cmdName);
}

int
main (int argc, char *argv []) {
    int ret;
    char option;
    /* Whether run as daemon service */
    BOOL runDaemon = FALSE;

    if (getuid () != 0) {
        fprintf (stderr, "Permission denied, please run as root\n");
        return -1;
    }

    /* Set locale */
    setlocale(LC_COLLATE,"");
    while ((option = getopt_long (argc, argv, "d:f:Dh?", logdOptions, NULL)) != -1) {
        switch (option) {
            case 'd':
                logFileDir = strdup (optarg);
                if (logFileDir == NULL) {
                    fprintf (stderr, "Strdup log file directory error: %s.\n", strerror (errno));
                    ret = -1;
                    goto exit;
                }
                break;

            case 'f':
                logFileName = strdup (optarg);
                if (logFileName == NULL) {
                    fprintf (stderr, "Strdup log file name error: %s.\n", strerror (errno));
                    ret = -1;
                    goto exit;
                }
                break;

            case 'D':
                runDaemon = TRUE;
                break;

            case 'h':
                showHelpInfo (argv [0]);
                ret = 0;
                goto exit;

            case '?':
                fprintf (stderr, "Unknown options.\n");
                showHelpInfo (argv [0]);
                ret = 0;
                goto exit;
        }
    }

    if (logFileDir == NULL) {
        logFileDir = strdup (DEFAULT_LOG_FILE_DIR);
        if (logFileDir == NULL) {
            fprintf (stderr, "Strdup log file directory error: %s.\n", strerror (errno));
            ret = -1;
            goto exit;
        }
    }

    if (logFileName == NULL) {
        logFileName = strdup (DEFAULT_LOG_FILE_NAME);
        if (logFileName == NULL) {
            fprintf (stderr, "Strdup log file name error: %s.\n", strerror (errno));
            ret = -1;
            goto exit;
        }
    }

    if (runDaemon)
        ret = logdDaemon ();
    else
        ret = logdRun ();

exit:
    free (logFileDir);
    free (logFileName);
    return ret;
}
