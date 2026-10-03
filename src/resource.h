#pragma once

// ---- icons ----
#define IDI_APP            101

// ---- tray menu commands ----
#define IDM_TRAY_BASE      40000
#define IDM_SHOW_HIDE      (IDM_TRAY_BASE + 1)
#define IDM_SETTINGS       (IDM_TRAY_BASE + 2)
#define IDM_CLICK_THROUGH  (IDM_TRAY_BASE + 4)
#define IDM_AUTOSTART      (IDM_TRAY_BASE + 5)
#define IDM_EXIT           (IDM_TRAY_BASE + 6)

// quick toggles for each display item, straight from the tray menu
#define IDM_TOGGLE_CPU     (IDM_TRAY_BASE + 10)
#define IDM_TOGGLE_MEM     (IDM_TRAY_BASE + 11)
#define IDM_TOGGLE_IP      (IDM_TRAY_BASE + 12)
#define IDM_TOGGLE_CPUTOP  (IDM_TRAY_BASE + 13)
#define IDM_TOGGLE_MEMTOP  (IDM_TRAY_BASE + 14)

// display-mode radio in the tray menu
#define IDM_MODE_MINIMAL   (IDM_TRAY_BASE + 20)
#define IDM_MODE_BARS      (IDM_TRAY_BASE + 21)
#define IDM_WINDOW_NORMAL (IDM_TRAY_BASE + 30)
#define IDM_WINDOW_GLOBAL (IDM_TRAY_BASE + 31)
#define IDM_WINDOW_DESKTOP (IDM_TRAY_BASE + 32)
#define IDM_HIDE_FULLSCREEN (IDM_TRAY_BASE + 33)
