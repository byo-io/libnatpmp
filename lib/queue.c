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

void natpmp_queue_answer(natpmp_queue_t *queue,
                         natpmp_request_t *request,
                         natpmp_time_ms_t now,
                         natpmp_response_t const *response) {
  assert(queue);
  assert(request);
  assert(response);
  assert(request->state == NATPMP_REQUEST_QUEUED ||
         request->state == NATPMP_REQUEST_SENT);

  natpmp_request_t **link = &queue->head;
  while (*link != request) {
    assert(*link);
    link = &(*link)->next;
  }
  *link = request->next;
  natpmp_request_answer(request, now, response);
}

static void natpmp_queue_pop(natpmp_queue_t *queue) {
  queue->head = queue->head->next;
}

void natpmp_queue_fail_all(natpmp_queue_t *queue) {
  while (queue->head) {
    queue->head->state = NATPMP_REQUEST_FAILED;
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
  auto const size = request->opcode == NATPMP_OPCODE_EXTERNAL_ADDRESS
                        ? natpmp_packet_write_address_request(dst)
                        : natpmp_packet_write_map_request(
                              dst, request->opcode, request->internal_port,
                              request->external_port, request->lifetime);
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

  natpmp_queue_answer(queue, queue->head, now, response);
}
