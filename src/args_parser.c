#include <stdio.h>
#include <getopt.h>
#include "properties.h"
#include "log.h"
#include "version.h"
#include "args_parser.h"

static struct option argsOptions [] = {
    {"daemonMode", no_argument, NULL, 'D'},
    {"mirrorInterface", required_argument, NULL, 'm'},
    {"breakdownSinkIp", required_argument, NULL, 'i'},
    {"breakdownSinkPort", required_argument, NULL, 'p'},
    {"logDir", required_argument, NULL, 'd'},
    {"logFileName", required_argument, NULL, 'f'},
    {"logLevel", required_argument, NULL, 'l'},
    {"version", no_argument, NULL, 'v'},
    {"help", no_argument, NULL, 'h'},
    {NULL, no_argument, NULL, 0},
};

static void
showHelpInfo (const char *cmd) {
    const char *cmdName;

    cmdName = strrchr (cmd, '/') ? (strrchr (cmd, '/') + 1) : cmd;
    fprintf (stdout,
             "Usage: %s -m <eth*> [options]\n"
             "       %s [-vh]\n"
             "Basic options: \n"
             "  -D|--daemonMode, run as daemon\n"
             "  -m|--mirrorInterface <eth*> interface to collect packets\n"
             "  -i|--breakdownSinkIp <ip> breakdown sink ip"
             "  -p|--breakdownSinkPort <port> breakdown sink port"
             "  -d|--logDir <path>, log file directory\n"
             "  -f|--logFileName <name>, log file name\n"
             "  -l|--logLevel <level> log level\n"
             "       Optional level: 0-ERR 1-WARNING 2-INFO 3-DEBUG\n"
             "  -v|--version, version of %s\n"
             "  -h|--help, help information\n",
             cmdName, cmdName, cmdName);
}

/* Command line arguments parser */
int
parseArgs (int argc, char *argv []) {
    char option;
    boolean showVersion = false;
    boolean showHelp = false;

    while ((option = getopt_long (argc, argv, "Dm:i:p:l:vh?", argsOptions, NULL)) != -1) {
        switch (option) {
            case 'D':
                updatePropertiesDaemonMode (true);
                break;

            case 'm':
                updatePropertiesMirrorInterface (optarg);
                if (getPropertiesMirrorInterface () == NULL) {
                    fprintf (stderr, "Parse mirroring interface error!\n");
                    return -1;
                }
                break;

            case 'i':
                updatePropertiesBreakdownSinkIp (optarg);
                if (getPropertiesBreakdownSinkIp () == NULL) {
                    fprintf (stderr, "Parse breakdown sink ip error!\n");
                    return -1;
                }
                break;

            case 'p':
                updatePropertiesBreakdownSinkPort (atoi (optarg));
                break;

            case 'd':
                updatePropertiesLogDir (optarg);
                if (getPropertiesLogDir () == NULL) {
                    fprintf (stderr, "Parse log dir error!\n");
                    return -1;
                }
                break;

            case 'f':
                updatePropertiesLogFileName (optarg);
                if (getPropertiesLogFileName () == NULL) {
                    fprintf (stderr, "Parse log file name error!\n");
                    return -1;
                }
                break;

            case 'l':
                updatePropertiesLogLevel (atoi (optarg));
                break;

            case 'v':
                showVersion = true;
                break;

            case 'h':
                showHelp = true;
                break;

            case '?':
                fprintf (stderr, "Unknown option.\n");
                showHelpInfo (argv [0]);
                return -1;
        }
    }

    if (showVersion || showHelp) {
        if (showVersion)
            fprintf (stdout, "Current version: %s\n", VERSION_STRING);
        if (showHelp)
            showHelpInfo (argv [0]);
        exit (0);
    }

    return 0;
}
