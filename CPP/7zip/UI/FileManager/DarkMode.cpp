// DarkMode.cpp
// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)

#include "StdAfx.h"

#ifndef UNDER_CE

#include <commctrl.h>
#include <uxtheme.h>
#include <vssym32.h>

#include "../../../Windows/Registry.h"
#include "../../../Windows/Control/Dialog.h"

#include "DarkMode.h"

using namespace NWindows;

namespace NDarkMode {

// ---------- colors (close to Explorer of Windows 11 in dark mode)

static const COLORREF k_Back     = RGB(0x20, 0x20, 0x20);  // windows, dialogs
static const COLORREF k_Ctrl     = RGB(0x2B, 0x2B, 0x2B);  // edit, combo, list box
static const COLORREF k_List     = RGB(0x19, 0x19, 0x19);  // file list
static const COLORREF k_Hot      = RGB(0x3D, 0x3D, 0x3D);  // hot / pressed item
static const COLORREF k_Edge     = RGB(0x58, 0x58, 0x58);  // lines, frames
static const COLORREF k_Text     = RGB(0xE8, 0xE8, 0xE8);
static const COLORREF k_TextDim  = RGB(0x8C, 0x8C, 0x8C);  // disabled text

static HBRUSH g_BackBrush;
static HBRUSH g_CtrlBrush;
static HBRUSH g_HotBrush;

static HBRUSH GetBrush(HBRUSH &b, COLORREF c)
{
  if (!b)
    b = ::CreateSolidBrush(c);
  return b;
}

#define BACK_BRUSH  GetBrush(g_BackBrush, k_Back)
#define CTRL_BRUSH  GetBrush(g_CtrlBrush, k_Ctrl)
#define HOT_BRUSH   GetBrush(g_HotBrush, k_Hot)

// ---------- run-time APIs

typedef HRESULT (WINAPI *Func_SetWindowTheme)(HWND, LPCWSTR, LPCWSTR);
typedef HTHEME  (WINAPI *Func_OpenThemeData)(HWND, LPCWSTR);
typedef HRESULT (WINAPI *Func_CloseThemeData)(HTHEME);
typedef HRESULT (WINAPI *Func_DrawThemeBackground)(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
typedef HRESULT (WINAPI *Func_GetThemePartSize)(HTHEME, HDC, int, int, LPCRECT, enum THEMESIZE, SIZE *);
typedef int     (WINAPI *Func_SetPreferredAppMode)(int mode);   // ordinal 135 (1903+)
typedef void    (WINAPI *Func_FlushMenuThemes)(void);           // ordinal 136
typedef BOOL    (WINAPI *Func_AllowDarkModeForWindow)(HWND, BOOL);  // ordinal 133
typedef HRESULT (WINAPI *Func_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
typedef void    (WINAPI *Func_RtlGetNtVersionNumbers)(DWORD *, DWORD *, DWORD *);

static Func_SetWindowTheme f_SetWindowTheme;
static Func_OpenThemeData f_OpenThemeData;
static Func_CloseThemeData f_CloseThemeData;
static Func_DrawThemeBackground f_DrawThemeBackground;
static Func_GetThemePartSize f_GetThemePartSize;
static Func_SetPreferredAppMode f_SetPreferredAppMode;
static Func_FlushMenuThemes f_FlushMenuThemes;
static Func_AllowDarkModeForWindow f_AllowDarkModeForWindow;
static Func_DwmSetWindowAttribute f_DwmSetWindowAttribute;

static bool g_Supported;
static DWORD g_Build;
static UInt32 g_Setting = k_Theme_System;
static bool g_Dark;

// ---------- setting

static LPCTSTR const k_RegPath = TEXT("Software") TEXT(STRING_PATH_SEPARATOR) TEXT("Heptazip");
static LPCTSTR const k_RegTheme = TEXT("Theme");

static bool SystemUsesDarkApps()
{
  NRegistry::CKey key;
  if (key.Open(HKEY_CURRENT_USER,
      TEXT("Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"), KEY_READ) != ERROR_SUCCESS)
    return false;
  UInt32 light = 1;
  key.GetValue_UInt32_IfOk(TEXT("AppsUseLightTheme"), light);
  return light == 0;
}

static bool HighContrast()
{
  HIGHCONTRASTW hc;
  memset(&hc, 0, sizeof(hc));
  hc.cbSize = sizeof(hc);
  if (::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0))
    return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
  return false;
}

static bool ComputeDark()
{
  if (!g_Supported || HighContrast())
    return false;
  if (g_Setting == k_Theme_Dark)
    return true;
  if (g_Setting == k_Theme_Light)
    return false;
  return SystemUsesDarkApps();
}

static void SetAppMode()
{
  if (!f_SetPreferredAppMode)
    return;
  // 2: ForceDark, 3: ForceLight (popup menus and the scroll bars of menus)
  f_SetPreferredAppMode(g_Dark ? 2 : 3);
  if (f_FlushMenuThemes)
    f_FlushMenuThemes();
}

static void DialogInitHook(HWND dialog);

void Init()
{
  {
    NRegistry::CKey key;
    if (key.Open(HKEY_CURRENT_USER, k_RegPath, KEY_READ) == ERROR_SUCCESS)
      key.GetValue_UInt32_IfOk(k_RegTheme, g_Setting);
    if (g_Setting > k_Theme_Dark)
      g_Setting = k_Theme_System;
  }

  const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
  if (ntdll)
  {
    const Func_RtlGetNtVersionNumbers f = Z7_GET_PROC_ADDRESS(
        Func_RtlGetNtVersionNumbers, ntdll, "RtlGetNtVersionNumbers");
    if (f)
    {
      DWORD major = 0, minor = 0, build = 0;
      f(&major, &minor, &build);
      if (major >= 10)
        g_Build = build & 0x0FFFFFFF;
    }
  }

  const HMODULE ux = ::LoadLibraryW(L"uxtheme.dll");
  if (ux)
  {
    f_SetWindowTheme = Z7_GET_PROC_ADDRESS(Func_SetWindowTheme, ux, "SetWindowTheme");
    f_OpenThemeData = Z7_GET_PROC_ADDRESS(Func_OpenThemeData, ux, "OpenThemeData");
    f_CloseThemeData = Z7_GET_PROC_ADDRESS(Func_CloseThemeData, ux, "CloseThemeData");
    f_DrawThemeBackground = Z7_GET_PROC_ADDRESS(Func_DrawThemeBackground, ux, "DrawThemeBackground");
    f_GetThemePartSize = Z7_GET_PROC_ADDRESS(Func_GetThemePartSize, ux, "GetThemePartSize");
    // the undocumented functions exist with these meanings since Windows 10 1903 (18362)
    if (g_Build >= 18362)
    {
      f_SetPreferredAppMode = Z7_GET_PROC_ADDRESS(Func_SetPreferredAppMode, ux, MAKEINTRESOURCEA(135));
      f_FlushMenuThemes = Z7_GET_PROC_ADDRESS(Func_FlushMenuThemes, ux, MAKEINTRESOURCEA(136));
      f_AllowDarkModeForWindow = Z7_GET_PROC_ADDRESS(Func_AllowDarkModeForWindow, ux, MAKEINTRESOURCEA(133));
    }
  }
  const HMODULE dwm = ::LoadLibraryW(L"dwmapi.dll");
  if (dwm)
    f_DwmSetWindowAttribute = Z7_GET_PROC_ADDRESS(Func_DwmSetWindowAttribute, dwm, "DwmSetWindowAttribute");

  g_Supported = g_Build >= 18362
      && f_SetWindowTheme && f_OpenThemeData && f_CloseThemeData
      && f_DrawThemeBackground && f_GetThemePartSize
      && f_SetPreferredAppMode && f_AllowDarkModeForWindow;

  g_Dark = ComputeDark();
  SetAppMode();
  NControl::g_DialogInitHook = DialogInitHook;
}

UInt32 GetSetting() { return g_Setting; }
bool IsDark() { return g_Dark; }

bool SetSetting(UInt32 setting)
{
  if (setting > k_Theme_Dark)
    setting = k_Theme_System;
  g_Setting = setting;
  {
    NRegistry::CKey key;
    if (key.Create(HKEY_CURRENT_USER, k_RegPath) == ERROR_SUCCESS)
      key.SetValue(k_RegTheme, setting);
  }
  const bool dark = ComputeDark();
  if (dark == g_Dark)
    return false;
  g_Dark = dark;
  SetAppMode();
  return true;
}

bool OnSettingChange(LPARAM lParam)
{
  if (lParam == 0)
    return false;
  const wchar_t *s = (const wchar_t *)lParam;
  if (::lstrcmpW(s, L"ImmersiveColorSet") != 0)
    return false;
  const bool dark = ComputeDark();
  if (dark == g_Dark)
    return false;
  g_Dark = dark;
  SetAppMode();
  return true;
}

// ---------- subclassing

enum
{
  k_Kind_Container = 1,   // dialogs, panels, frames: background and WM_CTLCOLOR*
  k_Kind_Main,            // main window: container + menu bar
  k_Kind_Host,            // toolbars, rebars, ComboBoxEx: WM_CTLCOLOR* of children
  k_Kind_ListView,        // text color of the header
  k_Kind_Button,          // check box, radio button, group box
  k_Kind_StatusBar,
  k_Kind_Tab
};

static const wchar_t * const k_PropProc = L"Heptazip.DarkProc";
static const wchar_t * const k_PropKind = L"Heptazip.DarkKind";

static LRESULT CALLBACK DarkWindowProc(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam);

static void Subclass(HWND wnd, int kind)
{
  if (::GetPropW(wnd, k_PropProc))
    return;
  const LONG_PTR old = ::SetWindowLongPtrW(wnd, GWLP_WNDPROC, (LONG_PTR)DarkWindowProc);
  if (!old)
    return;
  ::SetPropW(wnd, k_PropProc, (HANDLE)old);
  ::SetPropW(wnd, k_PropKind, (HANDLE)(INT_PTR)kind);
}

static void SetTheme(HWND wnd, LPCWSTR darkClass)
{
  if (f_SetWindowTheme)
    f_SetWindowTheme(wnd, g_Dark ? darkClass : NULL, NULL);
}

static void ThemeTooltips(HWND tooltip)
{
  if (tooltip)
    SetTheme(tooltip, L"DarkMode_Explorer");
}

static bool ClassIs(const wchar_t *name, const wchar_t *s)
{
  return ::lstrcmpiW(name, s) == 0;
}

static void ApplyToWindow(HWND wnd)
{
  wchar_t name[64];
  if (::GetClassNameW(wnd, name, 64) == 0)
    return;
  const LONG style = ::GetWindowLongW(wnd, GWL_STYLE);

  if (ClassIs(name, L"#32770"))
    Subclass(wnd, k_Kind_Container);
  else if (ClassIs(name, WC_LISTVIEWW))
  {
    Subclass(wnd, k_Kind_ListView);
    SetTheme(wnd, L"DarkMode_Explorer");
    ListView_SetBkColor(wnd, g_Dark ? k_List : ::GetSysColor(COLOR_WINDOW));
    ListView_SetTextBkColor(wnd, g_Dark ? k_List : ::GetSysColor(COLOR_WINDOW));
    ListView_SetTextColor(wnd, g_Dark ? k_Text : ::GetSysColor(COLOR_WINDOWTEXT));
    const HWND header = ListView_GetHeader(wnd);
    if (header)
      SetTheme(header, L"DarkMode_ItemsView");
    ThemeTooltips(ListView_GetToolTips(wnd));
  }
  else if (ClassIs(name, WC_TREEVIEWW))
  {
    SetTheme(wnd, L"DarkMode_Explorer");
    TreeView_SetBkColor(wnd, g_Dark ? k_List : (COLORREF)-1);
    TreeView_SetTextColor(wnd, g_Dark ? k_Text : (COLORREF)-1);
  }
  else if (ClassIs(name, WC_BUTTONW))
  {
    const LONG type = style & BS_TYPEMASK;
    if (type == BS_CHECKBOX || type == BS_AUTOCHECKBOX
        || type == BS_3STATE || type == BS_AUTO3STATE
        || type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON
        || type == BS_GROUPBOX)
      Subclass(wnd, k_Kind_Button);
    SetTheme(wnd, L"DarkMode_Explorer");
  }
  else if (ClassIs(name, WC_EDITW))
    SetTheme(wnd, L"DarkMode_CFD");
  else if (ClassIs(name, WC_COMBOBOXW))
  {
    SetTheme(wnd, L"DarkMode_CFD");
    COMBOBOXINFO ci;
    memset(&ci, 0, sizeof(ci));
    ci.cbSize = sizeof(ci);
    if (::GetComboBoxInfo(wnd, &ci) && ci.hwndList)
      SetTheme(ci.hwndList, L"DarkMode_Explorer");
  }
  else if (ClassIs(name, WC_COMBOBOXEXW))
    Subclass(wnd, k_Kind_Host);
  else if (ClassIs(name, WC_LISTBOXW) || ClassIs(name, WC_SCROLLBARW))
    SetTheme(wnd, L"DarkMode_Explorer");
  else if (ClassIs(name, TOOLBARCLASSNAMEW))
  {
    Subclass(wnd, k_Kind_Host);
    ThemeTooltips((HWND)::SendMessageW(wnd, TB_GETTOOLTIPS, 0, 0));
  }
  else if (ClassIs(name, REBARCLASSNAMEW))
    Subclass(wnd, k_Kind_Host);
  else if (ClassIs(name, STATUSCLASSNAMEW))
    Subclass(wnd, k_Kind_StatusBar);
  else if (ClassIs(name, WC_TABCONTROLW))
  {
    Subclass(wnd, k_Kind_Tab);
    ThemeTooltips(TabCtrl_GetToolTips(wnd));
  }
  else if ((style & WS_CHILD)
      && (HMODULE)::GetClassLongPtrW(wnd, GCLP_HMODULE) == ::GetModuleHandleW(NULL))
  {
    // the windows of the program (panel): background and the colors of the children
    Subclass(wnd, k_Kind_Container);
  }
}

static void SetTitleBar(HWND top)
{
  if (f_AllowDarkModeForWindow)
    f_AllowDarkModeForWindow(top, g_Dark ? TRUE : FALSE);
  if (f_DwmSetWindowAttribute && g_Build >= 17763)
  {
    const BOOL value = g_Dark ? TRUE : FALSE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE: 20 (19 before Windows 10 20H1)
    f_DwmSetWindowAttribute(top, (DWORD)(g_Build >= 18985 ? 20 : 19), &value, (DWORD)sizeof(value));
  }
}

static BOOL CALLBACK EnumChildProc(HWND wnd, LPARAM)
{
  ApplyToWindow(wnd);
  return TRUE;
}

void ApplyTree(HWND root)
{
  if (!root || !g_Supported)
    return;
  const HWND top = ::GetAncestor(root, GA_ROOT);
  if (top && top != root)
  {
    // a property page: the frame of the property sheet too
    ApplyToWindow(top);
    SetTitleBar(top);
    ::EnumChildWindows(top, EnumChildProc, 0);
    ::RedrawWindow(top, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
    return;
  }
  ApplyToWindow(root);
  if (!(::GetWindowLongW(root, GWL_STYLE) & WS_CHILD))
    SetTitleBar(root);
  ::EnumChildWindows(root, EnumChildProc, 0);
  ::RedrawWindow(root, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

void ApplyMainWindow(HWND mainWnd)
{
  if (!mainWnd || !g_Supported)
    return;
  Subclass(mainWnd, k_Kind_Main);
  ApplyTree(mainWnd);
  ::DrawMenuBar(mainWnd);
}

static void DialogInitHook(HWND dialog)
{
  ApplyTree(dialog);
}

// ---------- drawing

static LRESULT CtlColor(UINT msg, HDC dc)
{
  ::SetTextColor(dc, k_Text);
  if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX)
  {
    ::SetBkColor(dc, k_Ctrl);
    return (LRESULT)CTRL_BRUSH;
  }
  ::SetBkColor(dc, k_Back);
  return (LRESULT)BACK_BRUSH;
}

static bool IsCtlColor(UINT msg)
{
  return msg == WM_CTLCOLORDLG || msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLORBTN
      || msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX;
}

static void FillSolid(HDC dc, const RECT &rc, HBRUSH brush)
{
  ::FillRect(dc, &rc, brush);
}

static void FrameSolid(HDC dc, const RECT &rc, COLORREF color)
{
  const HBRUSH b = ::CreateSolidBrush(color);
  ::FrameRect(dc, &rc, b);
  ::DeleteObject(b);
}

static void Line(HDC dc, int x0, int y0, int x1, int y1, COLORREF color)
{
  const HPEN pen = ::CreatePen(PS_SOLID, 1, color);
  const HGDIOBJ old = ::SelectObject(dc, pen);
  ::MoveToEx(dc, x0, y0, NULL);
  ::LineTo(dc, x1, y1);
  ::SelectObject(dc, old);
  ::DeleteObject(pen);
}

static bool HideAccel(HWND wnd)
{
  return (::SendMessageW(wnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL) != 0;
}

// toolbar: our background, light text, hot item
static LRESULT ToolbarCustomDraw(NMTBCUSTOMDRAW *cd)
{
  switch (cd->nmcd.dwDrawStage)
  {
    case CDDS_PREPAINT:
      FillSolid(cd->nmcd.hdc, cd->nmcd.rc, BACK_BRUSH);
      return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
    {
      const UINT st = cd->nmcd.uItemState;
      if (st & (CDIS_HOT | CDIS_SELECTED | CDIS_CHECKED))
        FillSolid(cd->nmcd.hdc, cd->nmcd.rc, HOT_BRUSH);
      cd->clrText = (st & CDIS_DISABLED) ? k_TextDim : k_Text;
      cd->clrTextHighlight = k_Text;
      cd->nStringBkMode = TRANSPARENT;
      cd->nHLStringBkMode = TRANSPARENT;
      return TBCDRF_USECDCOLORS | TBCDRF_NOBACKGROUND | TBCDRF_NOEDGES | TBCDRF_NOOFFSET;
    }
    default:
      return CDRF_DODEFAULT;
  }
}

static bool HandleNotify(LPARAM lParam, LRESULT &res)
{
  const NMHDR *hdr = (const NMHDR *)lParam;
  if (!hdr || hdr->code != (UINT)NM_CUSTOMDRAW)
    return false;
  wchar_t name[64];
  if (::GetClassNameW(hdr->hwndFrom, name, 64) == 0)
    return false;
  if (ClassIs(name, TOOLBARCLASSNAMEW))
  {
    res = ToolbarCustomDraw((NMTBCUSTOMDRAW *)lParam);
    return true;
  }
  if (ClassIs(name, WC_HEADERW))
  {
    NMCUSTOMDRAW *cd = (NMCUSTOMDRAW *)lParam;
    if (cd->dwDrawStage == CDDS_PREPAINT)
    {
      res = CDRF_NOTIFYITEMDRAW;
      return true;
    }
    if (cd->dwDrawStage == CDDS_ITEMPREPAINT)
    {
      ::SetTextColor(cd->hdc, k_Text);
      res = CDRF_DODEFAULT;
      return true;
    }
  }
  return false;
}

static void PaintButton(HWND wnd, HDC dc)
{
  RECT rc;
  ::GetClientRect(wnd, &rc);
  const LONG style = ::GetWindowLongW(wnd, GWL_STYLE);
  const LONG type = style & BS_TYPEMASK;
  const bool enabled = ::IsWindowEnabled(wnd) != FALSE;

  wchar_t text[512];
  text[0] = 0;
  const int len = ::GetWindowTextW(wnd, text, 512);

  const HGDIOBJ oldFont = ::SelectObject(dc, (HGDIOBJ)::SendMessageW(wnd, WM_GETFONT, 0, 0));
  ::SetBkMode(dc, TRANSPARENT);
  ::SetTextColor(dc, enabled ? k_Text : k_TextDim);
  UINT dt = HideAccel(wnd) ? DT_HIDEPREFIX : 0;

  if (type == BS_GROUPBOX)
  {
    SIZE ts = { 0, 0 };
    ::GetTextExtentPoint32W(dc, text, len, &ts);
    const int y = rc.top + ts.cy / 2;
    const int x0 = rc.left + 6;
    const int gap = len > 0 ? ts.cx + 4 : 0;
    // the frame, with a gap for the text
    Line(dc, rc.left, y, x0 - 2, y, k_Edge);
    Line(dc, x0 + gap + 2, y, rc.right - 1, y, k_Edge);
    Line(dc, rc.right - 1, y, rc.right - 1, rc.bottom - 1, k_Edge);
    Line(dc, rc.right - 1, rc.bottom - 1, rc.left, rc.bottom - 1, k_Edge);
    Line(dc, rc.left, rc.bottom - 1, rc.left, y, k_Edge);
    if (len > 0)
    {
      RECT tr = { x0, rc.top, x0 + gap, rc.top + ts.cy };
      ::DrawTextW(dc, text, len, &tr, dt | DT_SINGLELINE | DT_CENTER | DT_NOCLIP);
    }
    ::SelectObject(dc, oldFont);
    return;
  }

  FillSolid(dc, rc, BACK_BRUSH);
  const bool radio = (type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON);
  const int part = radio ? BP_RADIOBUTTON : BP_CHECKBOX;
  const LRESULT check = ::SendMessageW(wnd, BM_GETCHECK, 0, 0);
  const LRESULT bstate = ::SendMessageW(wnd, BM_GETSTATE, 0, 0);
  int state = !enabled ? 4 : (bstate & BST_PUSHED) ? 3 : (bstate & BST_HOT) ? 2 : 1;
  if (check == BST_CHECKED)
    state += 4;
  else if (check == BST_INDETERMINATE && !radio)
    state += 8;

  SIZE gs = { 13, 13 };
  int textLeft = rc.left + 17;
  const HTHEME theme = f_OpenThemeData(wnd, L"Button");
  if (theme)
  {
    f_GetThemePartSize(theme, dc, part, state, NULL, TS_DRAW, &gs);
    RECT gr;
    gr.left = rc.left;
    gr.top = rc.top + (rc.bottom - rc.top - gs.cy) / 2;
    gr.right = gr.left + gs.cx;
    gr.bottom = gr.top + gs.cy;
    f_DrawThemeBackground(theme, dc, part, state, &gr, NULL);
    f_CloseThemeData(theme);
    // the space of Windows between the glyph and the text: 1/3 of the text height
    TEXTMETRICW tm;
    ::GetTextMetricsW(dc, &tm);
    textLeft = gr.right + tm.tmAveCharWidth / 2 + 1;
  }

  RECT tr = rc;
  tr.left = textLeft;
  if (style & BS_MULTILINE)
  {
    RECT calc = tr;
    ::DrawTextW(dc, text, len, &calc, dt | DT_WORDBREAK | DT_CALCRECT);
    const int h = calc.bottom - calc.top;
    tr.top = rc.top + (rc.bottom - rc.top - h) / 2;
    tr.bottom = tr.top + h;
    dt |= DT_WORDBREAK;
  }
  else
    dt |= DT_SINGLELINE | DT_VCENTER;
  ::DrawTextW(dc, text, len, &tr, dt);

  if (::GetFocus() == wnd && !(::SendMessageW(wnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) && len > 0)
  {
    RECT fr = tr;
    ::DrawTextW(dc, text, len, &fr, (dt & ~(UINT)DT_VCENTER) | DT_CALCRECT);
    const int h = fr.bottom - fr.top;
    if (!(style & BS_MULTILINE))
    {
      fr.top = rc.top + (rc.bottom - rc.top - h) / 2;
      fr.bottom = fr.top + h;
    }
    ::InflateRect(&fr, 1, 1);
    ::SetTextColor(dc, k_Text);
    ::DrawFocusRect(dc, &fr);
  }
  ::SelectObject(dc, oldFont);
}

static void PaintStatusBar(HWND wnd, HDC dc)
{
  RECT rc;
  ::GetClientRect(wnd, &rc);
  FillSolid(dc, rc, BACK_BRUSH);
  Line(dc, rc.left, rc.top, rc.right, rc.top, k_Edge);
  const HGDIOBJ oldFont = ::SelectObject(dc, (HGDIOBJ)::SendMessageW(wnd, WM_GETFONT, 0, 0));
  ::SetBkMode(dc, TRANSPARENT);
  ::SetTextColor(dc, k_Text);
  const int num = (int)::SendMessageW(wnd, SB_GETPARTS, 0, 0);
  for (int i = 0; i < num; i++)
  {
    RECT pr;
    if (!::SendMessageW(wnd, SB_GETRECT, (WPARAM)i, (LPARAM)&pr))
      continue;
    wchar_t text[512];
    const unsigned len = LOWORD(::SendMessageW(wnd, SB_GETTEXTLENGTHW, (WPARAM)i, 0));
    if (len > 0 && len < 512)
    {
      ::SendMessageW(wnd, SB_GETTEXTW, (WPARAM)i, (LPARAM)text);
      RECT tr = pr;
      tr.left += 6;
      tr.right -= 4;
      ::DrawTextW(dc, text, (int)len, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    if (i + 1 < num)
      Line(dc, pr.right - 1, pr.top + 3, pr.right - 1, pr.bottom - 3, k_Edge);
  }
  ::SelectObject(dc, oldFont);
}

static void PaintTab(HWND wnd, HDC dc)
{
  RECT rc;
  ::GetClientRect(wnd, &rc);
  FillSolid(dc, rc, BACK_BRUSH);
  const HGDIOBJ oldFont = ::SelectObject(dc, (HGDIOBJ)::SendMessageW(wnd, WM_GETFONT, 0, 0));
  ::SetBkMode(dc, TRANSPARENT);

  RECT display = rc;
  TabCtrl_AdjustRect(wnd, FALSE, &display);
  ::InflateRect(&display, 2, 2);
  FrameSolid(dc, display, k_Edge);

  const int num = TabCtrl_GetItemCount(wnd);
  const int sel = TabCtrl_GetCurSel(wnd);
  const UINT dt = DT_SINGLELINE | DT_VCENTER | DT_CENTER | (HideAccel(wnd) ? DT_HIDEPREFIX : 0);
  for (int i = 0; i < num; i++)
  {
    RECT ir;
    if (!TabCtrl_GetItemRect(wnd, i, &ir))
      continue;
    if (i == sel)
    {
      ir.top -= 2;
      ir.bottom += 1;
    }
    FillSolid(dc, ir, i == sel ? CTRL_BRUSH : BACK_BRUSH);
    FrameSolid(dc, ir, k_Edge);
    if (i == sel)
      Line(dc, ir.left + 1, ir.bottom - 1, ir.right - 1, ir.bottom - 1, k_Ctrl);
    wchar_t text[256];
    text[0] = 0;
    TCITEMW item;
    memset(&item, 0, sizeof(item));
    item.mask = TCIF_TEXT;
    item.pszText = text;
    item.cchTextMax = 256;
    ::SendMessageW(wnd, TCM_GETITEMW, (WPARAM)i, (LPARAM)&item);
    ::SetTextColor(dc, i == sel ? k_Text : k_TextDim);
    ::DrawTextW(dc, text, -1, &ir, dt);
  }
  ::SelectObject(dc, oldFont);
}

// ---------- menu bar (undocumented UAH messages, as in Notepad++ and win32-darkmode)

#define WM_UAHDRAWMENU         0x0091
#define WM_UAHDRAWMENUITEM     0x0092

struct UAHMENU
{
  HMENU hmenu;
  HDC hdc;
  DWORD dwFlags;
};

struct UAHMENUITEMMETRICS
{
  DWORD rgsizeBar[2][2];
  DWORD rgsizePopup[4][2];
};

struct UAHMENUPOPUPMETRICS
{
  DWORD rgcx[4];
  DWORD fUpdateMaxWidths;
};

struct UAHMENUITEM
{
  int iPosition;
  UAHMENUITEMMETRICS umim;
  UAHMENUPOPUPMETRICS umpm;
};

struct UAHDRAWMENUITEM
{
  DRAWITEMSTRUCT dis;
  UAHMENU um;
  UAHMENUITEM umi;
};

static void DrawMenuBarBack(HWND wnd, const UAHMENU *um)
{
  MENUBARINFO mbi;
  memset(&mbi, 0, sizeof(mbi));
  mbi.cbSize = sizeof(mbi);
  if (!::GetMenuBarInfo(wnd, OBJID_MENU, 0, &mbi))
    return;
  RECT wr;
  ::GetWindowRect(wnd, &wr);
  RECT rc = mbi.rcBar;
  ::OffsetRect(&rc, -wr.left, -wr.top);
  rc.top -= 1;
  FillSolid(um->hdc, rc, BACK_BRUSH);
}

static void DrawMenuBarItem(UAHDRAWMENUITEM *p)
{
  wchar_t text[256];
  text[0] = 0;
  MENUITEMINFOW mii;
  memset(&mii, 0, sizeof(mii));
  mii.cbSize = sizeof(mii);
  mii.fMask = MIIM_STRING;
  mii.dwTypeData = text;
  mii.cch = 255;
  ::GetMenuItemInfoW(p->um.hmenu, (UINT)p->umi.iPosition, TRUE, &mii);

  const UINT st = p->dis.itemState;
  HBRUSH back = BACK_BRUSH;
  if (st & (ODS_HOTLIGHT | ODS_SELECTED))
    back = HOT_BRUSH;
  FillSolid(p->um.hdc, p->dis.rcItem, back);
  ::SetBkMode(p->um.hdc, TRANSPARENT);
  ::SetTextColor(p->um.hdc, (st & (ODS_INACTIVE | ODS_GRAYED | ODS_DISABLED)) ? k_TextDim : k_Text);
  UINT dt = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
  if (st & ODS_NOACCEL)
    dt |= DT_HIDEPREFIX;
  RECT rc = p->dis.rcItem;
  ::DrawTextW(p->um.hdc, text, -1, &rc, dt);
}

// the light line that Windows draws under the menu bar
static void DrawMenuBarBottomLine(HWND wnd)
{
  if (!::GetMenu(wnd))
    return;
  RECT client;
  ::GetClientRect(wnd, &client);
  ::MapWindowPoints(wnd, NULL, (POINT *)&client, 2);
  RECT wr;
  ::GetWindowRect(wnd, &wr);
  ::OffsetRect(&client, -wr.left, -wr.top);
  RECT line = client;
  line.bottom = line.top;
  line.top--;
  const HDC dc = ::GetWindowDC(wnd);
  if (dc)
  {
    FillSolid(dc, line, BACK_BRUSH);
    ::ReleaseDC(wnd, dc);
  }
}

// ---------- the subclass procedure

static void InvalidateLater(HWND wnd)
{
  ::InvalidateRect(wnd, NULL, FALSE);
}

static LRESULT CALLBACK DarkWindowProc(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  const WNDPROC old = (WNDPROC)::GetPropW(wnd, k_PropProc);
  const int kind = (int)(INT_PTR)::GetPropW(wnd, k_PropKind);
  if (!old)
    return ::DefWindowProcW(wnd, msg, wParam, lParam);

  if (msg == WM_NCDESTROY)
  {
    ::SetWindowLongPtrW(wnd, GWLP_WNDPROC, (LONG_PTR)old);
    ::RemovePropW(wnd, k_PropProc);
    ::RemovePropW(wnd, k_PropKind);
    return ::CallWindowProcW(old, wnd, msg, wParam, lParam);
  }

  if (!g_Dark)
    return ::CallWindowProcW(old, wnd, msg, wParam, lParam);

  switch (kind)
  {
    case k_Kind_Main:
    case k_Kind_Container:
    case k_Kind_Host:
    {
      if (IsCtlColor(msg))
        return CtlColor(msg, (HDC)wParam);
      if (msg == WM_ERASEBKGND && kind != k_Kind_Host)
      {
        RECT rc;
        ::GetClientRect(wnd, &rc);
        FillSolid((HDC)wParam, rc, BACK_BRUSH);
        return 1;
      }
      if (msg == WM_ERASEBKGND)
      {
        // rebar and toolbars: our background, then the controls draw on it
        wchar_t name[64];
        if (::GetClassNameW(wnd, name, 64) && ClassIs(name, REBARCLASSNAMEW))
        {
          RECT rc;
          ::GetClientRect(wnd, &rc);
          FillSolid((HDC)wParam, rc, BACK_BRUSH);
          return 1;
        }
      }
      if (msg == WM_NOTIFY)
      {
        LRESULT res = 0;
        if (HandleNotify(lParam, res))
          return res;
      }
      if (kind == k_Kind_Main)
      {
        if (msg == WM_UAHDRAWMENU)
        {
          DrawMenuBarBack(wnd, (const UAHMENU *)lParam);
          return TRUE;
        }
        if (msg == WM_UAHDRAWMENUITEM)
        {
          DrawMenuBarItem((UAHDRAWMENUITEM *)lParam);
          return TRUE;
        }
        if (msg == WM_NCPAINT || msg == WM_NCACTIVATE)
        {
          const LRESULT res = ::CallWindowProcW(old, wnd, msg, wParam, lParam);
          DrawMenuBarBottomLine(wnd);
          return res;
        }
      }
      break;
    }

    case k_Kind_ListView:
      if (msg == WM_NOTIFY)
      {
        LRESULT res = 0;
        if (HandleNotify(lParam, res))
          return res;
      }
      break;

    case k_Kind_Button:
      if (msg == WM_PAINT)
      {
        PAINTSTRUCT ps;
        const HDC dc = ::BeginPaint(wnd, &ps);
        PaintButton(wnd, dc);
        ::EndPaint(wnd, &ps);
        return 0;
      }
      if (msg == WM_PRINTCLIENT)
      {
        PaintButton(wnd, (HDC)wParam);
        return 0;
      }
      if (msg == WM_ERASEBKGND)
        return 1;
      if (msg == BM_SETCHECK || msg == BM_SETSTATE || msg == WM_ENABLE || msg == WM_SETTEXT
          || msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_UPDATEUISTATE
          || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_MOUSEMOVE
          || msg == WM_MOUSELEAVE || msg == WM_KEYDOWN || msg == WM_KEYUP)
      {
        // the button can draw itself here (light): we draw it again
        const LRESULT res = ::CallWindowProcW(old, wnd, msg, wParam, lParam);
        InvalidateLater(wnd);
        return res;
      }
      break;

    case k_Kind_StatusBar:
    case k_Kind_Tab:
      if (msg == WM_PAINT)
      {
        PAINTSTRUCT ps;
        const HDC dc = ::BeginPaint(wnd, &ps);
        if (kind == k_Kind_Tab)
          PaintTab(wnd, dc);
        else
          PaintStatusBar(wnd, dc);
        ::EndPaint(wnd, &ps);
        return 0;
      }
      if (msg == WM_ERASEBKGND)
        return 1;
      if (kind == k_Kind_StatusBar && (msg == SB_SETTEXTW || msg == SB_SETTEXTA || msg == SB_SETPARTS))
      {
        const LRESULT res = ::CallWindowProcW(old, wnd, msg, wParam, lParam);
        InvalidateLater(wnd);
        return res;
      }
      break;

    default:
      break;
  }
  return ::CallWindowProcW(old, wnd, msg, wParam, lParam);
}

}

#else

namespace NDarkMode {
void Init() {}
UInt32 GetSetting() { return 0; }
bool SetSetting(UInt32) { return false; }
bool IsDark() { return false; }
bool OnSettingChange(LPARAM) { return false; }
void ApplyTree(HWND) {}
void ApplyMainWindow(HWND) {}
}

#endif
