#ifndef CAMSTREAM_COMMON_LOG_H
#define CAMSTREAM_COMMON_LOG_H

#ifndef CAMSTREAM_LOG_MODULE
#error "Define CAMSTREAM_LOG_MODULE before including camstream/common/log.h"
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum camstream_log_level
    {
        CAMSTREAM_LOG_ERROR,
        CAMSTREAM_LOG_WARN,
        CAMSTREAM_LOG_INFO,
        CAMSTREAM_LOG_DEBUG
    } camstream_log_level;

    /** @brief Writes one allocation-free, module-tagged diagnostic to standard error. */
    void camstream_log_write(const char* module, camstream_log_level level, const char* format, ...);

#ifdef __cplusplus
}
#endif

#define CAM_LOGE(...) camstream_log_write(CAMSTREAM_LOG_MODULE, CAMSTREAM_LOG_ERROR, __VA_ARGS__)
#define CAM_LOGW(...) camstream_log_write(CAMSTREAM_LOG_MODULE, CAMSTREAM_LOG_WARN, __VA_ARGS__)
#define CAM_LOGI(...) camstream_log_write(CAMSTREAM_LOG_MODULE, CAMSTREAM_LOG_INFO, __VA_ARGS__)
#ifdef CAMSTREAM_ENABLE_DEBUG_LOG
#define CAM_LOGD(...) camstream_log_write(CAMSTREAM_LOG_MODULE, CAMSTREAM_LOG_DEBUG, __VA_ARGS__)
#else
#define CAM_LOGD(...) ((void)0)
#endif

#endif
