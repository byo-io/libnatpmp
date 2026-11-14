#include "client.h"

bool sim_client_start(sim_client_t *client, natpmp_rng_t rng, sim_io_t io) {
  client->io = io;
  client->session = natpmp_session_begin((natpmp_config_t){.rng = rng});
  return client->session != nullptr;
}

void sim_client_address_request(sim_client_t *client) {
  natpmp_session_address_request(client->session);
  client->io.on_address_requested(client->io.ctx);
}

natpmp_handle_or_error_t sim_client_map_request(sim_client_t *client,
                                                natpmp_time_s_t lifetime,
                                                natpmp_protocol_t protocol,
                                                natpmp_port_t port,
                                                natpmp_port_t external_port) {
  auto const result = natpmp_session_map_request(client->session, lifetime,
                                                 protocol, port, external_port);
  client->io.on_map_requested(client->io.ctx, result, protocol, port);
  return result;
}

void sim_client_release(sim_client_t *client, natpmp_handle_t handle) {
  natpmp_session_release(client->session, handle);
  client->io.on_released(client->io.ctx, handle);
}

natpmp_optional_time_ms_t sim_client_run(sim_client_t *client,
                                         natpmp_time_ms_t now) {
  while (true) {
    auto const event = natpmp_session_poll(client->session, now);
    switch (event.type) {
    case NATPMP_EVENT_TIMEOUT:
      return event.timeout.deadline;
    case NATPMP_EVENT_SEND:
      client->io.send(client->io.ctx, event.send.size, event.send.data);
      break;
    default:
      client->io.on_event(client->io.ctx, &event);
      break;
    }
  }
}
