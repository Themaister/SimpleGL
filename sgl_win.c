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

#define SGL_EXPOSE_INTERNAL
#include "sgl.h"
#include "sgl_keysym.h"

#define WGL_WGLEXT_PROTOTYPES
#include <GL/wglext.h>

#include <stdio.h>
#include <windowsx.h>
#include <stdlib.h>

static HWND g_hwnd;
static HGLRC g_hrc;
static HDC g_hdc;

static BOOL g_quit;
static BOOL g_inited;

static BOOL g_resized;
static unsigned g_resize_width;
static unsigned g_resize_height;

static BOOL g_fullscreen;

static BOOL g_ctx_modern;
static unsigned g_gl_major;
static unsigned g_gl_minor;
static unsigned g_samples;

static struct sgl_input_callbacks g_input_cbs;
static BOOL g_mouse_relative;
static BOOL g_mouse_grabbed;
static int g_mouse_last_x;
static int g_mouse_last_y;
static BOOL g_mouse_delta_invalid;

static void setup_pixel_format(HDC hdc)
{
   int num_pixel_format;
   static PIXELFORMATDESCRIPTOR pfd = {0};
   pfd.nSize = sizeof(PIXELFORMATDESCRIPTOR);
   pfd.nVersion = 1;
   pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
   pfd.iPixelType = PFD_TYPE_RGBA;
   pfd.cColorBits = 32;
   pfd.cDepthBits = 24;
   pfd.cStencilBits = 8;
   pfd.iLayerType = PFD_MAIN_PLANE;

   num_pixel_format = ChoosePixelFormat(hdc, &pfd);
   SetPixelFormat(hdc, num_pixel_format, &pfd);
}

static PFNWGLCHOOSEPIXELFORMATEXTPROC pwglChoosePixelFormatARB;
static PFNWGLCREATECONTEXTATTRIBSARBPROC pwglCreateContextAttribsARB;

static void setup_dummy_window(void)
{
   HWND dummy;
   HDC hdc;
   HGLRC ctx;
   WNDCLASSEXA dummy_class = {0};
   dummy_class.cbSize = sizeof(dummy_class);
   dummy_class.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
   dummy_class.lpfnWndProc = DefWindowProc;
   dummy_class.hInstance = GetModuleHandle(NULL);
   dummy_class.hCursor = LoadCursor(NULL, IDC_ARROW);
   dummy_class.lpszClassName = "Dummy Window";

   RegisterClassExA(&dummy_class);
   dummy = CreateWindowExA(0, "Dummy Window", "",
         WS_OVERLAPPEDWINDOW,
         CW_USEDEFAULT, CW_USEDEFAULT, 1, 1,
         NULL, NULL, NULL, NULL);

   ShowWindow(dummy, SW_HIDE);
   hdc = GetDC(dummy);
   setup_pixel_format(hdc);
   ctx = wglCreateContext(hdc);
   wglMakeCurrent(hdc, ctx);

   pwglChoosePixelFormatARB = (PFNWGLCHOOSEPIXELFORMATEXTPROC)wglGetProcAddress("wglChoosePixelFormatARB");
   pwglCreateContextAttribsARB = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");

   wglMakeCurrent(NULL, NULL);
   wglDeleteContext(ctx);
   DestroyWindow(dummy);
   UnregisterClassA("Dummy Window", GetModuleHandle(NULL));
}

static void setup_pixel_format_modern(HDC hdc)
{
   int pixel_format;
   UINT num_formats;
   PIXELFORMATDESCRIPTOR pfd;
   float fattrs[2];
   int attribs[] = {
      WGL_DOUBLE_BUFFER_ARB, TRUE,
      WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB,
      WGL_RED_BITS_ARB, 8,
      WGL_GREEN_BITS_ARB, 8,
      WGL_BLUE_BITS_ARB, 8,
      WGL_ALPHA_BITS_ARB, 8,
      WGL_DEPTH_BITS_ARB, 24,
      WGL_STENCIL_BITS_ARB, 8,
      WGL_SAMPLE_BUFFERS_ARB, 1,
      WGL_SAMPLES_ARB, 0,
      0, 0,
   };

   attribs[19] = g_samples;
   fattrs[0] = fattrs[1] = 0.0f;

   pwglChoosePixelFormatARB(hdc, attribs, fattrs, 1, &pixel_format, &num_formats);

   DescribePixelFormat(hdc, pixel_format, sizeof(pfd), &pfd);
   SetPixelFormat(hdc, pixel_format, &pfd);
}

static void create_gl_context(HWND hwnd)
{
   BOOL has_modern;
   g_hdc = GetDC(hwnd);

   has_modern = pwglChoosePixelFormatARB && pwglCreateContextAttribsARB;

   if (has_modern)
      setup_pixel_format_modern(g_hdc);
   else
      setup_pixel_format(g_hdc);
   
   if (g_ctx_modern && has_modern)
   {
      int attribs[] = {
         WGL_CONTEXT_MAJOR_VERSION_ARB, 0,
         WGL_CONTEXT_MINOR_VERSION_ARB, 0,
         WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
#ifdef DEBUG
         WGL_CONTEXT_FLAGS_ARB, WGL_CONTEXT_DEBUG_BIT_ARB,
#endif
         0
      };

      attribs[1] = g_gl_major;
      attribs[3] = g_gl_minor;

      g_hrc = pwglCreateContextAttribsARB(g_hdc, NULL, attribs);
      wglMakeCurrent(g_hdc, g_hrc);
   }
   else
   {
      g_hrc = wglCreateContext(g_hdc);
      wglMakeCurrent(g_hdc, g_hrc);
   }
}

static void handle_key_press(WPARAM key, int pressed);
static void handle_mouse_move(int x, int y);
static void handle_mouse_press(UINT message, int x, int y);

static LRESULT CALLBACK WndProc(HWND hwnd, UINT message,
      WPARAM wparam, LPARAM lparam)
{
   switch (message)
   {
      case WM_SYSCOMMAND:
         /* Prevent screensavers, etc, while running :) */
         switch (wparam)
         {
            case SC_SCREENSAVE:
            case SC_MONITORPOWER:
               return 0;
         }
         break;

      case WM_KEYDOWN:
      case WM_KEYUP:
         handle_key_press(wparam, message == WM_KEYDOWN);
         return 0;

      case WM_MOUSEMOVE:
         handle_mouse_move(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
         return 0;

      case WM_LBUTTONDOWN:
      case WM_RBUTTONDOWN:
      case WM_MBUTTONDOWN:
      case WM_LBUTTONUP:
      case WM_RBUTTONUP:
      case WM_MBUTTONUP:
         handle_mouse_press(message, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
         return 0;

      case WM_CREATE:
         create_gl_context(hwnd);
         return 0;

      case WM_CLOSE:
      case WM_DESTROY:
      case WM_QUIT:
         g_quit = TRUE;
         return 0;

      case WM_SIZE:
         /* Do not send resize message if we minimize ... */
         if (wparam != SIZE_MAXHIDE && wparam != SIZE_MINIMIZED)
         {
            g_resize_width = LOWORD(lparam);
            g_resize_height = HIWORD(lparam);
            g_resized = TRUE;
         }
         return 0;
   }

   return DefWindowProc(hwnd, message, wparam, lparam);
}

static BOOL set_fullscreen(unsigned width, unsigned height)
{
   DEVMODE devmode;
   memset(&devmode, 0, sizeof(devmode));
   devmode.dmSize = sizeof(DEVMODE);
   devmode.dmPelsWidth = width;
   devmode.dmPelsHeight = height;
   devmode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;

   return ChangeDisplaySettings(&devmode, CDS_FULLSCREEN) == DISP_CHANGE_SUCCESSFUL;
}

struct sgl_resolution *sgl_get_desktop_modes(unsigned *num_modes)
{
   RECT rect;
   struct sgl_resolution *sgl_modes = (struct sgl_resolution*)calloc(1, sizeof(*sgl_modes));
   if (!sgl_modes)
      return NULL;

   *num_modes = 1;

   GetClientRect(GetDesktopWindow(), &rect);
   sgl_modes[0].width = rect.right - rect.left;
   sgl_modes[0].height = rect.bottom - rect.top;
   return sgl_modes;
}

int sgl_init(const struct sgl_context_options *opts)
{
   unsigned width, height;
   DWORD style;
   RECT rect;
   WNDCLASSEXA wndclass = {0};

   if (g_inited)
      return SGL_ERROR;

   g_quit = FALSE;
   g_resized = FALSE;

   g_ctx_modern = opts->context.style == SGL_CONTEXT_MODERN;
   g_gl_major = opts->context.major;
   g_gl_minor = opts->context.minor;
   g_samples = opts->samples == 0 ? 1 : opts->samples;

   setup_dummy_window();

   wndclass.cbSize = sizeof(wndclass);
   wndclass.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
   wndclass.lpfnWndProc = WndProc;
   wndclass.hInstance = GetModuleHandle(NULL);
   wndclass.hCursor = LoadCursor(NULL, IDC_ARROW);
   wndclass.lpszClassName = "SGL Window";


   if (!RegisterClassExA(&wndclass))
      return SGL_ERROR;

   width = opts->res.width;
   height = opts->res.height;
   style = 0;
   GetClientRect(GetDesktopWindow(), &rect);

   switch (opts->screen_type)
   {
      case SGL_SCREEN_WINDOWED:
      {
         RECT rect = {0};
         rect.right = width;
         rect.bottom = height;

         style = WS_OVERLAPPEDWINDOW;
         AdjustWindowRect(&rect, style, FALSE);
         width = rect.right - rect.left;
         height = rect.bottom - rect.top;
         break;
      }

      case SGL_SCREEN_FULLSCREEN:
         g_fullscreen = TRUE;
         style = WS_POPUP | WS_VISIBLE;

         /* Recover, just use windowed fullscreen instead. */
         if (!set_fullscreen(width, height))
         {
            g_fullscreen = FALSE;
            width = rect.right - rect.left;
            height = rect.bottom - rect.top;
         }
         break;

      case SGL_SCREEN_WINDOWED_FULLSCREEN:
         style = WS_POPUP | WS_VISIBLE;
         width = rect.right - rect.left;
         height = rect.bottom - rect.top;
         break;

      default:
         UnregisterClassA("SGL Window", GetModuleHandle(NULL));
         return FALSE;
   }

   g_hwnd = CreateWindowExA(0, "SGL Window", opts->title ? opts->title : "SGL Window",
         style,
         CW_USEDEFAULT, CW_USEDEFAULT, width, height,
         NULL, NULL, NULL, NULL);

   if (!g_hwnd)
   {
      UnregisterClassA("SGL Window", GetModuleHandle(NULL));
      return SGL_ERROR;
   }

   if (opts->screen_type == SGL_SCREEN_WINDOWED)
   {
      ShowWindow(g_hwnd, SW_RESTORE);
      UpdateWindow(g_hwnd);
      SetForegroundWindow(g_hwnd);
      SetFocus(g_hwnd);
   }

   sgl_set_swap_interval(opts->swap_interval);

   g_inited = TRUE;
   return SGL_OK;
}

void sgl_deinit(void)
{
   g_inited = FALSE;

   if (g_quit)
   {
      wglMakeCurrent(NULL, NULL);
      wglDeleteContext(g_hrc);
   }

   DestroyWindow(g_hwnd);
   UnregisterClassA("SGL Window", GetModuleHandle(NULL));

   if (g_fullscreen)
      ChangeDisplaySettings(NULL, 0);
   g_fullscreen = FALSE;
}

void sgl_set_window_title(const char *title)
{
   SetWindowTextA(g_hwnd, title);
}

int sgl_check_resize(unsigned *width, unsigned *height)
{
   if (g_resized)
   {
      *width = g_resize_width;
      *height = g_resize_height;
      g_resized = FALSE;
      return SGL_TRUE;
   }
   else
      return SGL_FALSE;
}

void sgl_set_swap_interval(unsigned interval)
{
   static BOOL (APIENTRY *swap_interval)(int) = NULL;
   if (!swap_interval)
      swap_interval = (BOOL (APIENTRY *)(int))sgl_get_proc_address("wglSwapIntervalEXT");

   if (swap_interval)
      swap_interval(interval);
}

void sgl_swap_buffers(void)
{
   SwapBuffers(g_hdc);
}

int sgl_has_focus(void)
{
   return GetFocus() == g_hwnd;
}

int sgl_is_alive(void)
{
   int old_x = g_mouse_last_x;
   int old_y = g_mouse_last_y;

   MSG msg;
   while (PeekMessage(&msg, g_hwnd, 0, 0, PM_REMOVE))
   {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
   }

   if (g_mouse_relative && g_input_cbs.mouse_move_cb)
   {
      POINT p;
      GetCursorPos(&p);

      if (!g_mouse_delta_invalid)
      {
         int delta_x = p.x - old_x;
         int delta_y = p.y - old_y;
         if (delta_x || delta_y)
            g_input_cbs.mouse_move_cb(delta_x, delta_y);
      }
      else
         g_mouse_delta_invalid = FALSE;

      if (g_mouse_grabbed)
      {
         RECT rect;
         GetWindowRect(g_hwnd, &rect);
         SetCursorPos((rect.left + rect.right) / 2, (rect.bottom + rect.top) / 2);
      }

      GetCursorPos(&p);
      g_mouse_last_x = p.x;
      g_mouse_last_y = p.y;
   }

   return !g_quit;
}

sgl_function_t sgl_get_proc_address(const char *sym)
{
   return (sgl_function_t)wglGetProcAddress(sym);
}

void sgl_get_handles(struct sgl_handles *handles)
{
   handles->hwnd = g_hwnd;
   handles->hglrc = g_hrc;
   handles->hdc = g_hdc;
}

void sgl_set_input_callbacks(const struct sgl_input_callbacks *cbs)
{
   g_input_cbs = *cbs;
}

void sgl_set_mouse_mode(int capture, int relative, int visible)
{
   static BOOL mouse_hidden = FALSE;
   if (!visible && !mouse_hidden)
   {
      ShowCursor(FALSE);
      mouse_hidden = TRUE;
   }
   else if (visible && mouse_hidden)
   {
      ShowCursor(TRUE);
      mouse_hidden = FALSE;
   }

   g_mouse_relative = relative;

   g_mouse_delta_invalid = TRUE;
   if (capture && !g_mouse_grabbed)
   {
      RECT rect;
      POINT p;

      GetWindowRect(g_hwnd, &rect);
      SetCapture(g_hwnd);
      ClipCursor(&rect);
      SetCursorPos((rect.left + rect.right) / 2, (rect.bottom + rect.top) / 2);
      GetCursorPos(&p);
      g_mouse_last_x = p.x;
      g_mouse_last_y = p.y;
      g_mouse_grabbed = TRUE;
   }
   else if (!capture && g_mouse_grabbed)
   {
      ClipCursor(NULL);
      ReleaseCapture();
      g_mouse_grabbed = FALSE;
   }
}

struct key_map
{
   int win;
   int sglk;
};

static const struct key_map bind_map[] = {
   { VK_ESCAPE, SGLK_ESCAPE },
   { VK_UP, SGLK_UP },
   { VK_DOWN, SGLK_DOWN },
   { VK_LEFT, SGLK_LEFT },
   { VK_RIGHT, SGLK_RIGHT },
   { VK_SPACE, SGLK_SPACE },
   { 'A', SGLK_a },
   { 'B', SGLK_b },
   { 'C', SGLK_c },
   { 'D', SGLK_d },
   { 'E', SGLK_e },
   { 'F', SGLK_f },
   { 'G', SGLK_g },
   { 'H', SGLK_h },
   { 'I', SGLK_i },
   { 'J', SGLK_j },
   { 'K', SGLK_k },
   { 'L', SGLK_l },
   { 'M', SGLK_m },
   { 'N', SGLK_n },
   { 'O', SGLK_o },
   { 'P', SGLK_p },
   { 'Q', SGLK_q },
   { 'R', SGLK_r },
   { 'S', SGLK_s },
   { 'T', SGLK_t },
   { 'U', SGLK_u },
   { 'V', SGLK_v },
   { 'W', SGLK_w },
   { 'X', SGLK_x },
   { 'Y', SGLK_y },
   { 'Z', SGLK_z },
};

static void handle_key_press(WPARAM key, int pressed)
{
   size_t i;
   if (!g_input_cbs.key_cb)
      return;

   for (i = 0; i < sizeof(bind_map) / sizeof(bind_map[0]); i++)
   {
      if (bind_map[i].win == key)
      {
         g_input_cbs.key_cb(bind_map[i].sglk, pressed);
         return;
      }
   }
}

static void handle_mouse_move(int x, int y)
{
   if (!g_input_cbs.mouse_move_cb)
      return;

   if (!g_mouse_relative)
      g_input_cbs.mouse_move_cb(x, y);
}

static void handle_mouse_press(UINT message, int x, int y)
{
   int pressed, button;
   if (!g_input_cbs.mouse_button_cb)
      return;

   switch (message)
   {
      case WM_LBUTTONDOWN:
      case WM_RBUTTONDOWN:
      case WM_MBUTTONDOWN:
         pressed = SGL_TRUE;
         break;

      case WM_LBUTTONUP:
      case WM_RBUTTONUP:
      case WM_MBUTTONUP:
         pressed = SGL_FALSE;

      default:
         pressed = SGL_FALSE;
   }

   switch (message)
   {
      case WM_LBUTTONDOWN:
      case WM_LBUTTONUP:
         button = 1;
         break;

      case WM_MBUTTONDOWN:
      case WM_MBUTTONUP:
         button = 2;
         break;

      case WM_RBUTTONDOWN:
      case WM_RBUTTONUP:
         button = 3;
         break;

      default:
         button = 0;
   }

   g_input_cbs.mouse_button_cb(button, pressed, x, y);
}


