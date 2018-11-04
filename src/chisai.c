/* Includes */
#include <err.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <xcb/xcb.h>

#include "config.h"
#include "types.h"

/* Macros */
#define MAX(a, b) ((a > b) ? (a) : (b))
#define CLEANMASK(mask) ((mask & ~0x80))

enum { INACTIVE, ACTIVE };

/* Modifiers - You can change to set different MOD */
#define SUPER XCB_MOD_MASK_4
#define ALT	  XCB_MOD_MASK_1
#define CTRL  XCB_MOD_MASK_CONTROL
#define SHIFT XCB_MOD_MASK_SHIFT
#define MOD	  SUPER

/* Socket Variables */
static int sock_fd;
static int client_fd;
static const char *sock_path;

/* Group Variables */
static uint16_t focused_workspace = 1;
static struct list *window_list = NULL;

/* XCB Variables */
static xcb_connection_t *connection;
static xcb_screen_t *screen;
static struct client *focused_window;
static struct conf config;

/* List Functions */
static struct node* add_node(struct list *list);
static void delete_node(struct list *list, struct node *node);

/* X Event Functions */
static void new_window(xcb_generic_event_t *event);
static void destroy_window(xcb_generic_event_t *event);
static void unmap_window(xcb_generic_event_t *event);
static void enter_window(xcb_generic_event_t *event);
static void configure_window(xcb_generic_event_t *event);

/* X Helper Functions */
static struct client* setup_window(xcb_window_t window);
static bool get_geometry(const xcb_drawable_t *window, int16_t *x, int16_t *y, uint16_t *height, uint16_t *width, uint8_t *depth);
static void set_borders(struct client *client, int mode);
static struct client* find_client(const xcb_drawable_t *window);
static void forget_window(xcb_window_t window);

/* Helper Functions */
static uint32_t get_color(const char *hex);

/* Main Functions */
static void cleanup(void);
static int socket_deploy(void);
static int x_deploy(void);
static void load_defaults(void);
static void subscribe(struct client *client);
static void focus(struct client *client, int mode);
static void events_loop(void);


static struct node*
add_node(struct list *list)
{
    struct node *node;

    node = (struct node *) malloc(sizeof(struct node));

    if (!node) {
        return NULL;
    }

    if (!list) {
        node->next = NULL;
    } else {
        node->next = list->head;
    }

    list->head = node;
    list->length++;

    return node;
}



static void
delete_node(struct list *list, struct node *node)
{
    struct node *previous = NULL;
    struct node *current  = list->head;
    struct node *next     = NULL;

    if (!list || !node) {
        return;
    } else if (node == list->head) {
        struct node *new_head = list->head->next;
        free(list->ehead);
        list->head = new_head;
    } else {
        while (current != node) {
            previous = current;
            current = current->next;
        }
        if (!current) {
            return;
        } else {
            next = current->next;
            free(current);
            previous->next = next;
        }
    }

    list->length--;
}


 void
new_window(xcb_generic_event_t *event)
{
    xcb_create_notify_event_t *e;
    e = (xcb_create_notify_event_t *)event;
    struct client *client;

    client = setup_window(e->window);

    if (!client) {
        return;
    }

    xcb_map_window(connection, client->window);

    if (!client->maxed) {
        set_borders(client, ACTIVE);
    }
}


static void
destroy_window(xcb_generic_event_t *event)
{
    xcb_destroy_notify_event_t *e;
    e = (xcb_destroy_notify_event_t *)event;
    struct client *client;

    if (focused_window && focused_window->window == e->window) {
        focused_window = NULL;
    }

    client = find_client(&e->window);

    if (client) {
        xcb_kill_client(connection, client->window);

        forget_window(client->window);
    }
}


static void
unmap_window(xcb_generic_event_t *event)
{
    xcb_unmap_notify_event_t *e;
    e = (xcb_unmap_notify_event_t *)event;
    struct client *client;

    client = find_client(&e->window);

    if (!client) {
        return;
    }

    if (focused_window && client->window == focused_window->window) {
        focused_window = NULL;
    }

    xcb_unmap_window(connection, client->window);
    forget_window(client->window);
}


static void
enter_window(xcb_generic_event_t *event)
{
    xcb_enter_notify_event_t *e;
    e = (xcb_enter_notify_event_t *)event;
    struct client *client;

    if (config.sloppy_focus) {
        if (focused_window && focused_window->window == e->event) {
            return;
        }

        client = find_client(&e->event);

        if (!client) {
            return;
        }

        focus(client, ACTIVE);
        set_borders(client, ACTIVE);
}


static void
configure_window(xcb_generic_event_t *event)
{
    xcb_configure_notify_event_t *e;
    e = (xcb_configure_notify_event_t *)event;
    struct client *client;

    if ((client = find_client(&e->window)) {
        if (client->window != focused_window) {
            focus(client, INACTIVE);
        }

        focus(focused_window, ACTIVE);
    }
}


static struct client*
setup_window(xcb_window_t window)
{
    struct client *client;
    struct node *node;

    node = add_node(window_list);
    client = malloc(sizeof(struct client));

    if (!node  || !client) {
        return NULL;
    }

    node->data = client;

    client->window = window;

    /* Reset all values */
    client->x = 0;
    client->y = 0;
    client->width= 0;
    client->height = 0;
    client->depth= 0;

    client->maxed = false;

    get_geometry(&client->window, &client->x, &client->y,
            &client->width, &client->height, &client->depth);
    client->workspace = focused_workspace;

    return client;
}


static bool
get_geometry(const xcb_drawable_t *window, int16_t *x, int16_t *y,
             uint16_t *height, uint16_t *width, uint8_t *depth)
{
    xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(connection,
            xcb_get_geometry(connection, *window), NULL);

    if (!geometry) {
        return false;
    }

    *x = geometry->x;
    *y = geometry->y;
    *width = geometry->width;
    *height = geometry->height;
    *depth = geometry->depth;

    free(geometry);
    return true;
}


static void
set_borders(struct client *client, int mode)
{
    uint32_t values[1];

    if(client->maxed) {
        return;
    }

    values[0] = config.border_width;

    /* TODO: Finish setting different borders depending on option */
    xcb_rectangle_t border_rect[] = {

    };

    xcb_pixmap_t pmap = xcb_generate_id(connection);
    xcb_gcontext_t gc = xcb_generate_id(connection);

    xcb_create_gc(connection, gc, pmap, 0, NULL);

    if (mode == ACTIVE) {
        values[0] = config.focus_color;
    } else if (mode == INACTIVE) {
        values[0] = config.unfocus_color;
    }

    xcb_change_gc(connection, gc, XCB_GC_FOREGROUND, &values[0]);
    xcb_poly_fill_rectangle(connection, pmap, gc, 5, border_rect);
    values[0] = pmap;
    xcb_change_window_attributes(connection, client->window, XCB_CW_BORDER_PIXMAP,
            &values[0]);

    xcb_free_pixmap(connection, pmap);
    xcb_free_gc(connection, gc);
}


static struct client*
find_client(const xcb_drawable_t *window)
{
    struct client *client;
    struct node *node;

    for (node = window_list->head; node; node = node->next) {
        client = node->data;

        if (*window == client->window) {
            return client;
        }
    }

    return NULL;
}


static void
forget_window(xcb_window_t window)
{
    struct client *client;
    struct node *node;

    for (node = window_list->head; node; node = node->next) {
        client = node->data;

        if (window == client->window) {
            delete_node(window_list, node);
            return;
        }
    }
}


static uint32_t
get_color(const char *hex)
{
    uint32_t rgb48;
    char strgroups[7] = {
        hex[1], hex[2], hex[3], hex[4], hex[5], hex[6], '\0'
    };

    rgb48 = strtol(strgroups, NULL, 16);
    return rgb48 | 0xff000000;
}


static void
cleanup(void)
{
    if (connection) {
        xcb_disconnect(connection);
    }
}


static int
socket_deploy(void)
{
    struct sockaddr_un sock_addr;

    /* Get socket path */
    sock_path = getenv("CHISAI_SOCKET");
    memset(&sock_addr, 0, sizeof(sock_addr));

    if (sock_path) {
        strncpy(sock_addr.sun_path, sock_path, sizeof(sock_addr.sun_path));
    } else {
        strncpy(sock_addr.sun_path, "/tmp/chisai.sock", sizeof(sock_addr.sun_path));
    }

    /* Create socket */
    if ((sock_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        return -1;
    }

    sock_addr.sun_family = AF_UNIX;

    unlink(sock_addr.sun_path);

    /* Bind socket */
    if (bind(sock_fd, (struct sockaddr*)&sock_addr, sizeof(sock_addr)) < 0) {
        return -1;
    }

    /* Listen to the socket */
    if (listen(sock_fd, 1) < 0) {
        return -1;
    }

    return 0;
}


static int
x_deploy(void)
{
    /* Init XCB and grab events */
    uint32_t values[2];
    int mask;

    /* Make sure XCB and the screen are working properly */
    if (xcb_connection_has_error(connection = xcb_connect(NULL, NULL))) {
        return -1;
    }

    if (!(screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data)) {
        return -1;
    }

    focused_window->window = screen->root;

    /* Grab mouse buttons */
    xcb_grab_button(connection, 0, screen->root, XCB_EVENT_MASK_BUTTON_PRESS |
			XCB_EVENT_MASK_BUTTON_RELEASE, XCB_GRAB_MODE_ASYNC,
			XCB_GRAB_MODE_ASYNC, screen->root, XCB_NONE, 1, MOD);

	xcb_grab_button(connection, 0, screen->root, XCB_EVENT_MASK_BUTTON_PRESS |
			XCB_EVENT_MASK_BUTTON_RELEASE, XCB_GRAB_MODE_ASYNC,
            XCB_GRAB_MODE_ASYNC, screen->root, XCB_NONE, 3, MOD);

    /* Update mask and root */
    mask = XCB_CW_EVENT_MASK;
    values[0] = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
	xcb_change_window_attributes_checked(connection, screen->root, mask, values);

    xcb_flush(connection);

    return 0;
}


static void
load_defaults(void)
{
    config.border_side   = BORDER_SIDE;
    config.border_width  = BORDER_WIDTH;
    config.focus_color   = get_color(COLOR_FOCUS);
    config.unfocus_color = get_color(COLOR_UNFOCUS);
    config.workspaces    = WORKSPACES;
    config.sloppy_focus  = SLOPPY_FOCUS;
}


/*
 * Function: subscribe
 * -------------------
 * Start listening for events that happen
 * to the window passed in
 *
 * returns: nothing
 */
static void
subscribe(struct client *client)
{
    uint32_t values[2] = {
        XCB_EVENT_MASK_ENTER_WINDOW,
        XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
    };

    xcb_change_window_attributes(connection, client->window, XCB_CW_EVENT_MASK, values);
}


static void
focus(struct client *client, int mode)
{
    if (mode == ACTIVE){
        if (!client) {
            focused_window = NULL;
            xcb_set_input_focus(connection, XCB_NONE,
                XCB_INPUT_FOCUS_POINTER_ROOT, XCB_CURRENT_TIME);
            return;
        }

        /* Don't bother focusing root or the window already in focus */
        if (client->window == focused_window->window || client->window == screen->root) {
            return;
        }

        if (focused_window) {
            focus(focused_window, INACTIVE);
        }

        xcb_set_input_focus(connection, XCB_INPUT_FOCUS_POINTER_ROOT,
            client->window, XCB_CURRENT_TIME);

        focused_window = client;

        set_borders(client, ACTIVE);
    } else if (mode == INACTIVE) {
        if (!focused_window || focused_window->window == screen->root) {
            return;
        }

        set_borders(focused_window, INACTIVE);
    }
}


static void
events_loop(void)
{

    while (true)
    {
        /* Select file descriptors and select which one you receive */
        int x_fd = xcb_get_file_descriptor(connection);

        fd_set file_descriptors;
        FD_ZERO(&file_descriptors);
        FD_SET(x_fd, &file_descriptors);
        FD_SET(sock_fd, &file_descriptors);

        int max_fd = MAX(sock_fd, x_fd) + 1;

        select(max_fd, &file_descriptors, NULL, NULL, NULL);

        /* Pathway for if a message from the client is received */
        if (FD_ISSET(sock_fd, &file_descriptors)) {
            char message[BUFSIZ];
            int message_length;

            if ((client_fd = accept(sock_fd, NULL, 0)) < 0) {
                errx(EXIT_FAILURE, "chisai: failed to accept client socket");
            }

            if ((message_length = read(client_fd, message, sizeof(message))) > 0) {
                message[message_length] = '\0';
            }
        }

        /* Pathway for if X event is received */
        if (FD_ISSET(x_fd, &file_descriptors)) {
            xcb_generic_event_t *event;
            uint32_t values[3];
            xcb_get_geometry_reply_t *geometry;
            xcb_window_t window = XCB_WINDOW_NONE;

            event = xcb_poll_for_event(connection);

            /* Make sure there is an event */
            if (!event) {
                continue;
            }

            /* Handle all the X events we are accepting */
            switch(CLEANMASK(event->response_type))
            {
                case XCB_MAP_NOTIFY: {
                    new_window(event);
                } break;

                case XCB_DESTROY_NOTIFY: {
                    destroy_window(event);
                } break;

                case XCB_UNMAP_NOTIFY: {
                    unmap_window(event);
                } break;

                case XCB_ENTER_NOTIFY: {
                    enter_window(event);
                } break;

                case XCB_BUTTON_PRESS: {
                    xcb_button_press_event_t *e;
                    e = (xcb_button_press_event_t *)event;
                    window = e->child;

                    if (!window || window == screen->rootop_wint) {
                        break;
                    }

                    values[0] = XCB_STACK_MODE_ABOVE;
                    xcb_configure_window(connection, window,
                            XCB_CONFIG_WINDOW_STACK_MODE, values);
                    geometry = xcb_get_geometry_reply(connection,
                            xcb_get_geometry(connection, window), NULL);

                    if(e->detail == 1) {
                        values[2] = 1;
                        xcb_warp_pointer(connection, XCB_NONE, window,
                                0, 0, 0, 0, geometry->width/2, geometry->height/2);

                    } else {
                        values[2] = 3;
                        xcb_warp_pointer(connection, XCB_NONE, window,
                                0, 0, 0, 0, geometry->width, geometry->height);
                    }

                    xcb_grab_pointer(connection, 0, screen->root,
                            XCB_EVENT_MASK_BUTTON_RELEASE
                            | XCB_EVENT_MASK_BUTTON_MOTION
                            | XCB_EVENT_MASK_POINTER_MOTION_HINT,
                            XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                            screen->root, XCB_NONE, XCB_CURRENT_TIME);
                    xcb_flush(connection);
                } break;

                case XCB_MOTION_NOTIFY: {
                    xcb_query_pointer_reply_t *pointer;
                    pointer = xcb_query_pointer_reply(connection,
                            xcb_query_pointer(connection, screen->root), 0);
                    if (values[2] == 1) {
                        geometry = xcb_get_geometry_reply(connection,
                                xcb_get_geometry(connection, screen->root), 0);

                        if (!geometry) {
                            break;
                        }

                        values[0] = (pointer->root_x + geometry->width / 2
                                > screen->width_in_pixels
                                - (config.border_width * 2))
                                ? (screen->width_in_pixels - geometry->width
                                - (config.border_width * 2))
                                : pointer->root_x - geometry->width /2;

                        values[1] = (pointer->root_y + geometry->height / 2
                                > screen->height_in_pixels
                                - (config.border_width * 2))
                                ? (screen->height_in_pixels - geometry->height
                                - (config.border_width * 2))
                                : pointer->root_y - geometry->height / 2;

                        if (pointer->root_x < geometry->width / 2) {
                            values[0] = 0;
                        }
                        if (pointer->root_y < geometry->height / 2) {
                            values[1] = 0;
                        }

                        xcb_configure_window(connection, window,
                                XCB_CONFIG_WINDOW_X
                                | XCB_CONFIG_WINDOW_Y, values);

                        xcb_flush(connection);
                    } else if (values[2] == 3 ) {
                        geometry = xcb_get_geometry_reply(connection,
                                xcb_get_geometry(connection, window), NULL);
                        values[0] = pointer->root_x - geometry->x;
                        values[1] = pointer->root_y - geometry->y;
                        xcb_configure_window(connection, window,
                                XCB_CONFIG_WINDOW_WIDTH
                                | XCB_CONFIG_WINDOW_HEIGHT, values);
                        xcb_flush(connection);
                    }
                } break;

                case XCB_BUTTON_RELEASE: {
                    focus(window, ACTIVE);
                    xcb_ungrab_pointer(connection, XCB_CURRENT_TIME);
                } break;

                case XCB_CONFIGURE_NOTIFY: {
                    configure_window(event);
                } break;
            }

            xcb_flush(connection);
            free(event);
        }
    }
}


int
main(void)
{
    /* Cleanup X Connections */
    atexit(cleanup);

    /* Setup socket and X */
    if (socket_deploy() < 0) {
        errx(EXIT_FAILURE, "chisai: error connecting to socket");
    }
    if (x_deploy() < 0) {
        errx(EXIT_FAILURE, "chisai: error connecting to x");
    }

    load_defaults();

    events_loop();

    return EXIT_FAILURE;
    /* TODO: COnfig shoud be maikuro config border_width 5 or something
     * of the likes where config shows it's editing the config
     */

}