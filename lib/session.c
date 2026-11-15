#include <byo/natpmp/natpmp.h>

#include "gateway.h"
#include "mapping.h"
#include "packet.h"
#include "queue.h"
#include "request.h"
#include "retransmit.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>

#define ALLOC(allocator, ...) (allocator.alloc(allocator.ctx, __VA_ARGS__))
#define FREE(allocator, ...) (allocator.free(allocator.ctx, __VA_ARGS__))

static constexpr uint16_t NATPMP_DEFAULT_CAPACITY = 8;

struct natpmp_session {
  natpmp_config_t config;
  natpmp_handle_t handles;
  natpmp_gateway_t gateway;
  natpmp_queue_t outgoing;
  /// Shared by every caller until its event is polled
  natpmp_request_t address_request;
  /// `config.capacity` slots
  natpmp_mapping_t mappings[];
};

static void *natpmp_default_alloc(void * /*ctx*/, size_t size) {
  return malloc(size);
}

static void natpmp_default_free(void * /*ctx*/, void *ptr) { free(ptr); }

natpmp_session_t *natpmp_session_begin(natpmp_config_t config) {
  if (!config.allocator.alloc && !config.allocator.free) {
    config.allocator.alloc = natpmp_default_alloc;
    config.allocator.free = natpmp_default_free;
  }
  assert(config.allocator.alloc && config.allocator.free);
  assert(config.rng.fill);

  if (config.capacity == 0) {
    config.capacity = NATPMP_DEFAULT_CAPACITY;
  }

  natpmp_session_t *session =
      ALLOC(config.allocator, sizeof(natpmp_session_t) +
                                  config.capacity * sizeof(natpmp_mapping_t));
  if (session) {
    *session = (natpmp_session_t){
        .config = config,
        .address_request = {.opcode = NATPMP_OPCODE_EXTERNAL_ADDRESS},
    };
    auto const begin = session->mappings;
    auto const end = begin + config.capacity;
    for (auto mapping = begin; mapping != end; ++mapping) {
      *mapping = (natpmp_mapping_t){};
    }
  }

  return session;
}

void natpmp_session_end(natpmp_session_t *session) {
  assert(session);
  FREE(session->config.allocator, session);
}

static natpmp_handle_or_error_t natpmp_session_add(natpmp_session_t *session,
                                                   natpmp_request_t request) {
  auto const begin = session->mappings;
  auto const end = begin + session->config.capacity;
  for (auto slot = begin; slot != end; ++slot) {
    if (slot->handle == 0) {
      // 0 marks a free slot
      if (++session->handles == 0) {
        ++session->handles;
      }
      *slot =
          (natpmp_mapping_t){.handle = session->handles, .request = request};
      natpmp_queue_push(&session->outgoing, &slot->request);
      return (natpmp_handle_or_error_t){.has_value = true,
                                        .value = slot->handle};
    }
  }

  return (natpmp_handle_or_error_t){.has_value = false, .error = ENOBUFS};
}

void natpmp_session_address_request(natpmp_session_t *session) {
  assert(session);

  if (session->address_request.state != NATPMP_REQUEST_IDLE) {
    // the earlier request's event answers this one too
    return;
  }

  natpmp_queue_push(&session->outgoing, &session->address_request);
}

natpmp_handle_or_error_t
natpmp_session_map_request(natpmp_session_t *session,
                           natpmp_time_s_t lifetime,
                           natpmp_protocol_t protocol,
                           natpmp_port_t port,
                           natpmp_port_t external_port) {
  assert(session);
  // lifetime 0 means delete, and port 0 deletes all mappings
  assert(lifetime > 0);
  assert(port > 0);
  assert(protocol == NATPMP_PROTOCOL_UDP || protocol == NATPMP_PROTOCOL_TCP);

  return natpmp_session_add(session,
                            (natpmp_request_t){
                                .opcode = protocol == NATPMP_PROTOCOL_UDP
                                              ? NATPMP_OPCODE_MAP_UDP
                                              : NATPMP_OPCODE_MAP_TCP,
                                .internal_port = port,
                                .external_port = external_port,
                                .lifetime = lifetime,
                            });
}

// the mapping with the handle, if it's still in its slot
static natpmp_mapping_t *natpmp_session_find(natpmp_session_t *session,
                                             natpmp_handle_t handle) {
  assert(session);

  auto const begin = session->mappings;
  auto const end = begin + session->config.capacity;
  for (auto mapping = begin; mapping != end; ++mapping) {
    if (mapping->handle != 0 && mapping->handle == handle) {
      return mapping;
    }
  }

  return nullptr;
}

void natpmp_session_release(natpmp_session_t *session, natpmp_handle_t handle) {
  assert(session);

  auto const mapping = natpmp_session_find(session, handle);
  if (mapping) {
    natpmp_mapping_release(mapping, &session->outgoing);
  }
}

static natpmp_optional_event_t
natpmp_session_take_address(natpmp_session_t *session) {
  auto const request = &session->address_request;
  if (request->state != NATPMP_REQUEST_ANSWERED &&
      request->state != NATPMP_REQUEST_FAILED) {
    return (natpmp_optional_event_t){};
  }

  // a refusal answers it too, as a failure
  auto const answered = request->state == NATPMP_REQUEST_ANSWERED &&
                        request->response.type == NATPMP_RESPONSE_ADDRESS;
  auto const refusal = natpmp_request_refusal(request);
  request->state = NATPMP_REQUEST_IDLE;
  if (answered) {
    return (natpmp_optional_event_t){
        .has_value = true,
        .value = {.type = NATPMP_EVENT_ADDRESS,
                  .address = {.external_ip =
                                  request->response.address.external_ip}},
    };
  }

  return (natpmp_optional_event_t){
      .has_value = true,
      .value = {.type = NATPMP_EVENT_ADDRESS_FAILED,
                .address_failed = {.result = refusal}},
  };
}

natpmp_event_t natpmp_session_poll(natpmp_session_t *session,
                                   natpmp_time_ms_t now) {
  assert(session);

  // report what's answered or failed, and queue any renewals that are due,
  // before sending anything
  auto const address = natpmp_session_take_address(session);
  if (address.has_value) {
    return address.value;
  }

  natpmp_optional_time_ms_t renewal = {};
  auto const begin = session->mappings;
  auto const end = begin + session->config.capacity;
  for (auto mapping = begin; mapping != end; ++mapping) {
    if (mapping->handle == 0) {
      continue;
    }

    auto const event = natpmp_mapping_poll(mapping, &session->outgoing, now);
    if (event.has_value) {
      return event.value;
    }

    if (mapping->handle != 0 && mapping->request.state == NATPMP_REQUEST_IDLE &&
        (!renewal.has_value || mapping->renew_at < renewal.value)) {
      renewal = (natpmp_optional_time_ms_t){.has_value = true,
                                            .value = mapping->renew_at};
    }
  }

  // if nothing is ready to send, the next renewal is the session's timeout
  if (!session->outgoing.head) {
    return (natpmp_event_t){.type = NATPMP_EVENT_TIMEOUT,
                            .timeout = {.deadline = renewal}};
  }

  // send the head of the queue, or wait for its answer
  auto const out = natpmp_queue_poll(&session->outgoing, now);
  switch (out.status) {
  case NATPMP_RETRANSMIT_WAIT:
    return (natpmp_event_t){
        .type = NATPMP_EVENT_TIMEOUT,
        .timeout = {.deadline = {.has_value = true,
                                 .value = session->outgoing.retransmit.next}},
    };
  case NATPMP_RETRANSMIT_DUE:
    return (natpmp_event_t){
        .type = NATPMP_EVENT_SEND,
        .send = {.size = out.size, .data = session->outgoing.data},
    };
  case NATPMP_RETRANSMIT_GAVE_UP:
    natpmp_queue_fail_all(&session->outgoing);
    // there are FAILED events to report now
    return natpmp_session_poll(session, now);
  }
  unreachable();
}

// renews every mapping within 5 s, after the gateway lost them
static void natpmp_session_recover(natpmp_session_t *session,
                                   natpmp_time_ms_t now) {
  assert(session);

  // a random delay spreads out the clients that all saw the gateway reboot
  uint32_t random = 0;
  if (!session->config.rng.fill(session->config.rng.ctx, sizeof(random),
                                (uint8_t *)&random)) {
    random = 0;
  }
  auto const renew_at = now + random % 5001;

  auto const begin = session->mappings;
  auto const end = begin + session->config.capacity;
  for (auto mapping = begin; mapping != end; ++mapping) {
    if (mapping->handle != 0) {
      natpmp_mapping_recover(mapping, renew_at);
    }
  }
}

void natpmp_session_on_datagram(natpmp_session_t *session,
                                natpmp_time_ms_t now,
                                size_t size,
                                uint8_t const data[]) {
  assert(session);
  assert(data || size == 0);

  auto const response =
      natpmp_packet_read_response((const_view_t){.size = size, .data = data});
  if (!response.has_value) {
    return;
  }

  auto const update =
      natpmp_gateway_update(&session->gateway, now, &response.value);
  if (update.address == NATPMP_GATEWAY_ADDRESS_STALE) {
    return;
  }

  // the response still answers the request on the wire, which went to the
  // gateway as it is now, so only the mappings granted before it get renewed
  if (update.rebooted) {
    natpmp_session_recover(session, now);
  }

  if (response.value.type == NATPMP_RESPONSE_ADDRESS) {
    // a waiting address request takes the first current address, even one it
    // isn't the answer to. Otherwise only a new address is reported.
    auto const request = &session->address_request;
    if (request->state == NATPMP_REQUEST_QUEUED ||
        request->state == NATPMP_REQUEST_SENT) {
      natpmp_queue_answer(&session->outgoing, request, now, &response.value);
    } else if (update.address == NATPMP_GATEWAY_ADDRESS_NEW) {
      natpmp_request_answer(request, now, &response.value);
    }
  }

  natpmp_queue_on_response(&session->outgoing, now, &response.value);
}

void natpmp_session_on_host_unreachable(natpmp_session_t *session) {
  assert(session);
  natpmp_queue_fail_all(&session->outgoing);
}
