/* 
 * Copyright (c) 2011, Hans-Kristian Arntzen <maister@archlinux.us>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef SGL_H__
#define SGL_H__

#ifdef __cplusplus
extern "C" {
#endif

#define SGL_SCREEN_WINDOWED 0
#define SGL_SCREEN_FULLSCREEN 1
#define SGL_SCREEN_WINDOWED_FULLSCREEN 2

#define SGL_CONTEXT_LEGACY 0
#define SGL_CONTEXT_MODERN 1

#ifdef SGL_HAVE_EGL
#define SGL_CONTEXT_GLES 2
#endif

struct sgl_resolution
{
   /* Requested width of window. Ignored if using windowed fullscreen. */
   unsigned width;
   /* Requested height of window. Ignored if using windowed fullscreen. */
   unsigned height;

   /* Monitor index. 0 = First monitor, 1 = Second monitor, etc ... */
   unsigned monitor_index;
};

struct sgl_context_options
{
   /* Resolution info. */
   struct sgl_resolution res;

   /* Context style. */
   struct
   {
      int style;

      /* Major/minor OpenGL version used for modern contexts. */
      unsigned major;
      unsigned minor;
   } context;

   /* Window type. */
   int screen_type;

   /* Swap interval. 0 = No VSync, 1 = VSync. */
   unsigned swap_interval;

   /* Multisampling (AA). A value of 0 implies 1xAA. */
   unsigned samples;

   /* Initial window title. */
   const char *title;
};

#define GL_GLEXT_PROTOTYPES

/* Win32 */
#if defined(_WIN32)
#define SGL_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#ifdef SGL_EXPOSE_INTERNAL
struct sgl_handles
{
   HWND hwnd;
   HGLRC hglrc;
   HDC hdc;
};
#else
struct sgl_handles;
#endif

/* OSX */
#elif defined(__APPLE__)
#define SGL_OSX
#else

/* X11 */
#define SGL_X11
#ifdef SGL_HAVE_EGL
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#else
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#endif
#ifdef SGL_EXPOSE_INTERNAL
#include <GL/glx.h>
struct sgl_handles
{
   Display *dpy;
   Window win;
   GLXContext ctx;
};
#else
struct sgl_handles;
#endif
#endif

#define SGL_OK 1
#define SGL_ERROR 0
#define SGL_TRUE 1
#define SGL_FALSE 0

/* Get information about the available desktop modes.
 * modes[0] will refer to the current desktop resolution.
 * Can be called before sgl_init().
 * The pointer must be free'd using free(). */
struct sgl_resolution *sgl_get_desktop_modes(unsigned *num_modes);

int sgl_init(const struct sgl_context_options *opts);
void sgl_deinit(void);

void sgl_set_window_title(const char *title);

/* Check if window was resized. Might be resized even if the user didn't explicitly resize. */
int sgl_check_resize(unsigned *width, unsigned *height);

void sgl_set_swap_interval(unsigned interval);
void sgl_swap_buffers(void);

int sgl_has_focus(void);

/* When this returns 0, the window or application was killed (SIGINT/SIGTERM). */ 
int sgl_is_alive(void);

/* Get underlying platform specific window handles. Use it to implement input. */
void sgl_get_handles(struct sgl_handles *handles);

/* GetProcAddress() wrapper. */
typedef void (*sgl_function_t)(void);
sgl_function_t sgl_get_proc_address(const char *sym);

/* Input callbacks. If non-NULL a callback may be called one or more times in calls to sgl_is_alive().
 * Coordinates for mouse are absolute with respect to the window. */
typedef void (*sgl_key_callback_t)(int key, int pressed);
typedef void (*sgl_mouse_move_callback_t)(int x, int y);
typedef void (*sgl_mouse_button_callback_t)(int button, int pressed, int x, int y);
struct sgl_input_callbacks
{
   sgl_key_callback_t key_cb;
   sgl_mouse_move_callback_t mouse_move_cb;
   sgl_mouse_button_callback_t mouse_button_cb;
};

void sgl_set_input_callbacks(const struct sgl_input_callbacks *cbs);
void sgl_set_mouse_mode(int capture, int relative, int visible);

#ifdef __cplusplus
}
#endif

#endif

