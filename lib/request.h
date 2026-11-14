#pragma once

#include "packet.h"
#include <byo/natpmp/natpmp.h>

/**
 * @brief A request to the gateway, from when it's queued until its owner takes
 *        what came of it
 *
 * Each mapping owns one, and the session owns the external address request.
 * They move through these states:
 *
 *   IDLE            -> QUEUED    it's pushed
 *   QUEUED          -> SENT      it's at the head of the queue and goes out
 *   QUEUED/SENT     -> ANSWERED  a response for it arrives
 *   QUEUED/SENT     -> FAILED    the head is given up on or the gateway can't
 *                                be reached, which fails everything queued
 *   SENT            -> QUEUED    it changed, and goes out again right away
 *   IDLE/ANSWERED/  -> ANSWERED  the gateway gives a new external address, for
 *   FAILED                       the external address request
 *   ANSWERED/FAILED -> IDLE      its owner takes what came of it
 *
 * A request is in the queue exactly while it's QUEUED or SENT, and only the
 * head is ever SENT. It stays SENT while it's retransmitted.
 */
typedef struct natpmp_request {
  /// The request behind this one in the queue
  struct natpmp_request *next;
  enum {
    NATPMP_REQUEST_IDLE,
    /// Waiting to go out
    NATPMP_REQUEST_QUEUED,
    /// On the wire, waiting for the gateway's answer
    NATPMP_REQUEST_SENT,
    /// `response` came, an error included
    NATPMP_REQUEST_ANSWERED,
    /// No response came
    NATPMP_REQUEST_FAILED,
  } state;
  enum natpmp_opcode opcode;
  natpmp_port_t internal_port;
  natpmp_port_t external_port;
  /// 0 asks for a deletion
  natpmp_time_s_t lifetime;
  /// Once ANSWERED, with `answered_at`
  natpmp_response_t response;
  natpmp_time_ms_t answered_at;
} natpmp_request_t;

bool natpmp_request_matches(natpmp_request_t const *request,
                            natpmp_response_t const *response);

void natpmp_request_answer(natpmp_request_t *request,
                           natpmp_time_ms_t now,
                           natpmp_response_t const *response);
