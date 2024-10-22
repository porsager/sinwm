#include <xcb/xcb.h>
#include <xcb/randr.h>
#include <xcb/xcb_icccm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <png.h>

#define MAX_WINDOWS 128
#define MAX_MONITORS 32

xcb_atom_t atom_net_wm_state
         , atom_net_wm_state_above
         , atom_net_wm_state_fullscreen
         , atom_net_supported
         , atom_net_supporting_wm_check
         , atom_net_active_window
         , atom_net_wm_fullscreen_monitors
         , atom_net_wm_name
         , atom_utf8_string
         , atom_wm_name
         , atom_wm_class
         , atom_wm_protocols
         , atom_wm_delete_window;

xcb_pixmap_t pixmap = XCB_PIXMAP_NONE;
xcb_window_t always_on_top_windows[MAX_WINDOWS];
int always_on_top_count = 0;

typedef struct {
  xcb_window_t window;
  xcb_rectangle_t original_geometry;
  int monitors[4];
  int has_monitors;
  int is_general_fullscreen;
  int is_monitor_fullscreen;
} fullscreen_window_t;

fullscreen_window_t fs_windows[MAX_WINDOWS];
int fullscreen_count = 0;
xcb_window_t active_window = XCB_WINDOW_NONE;

typedef struct {
  int id;
  int x;
  int y;
  int width;
  int height;
} monitor_t;

monitor_t monitors[MAX_MONITORS];
int monitor_count = 0;

int total_width = 0, total_height = 0;

xcb_window_t focus_stack[MAX_WINDOWS];
int focus_stack_top = -1;

int wallpaper_width = 0;
int wallpaper_height = 0;

xcb_pixmap_t load_wallpaper(xcb_connection_t *conn, xcb_screen_t *screen, const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    return XCB_PIXMAP_NONE;
  }

  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!png) {
    fclose(fp);
    return XCB_PIXMAP_NONE;
  }

  png_infop info = png_create_info_struct(png);
  if (!info) {
    png_destroy_read_struct(&png, NULL, NULL);
    fclose(fp);
    return XCB_PIXMAP_NONE;
  }

  if (setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);
    return XCB_PIXMAP_NONE;
  }

  png_init_io(png, fp);
  png_read_info(png, info);

  wallpaper_width      = png_get_image_width(png, info);
  wallpaper_height     = png_get_image_height(png, info);
  png_byte color_type  = png_get_color_type(png, info);
  png_byte bit_depth   = png_get_bit_depth(png, info);

  if (bit_depth == 16) png_set_strip_16(png);
  if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
  if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
  if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
  if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE) png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
  if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);

  png_read_update_info(png, info);

  png_bytep *row_pointers = malloc(sizeof(png_bytep) * wallpaper_height);
  for(int y = 0; y < wallpaper_height; y++)
    row_pointers[y] = malloc(png_get_rowbytes(png, info));

  png_read_image(png, row_pointers);
  fclose(fp);
  png_destroy_read_struct(&png, &info, NULL);

  if (pixmap != XCB_PIXMAP_NONE)
    xcb_free_pixmap(conn, pixmap);

  pixmap = xcb_generate_id(conn);
  xcb_create_pixmap(conn, screen->root_depth, pixmap, screen->root, wallpaper_width, wallpaper_height);

  xcb_gcontext_t gc = xcb_generate_id(conn);
  uint32_t mask_gc = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND;
  uint32_t values_gc[] = { screen->black_pixel, screen->white_pixel };
  xcb_create_gc(conn, gc, screen->root, mask_gc, values_gc);

  uint32_t *image_data = malloc(sizeof(uint32_t) * wallpaper_width * wallpaper_height);
  for(int y = 0; y < wallpaper_height; y++) {
    png_bytep row = row_pointers[y];
    for(int x = 0; x < wallpaper_width; x++) {
      png_bytep px = &(row[x * 4]);
      uint32_t pixel = (px[0] << 16) | (px[1] << 8) | px[2]; // RGB
      image_data[y * wallpaper_width + x] = pixel;
    }
  }

  xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap, gc, wallpaper_width, wallpaper_height, 0, 0, 0, screen->root_depth, wallpaper_width * wallpaper_height * 4, (const char*)image_data);

  xcb_free_gc(conn, gc);
  xcb_flush(conn);
  free(image_data);
  for(int y = 0; y < wallpaper_height; y++) {
    free(row_pointers[y]);
  }
  free(row_pointers);

  return pixmap;
}

void set_wallpaper(xcb_connection_t *conn, xcb_screen_t *screen) {
  const char *home = getenv("HOME");
  char path[1024];
  snprintf(path, sizeof(path), "%s/.config/sinwm/wallpaper.png", home);

  xcb_pixmap_t wallpaper_pixmap = load_wallpaper(conn, screen, path);
  if (wallpaper_pixmap == XCB_PIXMAP_NONE)
    return;

  xcb_gcontext_t gc = xcb_generate_id(conn);
  uint32_t mask_gc = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND;
  uint32_t values_gc[] = { screen->black_pixel, screen->white_pixel };
  xcb_create_gc(conn, gc, screen->root, mask_gc, values_gc);

  for (int i = 0; i < monitor_count; i++) {
    int x = monitors[i].x + (monitors[i].width - wallpaper_width) / 2;
    int y = monitors[i].y + (monitors[i].height - wallpaper_height) / 2;
    if (x < monitors[i].x) x = monitors[i].x;
    if (y < monitors[i].y) y = monitors[i].y;
    xcb_copy_area(conn, wallpaper_pixmap, screen->root, gc, 0, 0, x, y, wallpaper_width, wallpaper_height);
  }

  xcb_free_gc(conn, gc);
  xcb_free_pixmap(conn, wallpaper_pixmap);
  xcb_flush(conn);
}

void push_focus(xcb_window_t window) {
  for (int i = 0; i <= focus_stack_top; i++) {
    if (focus_stack[i] == window) {
      for (int j = i; j < focus_stack_top; j++) {
        focus_stack[j] = focus_stack[j + 1];
      }
      focus_stack_top--;
      break;
    }
  }

  if (focus_stack_top < MAX_WINDOWS - 1)
    focus_stack[++focus_stack_top] = window;
}

void remove_focus(xcb_window_t window) {
  for (int i = 0; i <= focus_stack_top; i++) {
    if (focus_stack[i] == window) {
      for (int j = i; j < focus_stack_top; j++) {
        focus_stack[j] = focus_stack[j + 1];
      }
      focus_stack_top--;
      break;
    }
  }
}

xcb_window_t get_top_focus() {
  if (focus_stack_top >= 0)
    return focus_stack[focus_stack_top];
  return XCB_WINDOW_NONE;
}

void set_input_focus(xcb_connection_t *conn, xcb_window_t window) {
  xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, window, XCB_CURRENT_TIME);
  active_window = window;
  push_focus(window);
  xcb_screen_iterator_t iter = xcb_setup_roots_iterator(xcb_get_setup(conn));
  xcb_screen_t *screen = iter.data;
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root, atom_net_active_window, XCB_ATOM_WINDOW, 32, 1, &window);
  xcb_flush(conn);
}

void remove_net_active_window(xcb_connection_t *conn) {
  xcb_screen_iterator_t iter = xcb_setup_roots_iterator(xcb_get_setup(conn));
  xcb_screen_t *screen = iter.data;
  xcb_delete_property(conn, screen->root, atom_net_active_window);
  xcb_flush(conn);
}

void setup_atoms(xcb_connection_t *conn) {
  xcb_intern_atom_cookie_t cookie_wm_state = xcb_intern_atom(conn, 0, strlen("_NET_WM_STATE"), "_NET_WM_STATE")
                          , cookie_wm_state_above = xcb_intern_atom(conn, 0, strlen("_NET_WM_STATE_ABOVE"), "_NET_WM_STATE_ABOVE")
                          , cookie_wm_state_fullscreen = xcb_intern_atom(conn, 0, strlen("_NET_WM_STATE_FULLSCREEN"), "_NET_WM_STATE_FULLSCREEN")
                          , cookie_net_supported = xcb_intern_atom(conn, 0, strlen("_NET_SUPPORTED"), "_NET_SUPPORTED")
                          , cookie_net_supporting_wm_check = xcb_intern_atom(conn, 0, strlen("_NET_SUPPORTING_WM_CHECK"), "_NET_SUPPORTING_WM_CHECK")
                          , cookie_net_active_window = xcb_intern_atom(conn, 0, strlen("_NET_ACTIVE_WINDOW"), "_NET_ACTIVE_WINDOW")
                          , cookie_net_wm_fullscreen_monitors = xcb_intern_atom(conn, 0, strlen("_NET_WM_FULLSCREEN_MONITORS"), "_NET_WM_FULLSCREEN_MONITORS")
                          , cookie_net_wm_name = xcb_intern_atom(conn, 0, strlen("_NET_WM_NAME"), "_NET_WM_NAME")
                          , cookie_utf8_string = xcb_intern_atom(conn, 1, strlen("UTF8_STRING"), "UTF8_STRING")
                          , cookie_wm_name = xcb_intern_atom(conn, 0, strlen("WM_NAME"), "WM_NAME")
                          , cookie_wm_class = xcb_intern_atom(conn, 0, strlen("WM_CLASS"), "WM_CLASS")
                          , cookie_wm_protocols = xcb_intern_atom(conn, 0, strlen("WM_PROTOCOLS"), "WM_PROTOCOLS")
                          , cookie_wm_delete_window = xcb_intern_atom(conn, 0, strlen("WM_DELETE_WINDOW"), "WM_DELETE_WINDOW");

  xcb_intern_atom_reply_t *reply_wm_state = xcb_intern_atom_reply(conn, cookie_wm_state, NULL)
                        , *reply_wm_state_above = xcb_intern_atom_reply(conn, cookie_wm_state_above, NULL)
                        , *reply_wm_state_fullscreen = xcb_intern_atom_reply(conn, cookie_wm_state_fullscreen, NULL)
                        , *reply_net_supported = xcb_intern_atom_reply(conn, cookie_net_supported, NULL)
                        , *reply_net_supporting_wm_check = xcb_intern_atom_reply(conn, cookie_net_supporting_wm_check, NULL)
                        , *reply_net_active_window = xcb_intern_atom_reply(conn, cookie_net_active_window, NULL)
                        , *reply_net_wm_fullscreen_monitors = xcb_intern_atom_reply(conn, cookie_net_wm_fullscreen_monitors, NULL)
                        , *reply_net_wm_name = xcb_intern_atom_reply(conn, cookie_net_wm_name, NULL)
                        , *reply_utf8_string = xcb_intern_atom_reply(conn, cookie_utf8_string, NULL)
                        , *reply_wm_name = xcb_intern_atom_reply(conn, cookie_wm_name, NULL)
                        , *reply_wm_class = xcb_intern_atom_reply(conn, cookie_wm_class, NULL)
                        , *reply_wm_protocols = xcb_intern_atom_reply(conn, cookie_wm_protocols, NULL)
                        , *reply_wm_delete_window = xcb_intern_atom_reply(conn, cookie_wm_delete_window, NULL);

  if (reply_wm_state) { atom_net_wm_state = reply_wm_state->atom; free(reply_wm_state); }
  if (reply_wm_state_above) { atom_net_wm_state_above = reply_wm_state_above->atom; free(reply_wm_state_above); }
  if (reply_wm_state_fullscreen) { atom_net_wm_state_fullscreen = reply_wm_state_fullscreen->atom; free(reply_wm_state_fullscreen); }
  if (reply_net_supported) { atom_net_supported = reply_net_supported->atom; free(reply_net_supported); }
  if (reply_net_supporting_wm_check) { atom_net_supporting_wm_check = reply_net_supporting_wm_check->atom; free(reply_net_supporting_wm_check); }
  if (reply_net_active_window) { atom_net_active_window = reply_net_active_window->atom; free(reply_net_active_window); }
  if (reply_net_wm_fullscreen_monitors) { atom_net_wm_fullscreen_monitors = reply_net_wm_fullscreen_monitors->atom; free(reply_net_wm_fullscreen_monitors); }
  if (reply_net_wm_name) { atom_net_wm_name = reply_net_wm_name->atom; free(reply_net_wm_name); }
  if (reply_utf8_string) { atom_utf8_string = reply_utf8_string->atom; free(reply_utf8_string); }
  if (reply_wm_name) { atom_wm_name = reply_wm_name->atom; free(reply_wm_name); }
  if (reply_wm_class) { atom_wm_class = reply_wm_class->atom; free(reply_wm_class); }
  if (reply_wm_protocols) { atom_wm_protocols = reply_wm_protocols->atom; free(reply_wm_protocols); }
  if (reply_wm_delete_window) { atom_wm_delete_window = reply_wm_delete_window->atom; free(reply_wm_delete_window); }
}

int is_always_on_top(xcb_window_t window) {
  for (int i = 0; i < always_on_top_count; i++) {
    if (always_on_top_windows[i] == window)
      return 1;
  }
  return 0;
}

void add_to_always_on_top(xcb_window_t window) {
  if (is_always_on_top(window))
    return;

  if (always_on_top_count < MAX_WINDOWS)
    always_on_top_windows[always_on_top_count++] = window;
}

void remove_from_always_on_top(xcb_window_t window) {
  for (int i = 0; i < always_on_top_count; i++) {
    if (always_on_top_windows[i] == window) {
      for (int j = i; j < always_on_top_count - 1; j++) {
        always_on_top_windows[j] = always_on_top_windows[j + 1];
      }
      always_on_top_count--;
      break;
    }
  }
}

int is_fullscreen_window(xcb_window_t window) {
  for (int i = 0; i < fullscreen_count; i++) {
    if (fs_windows[i].window == window)
      return 1;
  }
  return 0;
}

void setup_ewmh(xcb_connection_t *conn, xcb_screen_t *screen) {
  xcb_window_t wm_window = xcb_generate_id(conn);

  xcb_create_window(conn, XCB_COPY_FROM_PARENT, wm_window, screen->root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, NULL);
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root, atom_net_supporting_wm_check, XCB_ATOM_WINDOW, 32, 1, &wm_window);
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, wm_window, atom_net_supporting_wm_check, XCB_ATOM_WINDOW, 32, 1, &wm_window);

  xcb_atom_t supported_atoms[] = {
    atom_net_wm_state,
    atom_net_wm_state_above,
    atom_net_wm_state_fullscreen,
    atom_net_active_window,
    atom_net_wm_fullscreen_monitors,
    atom_net_wm_name
  };
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, screen->root, atom_net_supported, XCB_ATOM_ATOM, 32, sizeof(supported_atoms) / sizeof(xcb_atom_t), supported_atoms);
  const char *wm_name = "SinWM";
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, wm_window, atom_wm_name, XCB_ATOM_STRING, 8, strlen(wm_name), wm_name);
  const char *net_wm_name = "SinWM";
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, wm_window, atom_net_wm_name, atom_utf8_string, 8, strlen(net_wm_name), net_wm_name);
  const char *wm_class = "myminimalwm\0SinWM";
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, wm_window, atom_wm_class, XCB_ATOM_STRING, 8, strlen(wm_class) + 1, wm_class);
  xcb_atom_t protocols[] = { atom_wm_delete_window };
  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, wm_window, atom_wm_protocols, XCB_ATOM_ATOM, 32, sizeof(protocols)/sizeof(xcb_atom_t), protocols);

  xcb_flush(conn);
}

void query_xrandr(xcb_connection_t *conn, xcb_screen_t *screen) {
  xcb_randr_get_screen_resources_cookie_t res_cookie = xcb_randr_get_screen_resources(conn, screen->root);
  xcb_randr_get_screen_resources_reply_t *res_reply = xcb_randr_get_screen_resources_reply(conn, res_cookie, NULL);
  if (!res_reply) {
    fprintf(stderr, "Failed to get RandR screen resources\n");
    fflush(stderr);
    return;
  }

  monitor_count = 0;

  int num_crtcs = xcb_randr_get_screen_resources_crtcs_length(res_reply);
  xcb_randr_crtc_t *crtcs = xcb_randr_get_screen_resources_crtcs(res_reply);

  for (int i = 0; i < num_crtcs; i++) {
    xcb_randr_crtc_t crtc = crtcs[i];
    xcb_randr_get_crtc_info_cookie_t crtc_cookie = xcb_randr_get_crtc_info(conn, crtc, XCB_CURRENT_TIME);
    xcb_randr_get_crtc_info_reply_t *crtc_reply = xcb_randr_get_crtc_info_reply(conn, crtc_cookie, NULL);
    if (crtc_reply && crtc_reply->mode != XCB_NONE && crtc_reply->width > 0 && crtc_reply->height > 0) {
      if (monitor_count >= MAX_MONITORS) {
      } else {

        monitors[monitor_count].id = i;
        monitors[monitor_count].x = crtc_reply->x;
        monitors[monitor_count].y = crtc_reply->y;
        monitors[monitor_count].width = crtc_reply->width;
        monitors[monitor_count].height = crtc_reply->height;
        monitor_count++;
      }
    }
    free(crtc_reply);
  }
  free(res_reply);

  total_width = 0;
  total_height = 0;
  for (int i = 0; i < monitor_count; i++) {
    int monitor_right = monitors[i].x + monitors[i].width;
    int monitor_bottom = monitors[i].y + monitors[i].height;
    if (monitor_right > total_width)
      total_width = monitor_right;
    if (monitor_bottom > total_height)
      total_height = monitor_bottom;
  }
  fprintf(stderr, "total %dx%d\n", total_width, total_height);
  fflush(stderr);

}

int calculate_fullscreen_geometry(int xs[4], int *x1, int *y1, int *x2, int *y2) {
  if (xs[0] == -1 || xs[1] == -1 || xs[2] == -1 || xs[3] == -1) {
    *x1 = 0;
    *y1 = 0;
    *x2 = total_width;
    *y2 = total_height;
    return 0;
  } else {
    if (xs[0] >= 0 && xs[0] < monitor_count &&
        xs[1] >= 0 && xs[1] < monitor_count &&
        xs[2] >= 0 && xs[2] < monitor_count &&
        xs[3] >= 0 && xs[3] < monitor_count) {
      monitor_t *top_mon = &monitors[xs[0]];
      monitor_t *bottom_mon = &monitors[xs[1]];
      monitor_t *left_mon = &monitors[xs[2]];
      monitor_t *right_mon = &monitors[xs[3]];
      *x1 = left_mon->x;
      *y1 = top_mon->y;
      *x2 = right_mon->x + right_mon->width;
      *y2 = bottom_mon->y + bottom_mon->height;
      return 0;
    } else {
      fprintf(stderr, "Invalid monitor indices in _NET_WM_FULLSCREEN_MONITORS message.\n");
      fflush(stderr);
      return -1;
    }
  }
}

int add_fullscreen_window(xcb_connection_t *conn, xcb_window_t window) {
  if (fullscreen_count >= MAX_WINDOWS) {
    fprintf(stderr, "Maximum number of fullscreen windows reached.\n");
    fflush(stderr);
    return -1;
  }

  xcb_get_geometry_cookie_t geom_cookie = xcb_get_geometry(conn, window);
  xcb_get_geometry_reply_t *geom_reply = xcb_get_geometry_reply(conn, geom_cookie, NULL);
  if (!geom_reply) {
    fprintf(stderr, "Failed to get geometry for window 0x%08x.\n", window);
    fflush(stderr);
    return -1;
  }

  fs_windows[fullscreen_count].window = window;
  fs_windows[fullscreen_count].original_geometry.x = geom_reply->x;
  fs_windows[fullscreen_count].original_geometry.y = geom_reply->y;
  fs_windows[fullscreen_count].original_geometry.width = geom_reply->width;
  fs_windows[fullscreen_count].original_geometry.height = geom_reply->height;
  fs_windows[fullscreen_count].has_monitors = 0;
  fs_windows[fullscreen_count].is_general_fullscreen = 0;
  fs_windows[fullscreen_count].is_monitor_fullscreen = 0;
  free(geom_reply);
  fullscreen_count++;
  return fullscreen_count - 1;
}

void remove_fullscreen_window(xcb_connection_t *conn, xcb_window_t window) {
  int index = -1;
  for (int i = 0; i < fullscreen_count; i++) {
    if (fs_windows[i].window == window) {
      index = i;
      break;
    }
  }
  if (index != -1) {
    xcb_delete_property(conn, window, atom_net_wm_state);
    uint32_t values[] = { fs_windows[index].original_geometry.x, fs_windows[index].original_geometry.y, fs_windows[index].original_geometry.width, fs_windows[index].original_geometry.height };
    uint16_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
    xcb_configure_window(conn, window, mask, values);

    fs_windows[index].is_general_fullscreen = 0;
    fs_windows[index].is_monitor_fullscreen = 0;

    for (int i = index; i < fullscreen_count - 1; i++)
      fs_windows[i] = fs_windows[i + 1];
    fullscreen_count--;
  }
}

void adjust_windows_within_bounds(xcb_connection_t *conn, xcb_window_t root) {
  xcb_query_tree_cookie_t tree_cookie = xcb_query_tree(conn, root);
  xcb_query_tree_reply_t *tree_reply = xcb_query_tree_reply(conn, tree_cookie, NULL);
  if (!tree_reply) {
    fprintf(stderr, "Failed to query window tree.\n");
    fflush(stderr);
    return;
  }

  int len = xcb_query_tree_children_length(tree_reply);
  xcb_window_t *children = xcb_query_tree_children(tree_reply);

  for (int i = 0; i < len; i++) {
    xcb_window_t child = children[i];
    if (child == active_window)
      continue;

    xcb_get_geometry_reply_t *geom_reply = NULL;
    xcb_get_geometry_cookie_t geom_cookie = xcb_get_geometry(conn, child);
    geom_reply = xcb_get_geometry_reply(conn, geom_cookie, NULL);
    if (!geom_reply)
      continue;

    int new_x = geom_reply->x;
    int new_y = geom_reply->y;

    if (geom_reply->x + geom_reply->width > total_width) {
      new_x = total_width - geom_reply->width;
      if (new_x < 0) new_x = 0;
    }
    if (geom_reply->y + geom_reply->height > total_height) {
      new_y = total_height - geom_reply->height;
      if (new_y < 0) new_y = 0;
    }

    if (new_x != geom_reply->x || new_y != geom_reply->y) {
      uint32_t values[] = { new_x, new_y };
      uint16_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
      xcb_configure_window(conn, child, mask, values);
    }

    free(geom_reply);
  }

  free(tree_reply);
}

void handle_client_message(xcb_connection_t *conn, xcb_client_message_event_t *cm, xcb_screen_t *screen) {
  if (cm->type == atom_net_wm_state) {
    xcb_atom_t atom1 = cm->data.data32[1];
    xcb_atom_t atom2 = cm->data.data32[2];
    int action = cm->data.data32[0];

    if (atom1 == atom_net_wm_state_above || atom2 == atom_net_wm_state_above) {
      if (action == 1) {
        uint32_t values[] = { XCB_STACK_MODE_ABOVE };
        xcb_configure_window(conn, cm->window, XCB_CONFIG_WINDOW_STACK_MODE, values);
        add_to_always_on_top(cm->window);
      } else if (action == 0) {
        uint32_t values[] = { XCB_STACK_MODE_BELOW };
        xcb_configure_window(conn, cm->window, XCB_CONFIG_WINDOW_STACK_MODE, values);
        remove_from_always_on_top(cm->window);
      } else if (action == 2) {
        if (is_always_on_top(cm->window)) {
          uint32_t values[] = { XCB_STACK_MODE_BELOW };
          xcb_configure_window(conn, cm->window, XCB_CONFIG_WINDOW_STACK_MODE, values);
          remove_from_always_on_top(cm->window);
        } else {
          uint32_t values[] = { XCB_STACK_MODE_ABOVE };
          xcb_configure_window(conn, cm->window, XCB_CONFIG_WINDOW_STACK_MODE, values);
          add_to_always_on_top(cm->window);
        }
      }
    }

    if (atom1 == atom_net_wm_state_fullscreen || atom2 == atom_net_wm_state_fullscreen) {
      int index = -1;
      for (int i = 0; i < fullscreen_count; i++) {
        if (fs_windows[i].window == cm->window) {
          index = i;
          break;
        }
      }

      int is_monitor_fullscreen = (index != -1) ? fs_windows[index].is_monitor_fullscreen : 0;
      if (is_monitor_fullscreen) {
        return;
      }

      if (action == 1) {
        if (index == -1) {
          index = add_fullscreen_window(conn, cm->window);
          if (index == -1) {
            return;
          }
        }
        fs_windows[index].is_general_fullscreen = 1;
        uint32_t values[] = { 0, 0, screen->width_in_pixels, screen->height_in_pixels };
        uint16_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
        xcb_configure_window(conn, cm->window, mask, values);
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, cm->window, atom_net_wm_state, XCB_ATOM_ATOM, 32, 1, &atom_net_wm_state_fullscreen);
        add_to_always_on_top(cm->window);
      } else if (action == 0) {
        remove_fullscreen_window(conn, cm->window);
      }
    }

    xcb_flush(conn);
  } else if (cm->type == atom_net_active_window) {
    xcb_window_t target_window = cm->window;
    if (target_window != XCB_WINDOW_NONE) {
      uint32_t values[] = { XCB_STACK_MODE_ABOVE };
      xcb_configure_window(conn, target_window, XCB_CONFIG_WINDOW_STACK_MODE, values);
      set_input_focus(conn, target_window);
    }
  } else if (cm->type == atom_net_wm_fullscreen_monitors) {
    int xs[4];
    xs[0] = cm->data.data32[0];
    xs[1] = cm->data.data32[1];
    xs[2] = cm->data.data32[2];
    xs[3] = cm->data.data32[3];

    int x1, y1, x2, y2;
    if (calculate_fullscreen_geometry(xs, &x1, &y1, &x2, &y2) != 0)
      return;

    uint32_t values[] = { x1, y1, x2 - x1, y2 - y1 };
    uint16_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
    xcb_configure_window(conn, cm->window, mask, values);
    xcb_change_property(conn, XCB_PROP_MODE_REPLACE, cm->window, atom_net_wm_state, XCB_ATOM_ATOM, 32, 1, &atom_net_wm_state_fullscreen);

    int index = -1;
    for (int i = 0; i < fullscreen_count; i++) {
      if (fs_windows[i].window == cm->window) {
        index = i;
        break;
      }
    }

    if (index == -1)
      index = add_fullscreen_window(conn, cm->window);

    if (index != -1) {
      for (int i = 0; i < 4; i++)
        fs_windows[index].monitors[i] = xs[i];
      fs_windows[index].has_monitors = 1;
      fs_windows[index].is_monitor_fullscreen = 1;
      fs_windows[index].is_general_fullscreen = 0;
    }
    xcb_flush(conn);
  }
}

void handle_destroy_notify(xcb_connection_t *conn, xcb_destroy_notify_event_t *ev, xcb_screen_t *screen) {
  xcb_window_t window = ev->window;

  if (is_always_on_top(window))
    remove_from_always_on_top(window);

  if (is_fullscreen_window(window))
    remove_fullscreen_window(conn, window);

  if (window == active_window) {
    active_window = XCB_WINDOW_NONE;
    remove_net_active_window(conn);
    remove_focus(window);
    xcb_window_t new_focus = get_top_focus();
    if (new_focus != XCB_WINDOW_NONE) {
      set_input_focus(conn, new_focus);
    }
  }

  remove_focus(window);

  xcb_flush(conn);
}

void handle_map_request(xcb_connection_t *conn, xcb_map_request_event_t *ev) {
  uint32_t values[] = { XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_FOCUS_CHANGE };
  xcb_change_window_attributes(conn, ev->window, XCB_CW_EVENT_MASK, values);
  xcb_map_window(conn, ev->window);

  xcb_change_property(conn, XCB_PROP_MODE_REPLACE, ev->window, atom_wm_protocols, XCB_ATOM_ATOM, 32, 1, &atom_wm_delete_window);
  xcb_icccm_get_text_property_reply_t prop;
  if (xcb_icccm_get_wm_name_reply(conn, xcb_icccm_get_wm_name(conn, ev->window), &prop, NULL)) {
    if (prop.name_len == 0) {
      const char *default_name = "Unnamed";
      xcb_change_property(conn, XCB_PROP_MODE_REPLACE, ev->window, atom_wm_name, XCB_ATOM_STRING, 8, strlen(default_name), default_name);
    }
    xcb_icccm_get_text_property_reply_wipe(&prop);
  }

  xcb_get_property_cookie_t name_cookie = xcb_get_property(conn, 0, ev->window, atom_net_wm_name, atom_utf8_string, 0, 1024);
  xcb_get_property_reply_t *name_reply = xcb_get_property_reply(conn, name_cookie, NULL);
  if (name_reply) {
    if (name_reply->value_len == 0) {
      const char *default_net_name = "Unnamed";
      xcb_change_property(conn, XCB_PROP_MODE_REPLACE, ev->window, atom_net_wm_name, atom_utf8_string, 8, strlen(default_net_name), default_net_name);
    }
    free(name_reply);
  }

  set_input_focus(conn, ev->window);
  for (int i = 0; i < always_on_top_count; i++) {
    uint32_t stack_values[] = { XCB_STACK_MODE_ABOVE };
    xcb_configure_window(conn, always_on_top_windows[i], XCB_CONFIG_WINDOW_STACK_MODE, stack_values);
  }
  xcb_flush(conn);
}

void handle_focus_in(xcb_connection_t *conn, xcb_focus_in_event_t *ev) {
  if (ev->mode != XCB_NOTIFY_MODE_NORMAL)
    return;

  if (ev->detail == XCB_NOTIFY_DETAIL_POINTER ||
      ev->detail == XCB_NOTIFY_DETAIL_NONE) {
    set_input_focus(conn, ev->event);
  }
}

void handle_focus_out(xcb_connection_t *conn, xcb_focus_out_event_t *ev, xcb_screen_t *screen) {
  if (ev->event == active_window) {
    remove_focus(ev->event);

    xcb_window_t new_focus = get_top_focus();
    if (new_focus != XCB_WINDOW_NONE) {
      set_input_focus(conn, new_focus);
    } else {
      xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, screen->root, XCB_CURRENT_TIME);
      active_window = XCB_WINDOW_NONE;
      remove_net_active_window(conn);
    }
  }
  xcb_flush(conn);
}

void handle_configure_request(xcb_connection_t *conn, xcb_configure_request_event_t *ev) {
  uint32_t values[7];
  uint16_t mask = 0;
  int i = 0;

  ev->value_mask & XCB_CONFIG_WINDOW_X && (values[i] = ev->x, mask |= XCB_CONFIG_WINDOW_X, i++);
  ev->value_mask & XCB_CONFIG_WINDOW_Y && (values[i] = ev->y, mask |= XCB_CONFIG_WINDOW_Y, i++);
  ev->value_mask & XCB_CONFIG_WINDOW_WIDTH && (values[i] = ev->width, mask |= XCB_CONFIG_WINDOW_WIDTH, i++);
  ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT && (values[i] = ev->height, mask |= XCB_CONFIG_WINDOW_HEIGHT, i++);
  ev->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH && (values[i] = ev->border_width, mask |= XCB_CONFIG_WINDOW_BORDER_WIDTH, i++);
  ev->value_mask & XCB_CONFIG_WINDOW_SIBLING && (values[i] = ev->sibling, mask |= XCB_CONFIG_WINDOW_SIBLING, i++);
  ev->value_mask & XCB_CONFIG_WINDOW_STACK_MODE && (values[i] = ev->stack_mode, mask |= XCB_CONFIG_WINDOW_STACK_MODE, i++);

  if (i > 0) {
    xcb_configure_window(conn, ev->window, mask, values);
  }
  xcb_flush(conn);
}

void handle_randr_event(xcb_connection_t *conn, xcb_randr_screen_change_notify_event_t *event, xcb_screen_t *screen) {
  query_xrandr(conn, screen);
  adjust_windows_within_bounds(conn, screen->root);

  fprintf(stderr, "display fun %d %d %d\n", event->width, event->height, event->rotation);
  fflush(stderr);

  for (int i = 0; i < always_on_top_count; i++) {
    uint32_t values[] = { XCB_STACK_MODE_ABOVE };
    xcb_configure_window(conn, always_on_top_windows[i], XCB_CONFIG_WINDOW_STACK_MODE, values);
  }

  for (int i = 0; i < fullscreen_count; i++) {
    xcb_window_t window = fs_windows[i].window;
    if (fs_windows[i].has_monitors) {
      int *xs = fs_windows[i].monitors;

      int x1, y1, x2, y2;
      if (calculate_fullscreen_geometry(xs, &x1, &y1, &x2, &y2) != 0)
        continue;

      int width = x2 - x1;
      int height = y2 - y1;
      uint32_t values[] = { x1, y1, width, height };
      uint16_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
      xcb_configure_window(conn, window, mask, values);
    } else {
      uint32_t values[] = { 0, 0, total_width, total_height };
      uint16_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
      xcb_configure_window(conn, window, mask, values);
    }
  }

  xcb_change_window_attributes(conn, screen->root, XCB_CW_BACK_PIXMAP, (uint32_t[]){XCB_NONE});
  xcb_clear_area(conn, 0, screen->root, 0, 0, screen->width_in_pixels, screen->height_in_pixels);
  set_wallpaper(conn, screen);
  xcb_flush(conn);
}

int main() {
  xcb_connection_t *conn = xcb_connect(NULL, NULL);
  if (xcb_connection_has_error(conn)) {
    fprintf(stderr, "Unable to connect to the X server\n");
    fflush(stderr);
    return -1;
  }

  xcb_screen_iterator_t iter = xcb_setup_roots_iterator(xcb_get_setup(conn));
  xcb_screen_t *screen = iter.data;
  uint32_t event_mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT
                      | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
                      | XCB_EVENT_MASK_PROPERTY_CHANGE
                      | XCB_EVENT_MASK_STRUCTURE_NOTIFY
                      | XCB_EVENT_MASK_FOCUS_CHANGE
                      | XCB_EVENT_MASK_EXPOSURE;
  xcb_void_cookie_t cookie = xcb_change_window_attributes_checked(conn, screen->root, XCB_CW_EVENT_MASK, &event_mask);
  xcb_generic_error_t *error = xcb_request_check(conn, cookie);
  if (error) {
    fprintf(stderr, "Another window manager is already running (error code %d).\n", error->error_code);
    fflush(stderr);
    free(error);
    xcb_disconnect(conn);
    return -1;
  }

  setup_atoms(conn);
  setup_ewmh(conn, screen);
  query_xrandr(conn, screen);

  set_wallpaper(conn, screen);

  const xcb_query_extension_reply_t *randr_reply = xcb_get_extension_data(conn, &xcb_randr_id);
  if (!randr_reply || !randr_reply->present) {
    fprintf(stderr, "RandR extension is not available.\n");
    if (randr_reply)
      free((void*)randr_reply);
    xcb_disconnect(conn);
    return -1;
  }

  uint8_t randr_event_base = randr_reply->first_event;
  uint8_t randr_error_base = randr_reply->first_error;

  xcb_randr_select_input(conn, screen->root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
  xcb_flush(conn);

  xcb_generic_event_t *event;
  while ((event = xcb_wait_for_event(conn))) {
    uint8_t x = event->response_type & ~0x80;

    if (event->response_type >= randr_event_base && event->response_type < randr_event_base + 2) {
      uint8_t randr_event = event->response_type - randr_event_base;
      if (randr_event == XCB_RANDR_SCREEN_CHANGE_NOTIFY)
        handle_randr_event(conn, (xcb_randr_screen_change_notify_event_t *)event, screen);
    } else {
      if (x == XCB_MAP_REQUEST) handle_map_request(conn, (xcb_map_request_event_t *)event);
      if (x == XCB_CONFIGURE_REQUEST) handle_configure_request(conn, (xcb_configure_request_event_t *)event);
      if (x == XCB_CLIENT_MESSAGE) handle_client_message(conn, (xcb_client_message_event_t *)event, screen);
      if (x == XCB_DESTROY_NOTIFY) handle_destroy_notify(conn, (xcb_destroy_notify_event_t *)event, screen);
      if (x == XCB_FOCUS_IN) handle_focus_in(conn, (xcb_focus_in_event_t *)event);
      if (x == XCB_FOCUS_OUT) handle_focus_out(conn, (xcb_focus_out_event_t *)event, screen);
      if (x == XCB_EXPOSE) set_wallpaper(conn, screen);
    }

    free(event);
  }

  if (active_window != XCB_WINDOW_NONE) remove_net_active_window(conn);
  xcb_disconnect(conn);
  return 0;
}