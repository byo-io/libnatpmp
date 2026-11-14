#pragma once

#include <byo/natpmp/natpmp.h>

// The application side of the library: holds a session, asks for the external
// address and mappings, and runs the event loop any application would. It's
// generic on purpose, and only knows natpmp.h. Where datagrams go and what
// becomes of what it does is up to whoever provides the io, and nothing in here
// is specific to testing.

typedef struct sim_io {
  /// Send a datagram to the gateway, `data` is only valid during the call
  void (*send)(void *ctx, size_t size, uint8_t const data[size]);
  /// Handle anything else the application has to know about
  void (*on_event)(void *ctx, natpmp_event_t const *event);
  /// The application asked for the external address
  void (*on_address_requested)(void *ctx);
  /// The application asked for a mapping, and got `result` for it
  void (*on_map_requested)(void *ctx,
                           natpmp_handle_or_error_t result,
                           natpmp_protocol_t protocol,
                           natpmp_port_t port);
  /// The application released `handle`
  void (*on_released)(void *ctx, natpmp_handle_t handle);
  void *ctx;
} sim_io_t;

typedef struct sim_client {
  natpmp_session_t *session;
  sim_io_t io;
} sim_client_t;

bool sim_client_start(sim_client_t *client, natpmp_rng_t rng, sim_io_t io);

void sim_client_address_request(sim_client_t *client);

/**
 * @brief Ask for a mapping, like the application would
 *
 * @returns What the library returned, which the io hears about too
 */
natpmp_handle_or_error_t sim_client_map_request(sim_client_t *client,
                                                natpmp_time_s_t lifetime,
                                                natpmp_protocol_t protocol,
                                                natpmp_port_t port,
                                                natpmp_port_t external_port);

void sim_client_release(sim_client_t *client, natpmp_handle_t handle);

/**
 * @brief Handle everything that's due at `now`
 *
 * @returns When to run again if no datagram arrives first, nothing if only a
 *          datagram can change anything
 */
natpmp_optional_time_ms_t sim_client_run(sim_client_t *client,
                                         natpmp_time_ms_t now);
