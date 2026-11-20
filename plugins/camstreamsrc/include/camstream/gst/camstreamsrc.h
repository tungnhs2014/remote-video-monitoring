#ifndef CAMSTREAM_GST_CAMSTREAMSRC_H
#define CAMSTREAM_GST_CAMSTREAMSRC_H

#include <camstream/camera/camera_hal.h>
#include <gst/base/gstpushsrc.h>

G_BEGIN_DECLS

#define CAMSTREAM_TYPE_SRC (camstream_src_get_type())
G_DECLARE_FINAL_TYPE(CamstreamSrc, camstream_src, CAMSTREAM, SRC, GstPushSrc)

/**
 * @brief Borrows a configured HAL camera and its driver-confirmed active configuration.
 *
 * The session owner starts HAL before the source enters PAUSED, and stops/destroys HAL only after the pipeline
 * reaches NULL and any pending release is resolved. The owner serializes its HAL calls with source activity.
 * Call only while the source is stopped. Passing NULL for both arguments unbinds a quiescent source; an unresolved
 * release prevents unbinding so the borrowed camera remains available for cleanup.
 */
gboolean camstream_src_bind(
        CamstreamSrc* source, camstream_camera* camera, const camstream_camera_stream_config_v1* active);

/** @brief Closes source admission before the owner transitions the pipeline toward NULL; performs no HAL call. */
void camstream_src_request_stop(CamstreamSrc* source);

/** @brief Retries a failed frame release after the pipeline is stopped; the session retains HAL ownership meanwhile. */
gboolean camstream_src_retry_pending_release(CamstreamSrc* source);

#ifdef CAMSTREAM_M4_TESTING
/** @brief Host fault-injection seam; affects the next acquired frame only. */
void camstream_src_test_fail_allocation_once(CamstreamSrc* source);
#endif

G_END_DECLS

#endif
