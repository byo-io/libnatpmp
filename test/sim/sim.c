#include "sim.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <unity.h>

// more events than this at one point in time means the client is spinning
static constexpr size_t SIM_MAX_EVENTS_PER_RUN = 1024;
static constexpr sim_duration_ms_t SIM_LATENCY = 10;

static bool sim_rng_fill(void *ctx, size_t size, uint8_t data[size]) {
  sim_t *sim = ctx;
  for (size_t i = 0; i < size; ++i) {
    data[i] = (uint8_t)sim_random(&sim->rng_state);
  }
  return true;
}

[[gnu::format(printf, 2, 3)]]
static void sim_trace(sim_t *sim, char const *format, ...) {
  auto const entry = &sim->trace[sim->trace_count++ % SIM_MAX_TRACE];
  entry->at = sim->now;

  va_list args;
  va_start(args, format);
  vsnprintf(entry->text, sizeof(entry->text), format, args);
  va_end(args);
}

static void sim_print_trace(sim_t const *sim) {
  printf("\nseed %" PRIu64 ", timeline in seconds since the start:\n",
         sim->seed);

  auto const count =
      sim->trace_count < SIM_MAX_TRACE ? sim->trace_count : SIM_MAX_TRACE;
  if (count < sim->trace_count) {
    printf("  (%zu older entries dropped)\n", sim->trace_count - count);
  }

  for (size_t i = sim->trace_count - count; i < sim->trace_count; ++i) {
    auto const entry = &sim->trace[i % SIM_MAX_TRACE];
    auto const elapsed = entry->at - sim->started;
    printf("  %8" PRIu64 ".%03" PRIu64 "  %s\n", elapsed / 1000, elapsed % 1000,
           entry->text);
  }
}

static void sim_count_event(sim_t *sim) {
  TEST_ASSERT_LESS_THAN_size_t_MESSAGE(
      SIM_MAX_EVENTS_PER_RUN, sim->events_this_run,
      "the client keeps producing events without time passing");
  ++sim->events_this_run;
}

// the client's io, connecting it to the rest of the world

static void
sim_on_client_send(void *ctx, size_t size, uint8_t const data[size]) {
  sim_t *sim = ctx;
  sim_count_event(sim);
  sim_trace(sim, "client sent %zu bytes", size);
  if (!sim_network_send(&sim->network, sim->now, SIM_ENDPOINT_SERVER, size,
                        data)) {
    sim_trace(sim, "network lost the client's datagram");
  }
}

static void sim_on_client_event(void *ctx, natpmp_event_t const *event) {
  sim_t *sim = ctx;
  sim_count_event(sim);

  switch (event->type) {
  case NATPMP_EVENT_ADDRESS: {
    auto const octets = event->address.external_ip.octets;
    sim_trace(sim, "application got ADDRESS %u.%u.%u.%u", octets[0], octets[1],
              octets[2], octets[3]);
    break;
  }
  case NATPMP_EVENT_ADDRESS_FAILED:
    sim_trace(sim, "application got ADDRESS_FAILED");
    break;
  case NATPMP_EVENT_MAPPED:
    sim_trace(sim,
              "application got MAPPED for handle %u, port %u for %" PRIu32 " s",
              event->mapped.handle, event->mapped.external_port,
              event->mapped.lifetime);
    break;
  case NATPMP_EVENT_MAP_FAILED:
    sim_trace(sim, "application got MAP_FAILED for handle %u",
              event->map_failed.handle);
    break;
  case NATPMP_EVENT_SEND:
  case NATPMP_EVENT_TIMEOUT:
    break;
  }

  sim_observer_on_event(&sim->observer, event);
}

static void sim_on_client_address_requested(void *ctx) {
  sim_t *sim = ctx;
  sim_trace(sim, "application asked for the external address");
  sim_observer_on_address_request(&sim->observer);
}

static void sim_on_client_map_requested(void *ctx,
                                        natpmp_handle_or_error_t result,
                                        natpmp_protocol_t protocol,
                                        natpmp_port_t port) {
  sim_t *sim = ctx;
  TEST_ASSERT_TRUE_MESSAGE(result.has_value, "the map request failed");
  sim_trace(sim, "application asked to map %s port %u, handle %u",
            protocol == NATPMP_PROTOCOL_UDP ? "UDP" : "TCP", port,
            result.value);
  sim_observer_on_map_request(&sim->observer, result.value, protocol, port);
}

static void sim_on_client_released(void *ctx, natpmp_handle_t handle) {
  sim_t *sim = ctx;
  sim_trace(sim, "application released handle %u", handle);
  sim_observer_on_release(&sim->observer, handle);
}

static void sim_deliver_to_gateway(sim_t *sim, sim_datagram_t const *datagram) {
  auto const reply = sim_gateway_on_datagram(&sim->gateway, sim->now,
                                             datagram->size, datagram->data);
  sim_trace(sim, "gateway got %zu bytes, answered with result %u",
            datagram->size, reply.result);
  if (!sim_network_send(&sim->network, sim->now, SIM_ENDPOINT_CLIENT,
                        reply.size, reply.data)) {
    sim_trace(sim, "network lost the gateway's datagram");
  }
}

static void sim_announce_due(sim_t *sim) {
  sim_time_ms_t due = 0;
  if (!sim_gateway_next_announcement(&sim->gateway, &due) || due > sim->now) {
    return;
  }

  auto const announcement = sim_gateway_announce(&sim->gateway, sim->now);
  sim_trace(sim, "gateway announced its address");
  if (!sim_network_send(&sim->network, sim->now, SIM_ENDPOINT_CLIENT,
                        announcement.size, announcement.data)) {
    sim_trace(sim, "network lost the gateway's datagram");
  }
}

static void sim_deliver_due(sim_t *sim) {
  sim_datagram_t datagram;
  while (sim_network_receive(&sim->network, sim->now, &datagram)) {
    switch (datagram.to) {
    case SIM_ENDPOINT_SERVER:
      sim_deliver_to_gateway(sim, &datagram);
      break;
    case SIM_ENDPOINT_CLIENT:
      natpmp_session_on_datagram(sim->client.session, sim->now, datagram.size,
                                 datagram.data);
      break;
    }
  }
}

static void sim_run_client(sim_t *sim) {
  sim->events_this_run = 0;
  sim->deadline = sim_client_run(&sim->client, sim->now);
  if (sim->deadline.has_value) {
    TEST_ASSERT_GREATER_THAN_UINT64_MESSAGE(
        sim->now, sim->deadline.value, "the deadline has to be in the future");
  }
}

// the first moment something can happen, `until` at the latest
static sim_time_ms_t sim_next_time(sim_t const *sim, sim_time_ms_t until) {
  auto next = until;
  if (sim->deadline.has_value && sim->deadline.value < next) {
    next = sim->deadline.value;
  }

  sim_time_ms_t arrival = 0;
  if (sim_network_next_arrival(&sim->network, &arrival) && arrival < next) {
    next = arrival;
  }

  sim_time_ms_t announcement = 0;
  if (sim_gateway_next_announcement(&sim->gateway, &announcement) &&
      announcement < next) {
    next = announcement;
  }
  return next;
}

// A failed assertion jumps back here. This has to be its own function because
// what a function changes in its own locals after calling setjmp is lost after
// the jump, and the world has to survive it.
static bool sim_run_protected(sim_t *sim, sim_scenario_t scenario) {
  if (TEST_PROTECT()) {
    scenario(sim);
    return true;
  }
  return false;
}

void sim_run_scenario(sim_scenario_t scenario, uint64_t seeds) {
  for (uint64_t seed = 1; seed <= seeds; ++seed) {
    sim_t sim = {.seed = seed};
    auto const passed = sim_run_protected(&sim, scenario);
    if (!passed || seed == 1) {
      sim_print_trace(&sim);
    }
    if (sim.client.session) {
      natpmp_session_end(sim.client.session);
    }
    if (!passed) {
      return;
    }
  }
}

void sim_start(sim_t *sim) {
  sim->rng_state = sim->seed;

  // any origin works for the client's clock, see natpmp_time_ms_t
  sim->started = sim_random(&sim->rng_state) >> 24;
  sim->now = sim->started;

  sim_network_start(&sim->network, SIM_LATENCY);
  // its own sequence, so the ports it picks don't change the client's
  // randomness
  sim->gateway.rng = sim->seed ^ 0x67617465776179;
  sim_gateway_start(&sim->gateway, (natpmp_ipv4_address_t){{203, 0, 113, 7}},
                    sim->now);

  TEST_ASSERT_TRUE(sim_client_start(
      &sim->client, (natpmp_rng_t){.fill = sim_rng_fill, .ctx = sim},
      (sim_io_t){
          .send = sim_on_client_send,
          .on_event = sim_on_client_event,
          .on_address_requested = sim_on_client_address_requested,
          .on_map_requested = sim_on_client_map_requested,
          .on_released = sim_on_client_released,
          .ctx = sim,
      }));
}

void sim_run_for(sim_t *sim, sim_duration_ms_t duration) {
  auto const until = sim->now + duration;
  while (true) {
    sim_announce_due(sim);
    sim_deliver_due(sim);
    sim_run_client(sim);
    if (sim->now == until) {
      return;
    }
    sim->now = sim_next_time(sim, until);
  }
}

void sim_restart_gateway(sim_t *sim) {
  sim_trace(sim, "gateway restarted and lost its mappings");
  sim_gateway_restart(&sim->gateway, sim->now);
}

void sim_change_address(sim_t *sim, natpmp_ipv4_address_t external_ip) {
  auto const octets = external_ip.octets;
  sim_trace(sim, "gateway's address changed to %u.%u.%u.%u", octets[0],
            octets[1], octets[2], octets[3]);
  sim_gateway_change_address(&sim->gateway, external_ip, sim->now);
}

void sim_set_network_behavior(sim_t *sim, sim_network_behavior_t behavior) {
  sim_network_set_behavior(&sim->network, behavior);
}

sim_gateway_stats_t sim_stats(sim_t const *sim) { return sim->gateway.stats; }

sim_network_stats_t sim_network_stats(sim_t const *sim) {
  return sim->network.stats;
}

void sim_assert_address(sim_t const *sim) {
  auto const address = &sim->observer.address;
  TEST_ASSERT_TRUE_MESSAGE(address->known,
                           "the application doesn't know the address");
  TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(
      sim->gateway.external_ip.octets, address->external_ip.octets,
      sizeof(address->external_ip.octets),
      "the application and gateway disagree on the address");
}

void sim_assert_mapped(sim_t *sim, natpmp_handle_t handle) {
  auto const observed = sim_observer_find(&sim->observer, handle);
  TEST_ASSERT_EQUAL_MESSAGE(SIM_OBSERVED_MAPPED, observed->state,
                            "the application doesn't think it's mapped");

  auto const mapping = sim_gateway_find_mapping(
      &sim->gateway, sim->now, observed->protocol, observed->port);
  TEST_ASSERT_NOT_NULL_MESSAGE(mapping, "the gateway has no mapping for it");
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(
      mapping->external_port, observed->external_port,
      "the application and gateway disagree on the port");
}

void sim_assert_no_mapping(sim_t *sim, natpmp_handle_t handle) {
  auto const observed = sim_observer_find(&sim->observer, handle);
  TEST_ASSERT_NULL_MESSAGE(sim_gateway_find_mapping(&sim->gateway, sim->now,
                                                    observed->protocol,
                                                    observed->port),
                           "the gateway still has a mapping for it");
}

void sim_assert_failed(sim_t const *sim, natpmp_handle_t handle) {
  TEST_ASSERT_TRUE_MESSAGE(sim_observer_find(&sim->observer, handle)->failed,
                           "the application didn't hear it failed");
}

void sim_assert_idle(sim_t const *sim) {
  TEST_ASSERT_EQUAL_size_t_MESSAGE(0, sim_network_in_flight(&sim->network),
                                   "datagrams in flight");
  TEST_ASSERT_FALSE_MESSAGE(sim->deadline.has_value,
                            "the client still has something scheduled");
}
