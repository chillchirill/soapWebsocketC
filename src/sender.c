/*
 * sender.c — WebRTC sender (H.264) using GStreamer webrtcbin + libsoup signaling.
 *
 * Source base:
 *  - webrtc-sendrecv.c demo by Nirbheek Chauhan (Centricular)
 *    (same structure: libsoup websocket signaling + json-glib + webrtcbin callbacks)
 *  - Your gst-launch pipepline idea:
 *      mfvideosrc ! ... ! x264enc tune=zerolatency ... ! h264parse ! rtph264pay pt=96 ... ! (instead of udpsink) -> webrtcbin
 *
 * Build (Linux):
 *  gcc sender.c -o sender \
 *    $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 \
 *      libsoup-2.4 json-glib-1.0 glib-2.0 gobject-2.0)
 */

#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include <string.h>

#define STUN_SERVER " stun-server=stun://stun.l.google.com:19302 "
#define RTP_CAPS_H264 "application/x-rtp,media=video,encoding-name=H264,payload=96"

 /* ---------- Globals ---------- */
static GMainLoop* loop = NULL;
static GstElement* pipep = NULL;
static GstElement* webrtc = NULL;

static SoupWebsocketConnection* ws_conn = NULL;

static const gchar* server_url = "wss://108.130.0.118:8080"; /* change to your WSS */
static gboolean disable_ssl = TRUE;

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

/* ---------- Offer created callback ---------- */
static void
on_offer_created(GstPromise* promise, gpointer user_data)
{
    (void)user_data;

    const GstStructure* reply = NULL;
    GstWebRTCSessionDescription* offer = NULL;

    g_assert(gst_promise_wait(promise) == GST_PROMISE_RESULT_REPLIED);
    reply = gst_promise_get_reply(promise);
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
    gst_promise_unref(promise);

    /* Set local description */
    GstPromise* p = gst_promise_new();
    g_signal_emit_by_name(webrtc, "set-local-description", offer, p);
    gst_promise_interrupt(p);
    gst_promise_unref(p);

    /* Send to peer */
    g_print("[sender] Sending SDP offer\n");
    send_sdp(offer);

    gst_webrtc_session_description_free(offer);
}

/* ---------- Negotiation needed (sender always creates offer) ---------- */
static void
on_negotiation_needed(GstElement* element, gpointer user_data)
{
    (void)element;
    (void)user_data;

    g_print("[sender] on-negotiation-needed -> create-offer\n");
    GstPromise* promise = gst_promise_new_with_change_func(on_offer_created, NULL, NULL);
    g_signal_emit_by_name(webrtc, "create-offer", NULL, promise);
}

/* ---------- Parse incoming messages (we ignore offers, we only accept answer + ICE) ---------- */
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

    /* SDP? */
    if (json_object_has_member(obj, "sdp")) {
        JsonObject* sdpobj = json_object_get_object_member(obj, "sdp");
        const gchar* sdptype = json_object_get_string_member(sdpobj, "type");
        const gchar* sdptext = json_object_get_string_member(sdpobj, "sdp");

        /* Sender expects ANSWER */
        if (g_strcmp0(sdptype, "answer") == 0) {
            GstSDPMessage* sdp = NULL;
            gst_sdp_message_new(&sdp);
            gst_sdp_message_parse_buffer((guint8*)sdptext, strlen(sdptext), sdp);

            GstWebRTCSessionDescription* answer =
                gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);

            g_print("[sender] Received SDP answer -> set-remote-description\n");
            GstPromise* p = gst_promise_new();
            g_signal_emit_by_name(webrtc, "set-remote-description", answer, p);
            gst_promise_interrupt(p);
            gst_promise_unref(p);

            gst_webrtc_session_description_free(answer);
        }
    }
    /* ICE? */
    else if (json_object_has_member(obj, "ice")) {
        JsonObject* ice = json_object_get_object_member(obj, "ice");
        const gchar* candidate = json_object_get_string_member(ice, "candidate");
        gint mline = json_object_get_int_member(ice, "sdpMLineIndex");

        g_signal_emit_by_name(webrtc, "add-ice-candidate", mline, candidate);
    }

    g_object_unref(parser);
    g_free(text);
}

/* ---------- Create sender pipeline ---------- */



//ЗДЕСЯ РАБОТАЕМ
static gboolean
start_pipeline(void)
{
    GError* error = NULL;

    /* webrtcbin first, then send RTP into sendrecv. */
    pipep = gst_parse_launch(
        "webrtcbin name=sendrecv bundle-policy=max-bundle latency=20 "
        "mfvideosrc do-timestamp=true ! "
        "video/x-raw,width=640,height=360,framerate=30/1 ! "
        "queue max-size-buffers=2 max-size-time=0 max-size-bytes=0 leaky=downstream ! "
        "videoconvert ! video/x-raw,format=I420 ! "
        "x264enc tune=zerolatency speed-preset=ultrafast bitrate=1500 "
        "key-int-max=15 bframes=0 byte-stream=true aud=false ! "
        "h264parse config-interval=1 ! "
        "rtph264pay pt=96 config-interval=1 aggregate-mode=zero-latency ! "
        "application/x-rtp,media=video,encoding-name=H264,payload=96 ! "
        "sendrecv.",
        &error);

    if (error) {
        g_printerr("[sender] Failed to parse pipeline: %s\n", error->message);
        g_error_free(error);
        return FALSE;
    }

    if (!GST_IS_BIN(pipep)) {
        g_printerr("[sender] Parsed pipeline is not a bin/pipeline\n");
        return FALSE;
    }

    webrtc = gst_bin_get_by_name(GST_BIN(pipep), "sendrecv");
    if (!webrtc) {
        g_printerr("[sender] Failed to get webrtcbin by name 'sendrecv'\n");
        return FALSE;
    }

    g_object_set(webrtc,
        "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE,
        "latency", 20,
        NULL);

    g_signal_connect(webrtc, "on-negotiation-needed", G_CALLBACK(on_negotiation_needed), NULL);
    g_signal_connect(webrtc, "on-ice-candidate", G_CALLBACK(send_ice_candidate), NULL);

    if (gst_element_set_state(pipep, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        g_printerr("[sender] Failed to set pipeline to PLAYING\n");
        return FALSE;
    }

    g_print("[sender] pipeline started (VP8)\n");
    return TRUE;
}
/* ---------- WebSocket connect ---------- */
static void
on_server_closed(SoupWebsocketConnection* conn, gpointer user_data)
{
    (void)conn;
    (void)user_data;
    cleanup_and_quit("[sender] Server closed");
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
        cleanup_and_quit("[sender] WS connect failed");
        return;
    }

    g_print("[sender] Connected to signaling server\n");
    g_signal_connect(ws_conn, "message", G_CALLBACK(handle_server_message), NULL);
    g_signal_connect(ws_conn, "closed", G_CALLBACK(on_server_closed), NULL);

    /* Start media after WS is up (simple + predictable) */
    if (!start_pipeline())
        cleanup_and_quit("[sender] Failed to start pipeline");
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

    g_printerr("[sender] TLS certificate validation failed (errors=0x%x), "
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
        cleanup_and_quit("[sender] Failed to create SoupMessage");
        return;
    }

    /* libsoup3: якщо сертифікат невалідний, цей сигнал дасть шанс
       прийняти його вручну (тільки якщо --disable-ssl) */
    //g_signal_connect(message, "accept-certificate",
    //    G_CALLBACK(on_accept_certificate), NULL);

    g_print("[sender] Connecting to %s ...\n", server_url);

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
    GOptionContext* context = g_option_context_new("- sender (H.264 -> webrtcbin)");
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