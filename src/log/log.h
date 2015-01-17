#ifndef __LOG_H__
#define __LOG_H__

#define LOG_ERR_LEVEL 0
#define LOG_WARNING_LEVEL 1
#define LOG_INFO_LEVEL 2
#define LOG_DEBUG_LEVEL 3

#define LOG_SERVICE_PULL_PORT 59001
#define LOG_SERVICE_PUBLISH_PORT 59002

/*========================Interfaces definition============================*/
void
logToConsole (const char *msg, ...);
void
doLog (u_char logLevel, char *filePath, u_int line, const char *func, const char *msg, ...);
int
initLog (u_int logLevel);
void
destroyLog (void);

#define LOGE(...) doLog (LOG_ERR_LEVEL, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGW(...) doLog (LOG_WARNING_LEVEL, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOGI(...) doLog (LOG_INFO_LEVEL, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#ifdef NDEBUG
#define LOGD(...)
#else
#define LOGD(...) doLog (LOG_DEBUG_LEVEL, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#endif
/*=======================Interfaces definition end=========================*/

#endif /* __LOG_H__ */
