/*
 * Copyright (C) 2026 The PenguinOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * QMI LOC helper for SM8250 + external SDX55 modem over QRTR.
 *
 * Modes:
 *   kona-gnss [--interval <ms>]            Stream NMEA + structured QMI pos reports until SIGTERM
 *   kona-gnss --inject-xtra <file|->       Inject XTRA3 predicted orbits (0x00A7, fallback 0x0035)
 *   kona-gnss --inject-time <ms> <unc_ms>  Inject UTC time (0x0038)
 *   kona-gnss --inject-pos <lat> <lon> <acc> Inject coarse position (0x0039)
 *   kona-gnss --delete-aiding <flags>      Delete assistance data (0x0024)
 */

#include <glib-unix.h>
#include <libqmi-glib.h>
#include <libqrtr-glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define QMI_LOC_REG_EVENTS 0x0021
#define QMI_LOC_CONFIG_CONSTELLATION_MULTI_BAND 0x00E0
#define LOC_EVENT_MASK_POSITION_REPORT 0x1
#define LOC_EVENT_MASK_GNSS_SV_INFO 0x2
#define LOC_EVENT_MASK_NMEA 0x4
#define LOC_CLIENT_TYPE_AFW 1

#define XTRA_CHUNK_SIZE 1024
#define XTRA_MAX_SIZE (512 * 1024)

/* Matches aidl::android::hardware::gnss::GnssLocation::HAS_* */
#define POS_HAS_LAT_LONG (1u << 0)
#define POS_HAS_ALTITUDE (1u << 1)
#define POS_HAS_SPEED (1u << 2)
#define POS_HAS_BEARING (1u << 3)
#define POS_HAS_HORIZONTAL_ACCURACY (1u << 4)
#define POS_HAS_VERTICAL_ACCURACY (1u << 5)
#define POS_HAS_SPEED_ACCURACY (1u << 6)
#define POS_HAS_BEARING_ACCURACY (1u << 7)

typedef struct {
    GMainLoop *loop;
    GAsyncResult *res;
} Wait;

typedef struct {
    GMainLoop *loop;
    gboolean received;
    QmiLocIndicationStatus status;
    guint16 part_number;
} IndWait;

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

static gboolean on_ind_timeout(gpointer data) {
    IndWait *iw = data;
    g_main_loop_quit(iw->loop);
    return G_SOURCE_REMOVE;
}

static gboolean wait_indication(IndWait *iw, guint timeout_sec) {
    if (iw->received) return TRUE;
    guint tid = g_timeout_add_seconds(timeout_sec, on_ind_timeout, iw);
    g_main_loop_run(iw->loop);
    if (iw->received) g_source_remove(tid);
    return iw->received;
}

static void fail(const char *what, GError *error) {
    printf("error=%s: %s\n", what, error ? error->message : "unknown");
    fflush(stdout);
    exit(1);
}

static guint64 now_utc_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0 && ts.tv_sec > 1577836800LL) {
        return (guint64)ts.tv_sec * 1000ULL + (guint64)ts.tv_nsec / 1000000ULL;
    }
    return 0;
}

/*
 * The modem rejects a registration without a client id and type, which libqmi's
 * REGISTER_EVENTS doesn't send, so build the request by hand.
 */
static void register_events(QmiDevice *dev, QmiClient *loc, Wait *w) {
    GError *error = NULL;
    g_autoptr(QmiMessage) msg =
            qmi_message_new(QMI_SERVICE_LOC, qmi_client_get_cid(loc), 0, QMI_LOC_REG_EVENTS);
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
    if (!init ||
        !qmi_message_tlv_read_guint16(resp, init, &off, QMI_ENDIAN_LITTLE, &status, &error) ||
        !qmi_message_tlv_read_guint16(resp, init, &off, QMI_ENDIAN_LITTLE, &code, &error))
        fail("register events", error);
    if (status != 0) {
        printf("error=register events: QMI error %u\n", code);
        fflush(stdout);
        exit(1);
    }
}

/*
 * Enable all secondary frequency bands (L5/E5a/B2a) via QMI_LOC_CONFIG_CONSTELLATION_MULTI_BAND
 * (0x00E0): TLV 0x01 is the disabled-secondary-band constellation mask (0 = all enabled).
 */
static void enable_multiband(QmiDevice *dev, QmiClient *loc, Wait *w) {
    GError *error = NULL;
    g_autoptr(QmiMessage) msg = qmi_message_new(QMI_SERVICE_LOC, qmi_client_get_cid(loc), 0,
                                                QMI_LOC_CONFIG_CONSTELLATION_MULTI_BAND);
    gsize tlv = qmi_message_tlv_write_init(msg, 0x01, NULL);
    qmi_message_tlv_write_guint64(msg, QMI_ENDIAN_LITTLE, 0, NULL);
    qmi_message_tlv_write_complete(msg, tlv, NULL);

    qmi_device_command_full(dev, msg, NULL, 5, NULL, on_done, w);
    g_autoptr(QmiMessage) resp = qmi_device_command_full_finish(dev, wait_result(w), &error);
    if (!resp) {
        printf("warning=config multiband: %s\n", error ? error->message : "unknown");
        g_clear_error(&error);
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
    QmiLocSessionStatus status = QMI_LOC_SESSION_STATUS_GENERAL_FAILURE;
    guint32 flags = 0;
    gdouble lat = 0.0, lon = 0.0;
    gfloat alt = 0.0f, hacc = 0.0f, vacc = 0.0f;
    gfloat speed = 0.0f, sacc = 0.0f, bearing = 0.0f, bacc = 0.0f;
    guint64 utc_ms = 0;

    qmi_indication_loc_position_report_output_get_session_status(ind, &status, NULL);

    if (qmi_indication_loc_position_report_output_get_latitude(ind, &lat, NULL) &&
        qmi_indication_loc_position_report_output_get_longitude(ind, &lon, NULL) &&
        isfinite(lat) && isfinite(lon)) {
        flags |= POS_HAS_LAT_LONG;
    }

    if (qmi_indication_loc_position_report_output_get_altitude_from_ellipsoid(ind, &alt, NULL) &&
        isfinite(alt)) {
        flags |= POS_HAS_ALTITUDE;
    } else if (qmi_indication_loc_position_report_output_get_altitude_from_sealevel(ind, &alt,
                                                                                    NULL) &&
               isfinite(alt)) {
        flags |= POS_HAS_ALTITUDE;
    }

    if (qmi_indication_loc_position_report_output_get_horizontal_uncertainty_circular(ind, &hacc,
                                                                                      NULL) &&
        isfinite(hacc) && hacc > 0.0f) {
        flags |= POS_HAS_HORIZONTAL_ACCURACY;
    }

    if (qmi_indication_loc_position_report_output_get_vertical_uncertainty(ind, &vacc, NULL) &&
        isfinite(vacc) && vacc > 0.0f) {
        flags |= POS_HAS_VERTICAL_ACCURACY;
    }

    if (qmi_indication_loc_position_report_output_get_horizontal_speed(ind, &speed, NULL) &&
        isfinite(speed) && speed >= 0.0f) {
        flags |= POS_HAS_SPEED;
    }

    if (qmi_indication_loc_position_report_output_get_speed_uncertainty(ind, &sacc, NULL) &&
        isfinite(sacc) && sacc > 0.0f) {
        flags |= POS_HAS_SPEED_ACCURACY;
    }

    if (qmi_indication_loc_position_report_output_get_heading(ind, &bearing, NULL) &&
        isfinite(bearing)) {
        flags |= POS_HAS_BEARING;
    }

    if (qmi_indication_loc_position_report_output_get_heading_uncertainty(ind, &bacc, NULL) &&
        isfinite(bacc) && bacc > 0.0f) {
        flags |= POS_HAS_BEARING_ACCURACY;
    }

    if (!qmi_indication_loc_position_report_output_get_utc_timestamp(ind, &utc_ms, NULL) ||
        utc_ms == 0) {
        utc_ms = now_utc_ms();
    }

    printf("pos status=%d flags=%u lat=%.8f lon=%.8f alt=%.3f hacc=%.3f vacc=%.3f "
           "speed=%.3f sacc=%.3f bearing=%.3f bacc=%.3f utc=%" G_GUINT64_FORMAT "\n",
           (int)status, flags, lat, lon, alt, hacc, vacc, speed, sacc, bearing, bacc, utc_ms);
    fflush(stdout);
}

static void on_xtra_ind(QmiClientLoc *loc, QmiIndicationLocInjectXtraDataOutput *ind,
                        gpointer data) {
    IndWait *iw = data;
    iw->status = QMI_LOC_INDICATION_STATUS_GENERAL_FAILURE;
    iw->part_number = 0;
    qmi_indication_loc_inject_xtra_data_output_get_indication_status(ind, &iw->status, NULL);
    qmi_indication_loc_inject_xtra_data_output_get_part_number(ind, &iw->part_number, NULL);
    iw->received = TRUE;
    g_main_loop_quit(iw->loop);
}

static void on_orbits_ind(QmiClientLoc *loc, QmiIndicationLocInjectPredictedOrbitsDataOutput *ind,
                          gpointer data) {
    IndWait *iw = data;
    iw->status = QMI_LOC_INDICATION_STATUS_GENERAL_FAILURE;
    iw->part_number = 0;
    qmi_indication_loc_inject_predicted_orbits_data_output_get_indication_status(ind, &iw->status,
                                                                                 NULL);
    qmi_indication_loc_inject_predicted_orbits_data_output_get_part_number(ind, &iw->part_number,
                                                                           NULL);
    iw->received = TRUE;
    g_main_loop_quit(iw->loop);
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

/*
 * Bring the external SDX55 modem to ONLINE mode via QMI DMS when no RIL has
 * done so yet. Without ONLINE mode (e.g. modem sitting in shutting-down / LPM),
 * QMI LOC accepts START but keeps ENGINE_STATE=OFF with zero SV/NMEA output.
 */
static void ensure_modem_online(QmiDevice *dev, Wait *w) {
    GError *error = NULL;
    qmi_device_allocate_client(dev, QMI_SERVICE_DMS, QMI_CID_NONE, 10, NULL, on_done, w);
    g_autoptr(QmiClient) dms = qmi_device_allocate_client_finish(dev, wait_result(w), &error);
    if (!dms) {
        printf("warning=dms allocate: %s\n", error ? error->message : "unknown");
        g_clear_error(&error);
        return;
    }

    qmi_client_dms_get_operating_mode(QMI_CLIENT_DMS(dms), NULL, 10, NULL, on_done, w);
    g_autoptr(QmiMessageDmsGetOperatingModeOutput) out =
            qmi_client_dms_get_operating_mode_finish(QMI_CLIENT_DMS(dms), wait_result(w), &error);
    if (out && qmi_message_dms_get_operating_mode_output_get_result(out, &error)) {
        QmiDmsOperatingMode mode = QMI_DMS_OPERATING_MODE_ONLINE;
        qmi_message_dms_get_operating_mode_output_get_mode(out, &mode, NULL);
        if (mode != QMI_DMS_OPERATING_MODE_ONLINE) {
            g_autoptr(QmiMessageDmsSetOperatingModeInput) sin =
                    qmi_message_dms_set_operating_mode_input_new();
            qmi_message_dms_set_operating_mode_input_set_mode(sin, QMI_DMS_OPERATING_MODE_ONLINE,
                                                              NULL);
            qmi_client_dms_set_operating_mode(QMI_CLIENT_DMS(dms), sin, 10, NULL, on_done, w);
            g_autoptr(QmiMessageDmsSetOperatingModeOutput) sout =
                    qmi_client_dms_set_operating_mode_finish(QMI_CLIENT_DMS(dms), wait_result(w),
                                                             &error);
            if (!sout || !qmi_message_dms_set_operating_mode_output_get_result(sout, &error)) {
                printf("warning=dms set online: %s\n", error ? error->message : "unknown");
                g_clear_error(&error);
            }
        }
    } else {
        printf("warning=dms get mode: %s\n", error ? error->message : "unknown");
        g_clear_error(&error);
    }

    qmi_device_release_client(dev, dms, QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID, 5, NULL,
                              on_done, w);
    qmi_device_release_client_finish(dev, wait_result(w), &error);
    g_clear_error(&error);
}

static void inject_utc_time(QmiClient *loc, Wait *w, guint64 utc_ms, guint32 unc_ms) {
    GError *error = NULL;
    g_autoptr(QmiMessageLocInjectUtcTimeInput) in = qmi_message_loc_inject_utc_time_input_new();
    qmi_message_loc_inject_utc_time_input_set_utc_time(in, utc_ms, NULL);
    qmi_message_loc_inject_utc_time_input_set_time_uncertainty(in, unc_ms, NULL);
    qmi_client_loc_inject_utc_time(QMI_CLIENT_LOC(loc), in, 10, NULL, on_done, w);
    g_autoptr(QmiMessageLocInjectUtcTimeOutput) out =
            qmi_client_loc_inject_utc_time_finish(QMI_CLIENT_LOC(loc), wait_result(w), &error);
    if (!out || !qmi_message_loc_inject_utc_time_output_get_result(out, &error)) {
        printf("warning=inject utc time: %s\n", error ? error->message : "unknown");
        g_clear_error(&error);
    }
}

static int inject_xtra_chunks(QmiClient *loc, Wait *w, const guint8 *data, gsize len,
                              gboolean use_legacy) {
    GError *error = NULL;
    IndWait iw = {.loop = g_main_loop_new(NULL, FALSE)};
    gulong sig = g_signal_connect(
            loc, use_legacy ? "inject-predicted-orbits-data" : "inject-xtra-data",
            G_CALLBACK(use_legacy ? (GCallback)on_orbits_ind : (GCallback)on_xtra_ind), &iw);

    guint16 total_parts = (guint16)((len + XTRA_CHUNK_SIZE - 1) / XTRA_CHUNK_SIZE);
    int rc = 0;

    for (guint16 part = 1; part <= total_parts; part++) {
        gsize offset = (gsize)(part - 1) * XTRA_CHUNK_SIZE;
        gsize chunk_len = len - offset;
        if (chunk_len > XTRA_CHUNK_SIZE) chunk_len = XTRA_CHUNK_SIZE;

        GArray *arr = g_array_sized_new(FALSE, FALSE, sizeof(guint8), (guint)chunk_len);
        g_array_append_vals(arr, data + offset, (guint)chunk_len);

        iw.received = FALSE;
        iw.status = QMI_LOC_INDICATION_STATUS_GENERAL_FAILURE;
        iw.part_number = 0;

        if (!use_legacy) {
            g_autoptr(QmiMessageLocInjectXtraDataInput) in =
                    qmi_message_loc_inject_xtra_data_input_new();
            qmi_message_loc_inject_xtra_data_input_set_total_size(in, (guint32)len, NULL);
            qmi_message_loc_inject_xtra_data_input_set_total_parts(in, total_parts, NULL);
            qmi_message_loc_inject_xtra_data_input_set_part_number(in, part, NULL);
            qmi_message_loc_inject_xtra_data_input_set_part_data(in, arr, NULL);
            qmi_client_loc_inject_xtra_data(QMI_CLIENT_LOC(loc), in, 10, NULL, on_done, w);
            g_autoptr(QmiMessageLocInjectXtraDataOutput) out =
                    qmi_client_loc_inject_xtra_data_finish(QMI_CLIENT_LOC(loc), wait_result(w),
                                                           &error);
            if (!out || !qmi_message_loc_inject_xtra_data_output_get_result(out, &error)) {
                printf("warning=inject xtra part %u/%u: %s\n", part, total_parts,
                       error ? error->message : "unknown");
                g_clear_error(&error);
                g_array_unref(arr);
                rc = -1;
                break;
            }
        } else {
            g_autoptr(QmiMessageLocInjectPredictedOrbitsDataInput) in =
                    qmi_message_loc_inject_predicted_orbits_data_input_new();
            qmi_message_loc_inject_predicted_orbits_data_input_set_total_size(in, (guint32)len,
                                                                              NULL);
            qmi_message_loc_inject_predicted_orbits_data_input_set_total_parts(in, total_parts,
                                                                               NULL);
            qmi_message_loc_inject_predicted_orbits_data_input_set_part_number(in, part, NULL);
            qmi_message_loc_inject_predicted_orbits_data_input_set_part_data(in, arr, NULL);
            qmi_message_loc_inject_predicted_orbits_data_input_set_format_type(
                    in, QMI_LOC_PREDICTED_ORBITS_DATA_FORMAT_XTRA, NULL);
            qmi_client_loc_inject_predicted_orbits_data(QMI_CLIENT_LOC(loc), in, 10, NULL, on_done,
                                                        w);
            g_autoptr(QmiMessageLocInjectPredictedOrbitsDataOutput) out =
                    qmi_client_loc_inject_predicted_orbits_data_finish(QMI_CLIENT_LOC(loc),
                                                                       wait_result(w), &error);
            if (!out ||
                !qmi_message_loc_inject_predicted_orbits_data_output_get_result(out, &error)) {
                printf("warning=inject orbits part %u/%u: %s\n", part, total_parts,
                       error ? error->message : "unknown");
                g_clear_error(&error);
                g_array_unref(arr);
                rc = -1;
                break;
            }
        }
        g_array_unref(arr);

        if (!wait_indication(&iw, 3)) {
            printf("warning=xtra ind timeout part %u/%u\n", part, total_parts);
            rc = -1;
            break;
        }
        if (iw.status != QMI_LOC_INDICATION_STATUS_SUCCESS || iw.part_number != part) {
            printf("warning=xtra ind status=%d part=%u expected=%u\n", (int)iw.status,
                   iw.part_number, part);
            rc = -1;
            break;
        }
    }

    g_signal_handler_disconnect(loc, sig);
    g_main_loop_unref(iw.loop);
    return rc;
}

static int run_inject_xtra(QmiClient *loc, Wait *w, const char *path) {
    FILE *fp = (strcmp(path, "-") == 0) ? stdin : fopen(path, "rb");
    if (!fp) {
        printf("error=open xtra: %s\n", path);
        fflush(stdout);
        return 1;
    }

    guint8 *buf = malloc(XTRA_MAX_SIZE);
    if (!buf) {
        if (fp != stdin) fclose(fp);
        return 1;
    }

    gsize total = 0;
    while (total < XTRA_MAX_SIZE) {
        size_t n = fread(buf + total, 1, XTRA_MAX_SIZE - total, fp);
        if (n == 0) break;
        total += n;
    }
    if (fp != stdin) fclose(fp);

    if (total == 0) {
        free(buf);
        printf("error=empty xtra data\n");
        fflush(stdout);
        return 1;
    }

    int rc = inject_xtra_chunks(loc, w, buf, total, FALSE);
    if (rc != 0) {
        printf("warning=retrying xtra via legacy 0x0035\n");
        rc = inject_xtra_chunks(loc, w, buf, total, TRUE);
    }

    if (rc == 0) {
        printf("xtra_injected=%" G_GSIZE_FORMAT " parts=%u\n", total,
               (unsigned)((total + XTRA_CHUNK_SIZE - 1) / XTRA_CHUNK_SIZE));
    } else {
        printf("error=xtra injection failed\n");
    }
    fflush(stdout);
    free(buf);
    return rc == 0 ? 0 : 1;
}

static int run_inject_pos(QmiClient *loc, Wait *w, gdouble lat, gdouble lon, gfloat hacc) {
    GError *error = NULL;
    g_autoptr(QmiMessageLocInjectPositionInput) in = qmi_message_loc_inject_position_input_new();
    qmi_message_loc_inject_position_input_set_latitude(in, lat, NULL);
    qmi_message_loc_inject_position_input_set_longitude(in, lon, NULL);
    qmi_message_loc_inject_position_input_set_horizontal_uncertainty_circular(in, hacc, NULL);
    qmi_message_loc_inject_position_input_set_horizontal_reliability(
            in, QMI_LOC_RELIABILITY_MEDIUM, NULL);
    guint64 utc = now_utc_ms();
    if (utc > 0) qmi_message_loc_inject_position_input_set_utc_timestamp(in, utc, NULL);

    qmi_client_loc_inject_position(QMI_CLIENT_LOC(loc), in, 10, NULL, on_done, w);
    g_autoptr(QmiMessageLocInjectPositionOutput) out =
            qmi_client_loc_inject_position_finish(QMI_CLIENT_LOC(loc), wait_result(w), &error);
    if (!out || !qmi_message_loc_inject_position_output_get_result(out, &error)) {
        printf("error=inject position: %s\n", error ? error->message : "unknown");
        g_clear_error(&error);
        fflush(stdout);
        return 1;
    }
    printf("pos_injected=ok\n");
    fflush(stdout);
    return 0;
}

static int run_delete_aiding(QmiClient *loc, Wait *w, guint32 flags) {
    GError *error = NULL;
    g_autoptr(QmiMessageLocDeleteAssistanceDataInput) in =
            qmi_message_loc_delete_assistance_data_input_new();
    /* 0xFFFF is GnssAidingData::ALL; default to delete_all for cold-start testing */
    qmi_message_loc_delete_assistance_data_input_set_delete_all(in, flags != 0, NULL);
    qmi_client_loc_delete_assistance_data(QMI_CLIENT_LOC(loc), in, 10, NULL, on_done, w);
    g_autoptr(QmiMessageLocDeleteAssistanceDataOutput) out =
            qmi_client_loc_delete_assistance_data_finish(QMI_CLIENT_LOC(loc), wait_result(w),
                                                         &error);
    if (!out || !qmi_message_loc_delete_assistance_data_output_get_result(out, &error)) {
        printf("error=delete aiding: %s\n", error ? error->message : "unknown");
        g_clear_error(&error);
        fflush(stdout);
        return 1;
    }
    printf("aiding_deleted=ok\n");
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv) {
    GError *error = NULL;
    Wait w = {.loop = g_main_loop_new(NULL, FALSE)};
    guint32 interval_ms = 1000;

    enum {
        MODE_TRACK,
        MODE_INJECT_XTRA,
        MODE_INJECT_TIME,
        MODE_INJECT_POS,
        MODE_DELETE_AIDING,
    } mode = MODE_TRACK;
    const char *xtra_path = NULL;
    guint64 inject_time_ms = 0;
    guint32 inject_time_unc = 5000;
    gdouble inject_lat = 0.0, inject_lon = 0.0;
    gfloat inject_acc = 1000.0f;
    guint32 delete_flags = 0xFFFF;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
            long v = atol(argv[++i]);
            if (v >= 100 && v <= 600000) interval_ms = (guint32)v;
        } else if (strcmp(argv[i], "--inject-xtra") == 0 && i + 1 < argc) {
            mode = MODE_INJECT_XTRA;
            xtra_path = argv[++i];
        } else if (strcmp(argv[i], "--inject-time") == 0 && i + 2 < argc) {
            mode = MODE_INJECT_TIME;
            inject_time_ms = (guint64)strtoull(argv[++i], NULL, 10);
            inject_time_unc = (guint32)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--inject-pos") == 0 && i + 3 < argc) {
            mode = MODE_INJECT_POS;
            inject_lat = strtod(argv[++i], NULL);
            inject_lon = strtod(argv[++i], NULL);
            inject_acc = strtof(argv[++i], NULL);
        } else if (strcmp(argv[i], "--delete-aiding") == 0 && i + 1 < argc) {
            mode = MODE_DELETE_AIDING;
            delete_flags = (guint32)strtoul(argv[++i], NULL, 0);
        }
    }

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

    ensure_modem_online(dev, &w);

    qmi_device_allocate_client(dev, QMI_SERVICE_LOC, QMI_CID_NONE, 10, NULL, on_done, &w);
    g_autoptr(QmiClient) loc = qmi_device_allocate_client_finish(dev, wait_result(&w), &error);
    if (!loc) fail("allocate client", error);

    int exit_code = 0;

    if (mode != MODE_TRACK) {
        if (mode == MODE_INJECT_XTRA) {
            exit_code = run_inject_xtra(loc, &w, xtra_path);
        } else if (mode == MODE_INJECT_TIME) {
            inject_utc_time(loc, &w, inject_time_ms, inject_time_unc);
            printf("time_injected=ok\n");
            fflush(stdout);
        } else if (mode == MODE_INJECT_POS) {
            exit_code = run_inject_pos(loc, &w, inject_lat, inject_lon, inject_acc);
        } else if (mode == MODE_DELETE_AIDING) {
            exit_code = run_delete_aiding(loc, &w, delete_flags);
        }
        qmi_device_release_client(dev, loc, QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID, 5, NULL,
                                  on_done, &w);
        qmi_device_release_client_finish(dev, wait_result(&w), &error);
        g_clear_error(&error);
        return exit_code;
    }

    /* Stock's location HAL unlocks the engine at start; it stays locked otherwise */
    {
        g_autoptr(QmiMessageLocSetEngineLockInput) in = qmi_message_loc_set_engine_lock_input_new();
        qmi_message_loc_set_engine_lock_input_set_lock_type(in, QMI_LOC_LOCK_TYPE_NONE, NULL);
        qmi_client_loc_set_engine_lock(QMI_CLIENT_LOC(loc), in, 10, NULL, on_done, &w);
        g_autoptr(QmiMessageLocSetEngineLockOutput) out =
                qmi_client_loc_set_engine_lock_finish(QMI_CLIENT_LOC(loc), wait_result(&w), &error);
        if (!out || !qmi_message_loc_set_engine_lock_output_get_result(out, &error))
            fail("unlock engine", error);
    }

    /* Explicitly set STANDALONE operation mode (0x004A) like stock LocApiV02::startFix */
    {
        g_autoptr(QmiMessageLocSetOperationModeInput) in =
                qmi_message_loc_set_operation_mode_input_new();
        qmi_message_loc_set_operation_mode_input_set_operation_mode(
                in, QMI_LOC_OPERATION_MODE_STANDALONE, NULL);
        qmi_client_loc_set_operation_mode(QMI_CLIENT_LOC(loc), in, 10, NULL, on_done, &w);
        g_autoptr(QmiMessageLocSetOperationModeOutput) out =
                qmi_client_loc_set_operation_mode_finish(QMI_CLIENT_LOC(loc), wait_result(&w),
                                                         &error);
        if (!out || !qmi_message_loc_set_operation_mode_output_get_result(out, &error)) {
            printf("warning=set operation mode: %s\n", error ? error->message : "unknown");
            g_clear_error(&error);
        }
    }

    /* Enable dual-frequency secondary bands (L5/E5a/B2a) like stock configConstellationMultiBand */
    enable_multiband(dev, loc, &w);

    {
        g_autoptr(QmiMessageLocSetNmeaTypesInput) in = qmi_message_loc_set_nmea_types_input_new();
        qmi_message_loc_set_nmea_types_input_set_nmea_types(
                in, QMI_LOC_NMEA_TYPE_GGA | QMI_LOC_NMEA_TYPE_RMC | QMI_LOC_NMEA_TYPE_GSV |
                            QMI_LOC_NMEA_TYPE_GSA | QMI_LOC_NMEA_TYPE_VTG |
                            QMI_LOC_NMEA_TYPE_GLGSV | QMI_LOC_NMEA_TYPE_GAGSV |
                            QMI_LOC_NMEA_TYPE_GBGSV | QMI_LOC_NMEA_TYPE_GQGSV |
                            QMI_LOC_NMEA_TYPE_GIGSV | QMI_LOC_NMEA_TYPE_GNGSA,
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
        guint64 utc = now_utc_ms();
        if (utc > 0) inject_utc_time(loc, &w, utc, 5000);
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
        qmi_message_loc_start_input_set_minimum_interval_between_position_reports(in, interval_ms,
                                                                                  NULL);
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
