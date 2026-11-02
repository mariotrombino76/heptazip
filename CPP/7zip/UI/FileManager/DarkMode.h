// DarkMode.h
// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 Mario Trombino (Heptazip, LICENSE-Heptazip)

#ifndef ZIP7_INC_DARK_MODE_H
#define ZIP7_INC_DARK_MODE_H

#include "../../../Common/MyTypes.h"
#include "../../../Common/MyWindows.h"

/*
Dark theme for the Win32 GUI (7zFM, 7zG), following the "Apps" setting of Windows 10/11
or a forced choice (View > Theme, registry Software\Heptazip\Theme: 0 system, 1 light,
2 dark).

Windows has no public dark mode for Win32 controls: like Explorer and Notepad++ we use
the "DarkMode_*" theme classes, the dark "app mode" of uxtheme.dll (ordinals 135/136, for
popup menus), the dark title bar of DWM, and our own drawing where nothing else works
(menu bar, check boxes, radio buttons, group boxes, status bar, tabs, toolbar
background). Everything is loaded at run time: on Windows older than 10 1903 or with
high contrast the theme stays light.

Each window is subclassed once (ApplyTree); the subclass procedure does nothing while
the theme is light, so the theme can be switched while the program runs.
*/

namespace NDarkMode {

const UInt32 k_Theme_System = 0;
const UInt32 k_Theme_Light = 1;
const UInt32 k_Theme_Dark = 2;

// reads the setting, loads the APIs; installs the hook for all dialogs (CDialog)
void Init();

UInt32 GetSetting();
// saves the setting; returns true if the theme (dark or light) changed
bool SetSetting(UInt32 setting);
bool IsDark();

// WM_SETTINGCHANGE: returns true if the theme changed (setting "system")
bool OnSettingChange(LPARAM lParam);

// applies the current theme to a window and all its child windows
void ApplyTree(HWND root);
// the main window: also draws the menu bar
void ApplyMainWindow(HWND mainWnd);

}

#endif
