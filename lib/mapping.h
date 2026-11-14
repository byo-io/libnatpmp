#pragma once

#include "queue.h"
#include "request.h"
#include <byo/natpmp/natpmp.h>

/**
 * @brief A mapping the application asked for, from its request until its
 *        failure is reported or its deletion is done
 *
 * It's mapped once it has a grant, and released once its request asks for
 * lifetime 0. Its request asks for what was granted, to renew it.
 */
typedef struct natpmp_mapping {
  /// 0 for a free slot
  natpmp_handle_t handle;
  natpmp_request_t request;
  natpmp_time_ms_t renew_at;
  /// 0 until the first grant
  natpmp_time_ms_t expires_at;
} natpmp_mapping_t;

typedef struct natpmp_optional_event {
  bool has_value;
  natpmp_event_t value;
} natpmp_optional_event_t;

void natpmp_mapping_release(natpmp_mapping_t *mapping, natpmp_queue_t *queue);

/**
 * @brief Take what came of its request, expire it, or queue its renewal
 */
natpmp_optional_event_t natpmp_mapping_poll(natpmp_mapping_t *mapping,
                                            natpmp_queue_t *queue,
                                            natpmp_time_ms_t now);

void natpmp_mapping_recover(natpmp_mapping_t *mapping,
                            natpmp_time_ms_t renew_at);
