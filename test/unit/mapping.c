#include <unity.h>

#include "mapping.h"
#include "runner.h"

static natpmp_queue_t queue;

// asked for, and on the wire
static natpmp_mapping_t requested() {
  queue = (natpmp_queue_t){};
  return (natpmp_mapping_t){
      .handle = 1,
      .request = {.state = NATPMP_REQUEST_SENT,
                  .opcode = NATPMP_OPCODE_MAP_TCP,
                  .internal_port = 0x1234,
                  .external_port = 0x1111,
                  .lifetime = 3600},
  };
}

static natpmp_response_t const grant = {
    .type = NATPMP_RESPONSE_MAP,
    .map = {.opcode = NATPMP_OPCODE_MAP_TCP,
            .internal_port = 0x1234,
            .external_port = 0x5678,
            .lifetime = 7200},
};

static natpmp_response_t const refusal = {
    .type = NATPMP_RESPONSE_ERROR,
    .error = {.result = 2, .opcode = NATPMP_OPCODE_MAP_TCP},
};

static natpmp_optional_event_t poll(natpmp_mapping_t *mapping,
                                    natpmp_time_ms_t now) {
  return natpmp_mapping_poll(mapping, &queue, now);
}

static natpmp_optional_event_t answer(natpmp_mapping_t *mapping,
                                      natpmp_time_ms_t now,
                                      natpmp_response_t const *response) {
  natpmp_request_answer(&mapping->request, now, response);
  return poll(mapping, now);
}

// granted at 0, renewing at 3600 s and expiring at 7200 s
static natpmp_mapping_t mapped() {
  auto mapping = requested();
  answer(&mapping, 0, &grant);
  return mapping;
}

static void assert_map_failed(natpmp_optional_event_t event,
                              natpmp_mapping_t const *mapping) {
  TEST_ASSERT_TRUE(event.has_value);
  TEST_ASSERT_EQUAL(NATPMP_EVENT_MAP_FAILED, event.value.type);
  TEST_ASSERT_EQUAL_UINT16(1, event.value.map_failed.handle);
  TEST_ASSERT_EQUAL_UINT16(0, mapping->handle);
}

static void test_mapping_waits_for_its_request() {
  auto mapping = requested();
  TEST_ASSERT_FALSE(poll(&mapping, 0).has_value);
  TEST_ASSERT_EQUAL(NATPMP_REQUEST_SENT, mapping.request.state);
}

static void test_mapping_granted() {
  auto mapping = requested();
  auto const event = answer(&mapping, 1000, &grant);
  TEST_ASSERT_TRUE(event.has_value);
  TEST_ASSERT_EQUAL(NATPMP_EVENT_MAPPED, event.value.type);
  TEST_ASSERT_EQUAL_UINT16(1, event.value.mapped.handle);
  TEST_ASSERT_EQUAL_UINT16(0x5678, event.value.mapped.external_port);
  TEST_ASSERT_EQUAL_UINT32(7200, event.value.mapped.lifetime);

  // and asks for the same when it renews, halfway through the lifetime
  TEST_ASSERT_EQUAL(NATPMP_REQUEST_IDLE, mapping.request.state);
  TEST_ASSERT_EQUAL_UINT16(0x5678, mapping.request.external_port);
  TEST_ASSERT_EQUAL_UINT32(7200, mapping.request.lifetime);
  TEST_ASSERT_EQUAL_UINT64(1000 + 3600 * 1000, mapping.renew_at);
  TEST_ASSERT_EQUAL_UINT64(1000 + 7200 * 1000, mapping.expires_at);
}

static void test_mapping_refused() {
  auto mapping = requested();
  assert_map_failed(answer(&mapping, 0, &refusal), &mapping);

  // and a mapping for no time is no mapping
  mapping = requested();
  auto none = grant;
  none.map.lifetime = 0;
  assert_map_failed(answer(&mapping, 0, &none), &mapping);
}

static void test_mapping_unanswered() {
  auto mapping = requested();
  mapping.request.state = NATPMP_REQUEST_FAILED;
  assert_map_failed(poll(&mapping, 0), &mapping);
}

static void test_mapping_renews() {
  auto mapping = mapped();
  TEST_ASSERT_FALSE(poll(&mapping, 3'599'999).has_value);
  TEST_ASSERT_NULL(queue.head);

  TEST_ASSERT_FALSE(poll(&mapping, 3'600'000).has_value);
  TEST_ASSERT_EQUAL_PTR(&mapping.request, queue.head);
}

static void test_mapping_renewal_unanswered() {
  // tries again halfway to the expiry, and again after that
  auto mapping = mapped();
  mapping.request.state = NATPMP_REQUEST_FAILED;
  TEST_ASSERT_FALSE(poll(&mapping, 3'700'000).has_value);
  TEST_ASSERT_EQUAL_UINT64(5'400'000, mapping.renew_at);
  mapping.request.state = NATPMP_REQUEST_FAILED;
  TEST_ASSERT_FALSE(poll(&mapping, 5'500'000).has_value);
  TEST_ASSERT_EQUAL_UINT64(6'300'000, mapping.renew_at);

  // until it expires
  mapping.request.state = NATPMP_REQUEST_FAILED;
  assert_map_failed(poll(&mapping, 7'200'000), &mapping);
}

static void test_mapping_renewal_retried_late() {
  // when the retry is already due, it goes out right away
  auto mapping = mapped();
  mapping.request.state = NATPMP_REQUEST_FAILED;
  TEST_ASSERT_FALSE(poll(&mapping, 6'000'000).has_value);
  TEST_ASSERT_EQUAL_PTR(&mapping.request, queue.head);
}

static void test_mapping_renewal_failed_for_now() {
  // tries again like an unanswered one
  auto const failures = (uint16_t[]){NATPMP_RESULT_NETWORK_FAILURE,
                                     NATPMP_RESULT_OUT_OF_RESOURCES};
  for (size_t i = 0; i < 2; ++i) {
    auto mapping = mapped();
    auto failure = refusal;
    failure.error.result = failures[i];
    TEST_ASSERT_FALSE(answer(&mapping, 3'700'000, &failure).has_value);
    TEST_ASSERT_EQUAL_UINT64(5'400'000, mapping.renew_at);

    // but a new mapping fails right away
    mapping = requested();
    assert_map_failed(answer(&mapping, 0, &failure), &mapping);
  }

  // and so does a renewal that's refused
  auto mapping = mapped();
  assert_map_failed(answer(&mapping, 3'700'000, &refusal), &mapping);
}

static void test_mapping_expires() {
  auto mapping = mapped();
  mapping.renew_at = 7'200'000;
  assert_map_failed(poll(&mapping, 7'200'000), &mapping);
}

static void test_mapping_release_asks_for_deletion() {
  auto mapping = mapped();
  natpmp_mapping_release(&mapping, &queue);
  TEST_ASSERT_EQUAL_PTR(&mapping.request, queue.head);
  TEST_ASSERT_EQUAL_UINT16(0, mapping.request.external_port);
  TEST_ASSERT_EQUAL_UINT32(0, mapping.request.lifetime);

  // and what came of its request goes unreported, a failure too
  mapping = requested();
  natpmp_request_answer(&mapping.request, 0, &grant);
  natpmp_mapping_release(&mapping, &queue);
  TEST_ASSERT_EQUAL_PTR(&mapping.request, queue.head);
  mapping = requested();
  mapping.request.state = NATPMP_REQUEST_FAILED;
  natpmp_mapping_release(&mapping, &queue);
  TEST_ASSERT_EQUAL_PTR(&mapping.request, queue.head);
}

static void test_mapping_release_queued() {
  // keeps its place
  auto mapping = requested();
  mapping.request.state = NATPMP_REQUEST_IDLE;
  natpmp_request_t first = {};
  natpmp_queue_push(&queue, &first);
  natpmp_queue_push(&queue, &mapping.request);
  natpmp_mapping_release(&mapping, &queue);
  TEST_ASSERT_EQUAL(NATPMP_REQUEST_QUEUED, mapping.request.state);
  TEST_ASSERT_EQUAL_PTR(&mapping.request, first.next);
  TEST_ASSERT_NULL(mapping.request.next);
}

static void test_mapping_release_sent() {
  // still the head, and goes out again right away as a deletion
  auto mapping = requested();
  mapping.request.state = NATPMP_REQUEST_IDLE;
  natpmp_queue_push(&queue, &mapping.request);
  natpmp_queue_poll(&queue, 0);
  natpmp_mapping_release(&mapping, &queue);
  TEST_ASSERT_EQUAL(NATPMP_REQUEST_QUEUED, mapping.request.state);
  TEST_ASSERT_EQUAL(NATPMP_RETRANSMIT_DUE, natpmp_queue_poll(&queue, 0).status);

  // and once more changes nothing
  natpmp_mapping_release(&mapping, &queue);
  TEST_ASSERT_EQUAL(NATPMP_REQUEST_SENT, mapping.request.state);
}

static void test_mapping_released_frees() {
  // whether the gateway deleted it, refused to, or didn't answer
  auto const outcomes = (natpmp_response_t const *[]){&refusal, nullptr};
  for (size_t i = 0; i < 2; ++i) {
    auto mapping = mapped();
    natpmp_mapping_release(&mapping, &queue);
    if (outcomes[i]) {
      natpmp_request_answer(&mapping.request, 0, outcomes[i]);
    } else {
      mapping.request.state = NATPMP_REQUEST_FAILED;
    }
    TEST_ASSERT_FALSE(poll(&mapping, 0).has_value);
    TEST_ASSERT_EQUAL_UINT16(0, mapping.handle);
  }
}

static void test_mapping_recover() {
  // renews right after the delay
  auto mapping = mapped();
  natpmp_mapping_recover(&mapping, 1000);
  TEST_ASSERT_EQUAL_UINT64(1000, mapping.renew_at);

  // but no later than its expiry
  natpmp_mapping_recover(&mapping, 8'000'000);
  TEST_ASSERT_EQUAL_UINT64(7'200'000, mapping.renew_at);

  // one on the wire goes to the gateway as it is now
  mapping = requested();
  natpmp_mapping_recover(&mapping, 1000);
  TEST_ASSERT_EQUAL(NATPMP_REQUEST_SENT, mapping.request.state);
}

static void test_mapping_recover_drops_grant() {
  // the gateway lost it, so the mapping asks again after the delay
  auto mapping = requested();
  natpmp_request_answer(&mapping.request, 0, &grant);
  natpmp_mapping_recover(&mapping, 1000);
  TEST_ASSERT_FALSE(poll(&mapping, 999).has_value);
  TEST_ASSERT_NULL(queue.head);
  TEST_ASSERT_FALSE(poll(&mapping, 1000).has_value);
  TEST_ASSERT_EQUAL_PTR(&mapping.request, queue.head);
  TEST_ASSERT_EQUAL_UINT16(0x1111, mapping.request.external_port);

  // and so does an unanswered renewal, without waiting for its retry
  mapping = mapped();
  mapping.request.state = NATPMP_REQUEST_FAILED;
  natpmp_mapping_recover(&mapping, 1000);
  TEST_ASSERT_FALSE(poll(&mapping, 1000).has_value);
  TEST_ASSERT_EQUAL_PTR(&mapping.request, queue.head);

  // but a refusal is still reported
  mapping = requested();
  natpmp_request_answer(&mapping.request, 0, &refusal);
  natpmp_mapping_recover(&mapping, 1000);
  assert_map_failed(poll(&mapping, 0), &mapping);
}

void mapping_tests() {
  RUN_TEST(test_mapping_waits_for_its_request);
  RUN_TEST(test_mapping_granted);
  RUN_TEST(test_mapping_refused);
  RUN_TEST(test_mapping_unanswered);
  RUN_TEST(test_mapping_renews);
  RUN_TEST(test_mapping_renewal_unanswered);
  RUN_TEST(test_mapping_renewal_retried_late);
  RUN_TEST(test_mapping_renewal_failed_for_now);
  RUN_TEST(test_mapping_expires);
  RUN_TEST(test_mapping_release_asks_for_deletion);
  RUN_TEST(test_mapping_release_queued);
  RUN_TEST(test_mapping_release_sent);
  RUN_TEST(test_mapping_released_frees);
  RUN_TEST(test_mapping_recover);
  RUN_TEST(test_mapping_recover_drops_grant);
}
