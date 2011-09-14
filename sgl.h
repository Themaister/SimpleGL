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

enum sgl_screen_type
{
   SGL_SCREEN_WINDOWED = 0,
   SGL_SCREEN_FULLSCREEN,
   SGL_SCREEN_WINDOWED_FULLSCREEN,

   SGL_INT_DUMMY = 0x7fffffff // Force at least 32-bit representation.
};

struct sgl_resolution
{
   // Requested width of window. Ignored if using windowed fullscreen.
   unsigned width;
   // Requested height of window. Ignored if using windowed fullscreen.
   unsigned height;

   // Monitor index. 0 = First monitor, 1 = Second monitor, etc ...
   unsigned monitor_index;
};

struct sgl_context_options
{
   // Resolution info.
   struct sgl_resolution res;

   // Window type.
   enum sgl_screen_type screen_type;

   // Swap interval. 0 = No VSync, 1 = VSync.
   unsigned swap_interval;

   // Initial window title.
   const char *title;
};

#if defined(_WIN32)
#define SGL_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
struct sgl_handles
{
   HWND hwnd;
   HGLRC hglrc;
   HDC hdc;
};
#elif defined(__APPLE__)
#define SGL_OSX
#else
#define SGL_X11
#include <GL/gl.h>
#include <GL/glx.h>
struct sgl_handles
{
   Display *dpy;
   Window win;
   GLXContext ctx;
};
#endif

#define SGL_OK 1
#define SGL_ERROR 0
#define SGL_TRUE 1
#define SGL_FALSE 0

// Get information about the available desktop modes.
// modes[0] will refer to the current desktop resolution.
// Can be called before sgl_init().
// The pointer must be free'd using free().
struct sgl_resolution *sgl_get_desktop_modes(unsigned *num_modes);

int sgl_init(const struct sgl_context_options *opts);
void sgl_deinit(void);

void sgl_set_window_title(const char *title);

// Check if window was resized. Might be resized even if the user didn't explicitly resize.
int sgl_check_resize(unsigned *width, unsigned *height);

void sgl_set_swap_interval(unsigned interval);
void sgl_swap_buffers(void);

int sgl_has_focus(void);

// When this returns 0, the window or application was killed (SIGINT/SIGTERM). 
int sgl_is_alive(void);

// Get underlying platform specific window handles. Use it to implement input.
void sgl_get_handles(struct sgl_handles *handles);

// GetProcAddress() wrapper.
typedef void (*sgl_function_t)(void);
sgl_function_t sgl_get_proc_address(const char *sym);

#ifdef __cplusplus
}
#endif

#endif

