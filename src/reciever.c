/*
 * receiver.c — WebRTC receiver (H.264) using GStreamer webrtcbin + libsoup signaling.
 *
 * Updated for:
 *  - libsoup 3.x (websocket_connect_async has io_priority)
 *  - explicit H.264 receive chain:
 *      webrtcbin -> queue -> rtph264depay -> h264parse -> avdec_h264 -> videoconvert -> autovideosink
 */

#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include <string.h>

 /* ---------- Globals ---------- */
static GMainLoop* loop = NULL;
static GstElement* pipep = NULL;
static GstElement* webrtc = NULL;

static SoupWebsocketConnection* ws_conn = NULL;

static gchar* server_url = "wss://108.130.0.118:8080";
static gboolean disable_ssl = TRUE; /* currently not wired for libsoup3 self-signed handling */

static gboolean video_chain_built = FALSE;

/* ---------- Helpers: JSON stringify ---------- */
static gchar*
json_object_to_string(JsonObject* object)
{
    JsonNode* root = json_node_init_object(json_node_alloc(), object);
    JsonGenerator* gen = json_generator_new();
    json_generator_set_root(gen, root);
    gchar* text = json_generator_to_data(gen, NULL);
    g_object_unref(gen);
    json_node_free(root);
    return text;
}

/* ---------- Cleanup ---------- */
static gboolean
cleanup_and_quit(const gchar* msg)
{
    if (msg)
        g_printerr("%s\n", msg);

    if (ws_conn) {
        if (soup_websocket_connection_get_state(ws_conn) == SOUP_WEBSOCKET_STATE_OPEN)
            soup_websocket_connection_close(ws_conn, 1000, "");
        else
            g_clear_object(&ws_conn);
    }

    if (pipep) {
        gst_element_set_state(pipep, GST_STATE_NULL);
        g_clear_object(&pipep);
        webrtc = NULL;
    }

    if (loop) {
        g_main_loop_quit(loop);
        g_clear_pointer(&loop, g_main_loop_unref);
    }

    return G_SOURCE_REMOVE;
}

/* ---------- Media handling: explicit H.264 RTP -> depay -> parse -> decode -> display ---------- */



/*Треба віддебажити*/
static void
on_incoming_stream(GstElement* webrtc, GstPad* pad, GstElement* pipep)
{
    (void)webrtc;

    if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC)
        return;

    /* Щоб не створювати кілька decode/display chain */
    if (video_chain_built) {
        g_print("[receiver] Video chain already built, ignoring extra pad\n");
        return;
    }

    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) {
        g_printerr("[receiver] No caps on incoming pad\n");
        return;
    }

    const GstStructure* s = gst_caps_get_structure(caps, 0);
    const gchar* media_type = gst_structure_get_name(s);
    const gchar* encoding = gst_structure_get_string(s, "encoding-name");

    g_print("[receiver] pad caps: %s, encoding=%s\n",
        media_type ? media_type : "null",
        encoding ? encoding : "null");

    /* Працюємо тільки з RTP H264 */
    if (!media_type || !g_str_has_prefix(media_type, "application/x-rtp") ||
        !encoding || g_strcmp0(encoding, "H264") != 0) {
        g_print("[receiver] Ignoring non-H264 pad\n");
        gst_caps_unref(caps);
        return;
    }

    GError* err = NULL;
    //треба міняти max-size-buffers=значення щоб не було піксельного, але водночас не перебільшувати хоча якщо навіть 100 то не завжи погано
    /* Група (bin) з усіх модулів */
    GstElement* rxbin = gst_parse_bin_from_description(
        "queue name=rxq "
        "max-size-buffers=10 max-size-bytes=0 max-size-time=0 leaky=downstream ! "
        "rtph264depay name=depay ! "
        "h264parse name=parse config-interval=1 ! "
        "avdec_h264 name=dec ! "
        "videoconvert name=conv ! "
        "autovideosink name=vsink sync=false",
        TRUE,
        &err
    );

    if (err) {
        g_printerr("[receiver] Failed to create H264 bin: %s\n", err->message);
        g_error_free(err);
        gst_caps_unref(caps);
        return;
    }

    if (!rxbin) {
        g_printerr("[receiver] H264 bin is NULL\n");
        gst_caps_unref(caps);
        return;
    }

    /* Додаткові налаштування depay (лише якщо ці properties є у твоїй версії GStreamer) */
    GstElement* depay = gst_bin_get_by_name(GST_BIN(rxbin), "depay");
    if (depay) {
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(depay), "request-keyframe")) {
            g_object_set(depay, "request-keyframe", TRUE, NULL);
        }
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(depay), "wait-for-keyframe")) {
            /* FALSE = менше фризів, але після втрати можуть бути артефакти */
            g_object_set(depay, "wait-for-keyframe", FALSE, NULL);
        }
        gst_object_unref(depay);
    }

    /* Можна ще окремо докрутити sink (якщо захочеш qos=false / max-lateness) */
    GstElement* vsink = gst_bin_get_by_name(GST_BIN(rxbin), "vsink");
    if (vsink) {
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(vsink), "qos")) {
            g_object_set(vsink, "qos", FALSE, NULL);
        }
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(vsink), "max-lateness")) {
            g_object_set(vsink, "max-lateness", (gint64)0, NULL);
        }
        gst_object_unref(vsink);
    }

    gst_bin_add(GST_BIN(pipep), rxbin);
    gst_element_sync_state_with_parent(rxbin);

    /* У rxbin буде ghost sink pad (бо TRUE в gst_parse_bin_from_description) */
    GstPad* sinkpad = gst_element_get_static_pad(rxbin, "sink");
    if (!sinkpad) {
        g_printerr("[receiver] rxbin has no ghost sink pad\n");
        gst_caps_unref(caps);
        return;
    }

    GstPadLinkReturn ret = gst_pad_link(pad, sinkpad);
    gst_object_unref(sinkpad);

    if (ret != GST_PAD_LINK_OK) {
        g_printerr("[receiver] Failed to link webrtc pad -> rxbin (ret=%d)\n", ret);
    }
    else {
        video_chain_built = TRUE;
        g_print("[receiver] H264 receiver bin linked\n");
    }

    gst_caps_unref(caps);
}

/* ---------- Signaling: send ICE ---------- */
static void
send_ice_candidate(GstElement* webrtcbin, guint mlineindex, gchar* candidate, gpointer user_data)
{
    (void)webrtcbin;
    (void)user_data;

    if (!ws_conn || soup_websocket_connection_get_state(ws_conn) != SOUP_WEBSOCKET_STATE_OPEN)
        return;

    JsonObject* ice = json_object_new();
    json_object_set_string_member(ice, "candidate", candidate);
    json_object_set_int_member(ice, "sdpMLineIndex", (gint)mlineindex);

    JsonObject* msg = json_object_new();
    json_object_set_object_member(msg, "ice", ice);

    gchar* text = json_object_to_string(msg);
    json_object_unref(msg);

    soup_websocket_connection_send_text(ws_conn, text);
    g_free(text);
}

/* ---------- Signaling: send SDP ---------- */
static void
send_sdp(GstWebRTCSessionDescription* desc)
{
    if (!ws_conn || soup_websocket_connection_get_state(ws_conn) != SOUP_WEBSOCKET_STATE_OPEN)
        return;

    gchar* sdp_text = gst_sdp_message_as_text(desc->sdp);

    JsonObject* sdp = json_object_new();
    json_object_set_string_member(sdp, "type",
        desc->type == GST_WEBRTC_SDP_TYPE_OFFER ? "offer" : "answer");
    json_object_set_string_member(sdp, "sdp", sdp_text);

    JsonObject* msg = json_object_new();
    json_object_set_object_member(msg, "sdp", sdp);

    gchar* text = json_object_to_string(msg);
    json_object_unref(msg);

    soup_websocket_connection_send_text(ws_conn, text);

    g_free(text);
    g_free(sdp_text);
}

/* ---------- Answer created callback ---------- */
static void
on_answer_created(GstPromise* promise, gpointer user_data)
{
    (void)user_data;

    const GstStructure* reply = NULL;
    GstWebRTCSessionDescription* answer = NULL;

    g_assert(gst_promise_wait(promise) == GST_PROMISE_RESULT_REPLIED);
    reply = gst_promise_get_reply(promise);
    gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, NULL);
    gst_promise_unref(promise);

    /* Set local description */
    GstPromise* p = gst_promise_new();
    g_signal_emit_by_name(webrtc, "set-local-description", answer, p);
    gst_promise_interrupt(p);
    gst_promise_unref(p);

    /* Send to peer */
    g_print("[receiver] Sending SDP answer\n");
    send_sdp(answer);

    gst_webrtc_session_description_free(answer);
}

static void
on_offer_set(GstPromise* promise, gpointer user_data)
{
    (void)user_data;
    gst_promise_unref(promise);

    GstPromise* p = gst_promise_new_with_change_func(on_answer_created, NULL, NULL);
    g_signal_emit_by_name(webrtc, "create-answer", NULL, p);
}

/* ---------- Receive signaling messages (offer + ICE) ---------- */
static void
handle_server_message(SoupWebsocketConnection* conn, SoupWebsocketDataType type,
    GBytes* message, gpointer user_data)
{
    (void)conn;
    (void)user_data;

    if (type != SOUP_WEBSOCKET_DATA_TEXT)
        return;

    gsize size = 0;
    const gchar* data = g_bytes_get_data(message, &size);
    gchar* text = g_strndup(data, size);

    JsonParser* parser = json_parser_new();
    if (!json_parser_load_from_data(parser, text, -1, NULL)) {
        g_object_unref(parser);
        g_free(text);
        return;
    }

    JsonNode* root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        g_free(text);
        return;
    }

    JsonObject* obj = json_node_get_object(root);

    if (json_object_has_member(obj, "sdp")) {
        JsonObject* sdpobj = json_object_get_object_member(obj, "sdp");
        const gchar* sdptype = json_object_get_string_member(sdpobj, "type");
        const gchar* sdptext = json_object_get_string_member(sdpobj, "sdp");

        if (g_strcmp0(sdptype, "offer") == 0) {
            GstSDPMessage* sdp = NULL;
            gst_sdp_message_new(&sdp);
            gst_sdp_message_parse_buffer((guint8*)sdptext, (guint)strlen(sdptext), sdp);

            GstWebRTCSessionDescription* offer =
                gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp);

            g_print("[receiver] Received SDP offer -> set-remote-description\n");
            GstPromise* p = gst_promise_new_with_change_func(on_offer_set, NULL, NULL);
            g_signal_emit_by_name(webrtc, "set-remote-description", offer, p);

            gst_webrtc_session_description_free(offer);
        }
    }
    else if (json_object_has_member(obj, "ice")) {
        JsonObject* ice = json_object_get_object_member(obj, "ice");
        const gchar* candidate = json_object_get_string_member(ice, "candidate");
        gint mline = json_object_get_int_member(ice, "sdpMLineIndex");
        g_signal_emit_by_name(webrtc, "add-ice-candidate", mline, candidate);
    }

    g_object_unref(parser);
    g_free(text);
}

/* ---------- Create receiver pipepline ---------- */
static gboolean
start_pipeline(void)
{
    pipep = gst_pipeline_new("receiver-pipepline");
    webrtc = gst_element_factory_make("webrtcbin", "sendrecv");

    if (!pipep || !webrtc) {
        g_printerr("[receiver] Failed to create pipepline or webrtcbin\n");
        return FALSE;
    }

    /* Match your previous parse-launch property */
    g_object_set(webrtc, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, NULL);

    gst_bin_add(GST_BIN(pipep), webrtc);

    g_signal_connect(webrtc, "on-ice-candidate", G_CALLBACK(send_ice_candidate), NULL);
    g_signal_connect(webrtc, "pad-added", G_CALLBACK(on_incoming_stream), pipep);

    GstStateChangeReturn sret = gst_element_set_state(pipep, GST_STATE_PLAYING);
    if (sret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("[receiver] Failed to set pipepline to PLAYING\n");
        return FALSE;
    }

    g_print("[receiver] pipeline started (waiting for offer)\n");
    return TRUE;
}

/* ---------- WebSocket connect ---------- */
static void
on_server_closed(SoupWebsocketConnection* conn, gpointer user_data)
{
    (void)conn;
    (void)user_data;
    cleanup_and_quit("[receiver] Server closed");
}

static void
on_server_connected(SoupSession* session, GAsyncResult* res, SoupMessage* msg)
{
    (void)msg;

    GError* error = NULL;
    ws_conn = soup_session_websocket_connect_finish(session, res, &error);
    if (error) {
        g_printerr("WS connect failed: %s\n", error->message);
        g_error_free(error);
        cleanup_and_quit("[receiver] WS connect failed");
        return;
    }

    g_print("[receiver] Connected to signaling server\n");
    g_signal_connect(ws_conn, "message", G_CALLBACK(handle_server_message), NULL);
    g_signal_connect(ws_conn, "closed", G_CALLBACK(on_server_closed), NULL);

    if (!start_pipeline())
        cleanup_and_quit("[receiver] Failed to start pipeline");
}


static gboolean
on_accept_certificate(SoupMessage* msg,
    GTlsCertificate* tls_peer_certificate,
    GTlsCertificateFlags tls_errors,
    gpointer user_data)
{
    (void)msg;
    (void)tls_peer_certificate;
    (void)user_data;

    /* Якщо не просили disable-ssl — нічого не приймаємо вручну */
    if (!disable_ssl)
        return FALSE;

    g_printerr("[receiver] TLS certificate validation failed (errors=0x%x), "
        "but --disable-ssl is set, accepting certificate\n",
        (unsigned int)tls_errors);

    /* DEV ONLY: прийняти self-signed/invalid cert */
    return TRUE;
}

static void
connect_to_server_async(void)
{
    SoupSession* session = soup_session_new();
    SoupMessage* message = soup_message_new(SOUP_METHOD_GET, server_url);

    if (!message) {
        cleanup_and_quit("[receiver] Failed to create SoupMessage");
        return;
    }

    /* libsoup3: якщо сертифікат невалідний, цей сигнал дасть шанс
       прийняти його вручну (тільки якщо --disable-ssl) */
    //g_signal_connect(message, "accept-certificate",
    //    G_CALLBACK(on_accept_certificate), NULL);

    g_print("[receiver] Connecting to %s ...\n", server_url);

    soup_session_websocket_connect_async(
        session,
        message,
        NULL,                    /* origin */
        NULL,                    /* protocols */
        G_PRIORITY_DEFAULT,      /* io_priority */
        NULL,                    /* cancellable */
        (GAsyncReadyCallback)on_server_connected,
        message                  /* user_data */
    );
}

/* ---------- CLI ---------- */
static GOptionEntry entries[] = {
  {"server", 0, 0, G_OPTION_ARG_STRING, &server_url, "Signaling server URL (wss://...)", "URL"},
  {"disable-ssl", 0, 0, G_OPTION_ARG_NONE, &disable_ssl, "Disable TLS cert checks (useful for self-signed)", NULL},
  {NULL}
};

int
main(int argc, char* argv[])
{
    GOptionContext* context = g_option_context_new("- receiver (webrtcbin -> H264 decode -> display)");
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_add_group(context, gst_init_get_option_group());

    GError* error = NULL;
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("Option parsing failed: %s\n", error->message);
        g_error_free(error);
        return 1;
    }

    loop = g_main_loop_new(NULL, FALSE);

    connect_to_server_async();
    g_main_loop_run(loop);

    return 0;
}