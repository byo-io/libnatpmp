#include "observer.h"

#include <string.h>
#include <unity.h>

static sim_observed_mapping_t *find_mapping(sim_observer_t *observer,
                                            natpmp_handle_t handle) {
  // the latest one, a handle could come back once its mapping is gone
  for (size_t i = observer->mapping_count; i > 0; --i) {
    if (observer->mappings[i - 1].handle == handle) {
      return &observer->mappings[i - 1];
    }
  }

  TEST_FAIL_MESSAGE("unknown handle");
  return nullptr;
}

static sim_observed_mapping_t *find_open_mapping(sim_observer_t *observer,
                                                 natpmp_handle_t handle) {
  auto const mapping = find_mapping(observer, handle);
  TEST_ASSERT_NOT_EQUAL_MESSAGE(SIM_OBSERVED_FINISHED, mapping->state,
                                "event for a failed or released handle");
  return mapping;
}

static void on_address(sim_observer_t *observer,
                       natpmp_ipv4_address_t external_ip) {
  auto const address = &observer->address;
  if (!address->requested && address->known) {
    TEST_ASSERT_MESSAGE(memcmp(address->external_ip.octets, external_ip.octets,
                               sizeof(external_ip.octets)) != 0,
                        "ADDRESS repeated without being asked for");
  }

  address->requested = false;
  address->known = true;
  address->external_ip = external_ip;
}

void sim_observer_on_address_request(sim_observer_t *observer) {
  observer->address.requested = true;
}

void sim_observer_on_map_request(sim_observer_t *observer,
                                 natpmp_handle_t handle,
                                 natpmp_protocol_t protocol,
                                 natpmp_port_t port) {
  TEST_ASSERT_LESS_THAN_size_t_MESSAGE(SIM_OBSERVER_MAX_MAPPINGS,
                                       observer->mapping_count,
                                       "too many mappings to keep track of");

  observer->mappings[observer->mapping_count++] = (sim_observed_mapping_t){
      .handle = handle,
      .protocol = protocol,
      .port = port,
      .state = SIM_OBSERVED_WAITING,
  };
}

void sim_observer_on_release(sim_observer_t *observer, natpmp_handle_t handle) {
  find_mapping(observer, handle)->state = SIM_OBSERVED_FINISHED;
}

void sim_observer_on_event(sim_observer_t *observer,
                           natpmp_event_t const *event) {
  switch (event->type) {
  case NATPMP_EVENT_ADDRESS:
    on_address(observer, event->address.external_ip);
    break;
  case NATPMP_EVENT_ADDRESS_FAILED:
    TEST_ASSERT_TRUE_MESSAGE(observer->address.requested,
                             "ADDRESS_FAILED without being asked for");
    observer->address.requested = false;
    break;
  case NATPMP_EVENT_MAPPED: {
    auto const mapping = find_open_mapping(observer, event->mapped.handle);
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(0, event->mapped.lifetime,
                                            "MAPPED for no time");
    mapping->state = SIM_OBSERVED_MAPPED;
    mapping->external_port = event->mapped.external_port;
    ++mapping->mapped_count;
    break;
  }
  case NATPMP_EVENT_MAP_FAILED: {
    auto const mapping = find_open_mapping(observer, event->map_failed.handle);
    mapping->state = SIM_OBSERVED_FINISHED;
    mapping->failed = true;
    break;
  }
  case NATPMP_EVENT_SEND:
  case NATPMP_EVENT_TIMEOUT:
    TEST_FAIL_MESSAGE("the client's loop handles sends and timeouts itself");
    break;
  }
}

sim_observed_mapping_t const *sim_observer_find(sim_observer_t const *observer,
                                                natpmp_handle_t handle) {
  // finding doesn't change anything, it only needs the mutable type internally
  return find_mapping((sim_observer_t *)observer, handle);
}
