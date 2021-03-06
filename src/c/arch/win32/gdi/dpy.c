/* © 2010 David Given.
 * WordGrinder is licensed under the MIT open source license. See the COPYING
 * file in this distribution for the full text.
 *
 * $Id: dpy.c 159 2009-12-13 13:11:03Z dtrg $
 * $URL: https://wordgrinder.svn.sf.net/svnroot/wordgrinder/wordgrinder/src/c/arch/win32/console/dpy.c $
 */

#include "globals.h"
#include <string.h>
#include <windows.h>
#include <ctype.h>
#include "gdi.h"

#define CTRL_PRESSED (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)

#define DEFAULT_CHAR (' ' << 8)

#define MENUITEM_SETFONT 1
#define MENUITEM_FULLSCREEN 2

#define REGISTRY_PATH "Software\\Cowlark Technologies\\WordGrinder"

HWND window = INVALID_HANDLE_VALUE;
static LOGFONT fontlf;
static unsigned int* frontbuffer = NULL;
static unsigned int* backbuffer = NULL;
static int screenwidth = 0;
static int screenheight = 0;
static int defaultattr = 0;

static int cursorx = 0;
static int cursory = 0;

static bool isfullscreen = false;
static bool window_geometry_valid = false;
static RECT window_geometry;
static bool window_created = false;

static void resize_buffer(void);
static void fullscreen_cb(void);
static void switch_to_full_screen(void);
static void switch_to_windowed(void);

static HKEY make_key(const char* keystring)
{
	HKEY key;

	int e = RegCreateKeyEx(HKEY_CURRENT_USER, keystring,
			0, NULL, REG_OPTION_NON_VOLATILE,
			KEY_ALL_ACCESS, NULL, &key,
			NULL);
	if (e == ERROR_SUCCESS)
		return key;
	return NULL;
}

static void read_default_font(void)
{
	HKEY key = make_key(REGISTRY_PATH);
	DWORD size = sizeof(fontlf);
	DWORD e = RegQueryValueEx(key, "DefaultFont",
			NULL, NULL,
			(LPBYTE) &fontlf, &size);
	RegCloseKey(key);

	if (e != ERROR_SUCCESS)
	{
		HFONT defaultfont = (HFONT) GetStockObject(SYSTEM_FIXED_FONT);
		GetObject(defaultfont, sizeof(fontlf), &fontlf);
	}
}

static void write_default_font(void)
{
	HKEY key = make_key(REGISTRY_PATH);
	RegSetValueEx(key, "DefaultFont", 0,
			REG_BINARY,
			(LPBYTE) &fontlf, sizeof(fontlf));
	RegCloseKey(key);
}

static void read_window_geometry(void)
{
	HKEY key = make_key(REGISTRY_PATH);
	DWORD size = sizeof(window_geometry);
	DWORD e = RegQueryValueEx(key, "WindowGeometry",
			NULL, NULL,
			(LPBYTE) &window_geometry, &size);
	RegCloseKey(key);

	if (e != ERROR_SUCCESS)
		window_geometry_valid = false;
	else
		window_geometry_valid = true;
}

static void write_window_geometry(void)
{
	HKEY key = make_key(REGISTRY_PATH);

	if (window_geometry_valid)
		RegSetValueEx(key, "WindowGeometry", 0,
			REG_BINARY,
			(LPBYTE) &window_geometry, sizeof(window_geometry));
	else
		RegDeleteKey(key, "WindowGeometry");

	RegCloseKey(key);
}

static void unicode_key(uni_t key, unsigned flags)
{
	if ((key >= 1) && (key <= 31))
	{
		dpy_queuekey(-(VKM_CTRLASCII | key));
		return;
	}
	if ((key == ' ') && (GetKeyState(VK_CONTROL) & 0x8000))
	{
		dpy_queuekey(-(VKM_CTRLASCII | 0));
		return;
	}

	dpy_queuekey(key);
}

static bool special_key(int vk, unsigned flags)
{
	switch (vk)
	{
		case VK_RETURN:
			if (flags & (1<<29))
			{
				fullscreen_cb();
				return true;
			}
			break;

		case VK_SHIFT:
		case VK_CONTROL:
		case VK_CAPITAL:
		case VK_MENU:
		case VK_LWIN:
		case VK_RWIN:
		case VK_SNAPSHOT:
		case VK_PAUSE:
			return false;
	}

	if (vk > 0x90)
		return false;

	/* Numeric keypad. */
	if ((vk >= 0x60) && (vk <= 0x6f))
		return false;

	if ((vk == ' ') || isdigit(vk) || isupper(vk))
	{
		if (flags & (1<<29))
		{
			/* ALT pressed */
			dpy_queuekey(-27);
			dpy_queuekey(vk);
			return true;
		}

		return false;
	}

	if (GetKeyState(VK_CONTROL) & 0x8000)
		vk |= VKM_CTRL;
	if (GetKeyState(VK_SHIFT) & 0x8000)
		vk |= VKM_SHIFT;
		
	dpy_queuekey(-vk);
	return true;
}

static void paint_cb(HWND window, PAINTSTRUCT* ps, HDC dc)
{
	int textwidth, textheight;
	glyphcache_getfontsize(&textwidth, &textheight);

	int x1 = ps->rcPaint.left / textwidth;
	x1 -= 1; /* because of overlapping characters */
	if (x1 < 0)
		x1 = 0;

	int y1 = ps->rcPaint.top/textheight;
	int x2 = ps->rcPaint.right/textwidth;
	x2 += 1; /* because of overlapping characters */
	if (x2 >= screenwidth)
	{
		RECT r = {screenwidth*textwidth, 0, ps->rcPaint.right, ps->rcPaint.bottom};
		FillRect(dc, &r, GetStockObject(BLACK_BRUSH));
		x2 = screenwidth;
	}

	int y2 = ps->rcPaint.bottom / textheight;
	if (y2 >= screenheight)
	{
		RECT r = {0, screenheight*textheight, ps->rcPaint.right, ps->rcPaint.bottom};
		FillRect(dc, &r, GetStockObject(BLACK_BRUSH));
		y2 = screenheight-1;
	}

	int state = SaveDC(dc);

	HPEN brightpen = CreatePen(PS_SOLID, 0, 0xffffff);
	HPEN normalpen = CreatePen(PS_SOLID, 0, 0x888888);
	HPEN dimpen = CreatePen(PS_SOLID, 0, 0x555555);

	for (int y = y1; y <= y2; y++)
	{
		int sy = y * textheight;

		/* Clear this line (or at least the part of it we're drawing). */

		RECT r = {ps->rcPaint.left, sy, ps->rcPaint.right, sy+textheight};
		FillRect(dc, &r, GetStockObject(BLACK_BRUSH));

		/* Draw the actual text. */

		for (int x = x1; x < x2; x++)
		{
			int seq = y*screenwidth + x;
			int sx = x * textwidth;

			unsigned int id = frontbuffer[seq];
			struct glyph* glyph = glyphcache_getglyph(id, dc);
			if (glyph)
			{
				BitBlt(dc, sx+glyph->xoffset, sy+glyph->yoffset,
					glyph->realwidth, glyph->realheight,
					glyph->dc, 0, 0, SRCPAINT);

				if (id & DPY_UNDERLINE)
				{
					if (id & DPY_BRIGHT)
						SelectObject(dc, brightpen);
					else if (id & DPY_DIM)
						SelectObject(dc, dimpen);
					else
						SelectObject(dc, normalpen);

					MoveToEx(dc, sx, sy+textheight-1, NULL);
					LineTo(dc, sx+glyph->width, sy+textheight-1);
				}
			}
		}

		/* Now go through and invert any characters which are in reverse. */

		for (int x = x1; x < x2; x++)
		{
			int seq = y*screenwidth + x;
			int sx = x * textwidth;

			unsigned int id = frontbuffer[seq];
			if (id & DPY_REVERSE)
			{
				int w;
				struct glyph* glyph = glyphcache_getglyph(id, dc);
				if (glyph)
					w = glyph->width;
				else
					w = textwidth;

				BitBlt(dc, sx, sy, w, textheight, NULL, 0, 0, DSTINVERT);
			}
		}
	}

	/* Draw the cursor caret. */

	{
		int x = cursorx*textwidth;
		int y = cursory*textheight;

		SelectObject(dc, brightpen);
		MoveToEx(dc, x, y, NULL);
		LineTo(dc, x, y+textheight);
		SetPixelV(dc, x-1, y-1, 0xffffff);
		SetPixelV(dc, x+1, y-1, 0xffffff);
		SetPixelV(dc, x-1, y+textheight, 0xffffff);
		SetPixelV(dc, x+1, y+textheight, 0xffffff);
	}


	DeleteObject(brightpen);
	DeleteObject(normalpen);
	DeleteObject(dimpen);
	RestoreDC(dc, state);
}

static void setfont_cb(void)
{
	CHOOSEFONT cf;
	memset(&cf, 0, sizeof(cf));
	cf.lStructSize = sizeof(cf);
	cf.hwndOwner = window;
	cf.lpLogFont = &fontlf;
	cf.Flags = CF_INITTOLOGFONTSTRUCT | CF_FIXEDPITCHONLY | CF_SCREENFONTS;
	cf.nFontType = SCREEN_FONTTYPE;

	if (ChooseFont(&cf))
	{
		write_default_font();

		HDC dc = GetDC(window);
		glyphcache_deinit();
		glyphcache_init(dc, &fontlf);
		ReleaseDC(window, dc);

		resize_buffer();
	}
}

static void fullscreen_cb(void)
{
	if (!isfullscreen)
	{
		window_geometry_valid = true;
		GetWindowRect(window, &window_geometry);
		write_window_geometry();
	}

	isfullscreen = !isfullscreen;
	if (isfullscreen)
		switch_to_full_screen();
	else
		switch_to_windowed();

	resize_buffer();
}

static void create_cb(void)
{
	/* Initialise the glyph cache. */

	read_default_font();

	HDC dc = GetDC(window);
	glyphcache_init(dc, &fontlf);
	ReleaseDC(window, dc);
}

static void sizing_cb(int type, RECT* r)
{
	int w = r->right - r->left;
	if (w < 100)
		w = 100;
	int h = r->bottom - r->top;
	if (h < 100)
		h = 100;

	switch (type)
	{
		case WMSZ_LEFT:
		case WMSZ_TOPLEFT:
		case WMSZ_BOTTOMLEFT:
			r->left = r->right - w;
			break;

		case WMSZ_RIGHT:
		case WMSZ_TOPRIGHT:
		case WMSZ_BOTTOMRIGHT:
			r->right = r->left + w;
			break;
	}

	switch (type)
	{
		case WMSZ_TOP:
		case WMSZ_TOPLEFT:
		case WMSZ_TOPRIGHT:
			r->top = r->bottom - h;
			break;

		case WMSZ_BOTTOM:
		case WMSZ_BOTTOMLEFT:
		case WMSZ_BOTTOMRIGHT:
			r->bottom = r->top + h;
			break;
	}
}

static LRESULT CALLBACK window_cb(HWND window, UINT message,
		WPARAM wparam, LPARAM lparam)
{
	dpy_flushkeys();

	switch (message)
	{
		case WM_CLOSE:
			return 0;

		case WM_EXITSIZEMOVE:
			if (!isfullscreen)
			{
				window_geometry_valid = true;
				GetWindowRect(window, &window_geometry);
				write_window_geometry();
			}
			break;

		case WM_SIZE:
			if (!window_created)
			{
				create_cb();
				window_created = true;
			}
			resize_buffer();
			break;

		case WM_SIZING:
			sizing_cb(wparam, (RECT*) lparam);
			goto delegate;

		case WM_ERASEBKGND:
		    return 1;

		case WM_PAINT:
		{
			PAINTSTRUCT ps;
			BeginPaint(window, &ps);

			paint_cb(window, &ps, ps.hdc);

			EndPaint(window, &ps);
			break;
		}

		case WM_PRINTCLIENT:
		{
			PAINTSTRUCT ps;
			ps.hdc = (HDC) wparam;
			GetClientRect(window, &ps.rcPaint);
			paint_cb(window, &ps, ps.hdc);
			break;
		}

		case WM_CHAR:
		{
			unicode_key(wparam, lparam);
			break;
		}

		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
		{
			if (special_key(wparam, lparam))
				return 1;
			break;
		}

		case WM_SYSCOMMAND:
		{
			switch (wparam)
			{
				case MENUITEM_SETFONT:
					setfont_cb();
					break;

				case MENUITEM_FULLSCREEN:
					fullscreen_cb();
					break;
			}

			goto delegate;
		}

		case WM_TIMER:
		{
			if (wparam == TIMEOUT_TIMER_ID)
			{
				dpy_queuekey(-VK_TIMEOUT);
				break;
			}
			goto delegate;
		}
				
		delegate:
		default:
			return DefWindowProcW(window, message, wparam, lparam);
	}

	return 0;
}

void dpy_init(const char* argv[])
{
	SystemParametersInfo(SPI_SETFONTSMOOTHING,
			 TRUE, 0,
			 SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
	SystemParametersInfo(SPI_SETFONTSMOOTHINGTYPE,
			 0, (PVOID)FE_FONTSMOOTHINGCLEARTYPE,
			 SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);

	read_window_geometry();
}

static void resize_buffer(void)
{
	RECT rect;
	int e = GetClientRect(window, &rect);
	if (!e)
		SystemParametersInfo(SPI_GETWORKAREA, sizeof(RECT), &rect, 0);

	int textwidth, textheight;
	glyphcache_getfontsize(&textwidth, &textheight);

	int w = rect.right / textwidth;
	int h = rect.bottom / textheight;

	if ((w != screenwidth) || (h != screenheight))
	{
		glyphcache_flush();

		/* Wipe the character storage. */

		screenwidth = w;
		screenheight = h;

		frontbuffer = realloc(frontbuffer, sizeof(unsigned int) * w * h);
		backbuffer = realloc(backbuffer, sizeof(unsigned int) * w * h);

		for (int p = 0; p < (w * h); p++)
		{
			frontbuffer[p] = 0;
			backbuffer[p] = DEFAULT_CHAR;
		}

		/* Tell the main app that the screen has changed size; it'll
		 * redraw the character storage. */

		dpy_queuekey(-VK_RESIZE);
	}

	/* The front end will redraw the content area, if necessary, but it
	 * doesn't know anything about the screen borders. We need to force
	 * them to be redrawn as well. */

	RECT r;
	r = rect;
	r.left = w * textwidth;
	InvalidateRect(window, &r, 0);

	r = rect;
	r.top = h * textheight;
	InvalidateRect(window, &r, 0);
}

static void switch_to_full_screen(void)
{
	HMONITOR monitor;

	if (window)
	{
		monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
		DestroyWindow(window);
	}
	else
		monitor = MonitorFromWindow(HWND_DESKTOP, MONITOR_DEFAULTTONEAREST);

	MONITORINFO mi;
	mi.cbSize = sizeof(mi);
	GetMonitorInfo(monitor, &mi);

	window = CreateWindowExW(
		WS_EX_TOPMOST,                  /* Extended class style */
		L"WordGrinder",                 /* Class Name */
		L"WordGrinder",                 /* Title */
		WS_POPUP,                       /* Style */
		mi.rcMonitor.left,              /* x */
		mi.rcMonitor.top,               /* y */
		mi.rcMonitor.right - mi.rcMonitor.left, /* width */
		mi.rcMonitor.bottom - mi.rcMonitor.top, /* height */
		HWND_DESKTOP,                   /* Parent */
		NULL,                           /* No menu */
		GetModuleHandle(NULL),          /* Instance */
		0);                             /* No special parameters */

	ShowWindow(window, SW_SHOWDEFAULT);
}

/* Actually invalidates a 3x3 square around the character, to deal with
 * overdraw. */
static void invalidate_character_at(int x, int y)
{
	int textwidth, textheight;
	glyphcache_getfontsize(&textwidth, &textheight);

	RECT r;
	r.left = cursorx * textwidth - 1;
	r.top = cursory * textheight - 1;
	r.right = r.left + textwidth + 2;
	r.bottom = r.top + textheight + 2;
	InvalidateRect(window, &r, 0);
}

static void switch_to_windowed(void)
{
	if (window)
		DestroyWindow(window);

	/* Create the window. */

	window = CreateWindowW(
		L"WordGrinder",                 /* Class Name */
		L"WordGrinder",                 /* Title */
		WS_OVERLAPPEDWINDOW,            /* Style */
		CW_USEDEFAULT, CW_USEDEFAULT,   /* Position */
		CW_USEDEFAULT, CW_USEDEFAULT,   /* Size */
		NULL,                           /* Parent */
		NULL,                           /* No menu */
		GetModuleHandle(NULL),          /* Instance */
		0);                             /* No special parameters */

	if (window_geometry_valid)
		SetWindowPos(window, HWND_TOP,
			window_geometry.left,
			window_geometry.top,
			window_geometry.right - window_geometry.left,
			window_geometry.bottom - window_geometry.top,
			SWP_NOZORDER);

	/* Add the window menu commands and disable the close button. */

	{
		HMENU menu = GetSystemMenu(window, FALSE);
		int count = GetMenuItemCount(menu);

		MENUITEMINFO mii;
		mii.cbSize = sizeof(mii);

		mii.fMask = MIIM_FTYPE;
		mii.fType = MFT_SEPARATOR;
		InsertMenuItem(menu, count++, TRUE, &mii);

		mii.fMask = MIIM_FTYPE | MIIM_STRING | MIIM_ID;
		mii.fType = MFT_STRING;
		mii.dwTypeData = "Select display fon&t...";
		mii.cch = strlen(mii.dwTypeData);
		mii.wID = MENUITEM_SETFONT;
		InsertMenuItem(menu, count++, TRUE, &mii);

		mii.fMask = MIIM_FTYPE | MIIM_STRING | MIIM_ID;
		mii.fType = MFT_STRING;
		mii.dwTypeData = "&Fullscreen mode\tAlt+Enter";
		mii.cch = strlen(mii.dwTypeData);
		mii.wID = MENUITEM_FULLSCREEN;
		InsertMenuItem(menu, count++, TRUE, &mii);

		EnableMenuItem(menu, SC_CLOSE,
			MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
	}

	ShowWindow(window, SW_SHOWDEFAULT);
}

void dpy_start(void)
{
	/* Register our window class. */

	{
		WNDCLASSW wc;
		wc.style = CS_OWNDC;
		wc.lpfnWndProc = window_cb;
		wc.cbClsExtra = 0;
		wc.cbWndExtra = 0;
		wc.hInstance = GetModuleHandle(NULL);
		wc.hIcon = LoadIcon(wc.hInstance, MAKEINTRESOURCE(101));
		wc.hCursor = LoadCursor(NULL, IDC_ARROW);
		wc.hbrBackground = NULL;
		wc.lpszMenuName = NULL;
		wc.lpszClassName = L"WordGrinder";

		if (!RegisterClassW(&wc))
		{
			fprintf(stderr, "Unable to register window class.\n");
			exit(-1);
		}
	}

	switch_to_windowed();
}

void dpy_shutdown(void)
{
	free(frontbuffer);
	free(backbuffer);
	glyphcache_deinit();
}

void dpy_clearscreen(void)
{
	dpy_cleararea(0, 0, screenwidth-1, screenheight-1);
}

void dpy_getscreensize(int* x, int* y)
{
	*x = screenwidth;
	*y = screenheight;
}

void dpy_sync(void)
{
	int textwidth, textheight;
	glyphcache_getfontsize(&textwidth, &textheight);

	for (int y=0; y<screenheight; y++)
	{
		unsigned int* front = frontbuffer + y*screenwidth;
		unsigned int* back = backbuffer + y*screenwidth;
		if (memcmp(front, back, screenwidth * sizeof(*backbuffer)) != 0)
		{
			memcpy(front, back, screenwidth * sizeof(*backbuffer));

			int sy = y*textheight;
			RECT r = {0, sy, screenwidth*textwidth, sy+textheight};
			InvalidateRect(window, &r, 0);
		}
	}

	invalidate_character_at(cursorx, cursory);
	UpdateWindow(window);
}

void dpy_setcursor(int x, int y)
{
	invalidate_character_at(cursorx, cursory);
	invalidate_character_at(x, y);

	cursorx = x;
	cursory = y;

	UpdateWindow(window);
}

void dpy_setattr(int andmask, int ormask)
{
	defaultattr &= andmask;
	defaultattr |= ormask;
}

void dpy_writechar(int x, int y, uni_t c)
{
	if ((x < 0) || (y < 0) || (x >= screenwidth) || (y >= screenheight))
		return;

	backbuffer[y*screenwidth + x] = (c<<8) | defaultattr;
}

void dpy_cleararea(int x1, int y1, int x2, int y2)
{
	for (int y = y1; y <= y2; y++)
		for (int x = x1; x <= x2; x++)
			backbuffer[y*screenwidth + x] = (' '<<8) | defaultattr;
}

const char* dpy_getkeyname(uni_t k)
{
	switch (-k)
	{
		case VK_RESIZE:      return "KEY_RESIZE";
		case VK_TIMEOUT:     return "KEY_TIMEOUT";
		case VK_REDRAW:      return "KEY_REDRAW";
	}

	int mods = -k;
	int key = (-k & 0xFF);
	static char buffer[32];

	if (mods & VKM_CTRLASCII)
	{
		sprintf(buffer, "KEY_%s^%c",
				(mods & VKM_SHIFT) ? "S" : "",
				key + 64);
		return buffer;
	}

	const char* template = NULL;
	switch (key)
	{
		case VK_NUMLOCK:     return NULL;

		case VK_DOWN:        template = "DOWN"; break;
		case VK_UP:          template = "UP"; break;
		case VK_LEFT:        template = "LEFT"; break;
		case VK_RIGHT:       template = "RIGHT"; break;
		case VK_HOME:        template = "HOME"; break;
		case VK_END:         template = "END"; break;
		case VK_BACK:        template = "BACKSPACE"; break;
		case VK_DELETE:      template = "DELETE"; break;
		case VK_INSERT:      template = "INSERT"; break;
		case VK_NEXT:        template = "PGDN"; break;
		case VK_PRIOR:       template = "PGUP"; break;
		case VK_TAB:         template = "TAB"; break;
		case VK_RETURN:      template = "RETURN"; break;
		case VK_ESCAPE:      template = "ESCAPE"; break;
	}

	if (template)
	{
		sprintf(buffer, "KEY_%s%s%s",
				(mods & VKM_SHIFT) ? "S" : "",
				(mods & VKM_CTRL) ? "^" : "",
				template);
		return buffer;
	}

	if ((key >= VK_F1) && (key <= (VK_F24)))
	{
		sprintf(buffer, "KEY_%s%sF%d",
				(mods & VKM_SHIFT) ? "S" : "",
				(mods & VKM_CTRL) ? "^" : "",
				key - VK_F1 + 1);
		return buffer;
	}

	sprintf(buffer, "KEY_UNKNOWN_%d", -k);
	return buffer;
}
