/*
 * SPARCcord - Discord client for Solaris 7 / CDE / SPARCstation
 *
 * Motif/X11 native client that talks to a discord-bridge server
 * via HTTP/1.0. Displays channels, messages, presence, and images
 * in a classic 3-pane layout.
 *
 * Compile:
 *   make
 *
 * Run:
 *   DISPLAY=:0 ./sparccord
 *
 * Configuration: ~/.sparccordrc
 *   server=10.0.2.2
 *   port=3002
 *   poll_ms=2000
 *   image_colors=16
 *   image_maxwidth=320
 */

#include <Xm/Xm.h>
#include <Xm/MainW.h>
#include <Xm/Form.h>
#include <Xm/PanedW.h>
#include <Xm/Label.h>
#include <Xm/LabelG.h>
#include <Xm/PushB.h>
#include <Xm/CascadeB.h>
#include <Xm/RowColumn.h>
#include <Xm/Separator.h>
#include <Xm/Text.h>
#include <Xm/TextF.h>
#include <Xm/List.h>
#include <Xm/ScrolledW.h>
#include <Xm/MessageB.h>
#include <Xm/DrawingA.h>
#include <Xm/Frame.h>
#include <Xm/ToggleB.h>
#include <Xm/Scale.h>
#include <X11/Xlib.h>
#include <X11/Shell.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  /* strcasecmp on Solaris */
#include <time.h>

#include "http.h"
#include "json.h"
#include "gifload.h"
#include "icon48.h"

/* Forward declarations */
static void truncate_name(char *buf, int maxlen);

/* ================================================================
 * CONFIGURATION
 * ================================================================ */

static char cfg_server[256] = "10.0.2.2";
static int  cfg_port = 3002;
static int  cfg_poll_ms = 2000;
static int  cfg_image_colors = 16;
static int  cfg_image_maxwidth = 320;
static int  cfg_image_size = 1;  /* 0=Small(160) 1=Medium(320) 2=Large(480) 3=Full(640) */

static void load_config(void)
{
    char path[512];
    char line[512];
    char *home;
    FILE *f;

    home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(path, sizeof(path), "%s/.sparccordrc", home);

    f = fopen(path, "r");
    if (!f) return;

    while (fgets(line, sizeof(line), f)) {
        char *eq;
        /* Skip comments and blank lines */
        if (line[0] == '#' || line[0] == '\n') continue;
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        eq++;
        /* Trim trailing newline */
        { char *nl = strchr(eq, '\n'); if (nl) *nl = '\0'; }

        if (strcmp(line, "server") == 0) {
            strncpy(cfg_server, eq, sizeof(cfg_server) - 1);
        } else if (strcmp(line, "port") == 0) {
            cfg_port = atoi(eq);
        } else if (strcmp(line, "poll_ms") == 0) {
            cfg_poll_ms = atoi(eq);
        } else if (strcmp(line, "image_colors") == 0) {
            cfg_image_colors = atoi(eq);
        } else if (strcmp(line, "image_maxwidth") == 0) {
            cfg_image_maxwidth = atoi(eq);
        } else if (strcmp(line, "image_size") == 0) {
            cfg_image_size = atoi(eq);
            if (cfg_image_size < 0) cfg_image_size = 0;
            if (cfg_image_size > 3) cfg_image_size = 3;
        }
    }
    fclose(f);
}

/* ================================================================
 * APPLICATION STATE
 * ================================================================ */

typedef struct {
    char id[32];
    char name[128];
    int  unread_count;
    int  mention_count;
} guild_info_t;

typedef struct {
    char id[32];
    char name[128];
    int  type;
    int  position;
    int  unread;
    int  mention_count;
} channel_info_t;

#define MAX_GUILDS 100
#define MAX_CHANNELS 200

static guild_info_t   guilds[MAX_GUILDS];
static int            n_guilds = 0;
static int            current_guild_idx = -1;

static channel_info_t channels[MAX_CHANNELS];
static int            n_channels = 0;
static int            current_channel_idx = -1;

static char last_message_id[32] = "";  /* Snowflake of last seen message */

/* Image URL ring buffer — tracks viewable images from messages */
#define MAX_IMAGE_URLS 100
typedef struct {
    char url[1024];
    char label[128];    /* e.g. "image.png" or "embed thumbnail" */
    int  index;         /* display index shown in message text */
} image_entry_t;

static image_entry_t image_urls[MAX_IMAGE_URLS];
static int n_image_urls = 0;
static int image_url_next = 0;  /* next slot in ring buffer */

/* Per-popup image data (heap-allocated per popup window) */
typedef struct {
    Pixmap pixmap;
    int    width;
    int    height;
} popup_data_t;

/* Self (logged-in user) name for echo */
static char self_username[128] = "you";

/* DM mode flag: 1 = viewing DMs, 0 = viewing a server */
static int dm_mode = 0;

/* ================================================================
 * WIDGETS
 * ================================================================ */

static XtAppContext   app_context;
static Widget         top_shell;
static Widget         main_window;
static Widget         menu_bar;
static Widget         server_menu;
static Widget         server_pulldown;
static Widget         channel_list;
static Widget         message_text;
static Widget         input_text;
static Widget         member_list;
static Widget         status_label;
static Widget         form;
static XtIntervalId   poll_timer = 0;
static XtIntervalId   presence_timer = 0;

/* ================================================================
 * BRIDGE API HELPERS
 * ================================================================ */

static json_value_t *api_get(const char *path)
{
    http_response_t resp;
    json_value_t *result;

    if (http_get(cfg_server, cfg_port, path, &resp) < 0) {
        return NULL;
    }

    if (resp.status_code != 200 || !resp.body) {
        http_response_free(&resp);
        return NULL;
    }

    result = json_parse(resp.body);
    http_response_free(&resp);
    return result;
}

static json_value_t *api_post(const char *path, const char *json_body)
{
    http_response_t resp;
    json_value_t *result;

    if (http_post_json(cfg_server, cfg_port, path, json_body, &resp) < 0) {
        return NULL;
    }

    result = json_parse(resp.body);
    http_response_free(&resp);
    return result;
}

/* ================================================================
 * FETCH AND DISPLAY GUILDS (servers)
 * ================================================================ */

static void fetch_guilds(void)
{
    json_value_t *arr = api_get("/api/guilds");
    int i, len;
    Widget *buttons;
    Cardinal n_old;

    if (!arr) return;

    len = json_array_len(arr);
    n_guilds = 0;

    for (i = 0; i < len && n_guilds < MAX_GUILDS; i++) {
        json_value_t *g = json_array_get(arr, i);
        const char *id, *name;
        if (!g) continue;
        id = json_get_str(g, "id");
        name = json_get_str(g, "name");
        if (!id || !name) continue;

        strncpy(guilds[n_guilds].id, id, 31);
        guilds[n_guilds].id[31] = '\0';
        strncpy(guilds[n_guilds].name, name, 127);
        guilds[n_guilds].name[127] = '\0';
        guilds[n_guilds].unread_count = json_get_int(g, "unreadCount");
        guilds[n_guilds].mention_count = json_get_int(g, "mentionCount");
        n_guilds++;
    }

    json_free(arr);

    /* Rebuild server pulldown menu */
    /* First remove old children */
    XtVaGetValues(server_pulldown, XmNnumChildren, &n_old, NULL);
    if (n_old > 0) {
        XtVaGetValues(server_pulldown, XmNchildren, &buttons, NULL);
        /* Unmanage and destroy old buttons */
        for (i = 0; i < (int)n_old; i++) {
            XtDestroyWidget(buttons[i]);
        }
    }

    /* Add "Direct Messages" entry at the top */
    {
        Widget btn;
        XmString label = XmStringCreateLocalized("Direct Messages");
        btn = XtVaCreateManagedWidget("dmBtn",
            xmPushButtonWidgetClass, server_pulldown,
            XmNlabelString, label,
            NULL);
        XmStringFree(label);
    }

    /* Separator between DMs and servers */
    XtVaCreateManagedWidget("sep",
        xmSeparatorWidgetClass, server_pulldown, NULL);

    /* Add new guild buttons */
    for (i = 0; i < n_guilds; i++) {
        Widget btn;
        XmString label;
        char buf[160];

        if (guilds[i].mention_count > 0) {
            snprintf(buf, sizeof(buf), "%s (%d)", guilds[i].name, guilds[i].mention_count);
        } else if (guilds[i].unread_count > 0) {
            snprintf(buf, sizeof(buf), "%s *", guilds[i].name);
        } else {
            strncpy(buf, guilds[i].name, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
        }

        label = XmStringCreateLocalized(buf);
        btn = XtVaCreateManagedWidget("guildBtn",
            xmPushButtonWidgetClass, server_pulldown,
            XmNlabelString, label,
            XmNuserData, (XtPointer)(long)i,
            NULL);
        XmStringFree(label);

        /* Callback set below after function declaration */
    }
}

/* Forward declarations */
static void guild_select_cb(Widget w, XtPointer client, XtPointer call);
static void dm_select_cb(Widget w, XtPointer client, XtPointer call);
static void fetch_channels(const char *guild_id);
static void fetch_dm_channels(void);
static void channel_select_cb(Widget w, XtPointer client, XtPointer call);
static void fetch_messages(const char *channel_id, int initial);
static void send_message_cb(Widget w, XtPointer client, XtPointer call);
static void poll_messages_cb(XtPointer client, XtIntervalId *id);
static void poll_presence_cb(XtPointer client, XtIntervalId *id);
static void show_image(const char *url, const char *title);

/* Rebuild guild buttons with callbacks.
 * Child 0 = "Direct Messages" (dm_select_cb),
 * Child 1 = separator (skip),
 * Children 2..n = guilds (guild_select_cb with index). */
static void rebuild_guild_menu(void)
{
    int i;
    Cardinal n_old;
    Widget *children;

    XtVaGetValues(server_pulldown, XmNnumChildren, &n_old,
                  XmNchildren, &children, NULL);

    for (i = 0; i < (int)n_old; i++) {
        if (i == 0) {
            /* First entry is "Direct Messages" */
            XtAddCallback(children[i], XmNactivateCallback,
                          dm_select_cb, NULL);
        } else if (i >= 2) {
            /* Skip separator at index 1; guild index = i - 2 */
            XtAddCallback(children[i], XmNactivateCallback,
                          guild_select_cb, (XtPointer)(long)(i - 2));
        }
    }
}

/* ================================================================
 * GUILD SELECTION
 * ================================================================ */

static void guild_select_cb(Widget w, XtPointer client, XtPointer call)
{
    int idx = (int)(long)client;
    char title[256];

    (void)w; (void)call;

    if (idx < 0 || idx >= n_guilds) return;
    current_guild_idx = idx;
    current_channel_idx = -1;
    last_message_id[0] = '\0';
    dm_mode = 0;

    /* Update window title */
    snprintf(title, sizeof(title), "SPARCcord - %s", guilds[idx].name);
    XtVaSetValues(top_shell, XmNtitle, title, NULL);

    /* Clear message area */
    XmTextSetString(message_text, "");

    /* Fetch channels for this guild */
    fetch_channels(guilds[idx].id);
}

/* ================================================================
 * DM SELECTION
 * ================================================================ */

static void dm_select_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)w; (void)client; (void)call;

    dm_mode = 1;
    current_guild_idx = -1;
    current_channel_idx = -1;
    last_message_id[0] = '\0';

    XtVaSetValues(top_shell, XmNtitle, "SPARCcord - Direct Messages", NULL);

    /* Clear message area and member list */
    XmTextSetString(message_text, "");
    XmListDeleteAllItems(member_list);

    /* Fetch DM channels */
    fetch_dm_channels();
}

/* ================================================================
 * FETCH DM CHANNELS
 * ================================================================ */

static void fetch_dm_channels(void)
{
    json_value_t *arr;
    int i, len;

    arr = api_get("/api/channels?guild=dm");
    if (!arr) return;

    len = json_array_len(arr);
    n_channels = 0;

    XmListDeleteAllItems(channel_list);

    for (i = 0; i < len && n_channels < MAX_CHANNELS; i++) {
        json_value_t *ch = json_array_get(arr, i);
        const char *id, *name;
        XmString item;

        if (!ch) continue;
        id = json_get_str(ch, "id");
        name = json_get_str(ch, "name");
        if (!id || !name) continue;

        strncpy(channels[n_channels].id, id, 31);
        channels[n_channels].id[31] = '\0';
        strncpy(channels[n_channels].name, name, 127);
        channels[n_channels].name[127] = '\0';
        channels[n_channels].type = json_get_int(ch, "type");
        channels[n_channels].position = 0;
        channels[n_channels].unread = json_get_bool(ch, "unread");
        channels[n_channels].mention_count = json_get_int(ch, "mentionCount");

        /* DMs don't get # prefix — show name directly, truncated */
        {
            char trunc[128];
            strncpy(trunc, name, sizeof(trunc) - 1);
            trunc[sizeof(trunc) - 1] = '\0';
            truncate_name(trunc, 30);
            item = XmStringCreateLocalized(trunc);
        }
        XmListAddItem(channel_list, item, 0);
        XmStringFree(item);

        n_channels++;
    }

    json_free(arr);

    /* Update status */
    {
        char buf[64];
        XmString s;
        snprintf(buf, sizeof(buf), "Direct Messages (%d)", n_channels);
        s = XmStringCreateLocalized(buf);
        XtVaSetValues(status_label, XmNlabelString, s, NULL);
        XmStringFree(s);
    }
}

/* ================================================================
 * FETCH AND DISPLAY CHANNELS
 * ================================================================ */

static void fetch_channels(const char *guild_id)
{
    char path[256];
    json_value_t *arr;
    int i, len;

    snprintf(path, sizeof(path), "/api/channels?guild=%s", guild_id);
    arr = api_get(path);
    if (!arr) return;

    len = json_array_len(arr);
    n_channels = 0;

    /* Clear the list first */
    XmListDeleteAllItems(channel_list);

    for (i = 0; i < len && n_channels < MAX_CHANNELS; i++) {
        json_value_t *ch = json_array_get(arr, i);
        const char *id, *name;
        XmString item;
        char buf[160];

        if (!ch) continue;
        id = json_get_str(ch, "id");
        name = json_get_str(ch, "name");
        if (!id || !name) continue;

        strncpy(channels[n_channels].id, id, 31);
        channels[n_channels].id[31] = '\0';
        strncpy(channels[n_channels].name, name, 127);
        channels[n_channels].name[127] = '\0';
        channels[n_channels].type = json_get_int(ch, "type");
        channels[n_channels].position = json_get_int(ch, "position");
        channels[n_channels].unread = json_get_bool(ch, "unread");
        channels[n_channels].mention_count = json_get_int(ch, "mentionCount");

        /* Format: #channel-name or #channel-name (3) for mentions */
        if (channels[n_channels].mention_count > 0) {
            snprintf(buf, sizeof(buf), "#%s (%d)", name,
                     channels[n_channels].mention_count);
        } else if (channels[n_channels].unread) {
            snprintf(buf, sizeof(buf), "#%s *", name);
        } else {
            snprintf(buf, sizeof(buf), "#%s", name);
        }
        truncate_name(buf, 30);

        item = XmStringCreateLocalized(buf);
        XmListAddItem(channel_list, item, 0);
        XmStringFree(item);

        n_channels++;
    }

    json_free(arr);
}

/* ================================================================
 * CHANNEL SELECTION
 * ================================================================ */

static void channel_select_cb(Widget w, XtPointer client, XtPointer call)
{
    XmListCallbackStruct *cbs = (XmListCallbackStruct *)call;
    int idx;

    (void)w; (void)client;

    idx = cbs->item_position - 1;  /* XmList is 1-based */
    if (idx < 0 || idx >= n_channels) return;

    current_channel_idx = idx;
    last_message_id[0] = '\0';

    /* Clear message area */
    XmTextSetString(message_text, "");

    /* Update status */
    {
        XmString s;
        char buf[256];
        if (dm_mode) {
            snprintf(buf, sizeof(buf), "DM: %s", channels[idx].name);
        } else {
            snprintf(buf, sizeof(buf), "Channel: #%s", channels[idx].name);
        }
        s = XmStringCreateLocalized(buf);
        XtVaSetValues(status_label, XmNlabelString, s, NULL);
        XmStringFree(s);
    }

    /* In DM mode, populate member list with DM participants */
    if (dm_mode) {
        char mpath[256];
        json_value_t *marr;
        int mi, mlen;

        XmListDeleteAllItems(member_list);

        snprintf(mpath, sizeof(mpath), "/api/presence?channel=%s",
                 channels[idx].id);
        marr = api_get(mpath);
        if (marr) {
            mlen = json_array_len(marr);
            for (mi = 0; mi < mlen && mi < 50; mi++) {
                json_value_t *m = json_array_get(marr, mi);
                const char *mname = json_get_str(m, "username");
                const char *mstatus = json_get_str(m, "status");
                char mbuf[200];
                const char *icon;
                XmString mitem;

                if (!mname) continue;
                if (!mstatus) mstatus = "offline";
                if (strcmp(mstatus, "online") == 0) icon = "+";
                else if (strcmp(mstatus, "idle") == 0) icon = "~";
                else if (strcmp(mstatus, "dnd") == 0) icon = "-";
                else icon = " ";

                snprintf(mbuf, sizeof(mbuf), "%s %s", icon, mname);
                mitem = XmStringCreateLocalized(mbuf);
                XmListAddItem(member_list, mitem, 0);
                XmStringFree(mitem);
            }
            json_free(marr);
        }
    }

    /* Fetch messages */
    fetch_messages(channels[idx].id, 1);
}

/* ================================================================
 * FETCH AND DISPLAY MESSAGES
 * ================================================================ */

static void append_text(const char *text)
{
    XmTextPosition pos = XmTextGetLastPosition(message_text);
    XmTextInsert(message_text, pos, (char *)text);
    /* Scroll to bottom */
    XmTextSetInsertionPosition(message_text, XmTextGetLastPosition(message_text));
    XmTextShowPosition(message_text, XmTextGetLastPosition(message_text));
}

static void format_timestamp(const char *iso_ts, char *buf, int buflen)
{
    /* Just extract HH:MM from ISO timestamp */
    /* Format: "2024-01-15T12:34:56.789000+00:00" */
    const char *t = strchr(iso_ts, 'T');
    if (t && strlen(t) >= 6) {
        snprintf(buf, buflen, "%.5s", t + 1);
    } else {
        strncpy(buf, "??:??", buflen - 1);
        buf[buflen - 1] = '\0';
    }
}

/* ================================================================
 * TEXT BUFFERING
 * Buffer text during fetch_messages to minimize widget redraws.
 * On a 110MHz SPARC, reducing 100+ XmText operations to 1 is
 * critical for responsiveness.
 * ================================================================ */

static char msg_buf[65536];
static int  msg_buf_pos = 0;

static void msg_buf_reset(void)
{
    msg_buf_pos = 0;
    msg_buf[0] = '\0';
}

static void msg_buf_append(const char *text)
{
    int len = strlen(text);
    if (msg_buf_pos + len >= (int)sizeof(msg_buf) - 1) return;
    memcpy(msg_buf + msg_buf_pos, text, len);
    msg_buf_pos += len;
    msg_buf[msg_buf_pos] = '\0';
}

static void msg_buf_flush(void)
{
    if (msg_buf_pos > 0) {
        append_text(msg_buf);
        msg_buf_reset();
    }
}

/* ================================================================
 * NAME TRUNCATION HELPER
 * ================================================================ */

static void truncate_name(char *buf, int maxlen)
{
    if ((int)strlen(buf) > maxlen) {
        buf[maxlen - 3] = '.';
        buf[maxlen - 2] = '.';
        buf[maxlen - 1] = '.';
        buf[maxlen] = '\0';
    }
}

/* Check if a URL ends with an image extension */
static int is_image_url(const char *url)
{
    const char *dot;
    const char *q;
    char ext[8];
    int len;

    /* Find last dot before any query string */
    q = strchr(url, '?');
    dot = NULL;
    {
        const char *p = url;
        while (*p && p != q) {
            if (*p == '.') dot = p;
            p++;
        }
    }
    if (!dot) return 0;
    len = (q ? (int)(q - dot) : (int)strlen(dot));
    if (len < 2 || len > 6) return 0;
    strncpy(ext, dot, len);
    ext[len] = '\0';

    return (strcasecmp(ext, ".png") == 0 ||
            strcasecmp(ext, ".jpg") == 0 ||
            strcasecmp(ext, ".jpeg") == 0 ||
            strcasecmp(ext, ".gif") == 0 ||
            strcasecmp(ext, ".webp") == 0);
}

/* Scan message content for http(s) URLs that look like images,
 * register them in the ring buffer, and append [IMG #N] markers.
 * Returns the number of image URLs found. */
static int scan_content_for_images(const char *content)
{
    const char *p = content;
    int found = 0;

    while (*p) {
        /* Find http:// or https:// */
        if ((p[0] == 'h' && p[1] == 't' && p[2] == 't' && p[3] == 'p') &&
            (p[4] == ':' || (p[4] == 's' && p[5] == ':'))) {
            const char *start = p;
            const char *end;
            char url_buf[1024];
            int url_len;

            /* Skip to end of URL (whitespace or end of string) */
            while (*p && *p != ' ' && *p != '\n' && *p != '\r' &&
                   *p != '>' && *p != ')') p++;
            end = p;
            url_len = (int)(end - start);
            if (url_len > 0 && url_len < (int)sizeof(url_buf)) {
                strncpy(url_buf, start, url_len);
                url_buf[url_len] = '\0';

                if (is_image_url(url_buf)) {
                    int idx = image_url_next % MAX_IMAGE_URLS;
                    char line[256];

                    strncpy(image_urls[idx].url, url_buf, 1023);
                    image_urls[idx].url[1023] = '\0';
                    strncpy(image_urls[idx].label, "linked image", 127);
                    image_urls[idx].label[127] = '\0';
                    image_urls[idx].index = image_url_next;
                    image_url_next++;
                    if (n_image_urls < MAX_IMAGE_URLS) n_image_urls++;

                    snprintf(line, sizeof(line),
                             "        [IMG #%d: linked image]\n", idx);
                    msg_buf_append(line);

                    found++;
                }
            }
        } else {
            p++;
        }
    }
    return found;
}

static void fetch_messages(const char *channel_id, int initial)
{
    char path[512];
    json_value_t *arr;
    int i, len;

    if (initial) {
        snprintf(path, sizeof(path), "/api/messages?channel=%s&limit=50",
                 channel_id);
    } else {
        snprintf(path, sizeof(path),
                 "/api/messages?channel=%s&after=%s&limit=50",
                 channel_id, last_message_id);
    }

    arr = api_get(path);
    if (!arr) return;

    msg_buf_reset();
    len = json_array_len(arr);

    for (i = 0; i < len; i++) {
        json_value_t *msg = json_array_get(arr, i);
        const char *id, *content, *timestamp;
        json_value_t *author;
        const char *author_name;
        char ts_buf[16];
        char line[2048];
        json_value_t *ref;
        json_value_t *attachments;
        int j, n_attach;

        if (!msg) continue;

        id = json_get_str(msg, "id");
        content = json_get_str(msg, "content");
        timestamp = json_get_str(msg, "timestamp");
        author = json_get(msg, "author");
        author_name = author ? json_get_str(author, "name") : "???";
        if (!author_name) author_name = "???";
        if (!content) content = "";
        if (!id) continue;

        /* Update last seen message ID */
        strncpy(last_message_id, id, 31);
        last_message_id[31] = '\0';

        /* Format timestamp */
        if (timestamp) {
            format_timestamp(timestamp, ts_buf, sizeof(ts_buf));
        } else {
            strcpy(ts_buf, "??:??");
        }

        /* Show reply context if present */
        ref = json_get(msg, "referencedMessage");
        if (ref && ref->type == JSON_OBJECT) {
            const char *ref_author = json_get_str(ref, "author");
            const char *ref_content = json_get_str(ref, "content");
            if (ref_author && ref_content) {
                snprintf(line, sizeof(line), "  >> %s: %s\n",
                         ref_author, ref_content);
                msg_buf_append(line);
            }
        }

        /* Format: [HH:MM] <nick> message */
        snprintf(line, sizeof(line), "[%s] <%s> %s\n",
                 ts_buf, author_name, content);
        msg_buf_append(line);

        /* Scan content text for image URLs */
        if (content[0]) {
            scan_content_for_images(content);
        }

        /* Show attachments */
        attachments = json_get(msg, "attachments");
        n_attach = json_array_len(attachments);
        for (j = 0; j < n_attach; j++) {
            json_value_t *att = json_array_get(attachments, j);
            const char *filename = json_get_str(att, "filename");
            const char *url = json_get_str(att, "url");

            if (filename && url) {
                /* Check if it's an image by extension */
                const char *ext = strrchr(filename, '.');
                int is_image = 0;
                if (ext) {
                    if (strcasecmp(ext, ".png") == 0 ||
                        strcasecmp(ext, ".jpg") == 0 ||
                        strcasecmp(ext, ".jpeg") == 0 ||
                        strcasecmp(ext, ".gif") == 0 ||
                        strcasecmp(ext, ".webp") == 0) {
                        is_image = 1;
                    }
                }

                if (is_image) {
                    /* Track this image URL */
                    int idx = image_url_next % MAX_IMAGE_URLS;
                    strncpy(image_urls[idx].url, url, 1023);
                    image_urls[idx].url[1023] = '\0';
                    strncpy(image_urls[idx].label, filename, 127);
                    image_urls[idx].label[127] = '\0';
                    image_urls[idx].index = image_url_next;
                    image_url_next++;
                    if (n_image_urls < MAX_IMAGE_URLS) n_image_urls++;

                    snprintf(line, sizeof(line),
                             "        [IMG #%d: %s]\n", idx, filename);
                } else {
                    snprintf(line, sizeof(line),
                             "        [Attachment: %s]\n", filename);
                }
                msg_buf_append(line);
            } else if (filename) {
                snprintf(line, sizeof(line),
                         "        [Attachment: %s]\n", filename);
                msg_buf_append(line);
            }
        }

        /* Show embeds */
        {
            json_value_t *embeds = json_get(msg, "embeds");
            int n_embeds = json_array_len(embeds);
            for (j = 0; j < n_embeds; j++) {
                json_value_t *emb = json_array_get(embeds, j);
                const char *title = json_get_str(emb, "title");
                const char *desc = json_get_str(emb, "description");

                /* Track embed images */
                {
                    json_value_t *thumb = json_get(emb, "thumbnail");
                    json_value_t *image_obj = json_get(emb, "image");
                    const char *img_url = NULL;
                    const char *img_label = "embed";

                    if (image_obj) {
                        img_url = json_get_str(image_obj, "url");
                        if (!img_url) img_url = json_get_str(image_obj, "proxy_url");
                        img_label = "embed image";
                    } else if (thumb) {
                        img_url = json_get_str(thumb, "url");
                        if (!img_url) img_url = json_get_str(thumb, "proxy_url");
                        img_label = "thumbnail";
                    }

                    if (img_url) {
                        int idx = image_url_next % MAX_IMAGE_URLS;
                        strncpy(image_urls[idx].url, img_url, 1023);
                        image_urls[idx].url[1023] = '\0';
                        strncpy(image_urls[idx].label, img_label, 127);
                        image_urls[idx].label[127] = '\0';
                        image_urls[idx].index = image_url_next;
                        image_url_next++;
                        if (n_image_urls < MAX_IMAGE_URLS) n_image_urls++;

                        snprintf(line, sizeof(line),
                                 "        [IMG #%d: %s]\n", idx, img_label);
                        msg_buf_append(line);
                    }
                }

                if (title) {
                    snprintf(line, sizeof(line), "        [%s]\n", title);
                    msg_buf_append(line);
                }
                if (desc) {
                    snprintf(line, sizeof(line), "        %s\n", desc);
                    msg_buf_append(line);
                }
            }
        }
    }

    msg_buf_flush();
    json_free(arr);
}

/* ================================================================
 * IMAGE VIEWER
 * ================================================================ */

static void image_expose_cb(Widget w, XtPointer client, XtPointer call)
{
    popup_data_t *pd = (popup_data_t *)client;
    Display *dpy = XtDisplay(w);
    Window win = XtWindow(w);
    GC gc;

    (void)call;

    if (!pd || pd->pixmap == None) return;

    gc = XCreateGC(dpy, win, 0, NULL);
    XCopyArea(dpy, pd->pixmap, win, gc, 0, 0,
              pd->width, pd->height, 0, 0);
    XFreeGC(dpy, gc);
}

static void image_close_cb(Widget w, XtPointer client, XtPointer call)
{
    popup_data_t *pd = (popup_data_t *)client;
    Widget shell;
    (void)call;

    /* Walk up: button -> form -> shell */
    shell = XtParent(XtParent(w));

    /* Destroy popup window first (before freeing pixmap data) */
    XtDestroyWidget(shell);

    if (pd) {
        if (pd->pixmap != None) {
            XFreePixmap(XtDisplay(top_shell), pd->pixmap);
        }
        free(pd);
    }
}

static void show_image(const char *url, const char *title)
{
    char path[2048];
    http_response_t resp;
    gif_image_t img;
    Widget shell, form_w, drawing, close_btn;
    XmString btn_label, dlg_title;
    char title_buf[256];
    popup_data_t *pd;

    /* URL-encode the image URL so ?/&/= in it don't break query parsing */
    {
        char encoded_url[1800];
        const char *s = url;
        int ei = 0;
        /* Determine effective width from image_size preset */
        static const int size_widths[] = { 160, 320, 480, 640 };
        int eff_width = (cfg_image_size >= 0 && cfg_image_size <= 3)
                        ? size_widths[cfg_image_size] : cfg_image_maxwidth;

        while (*s && ei < (int)sizeof(encoded_url) - 4) {
            unsigned char c = (unsigned char)*s;
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                c == '.' || c == '~' || c == '/' || c == ':') {
                encoded_url[ei++] = c;
            } else {
                static const char hex[] = "0123456789ABCDEF";
                encoded_url[ei++] = '%';
                encoded_url[ei++] = hex[(c >> 4) & 0x0F];
                encoded_url[ei++] = hex[c & 0x0F];
            }
            s++;
        }
        encoded_url[ei] = '\0';

        snprintf(path, sizeof(path), "/api/image?url=%s&w=%d&colors=%d",
                 encoded_url, eff_width, cfg_image_colors);
    }

    /* Update status */
    {
        XmString s = XmStringCreateLocalized("Loading image...");
        XtVaSetValues(status_label, XmNlabelString, s, NULL);
        XmStringFree(s);
        XmUpdateDisplay(top_shell);
    }

    /* Fetch the GIF from the bridge */
    if (http_get(cfg_server, cfg_port, path, &resp) < 0) {
        XmString s = XmStringCreateLocalized("Failed to fetch image");
        XtVaSetValues(status_label, XmNlabelString, s, NULL);
        XmStringFree(s);
        return;
    }

    if (resp.status_code != 200 || !resp.body || resp.body_len < 6) {
        XmString s = XmStringCreateLocalized("Image not available");
        XtVaSetValues(status_label, XmNlabelString, s, NULL);
        XmStringFree(s);
        http_response_free(&resp);
        return;
    }

    /* Decode GIF to X11 Pixmap */
    img = gif_decode(XtDisplay(top_shell),
                     DefaultScreen(XtDisplay(top_shell)),
                     XtWindow(top_shell),
                     (unsigned char *)resp.body, resp.body_len);
    http_response_free(&resp);

    if (!img.loaded || img.pixmap == None) {
        XmString s = XmStringCreateLocalized("Failed to decode image");
        XtVaSetValues(status_label, XmNlabelString, s, NULL);
        XmStringFree(s);
        return;
    }

    /* Allocate per-popup image data */
    pd = (popup_data_t *)malloc(sizeof(popup_data_t));
    if (!pd) {
        XFreePixmap(XtDisplay(top_shell), img.pixmap);
        return;
    }
    pd->pixmap = img.pixmap;
    pd->width = img.width;
    pd->height = img.height;

    /* Create popup window */
    snprintf(title_buf, sizeof(title_buf), "Image: %s", title ? title : "image");
    dlg_title = XmStringCreateLocalized(title_buf);

    shell = XtVaCreatePopupShell("imagePopup",
        topLevelShellWidgetClass, top_shell,
        XmNtitle, title_buf,
        XmNwidth, img.width + 10,
        XmNheight, img.height + 40,
        XmNdeleteResponse, XmDESTROY,
        NULL);
    XmStringFree(dlg_title);

    form_w = XtVaCreateManagedWidget("imgForm",
        xmFormWidgetClass, shell, NULL);

    /* Close button at bottom */
    btn_label = XmStringCreateLocalized("Close");
    close_btn = XtVaCreateManagedWidget("closeBtn",
        xmPushButtonWidgetClass, form_w,
        XmNlabelString, btn_label,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        NULL);
    XmStringFree(btn_label);
    XtAddCallback(close_btn, XmNactivateCallback, image_close_cb,
                  (XtPointer)pd);

    /* Drawing area for the image */
    drawing = XtVaCreateManagedWidget("imageArea",
        xmDrawingAreaWidgetClass, form_w,
        XmNwidth, img.width,
        XmNheight, img.height,
        XmNtopAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_WIDGET,
        XmNbottomWidget, close_btn,
        NULL);
    XtAddCallback(drawing, XmNexposeCallback, image_expose_cb, (XtPointer)pd);

    XtPopup(shell, XtGrabNone);

    /* Update status */
    {
        char buf[256];
        XmString s;
        snprintf(buf, sizeof(buf), "Image: %dx%d", img.width, img.height);
        s = XmStringCreateLocalized(buf);
        XtVaSetValues(status_label, XmNlabelString, s, NULL);
        XmStringFree(s);
    }
}

/* ================================================================
 * SEND MESSAGE
 * ================================================================ */

static void send_message_cb(Widget w, XtPointer client, XtPointer call)
{
    char *text;
    char json_body[4096];
    json_value_t *result;

    (void)w; (void)client; (void)call;

    if (current_channel_idx < 0) return;

    text = XmTextFieldGetString(input_text);
    if (!text || text[0] == '\0') {
        if (text) XtFree(text);
        return;
    }

    /* Check for /img command */
    if (strncmp(text, "/img", 4) == 0) {
        int img_idx = -1;
        if (text[4] == ' ' && text[5] >= '0' && text[5] <= '9') {
            img_idx = atoi(text + 5) % MAX_IMAGE_URLS;
        } else if (text[4] == '\0' || text[4] == ' ') {
            /* Show last image */
            if (n_image_urls > 0) {
                img_idx = (image_url_next - 1) % MAX_IMAGE_URLS;
            }
        }

        if (img_idx >= 0 && img_idx < MAX_IMAGE_URLS &&
            image_urls[img_idx].url[0] != '\0') {
            show_image(image_urls[img_idx].url, image_urls[img_idx].label);
        } else {
            XmString s = XmStringCreateLocalized("No image to show. Use /img N");
            XtVaSetValues(status_label, XmNlabelString, s, NULL);
            XmStringFree(s);
        }
        XmTextFieldSetString(input_text, "");
        XtFree(text);
        return;
    }

    /* Build JSON body (manual, no generator needed) */
    /* Escape special chars in the message */
    {
        char escaped[4000];
        int si = 0, di = 0;
        while (text[si] && di < (int)sizeof(escaped) - 2) {
            char c = text[si++];
            if (c == '"' || c == '\\') {
                escaped[di++] = '\\';
            }
            escaped[di++] = c;
        }
        escaped[di] = '\0';

        snprintf(json_body, sizeof(json_body),
                 "{\"channel\":\"%s\",\"content\":\"%s\"}",
                 channels[current_channel_idx].id, escaped);
    }

    result = api_post("/api/send", json_body);
    if (result) {
        const char *msg_id = json_get_str(result, "id");

        /* Echo sent message locally so it appears immediately */
        {
            char line[2048];
            time_t now;
            struct tm *tm_val;
            char ts[16];

            time(&now);
            tm_val = localtime(&now);
            snprintf(ts, sizeof(ts), "%02d:%02d", tm_val->tm_hour, tm_val->tm_min);
            snprintf(line, sizeof(line), "[%s] <%s> %s\n",
                     ts, self_username, text);
            append_text(line);
        }

        /* Update last_message_id so next poll doesn't duplicate this msg */
        if (msg_id && msg_id[0]) {
            strncpy(last_message_id, msg_id, 31);
            last_message_id[31] = '\0';
        }
        json_free(result);
    }

    /* Clear input */
    XmTextFieldSetString(input_text, "");
    XtFree(text);
}

/* ================================================================
 * POLLING CALLBACKS
 * ================================================================ */

static void poll_messages_cb(XtPointer client, XtIntervalId *id)
{
    (void)client; (void)id;

    /* Fetch new messages for current channel */
    if (current_channel_idx >= 0 && last_message_id[0] != '\0') {
        fetch_messages(channels[current_channel_idx].id, 0);
    }

    /* Also update typing indicators */
    if (current_channel_idx >= 0) {
        char path[256];
        json_value_t *arr;
        int len, i;
        char status_buf[512];

        snprintf(path, sizeof(path), "/api/typing?channel=%s",
                 channels[current_channel_idx].id);
        arr = api_get(path);
        if (arr) {
            len = json_array_len(arr);
            if (len > 0) {
                strcpy(status_buf, "");
                for (i = 0; i < len && i < 3; i++) {
                    json_value_t *t = json_array_get(arr, i);
                    const char *name = json_get_str(t, "username");
                    if (name) {
                        if (i > 0) strcat(status_buf, ", ");
                        strncat(status_buf, name,
                                sizeof(status_buf) - strlen(status_buf) - 1);
                    }
                }
                if (len == 1) {
                    strcat(status_buf, " is typing...");
                } else {
                    strcat(status_buf, " are typing...");
                }
            } else {
                snprintf(status_buf, sizeof(status_buf), "#%s",
                         current_channel_idx >= 0 ?
                         channels[current_channel_idx].name : "");
            }
            {
                XmString s = XmStringCreateLocalized(status_buf);
                XtVaSetValues(status_label, XmNlabelString, s, NULL);
                XmStringFree(s);
            }
            json_free(arr);
        }
    }

    /* Re-register timer */
    poll_timer = XtAppAddTimeOut(app_context, cfg_poll_ms,
                                 poll_messages_cb, NULL);
}

static void poll_presence_cb(XtPointer client, XtIntervalId *id)
{
    (void)client; (void)id;

    if (dm_mode && current_channel_idx >= 0) {
        /* DM mode: show DM participants */
        char path[256];
        json_value_t *arr;
        int i, len;

        snprintf(path, sizeof(path), "/api/presence?channel=%s",
                 channels[current_channel_idx].id);
        arr = api_get(path);
        if (arr) {
            XmListDeleteAllItems(member_list);
            len = json_array_len(arr);
            for (i = 0; i < len && i < 50; i++) {
                json_value_t *m = json_array_get(arr, i);
                const char *name = json_get_str(m, "username");
                const char *status = json_get_str(m, "status");
                char buf[200];
                const char *icon;
                XmString item;

                if (!name) continue;
                if (!status) status = "offline";
                if (strcmp(status, "online") == 0) icon = "+";
                else if (strcmp(status, "idle") == 0) icon = "~";
                else if (strcmp(status, "dnd") == 0) icon = "-";
                else icon = " ";

                snprintf(buf, sizeof(buf), "%s %s", icon, name);
                item = XmStringCreateLocalized(buf);
                XmListAddItem(member_list, item, 0);
                XmStringFree(item);
            }
            json_free(arr);
        }
    } else if (!dm_mode && current_guild_idx >= 0) {
        char path[256];
        json_value_t *arr;
        int i, len;
        char cur_role[128];

        snprintf(path, sizeof(path), "/api/presence?guild=%s",
                 guilds[current_guild_idx].id);
        arr = api_get(path);
        if (arr) {
            XmListDeleteAllItems(member_list);
            len = json_array_len(arr);
            cur_role[0] = '\0';
            for (i = 0; i < len && i < 200; i++) {
                json_value_t *m = json_array_get(arr, i);
                const char *name = json_get_str(m, "username");
                const char *status = json_get_str(m, "status");
                const char *activity = json_get_str(m, "activity");
                const char *role = json_get_str(m, "role");
                char buf[200];
                const char *icon;
                XmString item;

                if (!name) continue;

                /* Insert role group header when role changes */
                {
                    const char *rname = role ? role : "Online";
                    if (strcmp(cur_role, rname) != 0) {
                        char hdr[140];
                        strncpy(cur_role, rname, sizeof(cur_role) - 1);
                        cur_role[sizeof(cur_role) - 1] = '\0';
                        snprintf(hdr, sizeof(hdr), "-- %s --", rname);
                        item = XmStringCreateLocalized(hdr);
                        XmListAddItem(member_list, item, 0);
                        XmStringFree(item);
                    }
                }

                /* Status icons using ASCII/basic chars */
                if (!status) status = "offline";
                if (strcmp(status, "online") == 0) icon = "+";
                else if (strcmp(status, "idle") == 0) icon = "~";
                else if (strcmp(status, "dnd") == 0) icon = "-";
                else icon = " ";

                if (activity) {
                    snprintf(buf, sizeof(buf), "  %s %s (%s)", icon, name, activity);
                } else {
                    snprintf(buf, sizeof(buf), "  %s %s", icon, name);
                }

                item = XmStringCreateLocalized(buf);
                XmListAddItem(member_list, item, 0);
                XmStringFree(item);
            }
            json_free(arr);
        }
    }

    /* Re-register timer (10 seconds) */
    presence_timer = XtAppAddTimeOut(app_context, 10000,
                                      poll_presence_cb, NULL);
}

/* ================================================================
 * INITIAL CONNECTION CHECK
 * ================================================================ */

static void check_connection(XtPointer client, XtIntervalId *id)
{
    json_value_t *status;
    int connected;

    (void)client; (void)id;

    status = api_get("/api/status");
    if (!status) {
        XmString s = XmStringCreateLocalized("Connecting to bridge...");
        XtVaSetValues(status_label, XmNlabelString, s, NULL);
        XmStringFree(s);
        /* Retry in 3 seconds */
        XtAppAddTimeOut(app_context, 3000, check_connection, NULL);
        return;
    }

    connected = json_get_bool(status, "connected");

    /* Capture our own username for message echo */
    {
        json_value_t *self = json_get(status, "self");
        if (self) {
            const char *gname = json_get_str(self, "globalName");
            const char *uname = json_get_str(self, "username");
            const char *name = gname ? gname : uname;
            if (name) {
                strncpy(self_username, name, sizeof(self_username) - 1);
                self_username[sizeof(self_username) - 1] = '\0';
            }
        }
    }

    json_free(status);

    if (!connected) {
        XmString s = XmStringCreateLocalized("Bridge: waiting for Discord...");
        XtVaSetValues(status_label, XmNlabelString, s, NULL);
        XmStringFree(s);
        XtAppAddTimeOut(app_context, 3000, check_connection, NULL);
        return;
    }

    /* Connected! Fetch guilds and start polling */
    {
        XmString s = XmStringCreateLocalized("Connected to Discord");
        XtVaSetValues(status_label, XmNlabelString, s, NULL);
        XmStringFree(s);
    }

    fetch_guilds();
    rebuild_guild_menu();

    /* Start polling */
    poll_timer = XtAppAddTimeOut(app_context, cfg_poll_ms,
                                 poll_messages_cb, NULL);
    presence_timer = XtAppAddTimeOut(app_context, 10000,
                                      poll_presence_cb, NULL);
}

/* ================================================================
 * SAVE CONFIGURATION
 * ================================================================ */

static void save_config(void)
{
    char path[512];
    char *home;
    FILE *f;

    home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(path, sizeof(path), "%s/.sparccordrc", home);

    f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "# SPARCcord configuration\n");
    fprintf(f, "server=%s\n", cfg_server);
    fprintf(f, "port=%d\n", cfg_port);
    fprintf(f, "poll_ms=%d\n", cfg_poll_ms);
    fprintf(f, "image_colors=%d\n", cfg_image_colors);
    fprintf(f, "image_maxwidth=%d\n", cfg_image_maxwidth);
    fprintf(f, "image_size=%d\n", cfg_image_size);
    fclose(f);
}

/* ================================================================
 * PREFERENCES DIALOG
 * ================================================================ */

/* Widgets for the prefs dialog — static so the OK callback can read them */
static Widget pref_server_tf, pref_port_tf, pref_poll_tf;
static Widget pref_colors_tf, pref_size_om;

static void prefs_ok_cb(Widget w, XtPointer client, XtPointer call)
{
    Widget dialog = (Widget)client;
    char *val;

    (void)w; (void)call;

    /* Read values from text fields */
    val = XmTextFieldGetString(pref_server_tf);
    if (val && val[0]) strncpy(cfg_server, val, sizeof(cfg_server) - 1);
    if (val) XtFree(val);

    val = XmTextFieldGetString(pref_port_tf);
    if (val && val[0]) cfg_port = atoi(val);
    if (val) XtFree(val);

    val = XmTextFieldGetString(pref_poll_tf);
    if (val && val[0]) cfg_poll_ms = atoi(val);
    if (val) XtFree(val);

    val = XmTextFieldGetString(pref_colors_tf);
    if (val && val[0]) cfg_image_colors = atoi(val);
    if (val) XtFree(val);

    /* Read image size from option menu */
    {
        Widget selected;
        XtVaGetValues(pref_size_om, XmNmenuHistory, &selected, NULL);
        if (selected) {
            XtPointer ud;
            XtVaGetValues(selected, XmNuserData, &ud, NULL);
            cfg_image_size = (int)(long)ud;
        }
    }

    /* Save to disk */
    save_config();

    /* Update status */
    {
        XmString s = XmStringCreateLocalized("Preferences saved.");
        XtVaSetValues(status_label, XmNlabelString, s, NULL);
        XmStringFree(s);
    }

    XtDestroyWidget(dialog);
}

static void prefs_cancel_cb(Widget w, XtPointer client, XtPointer call)
{
    Widget dialog = (Widget)client;
    (void)w; (void)call;
    XtDestroyWidget(dialog);
}

static void prefs_cb(Widget w, XtPointer client, XtPointer call)
{
    Widget dialog, form_w, ok_btn, cancel_btn, btn_row;
    Widget lbl;
    XmString s;
    char buf[64];

    (void)w; (void)client; (void)call;

    /* Create dialog shell */
    dialog = XtVaCreatePopupShell("prefsDialog",
        topLevelShellWidgetClass, top_shell,
        XmNtitle, "Preferences",
        XmNwidth, 340,
        XmNheight, 310,
        XmNdeleteResponse, XmDESTROY,
        NULL);

    form_w = XtVaCreateManagedWidget("prefsForm",
        xmFormWidgetClass, dialog, NULL);

    /* Row helper macro — label + text field */
    /* Server */
    s = XmStringCreateLocalized("Bridge Server:");
    lbl = XtVaCreateManagedWidget("lbl1", xmLabelWidgetClass, form_w,
        XmNlabelString, s,
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 10,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 10,
        NULL);
    XmStringFree(s);
    pref_server_tf = XtVaCreateManagedWidget("serverTF",
        xmTextFieldWidgetClass, form_w,
        XmNtopAttachment, XmATTACH_FORM, XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, lbl,
        XmNleftOffset, 5,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 10,
        XmNvalue, cfg_server,
        NULL);

    /* Port */
    snprintf(buf, sizeof(buf), "%d", cfg_port);
    s = XmStringCreateLocalized("Port:");
    lbl = XtVaCreateManagedWidget("lbl2", xmLabelWidgetClass, form_w,
        XmNlabelString, s,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, pref_server_tf,
        XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 10,
        NULL);
    XmStringFree(s);
    pref_port_tf = XtVaCreateManagedWidget("portTF",
        xmTextFieldWidgetClass, form_w,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, pref_server_tf,
        XmNtopOffset, 6,
        XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, lbl,
        XmNleftOffset, 5,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 10,
        XmNvalue, buf,
        XmNcolumns, 6,
        NULL);

    /* Poll interval */
    snprintf(buf, sizeof(buf), "%d", cfg_poll_ms);
    s = XmStringCreateLocalized("Poll (ms):");
    lbl = XtVaCreateManagedWidget("lbl3", xmLabelWidgetClass, form_w,
        XmNlabelString, s,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, pref_port_tf,
        XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 10,
        NULL);
    XmStringFree(s);
    pref_poll_tf = XtVaCreateManagedWidget("pollTF",
        xmTextFieldWidgetClass, form_w,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, pref_port_tf,
        XmNtopOffset, 6,
        XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, lbl,
        XmNleftOffset, 5,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 10,
        XmNvalue, buf,
        XmNcolumns, 6,
        NULL);

    /* Image colors */
    snprintf(buf, sizeof(buf), "%d", cfg_image_colors);
    s = XmStringCreateLocalized("Image Colors:");
    lbl = XtVaCreateManagedWidget("lbl4", xmLabelWidgetClass, form_w,
        XmNlabelString, s,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, pref_poll_tf,
        XmNtopOffset, 8,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 10,
        NULL);
    XmStringFree(s);
    pref_colors_tf = XtVaCreateManagedWidget("colorsTF",
        xmTextFieldWidgetClass, form_w,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, pref_poll_tf,
        XmNtopOffset, 6,
        XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, lbl,
        XmNleftOffset, 5,
        XmNrightAttachment, XmATTACH_FORM, XmNrightOffset, 10,
        XmNvalue, buf,
        XmNcolumns, 4,
        NULL);

    /* Image Size option menu */
    {
        Widget pulldown, size_btns[4];
        int i;
        static const char *size_labels[] = {
            "Small (160px)", "Medium (320px)", "Large (480px)", "Full (640px)"
        };

        s = XmStringCreateLocalized("Image Size:");
        lbl = XtVaCreateManagedWidget("lbl5", xmLabelWidgetClass, form_w,
            XmNlabelString, s,
            XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, pref_colors_tf,
            XmNtopOffset, 10,
            XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 10,
            NULL);
        XmStringFree(s);

        pulldown = XmCreatePulldownMenu(form_w, "sizePD", NULL, 0);
        for (i = 0; i < 4; i++) {
            s = XmStringCreateLocalized((char *)size_labels[i]);
            size_btns[i] = XtVaCreateManagedWidget("sizeBtn",
                xmPushButtonWidgetClass, pulldown,
                XmNlabelString, s,
                XmNuserData, (XtPointer)(long)i,
                NULL);
            XmStringFree(s);
        }

        s = XmStringCreateLocalized("Image Size");
        pref_size_om = XmCreateOptionMenu(form_w, "sizeOM", NULL, 0);
        XtVaSetValues(pref_size_om,
            XmNsubMenuId, pulldown,
            XmNmenuHistory, size_btns[cfg_image_size],
            XmNlabelString, s,
            XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, pref_colors_tf,
            XmNtopOffset, 6,
            XmNleftAttachment, XmATTACH_WIDGET, XmNleftWidget, lbl,
            XmNleftOffset, 5,
            NULL);
        XmStringFree(s);
        /* Hide the redundant label Motif adds to option menus */
        XtUnmanageChild(XmOptionLabelGadget(pref_size_om));
        XtManageChild(pref_size_om);
    }

    /* OK / Cancel buttons */
    btn_row = XtVaCreateManagedWidget("btnRow",
        xmRowColumnWidgetClass, form_w,
        XmNorientation, XmHORIZONTAL,
        XmNpacking, XmPACK_TIGHT,
        XmNtopAttachment, XmATTACH_WIDGET, XmNtopWidget, pref_size_om,
        XmNtopOffset, 10,
        XmNleftAttachment, XmATTACH_FORM, XmNleftOffset, 80,
        NULL);

    s = XmStringCreateLocalized("  OK  ");
    ok_btn = XtVaCreateManagedWidget("okBtn",
        xmPushButtonWidgetClass, btn_row,
        XmNlabelString, s, NULL);
    XmStringFree(s);
    XtAddCallback(ok_btn, XmNactivateCallback, prefs_ok_cb,
                  (XtPointer)dialog);

    s = XmStringCreateLocalized("Cancel");
    cancel_btn = XtVaCreateManagedWidget("cancelBtn",
        xmPushButtonWidgetClass, btn_row,
        XmNlabelString, s, NULL);
    XmStringFree(s);
    XtAddCallback(cancel_btn, XmNactivateCallback, prefs_cancel_cb,
                  (XtPointer)dialog);

    XtPopup(dialog, XtGrabNone);
}

/* ================================================================
 * MENU CALLBACKS
 * ================================================================ */

static void refresh_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)w; (void)client; (void)call;
    fetch_guilds();
    rebuild_guild_menu();
    if (current_guild_idx >= 0) {
        fetch_channels(guilds[current_guild_idx].id);
    }
}

static void about_cb(Widget w, XtPointer client, XtPointer call)
{
    Widget dialog;
    XmString msg;

    (void)w; (void)client; (void)call;

    msg = XmStringCreateLocalized(
        "SPARCcord - Discord on Solaris 7\n\n"
        "A native Motif/CDE Discord client.\n"
        "Talks to discord-bridge via HTTP.\n\n"
        "For SPARCstation 4/5/20");

    dialog = XmCreateMessageDialog(top_shell, "aboutDialog", NULL, 0);
    XtVaSetValues(dialog,
        XmNmessageString, msg,
        XmNdialogTitle, XmStringCreateLocalized("About SPARCcord"),
        NULL);
    XmStringFree(msg);

    /* Hide Cancel and Help buttons */
    XtUnmanageChild(XmMessageBoxGetChild(dialog, XmDIALOG_CANCEL_BUTTON));
    XtUnmanageChild(XmMessageBoxGetChild(dialog, XmDIALOG_HELP_BUTTON));

    XtManageChild(dialog);
}

static void quit_cb(Widget w, XtPointer client, XtPointer call)
{
    (void)w; (void)client; (void)call;
    exit(0);
}

/* ================================================================
 * IMAGE CLICK HANDLER
 * Detects clicks on [IMG #N] markers in message text and opens the image.
 * ================================================================ */

static void message_click_cb(Widget w, XtPointer client, XEvent *event,
                              Boolean *cont)
{
    XmTextPosition pos;
    char *text;
    int text_len;
    int line_start, line_end;

    (void)client;
    *cont = True;  /* Allow normal processing to continue */

    if (event->type != ButtonRelease) return;
    if (event->xbutton.button != 1) return;  /* Left click only */

    /* Get position from click coordinates */
    pos = XmTextXYToPos(w, event->xbutton.x, event->xbutton.y);
    text = XmTextGetString(w);
    if (!text) return;

    text_len = strlen(text);

    /* Find the line containing this position */
    line_start = pos;
    while (line_start > 0 && text[line_start - 1] != '\n') line_start--;
    line_end = pos;
    while (line_end < text_len && text[line_end] != '\n') line_end++;

    /* Check if this line contains [IMG #N */
    {
        const char *p = text + line_start;
        const char *end = text + line_end;
        while (p < end - 5) {
            if (p[0] == '[' && p[1] == 'I' && p[2] == 'M' &&
                p[3] == 'G' && p[4] == ' ' && p[5] == '#') {
                int img_idx = atoi(p + 6);
                if (img_idx >= 0 && img_idx < MAX_IMAGE_URLS &&
                    image_urls[img_idx].url[0] != '\0') {
                    XtFree(text);
                    show_image(image_urls[img_idx].url,
                               image_urls[img_idx].label);
                    return;
                }
            }
            p++;
        }
    }

    XtFree(text);
}

/* ================================================================
 * BUILD UI
 * ================================================================ */

static void create_ui(void)
{
    Widget settings_menu, settings_pulldown;
    Widget refresh_btn, prefs_btn, about_btn, quit_btn;
    Widget left_pane, center_pane, right_pane;
    Arg args[20];
    int n;

    /* Main Window */
    main_window = XtVaCreateManagedWidget("mainWindow",
        xmMainWindowWidgetClass, top_shell,
        NULL);

    /* Menu Bar */
    menu_bar = XmCreateMenuBar(main_window, "menuBar", NULL, 0);
    XtManageChild(menu_bar);

    /* Server menu */
    server_pulldown = XmCreatePulldownMenu(menu_bar, "serverPulldown", NULL, 0);
    server_menu = XtVaCreateManagedWidget("Server",
        xmCascadeButtonWidgetClass, menu_bar,
        XmNsubMenuId, server_pulldown,
        NULL);

    /* Settings menu */
    settings_pulldown = XmCreatePulldownMenu(menu_bar, "settingsPulldown", NULL, 0);
    settings_menu = XtVaCreateManagedWidget("Settings",
        xmCascadeButtonWidgetClass, menu_bar,
        XmNsubMenuId, settings_pulldown,
        NULL);

    refresh_btn = XtVaCreateManagedWidget("Refresh",
        xmPushButtonWidgetClass, settings_pulldown, NULL);
    XtAddCallback(refresh_btn, XmNactivateCallback, refresh_cb, NULL);

    prefs_btn = XtVaCreateManagedWidget("Preferences...",
        xmPushButtonWidgetClass, settings_pulldown, NULL);
    XtAddCallback(prefs_btn, XmNactivateCallback, prefs_cb, NULL);

    XtVaCreateManagedWidget("sep2",
        xmSeparatorWidgetClass, settings_pulldown, NULL);

    about_btn = XtVaCreateManagedWidget("About...",
        xmPushButtonWidgetClass, settings_pulldown, NULL);
    XtAddCallback(about_btn, XmNactivateCallback, about_cb, NULL);

    quit_btn = XtVaCreateManagedWidget("Quit",
        xmPushButtonWidgetClass, settings_pulldown, NULL);
    XtAddCallback(quit_btn, XmNactivateCallback, quit_cb, NULL);

    /* Main form with 3 panes — percentage-based for proportional scaling.
     * fractionBase=100: left=0..20%, center=20..80%, right=80..100%.
     * Panes scale proportionally when the window is resized.
     * (Motif 1.2 XmPanedWindow only supports vertical, not horizontal,
     *  so user-draggable pane borders aren't possible.) */
    form = XtVaCreateManagedWidget("form",
        xmFormWidgetClass, main_window,
        XmNfractionBase, 100,
        NULL);

    /* Left pane: Channel list (0-20%) */
    left_pane = XtVaCreateManagedWidget("leftFrame",
        xmFrameWidgetClass, form,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_POSITION,
        XmNleftPosition, 0,
        XmNrightAttachment, XmATTACH_POSITION,
        XmNrightPosition, 20,
        NULL);

    n = 0;
    XtSetArg(args[n], XmNselectionPolicy, XmBROWSE_SELECT); n++;
    XtSetArg(args[n], XmNvisibleItemCount, 20); n++;
    channel_list = XmCreateScrolledList(left_pane, "channelList", args, n);
    XtManageChild(channel_list);
    XtAddCallback(channel_list, XmNbrowseSelectionCallback,
                  channel_select_cb, NULL);

    /* Right pane: Member list (80-100%) */
    right_pane = XtVaCreateManagedWidget("rightFrame",
        xmFrameWidgetClass, form,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_POSITION,
        XmNleftPosition, 80,
        XmNrightAttachment, XmATTACH_POSITION,
        XmNrightPosition, 100,
        NULL);

    n = 0;
    XtSetArg(args[n], XmNselectionPolicy, XmBROWSE_SELECT); n++;
    XtSetArg(args[n], XmNvisibleItemCount, 20); n++;
    member_list = XmCreateScrolledList(right_pane, "memberList", args, n);
    XtManageChild(member_list);

    /* Center pane: Messages + Input (20-80%) */
    center_pane = XtVaCreateManagedWidget("centerPane",
        xmFormWidgetClass, form,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_POSITION,
        XmNleftPosition, 20,
        XmNrightAttachment, XmATTACH_POSITION,
        XmNrightPosition, 80,
        NULL);

    /* Input field at bottom of center pane */
    input_text = XtVaCreateManagedWidget("inputText",
        xmTextFieldWidgetClass, center_pane,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNmaxLength, 2000,
        NULL);
    XtAddCallback(input_text, XmNactivateCallback, send_message_cb, NULL);

    /* Status label above input */
    {
        XmString init_s = XmStringCreateLocalized("Starting...");
        status_label = XtVaCreateManagedWidget("statusLabel",
            xmLabelWidgetClass, center_pane,
            XmNbottomAttachment, XmATTACH_WIDGET,
            XmNbottomWidget, input_text,
            XmNleftAttachment, XmATTACH_FORM,
            XmNrightAttachment, XmATTACH_FORM,
            XmNalignment, XmALIGNMENT_BEGINNING,
            XmNlabelString, init_s,
            NULL);
        XmStringFree(init_s);
    }

    /* Scrolled text for messages (above status label) */
    n = 0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNbottomWidget, status_label); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNeditMode, XmMULTI_LINE_EDIT); n++;
    XtSetArg(args[n], XmNeditable, False); n++;
    XtSetArg(args[n], XmNcursorPositionVisible, False); n++;
    XtSetArg(args[n], XmNwordWrap, True); n++;
    XtSetArg(args[n], XmNscrollHorizontal, False); n++;
    XtSetArg(args[n], XmNscrollVertical, True); n++;
    XtSetArg(args[n], XmNrows, 20); n++;
    message_text = XmCreateScrolledText(center_pane, "messageText", args, n);
    XtManageChild(message_text);

    /* Click on [IMG #N] markers in message text to view images */
    XtAddEventHandler(message_text, ButtonReleaseMask, False,
                      message_click_cb, NULL);

    /* Set up Main Window areas */
    XtVaSetValues(main_window,
        XmNmenuBar, menu_bar,
        XmNworkWindow, form,
        NULL);
}

/* ================================================================
 * MAIN
 * ================================================================ */

int main(int argc, char *argv[])
{
    /* Load config first */
    load_config();

    /* Override from command line args */
    {
        int i;
        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-server") == 0 && i + 1 < argc) {
                strncpy(cfg_server, argv[++i], sizeof(cfg_server) - 1);
            } else if (strcmp(argv[i], "-port") == 0 && i + 1 < argc) {
                cfg_port = atoi(argv[++i]);
            }
        }
    }

    printf("SPARCcord - Discord for Solaris 7\n");
    printf("Bridge: %s:%d  Poll: %dms\n", cfg_server, cfg_port, cfg_poll_ms);
    fflush(stdout);

    /* Initialize Xt/Motif */
    top_shell = XtVaAppInitialize(&app_context, "SPARCcord",
        NULL, 0, &argc, argv, NULL,
        XmNtitle, "SPARCcord",
        XmNwidth, 780,
        XmNheight, 500,
        NULL);

    /* Initialize color cache for GIF loading */
    gif_init_colors(XtDisplay(top_shell),
                    DefaultScreen(XtDisplay(top_shell)));

    /* Build the UI */
    create_ui();

    /* Realize and show */
    XtRealizeWidget(top_shell);

    /* Set window icon */
    {
        Display *dpy = XtDisplay(top_shell);
        Window win = XtWindow(top_shell);
        Pixmap icon_pm = XCreateBitmapFromData(dpy, win,
            (char *)icon48_bits, icon48_width, icon48_height);
        if (icon_pm != None) {
            XtVaSetValues(top_shell, XmNiconPixmap, icon_pm, NULL);
        }
    }

    /* Start connection check (will trigger guild fetch + polling) */
    XtAppAddTimeOut(app_context, 500, check_connection, NULL);

    /* Enter Xt event loop */
    XtAppMainLoop(app_context);

    return 0;
}
