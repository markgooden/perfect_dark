#ifndef GFX_WINDOW_MANAGER_API_H
#define GFX_WINDOW_MANAGER_API_H

#include <stdint.h>
#include <stdbool.h>

struct GfxWindowInitSettings {
    const char *title;
    uint32_t width;
    uint32_t height;
    int32_t x;
    int32_t y;
    bool fullscreen;
    bool fullscreen_is_exclusive;
    bool maximized;
    bool centered;
    bool allow_hidpi;
    /* Create the window without an OpenGL context: no SDL_WINDOW_OPENGL, no
     * SDL_GL_CreateContext. Backends that bring their own device (RT64 creates
     * a D3D12 or Vulkan swap chain against the bare window) need this, and a
     * window that already carries a GL context cannot be given to one. Set
     * from the selected backend in videoInit; false is the fast3d path and
     * leaves window creation exactly as it was. */
    bool no_gl;
};

struct GfxWindowManagerAPI {
    void (*init)(const struct GfxWindowInitSettings *settings);
    void (*close)(void);
    int (*get_display_mode)(int modenum, int *out_w, int *out_h);
    int (*get_current_display_mode)(int *out_w, int *out_h);
    int (*get_num_display_modes)(void);
    int32_t (*get_fullscreen_state)(void);
    void (*set_fullscreen_changed_callback)(void (*on_fullscreen_changed)(bool is_now_fullscreen));
    void (*set_fullscreen)(bool enable);
    void (*set_fullscreen_exclusive)(bool exc);
    void (*set_fullscreen_flag)(int32_t mode);
    int32_t (*get_fullscreen_flag_mode)(void);
    int32_t (*get_maximized_state)(void);
    void (*set_maximize)(bool enable);
    void (*get_active_window_refresh_rate)(uint32_t* refresh_rate);
    void (*set_cursor_visibility)(bool visible);
    void (*set_closest_resolution)(int32_t width, int32_t height, bool should_center);
    void (*set_dimensions)(uint32_t width, uint32_t height, int32_t posX, int32_t posY);
    void (*get_dimensions)(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY);
    void (*get_centered_positions)(int32_t width, int32_t height, int32_t *posX, int32_t *posY);
    void (*handle_events)(void);
    bool (*start_frame)(void);
    void (*swap_buffers_begin)(void);
    void (*swap_buffers_end)(void);
    double (*get_time)(void); // For debug
    int32_t (*get_target_fps)(void);
    void (*set_target_fps)(int fps);
    bool (*can_disable_vsync)(void);
    void *(*get_window_handle)(void);
    /* The platform's own handle for the same window - HWND on Windows - as
     * opposed to get_window_handle, which returns the SDL_Window *. A renderer
     * that is not built on SDL needs this one to attach a swap chain to.
     * Returns NULL if the platform has no single-pointer handle, or before
     * init. */
    void *(*get_native_window_handle)(void);
    void (*set_window_title)(const char *);
    int (*get_swap_interval)(void);
    bool (*set_swap_interval)(int);
};

#endif
