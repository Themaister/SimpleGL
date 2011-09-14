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

#include "sgl.h"

#include <GL/gl.h>

#include <GL/glx.h>
#include <X11/extensions/xf86vmode.h>

#include <stddef.h>
#include <signal.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>

static Display *g_dpy;
static Window g_win;
static GLXContext g_ctx;
static Colormap g_cmap;

static bool g_inited;
static bool g_is_double_buffered;

static int g_last_width;
static int g_last_height;
static bool g_resized;

static Atom g_quit_atom;
static volatile sig_atomic_t g_quit;
static bool g_has_focus;

static int (*pglSwapInterval)(int);

static void sighandler(int sig)
{
   (void)sig;
   g_quit = 1;
}

static Bool glx_wait_notify(Display *d, XEvent *e, char *arg)
{
   (void)d;
   (void)arg;
   return (e->type == MapNotify) && (e->xmap.window == g_win);
}

static void hide_mouse(void)
{
   Cursor no_ptr;
   Pixmap bm_no;
   XColor black, dummy;
   Colormap colormap;
   static char bm_no_data[] = {0, 0, 0, 0, 0, 0, 0, 0};
   colormap = DefaultColormap(g_dpy, DefaultScreen(g_dpy));
   if (!XAllocNamedColor(g_dpy, colormap, "black", &black, &dummy))
      return;

   bm_no = XCreateBitmapFromData(g_dpy, g_win, bm_no_data, 8, 8);
   no_ptr = XCreatePixmapCursor(g_dpy, bm_no, bm_no, &black, &black, 0, 0);
   XDefineCursor(g_dpy, g_win, no_ptr);
   XFreeCursor(g_dpy, no_ptr);
   if (bm_no != None)
      XFreePixmap(g_dpy, bm_no);
   XFreeColors(g_dpy, colormap, &black.pixel, 1, 0);
}

static Atom XA_NET_WM_STATE;
static Atom XA_NET_WM_STATE_FULLSCREEN;
#define XA_INIT(x) XA##x = XInternAtom(g_dpy, #x, False)
#define _NET_WM_STATE_ADD 1
static void set_windowed_fullscreen(void)
{
   XA_INIT(_NET_WM_STATE);
   XA_INIT(_NET_WM_STATE_FULLSCREEN);

   if (!XA_NET_WM_STATE || !XA_NET_WM_STATE_FULLSCREEN)
   {
      fprintf(stderr, "[SGL]: GLX cannot set fullscreen :(\n");
      return;
   }

   XEvent xev;

   xev.xclient.type = ClientMessage;
   xev.xclient.serial = 0;
   xev.xclient.send_event = True;
   xev.xclient.message_type = XA_NET_WM_STATE;
   xev.xclient.window = g_win;
   xev.xclient.format = 32;
   xev.xclient.data.l[0] = _NET_WM_STATE_ADD;
   xev.xclient.data.l[1] = XA_NET_WM_STATE_FULLSCREEN;
   xev.xclient.data.l[2] = 0;
   xev.xclient.data.l[3] = 0;
   xev.xclient.data.l[4] = 0;

   XSendEvent(g_dpy, DefaultRootWindow(g_dpy), False,
         SubstructureRedirectMask | SubstructureNotifyMask,
         &xev);
}

static XF86VidModeModeInfo g_desktop_mode;
static bool g_should_reset_mode;

struct sgl_resolution *sgl_get_desktop_modes(unsigned *num_modes)
{
   XF86VidModeModeInfo **modes;
   int mode_num;
   Display *dpy = XOpenDisplay(NULL);
   if (!dpy)
      return NULL;

   XF86VidModeGetAllModeLines(dpy, DefaultScreen(dpy), &mode_num, &modes);

   struct sgl_resolution *sgl_modes = calloc(mode_num, sizeof(*sgl_modes));
   if (!sgl_modes)
      goto error;

   for (int i = 0; i < mode_num; i++)
   {
      sgl_modes[i].width = modes[i]->hdisplay;
      sgl_modes[i].height = modes[i]->vdisplay;
   }

   XCloseDisplay(dpy);
   XFree(modes);

   *num_modes = mode_num;

   return sgl_modes;

error:
   XCloseDisplay(dpy);
   XFree(modes);
   return NULL;
}

static void set_desktop_mode(void)
{
   XF86VidModeModeInfo **modes;
   int num_modes;
   XF86VidModeGetAllModeLines(g_dpy, DefaultScreen(g_dpy), &num_modes, &modes);
   g_desktop_mode = *modes[0];
   XFree(modes);
}

static bool get_video_mode(int width, int height, XF86VidModeModeInfo *mode)
{
   XF86VidModeModeInfo **modes;
   int num_modes;
   XF86VidModeGetAllModeLines(g_dpy, DefaultScreen(g_dpy), &num_modes, &modes);

   bool ret = false;
   for (int i = 0; i < num_modes; i++)
   {
      if (modes[i]->hdisplay == width && modes[i]->vdisplay == height)
      {
         *mode = *modes[i];
         ret = true;
         break;
      }
   }

   XFree(modes);
   return ret;
}

int sgl_init(const struct sgl_context_options *opts)
{
   if (g_inited)
      return SGL_ERROR;

   g_quit = 0;
   g_has_focus = true;
   g_resized = false;

   g_dpy = XOpenDisplay(NULL);
   if (!g_dpy)
      goto error;

   int major, minor;
   glXQueryVersion(g_dpy, &major, &minor);
   if (major < 1 || (major == 1 && minor < 2)) // Need GLX 1.2+.
      goto error;

   int attrs[] = { GLX_RGBA, GLX_DOUBLEBUFFER, None };
   XVisualInfo *vi = glXChooseVisual(g_dpy, DefaultScreen(g_dpy), attrs);

   bool fullscreen = opts->screen_type == SGL_SCREEN_FULLSCREEN;
   
   XSetWindowAttributes swa = {
      .colormap = g_cmap = XCreateColormap(g_dpy, RootWindow(g_dpy, vi->screen), vi->visual, AllocNone),
      .border_pixel = 0,
      .event_mask = StructureNotifyMask,
      .override_redirect = fullscreen ? True : False,
   };

   unsigned width = opts->res.width;
   unsigned height = opts->res.height;

   set_desktop_mode();

   if (fullscreen)
   {
      XF86VidModeModeInfo mode;
      if (get_video_mode(width, height, &mode))
      {
         XF86VidModeSwitchToMode(g_dpy, DefaultScreen(g_dpy), &mode);
         XF86VidModeSetViewPort(g_dpy, DefaultScreen(g_dpy), 0, 0);
         g_should_reset_mode = true;
      }
      else
         goto error;
   }
   else if (opts->screen_type == SGL_SCREEN_WINDOWED_FULLSCREEN)
   {
      width = g_desktop_mode.hdisplay;
      height = g_desktop_mode.vdisplay;
   }

   g_win = XCreateWindow(g_dpy, RootWindow(g_dpy, vi->screen),
         0, 0, width, height, 0,
         vi->depth, InputOutput, vi->visual, 
         CWBorderPixel | CWColormap | CWEventMask | (fullscreen ? CWOverrideRedirect : 0), &swa);
   XSetWindowBackground(g_dpy, g_win, 0);

   g_last_width = opts->res.width;
   g_last_height = opts->res.height;

   sgl_set_window_title(opts->title);

   if (fullscreen)
   {
      XMapRaised(g_dpy, g_win);
      XWarpPointer(g_dpy, None, g_win, 0, 0, 0, 0, 0, 0);
      XGrabKeyboard(g_dpy, g_win, True, GrabModeAsync, GrabModeAsync, CurrentTime);
      XGrabPointer(g_dpy, g_win, True, ButtonPressMask,
            GrabModeAsync, GrabModeAsync, g_win, None, CurrentTime);
   }
   else
      XMapWindow(g_dpy, g_win);

   if (opts->screen_type == SGL_SCREEN_WINDOWED_FULLSCREEN)
      set_windowed_fullscreen();

   g_quit_atom = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
   if (g_quit_atom)
      XSetWMProtocols(g_dpy, g_win, &g_quit_atom, 1);

   struct sigaction sa = {
      .sa_handler = sighandler,
      .sa_flags = SA_RESTART,
   };
   sigemptyset(&sa.sa_mask);
   sigaction(SIGINT, &sa, NULL);
   sigaction(SIGTERM, &sa, NULL);

   XEvent event;
   XIfEvent(g_dpy, &event, glx_wait_notify, NULL);

   g_ctx = glXCreateContext(g_dpy, vi, NULL, GL_TRUE);
   glXMakeCurrent(g_dpy, g_win, g_ctx);

   int val;
   glXGetConfig(g_dpy, vi, GLX_DOUBLEBUFFER, &val);
   g_is_double_buffered = val;
   if (!g_is_double_buffered)
      fprintf(stderr, "[SGL]: GLX is not double buffered!\n");

   if (!pglSwapInterval)
      pglSwapInterval = (int (*)(int))glXGetProcAddress((const GLubyte*)"glXSwapIntervalSGI");
   if (!pglSwapInterval)
      pglSwapInterval = (int (*)(int))glXGetProcAddress((const GLubyte*)"glXSwapIntervalMESA");
   if (pglSwapInterval)
      pglSwapInterval(opts->swap_interval);

   hide_mouse();

   g_inited = true;
   return SGL_OK;
   
error:
   sgl_deinit();
   return SGL_ERROR;
}

void sgl_deinit(void)
{
   if (g_ctx)
   {
      glXDestroyContext(g_dpy, g_ctx);
      g_ctx = NULL;
   }

   if (g_win)
   {
      XDestroyWindow(g_dpy, g_win);
      g_win = None;
   }

   if (g_cmap)
   {
      XFreeColormap(g_dpy, g_cmap);
      g_cmap = None;
   }

   if (g_should_reset_mode)
   {
      XF86VidModeSwitchToMode(g_dpy, DefaultScreen(g_dpy), &g_desktop_mode);
      XF86VidModeSetViewPort(g_dpy, DefaultScreen(g_dpy), 0, 0);
      g_should_reset_mode = false;
   }

   if (g_dpy)
   {
      XCloseDisplay(g_dpy);
      g_dpy = NULL;
   }

   g_inited = false;
}

void sgl_swap_buffers(void)
{
   if (g_is_double_buffered)
      glXSwapBuffers(g_dpy, g_win);
}

void sgl_set_swap_interval(unsigned interval)
{
   if (pglSwapInterval)
      pglSwapInterval(interval);
}

int sgl_check_resize(unsigned *width, unsigned *height)
{
   if (g_resized)
   {
      *width = g_last_width;
      *height = g_last_height;
      g_resized = false;
      return SGL_TRUE;
   }
   else
      return SGL_FALSE;
}

int sgl_is_alive(void)
{
   XEvent event;
   while (XPending(g_dpy))
   {
      XNextEvent(g_dpy, &event);
      switch (event.type)
      {
         case ClientMessage:
            if ((Atom)event.xclient.data.l[0] == g_quit_atom)
               g_quit = true;
            break;

         case ConfigureNotify:
            if (event.xconfigure.width != g_last_width || event.xconfigure.height != g_last_height)
            {
               g_resized = true;
               g_last_width = event.xconfigure.width;
               g_last_height = event.xconfigure.height;
            }
            printf("Configure event! Width = %u, Height = %u\n", g_last_width, g_last_height);
            break;

         case DestroyNotify:
            g_quit = true;
            break;

         case MapNotify:
            g_has_focus = true;
            break;

         case UnmapNotify:
            g_has_focus = false;
            break;
      }
   }

   return !g_quit;
}

int sgl_has_focus(void)
{
   if (!sgl_is_alive())
      return SGL_FALSE;

   Window win;
   int rev;
   XGetInputFocus(g_dpy, &win, &rev);

   return (win == g_win && g_has_focus) || g_should_reset_mode; // Fullscreen
}

void sgl_set_window_title(const char *name)
{
   if (name)
      XStoreName(g_dpy, g_win, (char*)name);
}

sgl_function_t sgl_get_proc_address(const char *sym)
{
   return glXGetProcAddress((const GLubyte*)sym);
}

void sgl_get_handles(struct sgl_handles *handles)
{
   handles->dpy = g_dpy;
   handles->win = g_win;
   handles->ctx = g_ctx;
}

