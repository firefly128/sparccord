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
#include <X11/Xlib.h>
#include <X11/Shell.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "http.h"
#include "json.h"
#include "gifload.h"

/* ================================================================
 * CONFIGURATION
 * ================================================================ */

static char cfg_server[256] = "10.0.2.2";
static int  cfg_port = 3002;
static int  cfg_poll_ms = 2000;
static int  cfg_image_colors = 16;
static int  cfg_image_maxwidth = 320;

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
static void fetch_channels(const char *guild_id);
static void channel_select_cb(Widget w, XtPointer client, XtPointer call);
static void fetch_messages(const char *channel_id, int initial);
static void send_message_cb(Widget w, XtPointer client, XtPointer call);
static void poll_messages_cb(XtPointer client, XtIntervalId *id);
static void poll_presence_cb(XtPointer client, XtIntervalId *id);

/* Rebuild guild buttons with callbacks */
static void rebuild_guild_menu(void)
{
    int i;
    Cardinal n_old;
    Widget *children;

    XtVaGetValues(server_pulldown, XmNnumChildren, &n_old,
                  XmNchildren, &children, NULL);

    for (i = 0; i < (int)n_old; i++) {
        XtAddCallback(children[i], XmNactivateCallback, guild_select_cb,
                      (XtPointer)(long)i);
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

    /* Update window title */
    snprintf(title, sizeof(title), "SPARCcord - %s", guilds[idx].name);
    XtVaSetValues(top_shell, XmNtitle, title, NULL);

    /* Fetch channels for this guild */
    fetch_channels(guilds[idx].id);
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
        snprintf(buf, sizeof(buf), "Channel: #%s", channels[idx].name);
        s = XmStringCreateLocalized(buf);
        XtVaSetValues(status_label, XmNlabelString, s, NULL);
        XmStringFree(s);
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
                append_text(line);
            }
        }

        /* Format: [HH:MM] <nick> message */
        snprintf(line, sizeof(line), "[%s] <%s> %s\n",
                 ts_buf, author_name, content);
        append_text(line);

        /* Show attachments */
        attachments = json_get(msg, "attachments");
        n_attach = json_array_len(attachments);
        for (j = 0; j < n_attach; j++) {
            json_value_t *att = json_array_get(attachments, j);
            const char *filename = json_get_str(att, "filename");
            const char *url = json_get_str(att, "url");

            if (filename) {
                snprintf(line, sizeof(line),
                         "        [Attachment: %s]\n", filename);
                append_text(line);
            }

            (void)url;  /* TODO: image popup on click */
        }

        /* Show embeds */
        {
            json_value_t *embeds = json_get(msg, "embeds");
            int n_embeds = json_array_len(embeds);
            for (j = 0; j < n_embeds; j++) {
                json_value_t *emb = json_array_get(embeds, j);
                const char *title = json_get_str(emb, "title");
                const char *desc = json_get_str(emb, "description");
                if (title) {
                    snprintf(line, sizeof(line), "        [%s]\n", title);
                    append_text(line);
                }
                if (desc) {
                    snprintf(line, sizeof(line), "        %s\n", desc);
                    append_text(line);
                }
            }
        }
    }

    json_free(arr);
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
    if (result) json_free(result);

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

    if (current_guild_idx >= 0) {
        char path[256];
        json_value_t *arr;
        int i, len;

        snprintf(path, sizeof(path), "/api/presence?guild=%s",
                 guilds[current_guild_idx].id);
        arr = api_get(path);
        if (arr) {
            XmListDeleteAllItems(member_list);
            len = json_array_len(arr);
            for (i = 0; i < len && i < 100; i++) {
                json_value_t *m = json_array_get(arr, i);
                const char *name = json_get_str(m, "username");
                const char *status = json_get_str(m, "status");
                const char *activity = json_get_str(m, "activity");
                char buf[200];
                const char *icon;
                XmString item;

                if (!name) continue;

                /* Status icons using ASCII/basic chars */
                if (!status) status = "offline";
                if (strcmp(status, "online") == 0) icon = "+";
                else if (strcmp(status, "idle") == 0) icon = "~";
                else if (strcmp(status, "dnd") == 0) icon = "-";
                else icon = " ";

                if (activity) {
                    snprintf(buf, sizeof(buf), "%s %s (%s)", icon, name, activity);
                } else {
                    snprintf(buf, sizeof(buf), "%s %s", icon, name);
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
 * BUILD UI
 * ================================================================ */

static void create_ui(void)
{
    Widget settings_menu, settings_pulldown;
    Widget refresh_btn, about_btn, quit_btn;
    Widget channel_sw, member_sw;
    Widget msg_frame, input_frame;
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

    about_btn = XtVaCreateManagedWidget("About...",
        xmPushButtonWidgetClass, settings_pulldown, NULL);
    XtAddCallback(about_btn, XmNactivateCallback, about_cb, NULL);

    quit_btn = XtVaCreateManagedWidget("Quit",
        xmPushButtonWidgetClass, settings_pulldown, NULL);
    XtAddCallback(quit_btn, XmNactivateCallback, quit_cb, NULL);

    /* Main form with 3 panes */
    form = XtVaCreateManagedWidget("form",
        xmFormWidgetClass, main_window,
        NULL);

    /* Left pane: Channel list */
    n = 0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNwidth, 150); n++;
    left_pane = XmCreateFrame(form, "leftFrame", args, n);
    XtManageChild(left_pane);

    n = 0;
    XtSetArg(args[n], XmNselectionPolicy, XmBROWSE_SELECT); n++;
    XtSetArg(args[n], XmNvisibleItemCount, 20); n++;
    channel_list = XmCreateScrolledList(left_pane, "channelList", args, n);
    XtManageChild(channel_list);
    XtAddCallback(channel_list, XmNbrowseSelectionCallback,
                  channel_select_cb, NULL);

    /* Right pane: Member list */
    n = 0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNwidth, 160); n++;
    right_pane = XmCreateFrame(form, "rightFrame", args, n);
    XtManageChild(right_pane);

    n = 0;
    XtSetArg(args[n], XmNselectionPolicy, XmBROWSE_SELECT); n++;
    XtSetArg(args[n], XmNvisibleItemCount, 20); n++;
    member_list = XmCreateScrolledList(right_pane, "memberList", args, n);
    XtManageChild(member_list);

    /* Center pane: Messages + Input */
    n = 0;
    XtSetArg(args[n], XmNtopAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNbottomAttachment, XmATTACH_FORM); n++;
    XtSetArg(args[n], XmNleftAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNleftWidget, left_pane); n++;
    XtSetArg(args[n], XmNrightAttachment, XmATTACH_WIDGET); n++;
    XtSetArg(args[n], XmNrightWidget, right_pane); n++;
    center_pane = XtVaCreateManagedWidget("centerPane",
        xmFormWidgetClass, form,
        XmNtopAttachment, XmATTACH_FORM,
        XmNbottomAttachment, XmATTACH_FORM,
        XmNleftAttachment, XmATTACH_WIDGET,
        XmNleftWidget, left_pane,
        XmNrightAttachment, XmATTACH_WIDGET,
        XmNrightWidget, right_pane,
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
    status_label = XtVaCreateManagedWidget("statusLabel",
        xmLabelWidgetClass, center_pane,
        XmNbottomAttachment, XmATTACH_WIDGET,
        XmNbottomWidget, input_text,
        XmNleftAttachment, XmATTACH_FORM,
        XmNrightAttachment, XmATTACH_FORM,
        XmNalignment, XmALIGNMENT_BEGINNING,
        XmNlabelString, XmStringCreateLocalized("Starting..."),
        NULL);

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
    XtSetArg(args[n], XmNscrollVertical, True); n++;
    XtSetArg(args[n], XmNrows, 20); n++;
    XtSetArg(args[n], XmNcolumns, 60); n++;
    message_text = XmCreateScrolledText(center_pane, "messageText", args, n);
    XtManageChild(message_text);

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

    /* Start connection check (will trigger guild fetch + polling) */
    XtAppAddTimeOut(app_context, 500, check_connection, NULL);

    /* Enter Xt event loop */
    XtAppMainLoop(app_context);

    return 0;
}
