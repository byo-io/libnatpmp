#pragma once

#include <byo/natpmp/natpmp.h>

#include "client.h"
#include "gateway.h"
#include "network.h"

// A simulated world for testing the client end to end. This is what scenarios
// use, and it owns virtual time and connects the pieces:
//
// - client.h, a generic application loop around the library's public API
// - network.h, what carries datagrams between the client and the gateway
// - gateway.h, a NAT-PMP gateway with real mappings
//
// A world only lives on the stack of sim_run_scenario, which also reports it
// and ends its session.

static constexpr sim_duration_ms_t SIM_SECOND = 1000;
static constexpr sim_duration_ms_t SIM_MINUTE = 60 * SIM_SECOND;
static constexpr sim_duration_ms_t SIM_HOUR = 60 * SIM_MINUTE;

static constexpr size_t SIM_MAX_TRACE = 1024;
static constexpr size_t SIM_MAX_TRACE_TEXT = 80;

/**
 * @brief One line of a scenario's timeline
 */
typedef struct sim_trace_entry {
  sim_time_ms_t at;
  char text[SIM_MAX_TRACE_TEXT];
} sim_trace_entry_t;

typedef struct sim {
  /// Where the client's randomness and the clock's starting point come from
  uint64_t seed;
  uint64_t rng_state;
  sim_time_ms_t started;
  sim_time_ms_t now;

  sim_client_t client;
  sim_gateway_t gateway;
  sim_network_t network;

  /// From the client's last run
  natpmp_optional_time_ms_t deadline;
  size_t events_this_run;

  size_t trace_count;
  sim_trace_entry_t trace[SIM_MAX_TRACE];
} sim_t;

typedef void (*sim_scenario_t)(sim_t *sim);

/**
 * @brief Run a scenario once per seed, from 1 to `seeds`, each in a new world
 *
 * Prints the timeline of the first seed, and of a seed that fails, which is
 * also where it stops.
 */
void sim_run_scenario(sim_scenario_t scenario, uint64_t seeds);

void sim_start(sim_t *sim);

void sim_run_for(sim_t *sim, sim_duration_ms_t duration);

sim_gateway_stats_t sim_stats(sim_t const *sim);

/**
 * @brief Check nothing is in flight and the client has nothing scheduled
 */
void sim_assert_idle(sim_t const *sim);
