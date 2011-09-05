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

#define SGL_SCREEN_WINDOWED 0x0
#define SGL_SCREEN_FULLSCREEN 0x1
#define SGL_SCREEN_WINDOWED_FULLSCREEN 0x2

struct sgl_context_options
{
   unsigned width;
   unsigned height;

   unsigned screen_type;
   unsigned monitor_index;
   unsigned swap_interval;

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

int sgl_init(const struct sgl_context_options *opts);
void sgl_deinit(void);

void sgl_set_window_title(const char *title);
int sgl_check_resize(unsigned *width, unsigned *height);
void sgl_set_swap_interval(unsigned interval);
void sgl_swap_buffers(void);

int sgl_has_focus(void);
int sgl_is_alive(void);

void sgl_get_handles(struct sgl_handles *handles);

typedef void (*sgl_function_t)(void);
sgl_function_t sgl_get_proc_address(const char *sym);

#ifdef __cplusplus
}
#endif

#endif

