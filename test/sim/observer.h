#pragma once

#include <byo/natpmp/natpmp.h>

// Watches what the application is told and fails the test as soon as that
// breaks a promise from natpmp.h. What it keeps is what the application would
// believe, for scenarios to compare against the gateway.

static constexpr size_t SIM_OBSERVER_MAX_MAPPINGS = 16;

typedef struct sim_observed_mapping {
  natpmp_handle_t handle;
  natpmp_protocol_t protocol;
  natpmp_port_t port;
  enum {
    SIM_OBSERVED_WAITING,
    SIM_OBSERVED_MAPPED,
    /// Failed or released, no more events allowed
    SIM_OBSERVED_FINISHED,
  } state;
  /// From the last MAPPED
  natpmp_port_t external_port;
  size_t mapped_count;
} sim_observed_mapping_t;

typedef struct sim_observer {
  struct {
    /// Asked for, and not answered yet
    bool requested;
    /// From the last ADDRESS
    bool known;
    natpmp_ipv4_address_t external_ip;
  } address;
  size_t mapping_count;
  sim_observed_mapping_t mappings[SIM_OBSERVER_MAX_MAPPINGS];
} sim_observer_t;

void sim_observer_on_address_request(sim_observer_t *observer);

void sim_observer_on_map_request(sim_observer_t *observer,
                                 natpmp_handle_t handle,
                                 natpmp_protocol_t protocol,
                                 natpmp_port_t port);

void sim_observer_on_release(sim_observer_t *observer, natpmp_handle_t handle);

/**
 * @brief The application got this event, which can't be a send or a timeout
 */
void sim_observer_on_event(sim_observer_t *observer,
                           natpmp_event_t const *event);

/**
 * @returns What's known about `handle`, failing the test if it was never handed
 *          out
 */
sim_observed_mapping_t const *sim_observer_find(sim_observer_t const *observer,
                                                natpmp_handle_t handle);
