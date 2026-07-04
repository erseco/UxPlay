/*
 * Minimal AirPlay reverse-HTTP ("PTTH/1.0") event-channel listener.
 *
 * EXPERIMENTAL (see FDH2/UxPlay#489, #533): when screen mirroring starts, the
 * client is told an eventPort in the SETUP response.  Genuine receivers run a
 * dedicated listener there; the client connects and then *waits for the
 * server to speak first* (server-initiated POST requests, e.g.
 * {"type":"updateInfo"}).  UxPlay historically advertised eventPort=0 and had
 * no listener at all — as does apsdk, the independent implementation used to
 * conclude the video-only ~60s teardown was pure client policy, so the
 * event-channel hypothesis was never actually tested.  This module is that
 * test: accept the connection and immediately send a server-initiated
 * request, shairport-sync style.
 *
 * This file is part of an experiment and intentionally self-contained.
 */

#ifndef RAOP_EVENT_H
#define RAOP_EVENT_H

#include "logger.h"
#include "dnssd.h"

typedef struct raop_event_s raop_event_t;

raop_event_t *raop_event_init(logger_t *logger, dnssd_t *dnssd,
                              int width, int height, int refreshRate, int maxFPS, int overscanned);
/* binds a TCP listener (ephemeral port), spawns the thread, returns the port */
int raop_event_start(raop_event_t *ev, unsigned short *port, int use_ipv6);
void raop_event_destroy(raop_event_t *ev);

#endif
