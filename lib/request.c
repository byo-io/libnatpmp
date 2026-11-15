#include "request.h"

#include <assert.h>

bool natpmp_request_matches(natpmp_request_t const *request,
                            natpmp_response_t const *response) {
  assert(request);
  assert(response);

  switch (response->type) {
  case NATPMP_RESPONSE_ERROR:
    return response->error.opcode == request->opcode;
  case NATPMP_RESPONSE_ADDRESS:
    return request->opcode == NATPMP_OPCODE_EXTERNAL_ADDRESS;
  case NATPMP_RESPONSE_MAP:
    // a deletion waits for its own answer, not a late grant
    return response->map.opcode == request->opcode &&
           response->map.internal_port == request->internal_port &&
           (request->lifetime > 0 || response->map.lifetime == 0);
  }
  unreachable();
}

natpmp_result_t natpmp_request_refusal(natpmp_request_t const *request) {
  assert(request);

  return request->state == NATPMP_REQUEST_ANSWERED &&
                 request->response.type == NATPMP_RESPONSE_ERROR
             ? request->response.error.result
             : NATPMP_RESULT_SUCCESS;
}

void natpmp_request_answer(natpmp_request_t *request,
                           natpmp_time_ms_t now,
                           natpmp_response_t const *response) {
  assert(request);
  assert(response);

  request->state = NATPMP_REQUEST_ANSWERED;
  request->response = *response;
  request->answered_at = now;
}
