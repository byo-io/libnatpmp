#include "gateway.h"

#include <assert.h>
#include <string.h>

static bool natpmp_gateway_update_epoch(natpmp_gateway_t *gateway,
                                        natpmp_time_ms_t now,
                                        uint32_t seconds) {
  auto const last = gateway->epoch;
  gateway->epoch.has_value = true;
  gateway->epoch.seconds = seconds;
  gateway->epoch.received = now;
  if (!last.has_value) {
    return false;
  }

  assert(now >= last.received);
  // in eighths of a millisecond
  return (uint64_t)seconds * 8000 + 16000 <
         (uint64_t)last.seconds * 8000 + 7 * (now - last.received);
}

natpmp_gateway_update_result_t
natpmp_gateway_update(natpmp_gateway_t *gateway,
                      natpmp_time_ms_t now,
                      natpmp_response_t const *response) {
  assert(gateway);
  assert(response);

  natpmp_gateway_update_result_t update = {
      .rebooted = natpmp_gateway_update_epoch(gateway, now, response->epoch),
      .address = NATPMP_GATEWAY_ADDRESS_UNCHANGED,
  };

  auto const known = &gateway->external_ip;

  // the epochs start over on reboot, ensure we compared with ones from after
  if (update.rebooted) {
    known->epoch = response->epoch;
  }

  if (response->type != NATPMP_RESPONSE_ADDRESS) {
    return update;
  }

  auto const external_ip = &response->address.external_ip;
  if (known->has_value && memcmp(known->value.octets, external_ip->octets,
                                 sizeof(external_ip->octets)) == 0) {
    return update;
  }

  // an address from a response no newer than the one that gave the current
  // address could be from before it changed. A genuine change from the same
  // second comes again in a later announcement or answer.
  if (known->has_value && !update.rebooted && response->epoch <= known->epoch) {
    update.address = NATPMP_GATEWAY_ADDRESS_STALE;
    return update;
  }

  known->has_value = true;
  known->value = *external_ip;
  known->epoch = response->epoch;
  update.address = NATPMP_GATEWAY_ADDRESS_NEW;

  return update;
}
