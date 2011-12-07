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
#include "sgl_keysym.h"

#include <X11/extensions/xf86vmode.h>
#include <X11/keysym.h>

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

static XF86VidModeModeInfo g_desktop_mode;
static bool g_should_reset_mode;

static struct sgl_input_callbacks g_input_cbs;
static bool g_mouse_grabbed;
static bool g_mouse_relative;
static int g_mouse_last_x;
static int g_mouse_last_y;

static int (*g_pglSwapInterval)(int);

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

static void show_mouse(void)
{
   XUndefineCursor(g_dpy, g_win);
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

   // Need GLX 1.4+.
   int major, minor;
   glXQueryVersion(g_dpy, &major, &minor);
   if (major < 1 || (major == 1 && minor < 4))
      goto error;

   // Initialize FBConfig and XVisuals.
   const int visual_attribs[] = {
      GLX_X_RENDERABLE     , True,
      GLX_DRAWABLE_TYPE    , GLX_WINDOW_BIT,
      GLX_RENDER_TYPE      , GLX_RGBA_BIT,
      GLX_X_VISUAL_TYPE    , GLX_TRUE_COLOR,
      GLX_RED_SIZE         , 8,
      GLX_GREEN_SIZE       , 8,
      GLX_BLUE_SIZE        , 8,
      GLX_ALPHA_SIZE       , 8,
      GLX_DEPTH_SIZE       , 24,
      GLX_STENCIL_SIZE     , 8,
      GLX_DOUBLEBUFFER     , True,
      None
   };

   int nelements;
   GLXFBConfig *fbc_temp = glXChooseFBConfig(g_dpy, DefaultScreen(g_dpy),
         visual_attribs, &nelements);

   if (!fbc_temp)
      goto error;

   GLXFBConfig fbc = fbc_temp[0];
   XFree(fbc_temp);

   XVisualInfo *vi = glXGetVisualFromFBConfig(g_dpy, fbc);
   if (!vi)
      goto error;

   // Create Window.
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
      XGrabKeyboard(g_dpy, g_win, True, GrabModeAsync, GrabModeAsync, CurrentTime);
   }
   else
      XMapWindow(g_dpy, g_win);

   if (opts->screen_type == SGL_SCREEN_WINDOWED_FULLSCREEN)
      set_windowed_fullscreen();

   g_quit_atom = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
   if (g_quit_atom)
      XSetWMProtocols(g_dpy, g_win, &g_quit_atom, 1);

   // Catch signals.
   struct sigaction sa = {
      .sa_handler = sighandler,
      .sa_flags = SA_RESTART,
   };
   sigemptyset(&sa.sa_mask);
   sigaction(SIGINT, &sa, NULL);
   sigaction(SIGTERM, &sa, NULL);

   XEvent event;
   XIfEvent(g_dpy, &event, glx_wait_notify, NULL);

   // Create context.
   if (opts->context.style == SGL_CONTEXT_MODERN)
   {
      fprintf(stderr, "[SGL]: Creating modern OpenGL %u.%u context!\n", opts->context.major, opts->context.minor);
      typedef GLXContext (*ContextProc)(Display*, GLXFBConfig,
            GLXContext, Bool, const int *);

      ContextProc proc = (ContextProc)glXGetProcAddress((const GLubyte *)"glXCreateContextAttribsARB");
      if (!proc)
      {
         fprintf(stderr, "[SGL]: Failed to get glXCreateContextAttribsARB symbol!\n");
         goto error;
      }

      const int attribs[] = {
         GLX_CONTEXT_MAJOR_VERSION_ARB, opts->context.major,
         GLX_CONTEXT_MINOR_VERSION_ARB, opts->context.minor,
         GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
         None,
      };

      g_ctx = proc(g_dpy, fbc, 0, true, attribs);
   }
   else
   {
      fprintf(stderr, "[SGL]: Creating legacy OpenGL context!\n");
      g_ctx = glXCreateNewContext(g_dpy, fbc, GLX_RGBA_TYPE, 0, True);
   }
   
   glXMakeCurrent(g_dpy, g_win, g_ctx);
   XSync(g_dpy, False);

   int val;
   glXGetConfig(g_dpy, vi, GLX_DOUBLEBUFFER, &val);

   g_is_double_buffered = val;
   if (g_is_double_buffered)
   {
      if (!g_pglSwapInterval)
         g_pglSwapInterval = (int (*)(int))glXGetProcAddress((const GLubyte*)"glXSwapIntervalSGI");
      if (!g_pglSwapInterval)
         g_pglSwapInterval = (int (*)(int))glXGetProcAddress((const GLubyte*)"glXSwapIntervalMESA");
      if (g_pglSwapInterval)
         g_pglSwapInterval(opts->swap_interval);
   }
   else
      fprintf(stderr, "[SGL]: GLX is not double buffered!\n");

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
   if (g_pglSwapInterval)
      g_pglSwapInterval(interval);
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

static void handle_key_press(int key, int pressed);
static void handle_button_press(int button, int pressed, int x, int y);
static void handle_motion(int x, int y);

int sgl_is_alive(void)
{
   int old_mouse_x = g_mouse_last_x;
   int old_mouse_y = g_mouse_last_y;

   XEvent event;
   while (XPending(g_dpy))
   {
      XNextEvent(g_dpy, &event);
      switch (event.type)
      {
         case KeyPress:
         case KeyRelease:
            handle_key_press(XLookupKeysym(&event.xkey, 0), event.xkey.type == KeyPress);
            break;

         case ButtonPress:
         case ButtonRelease:
            handle_button_press(event.xbutton.button, 
                  event.xbutton.type == ButtonPress,
                  event.xbutton.x,
                  event.xbutton.y);
            break;

         case MotionNotify:
            handle_motion(event.xmotion.x, event.xmotion.y);
            break;

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

   if (g_mouse_relative && g_input_cbs.mouse_move_cb)
   {
      int delta_x = g_mouse_last_x - old_mouse_x;
      int delta_y = g_mouse_last_y - old_mouse_y;
      
      if (delta_x || delta_y)
         g_input_cbs.mouse_move_cb(delta_x, delta_y);
   }

   if (g_mouse_grabbed)
   {
      XWarpPointer(g_dpy, None, g_win, 0, 0, 0, 0,
            g_last_width >> 1, g_last_height >> 1);
      g_mouse_last_x = g_last_width >> 1;
      g_mouse_last_y = g_last_height >> 1;
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

// Input.
struct key_bind
{
   int x;
   int sglk;
};

static const struct key_bind lut_binds[] = {
   { XK_Left, SGLK_LEFT },
   { XK_Right, SGLK_RIGHT },
   { XK_Up, SGLK_UP },
   { XK_Down, SGLK_DOWN },
   { XK_Return, SGLK_RETURN },
   { XK_Tab, SGLK_TAB },
   { XK_Insert, SGLK_INSERT },
   { XK_Delete, SGLK_DELETE },
   { XK_Shift_R, SGLK_RSHIFT },
   { XK_Shift_L, SGLK_LSHIFT },
   { XK_Control_L, SGLK_LCTRL },
   { XK_Alt_L, SGLK_LALT },
   { XK_space, SGLK_SPACE },
   { XK_Escape, SGLK_ESCAPE },
   { XK_BackSpace, SGLK_BACKSPACE },
   { XK_KP_Enter, SGLK_KP_ENTER },
   { XK_KP_Add, SGLK_KP_PLUS },
   { XK_KP_Subtract, SGLK_KP_MINUS },
   { XK_KP_Multiply, SGLK_KP_MULTIPLY },
   { XK_KP_Divide, SGLK_KP_DIVIDE },
   { XK_grave, SGLK_BACKQUOTE },
   { XK_Pause, SGLK_PAUSE },
   { XK_KP_0, SGLK_KP0 },
   { XK_KP_1, SGLK_KP1 },
   { XK_KP_2, SGLK_KP2 },
   { XK_KP_3, SGLK_KP3 },
   { XK_KP_4, SGLK_KP4 },
   { XK_KP_5, SGLK_KP5 },
   { XK_KP_6, SGLK_KP6 },
   { XK_KP_7, SGLK_KP7 },
   { XK_KP_8, SGLK_KP8 },
   { XK_KP_9, SGLK_KP9 },
   { XK_0, SGLK_0 },
   { XK_1, SGLK_1 },
   { XK_2, SGLK_2 },
   { XK_3, SGLK_3 },
   { XK_4, SGLK_4 },
   { XK_5, SGLK_5 },
   { XK_6, SGLK_6 },
   { XK_7, SGLK_7 },
   { XK_8, SGLK_8 },
   { XK_9, SGLK_9 },
   { XK_F1, SGLK_F1 },
   { XK_F2, SGLK_F2 },
   { XK_F3, SGLK_F3 },
   { XK_F4, SGLK_F4 },
   { XK_F5, SGLK_F5 },
   { XK_F6, SGLK_F6 },
   { XK_F7, SGLK_F7 },
   { XK_F8, SGLK_F8 },
   { XK_F9, SGLK_F9 },
   { XK_F10, SGLK_F10 },
   { XK_F11, SGLK_F11 },
   { XK_F12, SGLK_F12 },
   { XK_a, SGLK_a },
   { XK_b, SGLK_b },
   { XK_c, SGLK_c },
   { XK_d, SGLK_d },
   { XK_e, SGLK_e },
   { XK_f, SGLK_f },
   { XK_g, SGLK_g },
   { XK_h, SGLK_h },
   { XK_i, SGLK_i },
   { XK_j, SGLK_j },
   { XK_k, SGLK_k },
   { XK_l, SGLK_l },
   { XK_m, SGLK_m },
   { XK_n, SGLK_n },
   { XK_o, SGLK_o },
   { XK_p, SGLK_p },
   { XK_q, SGLK_q },
   { XK_r, SGLK_r },
   { XK_s, SGLK_s },
   { XK_t, SGLK_t },
   { XK_u, SGLK_u },
   { XK_v, SGLK_v },
   { XK_w, SGLK_w },
   { XK_x, SGLK_x },
   { XK_y, SGLK_y },
   { XK_z, SGLK_z },
};

void sgl_set_input_callbacks(const struct sgl_input_callbacks *cbs)
{
   g_input_cbs = *cbs;
   XSelectInput(g_dpy, g_win,
         (cbs->key_cb ? KeyPressMask | KeyReleaseMask : 0) |
         (cbs->mouse_button_cb ? ButtonPressMask | ButtonReleaseMask : 0) |
         (cbs->mouse_move_cb ? PointerMotionMask : 0));
}

static void handle_key_press(int key, int pressed)
{
   if (!g_input_cbs.key_cb)
      return;

   for (unsigned i = 0; i < sizeof(lut_binds) / sizeof(lut_binds[0]); i++)
   {
      if (key == lut_binds[i].x)
      {
         g_input_cbs.key_cb(lut_binds[i].sglk, pressed);
         return;
      }
   }
}

static void handle_button_press(int button, int pressed, int x, int y)
{
   if (!g_input_cbs.mouse_button_cb)
      return;

   g_input_cbs.mouse_button_cb(button, pressed, x, y);
}

static void handle_motion(int x, int y)
{
   if (!g_input_cbs.mouse_move_cb)
      return;

   if (!g_mouse_relative)
      g_input_cbs.mouse_move_cb(x, y);
   else
   {
      g_mouse_last_x = x;
      g_mouse_last_y = y;
   }
}

void sgl_set_mouse_mode(int grab, int relative, int visible)
{
   g_mouse_relative = relative;

   if (g_should_reset_mode) // Fullscreen
      return;
   
   g_mouse_grabbed = grab;
   if (grab)
   {
      g_mouse_last_x = g_last_width >> 1;
      g_mouse_last_y = g_last_height >> 1;
      XWarpPointer(g_dpy, None, g_win, 0, 0, 0, 0,
            g_last_width >> 1, g_last_height >> 1);

      XGrabPointer(g_dpy, g_win, True,
            ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
            GrabModeAsync, GrabModeAsync, g_win, None, CurrentTime);
   }
   else
      XUngrabPointer(g_dpy, CurrentTime);

   if (visible)
      show_mouse();
   else
      hide_mouse();
}

