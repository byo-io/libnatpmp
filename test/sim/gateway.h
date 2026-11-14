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
// After a restart or an address change it announces its address, which the
// world sends to the client like any other datagram (see
// sim_gateway_next_announcement).
//
// For now it serves a single client, and grants every mapping as asked.

static constexpr size_t SIM_GATEWAY_MAX_MAPPINGS = 16;
static constexpr size_t SIM_GATEWAY_RESPONSE_MAX_SIZE = 16;
// how many times it announces its address (RFC 6886 section 3.2.1)
static constexpr uint32_t SIM_GATEWAY_ANNOUNCEMENTS = 10;

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
  struct {
    /// Of this restart or address change
    uint32_t left;
    sim_time_ms_t next;
  } announcements;
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

/**
 * @brief Lose every mapping and start a new epoch, then announce it
 */
void sim_gateway_restart(sim_gateway_t *gateway, sim_time_ms_t now);

/**
 * @brief Keep the mappings on a new external address, and announce it
 */
void sim_gateway_change_address(sim_gateway_t *gateway,
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
 * @brief Get when the next announcement is due
 *
 * @returns Whether there's one left to send
 */
bool sim_gateway_next_announcement(sim_gateway_t const *gateway,
                                   sim_time_ms_t *out);

/**
 * @brief Write the announcement that's due, and schedule the next one
 */
sim_gateway_reply_t sim_gateway_announce(sim_gateway_t *gateway,
                                         sim_time_ms_t now);

/**
 * @returns The mapping that's live at `now`, or nullptr if there is none
 */
sim_gateway_mapping_t const *
sim_gateway_find_mapping(sim_gateway_t *gateway,
                         sim_time_ms_t now,
                         natpmp_protocol_t protocol,
                         natpmp_port_t internal_port);
