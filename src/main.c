/*
 * This file is part of StatusPop.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

// too lazy to spread it into multiple files
// you folks can do it if you want

#define _GNU_SOURCE   // for popen, realpath

#include <limits.h>   // PATH_MAX
#include <sys/time.h> // struct timeval SO_RCVTIMEO
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <linux/wireless.h>

//tunable values
#define REFRESH_MS       1000   // compact refresh rate          
#define EXPAND_REFRESH_MS 2000  // expanded rows refresh rate (heavier reads)
#define WINDOW_WIDTH     400
#define REVEAL_MS        180    // GtkRevealer animation

static const char *CSS =
"window { background: transparent; }"
".popup-box {"
"  background-color: rgba(13, 15, 23, 0.94);"
"  border-radius: 16px;"
"  border: 1px solid rgba(255,255,255,0.08);"
"  padding: 22px 26px 18px 26px;"
"}"
".time-label {"
"  font-size: 52px;"
"  font-weight: 700;"
"  color: #ffffff;"
"  letter-spacing: -2px;"
"}"
".date-label {"
"  font-size: 13px;"
"  color: rgba(255,255,255,0.45);"
"  margin-top: -4px;"
"  margin-bottom: 16px;"
"}"
".sep {"
"  background-color: rgba(255,255,255,0.07);"
"  margin-top: 2px;"
"  margin-bottom: 14px;"
"  min-height: 1px;"
"}"
".section-sep {"
"  background-color: rgba(255,255,255,0.07);"
"  margin-top: 8px;"
"  margin-bottom: 14px;"
"  min-height: 1px;"
"}"
".row-box { margin-bottom: 9px; }"
".row-icon {"
"  font-size: 14px;"
"  min-width: 24px;"
"  color: rgba(255,255,255,0.50);"
"}"
".row-label {"
"  font-size: 12px;"
"  color: rgba(255,255,255,0.38);"
"  min-width: 96px;"
"}"
".row-value {"
"  font-size: 12px;"
"  font-weight: 600;"
"  color: rgba(255,255,255,0.88);"
"}"
".row-value.ok   { color: #4ade80; }"
".row-value.warn { color: #facc15; }"
".row-value.crit { color: #f87171; }"
".row-value.dim  { color: rgba(255,255,255,0.38); }"
".hint-label {"
"  font-size: 10px;"
"  color: rgba(255,255,255,0.18);"
"  margin-top: 12px;"
"}";

typedef struct {
    int      level;
    gboolean charging;
    gboolean present;
} BatteryInfo;

typedef struct {
    char     ssid[33];
    gboolean connected;
} WifiInfo;

typedef struct {
    double  cpu_pct;
    double  mem_used_gb;
    double  mem_total_gb;
    int     mem_pct;
} SysInfo;

typedef struct {
    double  used_gb;
    double  total_gb;
    int     pct;
} DiskInfo;

typedef struct {
    char wm[64];
    char workspace[64];
} DesktopInfo;

typedef struct {
    char     title[192];
    char     artist[128];
    gboolean active;
    gboolean playing;
} MediaInfo;

typedef struct {
    char serial[256];
    int  serial_count;
    char storage[256];
    int  storage_count;
} UsbInfo;

typedef struct {
    char   str[48];
    double load1;
} UptimeInfo;

static void read_battery(BatteryInfo *b)
{
    b->level = -1; b->charging = FALSE; b->present = FALSE;
    const char *base = "/sys/class/power_supply";
    GDir *dir = g_dir_open(base, 0, NULL);
    if (!dir) return;
    const gchar *entry;
    while ((entry = g_dir_read_name(dir)) != NULL) {
        char path[256]; FILE *f;
        snprintf(path, sizeof(path), "%s/%s/type", base, entry);
        f = fopen(path, "r");
        if (!f) continue;
        char type[32] = {0};
        int _r = fscanf(f, "%31s", type); (void)_r;
        fclose(f);
        if (g_ascii_strcasecmp(type, "Battery") != 0) continue;
        b->present = TRUE;
        snprintf(path, sizeof(path), "%s/%s/capacity", base, entry);
        f = fopen(path, "r");
        if (f) { int _c = fscanf(f, "%d", &b->level); (void)_c; fclose(f); }
        snprintf(path, sizeof(path), "%s/%s/status", base, entry);
        f = fopen(path, "r");
        if (f) {
            char st[32] = {0};
            int _s = fscanf(f, "%31s", st); (void)_s;
            fclose(f);
            b->charging = (g_ascii_strcasecmp(st, "Charging") == 0 ||
                           g_ascii_strcasecmp(st, "Full")     == 0);
        }
        break;
    }
    g_dir_close(dir);
}

static void read_wifi(WifiInfo *w)
{
    w->connected = FALSE; w->ssid[0] = '\0';
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;
    const char *ifaces[] = {
        "wlan0","wlan1","wlp2s0","wlp3s0","wlp4s0","wlo1","wls1","phy0-sta0", NULL
    };
    struct iwreq req;
    char buf[IW_ESSID_MAX_SIZE + 1];
    for (int i = 0; ifaces[i]; i++) {
        memset(&req, 0, sizeof(req));
        strncpy(req.ifr_name, ifaces[i], IFNAMSIZ - 1);
        memset(buf, 0, sizeof(buf));
        req.u.essid.pointer = buf;
        req.u.essid.length  = IW_ESSID_MAX_SIZE;
        if (ioctl(sock, SIOCGIWESSID, &req) == 0 && req.u.essid.length > 0) {
            size_t len = req.u.essid.length < 32u ? req.u.essid.length : 32u;
            memcpy(w->ssid, buf, len);
            w->ssid[len] = '\0';
            if (w->ssid[0]) { w->connected = TRUE; break; }
        }
    }
    close(sock);
}

typedef struct {
    long long user, nice, sys, idle, iowait, irq, softirq, steal;
} CpuStat;
static CpuStat g_prev_cpu;

static gboolean read_cpu_stat(CpuStat *s)
{
    memset(s, 0, sizeof(*s));
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return FALSE;
    int n = fscanf(f, "cpu %lld %lld %lld %lld %lld %lld %lld %lld",
                   &s->user,&s->nice,&s->sys,&s->idle,
                   &s->iowait,&s->irq,&s->softirq,&s->steal);
    fclose(f);
    return (n == 8);
}

static double cpu_pct(const CpuStat *a, const CpuStat *b)
{
    long long ia = a->idle + a->iowait,  ib = b->idle + b->iowait;
    long long ta = a->user+a->nice+a->sys+ia+a->irq+a->softirq+a->steal;
    long long tb = b->user+b->nice+b->sys+ib+b->irq+b->softirq+b->steal;
    long long dt = tb - ta, di = ib - ia;
    if (dt <= 0) return 0.0;
    return 100.0 * (double)(dt - di) / (double)dt;
}

static void read_sysinfo(SysInfo *s)
{
    CpuStat cur;
    if (read_cpu_stat(&cur)) { s->cpu_pct = cpu_pct(&g_prev_cpu, &cur); g_prev_cpu = cur; }
    else                       s->cpu_pct = 0.0;

    s->mem_used_gb = s->mem_total_gb = 0; s->mem_pct = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    long long total=0, mfree=0, buffers=0, cached=0, srec=0;
    char key[64], unit[16]; long long val;
    while (fscanf(f, "%63s %lld %15s\n", key, &val, unit) >= 2) {
        if      (!strcmp(key,"MemTotal:"))     total   = val;
        else if (!strcmp(key,"MemFree:"))      mfree   = val;
        else if (!strcmp(key,"Buffers:"))      buffers = val;
        else if (!strcmp(key,"Cached:"))       cached  = val;
        else if (!strcmp(key,"SReclaimable:")) srec    = val;
    }
    fclose(f);
    long long used = total - mfree - buffers - cached - srec;
    if (used < 0) used = 0;
    s->mem_total_gb = total / (1024.0*1024.0);
    s->mem_used_gb  = used  / (1024.0*1024.0);
    if (total > 0) s->mem_pct = (int)(100.0*used/total);
}

static void read_disk(DiskInfo *d)
{
    d->used_gb = d->total_gb = 0; d->pct = 0;
    struct statvfs st;
    if (statvfs("/", &st) != 0) return;
    unsigned long long total = (unsigned long long)st.f_blocks * st.f_frsize;
    unsigned long long avail = (unsigned long long)st.f_bavail * st.f_frsize;
    unsigned long long used  = total - avail;
    d->total_gb = total / (1024.0*1024.0*1024.0);
    d->used_gb  = used  / (1024.0*1024.0*1024.0);
    if (total > 0) d->pct = (int)(100.0*used/total);
}

static gboolean ipc_get_workspace(const char *sockpath, char *out, size_t outsz)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return FALSE;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sockpath, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd); return FALSE;
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    uint8_t msg[14] = {'i','3','-','i','p','c', 0,0,0,0, 1,0,0,0};
    if (write(fd, msg, sizeof(msg)) != (ssize_t)sizeof(msg)) {
        close(fd); return FALSE;
    }

    uint8_t hdr[14];
    if (read(fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        close(fd); return FALSE;
    }

    uint32_t jlen;
    memcpy(&jlen, hdr + 6, 4);
    if (jlen == 0 || jlen > 131072) { close(fd); return FALSE; }

    char *json = malloc(jlen + 1);
    if (!json) { close(fd); return FALSE; }

    size_t got = 0;
    while (got < (size_t)jlen) {
        ssize_t n = read(fd, json + got, jlen - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got == 0) { free(json); return FALSE; }
    json[got] = '\0';

    char *focused = strstr(json, "\"focused\":true");
    if (!focused) { free(json); out[0]='\0'; return TRUE; }

    char *p = focused;
    while (p > json) {
        if (strncmp(p, "\"name\":\"", 8) == 0) break;
        p--;
    }
    if (p == json) { free(json); return FALSE; }
    p += 8;
    char *end = strchr(p, '"');
    if (!end) { free(json); return FALSE; }
    size_t n = (size_t)(end - p);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    free(json);
    return TRUE;
}

static void read_desktop(DesktopInfo *d)
{
    const char *xdg     = getenv("XDG_CURRENT_DESKTOP");
    const char *session = getenv("DESKTOP_SESSION");
    strncpy(d->wm, xdg ? xdg : (session ? session : "Unknown"), sizeof(d->wm)-1);
    d->wm[sizeof(d->wm)-1] = '\0';
    strncpy(d->workspace, "-", sizeof(d->workspace)-1);

    const char *i3sock = getenv("I3SOCK");
    if (i3sock && ipc_get_workspace(i3sock, d->workspace, sizeof(d->workspace)))
        return;

    const char *swaysock = getenv("SWAYSOCK");
    if (swaysock && ipc_get_workspace(swaysock, d->workspace, sizeof(d->workspace)))
        return;
	
    //TODO: Add a warning if not a supported DE/WM
}

static void read_media(MediaInfo *m)
{
    m->active = FALSE; m->playing = FALSE;
    m->title[0] = m->artist[0] = '\0';

    GError *err = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!bus) { if (err) g_error_free(err); return; }

    GVariant *res = g_dbus_connection_call_sync(bus,
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "ListNames",
        NULL, G_VARIANT_TYPE("(as)"),
        G_DBUS_CALL_FLAGS_NONE, 1500, NULL, &err);
    if (!res) { if (err) g_error_free(err); g_object_unref(bus); return; }

    GVariant *names_v;
    g_variant_get(res, "(@as)", &names_v);

    char player[256] = {0};
    GVariantIter *it = g_variant_iter_new(names_v);
    const char *nm;
    while (g_variant_iter_next(it, "&s", &nm)) {
        if (g_str_has_prefix(nm, "org.mpris.MediaPlayer2.")) {
            strncpy(player, nm, sizeof(player)-1); break;
        }
    }
    g_variant_iter_free(it);
    g_variant_unref(names_v);
    g_variant_unref(res);

    if (!player[0]) { g_object_unref(bus); return; }
    m->active = TRUE;

    GVariant *sv = g_dbus_connection_call_sync(bus, player,
        "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.mpris.MediaPlayer2.Player", "PlaybackStatus"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 1500, NULL, NULL);
    if (sv) {
        GVariant *inner; g_variant_get(sv, "(v)", &inner);
        m->playing = strcmp(g_variant_get_string(inner, NULL), "Playing") == 0;
        g_variant_unref(inner); g_variant_unref(sv);
    }

    GVariant *mv = g_dbus_connection_call_sync(bus, player,
        "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.mpris.MediaPlayer2.Player", "Metadata"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 1500, NULL, NULL);
    if (mv) {
        GVariant *dict; g_variant_get(mv, "(v)", &dict);

        GVariant *tv = g_variant_lookup_value(dict, "xesam:title", G_VARIANT_TYPE_STRING);
        if (tv) {
            strncpy(m->title, g_variant_get_string(tv, NULL), sizeof(m->title)-1);
            m->title[sizeof(m->title)-1] = '\0';
            g_variant_unref(tv);
        }

        GVariant *av = g_variant_lookup_value(dict, "xesam:artist", G_VARIANT_TYPE_STRING_ARRAY);
        if (av) {
            gsize n; const gchar **artists = g_variant_get_strv(av, &n);
            if (n > 0) {
                strncpy(m->artist, artists[0], sizeof(m->artist)-1);
                m->artist[sizeof(m->artist)-1] = '\0';
            }
            g_free(artists); g_variant_unref(av);
        }

        g_variant_unref(dict); g_variant_unref(mv);
    }

    g_object_unref(bus);
}

static void usb_append(char *buf, size_t bufsz, const char *s)
{
    if (buf[0]) strncat(buf, "  ", bufsz - strlen(buf) - 1);
    strncat(buf, s, bufsz - strlen(buf) - 1);
}

static void read_usb(UsbInfo *u)
{
    u->serial_count = u->storage_count = 0;
    u->serial[0] = u->storage[0] = '\0';

    // TODO: this may be incompatible for other OS
    GDir *dev = g_dir_open("/dev", 0, NULL);
    if (dev) {
        const char *name;
        while ((name = g_dir_read_name(dev)) != NULL) {
            if (g_str_has_prefix(name, "ttyUSB") || g_str_has_prefix(name, "ttyACM")) {
                char full[64];
                snprintf(full, sizeof(full), "/dev/%s", name);
                usb_append(u->serial, sizeof(u->serial), full);
                u->serial_count++;
            }
        }
        g_dir_close(dev);
    }

    GDir *bypath = g_dir_open("/dev/disk/by-path", 0, NULL);
    if (bypath) {
        const char *name;
        while ((name = g_dir_read_name(bypath)) != NULL) {
            if (!strstr(name, "usb"))   continue;
            if (strstr(name, "-part"))  continue; 
            char full[PATH_MAX];
            snprintf(full, sizeof(full), "/dev/disk/by-path/%s", name);
            char resolved[PATH_MAX];
            if (realpath(full, resolved)) {
                usb_append(u->storage, sizeof(u->storage), resolved);
                u->storage_count++;
            }
        }
        g_dir_close(bypath);
    }
}

static void read_uptime(UptimeInfo *u)
{
    u->str[0] = '\0'; u->load1 = 0;
    FILE *f = fopen("/proc/uptime", "r");
    if (f) {
        double secs;
        if (fscanf(f, "%lf", &secs) == 1) {
            long long s = (long long)secs;
            long long days = s / 86400, hours = (s % 86400) / 3600, mins = (s % 3600) / 60;
            if (days > 0)
                snprintf(u->str, sizeof(u->str), "%lldd %lldh %lldm", days, hours, mins);
            else if (hours > 0)
                snprintf(u->str, sizeof(u->str), "%lldh %lldm", hours, mins);
            else
                snprintf(u->str, sizeof(u->str), "%lldm", mins);
        }
        fclose(f);
    }
    f = fopen("/proc/loadavg", "r");
    if (f) {
        int _r = fscanf(f, "%lf", &u->load1); (void)_r;
        fclose(f);
    }
}

static const char *level_cls(int pct)
{
    if (pct >= 40) return "ok";
    if (pct >= 15) return "warn";
    return "crit";
}
static const char *inv_cls(int pct) /* high = bad */
{
    if (pct < 60) return "ok";
    if (pct < 85) return "warn";
    return "crit";
}

typedef struct {
    GtkWidget *window;
    GtkWidget *time_lbl, *date_lbl;
    GtkWidget *bat_val;
    GtkWidget *cpu_val;
    GtkWidget *mem_val;
    GtkWidget *revealer;
    GtkWidget *wifi_val;
    GtkWidget *disk_val;
    GtkWidget *desktop_val;
    GtkWidget *workspace_val;
    GtkWidget *media_val;
    GtkWidget *usb_serial_val;
    GtkWidget *usb_storage_val;
    GtkWidget *uptime_val;
    GtkWidget *load_val;
    GtkWidget *hint_lbl;
    gboolean   expanded;
    guint      compact_timer;
    guint      expand_timer;
} App;

static App g;

static GtkWidget *make_row(const char *icon, const char *label,
                            GtkWidget **out_val, GtkWidget **out_fill)
{
    GtkWidget *row  = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(row, "row-box");

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    GtkWidget *ic = gtk_label_new(icon);
    gtk_widget_add_css_class(ic, "row-icon");
    gtk_label_set_xalign(GTK_LABEL(ic), 0);
    gtk_box_append(GTK_BOX(hbox), ic);

    GtkWidget *kl = gtk_label_new(label);
    gtk_widget_add_css_class(kl, "row-label");
    gtk_label_set_xalign(GTK_LABEL(kl), 0);
    gtk_box_append(GTK_BOX(hbox), kl);

    GtkWidget *vl = gtk_label_new("-");
    gtk_widget_add_css_class(vl, "row-value");
    gtk_label_set_xalign(GTK_LABEL(vl), 0);
    gtk_label_set_ellipsize(GTK_LABEL(vl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(vl, TRUE);
    gtk_box_append(GTK_BOX(hbox), vl);
    if (out_val) *out_val = vl;

    gtk_box_append(GTK_BOX(row), hbox);

    if (out_fill) {
        GtkWidget *track = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(track, "bar-track");
        GtkWidget *fill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(fill, "bar-fill");
        gtk_widget_add_css_class(fill, "ok");
        gtk_widget_set_size_request(fill, 0, -1);
        gtk_box_append(GTK_BOX(track), fill);
        gtk_box_append(GTK_BOX(row), track);
        *out_fill = fill;
    }
    return row;
}

static void set_val(GtkWidget *lbl, const char *text,
                    const char *cls)   /* "ok","warn","crit","dim",NULL */
{
    gtk_label_set_text(GTK_LABEL(lbl), text);
    gtk_widget_remove_css_class(lbl, "ok");
    gtk_widget_remove_css_class(lbl, "warn");
    gtk_widget_remove_css_class(lbl, "crit");
    gtk_widget_remove_css_class(lbl, "dim");
    if (cls) gtk_widget_add_css_class(lbl, cls);
}

typedef struct { MediaInfo   m; } MediaResult;
typedef struct { DesktopInfo d; } DesktopResult;

static gboolean apply_media_ui(gpointer data)
{
    if (!g.window) { g_free(data); return G_SOURCE_REMOVE; }
    MediaInfo *m = &((MediaResult *)data)->m;
    if (!m->active) {
        set_val(g.media_val, "Nothing playing", "dim");
    } else {
        char buf[512]; 
        const char *st = m->playing ? "▶" : "⏸";
        if      (m->title[0] && m->artist[0])
            snprintf(buf, sizeof(buf), "%s  %s - %s", st, m->artist, m->title);
        else if (m->title[0])
            snprintf(buf, sizeof(buf), "%s  %s", st, m->title);
        else
            snprintf(buf, sizeof(buf), "%s  (unknown)", st);
        set_val(g.media_val, buf, m->playing ? "ok" : NULL);
    }
    g_free(data);
    return G_SOURCE_REMOVE;
}

static gpointer media_worker(gpointer _u)
{
    (void)_u;
    MediaResult *r = g_new0(MediaResult, 1);
    read_media(&r->m);
    g_idle_add(apply_media_ui, r);
    return NULL;
}

static gboolean apply_desktop_ui(gpointer data)
{
    if (!g.window) { g_free(data); return G_SOURCE_REMOVE; }
    DesktopInfo *d = &((DesktopResult *)data)->d;
    set_val(g.desktop_val,   d->wm,       NULL);
    set_val(g.workspace_val, d->workspace, NULL);
    g_free(data);
    return G_SOURCE_REMOVE;
}

static gpointer desktop_worker(gpointer _u)
{
    (void)_u;
    DesktopResult *r = g_new0(DesktopResult, 1);
    read_desktop(&r->d);
    g_idle_add(apply_desktop_ui, r);
    return NULL;
}

static gboolean refresh_compact(gpointer _u)
{
    (void)_u;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (t) {
        char ts[16], ds[64];
        strftime(ts, sizeof(ts), "%H:%M", t);
        strftime(ds, sizeof(ds), "%A, %B %-d", t);
        gtk_label_set_text(GTK_LABEL(g.time_lbl), ts);
        gtk_label_set_text(GTK_LABEL(g.date_lbl), ds);
    }

    BatteryInfo b; read_battery(&b);
    if (!b.present) {
        set_val(g.bat_val, "No battery", "dim");
    } else {
        char buf[48];
        const char *cls = (b.level < 0) ? "ok" : level_cls(b.level);
        if (b.level < 0)      snprintf(buf, sizeof(buf), "Unknown");
        else if (b.charging)  snprintf(buf, sizeof(buf), "%d%%  ⚡", b.level);
        else                   snprintf(buf, sizeof(buf), "%d%%", b.level);
        set_val(g.bat_val, buf, cls);
    }

    SysInfo s; read_sysinfo(&s);

    char buf[64];
    const char *cc = inv_cls((int)s.cpu_pct);
    snprintf(buf, sizeof(buf), "%.1f%%", s.cpu_pct);
    set_val(g.cpu_val, buf, cc);

    const char *mc = inv_cls(s.mem_pct);
    snprintf(buf, sizeof(buf), "%.1f / %.1f GB  (%d%%)",
             s.mem_used_gb, s.mem_total_gb, s.mem_pct);
    set_val(g.mem_val, buf, mc);

    return G_SOURCE_CONTINUE;
}

static gboolean refresh_expanded(gpointer _u)
{
    (void)_u;
    if (!g.expanded) return G_SOURCE_CONTINUE;

    WifiInfo w; read_wifi(&w);
    if (w.connected) set_val(g.wifi_val, w.ssid, "ok");
    else             set_val(g.wifi_val, "Not connected", "crit");

    DiskInfo d; read_disk(&d);
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.1f / %.1f GB  (%d%%)",
                 d.used_gb, d.total_gb, d.pct);
        set_val(g.disk_val, buf, inv_cls(d.pct));
    }

    g_thread_unref(g_thread_new("desktop", desktop_worker, NULL));

    g_thread_unref(g_thread_new("media", media_worker, NULL));

    UsbInfo usb; read_usb(&usb);
    if (usb.serial_count > 0) set_val(g.usb_serial_val, usb.serial, NULL);
    else                       set_val(g.usb_serial_val, "None", "dim");

    if (usb.storage_count > 0) set_val(g.usb_storage_val, usb.storage, NULL);
    else                        set_val(g.usb_storage_val, "None", "dim");

    UptimeInfo up; read_uptime(&up);
    set_val(g.uptime_val, up.str[0] ? up.str : "-", NULL);
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f", up.load1);
        const char *lc = up.load1 < 2.0 ? "ok" : up.load1 < 4.0 ? "warn" : "crit";
        set_val(g.load_val, buf, lc);
    }

    return G_SOURCE_CONTINUE;
}

static void toggle_expanded(void)
{
    g.expanded = !g.expanded;
    gtk_revealer_set_reveal_child(GTK_REVEALER(g.revealer), g.expanded);

    if (g.expanded) {
        refresh_expanded(NULL);
        if (!g.expand_timer)
            g.expand_timer = g_timeout_add(EXPAND_REFRESH_MS, refresh_expanded, NULL);
        gtk_label_set_text(GTK_LABEL(g.hint_lbl), "Enter - less  ·  Esc - close");
    } else {
        if (g.expand_timer) { g_source_remove(g.expand_timer); g.expand_timer = 0; }
        gtk_label_set_text(GTK_LABEL(g.hint_lbl), "Enter - more  ·  Esc - close");
    }
}

static void on_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (g.compact_timer) { g_source_remove(g.compact_timer); g.compact_timer = 0; }
    if (g.expand_timer)  { g_source_remove(g.expand_timer);  g.expand_timer  = 0; }
    g.window = NULL;
}

static gboolean on_key(GtkEventControllerKey *ctrl, guint kv, guint kc,
                        GdkModifierType state, gpointer d)
{
    (void)ctrl; (void)kc; (void)state; (void)d;
    if (kv == GDK_KEY_Escape) { gtk_window_close(GTK_WINDOW(g.window)); return TRUE; }
    if (kv == GDK_KEY_Return || kv == GDK_KEY_KP_Enter) { toggle_expanded(); return TRUE; }
    return FALSE;
}

static void on_click(GtkGestureClick *gc, int n, double x, double y, gpointer d)
{
    (void)gc; (void)n; (void)x; (void)y; (void)d;
    gtk_window_close(GTK_WINDOW(g.window));
}

static GtkWidget *hsep(const char *cls)
{
    GtkWidget *s = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_add_css_class(s, cls);
    return s;
}

static void activate(GtkApplication *app, gpointer _u)
{
    (void)_u;

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, CSS);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_USER);
    g_object_unref(css);

    GtkWidget *win = gtk_application_window_new(app);
    g.window = win;
    gtk_window_set_title(GTK_WINDOW(win), "statuspop");
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_widget_set_size_request(win, WINDOW_WIDTH, -1);

    GtkEventController *kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key), NULL);
    gtk_widget_add_controller(win, kc);
    
    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_click), NULL);
    gtk_widget_add_controller(win, GTK_EVENT_CONTROLLER(click));

    g_signal_connect(win, "destroy", G_CALLBACK(on_destroy), NULL);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(root, "popup-box");
    gtk_window_set_child(GTK_WINDOW(win), root);

    g.time_lbl = gtk_label_new("00:00");
    gtk_widget_add_css_class(g.time_lbl, "time-label");
    gtk_label_set_xalign(GTK_LABEL(g.time_lbl), 0);
    gtk_box_append(GTK_BOX(root), g.time_lbl);

    g.date_lbl = gtk_label_new("-");
    gtk_widget_add_css_class(g.date_lbl, "date-label");
    gtk_label_set_xalign(GTK_LABEL(g.date_lbl), 0);
    gtk_box_append(GTK_BOX(root), g.date_lbl);

    gtk_box_append(GTK_BOX(root), hsep("sep"));

    gtk_box_append(GTK_BOX(root), make_row("🔋","Battery",   &g.bat_val, NULL));
    gtk_box_append(GTK_BOX(root), make_row("⚙ ","CPU",       &g.cpu_val, NULL));
    gtk_box_append(GTK_BOX(root), make_row("💾","Memory",    &g.mem_val, NULL));

    g.revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(g.revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(g.revealer), REVEAL_MS);
    gtk_revealer_set_reveal_child(GTK_REVEALER(g.revealer), FALSE);
    gtk_box_append(GTK_BOX(root), g.revealer);

    GtkWidget *expbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_revealer_set_child(GTK_REVEALER(g.revealer), expbox);

    gtk_box_append(GTK_BOX(expbox), hsep("section-sep"));

    gtk_box_append(GTK_BOX(expbox), make_row("📶","Network",    &g.wifi_val,       NULL));
    gtk_box_append(GTK_BOX(expbox), make_row("💿","Disk  /",    &g.disk_val,       NULL));
    gtk_box_append(GTK_BOX(expbox), make_row("🖥 ","Desktop",   &g.desktop_val,    NULL));
    gtk_box_append(GTK_BOX(expbox), make_row("⬡ ","Workspace",  &g.workspace_val,  NULL));
    gtk_box_append(GTK_BOX(expbox), make_row("🎵","Media",      &g.media_val,      NULL));
    gtk_box_append(GTK_BOX(expbox), make_row("🔌","USB serial", &g.usb_serial_val, NULL));
    gtk_box_append(GTK_BOX(expbox), make_row("💽","USB storage",&g.usb_storage_val,NULL));
    gtk_box_append(GTK_BOX(expbox), make_row("⏱ ","Uptime",    &g.uptime_val,     NULL));
    gtk_box_append(GTK_BOX(expbox), make_row("📊","Load  1m",   &g.load_val,       NULL));

    g.hint_lbl = gtk_label_new("Enter - more  ·  Esc - close");
    gtk_widget_add_css_class(g.hint_lbl, "hint-label");
    gtk_label_set_xalign(GTK_LABEL(g.hint_lbl), 0.5);
    gtk_box_append(GTK_BOX(root), g.hint_lbl);

    refresh_compact(NULL);
    g.compact_timer = g_timeout_add(REFRESH_MS, refresh_compact, NULL);

    gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char **argv)
{
    GtkApplication *app = gtk_application_new("io.statuspop",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return rc;
}
