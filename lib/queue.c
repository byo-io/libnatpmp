#include "queue.h"

#include <assert.h>

void natpmp_queue_push(natpmp_queue_t *queue, natpmp_request_t *request) {
  assert(queue);
  assert(request);
  assert(request->state != NATPMP_REQUEST_QUEUED &&
         request->state != NATPMP_REQUEST_SENT);

  natpmp_request_t **last = &queue->head;
  while (*last) {
    last = &(*last)->next;
  }
  request->state = NATPMP_REQUEST_QUEUED;
  request->next = nullptr;
  *last = request;
}

void natpmp_queue_release(natpmp_queue_t *queue, natpmp_request_t *request) {
  assert(queue);
  assert(request);
  assert(request->opcode != NATPMP_OPCODE_EXTERNAL_ADDRESS);

  if (natpmp_request_released(request)) {
    return;
  }

  // what a deletion asks for
  request->map.external_port = 0;
  request->map.lifetime = 0;

  switch (request->state) {
  case NATPMP_REQUEST_FREE:
    // not in use
    unreachable();
  case NATPMP_REQUEST_QUEUED:
    // goes out as a deletion when its turn comes
    return;
  case NATPMP_REQUEST_SENT:
    // still the head, and goes out again right away as a deletion
    request->state = NATPMP_REQUEST_QUEUED;
    return;
  case NATPMP_REQUEST_ANSWERED:
  case NATPMP_REQUEST_ACTIVE:
    natpmp_queue_push(queue, request);
    return;
  case NATPMP_REQUEST_FAILED:
    // there's nothing to delete, and its failure goes unreported
    request->state = NATPMP_REQUEST_FREE;
    return;
  }
}

static void natpmp_queue_pop(natpmp_queue_t *queue) {
  queue->head = queue->head->next;
}

void natpmp_queue_fail_all(natpmp_queue_t *queue) {
  while (queue->head) {
    natpmp_request_fail(queue->head);
    natpmp_queue_pop(queue);
  }
}

natpmp_queue_status_t natpmp_queue_poll(natpmp_queue_t *queue,
                                        natpmp_time_ms_t now) {
  assert(queue);

  auto const request = queue->head;
  assert(request);

  if (request->state == NATPMP_REQUEST_QUEUED) {
    // a new head, whose retransmissions start from scratch
    queue->retransmit = (natpmp_retransmit_t){};
  }

  auto const status = natpmp_retransmit_poll(&queue->retransmit, now);
  if (status != NATPMP_RETRANSMIT_DUE) {
    return (natpmp_queue_status_t){.status = status};
  }

  view_t const dst = {.size = sizeof(queue->data), .data = queue->data};
  auto const size =
      request->opcode == NATPMP_OPCODE_EXTERNAL_ADDRESS
          ? natpmp_packet_write_address_request(dst)
          : natpmp_packet_write_map_request(
                dst, request->opcode, request->map.internal_port,
                request->map.external_port, request->map.lifetime);
  request->state = NATPMP_REQUEST_SENT;
  natpmp_retransmit_advance(&queue->retransmit, now);
  return (natpmp_queue_status_t){.status = NATPMP_RETRANSMIT_DUE, .size = size};
}

void natpmp_queue_on_response(natpmp_queue_t *queue,
                              natpmp_time_ms_t now,
                              natpmp_response_t const *response) {
  assert(queue);
  assert(response);

  if (!queue->head || queue->head->state != NATPMP_REQUEST_SENT ||
      !natpmp_request_matches(queue->head, response)) {
    return;
  }

  natpmp_request_apply_answer(queue->head, response, now);
  natpmp_queue_pop(queue);
}
