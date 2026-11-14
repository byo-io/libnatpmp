#include "gateway.h"

#include <string.h>
#include <unity.h>

#include "natpmp_gen.h"

// the dynamic range (RFC 6335 section 6), which external ports come from
static constexpr natpmp_port_t FIRST_DYNAMIC_PORT = 49152;
static constexpr uint32_t DYNAMIC_PORTS = 16384;
// set in the opcode of every response
static constexpr uint8_t RESPONSE_BIT = 128;

static_assert(SIM_GATEWAY_RESPONSE_MAX_SIZE ==
              NATPMP_RESPONSE_HEADER_MIN_SIZE + NATPMP_MAP_RESPONSE_MAX_SIZE);

static natpmp_protocol_t protocol_of(enum natpmp_opcode opcode) {
  return opcode == NATPMP_OPCODE_MAP_UDP ? NATPMP_PROTOCOL_UDP
                                         : NATPMP_PROTOCOL_TCP;
}

static void expire_mappings(sim_gateway_t *gateway, sim_time_ms_t now) {
  for (size_t i = 0; i < SIM_GATEWAY_MAX_MAPPINGS; ++i) {
    auto const mapping = &gateway->mappings[i];
    if (mapping->used && mapping->expires <= now) {
      mapping->used = false;
      ++gateway->stats.expired;
    }
  }
}

static sim_gateway_mapping_t *find_mapping(sim_gateway_t *gateway,
                                           natpmp_protocol_t protocol,
                                           natpmp_port_t internal_port) {
  for (size_t i = 0; i < SIM_GATEWAY_MAX_MAPPINGS; ++i) {
    auto const mapping = &gateway->mappings[i];
    if (mapping->used && mapping->protocol == protocol &&
        mapping->internal_port == internal_port) {
      return mapping;
    }
  }
  return nullptr;
}

static bool port_is_free(sim_gateway_t const *gateway,
                         natpmp_protocol_t protocol,
                         natpmp_port_t port) {
  if (port == 0) {
    return false;
  }

  for (size_t i = 0; i < SIM_GATEWAY_MAX_MAPPINGS; ++i) {
    auto const mapping = &gateway->mappings[i];
    if (mapping->used && mapping->protocol == protocol &&
        mapping->external_port == port) {
      return false;
    }
  }
  return true;
}

// takes the suggested port if it can and a random one otherwise
static natpmp_port_t allocate_port(sim_gateway_t *gateway,
                                   natpmp_protocol_t protocol,
                                   natpmp_port_t suggested) {
  if (port_is_free(gateway, protocol, suggested)) {
    return suggested;
  }

  while (true) {
    auto const port =
        (natpmp_port_t)(FIRST_DYNAMIC_PORT +
                        sim_random(&gateway->rng) % DYNAMIC_PORTS);
    if (port_is_free(gateway, protocol, port)) {
      return port;
    }
  }
}

static sim_gateway_mapping_t *create_mapping(sim_gateway_t *gateway,
                                             natpmp_protocol_t protocol,
                                             natpmp_port_t internal_port,
                                             natpmp_port_t suggested) {
  for (size_t i = 0; i < SIM_GATEWAY_MAX_MAPPINGS; ++i) {
    auto const mapping = &gateway->mappings[i];
    if (mapping->used) {
      continue;
    }

    *mapping = (sim_gateway_mapping_t){
        .used = true,
        .protocol = protocol,
        .internal_port = internal_port,
        .external_port = allocate_port(gateway, protocol, suggested),
    };
    return mapping;
  }

  TEST_FAIL_MESSAGE("the simulated gateway ran out of mappings");
  return nullptr;
}

// updates the mappings for `request`, and `body` with what the response says
static void apply_map_request(sim_gateway_t *gateway,
                              sim_time_ms_t now,
                              struct natpmp_map_request const *request,
                              struct natpmp_map_response *body) {
  auto const protocol = protocol_of(request->opcode);
  auto mapping = find_mapping(gateway, protocol, request->internal_port);

  // deleting a mapping that doesn't exist succeeds too (section 3.4)
  if (request->lifetime == 0) {
    if (mapping) {
      mapping->used = false;
      ++gateway->stats.deleted;
    }
    return;
  }

  if (mapping) {
    ++gateway->stats.renewed;
  } else {
    mapping = create_mapping(gateway, protocol, request->internal_port,
                             request->external_port);
    ++gateway->stats.created;
  }

  mapping->expires = now + (sim_time_ms_t)request->lifetime * 1000;
  body->external_port = mapping->external_port;
  body->lifetime = request->lifetime;
}

static uint32_t epoch_of(sim_gateway_t const *gateway, sim_time_ms_t now) {
  return (uint32_t)((now - gateway->started) / 1000);
}

// the encoders take the header and the body separately, see natpmp.pkt
static size_t write_response(sim_gateway_t const *gateway,
                             sim_time_ms_t now,
                             enum natpmp_opcode opcode,
                             size_t body_size,
                             uint8_t const body[body_size],
                             uint8_t data[SIM_GATEWAY_RESPONSE_MAX_SIZE]) {
  struct natpmp_response_header const header = {
      .r = 1,
      .opcode = opcode,
      .result = NATPMP_RESULT_SUCCESS,
      .epoch = epoch_of(gateway, now),
      .body = {.ptr = body, .len = body_size},
  };
  size_t size = 0;
  TEST_ASSERT_EQUAL(NATPMP_OK,
                    natpmp_response_header_encode(
                        &header, data, SIM_GATEWAY_RESPONSE_MAX_SIZE, &size));
  return size;
}

static sim_gateway_reply_t on_address_request(sim_gateway_t *gateway,
                                              sim_time_ms_t now,
                                              size_t size,
                                              uint8_t const data[size]) {
  struct natpmp_address_request request;
  TEST_ASSERT_EQUAL_MESSAGE(NATPMP_OK,
                            natpmp_address_request_decode(&request, data, size),
                            "malformed external address request");

  struct natpmp_address_response response = {};
  memcpy(response.external_ip, gateway->external_ip.octets,
         sizeof(response.external_ip));
  uint8_t body[NATPMP_ADDRESS_RESPONSE_MAX_SIZE];
  size_t body_size = 0;
  TEST_ASSERT_EQUAL(NATPMP_OK, natpmp_address_response_encode(
                                   &response, body, sizeof(body), &body_size));

  sim_gateway_reply_t reply = {.result = NATPMP_RESULT_SUCCESS};
  reply.size =
      write_response(gateway, now, request.opcode, body_size, body, reply.data);
  return reply;
}

static sim_gateway_reply_t on_map_request(sim_gateway_t *gateway,
                                          sim_time_ms_t now,
                                          size_t size,
                                          uint8_t const data[size]) {
  struct natpmp_map_request request;
  TEST_ASSERT_EQUAL_MESSAGE(NATPMP_OK,
                            natpmp_map_request_decode(&request, data, size),
                            "malformed mapping request");
  TEST_ASSERT_NOT_EQUAL_MESSAGE(
      0, request.internal_port,
      "deleting all mappings at once isn't simulated");
  if (request.lifetime == 0) {
    TEST_ASSERT_EQUAL_MESSAGE(0, request.external_port,
                              "a deletion has to suggest no external port");
  }

  // a deletion answers with no port and no time
  struct natpmp_map_response response = {.internal_port =
                                             request.internal_port};
  apply_map_request(gateway, now, &request, &response);
  uint8_t body[NATPMP_MAP_RESPONSE_MAX_SIZE];
  size_t body_size = 0;
  TEST_ASSERT_EQUAL(NATPMP_OK, natpmp_map_response_encode(
                                   &response, body, sizeof(body), &body_size));

  sim_gateway_reply_t reply = {.result = NATPMP_RESULT_SUCCESS};
  reply.size =
      write_response(gateway, now, request.opcode, body_size, body, reply.data);
  return reply;
}

void sim_gateway_start(sim_gateway_t *gateway,
                       natpmp_ipv4_address_t external_ip,
                       sim_time_ms_t now) {
  memset(gateway->mappings, 0, sizeof(gateway->mappings));
  gateway->external_ip = external_ip;
  gateway->started = now;
}

sim_gateway_reply_t sim_gateway_on_datagram(sim_gateway_t *gateway,
                                            sim_time_ms_t now,
                                            size_t size,
                                            uint8_t const data[size]) {
  TEST_ASSERT_GREATER_OR_EQUAL_size_t_MESSAGE(2, size,
                                              "a request without an opcode");
  TEST_ASSERT_LESS_THAN_UINT8_MESSAGE(RESPONSE_BIT, data[1],
                                      "a response sent to the gateway");

  expire_mappings(gateway, now);
  ++gateway->stats.requests;

  return data[1] == NATPMP_OPCODE_EXTERNAL_ADDRESS
             ? on_address_request(gateway, now, size, data)
             : on_map_request(gateway, now, size, data);
}

sim_gateway_mapping_t const *
sim_gateway_find_mapping(sim_gateway_t *gateway,
                         sim_time_ms_t now,
                         natpmp_protocol_t protocol,
                         natpmp_port_t internal_port) {
  expire_mappings(gateway, now);
  return find_mapping(gateway, protocol, internal_port);
}
