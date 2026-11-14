#pragma once

#include "packet.h"
#include <byo/natpmp/natpmp.h>

/**
 * @brief What the gateway told the session about itself
 *
 * Starts zeroed, before any response.
 */
typedef struct natpmp_gateway {
  /// The Seconds Since Start of Epoch from the last response
  struct {
    bool has_value;
    uint32_t seconds;
    /// When that response arrived
    natpmp_time_ms_t received;
  } epoch;
  /// The external address it last gave
  struct {
    bool has_value;
    natpmp_ipv4_address_t value;
    /// The epoch of the response that gave it, or of the reboot since
    uint32_t epoch;
  } external_ip;
} natpmp_gateway_t;

typedef struct natpmp_gateway_update_result {
  /// The gateway lost its mappings since the last response, going by an epoch
  /// more than 2 s behind 7/8 of the time since
  bool rebooted;
  enum {
    /// The response has no address, or the one the session has
    NATPMP_GATEWAY_ADDRESS_UNCHANGED,
    NATPMP_GATEWAY_ADDRESS_NEW,
    /// No newer than the one the session has, from a response that was delayed
    /// or duplicated on the way
    NATPMP_GATEWAY_ADDRESS_STALE,
  } address;
} natpmp_gateway_update_result_t;

/**
 * @brief Record what a response that arrived at `now` says about the gateway
 *
 * A stale response changes nothing but the epoch, and the session drops it.
 */
natpmp_gateway_update_result_t
natpmp_gateway_update(natpmp_gateway_t *gateway,
                      natpmp_time_ms_t now,
                      natpmp_response_t const *response);
