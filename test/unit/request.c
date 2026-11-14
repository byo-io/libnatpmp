#include <unity.h>

#include "request.h"
#include "runner.h"

static natpmp_request_t mapping(natpmp_time_s_t lifetime) {
  return (natpmp_request_t){
      .state = NATPMP_REQUEST_SENT,
      .opcode = NATPMP_OPCODE_MAP_TCP,
      .internal_port = 0x1234,
      .external_port = 0x1111,
      .lifetime = lifetime,
  };
}

static natpmp_response_t const grant = {
    .type = NATPMP_RESPONSE_MAP,
    .map = {.opcode = NATPMP_OPCODE_MAP_TCP,
            .internal_port = 0x1234,
            .external_port = 0x5678,
            .lifetime = 7200},
};

static natpmp_response_t const deletion = {
    .type = NATPMP_RESPONSE_MAP,
    .map = {.opcode = NATPMP_OPCODE_MAP_TCP, .internal_port = 0x1234},
};

static natpmp_response_t const refusal = {
    .type = NATPMP_RESPONSE_ERROR,
    .error = {.result = 2, .opcode = NATPMP_OPCODE_MAP_TCP},
};

static natpmp_response_t const address = {
    .type = NATPMP_RESPONSE_ADDRESS,
    .address = {.external_ip = {{203, 0, 113, 7}}},
};

static void test_request_matches() {
  auto const request = mapping(3600);
  TEST_ASSERT_TRUE(natpmp_request_matches(&request, &grant));
  TEST_ASSERT_TRUE(natpmp_request_matches(&request, &refusal));

  // for another protocol or port, or the external address
  auto other = grant;
  other.map.opcode = NATPMP_OPCODE_MAP_UDP;
  TEST_ASSERT_FALSE(natpmp_request_matches(&request, &other));
  other = grant;
  other.map.internal_port = 0x1235;
  TEST_ASSERT_FALSE(natpmp_request_matches(&request, &other));
  auto const error = (natpmp_response_t){
      .type = NATPMP_RESPONSE_ERROR,
      .error = {.result = 3, .opcode = NATPMP_OPCODE_EXTERNAL_ADDRESS}};
  TEST_ASSERT_FALSE(natpmp_request_matches(&request, &error));
  TEST_ASSERT_FALSE(natpmp_request_matches(&request, &address));
}

static void test_request_deletion_matches() {
  // a late grant isn't the answer to a deletion
  auto const request = mapping(0);
  TEST_ASSERT_FALSE(natpmp_request_matches(&request, &grant));
  TEST_ASSERT_TRUE(natpmp_request_matches(&request, &deletion));
  TEST_ASSERT_TRUE(natpmp_request_matches(&request, &refusal));
}

static void test_request_address_matches() {
  natpmp_request_t const request = {.state = NATPMP_REQUEST_SENT,
                                    .opcode = NATPMP_OPCODE_EXTERNAL_ADDRESS};
  TEST_ASSERT_TRUE(natpmp_request_matches(&request, &address));
  TEST_ASSERT_FALSE(natpmp_request_matches(&request, &grant));
  TEST_ASSERT_FALSE(natpmp_request_matches(&request, &deletion));
  TEST_ASSERT_FALSE(natpmp_request_matches(&request, &refusal));
}

static void test_request_answer() {
  // keeps the response for its owner, a refusal too
  auto request = mapping(3600);
  natpmp_request_answer(&request, 0, &refusal);
  TEST_ASSERT_EQUAL(NATPMP_REQUEST_ANSWERED, request.state);
  TEST_ASSERT_EQUAL(NATPMP_RESPONSE_ERROR, request.response.type);
  TEST_ASSERT_EQUAL_UINT16(2, request.response.error.result);
}

void request_tests() {
  RUN_TEST(test_request_matches);
  RUN_TEST(test_request_deletion_matches);
  RUN_TEST(test_request_address_matches);
  RUN_TEST(test_request_answer);
}
