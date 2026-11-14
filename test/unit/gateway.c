#include <unity.h>

#include "gateway.h"
#include "runner.h"

static bool
rebooted(natpmp_gateway_t *gateway, natpmp_time_ms_t now, uint32_t seconds) {
  natpmp_response_t const response = {.type = NATPMP_RESPONSE_MAP,
                                      .epoch = seconds};
  return natpmp_gateway_update(gateway, now, &response).rebooted;
}

static natpmp_gateway_update_result_t
address(natpmp_gateway_t *gateway,
        natpmp_time_ms_t now,
        uint32_t seconds,
        natpmp_ipv4_address_t external_ip) {
  natpmp_response_t const response = {
      .type = NATPMP_RESPONSE_ADDRESS,
      .epoch = seconds,
      .address = {.external_ip = external_ip},
  };
  return natpmp_gateway_update(gateway, now, &response);
}

static natpmp_ipv4_address_t const first = {{203, 0, 113, 7}};
static natpmp_ipv4_address_t const second = {{203, 0, 113, 8}};

static void test_gateway_epoch_first_response() {
  natpmp_gateway_t gateway = {};

  // any epoch is fine without one to compare it to
  TEST_ASSERT_FALSE(rebooted(&gateway, 600'000, 0));
  TEST_ASSERT_TRUE(gateway.epoch.has_value);
  TEST_ASSERT_EQUAL_UINT32(0, gateway.epoch.seconds);
  TEST_ASSERT_EQUAL_UINT64(600'000, gateway.epoch.received);
}

static void test_gateway_epoch_keeps_pace() {
  natpmp_gateway_t gateway = {};
  TEST_ASSERT_FALSE(rebooted(&gateway, 0, 10));
  TEST_ASSERT_FALSE(rebooted(&gateway, 0, 10));
  TEST_ASSERT_FALSE(rebooted(&gateway, 60'000, 70));
  // ahead is fine too
  TEST_ASSERT_FALSE(rebooted(&gateway, 61'000, 1000));
}

static void test_gateway_epoch_tolerance() {
  natpmp_gateway_t gateway = {};
  TEST_ASSERT_FALSE(rebooted(&gateway, 0, 10));

  // 7/8 of 16 s after 10 s, less 2 s
  natpmp_gateway_t copy = gateway;
  TEST_ASSERT_FALSE(rebooted(&copy, 16'000, 22));
  copy = gateway;
  TEST_ASSERT_TRUE(rebooted(&copy, 16'000, 21));

  // and a millisecond later, it's too far behind
  copy = gateway;
  TEST_ASSERT_TRUE(rebooted(&copy, 16'001, 22));
}

static void test_gateway_epoch_from_last_response() {
  natpmp_gateway_t gateway = {};
  TEST_ASSERT_FALSE(rebooted(&gateway, 0, 10));
  TEST_ASSERT_FALSE(rebooted(&gateway, 16'000, 22));

  // 22 s plus 7/8 of the 16 s since, less 2 s
  natpmp_gateway_t copy = gateway;
  TEST_ASSERT_FALSE(rebooted(&copy, 32'000, 34));
  TEST_ASSERT_TRUE(rebooted(&gateway, 32'000, 33));

  // and a reboot becomes the new baseline
  TEST_ASSERT_EQUAL_UINT32(33, gateway.epoch.seconds);
  TEST_ASSERT_EQUAL_UINT64(32'000, gateway.epoch.received);
  TEST_ASSERT_FALSE(rebooted(&gateway, 33'000, 34));
}

static void test_gateway_epoch_large() {
  natpmp_gateway_t gateway = {};
  TEST_ASSERT_FALSE(rebooted(&gateway, 0, UINT32_MAX - 100));
  TEST_ASSERT_FALSE(rebooted(&gateway, 100'000, UINT32_MAX));
  TEST_ASSERT_TRUE(rebooted(&gateway, 200'000, 0));
}

static void test_gateway_external_ip() {
  natpmp_gateway_t gateway = {};

  // the first one is news, a repeat isn't, and a change is again
  TEST_ASSERT_EQUAL(NATPMP_GATEWAY_ADDRESS_NEW,
                    address(&gateway, 0, 10, first).address);
  TEST_ASSERT_EQUAL(NATPMP_GATEWAY_ADDRESS_UNCHANGED,
                    address(&gateway, 1000, 11, first).address);
  TEST_ASSERT_EQUAL(NATPMP_GATEWAY_ADDRESS_NEW,
                    address(&gateway, 2000, 12, second).address);
  TEST_ASSERT_EQUAL(NATPMP_GATEWAY_ADDRESS_NEW,
                    address(&gateway, 3000, 13, first).address);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(first.octets, gateway.external_ip.value.octets,
                                4);
}

static void test_gateway_external_ip_stale() {
  natpmp_gateway_t gateway = {};
  address(&gateway, 0, 10, first);
  address(&gateway, 2000, 12, second);

  // from before the change, delayed past it
  TEST_ASSERT_EQUAL(NATPMP_GATEWAY_ADDRESS_STALE,
                    address(&gateway, 2100, 11, first).address);
  TEST_ASSERT_EQUAL(NATPMP_GATEWAY_ADDRESS_STALE,
                    address(&gateway, 2200, 12, first).address);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(second.octets, gateway.external_ip.value.octets,
                                4);

  // the address that's current is never stale, and a later change counts
  TEST_ASSERT_EQUAL(NATPMP_GATEWAY_ADDRESS_UNCHANGED,
                    address(&gateway, 2300, 11, second).address);
  TEST_ASSERT_EQUAL(NATPMP_GATEWAY_ADDRESS_NEW,
                    address(&gateway, 3000, 13, first).address);
}

static void test_gateway_external_ip_same_second() {
  natpmp_gateway_t gateway = {};
  address(&gateway, 0, 10, first);

  // can't be told apart from a stale one, until a later second
  TEST_ASSERT_EQUAL(NATPMP_GATEWAY_ADDRESS_STALE,
                    address(&gateway, 500, 10, second).address);
  TEST_ASSERT_EQUAL(NATPMP_GATEWAY_ADDRESS_NEW,
                    address(&gateway, 1000, 11, second).address);
}

static void test_gateway_external_ip_after_reboot() {
  natpmp_gateway_t gateway = {};
  address(&gateway, 0, 1000, first);

  // a rebooted gateway's address is never stale
  auto update = address(&gateway, 60'000, 1, second);
  TEST_ASSERT_TRUE(update.rebooted);
  TEST_ASSERT_EQUAL(NATPMP_GATEWAY_ADDRESS_NEW, update.address);

  // and a reboot any response shows starts the comparison over
  gateway = (natpmp_gateway_t){};
  address(&gateway, 0, 1000, first);
  TEST_ASSERT_TRUE(rebooted(&gateway, 60'000, 1));
  TEST_ASSERT_EQUAL(NATPMP_GATEWAY_ADDRESS_NEW,
                    address(&gateway, 64'000, 5, second).address);
}

void gateway_tests() {
  RUN_TEST(test_gateway_epoch_first_response);
  RUN_TEST(test_gateway_epoch_keeps_pace);
  RUN_TEST(test_gateway_epoch_tolerance);
  RUN_TEST(test_gateway_epoch_from_last_response);
  RUN_TEST(test_gateway_epoch_large);
  RUN_TEST(test_gateway_external_ip);
  RUN_TEST(test_gateway_external_ip_stale);
  RUN_TEST(test_gateway_external_ip_same_second);
  RUN_TEST(test_gateway_external_ip_after_reboot);
}
