/*
 * Copyright (C) 2026 The PenguinOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Runs a GNSS session on the modem's QMI LOC service and prints NMEA sentences and
 * position reports on stdout until SIGTERM.
 */

#include <glib-unix.h>
#include <libqmi-glib.h>
#include <libqrtr-glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define QMI_LOC_REG_EVENTS 0x0021
#define LOC_EVENT_MASK_POSITION_REPORT 0x1
#define LOC_EVENT_MASK_GNSS_SV_INFO 0x2
#define LOC_EVENT_MASK_NMEA 0x4
#define LOC_CLIENT_TYPE_AFW 1

typedef struct {
    GMainLoop *loop;
    GAsyncResult *res;
} Wait;

static void on_done(GObject *source, GAsyncResult *res, gpointer data) {
    Wait *w = data;
    w->res = g_object_ref(res);
    g_main_loop_quit(w->loop);
}

static GAsyncResult *wait_result(Wait *w) {
    g_clear_object(&w->res);
    g_main_loop_run(w->loop);
    return w->res;
}

static void fail(const char *what, GError *error) {
    printf("error=%s: %s\n", what, error ? error->message : "unknown");
    fflush(stdout);
    exit(1);
}

/*
 * The modem rejects a registration without a client id and type, which libqmi's
 * REGISTER_EVENTS doesn't send, so build the request by hand.
 */
static void register_events(QmiDevice *dev, QmiClient *loc, Wait *w) {
    GError *error = NULL;
    g_autoptr(QmiMessage) msg = qmi_message_new(QMI_SERVICE_LOC, qmi_client_get_cid(loc), 0,
                                                QMI_LOC_REG_EVENTS);
    gsize tlv = qmi_message_tlv_write_init(msg, 0x01, NULL);
    qmi_message_tlv_write_guint64(msg, QMI_ENDIAN_LITTLE,
                                  LOC_EVENT_MASK_POSITION_REPORT | LOC_EVENT_MASK_GNSS_SV_INFO |
                                          LOC_EVENT_MASK_NMEA,
                                  NULL);
    qmi_message_tlv_write_complete(msg, tlv, NULL);
    tlv = qmi_message_tlv_write_init(msg, 0x10, NULL);
    qmi_message_tlv_write_string(msg, 0, "HAL", -1, NULL);
    qmi_message_tlv_write_complete(msg, tlv, NULL);
    tlv = qmi_message_tlv_write_init(msg, 0x11, NULL);
    qmi_message_tlv_write_guint32(msg, QMI_ENDIAN_LITTLE, LOC_CLIENT_TYPE_AFW, NULL);
    qmi_message_tlv_write_complete(msg, tlv, NULL);
    tlv = qmi_message_tlv_write_init(msg, 0x12, NULL);
    qmi_message_tlv_write_guint8(msg, 0, NULL);
    qmi_message_tlv_write_complete(msg, tlv, NULL);

    qmi_device_command_full(dev, msg, NULL, 10, NULL, on_done, w);
    g_autoptr(QmiMessage) resp = qmi_device_command_full_finish(dev, wait_result(w), &error);
    if (!resp) fail("register events", error);
    gsize init = qmi_message_tlv_read_init(resp, 0x02, NULL, &error);
    gsize off = 0;
    guint16 status = 1, code = 0;
    if (!init || !qmi_message_tlv_read_guint16(resp, init, &off, QMI_ENDIAN_LITTLE, &status, &error) ||
            !qmi_message_tlv_read_guint16(resp, init, &off, QMI_ENDIAN_LITTLE, &code, &error))
        fail("register events", error);
    if (status != 0) {
        printf("error=register events: QMI error %u\n", code);
        exit(1);
    }
}

static void on_nmea(QmiClientLoc *loc, QmiIndicationLocNmeaOutput *ind, gpointer data) {
    const gchar *nmea = NULL;
    if (qmi_indication_loc_nmea_output_get_nmea_string(ind, &nmea, NULL) && nmea) {
        printf("%s", nmea);
        if (nmea[0] && nmea[strlen(nmea) - 1] != '\n') printf("\n");
        fflush(stdout);
    }
}

static void on_position_report(QmiClientLoc *loc, QmiIndicationLocPositionReportOutput *ind,
                               gpointer data) {
    QmiLocSessionStatus status = 0;
    gdouble lat = 0, lon = 0;
    gfloat acc = 0;
    qmi_indication_loc_position_report_output_get_session_status(ind, &status, NULL);
    qmi_indication_loc_position_report_output_get_latitude(ind, &lat, NULL);
    qmi_indication_loc_position_report_output_get_longitude(ind, &lon, NULL);
    qmi_indication_loc_position_report_output_get_horizontal_uncertainty_circular(ind, &acc, NULL);
    printf("position status=%d lat=%.6f lon=%.6f acc=%.1f\n", status, lat, lon, acc);
    fflush(stdout);
}

static gboolean on_sigterm(gpointer loop) {
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static QrtrNode *find_loc_node(QrtrBus *bus) {
    GList *nodes = qrtr_bus_peek_nodes(bus);
    for (GList *l = nodes; l != NULL; l = l->next) {
        QrtrNode *n = QRTR_NODE(l->data);
        if (qrtr_node_lookup_port(n, QMI_SERVICE_LOC) >= 0) {
            return g_object_ref(n);
        }
    }
    return NULL;
}

int main(void) {
    GError *error = NULL;
    Wait w = {.loop = g_main_loop_new(NULL, FALSE)};

    qrtr_bus_new(1000, NULL, on_done, &w);
    g_autoptr(QrtrBus) bus = qrtr_bus_new_finish(wait_result(&w), &error);
    if (!bus) fail("qrtr bus", error);
    /*
     * On OnePlus 8T (SM8250 + external SDX55 over MHI), the modem exposes
     * QMI LOC (service 16) on QRTR node 3 rather than node 0. Prefer any
     * node already advertising QMI_SERVICE_LOC, then fall back to node 3.
     */
    g_autoptr(QrtrNode) node = find_loc_node(bus);
    if (!node) {
        qrtr_bus_wait_for_node(bus, 3, 30000, NULL, on_done, &w);
        node = qrtr_bus_wait_for_node_finish(bus, wait_result(&w), &error);
    }
    if (!node) fail("find modem node", error);
    qmi_device_new_from_node(node, NULL, on_done, &w);
    g_autoptr(QmiDevice) dev = qmi_device_new_from_node_finish(wait_result(&w), &error);
    if (!dev) fail("open device", error);
    qmi_device_open(dev, QMI_DEVICE_OPEN_FLAGS_NONE, 15, NULL, on_done, &w);
    if (!qmi_device_open_finish(dev, wait_result(&w), &error)) fail("open device", error);

    qmi_device_allocate_client(dev, QMI_SERVICE_LOC, QMI_CID_NONE, 10, NULL, on_done, &w);
    g_autoptr(QmiClient) loc = qmi_device_allocate_client_finish(dev, wait_result(&w), &error);
    if (!loc) fail("allocate client", error);

    /* Stock's location HAL unlocks the engine at start; it stays locked otherwise */
    {
        g_autoptr(QmiMessageLocSetEngineLockInput) in = qmi_message_loc_set_engine_lock_input_new();
        qmi_message_loc_set_engine_lock_input_set_lock_type(in, QMI_LOC_LOCK_TYPE_NONE, NULL);
        qmi_client_loc_set_engine_lock(QMI_CLIENT_LOC(loc), in, 10, NULL, on_done, &w);
        g_autoptr(QmiMessageLocSetEngineLockOutput) out =
                qmi_client_loc_set_engine_lock_finish(QMI_CLIENT_LOC(loc), wait_result(&w), &error);
        if (!out || !qmi_message_loc_set_engine_lock_output_get_result(out, &error)) fail("unlock engine", error);
    }
    {
        g_autoptr(QmiMessageLocSetNmeaTypesInput) in = qmi_message_loc_set_nmea_types_input_new();
        qmi_message_loc_set_nmea_types_input_set_nmea_types(
                in, QMI_LOC_NMEA_TYPE_GGA | QMI_LOC_NMEA_TYPE_RMC | QMI_LOC_NMEA_TYPE_GSV |
                            QMI_LOC_NMEA_TYPE_GSA | QMI_LOC_NMEA_TYPE_VTG |
                            QMI_LOC_NMEA_TYPE_GLGSV | QMI_LOC_NMEA_TYPE_GAGSV |
                            QMI_LOC_NMEA_TYPE_GBGSV | QMI_LOC_NMEA_TYPE_GQGSV |
                            QMI_LOC_NMEA_TYPE_GNGSA,
                NULL);
        qmi_client_loc_set_nmea_types(QMI_CLIENT_LOC(loc), in, 10, NULL, on_done, &w);
        g_autoptr(QmiMessageLocSetNmeaTypesOutput) out =
                qmi_client_loc_set_nmea_types_finish(QMI_CLIENT_LOC(loc), wait_result(&w), &error);
        /* Refused (SessionOwnership) while another client is registered; the setting persists */
        if (!out || !qmi_message_loc_set_nmea_types_output_get_result(out, &error)) {
            printf("warning=set nmea types: %s\n", error ? error->message : "unknown");
            g_clear_error(&error);
        }
    }
    /* Inject current system UTC time to speed up cold-start TTFF and immediate NMEA */
    {
        struct timespec ts;
        if (clock_gettime(CLOCK_REALTIME, &ts) == 0 && ts.tv_sec > 1577836800LL) {
            guint64 utc_ms = (guint64)ts.tv_sec * 1000ULL + (guint64)ts.tv_nsec / 1000000ULL;
            g_autoptr(QmiMessageLocInjectUtcTimeInput) in =
                    qmi_message_loc_inject_utc_time_input_new();
            qmi_message_loc_inject_utc_time_input_set_utc_time(in, utc_ms, NULL);
            qmi_message_loc_inject_utc_time_input_set_time_uncertainty(in, 5000, NULL);
            qmi_client_loc_inject_utc_time(QMI_CLIENT_LOC(loc), in, 10, NULL, on_done, &w);
            g_autoptr(QmiMessageLocInjectUtcTimeOutput) out =
                    qmi_client_loc_inject_utc_time_finish(QMI_CLIENT_LOC(loc), wait_result(&w),
                                                          &error);
            if (!out || !qmi_message_loc_inject_utc_time_output_get_result(out, &error)) {
                g_clear_error(&error);
            }
        }
    }

    register_events(dev, loc, &w);
    g_signal_connect(loc, "nmea", G_CALLBACK(on_nmea), NULL);
    g_signal_connect(loc, "position-report", G_CALLBACK(on_position_report), NULL);

    {
        g_autoptr(QmiMessageLocStartInput) in = qmi_message_loc_start_input_new();
        qmi_message_loc_start_input_set_session_id(in, 1, NULL);
        qmi_message_loc_start_input_set_fix_recurrence_type(
                in, QMI_LOC_FIX_RECURRENCE_TYPE_REQUEST_PERIODIC_FIXES, NULL);
        qmi_message_loc_start_input_set_intermediate_report_state(
                in, QMI_LOC_INTERMEDIATE_REPORT_STATE_ENABLE, NULL);
        qmi_message_loc_start_input_set_minimum_interval_between_position_reports(in, 1000, NULL);
        qmi_client_loc_start(QMI_CLIENT_LOC(loc), in, 10, NULL, on_done, &w);
        g_autoptr(QmiMessageLocStartOutput) out =
                qmi_client_loc_start_finish(QMI_CLIENT_LOC(loc), wait_result(&w), &error);
        if (!out || !qmi_message_loc_start_output_get_result(out, &error)) fail("start", error);
    }
    printf("started\n");
    fflush(stdout);

    g_unix_signal_add(SIGTERM, on_sigterm, w.loop);
    g_unix_signal_add(SIGINT, on_sigterm, w.loop);
    g_main_loop_run(w.loop);

    /* Stop session and release CID cleanly on exit */
    {
        g_autoptr(QmiMessageLocStopInput) in = qmi_message_loc_stop_input_new();
        qmi_message_loc_stop_input_set_session_id(in, 1, NULL);
        qmi_client_loc_stop(QMI_CLIENT_LOC(loc), in, 5, NULL, on_done, &w);
        g_autoptr(QmiMessageLocStopOutput) out =
                qmi_client_loc_stop_finish(QMI_CLIENT_LOC(loc), wait_result(&w), &error);
        g_clear_error(&error);
    }
    qmi_device_release_client(dev, loc, QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID, 5, NULL,
                              on_done, &w);
    qmi_device_release_client_finish(dev, wait_result(&w), &error);
    g_clear_error(&error);
    return 0;
}
