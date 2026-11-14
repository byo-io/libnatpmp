#pragma once

#include <stddef.h>
#include <stdint.h>

// The network between the client and the server. It carries opaque datagrams
// and knows nothing about what's in them.

/**
 * @brief A point in time in milliseconds, from the same clock as the client's
 */
typedef uint64_t sim_time_ms_t;

/**
 * @brief A length of time in milliseconds
 */
typedef uint64_t sim_duration_ms_t;

static constexpr size_t SIM_NETWORK_MAX_IN_FLIGHT = 64;
static constexpr size_t SIM_NETWORK_MAX_DATAGRAM_SIZE = 1500;

typedef enum sim_endpoint {
  SIM_ENDPOINT_CLIENT,
  SIM_ENDPOINT_SERVER,
} sim_endpoint_t;

typedef struct sim_datagram {
  sim_time_ms_t deliver_at;
  sim_endpoint_t to;
  size_t size;
  uint8_t data[SIM_NETWORK_MAX_DATAGRAM_SIZE];
} sim_datagram_t;

typedef struct sim_network {
  /// Each way
  sim_duration_ms_t latency;
  size_t in_flight;
  sim_datagram_t datagrams[SIM_NETWORK_MAX_IN_FLIGHT];
} sim_network_t;

/**
 * @brief Get the next number from `state`, a sequence that only depends on the
 *        state it started with and is the same on every platform
 */
uint64_t sim_random(uint64_t *state);

/**
 * @brief Start a network where everything arrives after `latency`, `network`
 *        has to start out zeroed
 */
void sim_network_start(sim_network_t *network, sim_duration_ms_t latency);

void sim_network_send(sim_network_t *network,
                      sim_time_ms_t now,
                      sim_endpoint_t to,
                      size_t size,
                      uint8_t const data[size]);

/**
 * @brief Take the next datagram that has arrived by `now`
 *
 * @returns Whether there was one
 */
bool sim_network_receive(sim_network_t *network,
                         sim_time_ms_t now,
                         sim_datagram_t *out);

/**
 * @brief Get when the next datagram arrives
 *
 * @returns Whether anything is in flight
 */
bool sim_network_next_arrival(sim_network_t const *network, sim_time_ms_t *out);

size_t sim_network_in_flight(sim_network_t const *network);
