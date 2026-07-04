/*
 * Minimal AirPlay reverse-HTTP event-channel listener (see raop_event.h).
 * Accepts the client's connection to eventPort and immediately sends a
 * server-initiated  POST /command  request carrying a binary plist
 * {"type":"updateInfo","value":{}} , then keeps the socket open, discarding
 * whatever the client sends back and re-sending updateInfo every 30 s.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

#include <plist/plist.h>

#include "raop_event.h"
#include "compat.h"
#include "netutils.h"
#include "logger.h"
#include "dnssd.h"
#include "dnssdint.h"
#include "global.h"
#include "utils.h"

struct raop_event_s {
    logger_t *logger;
    dnssd_t *dnssd;

    /* display parameters mirrored from raop_t for the updateInfo payload */
    int width;
    int height;
    int refreshRate;
    int maxFPS;
    int overscanned;

    thread_handle_t thread;
    mutex_handle_t run_mutex;
    int running;
    int joined;

    int listen_sock;
    int client_sock;

    /* UXPLAY_EVT_DOWNGRADE experiment: after ~15s of session, re-send
     * updateInfo with the audio feature bits cleared ("hot downgrade") */
    int downgrade;
};

#define UXPLAY_AUDIO_FEATURE_BITS \
    ((((uint64_t) 1) << 9) | (((uint64_t) 1) << 11) | (((uint64_t) 1) << 18) | \
     (((uint64_t) 1) << 19) | (((uint64_t) 1) << 20) | (((uint64_t) 1) << 21))

static int
raop_event_send_update_info(raop_event_t *ev, bool downgraded)
{
    plist_t root = plist_new_dict();
    plist_dict_set_item(root, "type", plist_new_string("updateInfo"));

    /* value: a full GET /info-equivalent dict.  A real AppleTV's updateInfo
     * carries "basically what is returned when requesting /info" including the
     * raw txtAirPlay TXT record and supportedFormats (pyatv protocol docs) —
     * the client appears to REPLACE its cached receiver record with this, so
     * a partial value dict leaves the sender without the keys it gates on
     * (an empty one makes it abort the session setup outright). */
    plist_t value = plist_new_dict();
    uint64_t features = 0;
    if (ev->dnssd) {
        int name_len = 0;
        const char *name = dnssd_get_name(ev->dnssd, &name_len);
        if (name) {
            plist_dict_set_item(value, "name", plist_new_string(name));
        }
        int hw_addr_raw_len = 0;
        const char *hw_addr_raw = dnssd_get_hw_addr(ev->dnssd, &hw_addr_raw_len);
        if (hw_addr_raw && hw_addr_raw_len > 0) {
            char *hw_addr = calloc(1, 3 * hw_addr_raw_len);
            utils_hwaddr_airplay(hw_addr, 3 * hw_addr_raw_len, hw_addr_raw, hw_addr_raw_len);
            plist_dict_set_item(value, "deviceID", plist_new_string(hw_addr));
            plist_dict_set_item(value, "macAddress", plist_new_string(hw_addr));
            free(hw_addr);
        }
        features = dnssd_get_airplay_features(ev->dnssd);
        if (downgraded) {
            features &= ~UXPLAY_AUDIO_FEATURE_BITS;
        }
        plist_dict_set_item(value, "features", plist_new_uint(features));
        if (ev->dnssd->pk) {
            int pk_len = 0;
            char *pk = utils_parse_hex(ev->dnssd->pk, strlen(ev->dnssd->pk), &pk_len);
            if (pk) {
                plist_dict_set_item(value, "pk", plist_new_data(pk, pk_len));
                free(pk);
            }
        }
        if (!downgraded) {
            /* the raw TXT record still carries the original features string, so
             * omit it from downgraded sends rather than contradict "features" */
            int txt_len = 0;
            const char *txt = dnssd_get_airplay_txt(ev->dnssd, &txt_len);
            if (txt && txt_len > 0) {
                plist_dict_set_item(value, "txtAirPlay", plist_new_data(txt, txt_len));
            }
        }
    }

    /* supportedFormats: a real ATV reports a nonzero screenStream here; the
     * audioStream mask follows the advertised audio feature bit */
    bool audio_supported = (features & (((uint64_t) 1) << 9)) != 0;
    plist_t supported_formats = plist_new_dict();
    plist_dict_set_item(supported_formats, "screenStream", plist_new_uint(21235712));
    plist_dict_set_item(supported_formats, "audioStream", plist_new_uint(audio_supported ? 21235712 : 0));
    plist_dict_set_item(supported_formats, "bufferStream", plist_new_uint(0));
    plist_dict_set_item(value, "supportedFormats", supported_formats);

    plist_t displays_node = plist_new_array();
    plist_t display = plist_new_dict();
    plist_dict_set_item(display, "uuid", plist_new_string("e0ff8a27-6738-3d56-8a16-cc53aacee925"));
    plist_dict_set_item(display, "widthPhysical", plist_new_uint(0));
    plist_dict_set_item(display, "heightPhysical", plist_new_uint(0));
    plist_dict_set_item(display, "width", plist_new_uint(ev->width));
    plist_dict_set_item(display, "height", plist_new_uint(ev->height));
    plist_dict_set_item(display, "widthPixels", plist_new_uint(ev->width));
    plist_dict_set_item(display, "heightPixels", plist_new_uint(ev->height));
    plist_dict_set_item(display, "rotation", plist_new_bool(0));
    plist_dict_set_item(display, "refreshRate", plist_new_real((double) 1.0 / (ev->refreshRate > 0 ? ev->refreshRate : 60)));
    plist_dict_set_item(display, "maxFPS", plist_new_uint(ev->maxFPS));
    plist_dict_set_item(display, "overscanned", plist_new_bool(ev->overscanned));
    plist_dict_set_item(display, "features", plist_new_uint(14));
    plist_array_append_item(displays_node, display);
    plist_dict_set_item(value, "displays", displays_node);

    plist_dict_set_item(value, "model", plist_new_string(GLOBAL_MODEL));
    plist_dict_set_item(value, "sourceVersion", plist_new_string(GLOBAL_VERSION));
    plist_dict_set_item(value, "protocolVersion", plist_new_string("1.1"));
    plist_dict_set_item(value, "statusFlags", plist_new_uint(68));
    plist_dict_set_item(value, "keepAliveLowPower", plist_new_uint(1));
    plist_dict_set_item(value, "keepAliveSendStatsAsBody", plist_new_bool(1));
    plist_dict_set_item(value, "pi", plist_new_string(AIRPLAY_PI));
    plist_dict_set_item(value, "vv", plist_new_uint(strtol(AIRPLAY_VV, NULL, 10)));
    plist_dict_set_item(root, "value", value);

    char *body = NULL;
    uint32_t body_len = 0;
    plist_to_bin(root, &body, &body_len);
    plist_free(root);
    if (!body) {
        return -1;
    }

    char header[256];
    int header_len = snprintf(header, sizeof(header),
                              "POST /command RTSP/1.0\r\n"
                              "Content-Type: application/x-apple-binary-plist\r\n"
                              "Content-Length: %u\r\n"
                              "CSeq: 0\r\n"
                              "\r\n", (unsigned int) body_len);

    int ret = 0;
    if (send(ev->client_sock, header, header_len, 0) < 0 ||
        send(ev->client_sock, body, body_len, 0) < 0) {
        int sock_err = SOCKET_GET_ERROR();
        logger_log(ev->logger, LOGGER_WARNING,
                   "raop_event: send failed %d %s", sock_err, SOCKET_ERROR_STRING(sock_err));
        ret = -1;
    } else {
        logger_log(ev->logger, LOGGER_INFO,
                   "raop_event: sent %supdateInfo request on event channel (%u byte plist)",
                   downgraded ? "DOWNGRADED (audio bits cleared) " : "",
                   (unsigned int) body_len);
        if (downgraded) {
            /* signal the SETUP handler that the client has been told we no
             * longer do audio (see UXPLAY_DECLINE_AUDIO_AFTER_DG) */
            setenv("UXPLAY_AUDIO_DOWNGRADED", "1", 1);
        }
    }
    plist_mem_free(body);
    return ret;
}

static THREAD_RETVAL
raop_event_thread(void *arg)
{
    raop_event_t *ev = arg;
    assert(ev);
    unsigned char discard[4096];
    int last_update = 0;
    bool downgraded_sent = false;

    logger_log(ev->logger, LOGGER_DEBUG, "raop_event: thread started, waiting for client");

    while (1) {
        MUTEX_LOCK(ev->run_mutex);
        if (!ev->running) {
            MUTEX_UNLOCK(ev->run_mutex);
            break;
        }
        MUTEX_UNLOCK(ev->run_mutex);

        int nfds;
        fd_set rfds;
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        FD_ZERO(&rfds);
        if (ev->client_sock < 0) {
            FD_SET(ev->listen_sock, &rfds);
            nfds = ev->listen_sock + 1;
        } else {
            FD_SET(ev->client_sock, &rfds);
            nfds = ev->client_sock + 1;
        }

        int ret = select(nfds, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            break;
        }

        if (ev->client_sock < 0) {
            if (ret > 0 && FD_ISSET(ev->listen_sock, &rfds)) {
                struct sockaddr_storage saddr;
                socklen_t saddrlen = sizeof(saddr);
                ev->client_sock = accept(ev->listen_sock, (struct sockaddr *) &saddr, &saddrlen);
                if (ev->client_sock >= 0) {
                    logger_log(ev->logger, LOGGER_INFO,
                               "raop_event: client connected to event channel");
                    raop_event_send_update_info(ev, false);
                    last_update = 0;
                    downgraded_sent = false;
                }
            }
            continue;
        }

        if (ret > 0 && FD_ISSET(ev->client_sock, &rfds)) {
            int len = recv(ev->client_sock, (char *) discard, sizeof(discard), 0);
            if (len <= 0) {
                logger_log(ev->logger, LOGGER_INFO, "raop_event: client closed event channel");
                CLOSESOCKET(ev->client_sock);
                ev->client_sock = -1;
                continue;
            }
            discard[(len < (int) sizeof(discard)) ? len : (int) sizeof(discard) - 1] = 0;
            logger_log(ev->logger, LOGGER_DEBUG,
                       "raop_event: received %d bytes on event channel: %.100s", len, discard);
        }

        /* UXPLAY_EVT_DOWNGRADE: ~15s into the session, tell the client we no
         * longer support audio; otherwise periodic keep-alive every ~30s */
        ++last_update;
        if (ev->downgrade && !downgraded_sent && last_update >= 15) {
            raop_event_send_update_info(ev, true);
            downgraded_sent = true;
            last_update = 0;
        } else if (last_update >= 30) {
            raop_event_send_update_info(ev, ev->downgrade && downgraded_sent);
            last_update = 0;
        }
    }

    logger_log(ev->logger, LOGGER_DEBUG, "raop_event: thread exiting");
    return 0;
}

raop_event_t *
raop_event_init(logger_t *logger, dnssd_t *dnssd,
                int width, int height, int refreshRate, int maxFPS, int overscanned)
{
    raop_event_t *ev = calloc(1, sizeof(raop_event_t));
    if (!ev) {
        return NULL;
    }
    ev->logger = logger;
    ev->dnssd = dnssd;
    ev->width = width;
    ev->height = height;
    ev->refreshRate = refreshRate;
    ev->maxFPS = maxFPS;
    ev->overscanned = overscanned;
    ev->downgrade = (getenv("UXPLAY_EVT_DOWNGRADE") != NULL);
    if (ev->downgrade) {
        logger_log(logger, LOGGER_INFO,
                   "raop_event: UXPLAY_EVT_DOWNGRADE active — will send audio-bits-cleared updateInfo ~15s in");
    }
    ev->listen_sock = -1;
    ev->client_sock = -1;
    ev->running = 0;
    ev->joined = 1;
    MUTEX_CREATE(ev->run_mutex);
    return ev;
}

int
raop_event_start(raop_event_t *ev, unsigned short *port, int use_ipv6)
{
    assert(ev);
    assert(port);

    MUTEX_LOCK(ev->run_mutex);
    if (ev->running) {
        MUTEX_UNLOCK(ev->run_mutex);
        return 0;
    }

    *port = 0;
    ev->listen_sock = netutils_init_socket(port, use_ipv6, 0);
    if (ev->listen_sock < 0 || listen(ev->listen_sock, 1) < 0) {
        logger_log(ev->logger, LOGGER_WARNING, "raop_event: could not bind/listen event port");
        if (ev->listen_sock >= 0) {
            CLOSESOCKET(ev->listen_sock);
            ev->listen_sock = -1;
        }
        MUTEX_UNLOCK(ev->run_mutex);
        return -1;
    }

    unsetenv("UXPLAY_AUDIO_DOWNGRADED"); /* fresh session: downgrade not yet sent */
    logger_log(ev->logger, LOGGER_INFO, "raop_event: event channel listening on port %u", *port);

    ev->running = 1;
    ev->joined = 0;
    MUTEX_UNLOCK(ev->run_mutex);

    THREAD_CREATE(ev->thread, raop_event_thread, ev);
    if (!ev->thread) {
        MUTEX_LOCK(ev->run_mutex);
        ev->running = 0;
        ev->joined = 1;
        MUTEX_UNLOCK(ev->run_mutex);
        CLOSESOCKET(ev->listen_sock);
        ev->listen_sock = -1;
        return -1;
    }
    return 0;
}

void
raop_event_destroy(raop_event_t *ev)
{
    if (!ev) {
        return;
    }
    MUTEX_LOCK(ev->run_mutex);
    int was_running = ev->running;
    ev->running = 0;
    MUTEX_UNLOCK(ev->run_mutex);

    if (was_running && !ev->joined) {
        THREAD_JOIN(ev->thread);
        ev->joined = 1;
    }
    if (ev->client_sock >= 0) {
        CLOSESOCKET(ev->client_sock);
        ev->client_sock = -1;
    }
    if (ev->listen_sock >= 0) {
        CLOSESOCKET(ev->listen_sock);
        ev->listen_sock = -1;
    }
    MUTEX_DESTROY(ev->run_mutex);
    free(ev);
}
