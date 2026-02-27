#include <libsoup/soup.h>
#include <glib.h>
#include <stdio.h>

static void on_ws_message(SoupWebsocketConnection* conn,
    gint type,
    GBytes* message,
    gpointer user_data)
{
    gsize size = 0;
    const guint8* data = g_bytes_get_data(message, &size);

    if (type == SOUP_WEBSOCKET_DATA_TEXT) {
        // docs: message is NUL-terminated as convenience, but size excludes NUL
        printf("TEXT (%zu): %.*s\n", size, (int)size, (const char*)data);
    }
    else if (type == SOUP_WEBSOCKET_DATA_BINARY) {
        printf("BINARY (%zu bytes)\n", size);
    }
}

static void on_ws_closed(SoupWebsocketConnection* conn, gpointer user_data)
{
    printf("WebSocket closed\n");
    GMainLoop* loop = user_data;
    g_main_loop_quit(loop);
}

static void on_ws_error(SoupWebsocketConnection* conn, GError* error, gpointer user_data)
{
    fprintf(stderr, "WebSocket error: %s\n", error ? error->message : "unknown");
}

static void on_connect_finished(GObject* source_object, GAsyncResult* res, gpointer user_data)
{
    GMainLoop* loop = user_data;
    SoupSession* session = SOUP_SESSION(source_object);
    GError* error = NULL;

    SoupWebsocketConnection* conn =
        soup_session_websocket_connect_finish(session, res, &error);

    if (!conn) {
        fprintf(stderr, "Connect failed: %s\n", error->message);
        g_clear_error(&error);
        g_main_loop_quit(loop);
        return;
    }

    printf("Connected!\n");

    // Optional keepalive
    soup_websocket_connection_set_keepalive_interval(conn, 15);

    g_signal_connect(conn, "message", G_CALLBACK(on_ws_message), NULL);
    g_signal_connect(conn, "closed", G_CALLBACK(on_ws_closed), loop);
    g_signal_connect(conn, "error", G_CALLBACK(on_ws_error), NULL);

    soup_websocket_connection_send_text(conn, "Hello from libsoup client");
}

int main(void)
{
    // For older GLib versions you might call g_type_init(), but modern GLib doesn't need it.
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);

    SoupSession* session = soup_session_new();
    SoupMessage* msg = soup_message_new("GET", "wss://108.130.0.118:8080");

    // Optional subprotocols (NULL-terminated array)
    // char *protocols[] = { "chat", NULL };

    soup_session_websocket_connect_async(
        session,
        msg,
        NULL,      // origin
        NULL,      // protocols
        G_PRIORITY_DEFAULT,
        NULL,      // cancellable
        on_connect_finished,
        loop
    );

    g_main_loop_run(loop);

    g_object_unref(msg);
    g_object_unref(session);
    g_main_loop_unref(loop);
    return 0;
}