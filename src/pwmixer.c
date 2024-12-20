#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdarg.h>
#include <math.h>
#include <errno.h>
#include <curses.h>

#include <spa/utils/result.h>
#include <spa/utils/json.h>
#include <spa/param/props.h>
#include <spa/param/audio/format-utils.h>
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>


#define VOLUME_ZERO ((uint32_t) 0U)
#define VOLUME_FULL ((uint32_t) 0x1000U)
#define VOLUME_MAX  ((uint32_t) 0xA000U)

struct volume {
    uint32_t n_channels;
    uint32_t values[SPA_AUDIO_MAX_CHANNELS];
};

enum volume_method {
    VOLUME_METHOD_LINEAR,
    VOLUME_METHOD_CUBIC,
};

enum node_flag {
    NODE_FLAG_SINK = 1 << 0,
    NODE_FLAG_SOURCE = 1 << 1,
};

struct ctl {
    struct pw_thread_loop *mainloop;
    struct pw_context *context;
    struct spa_system *system;

    struct pw_core *core;

    struct pw_registry *registry;
    struct spa_hook registry_listener;

    struct pw_metadata *metadata;
    struct spa_hook metadata_listener;

    int fd;
    int pending_seq;
    int last_seq;
    int error;

    enum volume_method volume_method;

    char default_sink[1024];
    char default_source[1024];
    struct spa_list refs;
    uint32_t n_refs;
    uint32_t cursor;
    enum node_flag node_flags;
};

struct intf_info {
    const char *type;
    uint32_t version;
    const void *events;
};

struct intf {
    struct ctl *ctl;

    struct spa_list ref;
    struct pw_proxy *proxy;
    struct spa_hook proxy_listener;
    struct spa_hook object_listener;

    struct pw_properties *props;
    uint32_t id;
    uint32_t perms;
    const struct intf_info *info;

    union {
        struct {
            enum node_flag flags;
            uint32_t device_id;
            uint32_t profile_device_id;
            float volume;

            bool mute;
            struct volume channel_volume;
        } node;
        struct {
            uint32_t active_route_input;
            uint32_t active_route_output;
        } device;
        struct {
            enum pw_direction direction;
            uint32_t node;
        } port;
        struct {
            uint32_t output_port;
            uint32_t input_port;
        } link;
    };
};

static FILE *log_file;

static void log_debug(const char *format, ...)
{
    if (log_file == NULL)
        return;

    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);

    fprintf(log_file, "\n");
    fflush(log_file);
}

static int bound_int(int val, int min, int max)
{
    if (val < min)
        return min;
    else if (val > max)
        return max;
    else
        return val;
}

static uint32_t volume_from_linear(float vol, enum volume_method method)
{
    switch (method) {
    case VOLUME_METHOD_CUBIC:
        vol = cbrtf(vol);
        break;
    }
    return bound_int(lroundf(vol * VOLUME_FULL), VOLUME_ZERO, VOLUME_MAX);
}

static float volume_to_linear(uint32_t vol, enum volume_method method)
{
    float v = ((float)vol) / VOLUME_FULL;

    switch (method) {
    case VOLUME_METHOD_CUBIC:
        v = v * v * v;
        break;
    }
    return v;
}

static struct intf *find_node(struct ctl *ctl, uint32_t id,
    const char *name, const char *type)
{
    struct intf *intf;
    const char *str;

    spa_list_for_each(intf, &ctl->refs, ref) {
        if (intf->id == id &&
            (type == NULL || spa_streq(intf->info->type, type)))
        {
            return intf;
        }
        if (name != NULL && name[0] != '\0' &&
            (str = pw_properties_get(intf->props, PW_KEY_NODE_NAME)) != NULL &&
            spa_streq(name, str))
        {
            return intf;
        }
    }
    return NULL;
}

static struct intf *find_curnode(struct ctl *ctl)
{
    struct intf *intf;
    uint32_t i = 0;

    spa_list_for_each(intf, &ctl->refs, ref) {
        if (!SPA_FLAG_IS_SET(intf->node.flags, ctl->node_flags))
            continue;
        if (ctl->cursor == i)
            return intf;
        i++;
    }
    return NULL;
}

static struct spa_pod *build_volume_mute(struct spa_pod_builder *b,
    struct volume *volume, int *mute, int volume_method)
{
    struct spa_pod_frame f[1];

    spa_pod_builder_push_object(b, &f[0],
        SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    if (volume != NULL) {
        float values[SPA_AUDIO_MAX_CHANNELS];

        for (uint32_t i = 0; i < volume->n_channels; i++)
            values[i] = volume_to_linear(volume->values[i], volume_method);

        spa_pod_builder_prop(b, SPA_PROP_channelVolumes, 0);
        spa_pod_builder_array(b, sizeof(float),
            SPA_TYPE_Float, volume->n_channels, values);
    }
    if (mute != NULL) {
        spa_pod_builder_prop(b, SPA_PROP_mute, 0);
        spa_pod_builder_bool(b, *mute ? true : false);
    }
    return spa_pod_builder_pop(b, &f[0]);
}

static int set_volume_mute(struct intf *intf, struct volume *volume, int *mute)
{
    struct intf *dintf;
    struct ctl *ctl = intf->ctl;
    uint32_t id = SPA_ID_INVALID, device_id = SPA_ID_INVALID;
    char buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    struct spa_pod_frame f[2];
    struct spa_pod *param;

    if ((dintf = find_node(ctl, intf->node.device_id, NULL, PW_TYPE_INTERFACE_Device)) != NULL) {
        if (SPA_FLAG_IS_SET(intf->node.flags, NODE_FLAG_SINK))
            id = dintf->device.active_route_output;
        else if (SPA_FLAG_IS_SET(intf->node.flags, NODE_FLAG_SOURCE))
            id = dintf->device.active_route_input;
        device_id = intf->node.profile_device_id;

        log_debug("route #%d, #%d id:%d device_id:%d", intf->id,
            dintf->id, id, device_id);
    }

    if (id != SPA_ID_INVALID && device_id != SPA_ID_INVALID && dintf != NULL) {
        if (!SPA_FLAG_IS_SET(dintf->perms, PW_PERM_W | PW_PERM_X))
            return -EPERM;

        spa_pod_builder_push_object(&b, &f[0],
            SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route);
        spa_pod_builder_add(&b,
            SPA_PARAM_ROUTE_index, SPA_POD_Int(id),
            SPA_PARAM_ROUTE_device, SPA_POD_Int(device_id),
            SPA_PARAM_ROUTE_save, SPA_POD_Bool(true),
            0);

        spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_props, 0);
        build_volume_mute(&b, volume, mute, ctl->volume_method);
        param = spa_pod_builder_pop(&b, &f[0]);

        log_debug("set device #%d volume/mute for node #%d",
            intf->node.device_id, intf->id);
        pw_device_set_param((struct pw_device*)dintf->proxy,
            SPA_PARAM_Route, 0, param);
    } else {
        if (!SPA_FLAG_IS_SET(intf->perms, PW_PERM_W | PW_PERM_X))
            return -EPERM;

        param = build_volume_mute(&b, volume, mute, ctl->volume_method);

        log_debug("set node #%d volume/mute", intf->id);
        pw_node_set_param((struct pw_node*)intf->proxy,
            SPA_PARAM_Props, 0, param);
    }
    return 0;
}

static void ctl_pipewire_free(struct ctl *ctl)
{
    if (ctl == NULL)
        return;

    if (ctl->mainloop)
        pw_thread_loop_stop(ctl->mainloop);
    if (ctl->registry)
        pw_proxy_destroy((struct pw_proxy*)ctl->registry);
    if (ctl->context)
        pw_context_destroy(ctl->context);
    if (ctl->fd >= 0)
        spa_system_close(ctl->system, ctl->fd);
    if (ctl->mainloop)
        pw_thread_loop_destroy(ctl->mainloop);
}

/** curses */

static void init_curses(struct ctl *ctl)
{
    setlocale(LC_ALL, "");

    initscr();              // Start curses mode
    cbreak();               // Line buffering disabled
    noecho();               // Do not echo while typing
    keypad(stdscr, true);   // Enable special keys
    curs_set(0);            // Hide cursor

    if (has_colors()) {
        start_color();

        init_pair(1, COLOR_RED,     COLOR_BLACK);
        init_pair(2, COLOR_GREEN,   COLOR_BLACK);
        init_pair(3, COLOR_YELLOW,  COLOR_BLACK);
        init_pair(4, COLOR_BLUE,    COLOR_BLACK);
        init_pair(5, COLOR_CYAN,    COLOR_BLACK);
        init_pair(6, COLOR_MAGENTA, COLOR_BLACK);
        init_pair(7, COLOR_WHITE,   COLOR_BLACK);
    }
}

static void redraw(struct ctl *ctl)
{
    struct intf *intf;
    uint32_t i = 0, cur = 2;
    const char *str;

    move(0, 1);
    if (ctl->node_flags & NODE_FLAG_SINK)
        attron(A_BOLD);
    else
        attroff(A_BOLD);
    printw("F1 Output");
    attroff(A_BOLD);

    printw("  ");
    if (ctl->node_flags & NODE_FLAG_SOURCE)
        attron(A_BOLD);
    else
        attroff(A_BOLD);
    printw("F2 Input");
    attroff(A_BOLD);

    spa_list_for_each(intf, &ctl->refs, ref) {
        if (!SPA_FLAG_IS_SET(intf->node.flags, ctl->node_flags))
            continue;
        if (intf->node.channel_volume.n_channels == 0)
            continue;

        move(cur, 0);
        clrtoeol();
        if (ctl->cursor == i)
            attron(A_BOLD);

        if ((str = pw_properties_get(intf->props, PW_KEY_NODE_NAME)) != NULL &&
            (spa_streq(str, ctl->default_sink) || spa_streq(str, ctl->default_source)))
        {
            move(cur, 1);
            printw("*");
        }

        move(cur, 2);
        printw("%s", pw_properties_get(intf->props, PW_KEY_NODE_DESCRIPTION));

        move(cur, 50);
        clrtoeol();
        printw("%d", lroundf((float)intf->node.channel_volume.values[0] / VOLUME_FULL * 100));

        if (intf->node.mute)
            mvaddstr(cur, 54, "🔇");

        if (ctl->cursor == i)
            attroff(A_BOLD);

        i++;
        cur += 2;
    }

    ctl->n_refs = i;

    refresh();
}

static void toggle_curnode_mute(struct ctl *ctl)
{
    struct intf *intf = find_curnode(ctl);
    int mute;

    if (intf->node.mute)
        mute = 0;
    else
        mute = 1;

    pw_thread_loop_lock(ctl->mainloop);
    set_volume_mute(intf, NULL, &mute);
    pw_thread_loop_unlock(ctl->mainloop);
}

static uint32_t set_curnode_volume(struct ctl *ctl, int volume, bool relative)
{
    struct intf *intf = find_curnode(ctl);
    struct volume vol;

    vol.n_channels = intf->node.channel_volume.n_channels;
    for (uint32_t i = 0; i < vol.n_channels; i++) {
        if (relative)
            vol.values[i] = bound_int(volume + intf->node.channel_volume.values[i],
                VOLUME_ZERO, VOLUME_MAX);
        else
            vol.values[i] = bound_int(volume, VOLUME_ZERO, VOLUME_MAX);
    }

    pw_thread_loop_lock(ctl->mainloop);
    set_volume_mute(intf, &vol, NULL);
    pw_thread_loop_unlock(ctl->mainloop);
}

static void run_curses(struct ctl *ctl)
{
    int ch;

    while ((ch = getch())) {
        switch (ch) {
        case 'j':
        case KEY_DOWN:
            ctl->cursor = (ctl->cursor + 1) % ctl->n_refs;
            break;
        case 'k':
        case KEY_UP:
            ctl->cursor = (ctl->cursor - 1 + ctl->n_refs) % ctl->n_refs;
            break;
        case 'h':
        case KEY_LEFT:
            set_curnode_volume(ctl, -((int)VOLUME_FULL / 100), true);
            break;
        case 'l':
        case KEY_RIGHT:
            set_curnode_volume(ctl, VOLUME_FULL / 100, true);
            break;
        case 'H':
            set_curnode_volume(ctl, -((int)VOLUME_FULL / 10), true);
            break;
        case 'L':
            set_curnode_volume(ctl, VOLUME_FULL / 10, true);
            break;
        case 'm':
            toggle_curnode_mute(ctl);
            break;
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
        {
            uint32_t i = (ch - '0' + 9) % 10 + 1;
            set_curnode_volume(ctl, VOLUME_FULL / 10 * i, false);
            break;
        }
        case KEY_F(1):
            ctl->node_flags = NODE_FLAG_SINK;
            erase();
            break;
        case KEY_F(2):
            ctl->node_flags = NODE_FLAG_SOURCE;
            erase();
            break;
        case 'q':
            ctl_pipewire_free(ctl);
            return;
        }

        redraw(ctl);
    }
}

/** node */

static void parse_props(struct intf *intf, const struct spa_pod *param)
{
    struct ctl *ctl = intf->ctl;
    struct spa_pod_prop *prop;
    struct spa_pod_object *obj = (struct spa_pod_object*)param;

    SPA_POD_OBJECT_FOREACH(obj, prop) {
        switch (prop->key) {
        case SPA_PROP_volume:
            if (spa_pod_get_float(&prop->value, &intf->node.volume) < 0)
                continue;
            log_debug("update node#%d volume", intf->id);
            break;
        case SPA_PROP_mute:
            if (spa_pod_get_bool(&prop->value, &intf->node.mute) < 0)
                continue;
            log_debug("update node#%d mute", intf->id);
            break;
        case SPA_PROP_channelVolumes:
        {
            float channels[SPA_AUDIO_MAX_CHANNELS];
            uint32_t n_channels, i;

            n_channels = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
                channels, SPA_AUDIO_MAX_CHANNELS);

            intf->node.channel_volume.n_channels = n_channels;
            for (i = 0; i < n_channels; i++)
                intf->node.channel_volume.values[i] =
                    volume_from_linear(channels[i], ctl->volume_method);

            log_debug("update node#%d channelVolumes", intf->id);
            break;
        }
        default:
            break;
        }
    }

    redraw(ctl);
}

static void node_event_info(void *data, const struct pw_node_info *info)
{
    struct intf *intf = data;
    const char *str;
    uint32_t i;

    if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS && info->props) {
        if ((str = spa_dict_lookup(info->props, "card.profile.device")))
            intf->node.profile_device_id = atoi(str);
        else
            intf->node.profile_device_id = SPA_ID_INVALID;

        if ((str = spa_dict_lookup(info->props, PW_KEY_DEVICE_ID)))
            intf->node.device_id = atoi(str);
        else
            intf->node.device_id = SPA_ID_INVALID;

        if ((str = spa_dict_lookup(info->props, PW_KEY_MEDIA_CLASS))) {
            if (spa_streq(str, "Audio/Sink"))
                SPA_FLAG_SET(intf->node.flags, NODE_FLAG_SINK);
            else if (spa_streq(str, "Audio/Source"))
                SPA_FLAG_SET(intf->node.flags, NODE_FLAG_SOURCE);
        }

        log_debug("node#%d: device_id:%d profile_device_id:%d", intf->id,
            intf->node.device_id, intf->node.profile_device_id);
    }
    if (info->change_mask & PW_NODE_CHANGE_MASK_PARAMS) {
        for (i = 0; i < info->n_params; i++) {
            if (!(info->params[i].flags & SPA_PARAM_INFO_READ))
                continue;

            switch (info->params[i].id) {
            case SPA_PARAM_Props:
                pw_node_enum_params(intf->proxy,
                    0, info->params[i].id, 0, -1, NULL);
                break;
            default:
                break;
            }
        }
    }
}

static void node_event_param(void *data, int seg,
    uint32_t id, uint32_t index, uint32_t next,
    const struct spa_pod *param)
{
    struct intf *intf = data;

    switch (id) {
    case SPA_PARAM_Props:
        parse_props(intf, param);
        break;
    default:
        break;
    }
}

static const struct pw_node_events node_events = {
    PW_VERSION_NODE_EVENTS,
    .info = node_event_info,
    .param = node_event_param,
};

static const struct intf_info node_info = {
    .type = PW_TYPE_INTERFACE_Node,
    .version = PW_VERSION_NODE,
    .events = &node_events,
};

/** device */

static void device_event_info(void *data, const struct pw_device_info *info)
{
    struct intf *intf = data;
    uint32_t i;

    if (info->change_mask & PW_DEVICE_CHANGE_MASK_PARAMS) {
        for (i = 0; i < info->n_params; i++) {
            if (!(info->params[i].flags & SPA_PARAM_INFO_READ))
                continue;

            switch (info->params[i].id) {
            case SPA_PARAM_Route:
                pw_device_enum_params((struct pw_device*)intf->proxy,
                    0, info->params[i].id, 0, -1, NULL);
                break;
            default:
                break;
            }
        }
    }
}

static void device_event_param(void *data, int seg,
    uint32_t id, uint32_t index, uint32_t next,
    const struct spa_pod *param)
{
    struct intf *intf = data;

    switch (id) {
    case SPA_PARAM_Route:
    {
        uint32_t id, device_id;
        enum spa_direction direction;
        struct spa_pod *props = NULL;

        if (spa_pod_parse_object(param,
            SPA_TYPE_OBJECT_ParamRoute, NULL,
            SPA_PARAM_ROUTE_index, SPA_POD_Int(&id),
            SPA_PARAM_ROUTE_direction, SPA_POD_Id(&direction),
            SPA_PARAM_ROUTE_device, SPA_POD_Int(&device_id),
            SPA_PARAM_ROUTE_props, SPA_POD_OPT_Pod(&props)) < 0)
        {
            return;
        }

        if (direction == SPA_DIRECTION_OUTPUT)
            intf->device.active_route_output = id;
        else
            intf->device.active_route_input = id;

        log_debug("device#%d: active %s route id:%d device:%d", intf->id,
            direction == SPA_DIRECTION_OUTPUT ? "output" : "input",
            id, device_id);

        if (props != NULL)
            parse_props(intf, props);

        break;
    }
    default:
        break;
    }
}

static const struct pw_device_events device_events = {
    PW_VERSION_DEVICE_EVENTS,
    .info = device_event_info,
    .param = device_event_param,
};

static const struct intf_info device_info = {
    .type = PW_TYPE_INTERFACE_Device,
    .version = PW_VERSION_DEVICE,
    .events = &device_events,
};

/** metadata */

static int metadata_event_property(void *data, uint32_t subject,
    const char *key, const char *type, const char *value)
{
    struct intf *intf = data;
    struct ctl *ctl = intf->ctl;
    struct spa_json it[2];
    char k[1024], v[1024];

    if (subject == PW_ID_CORE) {
        if (spa_streq(key, "default.audio.sink")) {
            spa_json_init(&it[0], value, strlen(value));
            while (spa_json_enter_object(&it[0], &it[1]) > 0) {
                if (spa_json_get_string(&it[1], k, sizeof(k)) > 0 &&
                    spa_json_get_string(&it[1], v, sizeof(v)) > 0 &&
                    spa_streq(k, "name"))
                {
                    strncpy(ctl->default_sink, v, sizeof(v));
                    log_debug("found default sink %s", ctl->default_sink);
                }
            }
        }
        if (spa_streq(key, "default.audio.source")) {
            spa_json_init(&it[0], value, strlen(value));
            while (spa_json_enter_object(&it[0], &it[1]) > 0) {
                if (spa_json_get_string(&it[1], k, sizeof(k)) > 0 &&
                    spa_json_get_string(&it[1], v, sizeof(v)) > 0 &&
                    spa_streq(k, "name"))
                {
                    strncpy(ctl->default_source, v, sizeof(v));
                    log_debug("found default source %s", ctl->default_source);
                }
            }
        }
    }
    return 0;
}

static const struct pw_metadata_events metadata_events = {
    PW_VERSION_METADATA_EVENTS,
    .property = metadata_event_property,
};

static const struct intf_info metadata_info = {
    .type = PW_TYPE_INTERFACE_Metadata,
    .version = PW_VERSION_METADATA,
    .events = &metadata_events,
};

/** link */

static void link_event_info(void *data, const struct pw_link_info *info)
{
    struct intf *intf = data, *node;
    struct ctl *ctl = intf->ctl;

    if (info->change_mask & PW_LINK_CHANGE_MASK_PROPS) {
        intf->link.output_port = info->output_port_id;
        intf->link.input_port = info->input_port_id;

        log_debug("link#%d: out:%d in:%d", intf->id,
            intf->link.output_port, intf->link.input_port);
    }

    redraw(ctl);
}

static const struct pw_link_events link_events = {
    PW_VERSION_LINK_EVENTS,
    .info = link_event_info,
};

static const struct intf_info link_info = {
    .type = PW_TYPE_INTERFACE_Link,
    .version = PW_VERSION_LINK,
    .events = &link_events,
};

/** port */

static void port_event_info(void *data, const struct pw_port_info *info)
{
    struct intf *intf = data;
    struct ctl *ctl = intf->ctl;
    const char *str;

    if (info->change_mask & PW_PORT_CHANGE_MASK_PROPS) {
        if ((str = pw_properties_get(intf->props, PW_KEY_NODE_ID)) != NULL)
            intf->port.node = atoi(str);
        else
            intf->port.node = SPA_ID_INVALID;

        intf->port.direction = info->direction;

        log_debug("port#%d node:%d direction:%s", intf->id,
            intf->port.node,
            intf->port.direction == SPA_DIRECTION_OUTPUT ? "output" : "input");
    }

    redraw(ctl);
}

const struct pw_port_events port_events = {
    PW_VERSION_PORT_EVENTS,
    .info = port_event_info,
};

const struct intf_info port_info = {
    .type = PW_TYPE_INTERFACE_Port,
    .version = PW_VERSION_PORT,
    .events = &port_events,
};

/** proxy */

static void proxy_event_removed(void *data)
{
    struct intf *intf = data;
    pw_proxy_destroy(intf->proxy);
}

static void proxy_event_destroy(void *data)
{
    struct intf *intf = data;
    spa_list_remove(&intf->ref);
    intf->proxy = NULL;
    pw_properties_free(intf->props);
}

static const struct pw_proxy_events proxy_events = {
    PW_VERSION_PROXY_EVENTS,
    .removed = proxy_event_removed,
    .destroy = proxy_event_destroy,
};

/** registry */

static void registry_event_global(void *data, uint32_t id,
    uint32_t permissions, const char *type,
    uint32_t version, const struct spa_dict *props)
{
    struct ctl *ctl = data;
    struct intf *intf;
    struct pw_proxy *proxy;
    const struct intf_info *info = NULL;
    const char *str;

    if (props == NULL)
        return;
    if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
        if ((str = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS)) == NULL)
            return;
        log_debug("found node#%d type:%s", id, str);
        info = &node_info;
    } else if (spa_streq(type, PW_TYPE_INTERFACE_Device)) {
        if ((str = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS)) == NULL)
            return;
        log_debug("found device#%d type:%s", id, str);
        info = &device_info;
    } else if (spa_streq(type, PW_TYPE_INTERFACE_Metadata)) {
        if ((str = spa_dict_lookup(props, PW_KEY_METADATA_NAME)) == NULL ||
            !spa_streq(str, "default"))
        {
            return;
        }
        log_debug("found metadata#%d name:%s", id, str);
        info = &metadata_info;
    } else if (spa_streq(type, PW_TYPE_INTERFACE_Link)) {
        log_debug("found link#%d", id);
        info = &link_info;
    } else if (spa_streq(type, PW_TYPE_INTERFACE_Port)) {
        log_debug("found port#%d", id);
        info = &port_info;
    } else
        return;

    proxy = pw_registry_bind(ctl->registry, id,
        info->type, info->version, sizeof(struct intf));
    intf = pw_proxy_get_user_data(proxy);
    intf->ctl = ctl;
    intf->id = id;
    intf->perms = permissions;
    intf->props = props ? pw_properties_new_dict(props) : NULL;
    intf->proxy = proxy;
    intf->info = info;
    spa_list_append(&ctl->refs, &intf->ref);
    ctl->n_refs++;

    pw_proxy_add_listener(proxy,
        &intf->proxy_listener,
        &proxy_events, intf);

    if (info->events != NULL) {
        pw_proxy_add_object_listener(proxy,
            &intf->object_listener,
            info->events, intf);
    }
    if (spa_streq(type, PW_TYPE_INTERFACE_Metadata))
        ctl->metadata = (struct pw_metadata*)proxy;
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY,
    .global = registry_event_global,
};

int main(int argc, char *argv[])
{
    struct ctl ctl;
    struct pw_loop *loop;

    // init
    log_file = fopen("pwmixer.log", "w");
    pw_init(NULL, NULL);
    ctl.cursor = 0;
    ctl.n_refs = 0;
    ctl.metadata = NULL;
    ctl.node_flags = NODE_FLAG_SINK;
    ctl.volume_method = VOLUME_METHOD_CUBIC;
    spa_list_init(&ctl.refs);

    ctl.mainloop = pw_thread_loop_new("pwmixer", NULL);
    loop = pw_thread_loop_get_loop(ctl.mainloop);
    ctl.system = loop->system;
    ctl.fd = spa_system_eventfd_create(ctl.system, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
    if (ctl.fd < 0) {
        log_debug("cannot create eventfd");
        return -ctl.fd;
    }

    ctl.context = pw_context_new(loop, NULL, 0);

    pw_thread_loop_lock(ctl.mainloop);
    pw_thread_loop_start(ctl.mainloop);

    ctl.core = pw_context_connect(ctl.context, NULL, 0);
    if (ctl.core == NULL) {
        log_debug("pw_core create failed");
        return -errno;
    }

    ctl.registry = pw_core_get_registry(ctl.core, PW_VERSION_REGISTRY, 0);
    if (ctl.registry == NULL) {
        log_debug("pw_registry create failed");
        return -errno;
    }
    pw_registry_add_listener(ctl.registry, &ctl.registry_listener,
        &registry_events, &ctl);

    pw_thread_loop_unlock(ctl.mainloop);

    // init curses
    init_curses(&ctl);

    // run curses
    run_curses(&ctl);

    // clean up
    endwin();
    fclose(log_file);

    return 0;
}
