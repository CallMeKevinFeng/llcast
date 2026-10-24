#define GST_USE_UNSTABLE_API

/*
 * whep_server.c —— 设备直连浏览器的 WHEP 服务
 *
 * 输入：stdin 上的 Annex B H.264（由 llcast_encode --pipe 输入）
 * 输出：HTTP :8080 上的播放页 + POST /whep 信令，视频经 WebRTC 直发浏览器
 *
 * 管线：appsrc ! h264parse ! rtph264pay ! webrtcbin
 *
 * 非 trickle ICE：浏览器等候选收集完再 POST offer，服务端等
 * ice-gathering-state=complete 后返回完整 answer，省掉 PATCH 流程。
 */

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>

#include <gio/gio.h>
#include <glib-unix.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    GstElement *pipeline;
    GstElement *appsrc;
    GstElement *pay;
    GstElement *webrtc;
    GMainLoop *loop;
    guint port;
    const char *page_path;
    gboolean ice_complete;
    gboolean pay_linked;
    GPtrArray *candidates;
} App;

static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data)
{
    (void)bus;
    App *app = data;
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar *dbg = NULL;
        gst_message_parse_error(msg, &err, &dbg);
        fprintf(stderr, "[bus] error: %s (%s)\n", err->message, dbg ? dbg : "");
        g_error_free(err);
        g_free(dbg);
        g_main_loop_quit(app->loop);
        break;
    }
    case GST_MESSAGE_EOS:
        g_main_loop_quit(app->loop);
        break;
    default:
        break;
    }
    return TRUE;
}

static void on_ice_notify(GObject *obj, GParamSpec *pspec, gpointer data)
{
    (void)pspec;
    App *app = data;
    GstWebRTCICEGatheringState st;
    g_object_get(obj, "ice-gathering-state", &st, NULL);
    if (st == GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE)
        app->ice_complete = TRUE;
}

static void on_ice_candidate(GstElement *webrtc, guint mline, gchar *cand,
                             gpointer data)
{
    (void)webrtc;
    App *app = data;
    fprintf(stderr, "[ice] mline=%u %s\n", mline, cand);
    g_ptr_array_add(app->candidates, g_strdup(cand));
}

static void link_pay_to_pad(App *app, GstPad *sinkpad)
{
    if (app->pay_linked)
        return;
    GstPad *srcpad = gst_element_get_static_pad(app->pay, "src");
    if (srcpad && !gst_pad_is_linked(srcpad)) {
        GstPadLinkReturn lr = gst_pad_link(srcpad, sinkpad);
        fprintf(stderr, "[whep] link %s -> rtph264pay: %d\n",
                GST_PAD_NAME(sinkpad), lr);
        if (lr == GST_PAD_LINK_OK)
            app->pay_linked = TRUE;
    }
    if (srcpad)
        gst_object_unref(srcpad);
}

static void on_pad_added(GstElement *webrtc, GstPad *pad, gpointer data)
{
    (void)webrtc;
    App *app = data;
    if (GST_PAD_DIRECTION(pad) == GST_PAD_SINK)
        link_pay_to_pad(app, pad);
}

static void try_link_pay(App *app)
{
    GstPad *sinkpad = gst_element_get_static_pad(app->webrtc, "sink_0");
    if (!sinkpad)
        sinkpad = gst_element_request_pad_simple(app->webrtc, "sink_%u");
    if (sinkpad) {
        link_pay_to_pad(app, sinkpad);
        gst_object_unref(sinkpad);
    }
}

static gboolean on_stdin(gint fd, GIOCondition cond, gpointer data)
{
    (void)cond;
    App *app = data;
    guint8 buf[65536];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
        /* 还没有观看者时直接丢弃，避免 appsrc 下游未连接报错 */
        if (!app->pay_linked)
            return TRUE;
        GstBuffer *b = gst_buffer_new_allocate(NULL, (gsize)n, NULL);
        gst_buffer_fill(b, 0, buf, (gsize)n);
        gst_app_src_push_buffer(GST_APP_SRC(app->appsrc), b);
        return TRUE;
    }
    if (n == 0) {
        fprintf(stderr, "[whep] stdin 结束\n");
        g_main_loop_quit(app->loop);
        return FALSE;
    }
    if (errno == EAGAIN || errno == EINTR)
        return TRUE;
    perror("read stdin");
    g_main_loop_quit(app->loop);
    return FALSE;
}

static gboolean build_pipeline(App *app)
{
    app->pipeline = gst_pipeline_new("whep");
    app->appsrc = gst_element_factory_make("appsrc", "src");
    GstElement *parse = gst_element_factory_make("h264parse", "parse");
    app->pay = gst_element_factory_make("rtph264pay", "pay");
    app->webrtc = gst_element_factory_make("webrtcbin", "webrtc");
    if (!app->pipeline || !app->appsrc || !parse || !app->pay || !app->webrtc) {
        fprintf(stderr, "[whep] 缺少 GStreamer 插件（appsrc/h264parse/rtph264pay/webrtcbin）\n");
        return FALSE;
    }

    GstCaps *caps = gst_caps_from_string(
        "video/x-h264,stream-format=(string)byte-stream,alignment=(string)au");
    g_object_set(app->appsrc,
                 "caps", caps,
                 "is-live", TRUE,
                 "format", GST_FORMAT_TIME,
                 "do-timestamp", TRUE,
                 NULL);
    gst_caps_unref(caps);
    g_object_set(app->pay, "config-interval", 1, "pt", 96, NULL);
    g_object_set(app->webrtc, "bundle-policy",
                 GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, NULL);

    gst_bin_add_many(GST_BIN(app->pipeline), app->appsrc, parse, app->pay,
                     app->webrtc, NULL);
    if (!gst_element_link_many(app->appsrc, parse, app->pay, NULL)) {
        fprintf(stderr, "[whep] appsrc -> h264parse -> rtph264pay 链接失败\n");
        return FALSE;
    }

    g_signal_connect(app->webrtc, "pad-added", G_CALLBACK(on_pad_added), app);

    GstPad *sinkpad = gst_element_request_pad_simple(app->webrtc, "sink_%u");
    if (!sinkpad) {
        fprintf(stderr, "[whep] 无法创建 webrtcbin sink pad\n");
        return FALSE;
    }
    GstWebRTCRTPTransceiver *tr = NULL;
    g_object_get(sinkpad, "transceiver", &tr, NULL);
    if (tr) {
        GstCaps *rtpcaps = gst_caps_from_string(
            "application/x-rtp,media=(string)video,encoding-name=(string)H264,"
            "payload=(int)96,clock-rate=(int)90000");
        g_object_set(tr, "direction",
                     GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY,
                     "codec-preferences", rtpcaps, NULL);
        gst_caps_unref(rtpcaps);
        gst_object_unref(tr);
    }
    link_pay_to_pad(app, sinkpad);
    gst_object_unref(sinkpad);

    g_signal_connect(app->webrtc, "notify::ice-gathering-state",
                     G_CALLBACK(on_ice_notify), app);
    g_signal_connect(app->webrtc, "on-ice-candidate",
                     G_CALLBACK(on_ice_candidate), app);

    GstBus *bus = gst_element_get_bus(app->pipeline);
    gst_bus_add_watch(bus, bus_call, app);
    gst_object_unref(bus);

    if (gst_element_set_state(app->pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "[whep] 管线启动失败\n");
        return FALSE;
    }
    return TRUE;
}

static gchar *whep_answer(App *app, const gchar *offer_text)
{
    g_ptr_array_set_size(app->candidates, 0);
    GstSDPMessage *offer = NULL;
    if (gst_sdp_message_new(&offer) != GST_SDP_OK ||
        gst_sdp_message_parse_buffer((const guint8 *)offer_text,
                                     (guint)strlen(offer_text), offer) != GST_SDP_OK) {
        fprintf(stderr, "[whep] offer SDP 解析失败\n");
        if (offer)
            gst_sdp_message_free(offer);
        return NULL;
    }

    GstWebRTCSessionDescription *rdesc =
        gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, offer);
    GstPromise *p = gst_promise_new();
    g_signal_emit_by_name(app->webrtc, "set-remote-description", rdesc, p);
    GstPromiseResult pres = gst_promise_wait(p);
    fprintf(stderr, "[whep] set-remote result=%d\n", pres);
    gst_promise_unref(p);
    gst_webrtc_session_description_free(rdesc);

    try_link_pay(app);

    GstPromise *ap = gst_promise_new();
    g_signal_emit_by_name(app->webrtc, "create-answer", NULL, ap);
    gst_promise_wait(ap);
    const GstStructure *reply = gst_promise_get_reply(ap);
    GstWebRTCSessionDescription *adesc = NULL;
    if (reply) {
        gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
                          &adesc, NULL);
    }
    if (!adesc && reply)
        gst_structure_get(reply, "sdp", GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
                          &adesc, NULL);
    gst_promise_unref(ap);
    if (!adesc) {
        fprintf(stderr, "[whep] create-answer 失败\n");
        return NULL;
    }

    app->ice_complete = FALSE;
    GstPromise *lp = gst_promise_new();
    g_signal_emit_by_name(app->webrtc, "set-local-description", adesc, lp);
    gst_promise_wait(lp);
    gst_promise_unref(lp);

    for (int i = 0; i < 300; i++) {
        GstWebRTCICEGatheringState st;
        g_object_get(app->webrtc, "ice-gathering-state", &st, NULL);
        if (st == GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE)
            break;
        g_usleep(10000);
        while (g_main_context_iteration(NULL, FALSE))
            ;
    }

    GstWebRTCSessionDescription *local = NULL;
    g_object_get(app->webrtc, "pending-local-description", &local, NULL);
    gchar *txt = gst_sdp_message_as_text(local ? local->sdp : adesc->sdp);
    if (local)
        gst_webrtc_session_description_free(local);
    gst_webrtc_session_description_free(adesc);

    GString *full = g_string_new(txt);
    for (guint i = 0; i < app->candidates->len; i++)
        g_string_append_printf(full, "a=%s\r\n",
                               (gchar *)g_ptr_array_index(app->candidates, i));
    g_string_append(full, "a=end-of-candidates\r\n");
    g_free(txt);
    fprintf(stderr, "[whep] answer 附带 %u 个 ICE 候选\n",
            app->candidates->len);
    return g_string_free(full, FALSE);
}

static void respond(GOutputStream *out, int code, const char *reason,
                    const char *ctype, const char *body, gsize len)
{
    gchar header[512];
    gint n = g_snprintf(header, sizeof(header),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %" G_GSIZE_FORMAT "\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Connection: close\r\n\r\n",
                        code, reason, ctype, len);
    g_output_stream_write_all(out, header, (gsize)n, NULL, NULL, NULL);
    if (body && len)
        g_output_stream_write_all(out, body, len, NULL, NULL, NULL);
}

static gboolean on_incoming(GSocketService *service, GSocketConnection *conn,
                            GObject *source, gpointer data)
{
    (void)service;
    (void)source;
    App *app = data;
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    g_socket_set_timeout(g_socket_connection_get_socket(conn), 5);

    gsize cap = 256 * 1024;
    gchar *buf = g_malloc(cap);
    gsize total = 0, body_off = 0, body_len = 0;
    gchar *hdr_end = NULL;
    while (total + 1 < cap) {
        gssize n = g_input_stream_read(in, buf + total, cap - 1 - total, NULL, NULL);
        if (n <= 0)
            break;
        total += (gsize)n;
        buf[total] = '\0';
        hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end)
            break;
    }
    if (!hdr_end) {
        respond(out, 400, "Bad Request", "text/plain", "bad request\n", 12);
        g_free(buf);
        return TRUE;
    }
    body_off = (gsize)(hdr_end - buf) + 4;
    for (gchar *line = buf; line < hdr_end;) {
        gchar *eol = strstr(line, "\r\n");
        if (!eol)
            break;
        if (!g_ascii_strncasecmp(line, "Content-Length:", 15))
            body_len = (gsize)strtoul(line + 15, NULL, 10);
        line = eol + 2;
    }
    while (total < body_off + body_len && total + 1 < cap) {
        gssize n = g_input_stream_read(in, buf + total, cap - 1 - total, NULL, NULL);
        if (n <= 0)
            break;
        total += (gsize)n;
    }
    buf[total] = '\0';

    gchar method[16] = { 0 }, path[512] = { 0 };
    sscanf(buf, "%15s %511s", method, path);
    fprintf(stderr, "[http] %s %s (%zu bytes)\n", method, path, body_len);

    if (!strcmp(method, "GET") && (!strcmp(path, "/") || !strcmp(path, "/index.html"))) {
        gchar *html = NULL;
        gsize hlen = 0;
        if (!g_file_get_contents(app->page_path, &html, &hlen, NULL)) {
            respond(out, 500, "Internal Server Error", "text/plain", "page not found\n", 16);
        } else {
            respond(out, 200, "OK", "text/html; charset=utf-8", html, hlen);
            g_free(html);
        }
    } else if (!strcmp(method, "POST") && (!strcmp(path, "/whep") || !strcmp(path, "/"))) {
        gchar *answer = whep_answer(app, buf + body_off);
        if (answer) {
            respond(out, 201, "Created", "application/sdp", answer, strlen(answer));
            g_free(answer);
        } else {
            respond(out, 500, "Internal Server Error", "text/plain", "sdp failed\n", 11);
        }
    } else {
        respond(out, 404, "Not Found", "text/plain", "not found\n", 10);
    }

    g_free(buf);
    g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
    return TRUE;
}

int main(int argc, char **argv)
{
    App app;
    memset(&app, 0, sizeof(app));
    app.port = 8080;
    app.page_path = "player/whep.html";
    app.candidates = g_ptr_array_new_with_free_func(g_free);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)
            app.port = (guint)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--page") && i + 1 < argc)
            app.page_path = argv[++i];
    }

    gst_init(&argc, &argv);
    app.loop = g_main_loop_new(NULL, FALSE);
    if (!build_pipeline(&app))
        return 1;

    g_unix_fd_add(STDIN_FILENO, G_IO_IN | G_IO_HUP, on_stdin, &app);

    GSocketService *service = g_socket_service_new();
    GError *err = NULL;
    if (!g_socket_listener_add_inet_port(G_SOCKET_LISTENER(service), app.port,
                                         NULL, &err)) {
        fprintf(stderr, "[http] 监听 %u 失败: %s\n", app.port, err->message);
        return 1;
    }
    g_signal_connect(service, "incoming", G_CALLBACK(on_incoming), &app);
    g_socket_service_start(service);

    fprintf(stderr, "[http] 打开 http://127.0.0.1:%u/ 播放（单观看者）\n", app.port);
    g_main_loop_run(app.loop);

    g_socket_service_stop(service);
    g_object_unref(service);
    gst_element_set_state(app.pipeline, GST_STATE_NULL);
    gst_object_unref(app.pipeline);
    g_ptr_array_unref(app.candidates);
    g_main_loop_unref(app.loop);
    return 0;
}
