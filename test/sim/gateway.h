#pragma once

#include <byo/natpmp/natpmp.h>

#include "network.h"

// A NAT-PMP gateway (RFC 6886) for the simulation. It keeps real mappings and
// only knows the client through the bytes it sends. It shares the library's
// generated wire format (natpmp_gen.h), test/unit/packet.c is what pins that to
// the RFC's layout.
//
// A request no real gateway should have to accept fails the test instead of
// getting an error response.
//
// For now it serves a single client, and grants every mapping as asked.

static constexpr size_t SIM_GATEWAY_MAX_MAPPINGS = 16;
static constexpr size_t SIM_GATEWAY_RESPONSE_MAX_SIZE = 16;

typedef struct sim_gateway_mapping {
  bool used;
  natpmp_protocol_t protocol;
  natpmp_port_t internal_port;
  natpmp_port_t external_port;
  sim_time_ms_t expires;
} sim_gateway_mapping_t;

typedef struct sim_gateway_stats {
  /// Of any kind
  size_t requests;
  size_t created;
  size_t renewed;
  size_t deleted;
  /// Mappings that ran out instead of being renewed or deleted
  size_t expired;
} sim_gateway_stats_t;

typedef struct sim_gateway {
  natpmp_ipv4_address_t external_ip;
  /// The state for sim_random, which picks the ports nobody suggested
  uint64_t rng;
  sim_time_ms_t started;
  sim_gateway_mapping_t mappings[SIM_GATEWAY_MAX_MAPPINGS];
  sim_gateway_stats_t stats;
} sim_gateway_t;

/**
 * @brief Boot the gateway with this external address
 *
 * `gateway` has to start out zeroed, with `rng` seeded.
 */
void sim_gateway_start(sim_gateway_t *gateway,
                       natpmp_ipv4_address_t external_ip,
                       sim_time_ms_t now);

typedef struct sim_gateway_reply {
  /// What came of the request
  uint16_t result;
  /// The response to send back
  size_t size;
  uint8_t data[SIM_GATEWAY_RESPONSE_MAX_SIZE];
} sim_gateway_reply_t;

sim_gateway_reply_t sim_gateway_on_datagram(sim_gateway_t *gateway,
                                            sim_time_ms_t now,
                                            size_t size,
                                            uint8_t const data[size]);

/**
 * @returns The mapping that's live at `now`, or nullptr if there is none
 */
sim_gateway_mapping_t const *
sim_gateway_find_mapping(sim_gateway_t *gateway,
                         sim_time_ms_t now,
                         natpmp_protocol_t protocol,
                         natpmp_port_t internal_port);
