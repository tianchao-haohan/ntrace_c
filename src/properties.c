#include <ini_config.h>
#include "config.h"
#include "log.h"
#include "properties.h"

/* Properties instance */
static propertiesPtr propertiesInstance = NULL;

static propertiesPtr
newProperties (void) {
    propertiesPtr tmp;

    tmp = (propertiesPtr) malloc (sizeof (properties));
    if (tmp == NULL)
        return NULL;

    tmp->daemonMode = 0;
    tmp->mirrorInterface = NULL;
    tmp->breakdownSinkIp = NULL;
    tmp->breakdownSinkPort = 0;
    tmp->logDir = NULL;
    tmp->logFileName = NULL;
    tmp->logLevel = LOG_ERR_LEVEL;
    return tmp;
}

static void
freeProperties (propertiesPtr instance) {
    if (instance == NULL)
        return;

    free (instance->mirrorInterface);
    instance->mirrorInterface = NULL;
    free (instance->breakdownSinkIp);
    instance->breakdownSinkIp = NULL;
    free (instance->logDir);
    instance->logDir = NULL;
    free (instance->logFileName);
    instance->logFileName = NULL;

    free (instance);
}

static void
displayPropertiesDetail (void) {
    fprintf (stdout, "Startup with properties:\n");
    fprintf (stdout, "{\n");
    fprintf (stdout, "    daemonMode: %s\n", propertiesInstance->daemonMode ? "true" : "false");
    fprintf (stdout, "    mirrorInterface: %s\n", propertiesInstance->mirrorInterface);
    fprintf (stdout, "    breakdownSinkIp: %s\n", propertiesInstance->breakdownSinkIp);
    fprintf (stdout, "    breakdownSinkPort: %u\n", propertiesInstance->breakdownSinkPort);
    fprintf (stdout, "    logDir: %s\n", propertiesInstance->logDir);
    fprintf (stdout, "    logFileName: %s\n", propertiesInstance->logFileName);
    fprintf (stdout, "    logLevel: ");
    switch (propertiesInstance->logLevel) {
        case LOG_ERR_LEVEL:
            fprintf (stdout, "ERR\n");
            break;

        case LOG_WARNING_LEVEL:
            fprintf (stdout, "WARNING\n");
            break;

        case LOG_INFO_LEVEL:
            fprintf (stdout, "INFO\n");
            break;

        case LOG_DEBUG_LEVEL:
            fprintf (stdout, "DEBUG\n");
            break;

        default:
            fprintf (stdout, "Unknown\n");
    }
    fprintf (stdout, "}\n");
}

static propertiesPtr
loadPropertiesFromConfigFile (void) {
    int ret, error;
    struct collection_item *iniConfig = NULL;
    struct collection_item *errorSet = NULL;
    struct collection_item *item;
    propertiesPtr tmp;

    /* Alloc new properties */
    tmp = newProperties ();
    if (tmp == NULL) {
        fprintf (stderr, "Alloc new agent config error.\n");
        return NULL;
    }

    /* Load agent properties from AGENT_CONFIG_FILE */
    ret = config_from_file ("Agent", AGENT_CONFIG_FILE,
                            &iniConfig, INI_STOP_ON_ANY, &errorSet);
    if (ret) {
        fprintf (stderr, "Parse config file: %s error.\n", AGENT_CONFIG_FILE);
        goto freeProperties;
    }

    /* Get daemon mode */
    ret = get_config_item ("MAIN", "daemonMode", iniConfig, &item);
    if (ret) {
        fprintf (stderr, "Get_config_item \"daemonMode\" error.\n");
        goto freeProperties;
    }
    tmp->daemonMode = get_int_config_value (item, 1, 0, &error);
    if (error) {
        fprintf (stderr, "Parse \"daemonMode\" error.\n");
        goto freeProperties;
    }

    /* Get mirror interface */
    ret = get_config_item ("MAIN", "mirrorInterface", iniConfig, &item);
    if (ret) {
        fprintf (stderr, "Get_config_item \"mirrorInterface\" error.\n");
        goto freeProperties;
    }
    tmp->mirrorInterface = strdup (get_const_string_config_value (item, &error));
    if (tmp->mirrorInterface == NULL) {
        fprintf (stderr, "Get \"mirrorInterface\" error.\n");
        goto freeProperties;
    }

    /* Get breakdown sink ip */
    ret = get_config_item ("MAIN", "breakdownSinkIp", iniConfig, &item);
    if (ret) {
        fprintf (stderr, "Get_config_item \"breakdownSinkIp\" error.\n");
        goto freeProperties;
    }
    tmp->breakdownSinkIp = strdup (get_const_string_config_value (item, &error));
    if (tmp->breakdownSinkIp == NULL) {
        fprintf (stderr, "Get \"breakdownSinkIp\" error.\n");
        goto freeProperties;
    }

    /* Get breakdown sink port */
    ret = get_config_item ("MAIN", "breakdownSinkPort", iniConfig, &item);
    if (ret) {
        fprintf (stderr, "Get_config_item \"breakdownSinkPort\" error.\n");
        goto freeProperties;
    }
    tmp->breakdownSinkPort = get_int_config_value (item, 1, 0, &error);
    if (error) {
        fprintf (stderr, "Get \"breakdownSinkPort\" error.\n");
        goto freeProperties;
    }

    /* Get log dir */
    ret = get_config_item ("LOG", "logDir", iniConfig, &item);
    if (ret) {
        fprintf (stderr, "Get_config_item \"logDir\" error.\n");
        goto freeProperties;
    }
    tmp->logDir = strdup (get_const_string_config_value (item, &error));
    if (error) {
        fprintf (stderr, "Get \"logDir\" error.\n");
        goto freeProperties;
    }

    /* Get log file name */
    ret = get_config_item ("LOG", "logFileName", iniConfig, &item);
    if (ret) {
        fprintf (stderr, "Get_config_item \"logFileName\" error.\n");
        goto freeProperties;
    }
    tmp->logFileName = strdup (get_const_string_config_value (item, &error));
    if (error) {
        fprintf (stderr, "Get \"logFileName\" error.\n");
        goto freeProperties;
    }

    /* Get log level */
    ret = get_config_item ("LOG", "logLevel", iniConfig, &item);
    if (ret) {
        fprintf (stderr, "Get_config_item \"logLevel\" error.\n");
        goto freeProperties;
    }
    tmp->logLevel = get_int_config_value (item, 1, -1, &error);
    if (error) {
        fprintf (stderr, "Parse \"logLevel\" error.\n");
        goto freeProperties;
    }
    /* Return properties in the last */
    goto exit;

freeProperties:
    freeProperties (tmp);
    tmp = NULL;
exit:
    if (iniConfig)
        free_ini_config (iniConfig);
    if (errorSet)
        free_ini_config_errors (errorSet);
    return tmp;
}

boolean
getPropertiesDaemonMode (void) {
    return propertiesInstance->daemonMode;
}

void
updatePropertiesDaemonMode (boolean daemonMode) {
    propertiesInstance->daemonMode = daemonMode;
}

char *
getPropertiesMirrorInterface (void) {
    return propertiesInstance->mirrorInterface;
}

void
updatePropertiesMirrorInterface (char *mirrorInterface) {
    free (propertiesInstance->mirrorInterface);
    propertiesInstance->mirrorInterface = strdup (mirrorInterface);
}

char *
getPropertiesBreakdownSinkIp (void) {
    return propertiesInstance->breakdownSinkIp;
}

void
updatePropertiesBreakdownSinkIp (char *ip) {
    free (propertiesInstance->breakdownSinkIp);
    propertiesInstance->breakdownSinkIp = strdup (ip);
}

u_short
getPropertiesBreakdownSinkPort (void) {
    return propertiesInstance->breakdownSinkPort;
}

void
updatePropertiesBreakdownSinkPort (u_short port) {
    propertiesInstance->breakdownSinkPort = port;
}

u_int
getPropertiesLogLevel (void) {
    return propertiesInstance->logLevel;
}

char *
getPropertiesLogDir (void) {
    return propertiesInstance->logDir;
}

void
updatePropertiesLogDir (char *path) {
    free (propertiesInstance->logDir);
    propertiesInstance->logDir = strdup (path);
}

char *
getPropertiesLogFileName (void) {
    return propertiesInstance->logFileName;
}

void
updatePropertiesLogFileName (char *fileName) {
    free (propertiesInstance->logFileName);
    propertiesInstance->logFileName = strdup (fileName);
}

void
updatePropertiesLogLevel (u_int logLevel) {
    propertiesInstance->logLevel = logLevel;
}

int
initProperties (void) {
    propertiesInstance = loadPropertiesFromConfigFile ();
    if (propertiesInstance == NULL)
        return -1;

    displayPropertiesDetail ();
    return 0;
}

void
destroyProperties (void) {
    freeProperties (propertiesInstance);
    propertiesInstance = NULL;
}
