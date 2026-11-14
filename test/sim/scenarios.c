#include <unity.h>

#include "runner.h"
#include "sim.h"

// Scenarios for the client in a simulated world (see sim.h). Each one is a
// short script: set up the world, act like an application, let time pass, and
// check what the application and the gateway ended up with.
//
//   SCENARIO(test_sim_maps_a_port) {
//     sim_start(sim);
//     auto const handle = map(sim);
//     sim_run_for(sim, SIM_SECOND);
//     sim_assert_mapped(sim, handle);
//   }
//
// and a RUN_TEST for it at the bottom of this file. It runs once for each of
// SEEDS seeds, and prints the timeline of the first seed and of any that fail.
// To watch one, run e.g. ./build/test/sim/sim_tests test_sim_full_lifecycle
//
// What a scenario has to work with:
//
// - The world: sim_start, and sim_run_for to let time pass.
// - The network: sim_set_network_behavior, with SIM_RANDOM_FAULTS for loss,
//   duplicates and reordering, or SIM_NETWORK_DOWN and SIM_NETWORK_UP for an
//   outage.
// - The gateway: sim_restart_gateway and sim_change_address, which it
//   announces.
// - The application: sim_client_address_request, map, or
//   sim_client_map_request for the library's whole result, and
//   sim_client_release.
// - Checks: sim_assert_address, sim_assert_mapped, sim_assert_no_mapping,
//   sim_assert_failed, sim_assert_idle, what the application heard through
//   sim_observer_find, and the counts in sim_stats and sim_network_stats.
//
// Use sim->seed for anything random, so that every seed is different.

// the client's randomness and the gateway's ports come from the seed, so go
// through a few
static constexpr uint64_t SEEDS = 16;

static constexpr natpmp_time_s_t LIFETIME = 3600;
static constexpr natpmp_port_t PORT = 1234;

// renewing starts halfway through a lifetime (RFC 6886 section 3.3), so this is
// before the first renewal is due
static constexpr sim_duration_ms_t BEFORE_RENEWAL = 25 * SIM_MINUTE;

// nine attempts 250 ms apart and doubling, then 64 s more for the last answer
// (section 3.1), which is when the client gives up
static constexpr sim_duration_ms_t GIVE_UP = 127'750;

// the client renews within 5 s of seeing the gateway restart (section 3.7),
// plus a round trip
static constexpr sim_duration_ms_t RECOVERY = 6 * SIM_SECOND;

static constexpr natpmp_ipv4_address_t NEW_ADDRESS = {{198, 51, 100, 1}};

// a scenario that runs once per seed, in a new world each time
#define SCENARIO(name)                                                         \
  static void name##_scenario(sim_t *sim);                                     \
  static void name() { sim_run_scenario(name##_scenario, SEEDS); }             \
  static void name##_scenario(sim_t *sim)

// asks for a mapping like the application would. A request that fails has
// already failed the scenario.
static natpmp_handle_t map(sim_t *sim) {
  return sim_client_map_request(&sim->client, LIFETIME, NATPMP_PROTOCOL_TCP,
                                PORT, PORT)
      .value;
}

static sim_observed_mapping_t const *observed(sim_t const *sim,
                                              natpmp_handle_t handle) {
  return sim_observer_find(&sim->observer, handle);
}

SCENARIO(test_sim_full_lifecycle) {
  sim_start(sim);
  sim_client_address_request(&sim->client);
  auto const handle = map(sim);
  sim_run_for(sim, SIM_SECOND);
  sim_assert_address(sim);
  sim_assert_mapped(sim, handle);
  // the port it asked for, which nobody else has
  TEST_ASSERT_EQUAL_UINT16(PORT, observed(sim, handle)->external_port);

  // the same mapping all day
  sim_run_for(sim, 24 * SIM_HOUR);
  sim_assert_mapped(sim, handle);
  TEST_ASSERT_EQUAL_UINT16(PORT, observed(sim, handle)->external_port);

  // renewing halfway through every lifetime (RFC 6886 section 3.3), each a
  // round trip later than the last, and nothing is lost so every request is
  // answered the first time. The application hears about every grant.
  auto stats = sim_stats(sim);
  TEST_ASSERT_EQUAL_size_t(1, stats.created);
  TEST_ASSERT_EQUAL_size_t(0, stats.expired);
  TEST_ASSERT_GREATER_OR_EQUAL_size_t(24 * 3600 / (LIFETIME / 2) - 1,
                                      stats.renewed);
  TEST_ASSERT_LESS_OR_EQUAL_size_t(24 * 3600 / (LIFETIME / 2), stats.renewed);
  TEST_ASSERT_EQUAL_size_t(2 + stats.renewed, stats.requests);
  TEST_ASSERT_EQUAL_size_t(1 + stats.renewed,
                           observed(sim, handle)->mapped_count);

  sim_client_release(&sim->client, handle);
  sim_run_for(sim, SIM_SECOND);
  sim_assert_no_mapping(sim, handle);
  sim_assert_idle(sim);

  stats = sim_stats(sim);
  TEST_ASSERT_EQUAL_size_t(1, stats.deleted);
  TEST_ASSERT_EQUAL_size_t(0, stats.expired);
  TEST_ASSERT_EQUAL_size_t(3 + stats.renewed, stats.requests);
}

// a mapping the gateway has granted, with some time left until its renewal
static natpmp_handle_t start_mapped(sim_t *sim) {
  sim_start(sim);
  auto const handle = map(sim);
  sim_run_for(sim, SIM_SECOND);
  sim_assert_mapped(sim, handle);
  return handle;
}

SCENARIO(test_sim_heavy_loss) {
  sim_start(sim);
  sim_set_network_behavior(sim,
                           SIM_RANDOM_FAULTS(sim->seed, .loss_percent = 30));
  sim_client_address_request(&sim->client);
  auto const handle = map(sim);

  sim_run_for(sim, 24 * SIM_HOUR);
  TEST_ASSERT_GREATER_THAN_size_t(0, sim_network_stats(sim).lost);
  sim_assert_address(sim);
  sim_assert_mapped(sim, handle);
}

SCENARIO(test_sim_duplicates_and_reordering) {
  sim_start(sim);
  sim_set_network_behavior(
      sim,
      SIM_RANDOM_FAULTS(sim->seed, .duplicate_percent = 20, .jitter = 500));
  sim_client_address_request(&sim->client);
  auto const handle = map(sim);

  sim_run_for(sim, 24 * SIM_HOUR);
  TEST_ASSERT_GREATER_THAN_size_t(0, sim_network_stats(sim).duplicated);
  sim_assert_address(sim);
  sim_assert_mapped(sim, handle);
  TEST_ASSERT_EQUAL_UINT16(PORT, observed(sim, handle)->external_port);
  TEST_ASSERT_EQUAL_size_t(1, sim_stats(sim).created);
  TEST_ASSERT_EQUAL_size_t(0, sim_stats(sim).expired);
}

SCENARIO(test_sim_outage_before_mapping) {
  sim_start(sim);
  sim_set_network_behavior(sim, SIM_NETWORK_DOWN);
  auto const handle = map(sim);

  sim_run_for(sim, GIVE_UP + SIM_SECOND);
  sim_assert_failed(sim, handle);
  TEST_ASSERT_EQUAL_size_t(9, sim_network_stats(sim).lost);

  sim_set_network_behavior(sim, SIM_NETWORK_UP);
  auto const again = map(sim);
  sim_run_for(sim, SIM_SECOND);
  sim_assert_mapped(sim, again);
}

SCENARIO(test_sim_outage_shorter_than_retransmissions) {
  auto const handle = start_mapped(sim);

  sim_run_for(sim, BEFORE_RENEWAL + 4 * SIM_MINUTE);
  sim_set_network_behavior(sim, SIM_NETWORK_DOWN);
  sim_run_for(sim, SIM_MINUTE);
  sim_set_network_behavior(sim, SIM_NETWORK_UP);
  TEST_ASSERT_GREATER_THAN_size_t(0, sim_network_stats(sim).lost);

  sim_run_for(sim, SIM_HOUR);
  sim_assert_mapped(sim, handle);
  TEST_ASSERT_EQUAL_size_t(0, sim_stats(sim).expired);
}

SCENARIO(test_sim_outage_longer_than_retransmissions) {
  auto const handle = start_mapped(sim);

  // the renewal goes unanswered, but the mapping is still there, so the client
  // tries again later
  sim_run_for(sim, BEFORE_RENEWAL);
  sim_set_network_behavior(sim, SIM_NETWORK_DOWN);
  sim_run_for(sim, 10 * SIM_MINUTE);
  sim_set_network_behavior(sim, SIM_NETWORK_UP);
  TEST_ASSERT_EQUAL_size_t(9, sim_network_stats(sim).lost);

  sim_run_for(sim, SIM_HOUR);
  sim_assert_mapped(sim, handle);
  TEST_ASSERT_EQUAL_size_t(1, sim_stats(sim).created);
  TEST_ASSERT_EQUAL_size_t(0, sim_stats(sim).expired);
}

SCENARIO(test_sim_outage_longer_than_lifetime) {
  auto const handle = start_mapped(sim);

  sim_run_for(sim, BEFORE_RENEWAL);
  sim_set_network_behavior(sim, SIM_NETWORK_DOWN);
  sim_run_for(sim, SIM_HOUR);
  sim_set_network_behavior(sim, SIM_NETWORK_UP);

  sim_assert_failed(sim, handle);
  sim_assert_no_mapping(sim, handle);
  TEST_ASSERT_EQUAL_size_t(1, sim_stats(sim).expired);
}

SCENARIO(test_sim_gateway_restart) {
  auto const handle = start_mapped(sim);
  sim_run_for(sim, 10 * SIM_MINUTE);
  sim_restart_gateway(sim);
  sim_assert_no_mapping(sim, handle);

  // the first announcement shows the new epoch, and the client maps it again
  sim_run_for(sim, RECOVERY);
  sim_assert_mapped(sim, handle);
  TEST_ASSERT_EQUAL_UINT16(PORT, observed(sim, handle)->external_port);
  TEST_ASSERT_EQUAL_size_t(2, sim_stats(sim).created);
  TEST_ASSERT_EQUAL_size_t(2, observed(sim, handle)->mapped_count);

  // and only once, the other announcements are from the same epoch
  sim_run_for(sim, 5 * SIM_MINUTE);
  TEST_ASSERT_EQUAL_size_t(2, sim_stats(sim).created);
  TEST_ASSERT_EQUAL_size_t(0, sim_stats(sim).renewed);
}

// everything the gateway sends to the client unasked, as long as the client
// doesn't ask for the address either
static sim_fate_t lose_address_responses(void * /*ctx*/,
                                         sim_endpoint_t to,
                                         size_t size,
                                         uint8_t const data[]) {
  return (sim_fate_t){
      .lost = to == SIM_ENDPOINT_CLIENT && size >= 2 && data[1] == 128,
  };
}

SCENARIO(test_sim_gateway_restart_unannounced) {
  auto const handle = start_mapped(sim);
  sim_set_network_behavior(
      sim, (sim_network_behavior_t){.on_send = lose_address_responses});
  sim_run_for(sim, 10 * SIM_MINUTE);
  sim_restart_gateway(sim);

  // the client can't know
  sim_run_for(sim, 10 * SIM_MINUTE);
  TEST_ASSERT_EQUAL_size_t(SIM_GATEWAY_ANNOUNCEMENTS,
                           sim_network_stats(sim).lost);
  sim_assert_no_mapping(sim, handle);

  // until it renews, which maps it again
  sim_run_for(sim, 15 * SIM_MINUTE);
  sim_assert_mapped(sim, handle);
  TEST_ASSERT_EQUAL_size_t(2, sim_stats(sim).created);
}

SCENARIO(test_sim_gateway_restart_during_outage) {
  auto const handle = start_mapped(sim);
  sim_run_for(sim, 10 * SIM_MINUTE);
  sim_set_network_behavior(sim, SIM_NETWORK_DOWN);
  sim_restart_gateway(sim);
  sim_run_for(sim, 10 * SIM_SECOND);
  sim_set_network_behavior(sim, SIM_NETWORK_UP);

  // the announcements keep coming after the first few are lost
  sim_run_for(sim, 10 * SIM_SECOND + RECOVERY);
  TEST_ASSERT_EQUAL_size_t(6, sim_network_stats(sim).lost);
  sim_assert_mapped(sim, handle);
  TEST_ASSERT_EQUAL_size_t(2, sim_stats(sim).created);
}

SCENARIO(test_sim_address_change) {
  sim_start(sim);
  sim_client_address_request(&sim->client);
  auto const handle = map(sim);
  sim_run_for(sim, 10 * SIM_MINUTE);

  // heard once, the observer fails a repeat of any of the announcements
  sim_change_address(sim, NEW_ADDRESS);
  sim_run_for(sim, 5 * SIM_MINUTE);
  sim_assert_address(sim);
  sim_assert_mapped(sim, handle);
  TEST_ASSERT_EQUAL_size_t(1, sim_stats(sim).created);
}

SCENARIO(test_sim_address_change_while_asking) {
  sim_start(sim);
  sim_set_network_behavior(
      sim,
      SIM_RANDOM_FAULTS(sim->seed, .duplicate_percent = 20, .jitter = 500));
  sim_client_address_request(&sim->client);
  sim_run_for(sim, SIM_SECOND);

  // asking as the address changes, with duplicates of the answers and
  // announcements
  sim_client_address_request(&sim->client);
  sim_change_address(sim, NEW_ADDRESS);
  sim_run_for(sim, 5 * SIM_MINUTE);
  sim_assert_address(sim);
}

void scenario_tests() {
  RUN_TEST(test_sim_full_lifecycle);
  RUN_TEST(test_sim_heavy_loss);
  RUN_TEST(test_sim_duplicates_and_reordering);
  RUN_TEST(test_sim_outage_before_mapping);
  RUN_TEST(test_sim_outage_shorter_than_retransmissions);
  RUN_TEST(test_sim_outage_longer_than_retransmissions);
  RUN_TEST(test_sim_outage_longer_than_lifetime);
  RUN_TEST(test_sim_gateway_restart);
  RUN_TEST(test_sim_gateway_restart_unannounced);
  RUN_TEST(test_sim_gateway_restart_during_outage);
  RUN_TEST(test_sim_address_change);
  RUN_TEST(test_sim_address_change_while_asking);
}
