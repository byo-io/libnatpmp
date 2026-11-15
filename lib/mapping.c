#include "mapping.h"

#include <assert.h>

static bool natpmp_mapping_released(natpmp_mapping_t const *mapping) {
  assert(mapping);
  return mapping->request.lifetime == 0;
}

static bool natpmp_mapping_has_expiration(natpmp_mapping_t const *mapping) {
  assert(mapping);
  return mapping->expires_at > 0;
}

static bool natpmp_mapping_expired(natpmp_mapping_t const *mapping,
                                   natpmp_time_ms_t now) {
  assert(mapping);
  return natpmp_mapping_has_expiration(mapping) && mapping->expires_at <= now;
}

void natpmp_mapping_release(natpmp_mapping_t *mapping, natpmp_queue_t *queue) {
  assert(mapping);
  assert(queue);

  if (natpmp_mapping_released(mapping)) {
    return;
  }

  // what a deletion asks for
  auto const request = &mapping->request;
  request->external_port = 0;
  request->lifetime = 0;

  switch (request->state) {
  case NATPMP_REQUEST_QUEUED:
    // goes out as a deletion when its turn comes
    return;
  case NATPMP_REQUEST_SENT:
    // still the head, and goes out again right away as a deletion
    request->state = NATPMP_REQUEST_QUEUED;
    return;
  case NATPMP_REQUEST_IDLE:
  case NATPMP_REQUEST_ANSWERED:
  case NATPMP_REQUEST_FAILED:
    // what came of the request goes unreported
    natpmp_queue_push(queue, request);
    return;
  }
  unreachable();
}

static bool natpmp_response_transient_error(natpmp_response_t const *response) {
  assert(response);

  return response->type == NATPMP_RESPONSE_ERROR &&
         (response->error.result == NATPMP_RESULT_NETWORK_FAILURE ||
          response->error.result == NATPMP_RESULT_OUT_OF_RESOURCES);
}

static natpmp_optional_event_t natpmp_mapping_fail(natpmp_mapping_t *mapping) {
  assert(mapping);

  auto const handle = mapping->handle;
  *mapping = (natpmp_mapping_t){};
  return (natpmp_optional_event_t){
      .has_value = true,
      .value = {.type = NATPMP_EVENT_MAP_FAILED,
                .map_failed = {.handle = handle}},
  };
}

static natpmp_optional_event_t
natpmp_mapping_take_answer(natpmp_mapping_t *mapping) {
  assert(mapping);

  auto const request = &mapping->request;
  auto const response = &request->response;
  if (response->type == NATPMP_RESPONSE_ERROR || response->map.lifetime == 0) {
    // refused, or mapped for no time, which is no mapping
    return natpmp_mapping_fail(mapping);
  }

  request->external_port = response->map.external_port;
  request->lifetime = response->map.lifetime;
  // halfway through the lifetime
  auto const granted_at = request->answered_at;
  mapping->renew_at = granted_at + (natpmp_time_ms_t)request->lifetime * 500;
  mapping->expires_at = granted_at + (natpmp_time_ms_t)request->lifetime * 1000;
  return (natpmp_optional_event_t){
      .has_value = true,
      .value = {.type = NATPMP_EVENT_MAPPED,
                .mapped = {.handle = mapping->handle,
                           .external_port = request->external_port,
                           .lifetime = request->lifetime}},
  };
}

natpmp_optional_event_t natpmp_mapping_poll(natpmp_mapping_t *mapping,
                                            natpmp_queue_t *queue,
                                            natpmp_time_ms_t now) {
  assert(mapping);
  assert(mapping->handle != 0);
  assert(queue);

  auto const request = &mapping->request;
  if (request->state == NATPMP_REQUEST_QUEUED ||
      request->state == NATPMP_REQUEST_SENT) {
    return (natpmp_optional_event_t){};
  }

  if (request->state != NATPMP_REQUEST_IDLE) {
    auto const retry = request->state == NATPMP_REQUEST_FAILED ||
                       natpmp_response_transient_error(&request->response);
    request->state = NATPMP_REQUEST_IDLE;

    if (natpmp_mapping_released(mapping)) {
      // deleted, refused, or given up on and can be left to expire
      *mapping = (natpmp_mapping_t){};
      return (natpmp_optional_event_t){};
    }

    if (!retry) {
      return natpmp_mapping_take_answer(mapping);
    }

    if (!natpmp_mapping_has_expiration(mapping)) {
      return natpmp_mapping_fail(mapping);
    }

    // when a renewal goes unanswered or fails for now, try again halfway
    // through the remaining lifetime to avoid a dangling mapping on the gateway
    // that's not tracked
    mapping->renew_at += (mapping->expires_at - mapping->renew_at) / 2;
  }

  if (natpmp_mapping_expired(mapping, now)) {
    return natpmp_mapping_fail(mapping);
  }

  if (mapping->renew_at <= now) {
    natpmp_queue_push(queue, request);
  }

  return (natpmp_optional_event_t){};
}

void natpmp_mapping_recover(natpmp_mapping_t *mapping,
                            natpmp_time_ms_t renew_at) {
  assert(mapping);

  // a grant not taken yet is gone with the gateway's other mappings, and an
  // unanswered renewal is overtaken, so renew either one instead
  auto const request = &mapping->request;
  auto const granted = request->state == NATPMP_REQUEST_ANSWERED &&
                       request->response.type == NATPMP_RESPONSE_MAP &&
                       request->response.map.lifetime > 0;
  auto const renewal_unanswered = request->state == NATPMP_REQUEST_FAILED &&
                                  natpmp_mapping_has_expiration(mapping) &&
                                  !natpmp_mapping_released(mapping);

  if (granted || renewal_unanswered) {
    request->state = NATPMP_REQUEST_IDLE;
  }

  // one on the wire goes to the gateway as it is now
  if (request->state != NATPMP_REQUEST_IDLE) {
    return;
  }

  // a mapping close to its expiry can get a delay that runs past it, so cap the
  // delay at the expiry, where the mapping fails on time instead
  mapping->renew_at =
      natpmp_mapping_has_expiration(mapping) && mapping->expires_at < renew_at
          ? mapping->expires_at
          : renew_at;
}
