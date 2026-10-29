// term: the window's app. Pure with respect to the OS: each frame it reads AppFrameInput (input, the server's tabs
// and the grids it shows, folder listings), lays the window out with rect cuts and draws it into a DrawList (panes
// with tab strips, splitters, tab drags, caption buttons, the sessions sidebar, the palette), puts each shown terminal
// into its pane as an external draw, turns keys and mouse into VT bytes for the focused terminal, and asks the
// platform for every side effect through AppOutput (term_app.h).
#include "term_app.h"
#include "term_panes.h"
#include "term_debug.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define R(x) RoundS32((f32)(x) * em)

// Key names (the platform's codes), for showing bindings and reading them from settings.txt; a key's first name is
// the one shown.
static const struct { u8 key; const char *name; } g_KeyNames[] = {
    { 5, "Space" }, { 6, "Tab" }, { 7, "Enter" }, { 8, "Backspace" }, { 9, "Delete" }, { 10, "Esc" }, { 11, "PgUp" },
    { 12, "PgDn" }, { 13, "Home" }, { 14, "End" }, { 15, "Left" }, { 16, "Right" }, { 17, "Up" }, { 18, "Down" },
    { 19, ";" }, { 20, "=" }, { 21, "-" }, { 22, "." }, { 23, "," }, { 24, "/" }, { 25, "\\" }, { 26, "`" }, { 27, "'" },
    { 28, "[" }, { 29, "]" }, { 46, "Insert" }, { 9, "Del" }, { 10, "Escape" }, { 11, "PageUp" }, { 12, "PageDown" },
};

// Segoe Fluent Icons (Windows 11; Segoe MDL2 Assets on 10).
enum {
    Icon_Add = 0xE710, Icon_Cancel = 0xE711, Icon_Prompt = 0xE756, Icon_Close = 0xE8BB, Icon_Minimize = 0xE921,
    Icon_Maximize = 0xE922, Icon_Restore = 0xE923, Icon_Robot = 0xE99A, Icon_Search = 0xE721, Icon_Folder = 0xE8B7,
    Icon_Page = 0xE7C3, Icon_ChevronRight = 0xE76C, Icon_Lock = 0xE72E, Icon_Up = 0xE74A, Icon_HardDrive = 0xEDA2,
    Icon_Down = 0xE74B, Icon_ChevronLeft = 0xE76B, Icon_CheckMark = 0xE73E, Icon_OpenInWindow = 0xE8A7, Icon_BackToWindow = 0xE73F,
    Icon_Cloud = 0xE753, Icon_Back = 0xE72B, Icon_Forward = 0xE72A, Icon_Star = 0xE734, Icon_StarFilled = 0xE735,
    Icon_History = 0xE81C, Icon_Home = 0xE80F, Icon_Download = 0xE896, Icon_Document = 0xE8A5, Icon_Picture = 0xE8B9,
    Icon_Music = 0xEC4F, Icon_Video = 0xE714, Icon_Desktop = 0xE7F4, Icon_OpenFolder = 0xE838,
    Icon_Refresh = 0xE72C, Icon_Globe = 0xE774, Icon_Copy = 0xE8C8,
};

// --- commands --------------------------------------------------------------------------------------------------
enum {
    Cmd_None, Cmd_NewTab, Cmd_NewAgentTab, Cmd_CloseTab, Cmd_SplitRight, Cmd_SplitDown, Cmd_FocusLeft, Cmd_FocusRight,
    Cmd_FocusUp, Cmd_FocusDown, Cmd_NextTab, Cmd_PrevTab, Cmd_NextPane, Cmd_EvenPanes, Cmd_ZoomIn, Cmd_ZoomOut, Cmd_ZoomReset,
    Cmd_Fullscreen, Cmd_Copy, Cmd_Paste, Cmd_ScrollPageUp, Cmd_ScrollPageDown, Cmd_ScrollBottom, Cmd_NextTheme,
    Cmd_Palette, Cmd_Profile, Cmd_DebugOverlay, Cmd_CloseWindow, Cmd_ToggleBrowser, Cmd_PrevCommand, Cmd_NextCommand, Cmd_Sessions, Cmd_Settings, Cmd_TearOff,
    Cmd_Find, Cmd_Search, Cmd_RenameTab, Cmd_ZoomPane, Cmd_ClearScrollback, Cmd_CopyAll, Cmd_Broadcast, Cmd_Topmost, Cmd_Duplicate,
    Cmd_ResizeLeft, Cmd_ResizeRight, Cmd_ResizeUp, Cmd_ResizeDown, Cmd_GoTo, Cmd_Bookmark, Cmd_FocusMode, Cmd_NewWebTab,
    Cmd_NewClaudeAs, Cmd_AddAccount, Cmd_Compose, Cmd_Reader,
    Cmd_Count
};

typedef struct CommandDef { const char *name; const char *label; u8 key; u8 mods; } CommandDef;

static const CommandDef g_Commands[Cmd_Count] = {
    [Cmd_NewTab]        = { "new_tab", "New tab", Key_A + 'T' - 'A', Mod_Ctrl | Mod_Shift },
    [Cmd_NewAgentTab]   = { "new_agent_tab", "New agent tab", Key_A, Mod_Ctrl | Mod_Shift },
    [Cmd_CloseTab]      = { "close_tab", "Close tab", Key_A + 'W' - 'A', Mod_Ctrl | Mod_Shift },
    [Cmd_SplitRight]    = { "split_right", "Split right", Key_Equals, Mod_Alt | Mod_Shift },
    [Cmd_SplitDown]     = { "split_down", "Split down", Key_Minus, Mod_Alt | Mod_Shift },
    [Cmd_FocusLeft]     = { "focus_left", "Focus the pane on the left", Key_Left, Mod_Alt },
    [Cmd_FocusRight]    = { "focus_right", "Focus the pane on the right", Key_Right, Mod_Alt },
    [Cmd_FocusUp]       = { "focus_up", "Focus the pane above", Key_Up, Mod_Alt },
    [Cmd_FocusDown]     = { "focus_down", "Focus the pane below", Key_Down, Mod_Alt },
    [Cmd_NextTab]       = { "next_tab", "Next tab", Key_Tab, Mod_Ctrl },
    [Cmd_PrevTab]       = { "prev_tab", "Previous tab", Key_Tab, Mod_Ctrl | Mod_Shift },
    [Cmd_NextPane]      = { "next_pane", "Next pane", 0, 0 },
    [Cmd_EvenPanes]     = { "even_panes", "Even out the panes", 0, 0 },
    [Cmd_ZoomIn]        = { "zoom_in", "Bigger text", Key_Equals, Mod_Ctrl },
    [Cmd_ZoomOut]       = { "zoom_out", "Smaller text", Key_Minus, Mod_Ctrl },
    [Cmd_ZoomReset]     = { "zoom_reset", "Default text size", Key_0, Mod_Ctrl },
    [Cmd_Fullscreen]    = { "fullscreen", "Fullscreen", Key_F1 + 10, 0 },
    [Cmd_Copy]          = { "copy", "Copy", Key_A + 'C' - 'A', Mod_Ctrl | Mod_Shift },
    [Cmd_Paste]         = { "paste", "Paste", Key_A + 'V' - 'A', Mod_Ctrl | Mod_Shift },
    [Cmd_ScrollPageUp]  = { "scroll_page_up", "Scroll up a page", Key_PageUp, Mod_Shift },
    [Cmd_ScrollPageDown]= { "scroll_page_down", "Scroll down a page", Key_PageDown, Mod_Shift },
    [Cmd_ScrollBottom]  = { "scroll_bottom", "Scroll to the bottom", Key_End, Mod_Shift },
    [Cmd_NextTheme]     = { "next_theme", "Next theme", 0, 0 },
    [Cmd_Palette]       = { "palette", "Command palette", Key_A + 'P' - 'A', Mod_Ctrl | Mod_Shift },
    [Cmd_Profile]       = { "profile", "Log a profile of the last frames", Key_F1 + 9, Mod_Ctrl | Mod_Alt | Mod_Shift },
    [Cmd_DebugOverlay]  = { "debug_overlay", "Debug overlay", Key_F1 + 11, Mod_Ctrl | Mod_Alt | Mod_Shift },
    [Cmd_CloseWindow]   = { "close_window", "Close the window (tabs keep running)", 0, 0 },
    [Cmd_ToggleBrowser] = { "toggle_browser", "Files / shell", Key_A + 'E' - 'A', Mod_Ctrl | Mod_Shift },
    [Cmd_Sessions]      = { "sessions", "Claude Code sessions", Key_A + 'S' - 'A', Mod_Ctrl | Mod_Shift },
    [Cmd_PrevCommand]   = { "prev_command", "Scroll to the previous command", Key_Up, Mod_Ctrl | Mod_Shift },
    [Cmd_NextCommand]   = { "next_command", "Scroll to the next command", Key_Down, Mod_Ctrl | Mod_Shift },
    [Cmd_Settings]      = { "settings", "Settings", Key_Comma, Mod_Ctrl },
    [Cmd_NewClaudeAs]   = { "claude_accounts", "Claude accounts", 0, 0 },
    [Cmd_AddAccount]    = { "add_claude_account", "Add a Claude account", 0, 0 },
    [Cmd_TearOff]       = { "tear_off", "Move tab to a new window", 0, 0 },
    [Cmd_Find]          = { "find", "Find", Key_A + 'F' - 'A', Mod_Ctrl },
    [Cmd_Search]        = { "search", "Search (as vim's /)", Key_Slash, Mod_Ctrl },
    [Cmd_RenameTab]     = { "rename_tab", "Rename tab", 0, 0 },
    [Cmd_ZoomPane]      = { "zoom_pane", "Zoom the pane (again: back)", Key_A + 'Z' - 'A', Mod_Ctrl | Mod_Shift },
    [Cmd_ResizeLeft]    = { "resize_left", "Move the pane's edge left", Key_Left, Mod_Alt | Mod_Shift },
    [Cmd_ResizeRight]   = { "resize_right", "Move the pane's edge right", Key_Right, Mod_Alt | Mod_Shift },
    [Cmd_ResizeUp]      = { "resize_up", "Move the pane's edge up", Key_Up, Mod_Alt | Mod_Shift },
    [Cmd_ResizeDown]    = { "resize_down", "Move the pane's edge down", Key_Down, Mod_Alt | Mod_Shift },
    [Cmd_CopyAll]       = { "copy_all", "Copy everything in the terminal", 0, 0 },
    [Cmd_Duplicate]     = { "duplicate_tab", "Duplicate the tab (its shell, its folder)", Key_A + 'D' - 'A', Mod_Ctrl | Mod_Shift },
    [Cmd_Topmost]       = { "always_on_top", "Always on top (again: not)", 0, 0 },
    [Cmd_Broadcast]     = { "broadcast", "Type into every pane (again: just the focused one)", 0, 0 },
    [Cmd_ClearScrollback] = { "clear_scrollback", "Clear the scrollback", Key_A + 'K' - 'A', Mod_Ctrl | Mod_Shift },
    [Cmd_GoTo]          = { "go_to", "Go to a folder", Key_A + 'G' - 'A', Mod_Ctrl | Mod_Shift },
    [Cmd_Bookmark]      = { "bookmark", "Bookmark this folder (again: not)", 0, 0 },
    [Cmd_Reader]        = { "reader", "Read the claude conversation (markdown, code to copy)", Key_A + 'R' - 'A', Mod_Ctrl | Mod_Shift },
    [Cmd_Compose]       = { "compose", "Write a prompt (Ctrl+Enter sends it to the terminal)", Key_A + 'I' - 'A', Mod_Ctrl | Mod_Shift },
    [Cmd_FocusMode]     = { "focus_mode", "Focus: this pane alone, nothing else (again: back)", Key_A + 'F' - 'A', Mod_Ctrl | Mod_Shift },
    [Cmd_NewWebTab]     = { "new_web_tab", "New web tab", Key_A + 'B' - 'A', Mod_Ctrl | Mod_Shift },
};

// --- settings: the palette's settings mode (Ctrl+,), a row each; a change applies at once and goes to settings.txt ---
enum {
    Setting_Theme, Setting_Font, Setting_FontSize, Setting_Ligatures, Setting_Opacity, Setting_Backdrop, Setting_Background, Setting_BackgroundStrength, Setting_Cursor, Setting_CursorBlink,
    Setting_DimUnfocused,
    Setting_CopyOnSelect, Setting_Notifications, Setting_VsEnv, Setting_HiddenFiles, Setting_FolderSizes,
    Setting_Agent, Setting_Agents, Setting_GoodProgrammer, Setting_StartWithWindows, Setting_ExplorerMenu, Setting_OpenFolder, Setting_Reset,
    Setting_Count
};
enum { SettingKind_Choice, SettingKind_Bool, SettingKind_Text, SettingKind_Action };
typedef struct SettingDef { const char *group, *label, *note; u8 kind; } SettingDef;

static const SettingDef g_Settings[Setting_Count] = {
    [Setting_Theme]         = { "Appearance", "Theme", "", SettingKind_Choice },
    [Setting_Font]          = { "Appearance", "Font", "monospace fonts installed", SettingKind_Choice },
    [Setting_FontSize]      = { "Appearance", "Text size", "Ctrl+= / Ctrl+-", SettingKind_Choice },
    [Setting_Ligatures]     = { "Appearance", "Ligatures", "-> != === drawn as one, in fonts that have them (Cascadia Code)", SettingKind_Bool },
    [Setting_Opacity]       = { "Appearance", "Opacity", "the terminals' background", SettingKind_Choice },
    [Setting_Backdrop]      = { "Appearance", "Behind the window", "seen through the terminals below 100% opacity", SettingKind_Choice },
    [Setting_Background]    = { "Appearance", "Background image", "behind the terminals, across the panes", SettingKind_Action },
    [Setting_BackgroundStrength] = { "Appearance", "Background image strength", "how much of it shows through", SettingKind_Choice },
    [Setting_Cursor]        = { "Appearance", "Cursor", "when the program asks for none", SettingKind_Choice },
    [Setting_CursorBlink]   = { "Appearance", "Cursor blinks", "", SettingKind_Bool },
    [Setting_DimUnfocused]  = { "Appearance", "Dim the panes not in focus", "the focused one stands out", SettingKind_Bool },
    [Setting_CopyOnSelect]  = { "Terminal", "Copy on select", "a finished selection goes to the clipboard", SettingKind_Bool },
    [Setting_Agent]         = { "Agents", "Agent command", "what Ctrl+Shift+A starts", SettingKind_Text },
    [Setting_Agents]        = { "Agents", "Agent programs", "tabs running these show as agents", SettingKind_Text },
    [Setting_GoodProgrammer] = { "Agents", "Be a good programmer", "your principles (good-programmer.md) in every Claude Code session", SettingKind_Bool },
    [Setting_HiddenFiles]   = { "Files", "Show hidden files", "Ctrl+H", SettingKind_Bool },
    [Setting_FolderSizes]   = { "Files", "Folder sizes", "local SSDs, measured in the background", SettingKind_Bool },
    [Setting_Notifications] = { "Shell", "Notifications", "when a tab off screen wants you", SettingKind_Bool },
    [Setting_VsEnv]         = { "Shell", "Visual Studio build tools", "in new tabs", SettingKind_Bool },
    [Setting_StartWithWindows] = { "More", "Start with Windows", "tabs come back at sign-in, in the tray", SettingKind_Bool },
    [Setting_ExplorerMenu]  = { "More", "\"Open in moss\" in Explorer", "on folders and their background", SettingKind_Bool },
    [Setting_OpenFolder]    = { "More", "Open the settings folder", "settings.txt and the shell setup", SettingKind_Action },
    [Setting_Reset]         = { "More", "Reset to moss's defaults", "settings.txt as moss ships it; the old one kept as settings-old.txt", SettingKind_Action },
};
enum { PaletteMode_Commands, PaletteMode_Settings, PaletteMode_GoTo, PaletteMode_Account, PaletteMode_Model };

// The models the account line's picker offers (Claude Code's /model names); a name typed that isn't one is offered too.
static const char *const g_Models[][2] = {
    { "default", "the plan's default" }, { "opus", "the most capable" }, { "sonnet", "fast and capable" },
    { "haiku", "the fastest" }, { "opus[1m]", "Opus with a 1M token context" }, { "sonnet[1m]", "Sonnet with a 1M token context" },
};

// --- terminal themes: the terminal's colors; the chrome is made from them ----------------------------------------
// The first is the default when settings.txt names none: Moss, term's own (the official theme).
static const TermTheme g_TermThemes[] = {
    { "Moss", 0, 0xD3DDD3, 0x141A16, 0x2F4235, 0xA3D977,
      { 0x202822, 0xE8736C, 0xA3D977, 0xE6C46A, 0x7FA8D6, 0xC296C9, 0x72C7B0, 0xBAC6BB,
        0x56635A, 0xF59088, 0xBDEB97, 0xF2D68C, 0x9DC0EA, 0xD6B0DC, 0x92DCC6, 0xEEF3EE } },
    { "Graphite", 0, 0xD5D8DC, 0x15171A, 0x2C3A40, 0x2EC4B6,
      { 0x23262B, 0xEF6B73, 0x8CCF7E, 0xE5C07B, 0x6CA8F0, 0xC58AF9, 0x2EC4B6, 0xB8BCC2,
        0x5C636B, 0xFF8A91, 0xA6E39A, 0xF2D49B, 0x8FC0FF, 0xD7A8FF, 0x5EE0D3, 0xEEF0F2 } },
    { "Dusk", 0, 0xDCD7EE, 0x1B1A2A, 0x3A3660, 0xFFA07A,
      { 0x2A2840, 0xF0717F, 0x9BD3A0, 0xF4C97A, 0x8AA6FF, 0xC79BF2, 0x7FD4E0, 0xC5C0DA,
        0x625E80, 0xFF8E9A, 0xB5E6B9, 0xFFDC9C, 0xA9BEFF, 0xDAB6FF, 0x9FE6EF, 0xF2EFFA } },
    { "Signal", 0, 0xE6E6E6, 0x101010, 0x3A3220, 0xFFB000,
      { 0x1E1E1E, 0xFF5F56, 0x5FD38D, 0xFFB000, 0x5AA9FF, 0xC792EA, 0x4FD6D6, 0xBDBDBD,
        0x5A5A5A, 0xFF7B73, 0x7FE3A5, 0xFFC94D, 0x7FBFFF, 0xD9AEF2, 0x77E5E5, 0xF5F5F5 } },
    { "Paper", 1, 0x2E2C29, 0xF7F4ED, 0xE4DCCB, 0xD9480F,
      { 0x2E2C29, 0xC4372D, 0x3F7D3A, 0x9A6A00, 0x2F5FA8, 0x8A3FA0, 0x1F7A7A, 0x8C877E,
        0x5E5A53, 0xD9480F, 0x4F9446, 0xB07D00, 0x3B73C6, 0xA453BA, 0x2A9191, 0xB5AFA4 } },
    { "Tokyo Night", 0, 0xC0CAF5, 0x1A1B26, 0x33467C, 0x7AA2F7,
      { 0x15161E, 0xF7768E, 0x9ECE6A, 0xE0AF68, 0x7AA2F7, 0xBB9AF7, 0x7DCFFF, 0xA9B1D6,
        0x414868, 0xF7768E, 0x9ECE6A, 0xE0AF68, 0x7AA2F7, 0xBB9AF7, 0x7DCFFF, 0xC0CAF5 } },
    { "Catppuccin Mocha", 0, 0xCDD6F4, 0x1E1E2E, 0x45475A, 0xCBA6F7,
      { 0x45475A, 0xF38BA8, 0xA6E3A1, 0xF9E2AF, 0x89B4FA, 0xF5C2E7, 0x94E2D5, 0xBAC2DE,
        0x585B70, 0xF38BA8, 0xA6E3A1, 0xF9E2AF, 0x89B4FA, 0xF5C2E7, 0x94E2D5, 0xA6ADC8 } },
    { "Ember", 0, 0xD8D2C6, 0x1C1A18, 0x4A3A30, 0xD97757,
      { 0x2B2826, 0xE5636F, 0x9CBF7E, 0xE3B65E, 0x78A6D8, 0xC590C8, 0x6FBFB2, 0xC9C2B5,
        0x6E6861, 0xF4848E, 0xB5D69A, 0xF2CC84, 0x9BC0EF, 0xDCAEDD, 0x93D8CD, 0xF3EFE7 } },
    { "Nord", 0, 0xD8DEE9, 0x2E3440, 0x434C5E, 0x88C0D0,
      { 0x3B4252, 0xBF616A, 0xA3BE8C, 0xEBCB8B, 0x81A1C1, 0xB48EAD, 0x88C0D0, 0xE5E9F0,
        0x4C566A, 0xBF616A, 0xA3BE8C, 0xEBCB8B, 0x81A1C1, 0xB48EAD, 0x8FBCBB, 0xECEFF4 } },
    { "Campbell", 0, 0xCCCCCC, 0x0C0C0C, 0x264F78, 0x3B78FF,
      { 0x0C0C0C, 0xC50F1F, 0x13A10E, 0xC19C00, 0x0037DA, 0x881798, 0x3A96DD, 0xCCCCCC,
        0x767676, 0xE74856, 0x16C60C, 0xF9F1A5, 0x3B78FF, 0xB4009E, 0x61D6D6, 0xF2F2F2 } },
    { "Catppuccin Latte", 1, 0x4C4F69, 0xEFF1F5, 0xCCD0DA, 0x8839EF,
      { 0x5C5F77, 0xD20F39, 0x40A02B, 0xDF8E1D, 0x1E66F5, 0xEA76CB, 0x179299, 0xACB0BE,
        0x6C6F85, 0xD20F39, 0x40A02B, 0xDF8E1D, 0x1E66F5, 0xEA76CB, 0x179299, 0xBCC0CC } },
    { "GitHub Light", 1, 0x1F2328, 0xFFFFFF, 0xB6E3FF, 0x0969DA,
      { 0x24292F, 0xCF222E, 0x116329, 0x4D2D00, 0x0969DA, 0x8250DF, 0x1B7C83, 0x6E7781,
        0x57606A, 0xA40E26, 0x1A7F37, 0x633C01, 0x218BFF, 0xA475F9, 0x3192AA, 0x8C959F } },
};
enum { TermTheme_Count = (s32)(sizeof(g_TermThemes) / sizeof(g_TermThemes[0])) };

// The window's own colors, made from the terminal theme: the panes are the terminal's background, the title band
// (the tab strips) a shade off it, popups a shade towards the text. Hover and press are see-through text color, so
// they work on any fill.
typedef struct Chrome {
    u32 bg, caption, surface, text, dim, faint, line, hover, press, accent, on_accent, selection, danger;
} Chrome;

// A terminal's palette in a theme. The find bar's matches: the theme's yellow a third of the way in from the background.
static TermPalette TermPaletteFor(const TermTheme *tt, b32 ligatures)
{
    u32 found_bg = 0;
    for (s32 k = 0; k < 3; k++) {
        u32 a = (tt->bg >> (k * 8)) & 0xFF, b = (tt->ansi[3] >> (k * 8)) & 0xFF;
        found_bg |= ((a * 2 + b) / 3) << (k * 8);
    }
    TermPalette p = {
        tt->fg, tt->bg, tt->selection, tt->accent, found_bg, tt->ansi[11], 0, 0, 0, 0, ligatures,
        { tt->ansi[0], tt->ansi[1], tt->ansi[2], tt->ansi[3], tt->ansi[4], tt->ansi[5], tt->ansi[6], tt->ansi[7],
          tt->ansi[8], tt->ansi[9], tt->ansi[10], tt->ansi[11], tt->ansi[12], tt->ansi[13], tt->ansi[14], tt->ansi[15] },
    };
    return p;
}

static Chrome ChromeFor(const TermTheme *tt)
{
    Chrome c = {0};
    u32 bg = 0xFF000000u | tt->bg, fg = 0xFF000000u | tt->fg;
    c.bg = bg;
    c.caption = ColorMix(bg, tt->light ? 0xFF000000u : 0xFFFFFFFFu, tt->light ? 0.05f : 0.035f);
    c.surface = tt->light ? ColorMix(bg, 0xFFFFFFFFu, 0.6f) : ColorMix(bg, fg, 0.07f);
    c.text = fg;
    c.dim = ColorMix(bg, fg, 0.62f);
    c.faint = ColorMix(bg, fg, 0.38f);
    c.line = ColorMix(bg, fg, 0.15f);
    c.hover = ColorAlpha(fg, 0.08f);
    c.press = ColorAlpha(fg, 0.14f);
    c.accent = 0xFF000000u | tt->accent;
    u32 a = tt->accent;
    c.on_accent = ((a >> 16) & 255) * 299 + ((a >> 8) & 255) * 587 + (a & 255) * 114 > 150 * 1000 ? 0xFF000000u : 0xFFFFFFFFu;
    c.selection = 0xFF000000u | tt->selection;
    c.danger = 0xFFC42B1Cu;
    return c;
}

// --- app state ---------------------------------------------------------------------------------------------------
typedef struct Selection {
    u32 term_id;          // 0 = none
    s32 ax, ay, bx, by;   // anchor and moving end, in the tab's view cells
    b32 dragging;
    b32 visible;          // anything selected (a click alone selects nothing)
    s32 clicks;
    b32 block;            // a rectangle (Alt+drag) instead of reading order
    u64 click_us;
    s32 click_x, click_y;
} Selection;

typedef struct NamedRect { String name; Rect rect; } NamedRect;
enum { App_MaxNamed = 512, App_MaxPanes = 64 };

// The history view (a claude's transcript, searched): its messages wrapped to the pane, and the matches in it.
enum { HistLine_Text = 0, HistLine_User, HistLine_Claude, HistLine_Blank, HistLine_Heading, HistLine_Code, HistLine_Table, HistLine_Rule };
enum { HistSpan_Bold = 1, HistSpan_Code = 2 };
typedef struct HistSpan { s32 start, len; u32 style; } HistSpan;   // a bold or `code` run in a line's text
typedef struct HistLine {
    u32 kind;                 // HistLine_*
    String text;              // markdown's marks taken out
    HistSpan *spans; s32 span_count;
    String marker; s32 indent;   // a list item: its marker (on its first line) and how far its lines are in
    s32 block; b32 first;     // Code: its block (App.hist_blocks), the block's first line; Table: the header row
    String *cells; s32 cell_count; s32 *widths;   // Table: the row's cells, its table's column widths
} HistLine;
typedef struct HistMatch { s32 line, start, len; } HistMatch;

// A tab's other face: its folder listed. Listings come from the platform's lister (term_files.h), a folder watched
// while shown is listed again when it changes.
typedef struct Browser {
    u8 path_buf[1024];
    String path;                // the folder ("" = This PC), no trailing separator ("C:")
    u32 request;                // its listing on the way (0: none)
    b32 listed;                 // a listing came for this folder
    b32 stale;                  // it changed since: list it again
    s32 error;                  // the listing's (5: access denied)
    Arena list_arena;
    s64 count;
    FileEntry *entries;
    Arena view_arena;
    s32 *view;                  // the rows: entries in display order (sorted, filtered)
    s64 view_count, view_dirs, view_files;
    b32 view_dirty;
    u64 view_key;               // filter, sort and hidden files it was built with
    s64 cursor, top;
    b32 reveal;                 // scroll the cursor into view (it moved by key)
    u8 select[260];             // select this name once the listing has it (the folder we came up from)
    s32 select_len;
    u8 cursor_name[260];        // the selected row's name: the selection stays on it when rows come and go
    s32 cursor_name_len;
    u8 top_name[260];           // listed again: the row that was at the top stays there
    s32 top_name_len;
    struct { u64 key; u64 at_us; } flashes[32];   // rows new or changed in a listing again (NameKey | 1), lit for a moment
    s32 flash_next;
    b32 renaming;              // the selected row is a text edit (F2)
    TextEdit rename;
    Arena sel_arena;
    Arena size_arena;
    Array(struct { u64 key; u64 bytes; }) sizes;   // subfolders' sizes by NameKey (a listing again keeps them)
    Array(u64) sel;             // the selected rows' NameKeys, sorted; empty: the cursor's row alone
    s64 anchor;                 // Shift ranges reach from this row
    u8 filter[256];             // typed text: rows whose names have it
    s32 filter_len;
    b32 cd_pending;             // moved while the shell was idle: it follows when the browser closes
    u8 opened_cwd[520];         // the shell's folder when the browser opened last
    s32 opened_cwd_len;
    u32 shown_frame;            // the last frame it was shown (its folder watched)
    u8 back[16][520];           // folders it was in, the last one last (Back goes there); "" is This PC
    s32 back_len[16], back_count;
    u8 forward[16][520];        // folders Back left, the next one last
    s32 forward_len[16], forward_count;
    b32 from_history;           // this move is Back / Forward: the histories are already right
    u8 notice[160];             // a short message in the footer (why something was refused)
    s32 notice_len;
    u64 notice_until_us;
    struct Browser *free_next;
} Browser;

// A tab's picture view: a picture over the tab, its folder's other pictures a key away. Its folder is listed through
// the same lister; the platform decodes the pictures on a worker (AppOutput.image_wants -> AppFrameInput.images).
typedef struct Viewer {
    u8 folder_buf[1024];
    String folder;
    u8 name_buf[520];
    String name;                // the picture shown (empty: the folder's first, once listed)
    u32 request;                // its folder's listing on the way
    b32 listed;
    Arena arena;
    s64 count;                  // the folder's pictures, in Explorer's order
    String *names;
    b32 from_browser;           // Esc goes back to the browser (else the shell)
    struct Viewer *free_next;
} Viewer;

// Where a dragged tab lands: into a strip before a tab (0: at its end), into a pane, or a new pane beside a node
// (0: the window's edge, across the root's split).
typedef struct TabDrop {
    u64 node_id, before_id;
    b32 strip;
    b32 into;                   // join the node's pane as a tab
    b32 vertical;               // the new pane beside (else above / below)
    b32 after;                  // right of / below the node
} TabDrop;

struct App {
    Arena arena;
    Arena frame_arena;
    Ui ui;
    DrawList dl;
    Font *fonts;              // this frame's (the platform's)
    Chrome c;
    f32 em;
    f32 term_font_pt;
    s32 term_theme;
    s32 theme_shown, theme_from;   // the theme drawn, and the one it fades in from
    u32 theme_changes;        // counts changes: each fade is its own animation (0: none yet, no fade)
    b32 headless;
    f32 opacity;              // terminals' default backgrounds (0.2..1)
    String agent_command;     // points into agent_buf
    u8 agent_buf[256];
    String agents;            // the server's agent names (settings.txt `agents`), in agents_buf
    u8 agents_buf[256];
    String term_font;         // the terminal font family, in term_font_buf
    u8 term_font_buf[128];
    b32 notifications_off;
    b32 vs_env_off;
    String window_name;       // empty = the main window (window_name_buf)
    u8 window_name_buf[64];
    b32 windows_opened;       // the main window asked for the other windows that own tabs

    Panes panes;
    u32 next_request;
    b32 tabs_seen;            // a tab list has come since the last connect
    b32 was_connected;
    b32 had_tabs;             // a pane has shown a server tab (closing the last one then closes the window)
    b32 first_requested;      // the first shell was asked for (no tabs at all)
    b32 placement_pending;
    SessionLayout restore;    // window placement from the session
    Arena session_arena;
    String session_saved;
    u64 session_check_us;
    s32 window_rect[4];
    b32 maximized;

    // --- per frame, from the input ---
    const AppFrameInput *in;
    const TermTab *tabs;
    s64 tab_count;

    // --- requests sent ---
    TermWatch sent_watch[App_MaxPanes * 8];
    s64 sent_watch_count;
    b32 watch_ever_sent;
    u64 layout_changed_us;    // panes last moved (splitter drag, window resize): sizes go out once still

    // --- interaction ---
    u64 tab_drag_id;          // the PaneTab picked up out of its strip (0 = none)
    u32 arrive_tab;           // a server tab dropped here from another window, on its way: it lands at arrive_drop
    TabDrop arrive_drop;
    s32 grab_offset;
    u32 snap_until_frame;
    Selection sel;
    u32 mouse_tab;            // the program holding a mouse button (reported press)
    s32 mouse_button;         // + 1
    s32 mouse_cx, mouse_cy;   // last reported cell, 1-based
    s32 wheel_accum;
    u64 blink_start_us;
    u8 backdrop;              // 0 the desktop, 1 acrylic, 2 Mica (settings `backdrop`)
    s32 background_strength;  // percent of the background image seen through the theme'"'"'s background (0: none)
    u8 cursor;                // 0 block, 1 bar, 2 underline (settings `cursor`): when the program asks for none
    b32 cursor_blink;
    b32 ligatures;
    b32 copy_on_select_off;
    b32 dim_unfocused_off;    // settings.txt `dim_unfocused = off`
    b32 broadcast;            // keys typed go to every terminal shown in the window
    b32 explorer_menu;        // Explorer has "Open in moss"
    b32 start_with_windows;   // the server starts at sign-in
    b32 good_programmer;      // the global CLAUDE.md has the principles block
    u64 tab_menu;             // the PaneTab whose right-click menu is open (0 = none), at tab_menu_at
    s32 tab_menu_at[2];
    u8 bind_key[Cmd_Count];   // each command's key and modifiers: g_Commands' unless settings.txt `key.<command>` says
    u8 bind_mods[Cmd_Count];
    u32 sb_drag_tab;          // the terminal whose scrollbar thumb is dragged (0 = none)
    s32 sb_grab;              // where in the thumb it was taken
    s32 sb_target;            // the scroll offset asked for so far (the grid catches up a frame later)
    u64 rename_tab;           // the PaneTab whose name is being edited in its strip (0 = none)
    TextEdit rename_edit;
    u64 web_edit_tab;         // the PaneTab whose web address is being edited (0 = none)
    TextEdit web_edit;
    u32 web_popup_taken;      // the last page's new window (AppWebPopup.id) a web tab was made for
    u64 zoom_node;            // the pane shown over the whole area (0 = none)
    b32 focus_mode;           // focus: the focused pane alone over the whole window, no strips, sidebar or caption buttons;
                              // only its shell / browser keys work (zoom_node is its pane meanwhile)
    u64 focus_prev_zoom;      // zoom_node from before, back after
    u64 focus_since_us;       // when it came on (its hint fades)
    b32 find_open;            // the find bar, on the pane of find_term
    b32 find_focus;           // it has the keyboard (a click in the pane gives it back to the terminal)
    u32 find_term;
    TextEdit find_edit;
    b32 find_case;
    u64 find_sent;            // hash of the text and case last sent (0 = nothing)
    Rect find_rect;           // where it was last frame (clicks there keep its focus)
    b32 find_vim;             // opened by Ctrl+/: Enter ends typing, then n / N step; else Enter / Shift+Enter step
    b32 find_history;         // a claude's: its whole history (the transcript) in the history view, not the scrollback
    Arena hist_arena;
    u32 hist_version, hist_term;   // what the history view was built from
    s32 hist_width;
    u64 hist_query;
    Array(HistLine) hist_lines;
    Array(HistMatch) hist_matches;
    s32 hist_current;         // the current match, -1 = none
    Array(String) hist_blocks;   // the code blocks' text (their copy buttons)
    s32 hist_copied;          // the block copied last (1-based), and when: "Copied" for a moment
    u64 hist_copied_us;
    b32 hist_at_end;          // the view was at its end: a fresh read stays at the end
    // --- the reader (Ctrl+Shift+R): a claude tab's transcript as the history view, without a search; read again when
    //     a turn ends ---
    b32 reader_open;
    u32 reader_term;
    b32 reader_busy;          // its claude's turn was under way (a read follows the end)
    f32 hist_scroll;          // px from the top
    b32 hist_follow;          // scroll the current match into view (or the end, with none)
    // --- Claude accounts (accounts.txt): [0] is the default one (Claude Code's own); a claude tab's account is the one
    //     with its config folder ---
    u32 accounts_version;
    u8 accounts_buf[8192];
    struct { String name, dir; s32 theme; } accounts[16];   // theme: a g_TermThemes index, -1 = the window's
    s32 account_count;
    struct { String folder, account; } folder_rules[32];  // claudes under folder use account, and it is used nowhere else
    s32 folder_rule_count;
    u8 pending_account[64];   // an account just asked for: its claude tab opens (to /login) once accounts.txt lists it
    s32 pending_account_len;
    b32 account_adding;       // the accounts palette came from "Add a Claude account" (its prompt asks for a name)
    u32 model_term;           // the claude tab the model picker (PaletteMode_Model) is for
    // --- the prompt composer (Ctrl+Shift+I): a draft written in a real text box, sent to its terminal as a paste and
    //     Enter (Ctrl+Enter); one draft, kept while it's away ---
    TextArea compose;
    b32 compose_open;
    u32 compose_term;
    b32 palette_open;
    u8 palette_mode;          // PaletteMode_*
    TextEdit palette_edit;
    s32 palette_sel;
    s32 palette_top;          // the first row shown; follows the selection when it moves
    s32 palette_sel_seen;
    u64 palette_filter_hash;
    // --- the go-to box (the palette's GoTo mode): a typed path lists its folder; empty, the groups below ---
    u64 goto_tab;             // the PaneTab whose browser it moves
    u8 goto_closed;           // groups folded to their heading (bit per Goto_* group)
    b32 goto_all_recents;     // every recent, not the newest 10
    u8 goto_dir_buf[1024];
    String goto_dir;          // the folder the typed text is in (up to its last separator), as listed
    u32 goto_request;         // its listing on the way (0: none)
    b32 goto_listed;
    Arena goto_arena;
    s64 goto_count;
    FileEntry *goto_entries;
    u32 drives_request;       // the drives (Storage), listed when the box opens
    Arena drives_arena;
    s64 drive_count;
    FileEntry *drives;
    String environment;       // NAME=value lines: %NAME% in a typed path
    s32 place_count;
    AppPlace places[8];
    u8 recent[32][520];       // folders browsed, newest first (paths.txt)
    s32 recent_len[32], recent_count;
    u8 bookmark[32][520];     // folders bookmarked (Ctrl+D, the path bar's star), in the order they were added
    s32 bookmark_len[32], bookmark_count;
    b32 paths_dirty;          // save paths.txt
    s32 setting_editing;      // the Setting_* + 1 whose text is being edited in its row
    TextEdit setting_edit;
    u64 confirm_tab;          // PaneTab id waiting for "close anyway"
    u32 paste_wait;           // the terminal a paste goes to, once the clipboard's text is in (AppOutput.read_clipboard)
    u32 paste_confirm;        // the terminal a paste of several lines waits to be confirmed for (paste_arena holds it)
    b32 reset_confirm;        // "Reset to moss's defaults" waits to be confirmed
    Arena paste_arena;
    String paste_text;
    b32 paste_warning_off;
    b32 win32_input_off;
    u32 vk_withheld[8];       // keys whose press term kept (a command): their release isn't sent either
    b32 debug_overlay;
    u32 queued[32];
    s32 queued_count;
    Browser *free_browsers;
    Viewer *free_viewers;
    b32 sidebar_open;         // the sessions sidebar on the window's left
    b32 side_focus;           // it has the keyboard (type to filter, arrows, Enter resumes)
    u8 side_filter[128];
    // --- the sidebar's own notes on sessions (sessions.txt): a name of the user's, pinned (first), archived (left out
    //     unless something's typed); a right-click's menu; a name being edited in its row; the full-text search ---
    struct { u8 id[48]; s32 id_len; u8 name[128]; s32 name_len; b32 pin, archive; } side_meta[256];
    s32 side_meta_count;
    u8 side_menu_id[48]; s32 side_menu_len;   // the session its menu is for (none: 0)
    s32 side_menu_x, side_menu_y;
    u8 side_rename_id[48]; s32 side_rename_len;
    TextEdit side_rename_edit;
    u8 side_sent[128]; s32 side_sent_len;   // the text every transcript was last searched for
    u64 side_typed_us;        // the filter last changed: the search goes out once it's been still a moment
    u32 side_typed_frame;     // (headless runs: two frames, their clock moves only with frames)
    s32 side_filter_len;
    s64 side_cursor;          // the selected session (in the filtered list)
    s64 side_top;             // px scrolled
    b32 side_reveal;          // scroll the selection into view
    u8 browse_sort;           // every browser's: 0 name, 2 modified, 4 size; +1 the other way round
    b32 hide_hidden;          // browsers leave out hidden files (Ctrl+H)
    b32 folder_sizes_off;     // settings `folder_sizes = off`: no subfolder sizes
    u32 next_file_request;

    // --- last frame, for the headless runner ---
    NamedRect named[App_MaxNamed];
    s32 named_count;
    struct { Rect rect; u32 term_id; } pane_rects[App_MaxPanes];
    s32 pane_rect_count;
};

static const TermTab *FindServerTab(App *app, u32 term_id)
{
    for (s64 i = 0; term_id && i < app->tab_count; i++) if (app->tabs[i].id == term_id) return &app->tabs[i];
    return NULL;
}

// One field of a claude's account (TermTab.account): 0 login method, 1 organization, 2 email, 3 model.
static String AccountPart(String account, s32 k)
{
    s64 at = 0;
    for (s32 i = 0; i < k; i++) {
        while (at < account.count && account.data[at] != '\n') at++;
        if (at >= account.count) return (String){0};
        at++;
    }
    s64 end = at;
    while (end < account.count && account.data[end] != '\n') end++;
    return (String){ account.data + at, end - at };
}

static const TermGrid *FindGrid(const AppFrameInput *in, u32 term_id)
{
    for (s64 i = 0; term_id && i < in->grid_count; i++) if (in->grids[i].tab_id == term_id) return &in->grids[i];
    return NULL;
}

// A UI target the headless runner can find by name (`cursorui`).
static void Named(App *app, String name, Rect r)
{
    if (app->named_count >= App_MaxNamed) return;
    NamedRect *n = &app->named[app->named_count++];
    n->name = Str_PushCopy(&app->frame_arena, name);
    n->rect = r;
}

// The name a tab shows: the server's, without the marks programs put in front (claude's ✳ and spinner, the
// shell's ❯), which the tab's own icon says better.
static String TabLabel(const TermTab *t)
{
    if (!t) return S("starting...");
    String n = t->name;
    for (;;) {
        s64 i = 0;
        u32 c = n.count ? Utf8_Decode(n, &i) : 0;
        if (c == 0x2733 || (c >= 0x25D0 && c <= 0x25D3) || c == 0x276F || c == 0x00B7 || c == ' ' || (c >= 0x2800 && c <= 0x28FF)) {
            n.data += i;
            n.count -= i;
            continue;
        }
        break;
    }
    return n.count ? n : S("tab");
}

static String PaneTabLabel(App *app, const PaneTab *t)
{
    if (!t->title_len && t->web) {
        return t->web_title_len ? (String){ (u8 *)t->web_title, t->web_title_len } : t->web_url_len ? (String){ (u8 *)t->web_url, t->web_url_len } : S("New tab");
    }
    return t->title_len ? (String){ (u8 *)t->title, t->title_len } : TabLabel(FindServerTab(app, t->term_id));
}

// An icon (a Segoe Fluent Icons codepoint) centred in r.
static void Draw_Icon(DrawList *dl, Font *font, Rect r, u32 color, u32 icon)
{
    u8 buf[4];
    String s = { buf, Utf8_Encode(buf, icon) };
    Draw_TextIn(dl, font, r, color, s, 0.5f);
}

// A hover fill as it fades in (fast) and out (slower): `color` at the hover's current strength. A press shows at once.
static u32 HoverFill(App *app, u64 id, UiSignal s, u32 color)
{
    b32 on = s.hovered || s.held;
    f32 a = Ui_Animate(&app->ui, Ui_IdN(id, 0x40F), 0, on ? 1.0f : 0.0f, on ? 40.0f : 12.0f);
    return s.held ? color : ColorAlpha(color, a);
}

// A flat button with an icon: a fill under the mouse (`hover`), named for the headless runner.
static UiSignal IconButton(App *app, u64 id, Rect r, u32 icon, Font *font, u32 color, u32 hover, f32 radius, String name)
{
    UiSignal s = Ui_Hit(&app->ui, id, r);
    Draw_Rect(&app->dl, r, HoverFill(app, id, s, s.held && hover == app->c.hover ? app->c.press : hover), radius);
    Draw_Icon(&app->dl, font, r, color, icon);
    if (name.count) Named(app, name, r);
    return s;
}

// A popup's box: a soft shadow, the fill, a hairline.
static void PopupBox(App *app, Rect r, f32 radius)
{
    f32 em = app->em;
    Rect sh = r;
    sh.y0 += R(0.25);
    sh.y1 += R(0.5);
    Draw_RectEx(&app->dl, sh, 0x50000000u, 0, 0, radius, NULL, 0, 0, (f32)R(0.9));
    Draw_RectEx(&app->dl, r, app->c.surface, app->c.line, 1, radius, NULL, 0, 0, 0);
}

// A popup coming in: from the frame it first shows it eases in, sliding down half a line and fading in (the draw
// list fades everything drawn until it's put back to 0). Returns how far up it still sits.
static s32 PopupAppear(App *app, u64 id)
{
    f32 a = Ui_Animate(&app->ui, id, 0, 1, 22);
    app->dl.fade = 1.0f - a;
    return -RoundS32((1.0f - a) * app->em * 0.5f);
}

// Everything under a popup: dimmed when `dim`, and nothing there takes the mouse. 1 when it was clicked.
static b32 Blocker(App *app, b32 dim)
{
    Rect all = R4(0, 0, app->dl.w, app->dl.h);
    UiSignal s = Ui_Hit(&app->ui, Ui_Id("Blocker"), all);
    if (dim) Draw_Rect(&app->dl, all, 0x38000000u, 0);
    return s.pressed;
}

// A question in the middle of the window: a title, a line, Cancel and an OK button (red when danger). 1 OK or Enter,
// -1 Cancel or Esc, 0 not yet.
static s32 ConfirmBox(App *app, String title, String text, String ok_label, String ok_name, b32 danger)
{
    f32 em = app->em;
    Ui *ui = &app->ui;
    DrawList *dl = &app->dl;
    Chrome *c = &app->c;
    Blocker(app, 1);
    Rect win = R4(0, 0, dl->w, dl->h);
    Rect box = RectCenter(win, Min(R(28), dl->w - R(2)), R(9.6));
    box.y0 -= dl->h / 10;
    box.y1 -= dl->h / 10;
    s32 slide = PopupAppear(app, Ui_IdS(title));
    box.y0 += slide;
    box.y1 += slide;
    PopupBox(app, box, (f32)R(0.9));
    Rect in = RectShrink(box, R(1));
    Draw_TextIn(dl, &app->fonts[Font_UiBold], CutTop(&in, R(2)), c->text, title, 0);
    Draw_TextIn(dl, &app->fonts[Font_Ui], CutTop(&in, R(3)), c->text, text, 0);
    Rect row = CutBottom(&in, R(2.4));
    Rect ok = CutRight(&row, R(7));
    CutRight(&row, R(0.6));
    Rect cancel = CutRight(&row, R(7));
    UiSignal cs = Ui_Hit(ui, Ui_Id("ConfirmCancel"), cancel);
    UiSignal os = Ui_Hit(ui, Ui_Id("ConfirmOk"), ok);
    Draw_RectEx(dl, cancel, cs.hovered ? c->hover : 0, c->line, 1, (f32)R(0.35), NULL, 0, 0, 0);
    Draw_TextIn(dl, &app->fonts[Font_Ui], cancel, c->text, S("Cancel"), 0.5f);
    u32 fill = danger ? c->danger : c->accent;
    Draw_Rect(dl, ok, os.hovered ? ColorMix(fill, 0xFFFFFFFFu, 0.12f) : fill, (f32)R(0.35));
    Draw_TextIn(dl, &app->fonts[Font_Ui], ok, danger ? 0xFFFFFFFFu : c->on_accent, ok_label, 0.5f);
    Named(app, S("cancel"), cancel);
    Named(app, ok_name, ok);
    s32 answer = 0;
    for (s64 i = 0; i < ui->in.event_count; i++) {
        if (!ui->in.events[i].down) continue;
        if (ui->in.events[i].key == Key_Enter || ui->in.events[i].key == Key_KeypadEnter) answer = 1;
        if (ui->in.events[i].key == Key_Escape) answer = -1;
    }
    if (os.clicked) answer = 1;
    if (cs.clicked) answer = -1;
    app->dl.fade = 0;
    return answer;
}

// A row of a popup menu: its label, and a key or note on the right. 1 when clicked.
static b32 MenuRow(App *app, Rect r, String label, String note)
{
    f32 em = app->em;
    u64 id = Ui_IdN(Ui_Id("MenuRow"), Ui_IdS(label));
    UiSignal s = Ui_Hit(&app->ui, id, r);
    Draw_Rect(&app->dl, r, HoverFill(app, id, s, app->c.hover), (f32)R(0.35));
    Rect in = r;
    CutLeft(&in, R(0.6));
    CutRight(&in, R(0.6));
    if (note.count) Draw_TextIn(&app->dl, &app->fonts[Font_UiSmall], CutRight(&in, R(8)), app->c.dim, note, 1);
    Draw_TextIn(&app->dl, &app->fonts[Font_Ui], in, app->c.text, label, 0);
    Named(app, Str_PushFormat(&app->frame_arena, "menu %.*s", STR_ARG(label)), r);
    return s.clicked;
}

// --- terminal input encoding ---------------------------------------------------------------------------------------

// The bytes a terminal sends for a key that doesn't arrive as typed text (arrows, function keys, ...), as Windows
// Terminal sends them; empty when the key is plain text.
static String SpecialKey(Arena *a, const UiKeyEvent *e, b32 app_cursor)
{
    b32 shift = (e->modifiers & Mod_Shift) != 0, ctrl = (e->modifiers & Mod_Ctrl) != 0, alt = (e->modifiers & Mod_Alt) != 0;
    s32 mod = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    char final = 0;
    s32 tilde = 0;
    switch (e->key) {
    case Key_Up: final = 'A'; break;
    case Key_Down: final = 'B'; break;
    case Key_Right: final = 'C'; break;
    case Key_Left: final = 'D'; break;
    case Key_Home: final = 'H'; break;
    case Key_End: final = 'F'; break;
    case Key_Insert: tilde = 2; break;
    case Key_Delete: tilde = 3; break;
    case Key_PageUp: tilde = 5; break;
    case Key_PageDown: tilde = 6; break;
    case Key_Tab: return shift ? S("\x1b[Z") : S("\t");
    case Key_Enter: case Key_KeypadEnter: return (shift || alt) ? S("\x1b\r") : S("\r");   // Shift+Enter: a newline in agents' prompts
    case Key_Backspace: return ctrl ? S("\x08") : alt ? S("\x1b\x7f") : S("\x7f");
    case Key_Escape: return S("\x1b");
    case Key_Space: return ctrl ? (String){ (u8 *)"\0", 1 } : (String){0};
    default:
        if (e->key >= Key_F1 && e->key < Key_F1 + 12) {
            s32 f = e->key - Key_F1;
            if (f < 4) final = (char)('P' + f);
            else { static const s32 codes[8] = { 15, 17, 18, 19, 20, 21, 23, 24 }; tilde = codes[f - 4]; }
            break;
        }
        return (String){0};
    }
    if (final >= 'P' && final <= 'S') return mod > 1 ? Str_PushFormat(a, "\x1b[1;%d%c", mod, final) : Str_PushFormat(a, "\x1bO%c", final);
    if (final) {
        if (mod > 1) return Str_PushFormat(a, "\x1b[1;%d%c", mod, final);
        return Str_PushFormat(a, app_cursor ? "\x1bO%c" : "\x1b[%c", final);
    }
    return mod > 1 ? Str_PushFormat(a, "\x1b[%d;%d~", tilde, mod) : Str_PushFormat(a, "\x1b[%d~", tilde);
}

// A mouse report for a program that asked for them (SGR, else the X10 bytes). x, y 1-based cells.
static String MouseBytes(Arena *a, const TermGrid *g, s32 button, s32 x, s32 y, b32 release, u8 mods)
{
    if (mods & Mod_Shift) button |= 4;
    if (mods & Mod_Alt) button |= 8;
    if (mods & Mod_Ctrl) button |= 16;
    if (g->flags & TermGrid_MouseSgr) return Str_PushFormat(a, "\x1b[<%d;%d;%d%c", button, x, y, release ? 'm' : 'M');
    u8 *b = (u8 *)Arena_Push(a, 6, 1);
    b[0] = 0x1b; b[1] = '['; b[2] = 'M';
    b[3] = (u8)(32 + (release ? 3 : button));
    b[4] = (u8)Min(255, 32 + x);
    b[5] = (u8)Min(255, 32 + y);
    return (String){ b, 6 };
}

// The selection's start and end in reading order.
static void SelectionRange(const Selection *s, s32 *sx, s32 *sy, s32 *ex, s32 *ey)
{
    b32 forward = s->ay < s->by || (s->ay == s->by && s->ax <= s->bx);
    *sx = forward ? s->ax : s->bx;
    *sy = forward ? s->ay : s->by;
    *ex = forward ? s->bx : s->ax;
    *ey = forward ? s->by : s->ay;
}

// Text copied from an agent's tab without its screen's furniture: the marks claude puts in front of its output (a bullet,
// a tree corner) become spaces, the indent the lines share goes, and a line it wrapped near the screen's edge joins the
// next (when it ends in a word or punctuation and the next starts a word).
static String CleanAgentCopy(Arena *a, String text, s32 cols)
{
    s64 cap = 0;
    for (s64 i = 0; i < text.count; i++) cap += text.data[i] == '\n';
    String *lines = Arena_PushArray(a, String, cap + 1);
    s32 *widths = Arena_PushArray(a, s32, cap + 1);
    s64 n = 0;
    for (s64 at = 0; at <= text.count;) {
        s64 end = at;
        while (end < text.count && text.data[end] != '\n') end++;
        String l = { text.data + at, end - at };
        if (l.count && l.data[l.count - 1] == '\r') l.count--;
        // the marks as spaces (they're one cell each)
        TextBuilder tb = { a, NULL, 0, 0 };
        s32 w = 0;
        for (s64 k = 0; k < l.count;) {
            s64 from = k;
            u32 cp = Utf8_Decode(l, &k);
            b32 lead = 1;
            for (s64 j = 0; j < from; j++) lead &= l.data[j] == ' ';
            if (lead && (cp == 0x25CF || cp == 0x23BF)) TB_Append(&tb, S(" "));
            else TB_Append(&tb, (String){ l.data + from, k - from });
            w++;
        }
        lines[n] = TB_String(&tb);
        while (lines[n].count && lines[n].data[lines[n].count - 1] == ' ') lines[n].count--;
        widths[n++] = w;
        at = end + 1;
    }
    // the indent they share
    s64 common = -1;
    for (s64 i = 0; i < n; i++) {
        s64 k = 0;
        while (k < lines[i].count && lines[i].data[k] == ' ') k++;
        if (k < lines[i].count && (common < 0 || k < common)) common = k;
    }
    TextBuilder out = { a, NULL, 0, 0 };
    for (s64 i = 0; i < n; i++) {
        String l = lines[i];
        s64 cut = Min(Max(common, (s64)0), l.count);
        l.data += cut; l.count -= cut;
        TB_Append(&out, l);
        if (i + 1 >= n) break;
        String next = lines[i + 1];
        while (next.count && next.data[0] == ' ') { next.data++; next.count--; }
        u8 last = l.count ? l.data[l.count - 1] : 0;
        b32 join = widths[i] >= cols - 12 && next.count && l.count &&
                   ((last >= 'a' && last <= 'z') || (last >= 'A' && last <= 'Z') || (last >= '0' && last <= '9') || strchr(",.;:)'\"", last)) &&
                   ((next.data[0] >= 'a' && next.data[0] <= 'z') || (next.data[0] >= 'A' && next.data[0] <= 'Z'));
        if (join) { TB_Append(&out, S(" ")); lines[i + 1] = next; continue; }
        TB_Append(&out, S("\n"));
    }
    return TB_String(&out);
}

static String SelectionText(Arena *a, const Selection *s, const TermGrid *g)
{
    s32 sx = 0, sy = 0, ex = 0, ey = 0;
    SelectionRange(s, &sx, &sy, &ex, &ey);
    TextBuilder tb = { a, NULL, 0, 0 };
    for (s32 y = Max(sy, 0); y <= ey && y < g->rows; y++) {
        s32 x0 = y == sy ? sx : 0, x1 = y == ey ? ex : g->cols - 1;
        if (s->block) { x0 = Min(s->ax, s->bx); x1 = Max(s->ax, s->bx); }
        s64 last_ink = tb.count;
        for (s32 x = Max(x0, 0); x <= x1 && x < g->cols; x++) {
            const TermCell *cell = &g->cells[(s64)y * g->cols + x];
            if (cell->flags & TermCell_WideCont) continue;
            u8 u[4];
            s64 n = Utf8_Encode(u, cell->codepoint >= 0x20 ? cell->codepoint : ' ');
            TB_Append(&tb, (String){ u, n });
            if (cell->codepoint > ' ') last_ink = tb.count;
        }
        b32 wrapped = !s->block && (g->cells[(s64)y * g->cols + g->cols - 1].flags & TermCell_Wrapped) != 0;
        if (!wrapped) tb.count = last_ink;   // trailing blanks dropped
        if (y < ey && !wrapped) TB_Append(&tb, S("\r\n"));   // a row that wrapped goes on in the next: one line
    }
    return TB_String(&tb);
}

// The word under cell (x, y) as a link or path would run (stops at blanks, quotes, brackets and pipes; trailing
// punctuation dropped), else empty.
static String LinkAt(Arena *a, const TermGrid *g, s32 x, s32 y, s32 *first_x)
{
    if (y < 0 || y >= g->rows || x < 0 || x >= g->cols) return (String){0};
    const TermCell *row = g->cells + (s64)y * g->cols;
#define LINK_STOP(cp) ((cp) <= ' ' || ((cp) < 0x80 && strchr("<>\"'`|", (int)(cp))))
    s32 x0 = x, x1 = x;
    while (x0 > 0 && !LINK_STOP(row[x0 - 1].codepoint)) x0--;
    while (x1 < g->cols - 1 && !LINK_STOP(row[x1 + 1].codepoint)) x1++;
    *first_x = x0;
    TextBuilder tb = { a, NULL, 0, 0 };
    for (s32 i = x0; i <= x1; i++) {
        if ((row[i].flags & TermCell_WideCont) || LINK_STOP(row[i].codepoint)) continue;
        u8 u[4];
        TB_Append(&tb, (String){ u, Utf8_Encode(u, row[i].codepoint) });
    }
    // a word that runs to the end of a row that wrapped goes on in the next rows (a long address)
    for (s32 ny = y; x1 == g->cols - 1 && (g->cells[(s64)ny * g->cols + g->cols - 1].flags & TermCell_Wrapped) && ny + 1 < g->rows; ny++) {
        const TermCell *next = g->cells + (s64)(ny + 1) * g->cols;
        for (x1 = 0; x1 < g->cols && !LINK_STOP(next[x1].codepoint); x1++) {
            if (next[x1].flags & TermCell_WideCont) continue;
            u8 u[4];
            TB_Append(&tb, (String){ u, Utf8_Encode(u, next[x1].codepoint) });
        }
        x1 = x1 == g->cols ? g->cols - 1 : 0;
    }
#undef LINK_STOP
    String link = TB_String(&tb);
    while (link.count && strchr(".,;:!?]}", link.data[link.count - 1])) link.count--;
    if (link.count && link.data[link.count - 1] == ')' && !memchr(link.data, '(', (size_t)link.count)) link.count--;
    return link;
}

// --- lifetime ------------------------------------------------------------------------------------------------------

static String CopyInto(u8 *buf, s64 cap, String s)
{
    s64 n = Min(s.count, cap);
    memcpy(buf, s.data, (size_t)n);
    return (String){ buf, n };
}

// settings.txt's part of AppInit: at start, and again after "Reset to moss's defaults" (AppFrameInput.settings)
static void ApplySettings(App *app, const AppInit *init)
{
    app->term_font_pt = init->font_size_pt > 0 ? init->font_size_pt : 12;
    app->opacity = init->opacity > 0 ? Clamp(init->opacity, 0.2f, 1.0f) : 1.0f;
    app->agent_command = CopyInto(app->agent_buf, sizeof(app->agent_buf), init->agent_command);
    app->agents = CopyInto(app->agents_buf, sizeof(app->agents_buf), init->agents);
    app->term_font = CopyInto(app->term_font_buf, sizeof(app->term_font_buf), init->term_font);
    app->hide_hidden = init->hide_hidden;
    app->folder_sizes_off = init->folder_sizes_off;
    app->notifications_off = init->notifications_off;
    app->vs_env_off = init->vs_env_off;
    app->backdrop = init->backdrop;
    app->background_strength = init->background_strength < 0 ? 0 : init->background_strength ? Min(init->background_strength, 100) : 30;   // -1: off
    app->cursor = init->cursor;
    app->cursor_blink = init->cursor_blink;
    app->ligatures = init->ligatures;
    app->copy_on_select_off = init->copy_on_select_off;
    app->dim_unfocused_off = init->dim_unfocused_off;
    app->paste_warning_off = init->paste_warning_off;
    app->win32_input_off = init->win32_input_off;
    // --- key bindings: the defaults, then settings.txt's `key.<command> = <keys>` lines ("ctrl+shift+d", "none") ---
    for (u32 c = 0; c < Cmd_Count; c++) { app->bind_key[c] = g_Commands[c].key; app->bind_mods[c] = g_Commands[c].mods; }
    for (s64 at = 0; at < init->settings_text.count;) {
        s64 end = at;
        while (end < init->settings_text.count && init->settings_text.data[end] != '\n') end++;
        String line = { init->settings_text.data + at, end - at };
        at = end + 1;
        s64 eq = 0;
        while (eq < line.count && line.data[eq] != '=') eq++;
        if (line.count < 5 || memcmp(line.data, "key.", 4) != 0 || eq >= line.count) continue;
        String name = { line.data + 4, eq - 4 }, keys = { line.data + eq + 1, line.count - eq - 1 };
        while (name.count && name.data[name.count - 1] == ' ') name.count--;
        while (keys.count && keys.data[0] == ' ') { keys.data++; keys.count--; }
        while (keys.count && strchr(" \r\t", keys.data[keys.count - 1])) keys.count--;
        u32 cmd = 0;
        for (u32 c = 1; c < Cmd_Count; c++) if (Str_EqualNoCase(name, Str_FromCStr(g_Commands[c].name))) cmd = c;
        u8 key = 0, mods = 0;
        b32 bad = !cmd;
        while (keys.count && !bad && !Str_EqualNoCase(keys, S("none"))) {
            s64 plus = 1;
            while (plus < keys.count && keys.data[plus] != '+') plus++;
            String part = { keys.data, plus };
            if (Str_EqualNoCase(part, S("ctrl"))) mods |= Mod_Ctrl;
            else if (Str_EqualNoCase(part, S("alt"))) mods |= Mod_Alt;
            else if (Str_EqualNoCase(part, S("shift"))) mods |= Mod_Shift;
            else if (part.count == 1 && isalpha(part.data[0])) key = (u8)(Key_A + toupper(part.data[0]) - 'A');
            else if (part.count == 1 && isdigit(part.data[0])) key = (u8)(Key_0 + part.data[0] - '0');
            else if (part.count >= 2 && part.count <= 3 && (part.data[0] == 'f' || part.data[0] == 'F') && isdigit(part.data[1])) {
                s32 n = part.data[1] - '0';
                if (part.count == 3) n = n * 10 + part.data[2] - '0';
                key = n >= 1 && n <= 24 ? (u8)(Key_F1 + n - 1) : 0;
            } else {
                u8 found = 0;
                for (s32 i = 0; i < (s32)ArrayCount(g_KeyNames); i++) if (Str_EqualNoCase(part, Str_FromCStr(g_KeyNames[i].name))) found = g_KeyNames[i].key;
                if (found) key = found; else bad = 1;
            }
            keys.data += Min(plus + 1, keys.count);
            keys.count -= Min(plus + 1, keys.count);
        }
        if (bad) { Logf("app", "settings: no such key binding: %.*s", STR_ARG(line)); continue; }
        app->bind_key[cmd] = key;
        app->bind_mods[cmd] = key ? mods : 0;
    }
    app->term_theme = 0;   // Moss unless settings.txt names another
    for (s32 i = 0; i < TermTheme_Count; i++) {
        if (init->settings_theme.count && Str_EqualNoCase(init->settings_theme, Str_FromCStr(g_TermThemes[i].name))) app->term_theme = i;
    }
}

App *App_Create(const AppInit *init)
{
    Arena boot = {0};
    App *app = Arena_PushStruct(&boot, App);
    app->arena = boot;
    app->headless = init->headless;
    app->window_name = CopyInto(app->window_name_buf, sizeof(app->window_name_buf), init->window_name);
    app->explorer_menu = init->explorer_menu;
    app->start_with_windows = init->start_with_windows;
    app->good_programmer = init->good_programmer;
    app->hist_current = -1;
    ApplySettings(app, init);
    app->account_count = 1;   // the default account, until accounts.txt is read
    app->accounts[0].name = S("default");
    app->accounts[0].theme = -1;
    // --- the go-to box's: the environment, the known folders, recents and bookmarks (paths.txt) ---
    app->environment = Str_PushCopy(&app->arena, init->environment);
    app->place_count = Min(init->place_count, (s32)ArrayCount(app->places));
    for (s32 i = 0; i < app->place_count; i++) {
        app->places[i].name = Str_PushCopy(&app->arena, init->places[i].name);
        app->places[i].path = Str_PushCopy(&app->arena, init->places[i].path);
    }
    for (s64 at = 0; at < init->paths_text.count;) {
        s64 end = at;
        while (end < init->paths_text.count && init->paths_text.data[end] != '\n') end++;
        String line = { init->paths_text.data + at, end - at };
        at = end + 1;
        if (line.count && line.data[line.count - 1] == '\r') line.count--;
        b32 recent = line.count > 7 && memcmp(line.data, "recent ", 7) == 0, mark = line.count > 9 && memcmp(line.data, "bookmark ", 9) == 0;
        String path = recent ? (String){ line.data + 7, line.count - 7 } : (String){ line.data + 9, line.count - 9 };
        if (recent && app->recent_count < 32 && path.count < 520) {
            memcpy(app->recent[app->recent_count], path.data, (size_t)path.count);
            app->recent_len[app->recent_count++] = (s32)path.count;
        }
        if (mark && app->bookmark_count < 32 && path.count < 520) {
            memcpy(app->bookmark[app->bookmark_count], path.data, (size_t)path.count);
            app->bookmark_len[app->bookmark_count++] = (s32)path.count;
        }
    }
    app->theme_shown = app->term_theme;   // the first frame shows it at once

    // --- the saved layout: panes whose tabs find their server tabs by lasting id ---
    SessionLayout s = {0};
    if (init->session_text.count && Session_Read(&app->panes, init->session_text, &s)) {
        app->restore = s;
        app->sidebar_open = !s.sidebar_closed;
        app->placement_pending = s.w > 0 && s.h > 0;
        Logf("app", "layout restored: %d tabs", Panes_TabCount(&app->panes));
    } else {
        app->panes.root = Panes_NewNode(&app->panes);
        app->sidebar_open = 1;   // Claude Code's sessions, there from the start
    }
    return app;
}

static void FreeBrowserMemory(Browser *b)
{
    Arena_Release(&b->list_arena);
    Arena_Release(&b->view_arena);
    Arena_Release(&b->sel_arena);
    Arena_Release(&b->size_arena);
}

void App_Destroy(App *app)
{
    for (Browser *b = app->free_browsers; b; b = b->free_next) FreeBrowserMemory(b);
    for (PaneNode *n = app->panes.root; n; n = Panes_NextNode(n)) {
        for (PaneTab *t = n->first_tab; t; t = t->next) if (t->browser) FreeBrowserMemory(t->browser);
    }
    Arena_Release(&app->frame_arena);
    Arena_Release(&app->session_arena);
    Arena_Release(&app->paste_arena);
    Arena_Release(&app->hist_arena);
    Arena_Release(&app->ui.hit_arenas[0]);
    Arena_Release(&app->ui.hit_arenas[1]);
    Arena_Release(&app->panes.arena);
    Arena arena = app->arena;
    Arena_Release(&arena);
}

static String BuildSession(App *app, Arena *a)
{
    SessionLayout s = {0};
    s.x = app->window_rect[0];
    s.y = app->window_rect[1];
    s.w = app->window_rect[2];
    s.h = app->window_rect[3];
    s.maximized = app->maximized;
    s.sidebar_closed = !app->sidebar_open;
    return Session_Write(a, &app->panes, &s);
}

String App_SessionText(App *app)
{
    String text = BuildSession(app, &app->frame_arena);
    return Str_Equal(text, app->session_saved) ? (String){0} : text;
}

// --- a frame's requests ------------------------------------------------------------------------------------------

typedef struct Frame {
    const AppFrameInput *in;
    AppOutput *out;
    Arena *fa;
    Array(TermInput) inputs;
    Array(TermNewTab) new_tabs;
    Array(u32) closes;
    Array(TermScroll) scrolls;
    Array(FileRequest) file_requests;
    Array(FileWatch) file_watches;
    Array(FileOp) file_ops;
    // a browser move asked by a click: done after the frame is drawn (the rows' text points into the listing)
    PaneTab *nav_tab;
    b32 nav_up;
    s32 nav_history;            // -1 Back, +1 Forward
    String nav_path, nav_select;
    s32 nav_open;               // the entry to open (-1 none)
    s32 pane_cols, pane_rows;   // the focused pane's terminal size (new tabs start at it)
} Frame;

static void TermSend(Frame *f, u32 term_id, String bytes)
{
    if (!term_id || !bytes.count) return;
    // one input per tab per frame: keys typed in a burst go together
    for (s64 i = 0; i < f->inputs.count; i++) {
        TermInput *t = &f->inputs.data[i];
        if (t->tab_id != term_id) continue;
        t->bytes = Str_PushFormat(f->fa, "%.*s%.*s", STR_ARG(t->bytes), STR_ARG(bytes));
        return;
    }
    TermInput *t = Array_Push(f->fa, f->inputs);
    t->tab_id = term_id;
    t->bytes = Str_PushCopy(f->fa, bytes);
}

static void TermScrollBy(Frame *f, u32 term_id, s32 lines)
{
    TermScroll *s = Array_Push(f->fa, f->scrolls);
    s->tab_id = term_id;
    s->lines = lines;
}

static void SetSetting(AppOutput *out, String key, String value)
{
    if (out->setting_count >= (s32)ArrayCount(out->setting_keys)) return;
    out->setting_keys[out->setting_count] = key;
    out->setting_values[out->setting_count++] = value;
}

// A new terminal tab in `leaf` (a shell, or the agent), asked of the server; it shows "starting" until the server
// lists it.
// The account a claude config folder belongs to (empty = the default account's): its index, -1 = not in accounts.txt.
static s32 AccountIndex(App *app, String dir)
{
    if (!dir.count) return 0;
    for (s32 i = 1; i < app->account_count; i++) if (Str_EqualNoCase(app->accounts[i].dir, dir)) return i;
    return -1;
}

static PaneTab *RequestTab(App *app, Frame *f, PaneNode *leaf, u32 kind, String cwd)
{
    PaneTab *t = Panes_NewTab(&app->panes, leaf, NULL);
    t->request = ++app->next_request;
    t->created_frame = app->ui.frame;
    Panes_Select(&app->panes, t);
    TermNewTab *n = Array_Push(f->fa, f->new_tabs);
    n->request = t->request;
    n->kind = kind;
    n->cols = Max(f->pane_cols, 20);
    n->rows = Max(f->pane_rows, 5);
    n->cwd = cwd;
    if (kind == TermNew_Agent) n->command = app->agent_command;
    Logf("app", "new tab requested (#%u, %s)", t->request, kind == TermNew_Shell ? "shell" : kind == TermNew_Claude ? "claude" : "agent");
    return t;
}

static String FocusedCwd(App *app);

// The sidebar's note on a session (sessions.txt): its index, -1 when it has none (add: a new, empty one).
static s32 SessionMetaIndex(App *app, String id, b32 add)
{
    for (s32 i = 0; i < app->side_meta_count; i++) if (Str_Equal((String){ app->side_meta[i].id, app->side_meta[i].id_len }, id)) return i;
    if (!add || app->side_meta_count >= (s32)ArrayCount(app->side_meta) || id.count > (s64)sizeof(app->side_meta[0].id)) return -1;
    s32 i = app->side_meta_count++;
    memset(&app->side_meta[i], 0, sizeof(app->side_meta[i]));
    memcpy(app->side_meta[i].id, id.data, (size_t)id.count);
    app->side_meta[i].id_len = (s32)id.count;
    return i;
}

// sessions.txt as it is now: "name <id> = <name>", "pin <id>", "archive <id>" lines.
static String SessionMetaText(App *app, Arena *a)
{
    TextBuilder tb = { a, NULL, 0, 0 };
    TB_Append(&tb, S("# moss: the sessions sidebar's names, pins and archived sessions\r\n"));
    for (s32 i = 0; i < app->side_meta_count; i++) {
        String id = { app->side_meta[i].id, app->side_meta[i].id_len };
        if (app->side_meta[i].name_len) TB_Format(&tb, "name %.*s = %.*s\r\n", STR_ARG(id), app->side_meta[i].name_len, (const char *)app->side_meta[i].name);
        if (app->side_meta[i].pin) TB_Format(&tb, "pin %.*s\r\n", STR_ARG(id));
        if (app->side_meta[i].archive) TB_Format(&tb, "archive %.*s\r\n", STR_ARG(id));
    }
    return TB_String(&tb);
}

// A new claude tab in the focused pane, at its folder, under an account (its config folder; empty = the default).
static void RequestClaudeTab(App *app, Frame *f, String config_dir)
{
    PaneTab *t = app->panes.selected;
    RequestTab(app, f, t ? t->node : app->panes.root, TermNew_Claude, FocusedCwd(app));
    f->new_tabs.data[f->new_tabs.count - 1].config_dir = config_dir;
}

// The text a paste types into a terminal: line ends as Enter (\r), bracketed when the program asked.
static String PasteBytes(Arena *a, String text, b32 bracketed)
{
    TextBuilder tb = { a, NULL, 0, 0 };
    if (bracketed) TB_Append(&tb, S("\x1b[200~"));
    for (s64 i = 0; i < text.count; i++) {
        u8 ch = text.data[i];
        if (ch == '\r' && i + 1 < text.count && text.data[i + 1] == '\n') continue;
        if (ch == '\n') ch = '\r';
        TB_Append(&tb, (String){ &ch, 1 });
    }
    if (bracketed) TB_Append(&tb, S("\x1b[201~"));
    return TB_String(&tb);
}

// ASCII case-insensitive substring.
static b32 ContainsNoCase(String hay, String needle)
{
    for (s64 i = 0; i + needle.count <= hay.count; i++) if (Str_EqualNoCase((String){ hay.data + i, needle.count }, needle)) return 1;
    return 0;
}

// How well one term matches text, its characters in order (case not counting), -1 when it doesn't: up for consecutive
// characters, word starts (after a space or punctuation, or a lower-to-upper hump) and an early start; down a point
// per character skipped. The best alignment over the whole text (a small table: text position x term character).
static s32 FuzzyTerm(String text, String term)
{
    enum { Longest = 256, Nothing = -1000000 };
    s64 n = Min(text.count, (s64)Longest), m = term.count;
    if (!m || m > n) return m ? -1 : 0;
    s32 prev[Longest], cur[Longest];
    for (s64 i = 0; i < m; i++) {
        s32 run = Nothing;   // the best prev[k] + k for k <= j - 2 (a match after a gap)
        for (s64 j = 0; j < n; j++) {
            cur[j] = Nothing;
            u8 a = text.data[j], b = term.data[i];
            if (tolower(a) == tolower(b)) {
                u8 before = j ? text.data[j - 1] : ' ';
                b32 start = strchr(" _-./\\:()[]", before) || (islower(before) && isupper(a));
                s32 bonus = 10 + (start ? 12 : 0);
                if (i == 0) cur[j] = bonus - (s32)Min(j, (s64)12) + (j == 0 ? 8 : 0);
                else {
                    s32 s = Nothing;
                    if (j && prev[j - 1] > Nothing) s = prev[j - 1] + 15;
                    if (run > Nothing) s = Max(s, run - (s32)j + 1);
                    if (s > Nothing) cur[j] = s + bonus;
                }
            }
            if (i && j && prev[j - 1] > Nothing) run = Max(run, prev[j - 1] + (s32)(j - 1));
        }
        memcpy(prev, cur, sizeof(s32) * (size_t)n);
    }
    s32 best = Nothing;
    for (s64 j = 0; j < n; j++) best = Max(best, prev[j]);
    return best > Nothing ? Max(best, 0) : -1;
}

// The palette's ranking: every space-separated term must match (FuzzyTerm); the sum of their scores, -1 when one
// doesn't.
static s32 FuzzyScore(String text, String query)
{
    s32 total = 0;
    for (s64 q = 0; q < query.count;) {
        while (q < query.count && query.data[q] == ' ') q++;
        s64 from = q;
        while (q < query.count && query.data[q] != ' ') q++;
        if (q == from) break;
        s32 s = FuzzyTerm(text, (String){ query.data + from, q - from });
        if (s < 0) return -1;
        total += s;
    }
    return total;
}

// The browser's filter: the typed characters in the name in order, not necessarily together ("ix" finds index.html),
// case not counting. 1 when they are; marks (when given) gets the codepoint indices they matched at.
static b32 FilterMatch(String name, String filter, u32 *marks)
{
    s64 at = 0, index = 0, f = 0;
    u32 want = 0;
    if (filter.count) want = Utf8_Decode(filter, &f);
    while (filter.count && at < name.count) {
        u32 cp = Utf8_Decode(name, &at);
        u32 a = cp < 128 ? (u32)tolower((int)cp) : cp, b = want < 128 ? (u32)tolower((int)want) : want;
        if (a == b) {
            if (marks) *marks++ = (u32)index;
            if (f >= filter.count) return 1;
            want = Utf8_Decode(filter, &f);
        }
        index++;
    }
    return !filter.count;
}

// Days since 1970-01-01 -> year, month (1-12), day (Howard Hinnant's civil_from_days).
static void CivilFromDays(s64 z, s32 *y, s32 *m, s32 *d)
{
    z += 719468;
    s64 era = (z >= 0 ? z : z - 146096) / 146097;
    s64 doe = z - era * 146097;
    s64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    s64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    s64 mp = (5 * doy + 2) / 153;
    *d = (s32)(doy - (153 * mp + 2) / 5 + 1);
    *m = (s32)(mp < 10 ? mp + 3 : mp - 9);
    *y = (s32)(yoe + era * 400 + (*m <= 2));
}

static const char *const g_Months[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

// --- the file browser --------------------------------------------------------------------------------------------

// Busy: a program runs in the shell, or an agent session is open (even idle). The browser is read-only then: it
// looks anywhere, the shell doesn't follow, nothing changes the disk.
static b32 TabBusy(const TermTab *st)
{
    return st && (st->flags & (TermTab_Running | TermTab_Agent | TermTab_Busy));
}

static String BusyReason(Arena *a, const TermTab *st)
{
    if (st->flags & TermTab_Agent) return Str_PushFormat(a, "%.*s is open", STR_ARG(st->agent.count ? st->agent : S("an agent")));
    return Str_PushFormat(a, "%.*s is running", STR_ARG(TabLabel(st)));
}

// A folder as the browser keeps it: no trailing separator ("C:\" -> "C:"); "" is This PC.
static String BrowsePath(String path)
{
    while (path.count > 0 && (path.data[path.count - 1] == '\\' || path.data[path.count - 1] == '/')) path.count--;
    return path;
}

// A browser folder as Windows takes it: a drive root keeps its separator ("C:" alone is C:'s current folder).
static String ShellPath(Arena *a, String path)
{
    return path.count && path.data[path.count - 1] == ':' ? Str_PushFormat(a, "%.*s\\", STR_ARG(path)) : path;
}

static FileEntry *BrowserEntry(const Browser *b, s64 row)
{
    if (row < 0 || row >= b->view_count) return NULL;
    return &b->entries[b->view[row]];
}

static String EntryPath(Arena *a, const Browser *b, const FileEntry *e)
{
    if (!b->path.count) return Str_PushCopy(a, e->name);
    return Str_PushFormat(a, "%.*s\\%.*s", STR_ARG(b->path), STR_ARG(e->name));
}

static void BrowserNotice(Browser *b, u64 now, String text)
{
    b->notice_len = (s32)CopyInto(b->notice, sizeof(b->notice), text).count;
    b->notice_until_us = now + 4000000;
}

// Lists `path` in t's browser, selecting `select` once it's there. Moving while the shell is idle makes the shell
// follow when the browser closes; while it's busy the browser looks on its own.
static void Browse(App *app, PaneTab *t, String path, String select)
{
    Browser *b = t->browser;
    path = Str_PushCopy(&app->frame_arena, BrowsePath(path));   // both may point into this browser
    select = Str_PushCopy(&app->frame_arena, select);
    // --- history: where it was goes on Back's list (Forward's is dropped), unless this is Back / Forward itself ---
    if (!b->from_history && (b->listed || b->request) && !Str_EqualNoCase(path, b->path) && b->path.count < 520) {
        if (b->back_count == 16) {
            memmove(b->back[0], b->back[1], sizeof(b->back[0]) * 15);
            memmove(b->back_len, b->back_len + 1, sizeof(b->back_len[0]) * 15);
            b->back_count--;
        }
        memcpy(b->back[b->back_count], b->path.data, (size_t)b->path.count);
        b->back_len[b->back_count++] = (s32)b->path.count;
        b->forward_count = 0;
    }
    b->from_history = 0;
    // --- a recent folder: to the front of the recents (once), at most 32 ---
    if (path.count && path.count < 520) {
        s32 at = 0;
        while (at < app->recent_count && !Str_EqualNoCase((String){ app->recent[at], app->recent_len[at] }, path)) at++;
        if (at == app->recent_count) { app->recent_count = Min(app->recent_count + 1, 32); at = app->recent_count - 1; }   // full: the oldest goes
        if (at > 0 || app->recent_count == 1) {
            memmove(app->recent[1], app->recent[0], sizeof(app->recent[0]) * (size_t)at);
            memmove(app->recent_len + 1, app->recent_len, sizeof(app->recent_len[0]) * (size_t)at);
            memcpy(app->recent[0], path.data, (size_t)path.count);
            app->recent_len[0] = (s32)path.count;
            app->paths_dirty = 1;
        }
    }
    b->path = CopyInto(b->path_buf, sizeof(b->path_buf), path);
    b->request = 0;
    b->listed = 0;
    b->stale = 0;
    b->error = 0;
    Arena_Clear(&b->list_arena);
    b->count = 0;
    b->entries = NULL;
    b->view_count = 0;
    b->view_dirty = 1;
    b->cursor = b->top = 0;
    b->filter_len = 0;
    b->cursor_name_len = 0;
    b->renaming = 0;
    b->sel.count = 0;
    Arena_Clear(&b->size_arena);
    memset(&b->sizes, 0, sizeof(b->sizes));
    b->anchor = 0;
    b->notice_len = 0;
    b->select_len = (s32)CopyInto(b->select, sizeof(b->select), select).count;
    t->browse_path = b->path;
    b->cd_pending = t->term_id && !TabBusy(FindServerTab(app, t->term_id));
}

// How much of a path is its root: "C:" (2), a network share "\\server\share" (all of that), else 0.
static s64 PathRoot(String path)
{
    if (path.count >= 2 && path.data[1] == ':') return 2;
    if (path.count < 3 || path.data[0] != '\\' || path.data[1] != '\\') return 0;
    s64 at = 2, slashes = 0;
    while (at < path.count && !(path.data[at] == '\\' && ++slashes == 2)) at++;
    return at;
}

static void BrowseUp(App *app, PaneTab *t)
{
    String path = t->browser->path;
    if (!path.count) return;
    s64 root = PathRoot(path);
    if (path.count <= root) {
        // a drive or a share: up is This PC (a drive's row selected there)
        Browse(app, t, (String){0}, path.data[0] == '\\' ? (String){0} : Str_PushFormat(&app->frame_arena, "%.*s\\", STR_ARG(path)));
        return;
    }
    s64 i = path.count;
    while (i > root && path.data[i - 1] != '\\') i--;
    String parent = { path.data, Max(i - 1, root) };
    Browse(app, t, parent, (String){ path.data + i, path.count - i });
}

// The go-to box (the palette's GoTo mode) for t's browser, `text` typed in it (the caret at its end).
static void OpenGoTo(App *app, PaneTab *t, String text)
{
    app->palette_open = 1;
    app->palette_mode = PaletteMode_GoTo;
    TextEdit_Set(&app->palette_edit, text);
    app->palette_edit.mark = app->palette_edit.cursor = app->palette_edit.count;
    app->palette_sel = 0;
    app->palette_top = 0;
    app->palette_filter_hash = 0;
    app->goto_tab = t ? t->id : 0;
    app->goto_dir.count = 0;   // listed again: it may have changed
    app->goto_listed = 0;
    app->goto_request = 0;
    app->goto_count = 0;
    app->drives_request = 0;   // the drives too (one may have come or gone)
}

// Bookmarks `path` (a browser's folder), or takes its bookmark away.
static void ToggleBookmark(App *app, String path)
{
    if (!path.count || path.count >= 520) return;
    s32 at = 0;
    while (at < app->bookmark_count && !Str_EqualNoCase((String){ app->bookmark[at], app->bookmark_len[at] }, path)) at++;
    if (at < app->bookmark_count) {
        memmove(app->bookmark[at], app->bookmark[at + 1], sizeof(app->bookmark[0]) * (size_t)(app->bookmark_count - at - 1));
        memmove(app->bookmark_len + at, app->bookmark_len + at + 1, sizeof(app->bookmark_len[0]) * (size_t)(app->bookmark_count - at - 1));
        app->bookmark_count--;
    } else if (app->bookmark_count < 32) {
        memcpy(app->bookmark[app->bookmark_count], path.data, (size_t)path.count);
        app->bookmark_len[app->bookmark_count++] = (s32)path.count;
    }
    app->paths_dirty = 1;
}

static b32 IsBookmarked(App *app, String path)
{
    for (s32 i = 0; i < app->bookmark_count; i++) if (Str_EqualNoCase((String){ app->bookmark[i], app->bookmark_len[i] }, path)) return 1;
    return 0;
}

// Back (dir -1) or Forward (+1) in t's browser history.
static void BrowseHistory(App *app, PaneTab *t, s32 dir)
{
    Browser *b = t->browser;
    if (!b || (dir < 0 ? !b->back_count : !b->forward_count) || b->path.count >= 520) return;
    u8 (*from)[520] = dir < 0 ? b->back : b->forward, (*to)[520] = dir < 0 ? b->forward : b->back;
    s32 *from_len = dir < 0 ? b->back_len : b->forward_len, *to_len = dir < 0 ? b->forward_len : b->back_len;
    s32 *from_count = dir < 0 ? &b->back_count : &b->forward_count, *to_count = dir < 0 ? &b->forward_count : &b->back_count;
    String there = Str_PushCopy(&app->frame_arena, (String){ from[*from_count - 1], from_len[*from_count - 1] });
    (*from_count)--;
    if (*to_count < 16) {
        memcpy(to[*to_count], b->path.data, (size_t)b->path.count);
        to_len[(*to_count)++] = (s32)b->path.count;
    }
    // the folder it leaves is selected when it comes back to its parent
    s64 cut = b->path.count;
    while (cut > 0 && b->path.data[cut - 1] != '\\') cut--;
    String select = Str_EqualNoCase((String){ b->path.data, Max(cut - 1, 0) }, there) ? Str_PushCopy(&app->frame_arena, (String){ b->path.data + cut, b->path.count - cut }) : (String){0};
    b->from_history = 1;
    Browse(app, t, there, select);
}

// Flips t to its browser: at the shell's folder when the shell moved since the browser was last open (the first
// time: the session's folder, else the shell's).
static void OpenBrowser(App *app, PaneTab *t)
{
    const TermTab *st = FindServerTab(app, t->term_id);
    String cwd = st ? BrowsePath(st->cwd) : (String){0};
    Browser *b = t->browser;
    if (!b) {
        b = app->free_browsers;
        if (b) app->free_browsers = b->free_next;
        else b = Arena_PushStruct(&app->arena, Browser);
        Arena list = b->list_arena, view = b->view_arena, sel = b->sel_arena, sized = b->size_arena;   // their memory is used again
        memset(b, 0, sizeof(*b));
        b->size_arena = sized;
        b->list_arena = list;
        b->view_arena = view;
        b->sel_arena = sel;
        Arena_Clear(&b->sel_arena);
        t->browser = b;
        Browse(app, t, t->browse_path.count ? t->browse_path : cwd, (String){0});
    } else if (cwd.count && !Str_EqualNoCase(cwd, (String){ b->opened_cwd, b->opened_cwd_len }) && !Str_EqualNoCase(cwd, b->path)) {
        Browse(app, t, cwd, (String){0});
    }
    if (cwd.count) b->opened_cwd_len = (s32)CopyInto(b->opened_cwd, sizeof(b->opened_cwd), cwd).count;
    b->cd_pending = 0;
    t->browsing = 1;
}

// Back to the shell, which follows to the browser's folder when it's still idle at its prompt.
static void CloseBrowser(App *app, Frame *f, PaneTab *t)
{
    Browser *b = t->browser;
    t->browsing = 0;
    if (!b || !b->cd_pending) return;
    b->cd_pending = 0;
    const TermTab *st = FindServerTab(app, t->term_id);
    if (!st || TabBusy(st) || (st->flags & TermTab_Exited) || !b->path.count || Str_EqualNoCase(BrowsePath(st->cwd), b->path)) return;
    // a folder below the shell's goes as a short relative cd
    String cwd = BrowsePath(st->cwd), to = b->path;
    b32 below = cwd.count && to.count > cwd.count + 1 && to.data[cwd.count] == '\\' && Str_EqualNoCase((String){ to.data, cwd.count }, cwd);
    if (below) TermSend(f, t->term_id, Str_PushFormat(f->fa, "cd \"%.*s\"\r", (int)(to.count - cwd.count - 1), (const char *)to.data + cwd.count + 1));
    else if (to.data[0] == '\\') TermSend(f, t->term_id, Str_PushFormat(f->fa, "pushd \"%.*s\"\r", STR_ARG(to)));   // cmd can't cd to a share: pushd maps it a drive letter
    else TermSend(f, t->term_id, Str_PushFormat(f->fa, "cd /d \"%.*s\"\r", STR_ARG(ShellPath(f->fa, to))));
    Logf("app", "shell %u follows the browser to %.*s", t->term_id, STR_ARG(b->path));
}

static void FreeBrowser(App *app, PaneTab *t)
{
    if (t->viewer) {
        // the tab's picture view goes with it
        t->viewer->free_next = app->free_viewers;
        app->free_viewers = t->viewer;
        t->viewer = NULL;
        t->viewing = 0;
    }
    Browser *b = t->browser;
    if (!b) return;
    b->free_next = app->free_browsers;
    app->free_browsers = b;
    t->browser = NULL;
}

// A file name's extension is one of `list` ("png jpg ..."), case not counting. No extension matches "".
static b32 HasExtension(String name, const char *list)
{
    s64 dot = name.count;
    while (dot > 0 && name.data[dot - 1] != '.' && name.data[dot - 1] != '\\') dot--;
    String ext = dot > 0 && name.data[dot - 1] == '.' ? (String){ name.data + dot, name.count - dot } : (String){0};
    if (!ext.count) return list[0] == ' ' || strstr(list, "  ") != NULL;   // "" is in the list as a double space
    for (const char *w = list; *w;) {
        while (*w == ' ') w++;
        const char *e = w;
        while (*e && *e != ' ') e++;
        if (e > w && Str_EqualNoCase(ext, (String){ (u8 *)w, e - w })) return 1;
        w = e;
    }
    return 0;
}

static const char *const g_PictureTypes = "png jpg jpeg jfif gif bmp webp tif tiff ico heic heif avif dds";
static const char *const g_TextTypes =
    "txt md markdown rst log csv tsv json jsonc json5 yaml yml toml ini cfg conf config env properties xml html htm css scss "
    "less js mjs cjs ts tsx jsx vue svelte c h cc cpp cxx hpp hh hxx inl cs java kt kts go rs py pyw rb lua php pl sh bash zsh "
    "fish ps1 psm1 bat cmd sql gradle cmake mk makefile dockerfile gitignore gitattributes editorconfig lock tex bib swift m mm "
    "zig nim dart r jl hlsl glsl shader vert frag ex exs erl hs ml fs clj scala groovy diff patch  ";   // "  ": no extension

// The picture view over t: `path` is a picture (its folder's others a key away) or a folder (its first picture).
// from_browser: Esc goes back to the browser, else to the shell.
static void OpenViewer(App *app, PaneTab *t, String path, b32 from_browser)
{
    Viewer *v = t->viewer;
    if (!v) {
        v = app->free_viewers;
        if (v) app->free_viewers = v->free_next;
        else v = Arena_PushStruct(&app->arena, Viewer);
        Arena keep = v->arena;
        memset(v, 0, sizeof(*v));
        v->arena = keep;
        t->viewer = v;
    }
    path = BrowsePath(path);
    b32 picture = HasExtension(path, g_PictureTypes);
    s64 cut = path.count;
    while (picture && cut > 0 && path.data[cut - 1] != '\\') cut--;
    v->folder = CopyInto(v->folder_buf, sizeof(v->folder_buf), picture ? (String){ path.data, Max(cut - 1, 0) } : path);
    v->name = CopyInto(v->name_buf, sizeof(v->name_buf), picture ? (String){ path.data + cut, path.count - cut } : (String){0});
    v->request = 0;
    v->listed = 0;
    v->count = 0;
    Arena_Clear(&v->arena);
    v->from_browser = from_browser;
    t->viewing = 1;
    Logf("app", "view %.*s", STR_ARG(path));
}

// Opens a file the way term does: a picture in the tab's picture view, a text file in the terminal editor in the tab
// (its shell when idle at its prompt, else a new tab next to it), anything else with its own program.
static void OpenInTab(App *app, Frame *f, PaneTab *t, String path)
{
    if (HasExtension(path, g_PictureTypes)) { OpenViewer(app, t, path, t->browsing); return; }
    if (f->in->editor.count && HasExtension(path, g_TextTypes)) {
        String command = Str_PushFormat(f->fa, "%.*s \"%.*s\"", STR_ARG(f->in->editor), STR_ARG(path));
        const TermTab *st = FindServerTab(app, t->term_id);
        if (t->term_id && st && !TabBusy(st) && !(st->flags & TermTab_Exited)) {
            if (t->browsing) CloseBrowser(app, f, t);
            TermSend(f, t->term_id, Str_PushFormat(f->fa, "%.*s\r", STR_ARG(command)));
        } else {
            // the tab is busy: a new one beside it, in the file's folder, running the editor
            s64 cut = path.count;
            while (cut > 0 && path.data[cut - 1] != '\\') cut--;
            RequestTab(app, f, t->node, TermNew_Shell, Str_PushCopy(f->fa, (String){ path.data, Max(cut - 1, 0) }));
            f->new_tabs.data[f->new_tabs.count - 1].command = command;
        }
        Logf("app", "edit %.*s with %.*s", STR_ARG(path), STR_ARG(f->in->editor));
        return;
    }
    f->out->open_file = path;
}

// Folders open in the browser, files as OpenInTab does.
static void BrowserOpen(App *app, Frame *f, PaneTab *t, const FileEntry *e)
{
    String path = EntryPath(f->fa, t->browser, e);
    if (e->flags & File_Dir) Browse(app, t, path, (String){0});
    else OpenInTab(app, f, t, path);
}

// --- the browser's selection: a sorted set of name keys (case-folded); empty means the cursor's row alone ---
static u64 NameKey(String name)
{
    u64 h = 0xcbf29ce484222325ull;
    for (s64 i = 0; i < name.count; i++) {
        u8 ch = name.data[i];
        h = (h ^ (u8)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch)) * 0x100000001b3ull;
    }
    return h;
}

// Where key is in the set, or where it would go (*found 0).
static s64 SelFind(const Browser *b, u64 key, b32 *found)
{
    s64 lo = 0, hi = b->sel.count;
    while (lo < hi) {
        s64 mid = (lo + hi) / 2;
        if (b->sel.data[mid] < key) lo = mid + 1;
        else hi = mid;
    }
    *found = lo < b->sel.count && b->sel.data[lo] == key;
    return lo;
}

static void SelSet(Browser *b, String name, b32 on)
{
    b32 found = 0;
    u64 key = NameKey(name);
    s64 at = SelFind(b, key, &found);
    if (on && !found) {
        Array_Push(&b->sel_arena, b->sel);
        memmove(b->sel.data + at + 1, b->sel.data + at, (size_t)(b->sel.count - 1 - at) * sizeof(u64));
        b->sel.data[at] = key;
    } else if (!on && found) {
        memmove(b->sel.data + at, b->sel.data + at + 1, (size_t)(b->sel.count - 1 - at) * sizeof(u64));
        b->sel.count--;
    }
}

static b32 RowSelected(const Browser *b, s64 row)
{
    if (!b->sel.count) return row == b->cursor;
    const FileEntry *e = BrowserEntry(b, row);
    b32 found = 0;
    if (e) SelFind(b, NameKey(e->name), &found);
    return found;
}

// Rows from..to (either order) become the selection.
static void SelRange(Browser *b, s64 from, s64 to)
{
    b->sel.count = 0;
    for (s64 r = Min(from, to); r <= Max(from, to); r++) {
        const FileEntry *e = BrowserEntry(b, r);
        if (e) SelSet(b, e->name, 1);
    }
}

// The rows a copy or a delete acts on: the selection's, else the cursor's (never This PC's drives).
static s64 SelectedPaths(Arena *a, const Browser *b, String **out, String *first_name)
{
    s64 n = 0;
    *out = Arena_PushArray(a, String, Max(1, b->sel.count) + 1);
    for (s64 r = 0; r < b->view_count; r++) {
        const FileEntry *e = BrowserEntry(b, r);
        if (!e || (e->flags & File_Drive) || !RowSelected(b, r) || n > b->sel.count) continue;
        if (!n) *first_name = e->name;
        (*out)[n++] = EntryPath(a, b, e);
    }
    return n;
}

// Puts the selected files on the clipboard (Explorer pastes them; their paths go as text).
static void BrowserCopy(Frame *f, PaneTab *t)
{
    String *paths = NULL, first = {0};
    s64 n = SelectedPaths(f->fa, t->browser, &paths, &first);
    if (!n) return;
    f->out->clipboard_file_count = n;
    f->out->clipboard_files = paths;
    BrowserNotice(t->browser, f->in->now_us, n == 1 ? Str_PushFormat(f->fa, "Copied %.*s", STR_ARG(first)) : Str_PushFormat(f->fa, "Copied %lld items", (long long)n));
}

// Names as Explorer sorts them: case doesn't count, runs of digits by their value ("file2" before "file10").
static s32 CompareFileNames(String a, String b)
{
    s64 i = 0, j = 0;
    while (i < a.count && j < b.count) {
        u8 x = a.data[i], y = b.data[j];
        if (isdigit(x) && isdigit(y)) {
            s64 i2 = i, j2 = j;
            while (i2 < a.count && a.data[i2] == '0') i2++;
            while (j2 < b.count && b.data[j2] == '0') j2++;
            s64 ie = i2, je = j2;
            while (ie < a.count && isdigit(a.data[ie])) ie++;
            while (je < b.count && isdigit(b.data[je])) je++;
            if (ie - i2 != je - j2) return ie - i2 < je - j2 ? -1 : 1;
            for (s64 k = 0; k < ie - i2; k++) if (a.data[i2 + k] != b.data[j2 + k]) return a.data[i2 + k] < b.data[j2 + k] ? -1 : 1;
            i = ie;
            j = je;
            continue;
        }
        x = (u8)tolower(x);
        y = (u8)tolower(y);
        if (x != y) return x < y ? -1 : 1;
        i++;
        j++;
    }
    return i < a.count ? 1 : j < b.count ? -1 : 0;
}

typedef struct SortContext { const FileEntry *entries; u8 sort; } SortContext;

// The picture view's pictures by name, as Explorer orders them.
static int __cdecl CompareNameStrings(const void *pa, const void *pb)
{
    return CompareFileNames(*(const String *)pa, *(const String *)pb);
}

// The go-to box's rows of a folder: best match first, then folders before files, then by name.
typedef struct GotoSort { s32 score; b32 dir; String name; s32 index; } GotoSort;
static int __cdecl CompareGotoSort(const void *pa, const void *pb)
{
    const GotoSort *a = (const GotoSort *)pa, *b = (const GotoSort *)pb;
    if (a->score != b->score) return a->score > b->score ? -1 : 1;
    if (a->dir != b->dir) return a->dir ? -1 : 1;
    return CompareFileNames(a->name, b->name);
}

// "1.5 GB", "320 KB", "12 B".
static String SizeText(Arena *a, u64 bytes)
{
    static const char *const units[5] = { "B", "KB", "MB", "GB", "TB" };
    f64 v = (f64)bytes;
    s32 u = 0;
    while (v >= 1024 && u < 4) { v /= 1024; u++; }
    return u == 0 ? Str_PushFormat(a, "%llu B", (unsigned long long)bytes) : Str_PushFormat(a, v < 10 ? "%.1f %s" : "%.0f %s", v, units[u]);
}

// An environment variable's value (the window's, NAME=value lines), case not counting; empty when unset.
static String EnvValue(String env, String name)
{
    for (s64 at = 0; at < env.count;) {
        s64 end = at;
        while (end < env.count && env.data[end] != '\n') end++;
        String line = { env.data + at, end - at };
        at = end + 1;
        if (line.count > name.count && line.data[name.count] == '=' && Str_EqualNoCase((String){ line.data, name.count }, name))
            return (String){ line.data + name.count + 1, line.count - name.count - 1 };
    }
    return (String){0};
}

// Folders first; then by the sort (0 name, 2 newest first, 4 biggest first; odd: the other way round), names
// breaking ties.
static int __cdecl CompareFileRows(void *ctx, const void *pa, const void *pb)
{
    const SortContext *sc = (const SortContext *)ctx;
    const FileEntry *a = &sc->entries[*(const s32 *)pa], *b = &sc->entries[*(const s32 *)pb];
    b32 ad = (a->flags & File_Dir) != 0, bd = (b->flags & File_Dir) != 0;
    if (ad != bd) return ad ? -1 : 1;
    s32 by = sc->sort / 2, r = 0;
    if (by == 1) r = a->modified < b->modified ? 1 : a->modified > b->modified ? -1 : 0;
    if (by == 2) r = a->size < b->size ? 1 : a->size > b->size ? -1 : 0;
    if (!r) r = CompareFileNames(a->name, b->name);
    return (sc->sort & 1) ? -r : r;
}

// --- commands ----------------------------------------------------------------------------------------------------

// A session's tab comes up: the one running it, else a new tab of the focused pane resumes it.
static void OpenSession(App *app, Frame *f, const TermSession *s)
{
    for (s64 i = 0; i < app->tab_count; i++) {
        if (!Str_Equal(app->tabs[i].key, s->id)) continue;
        PaneTab *t = Panes_FindTerm(&app->panes, app->tabs[i].id);
        if (t) { Panes_Select(&app->panes, t); return; }
    }
    PaneTab *t = app->panes.selected;
    RequestTab(app, f, t ? t->node : app->panes.root, TermNew_Claude, s->cwd);
    f->new_tabs.data[f->new_tabs.count - 1].session = Str_PushCopy(f->fa, s->id);
    f->new_tabs.data[f->new_tabs.count - 1].config_dir = Str_PushCopy(f->fa, s->config_dir);   // resumed under its own account
    Logf("app", "resume session %.*s", STR_ARG(s->id));
}

// Where new tabs start: the focused tab's folder (its browser's while it browses).
static String FocusedCwd(App *app)
{
    PaneTab *t = app->panes.selected;
    if (t && t->browsing && t->browser && t->browser->path.count) return ShellPath(&app->frame_arena, t->browser->path);
    const TermTab *st = t ? FindServerTab(app, t->term_id) : NULL;
    return st ? st->cwd : (String){0};
}

static const AppWebState *FindWebState(const AppFrameInput *in, u64 id)
{
    for (s32 i = 0; i < in->web_count; i++) if (in->webs[i].id == id) return &in->webs[i];
    return NULL;
}

// A web page's zoom steps (Chrome's); PaneTab.web_zoom counts from 100%.
static const f32 g_WebZooms[] = { 0.25f, 0.33f, 0.5f, 0.67f, 0.75f, 0.8f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f, 4.0f, 5.0f };
enum { WebZoom_Base = 7 };

// The address row's text, as where the page goes: an address (https:// added when it has no scheme), else a search.
static void WebGo(PaneTab *t, String text)
{
    while (text.count && text.data[0] == ' ') { text.data++; text.count--; }
    while (text.count && text.data[text.count - 1] == ' ') text.count--;
    if (!text.count) return;
    b32 scheme = 0, space = 0, dot = 0;
    for (s64 i = 0; i < text.count; i++) {
        if (text.data[i] == ' ') space = 1;
        if (text.data[i] == '.') dot = 1;
        if (text.data[i] == ':' && i + 2 < text.count && text.data[i + 1] == '/' && text.data[i + 2] == '/') scheme = 1;
    }
    s64 n = 0;
    const char *prefix = scheme ? "" : !space && (dot || (text.count >= 9 && memcmp(text.data, "localhost", 9) == 0)) ? "https://" : "https://www.google.com/search?q=";
    for (const char *s = prefix; *s && n < (s64)sizeof(t->web_go); s++) t->web_go[n++] = (u8)*s;
    for (s64 i = 0; i < text.count && n + 3 <= (s64)sizeof(t->web_go); i++) {
        u8 ch = text.data[i];
        if (!scheme && space && (ch == ' ' || ch == '&' || ch == '#' || ch == '+' || ch == '?')) {
            n += snprintf((char *)t->web_go + n, 4, "%%%02X", ch);   // a search's words
        } else {
            t->web_go[n++] = ch;
        }
    }
    t->web_go_len = (s32)n;
    memcpy(t->web_url, t->web_go, (size_t)n);   // until the page says where it is
    t->web_url_len = (s32)n;
}

static void StartWebEdit(App *app, const AppFrameInput *in, PaneTab *t)
{
    const AppWebState *ws = FindWebState(in, t->id);
    String url = ws && ws->url.count ? ws->url : (String){ t->web_url, t->web_url_len };
    TextEdit_Set(&app->web_edit, Str_Equal(url, S("about:blank")) ? (String){0} : url);
    app->web_edit_tab = t->id;
}

// A web tab in `leaf` (after `after`, else at the end), shown and focused; its page goes to `url` (empty: none yet).
static PaneTab *NewWebTab(App *app, PaneNode *leaf, PaneTab *after, String url, u32 popup)
{
    PaneTab *t = Panes_NewTab(&app->panes, leaf, after && after->node == leaf ? after->next : NULL);
    t->web = 1;
    t->web_popup = popup;
    t->created_frame = app->ui.frame;
    t->web_url_len = (s32)CopyInto(t->web_url, sizeof(t->web_url), url).count;
    if (url.count && !popup) WebGo(t, url);
    Panes_Select(&app->panes, t);
    return t;
}

static PaneTab *FindPaneTab(Panes *panes, u64 id)
{
    for (PaneNode *n = panes->root; n && id; n = Panes_NextNode(n)) {
        for (PaneTab *t = n->first_tab; t; t = t->next) if (t->id == id) return t;
    }
    return NULL;
}

// Moves a tab of the panes to where a drop put it, and selects it. Nodes are found again by id: taking the tab out may
// fold the tree.
static void DropTabAt(App *app, PaneTab *t, TabDrop d)
{
    Panes *panes = &app->panes;
    if (d.strip) {
        PaneTab *before = FindPaneTab(panes, d.before_id);
        if (t->node->id != d.node_id || before != t) {
            Panes_DetachTab(panes, t);
            PaneNode *dest = NULL;
            for (PaneNode *n = panes->root; n; n = Panes_NextNode(n)) if (n->id == d.node_id) dest = n;
            if (!dest) dest = panes->root;
            while (dest->first_child) dest = dest->first_child;
            Panes_InsertTab(t, dest, before && before->node == dest ? before : NULL);
        }
    } else {
        Panes_DetachTab(panes, t);
        PaneNode *node = NULL;
        for (PaneNode *n = panes->root; n && d.node_id; n = Panes_NextNode(n)) if (n->id == d.node_id) node = n;
        if (d.into && node) {
            Panes_InsertTab(t, node, NULL);
        } else {
            // the window's edges: into the root group when it splits the same way, else around it
            if (!d.node_id) {
                node = panes->root;
                if (node->first_child && node->vertical == d.vertical) {
                    node = node->first_child;
                    if (d.after) while (node->next) node = node->next;
                }
            }
            if (!node) node = panes->root;
            PaneNode *nn = Panes_InsertSibling(panes, node, d.vertical, d.after);
            Panes_InsertTab(t, nn, NULL);
            t->created_frame = app->ui.frame;
        }
    }
    Panes_Select(panes, t);
}

static void CloseTab(App *app, Frame *f, PaneTab *t, b32 confirmed)
{
    const TermTab *st = FindServerTab(app, t->term_id);
    if (!confirmed && st && (st->flags & (TermTab_Busy | TermTab_Running))) {
        app->confirm_tab = t->id;   // asks first: something is running there
        return;
    }
    if (t->term_id) *Array_Push(f->fa, f->closes) = t->term_id;
    if (app->sel.term_id == t->term_id) app->sel.term_id = 0;
    FreeBrowser(app, t);
    Panes_DetachTab(&app->panes, t);
    Panes_FreeTab(&app->panes, t);
    if (!Panes_TabCount(&app->panes)) f->out->window_command = AppWindow_Close;
}

static void SplitFocused(App *app, Frame *f, b32 vertical)
{
    PaneTab *t = app->panes.selected;
    PaneNode *n = Panes_InsertSibling(&app->panes, t ? t->node : app->panes.root, vertical, 1);
    RequestTab(app, f, n, TermNew_Shell, FocusedCwd(app));
}

static void RunCommand(App *app, Frame *f, u32 cmd)
{
    PaneTab *t = app->panes.selected;
    const TermGrid *g = t ? FindGrid(f->in, t->term_id) : NULL;
    switch (cmd) {
    case Cmd_NewTab: RequestTab(app, f, t ? t->node : app->panes.root, TermNew_Shell, FocusedCwd(app)); break;
    case Cmd_NewAgentTab: {
        if (app->agent_command.count) { RequestTab(app, f, t ? t->node : app->panes.root, TermNew_Agent, FocusedCwd(app)); break; }
        // claude: under the account the folder's rule names (the longest rule folder holding it), else the default
        String cwd = FocusedCwd(app), want = {0};
        s64 best = -1;
        for (s32 r = 0; r < app->folder_rule_count; r++) {
            String folder = app->folder_rules[r].folder;
            b32 holds = cwd.count >= folder.count && Str_EqualNoCase((String){ cwd.data, folder.count }, folder) &&
                        (cwd.count == folder.count || cwd.data[folder.count] == '\\' || cwd.data[folder.count] == '/');
            if (holds && folder.count > best) { best = folder.count; want = app->folder_rules[r].account; }
        }
        s32 account = 0;
        for (s32 i = 1; i < app->account_count && want.count; i++) if (Str_EqualNoCase(app->accounts[i].name, want)) account = i;
        if (want.count && !account) Logf("app", "the folder is for the %.*s account, which isn't set up: claude starts as the default one (and is held)", STR_ARG(want));
        RequestClaudeTab(app, f, app->accounts[account].dir);
    } break;
    case Cmd_NewClaudeAs: case Cmd_AddAccount:
        app->palette_open = 1;
        app->palette_mode = PaletteMode_Account;
        app->account_adding = cmd == Cmd_AddAccount;
        memset(&app->palette_edit, 0, sizeof(app->palette_edit));
        app->palette_sel = 0;
        app->palette_top = 0;
        break;
    case Cmd_CloseTab: if (t) CloseTab(app, f, t, 0); break;
    case Cmd_SplitRight: SplitFocused(app, f, 1); break;
    case Cmd_SplitDown: SplitFocused(app, f, 0); break;
    case Cmd_FocusLeft: case Cmd_FocusRight: case Cmd_FocusUp: case Cmd_FocusDown:
        if (t) { PaneTab *o = Panes_InDirection(&app->panes, t, (s32)(cmd - Cmd_FocusLeft)); if (o) Panes_Select(&app->panes, o); }
        break;
    case Cmd_NextTab: case Cmd_PrevTab:
        if (t) {
            // cycles the focused pane's strip
            PaneTab *first = t->node->first_tab, *pick = NULL;
            if (cmd == Cmd_NextTab) pick = t->next ? t->next : first;
            else {
                for (PaneTab *o = first; o; o = o->next) if (o->next == t) pick = o;
                if (!pick) for (pick = first; pick && pick->next; pick = pick->next) {}
            }
            Panes_Select(&app->panes, pick);
        }
        break;
    case Cmd_NextPane: if (t) Panes_Select(&app->panes, Panes_Neighbor(&app->panes, t, 1)); break;
    case Cmd_EvenPanes: Panes_EvenChildren(app->panes.root, 1); Panes_ComputeRects(app->panes.root); break;
    case Cmd_ZoomIn: case Cmd_ZoomOut: case Cmd_ZoomReset:
        if (t && t->web) {
            // a web tab's page zooms instead of the terminals' text
            t->web_zoom = cmd == Cmd_ZoomReset ? 0 : Clamp(t->web_zoom + (cmd == Cmd_ZoomIn ? 1 : -1), -WebZoom_Base, (s32)ArrayCount(g_WebZooms) - 1 - WebZoom_Base);
            break;
        }
        app->term_font_pt = cmd == Cmd_ZoomReset ? 12 : Clamp(app->term_font_pt + (cmd == Cmd_ZoomIn ? 1 : -1), 6, 72);
        SetSetting(f->out, S("font_size"), Str_PushFormat(f->fa, "%g", (double)app->term_font_pt));
        break;
    case Cmd_Fullscreen: f->out->window_command = AppWindow_Fullscreen; break;
    case Cmd_Copy:
        if (t && t->browsing && t->browser) { BrowserCopy(f, t); break; }
        if (app->sel.visible && app->sel.term_id) {
            const TermGrid *sg = FindGrid(f->in, app->sel.term_id);
            if (sg) {
                const TermTab *sst = FindServerTab(app, sg->tab_id);
                f->out->clipboard_set = 1;
                f->out->clipboard_text = SelectionText(f->fa, &app->sel, sg);
                if (sst && (sst->flags & TermTab_Agent) && !app->sel.block) f->out->clipboard_text = CleanAgentCopy(f->fa, f->out->clipboard_text, sg->cols);
            }
        }
        break;
    case Cmd_Paste: if (t && t->term_id && !t->browsing) { app->paste_wait = t->term_id; f->out->read_clipboard = 1; } break;
    case Cmd_ScrollPageUp: case Cmd_ScrollPageDown:
        if (t && t->term_id && g) TermScrollBy(f, t->term_id, (cmd == Cmd_ScrollPageUp ? 1 : -1) * Max(1, g->rows - 1));
        break;
    case Cmd_PrevCommand: case Cmd_NextCommand:
        if (t && t->term_id && !t->browsing) TermScrollBy(f, t->term_id, cmd == Cmd_PrevCommand ? TermScroll_PrevMark : TermScroll_NextMark);
        break;
    case Cmd_Broadcast: app->broadcast = !app->broadcast; break;
    case Cmd_Topmost: f->out->window_command = AppWindow_ToggleTopmost; break;
    case Cmd_Duplicate:
        if (t && t->web) NewWebTab(app, t->node, t, (String){ t->web_url, t->web_url_len }, 0);
        else if (t) RequestTab(app, f, t->node, TermNew_Shell, FocusedCwd(app));
        break;
    case Cmd_CopyAll: if (t && t->term_id) TermScrollBy(f, t->term_id, TermScroll_CopyAll); break;
    case Cmd_ClearScrollback: if (t && t->term_id) TermScrollBy(f, t->term_id, TermScroll_Clear); break;
    case Cmd_ScrollBottom: if (t && t->term_id) TermScrollBy(f, t->term_id, TermScroll_ToBottom); break;
    case Cmd_ResizeLeft: case Cmd_ResizeRight: case Cmd_ResizeUp: case Cmd_ResizeDown:
        if (t) {
            // the separator the focused pane (or its nearest group split that way) has on that side moves that way
            b32 side_by_side = cmd == Cmd_ResizeLeft || cmd == Cmd_ResizeRight;
            f32 d = (cmd == Cmd_ResizeLeft || cmd == Cmd_ResizeUp) ? -0.05f : 0.05f;
            PaneNode *a = t->node;
            while (a->parent && a->parent->vertical != side_by_side) a = a->parent;
            if (a->parent && (a->next || a->prev)) {
                PaneNode *first = a->next ? a : a->prev, *second = a->next ? a->next : a;
                if (first->split_ratio + d >= 0.05f && second->split_ratio - d >= 0.05f) {
                    first->split_ratio += d;
                    second->split_ratio -= d;
                    Panes_ComputeRects(app->panes.root);
                    app->layout_changed_us = f->in->now_us;
                }
            }
        }
        break;
    case Cmd_NextTheme:
        app->term_theme = (app->term_theme + 1) % TermTheme_Count;
        SetSetting(f->out, S("theme"), Str_FromCStr(g_TermThemes[app->term_theme].name));
        break;
    case Cmd_Palette: case Cmd_Settings: {
        // again in the same mode closes it
        u8 mode = cmd == Cmd_Settings ? PaletteMode_Settings : PaletteMode_Commands;
        app->palette_open = !(app->palette_open && app->palette_mode == mode);
        app->palette_mode = mode;
        memset(&app->palette_edit, 0, sizeof(app->palette_edit));
        app->palette_sel = 0;
        app->palette_top = 0;
        app->setting_editing = 0;
    } break;
    case Cmd_Reader: {
        // a claude tab's transcript as the reader (again: back to the terminal); read now, and again when a turn ends
        const TermTab *st = t ? FindServerTab(app, t->term_id) : NULL;
        if (!st || !(st->flags & TermTab_Agent) || !st->key.count || t->browsing || t->viewing) break;
        app->reader_open = !(app->reader_open && app->reader_term == t->term_id);
        app->reader_term = t->term_id;
        if (app->reader_open) {
            f->out->history_get = t->term_id;
            app->hist_version = 0;
            app->hist_at_end = 1;
            app->hist_follow = 1;
            app->reader_busy = (st->flags & TermTab_Busy) != 0;
        }
    } break;
    case Cmd_Compose:
        // the composer over the focused terminal (again: away, the draft kept)
        if (!t || !t->term_id || t->browsing || t->viewing) break;
        app->compose_open = !(app->compose_open && app->compose_term == t->term_id);
        app->compose_term = t->term_id;
        break;
    case Cmd_FocusMode:
        // at once, both ways: the panes snap (no easing); the next frame lays them out
        if (!app->focus_mode && !t) break;
        app->focus_mode = !app->focus_mode;
        if (app->focus_mode) {
            app->focus_prev_zoom = app->zoom_node;
            app->zoom_node = app->panes.root->first_child ? t->node->id : 0;
            app->focus_since_us = f->in->now_us;
            app->palette_open = 0;
            app->tab_menu = 0;
        } else {
            app->zoom_node = app->focus_prev_zoom;
        }
        app->snap_until_frame = app->ui.frame + 2;
        app->ui.animating = 1;
        break;
    case Cmd_GoTo:
        // the focused tab's browser comes up there (from a shell too: its files face)
        if (app->palette_open && app->palette_mode == PaletteMode_GoTo) app->palette_open = 0;
        else if (t && t->term_id) OpenGoTo(app, t, (String){0});
        break;
    case Cmd_Bookmark:
        if (t && t->browsing && t->browser) ToggleBookmark(app, t->browser->path);
        break;
    case Cmd_Profile: f->out->log_profile = 1; break;
    case Cmd_DebugOverlay: app->debug_overlay = !app->debug_overlay; break;
    case Cmd_CloseWindow: f->out->window_command = AppWindow_Close; break;
    case Cmd_Sessions:
        // open and focused, else open and focus it; again closes it
        if (app->sidebar_open && app->side_focus) { app->sidebar_open = 0; app->side_focus = 0; }
        else { app->sidebar_open = 1; app->side_focus = 1; app->side_reveal = 1; }
        break;
    case Cmd_TearOff:
        if (t && (t->term_id || t->web)) {
            f->out->tear_off_tab = t->term_id;
            if (t->web) { f->out->tear_off_web = t->id; f->out->tear_off_url = (String){ t->web_url, t->web_url_len }; }
            f->out->tear_off_last = Panes_TabCount(&app->panes) == 1;
        }
        break;
    case Cmd_RenameTab:
        if (t) {
            TextEdit_Set(&app->rename_edit, PaneTabLabel(app, t));
            app->rename_edit.mark = 0;
            app->rename_tab = t->id;
        }
        break;
    case Cmd_ZoomPane:
        app->zoom_node = !app->zoom_node && t && app->panes.root->first_child ? t->node->id : 0;
        break;
    case Cmd_Find: case Cmd_Search:
        // search, as vim's /: the bar on the focused terminal (a claude's: its whole history); again while it has the
        // keyboard closes it. In a browser, typing already filters
        if (app->find_open && app->find_focus && t && t->term_id == app->find_term) { app->find_open = 0; break; }
        if (t && t->term_id && !t->browsing) {
            if (app->find_open && app->find_term != t->term_id) app->find_sent = 0;
            const TermTab *st = FindServerTab(app, t->term_id);
            app->find_open = 1;
            app->find_focus = 1;
            app->find_vim = cmd == Cmd_Search;
            app->find_term = t->term_id;
            app->find_history = st && (st->flags & TermTab_Agent) && st->key.count;
            app->find_edit.mark = 0;   // all of the last text selected: typing replaces it
            app->find_edit.cursor = app->find_edit.count;
            if (app->find_history) {
                f->out->history_get = t->term_id;
                app->hist_version = 0;   // built again from the fresh read
                app->hist_current = -1;
                app->hist_follow = 1;
                app->hist_at_end = 1;
            }
        }
        break;
    case Cmd_ToggleBrowser:
        if (t && t->browsing) CloseBrowser(app, f, t);
        else if (t && !t->web) OpenBrowser(app, t);
        break;
    case Cmd_NewWebTab: {
        // a blank page, the address being typed
        PaneTab *w = NewWebTab(app, t ? t->node : app->panes.root, t, (String){0}, 0);
        StartWebEdit(app, f->in, w);
        break;
    }
    }
}

b32 App_RunCommandByName(App *app, String line)
{
    String name = line, arg = {0};
    for (s64 i = 0; i < line.count; i++) {
        if (line.data[i] == ' ') { name.count = i; arg = (String){ line.data + i + 1, line.count - i - 1 }; break; }
    }
    if (Str_EqualNoCase(name, S("theme")) && arg.count) {
        for (s32 i = 0; i < TermTheme_Count; i++) {
            if (Str_EqualNoCase(arg, Str_FromCStr(g_TermThemes[i].name))) { app->term_theme = i; return 1; }
        }
        return 0;
    }
    for (u32 c = 1; c < Cmd_Count; c++) {
        if (!Str_EqualNoCase(name, Str_FromCStr(g_Commands[c].name))) continue;
        if (app->queued_count < (s32)ArrayCount(app->queued)) app->queued[app->queued_count++] = c;
        return 1;
    }
    return 0;
}

String App_CommandNames(App *app, Arena *arena)
{
    (void)app;
    TextBuilder tb = { arena, NULL, 0, 0 };
    for (u32 c = 1; c < Cmd_Count; c++) TB_Format(&tb, "%s\t%s\n", g_Commands[c].name, g_Commands[c].label);
    TB_Append(&tb, S("theme <name>\t"));
    for (s32 i = 0; i < TermTheme_Count; i++) TB_Format(&tb, "%s%s", i ? ", " : "", g_TermThemes[i].name);
    TB_Append(&tb, S("\n"));
    return TB_String(&tb);
}

static String HotkeyName(Arena *a, u8 key, u8 mods)
{
    if (!key) return (String){0};
    char k[16] = {0};
    if (key >= Key_A && key < Key_A + 26) k[0] = (char)('A' + key - Key_A);
    else if (key >= Key_0 && key < Key_0 + 10) k[0] = (char)('0' + key - Key_0);
    else if (key >= Key_F1 && key < Key_F1 + 24) snprintf(k, sizeof(k), "F%d", key - Key_F1 + 1);
    else {
        const char *n = "?";
        for (s32 i = (s32)ArrayCount(g_KeyNames) - 1; i >= 0; i--) if (g_KeyNames[i].key == key) n = g_KeyNames[i].name;
        snprintf(k, sizeof(k), "%s", n);
    }
    return Str_PushFormat(a, "%s%s%s%s", (mods & Mod_Ctrl) ? "Ctrl+" : "", (mods & Mod_Alt) ? "Alt+" : "", (mods & Mod_Shift) ? "Shift+" : "", k);
}

// Pane rects in window pixels: the tree's split ratios over the area right of the sidebar; a zoomed pane covers it.
static void LayoutPanes(App *app, s32 area_x, s32 area_w, s32 area_h)
{
    Panes *panes = &app->panes;
    Panes_ComputeRects(panes->root);
    Panes_ComputePixels(panes->root, (f32)area_x, (f32)area_w, (f32)area_h);
    PaneNode *zoomed = NULL;
    for (PaneNode *n = panes->root; n && app->zoom_node; n = Panes_NextNode(n)) if (n->id == app->zoom_node && !n->first_child) zoomed = n;
    if (!zoomed || !panes->root->first_child || (panes->selected && panes->selected->node != zoomed)) app->zoom_node = 0;
    if (!app->zoom_node) return;
    zoomed->px[0] = (f32)area_x;
    zoomed->px[1] = 0;
    zoomed->px[2] = (f32)(area_x + area_w);
    zoomed->px[3] = (f32)area_h;
}

// --- frame -------------------------------------------------------------------------------------------------------

void App_Frame(App *app, const AppFrameInput *in, AppOutput *out)
{
    BEGIN_TIMED_BLOCK(AppFrame);
    Ui *ui = &app->ui;
    Arena *fa = &app->frame_arena;
    Arena_Clear(fa);
    app->in = in;
    app->tabs = in->tabs;
    app->tab_count = in->tab_count;
    app->fonts = in->fonts;
    app->named_count = 0;
    app->pane_rect_count = 0;
    u64 now = in->now_us;
    Panes *panes = &app->panes;
    Frame f = {0};
    f.in = in;
    f.out = out;
    f.fa = fa;
    f.nav_open = -1;
    b32 reset_asked = 0;   // the reset's confirm box opens next frame: the Enter that asked mustn't answer it
    app->maximized = in->maximized;
    memcpy(app->window_rect, in->window_rect, sizeof(app->window_rect));
    // settings.txt read again after a reset: applied as at start (the theme fades to its new one)
    if (in->settings) {
        ApplySettings(app, in->settings);
        out->term_font = app->term_font;
    }

    // --- metrics, colors, fonts ---
    u32 dpi = in->dpi ? in->dpi : 96;
    f32 em = 9.0f * (f32)dpi / 72.0f;   // the UI font's size (9 pt) in px
    app->em = em;
    DrawList *dl = &app->dl;
    DrawList_Begin(dl, fa, in->client_w, in->client_h);
    Ui_BeginFrame(ui, &in->input, dl, fa, now);
    ui->dt = Min((f32)in->dt_us / 1e6f, 0.05f);
    // --- the theme: a new one fades in from the last (the chrome and the terminals' colors together) ---
    if (app->term_theme != app->theme_shown) {
        app->theme_from = app->theme_shown;
        app->theme_shown = app->term_theme;
        app->theme_changes++;
    }
    TermTheme theme = g_TermThemes[app->term_theme];
    f32 theme_t = app->theme_changes ? Ui_Animate(ui, Ui_IdN(Ui_Id("ThemeFade"), app->theme_changes), 0, 1, 14) : 1;
    if (theme_t < 1) {
        const TermTheme *from = &g_TermThemes[app->theme_from];
        theme.fg = ColorMix(from->fg, theme.fg, theme_t);
        theme.bg = ColorMix(from->bg, theme.bg, theme_t);
        theme.selection = ColorMix(from->selection, theme.selection, theme_t);
        theme.accent = ColorMix(from->accent, theme.accent, theme_t);
        for (s32 k = 0; k < 16; k++) theme.ansi[k] = ColorMix(from->ansi[k], theme.ansi[k], theme_t);
    }
    const TermTheme *tt = &theme;
    app->c = ChromeFor(tt);
    Chrome *c = &app->c;
    Font *font = &in->fonts[Font_Ui], *small = &in->fonts[Font_UiSmall], *bold = &in->fonts[Font_UiBold];
    Font *icons = &in->fonts[Font_Icon], *icons_small = &in->fonts[Font_IconSmall];
    if (app->focus_mode && (!panes->selected || !panes->selected->term_id)) { app->focus_mode = 0; app->zoom_node = app->focus_prev_zoom; }   // its tab went
    s32 caption_h = app->focus_mode ? 0 : R(2.7);   // focus: no strips
    s32 resize_border = R(0.3);
    s32 line_w = Max(1, R(0.1));
    s32 term_margin = R(0.5);
    s32 cell_w = Max(1, in->cell_w), cell_h = Max(1, in->cell_h);
    s32 browse_header_h = R(2.8), browse_footer_h = R(2.2), browse_row_h = R(2.0), browse_heads_h = R(1.8);
    s32 account_h = R(2.0);   // a claude tab's account line, under its terminal
    f32 radius_small = (f32)R(0.35), radius_medium = (f32)R(0.6), radius_large = (f32)R(0.9);

    ui->tip_font = small;
    ui->tip_fg = c->text;
    ui->tip_bg = c->surface;
    ui->tip_border = c->line;
    s32 mouse_x = in->input.mouse[0], mouse_y = in->input.mouse[1];

    // --- the server went away: its tab ids mean nothing now; the panes keep their tabs by lasting id and find them
    //     again in the next server's list ---
    if (app->was_connected && !in->connected) {
        Logf("app", "server gone: panes keep their tabs until a server is back");
        for (PaneNode *n = panes->root; n; n = Panes_NextNode(n)) {
            for (PaneTab *t = n->first_tab; t; t = t->next) { t->term_id = 0; t->request = 0; }
        }
        app->tabs_seen = 0;
        app->first_requested = 0;
        app->sel.term_id = 0;
        app->mouse_tab = 0;
        app->watch_ever_sent = 0;
        app->sent_watch_count = 0;
    }
    app->was_connected = in->connected;

    // --- the server's tabs and the panes' tabs, matched up ---
    BEGIN_TIMED_BLOCK(Reconcile);
    app->tabs_seen = in->tabs_received;
    for (PaneNode *n = panes->root; n; n = Panes_NextNode(n)) {
        for (PaneTab *t = n->first_tab; t; t = t->next) {
            if (t->term_id || t->web) continue;
            for (s64 i = 0; i < in->tab_count && !t->term_id; i++) {
                const TermTab *st = &in->tabs[i];
                b32 taken = Panes_FindTerm(panes, st->id) != NULL || !Str_Equal(st->owner, app->window_name);
                b32 by_request = t->request && st->request == t->request;
                b32 by_key = !t->request && t->key_len && st->key.count == t->key_len && memcmp(st->key.data, t->key, (size_t)t->key_len) == 0;
                if (!taken && (by_request || by_key)) {
                    t->term_id = st->id;
                    t->request = 0;
                    app->had_tabs = 1;
                }
            }
        }
    }
    // tabs the panes lost (closed or exited at the server, moved to another window), and restored ones whose tab
    // isn't coming back
    for (PaneNode *n = panes->root; n;) {
        PaneNode *next_node = Panes_NextNode(n);
        b32 restart = 0;
        for (PaneTab *t = n->first_tab; t;) {
            PaneTab *next = t->next;
            const TermTab *st = FindServerTab(app, t->term_id);
            b32 gone = !t->web && in->connected && app->tabs_seen && ((t->term_id && !st) || (!t->term_id && !t->request) || (st && !Str_Equal(st->owner, app->window_name)));
            if (gone) {
                Logf("app", "pane tab %llu gone (server tab %u)", (unsigned long long)t->id, t->term_id);
                if (app->sel.term_id == t->term_id) app->sel.term_id = 0;
                b32 pane_goes = n->first_tab == t && !t->next && n->parent;
                FreeBrowser(app, t);
                Panes_DetachTab(panes, t);
                Panes_FreeTab(panes, t);
                if (pane_goes) { restart = 1; break; }
            }
            t = next;
        }
        n = restart ? panes->root : next_node;
    }
    // server tabs of this window no pane has (restored at the server, or moved here from another window): into the
    // focused pane
    for (s64 i = 0; i < in->tab_count; i++) {
        const TermTab *st = &in->tabs[i];
        if (Panes_FindTerm(panes, st->id) || !Str_Equal(st->owner, app->window_name)) continue;
        b32 pending = 0;
        for (PaneNode *n = panes->root; n && !pending; n = Panes_NextNode(n)) {
            for (PaneTab *t = n->first_tab; t && !pending; t = t->next) pending = t->request && t->request == st->request;
        }
        if (pending) continue;
        PaneNode *leaf = panes->selected ? panes->selected->node : NULL;
        for (PaneNode *n = panes->root; n && !leaf; n = Panes_NextNode(n)) if (!n->first_child) leaf = n;
        if (!leaf) continue;
        PaneTab *t = Panes_NewTab(panes, leaf, NULL);
        t->term_id = st->id;
        t->created_frame = ui->frame;
        if (!panes->selected) Panes_Select(panes, t);
        app->had_tabs = 1;
        if (st->id == app->arrive_tab) { DropTabAt(app, t, app->arrive_drop); app->arrive_tab = 0; }   // dropped here from another window
    }
    // the main window, once it has the tab list: the other windows that have tabs (torn off before a restart)
    if (!app->windows_opened && app->tabs_seen && !app->window_name.count) {
        app->windows_opened = 1;
        String *names = Arena_PushArray(fa, String, in->tab_count + 1);
        s64 name_count = 0;
        for (s64 i = 0; i < in->tab_count; i++) {
            b32 known = !in->tabs[i].owner.count;
            for (s64 k = 0; k < name_count && !known; k++) known = Str_Equal(names[k], in->tabs[i].owner);
            if (!known) names[name_count++] = in->tabs[i].owner;
        }
        out->open_window_count = name_count;
        out->open_windows = names;
    }
    // keys follow the server (a claude found in a shell gives the tab a session id)
    for (PaneNode *n = panes->root; n; n = Panes_NextNode(n)) {
        for (PaneTab *t = n->first_tab; t; t = t->next) {
            const TermTab *st = FindServerTab(app, t->term_id);
            if (!st) continue;
            t->key_len = (s32)Min(st->key.count, (s64)Pane_KeyMax);
            memcpy(t->key, st->key.data, (size_t)t->key_len);
        }
    }
    if (!panes->selected) {
        for (PaneNode *n = panes->root; n && !panes->selected; n = Panes_NextNode(n)) if (n->first_tab) Panes_Select(panes, Panes_VisibleTab(n));
    }
    END_TIMED_BLOCK(Reconcile);

    // --- pane pixels and the focused pane's terminal size ---
    s32 sidebar_w = app->sidebar_open && !app->focus_mode ? Min(R(24), in->client_w / 2) : 0;
    s32 area_x = sidebar_w;
    s32 area_w = Max(1, in->client_w - area_x), area_h = Max(1, in->client_h);
    LayoutPanes(app, area_x, area_w, area_h);
    {
        PaneNode *n = panes->selected ? panes->selected->node : panes->root;
        f.pane_cols = ((s32)(n->px[2] - n->px[0]) - 2 * term_margin) / cell_w;
        f.pane_rows = ((s32)(n->px[3] - n->px[1]) - caption_h - 2 * term_margin) / cell_h;
    }

    // --- accounts.txt, read again when it changed: "account <name> = <config folder>", "theme <name> = <theme>",
    //     "folder <folder> = <name>" lines (the accounts first, then their themes and folders) ---
    if (in->accounts_version != app->accounts_version) {
        app->accounts_version = in->accounts_version;
        String text = CopyInto(app->accounts_buf, sizeof(app->accounts_buf), in->accounts_text);
        app->account_count = 1;
        app->accounts[0].name = S("default");
        app->accounts[0].dir = (String){0};
        app->accounts[0].theme = -1;
        app->folder_rule_count = 0;
        for (s32 pass = 0; pass < 2; pass++) {
            for (s64 at = 0; at < text.count;) {
                s64 end = at;
                while (end < text.count && text.data[end] != '\n') end++;
                String line = { text.data + at, end - at };
                at = end + 1;
                while (line.count && (line.data[0] == ' ' || line.data[0] == '\t')) { line.data++; line.count--; }
                while (line.count && strchr(" \t\r", line.data[line.count - 1])) line.count--;
                s64 sp = 0, eq = 0;
                while (sp < line.count && line.data[sp] != ' ' && line.data[sp] != '\t') sp++;
                while (eq < line.count && line.data[eq] != '=') eq++;
                if (!line.count || line.data[0] == '#' || eq >= line.count || eq < sp) continue;
                String kind = { line.data, sp }, name = { line.data + sp, eq - sp }, value = { line.data + eq + 1, line.count - eq - 1 };
                while (name.count && (name.data[0] == ' ' || name.data[0] == '\t')) { name.data++; name.count--; }
                while (name.count && (name.data[name.count - 1] == ' ' || name.data[name.count - 1] == '\t')) name.count--;
                while (value.count && (value.data[0] == ' ' || value.data[0] == '\t')) { value.data++; value.count--; }
                while (value.count > 3 && (value.data[value.count - 1] == '\\' || value.data[value.count - 1] == '/')) value.count--;
                while (name.count > 3 && (name.data[name.count - 1] == '\\' || name.data[name.count - 1] == '/')) name.count--;
                if (!name.count || !value.count) continue;
                s32 known = -1;
                for (s32 i = 0; i < app->account_count; i++) if (Str_EqualNoCase(app->accounts[i].name, name)) known = i;
                if (pass == 0 && Str_Equal(kind, S("account")) && known != 0) {
                    if (known < 0 && app->account_count < (s32)ArrayCount(app->accounts)) {
                        known = app->account_count++;
                        app->accounts[known].name = name;
                        app->accounts[known].theme = -1;
                    }
                    if (known > 0) app->accounts[known].dir = value;
                } else if (pass == 1 && Str_Equal(kind, S("theme")) && known >= 0) {
                    for (s32 k = 0; k < TermTheme_Count; k++) if (Str_EqualNoCase(Str_FromCStr(g_TermThemes[k].name), value)) app->accounts[known].theme = k;
                } else if (pass == 1 && Str_Equal(kind, S("folder")) && app->folder_rule_count < (s32)ArrayCount(app->folder_rules)) {
                    app->folder_rules[app->folder_rule_count].folder = name;
                    app->folder_rules[app->folder_rule_count++].account = value;
                }
            }
        }
        Logf("app", "accounts: %d, folder rules: %d", app->account_count, app->folder_rule_count);
    }
    // an account just added: its claude tab (to /login) once it's listed
    for (s32 i = 1; i < app->account_count && app->pending_account_len && in->connected && app->tabs_seen; i++) {
        if (!Str_EqualNoCase(app->accounts[i].name, (String){ app->pending_account, app->pending_account_len })) continue;
        app->pending_account_len = 0;
        RequestClaudeTab(app, &f, app->accounts[i].dir);
    }

    // --- a tab asked for from outside (`moss -d DIR [-- command]`, Explorer's "Open in moss"): into the focused pane ---
    if (in->open_request && in->connected && app->tabs_seen) {
        PaneTab *ft = panes->selected;
        RequestTab(app, &f, ft ? ft->node : panes->root, TermNew_Shell, in->open_cwd);
        out->open_taken = 1;
        app->first_requested = 1;
    }
    // --- a web tab torn off another window: this new window's first tab ---
    if (in->open_web.count) {
        PaneNode *leaf = panes->root;
        while (leaf->first_child) leaf = leaf->first_child;
        NewWebTab(app, leaf, NULL, Str_Equal(in->open_web, S("about:blank")) ? (String){0} : in->open_web, 0);
        out->open_web_taken = 1;
    }
    // --- the very first tab: nothing at the server, nothing on the way ---
    if (in->connected && app->tabs_seen && !in->tab_count && !Panes_TabCount(panes) && !app->first_requested && !in->open_request) {
        app->first_requested = 1;
        PaneNode *leaf = panes->root;
        while (leaf->first_child) leaf = leaf->first_child;
        RequestTab(app, &f, leaf, TermNew_Shell, (String){0});
    }
    // the last tab exited at the server: the window goes with it
    if (in->connected && app->had_tabs && app->tabs_seen && !Panes_TabCount(panes)) out->window_command = AppWindow_Close;

    // --- the go-to box's listings: the typed folder's, the drives ---
    for (s64 i = 0; i < in->files.listing_count; i++) {
        const FileListing *l = &in->files.listings[i];
        b32 typed = app->goto_request && l->request == app->goto_request, drives = app->drives_request && l->request == app->drives_request;
        if (!typed && !drives) continue;
        Arena *a = typed ? &app->goto_arena : &app->drives_arena;
        Arena_Clear(a);
        FileEntry *copy = Arena_PushArray(a, FileEntry, l->count + 1);
        for (s64 k = 0; k < l->count; k++) {
            copy[k] = l->entries[k];
            copy[k].name = Str_PushCopy(a, l->entries[k].name);
            copy[k].label = Str_PushCopy(a, l->entries[k].label);
        }
        if (typed) { app->goto_entries = copy; app->goto_count = l->count; app->goto_listed = 1; app->goto_request = 0; }
        else { app->drives = copy; app->drive_count = l->count; }
    }

    // --- the picture views: their folders' listings (the pictures in them, Explorer's order); `view <file>` typed in a
    //     tab of this window brings one up ---
    for (PaneNode *n = panes->root; n; n = Panes_NextNode(n)) {
        for (PaneTab *t = n->first_tab; t; t = t->next) {
            Viewer *v = t->viewer;
            if (in->view_tab && t->term_id == in->view_tab && in->view_path.count) { OpenViewer(app, t, in->view_path, 0); Panes_Select(panes, t); v = t->viewer; }
            if (!v) continue;
            for (s64 i = 0; i < in->files.listing_count; i++) {
                const FileListing *l = &in->files.listings[i];
                if (!v->request || l->request != v->request) continue;
                Arena_Clear(&v->arena);
                v->names = Arena_PushArray(&v->arena, String, l->count + 1);
                v->count = 0;
                for (s64 k = 0; k < l->count; k++) {
                    if ((l->entries[k].flags & File_Dir) || !HasExtension(l->entries[k].name, g_PictureTypes)) continue;
                    v->names[v->count++] = Str_PushCopy(&v->arena, l->entries[k].name);
                }
                qsort(v->names, (size_t)v->count, sizeof(String), CompareNameStrings);
                if (!v->name.count && v->count) v->name = CopyInto(v->name_buf, sizeof(v->name_buf), v->names[0]);
                v->listed = 1;
                v->request = 0;
            }
        }
    }

    // --- file browsers: listings and changes in; the rows built again when the listing, filter or sort changed ---
    BEGIN_TIMED_BLOCK(Browsers);
    s32 browser_rows = 1;
    if (panes->selected) {
        PaneNode *n = panes->selected->node;
        browser_rows = Max(1, ((s32)(n->px[3] - n->px[1]) - caption_h - browse_header_h - browse_footer_h - browse_heads_h) / browse_row_h);
    }
    for (PaneNode *n = panes->root; n; n = Panes_NextNode(n)) {
        for (PaneTab *t = n->first_tab; t; t = t->next) {
            if (t->browsing && !t->browser) OpenBrowser(app, t);   // restored from the session
            Browser *b = t->browser;
            if (!b) continue;
            for (s64 i = 0; i < in->files.listing_count; i++) {
                const FileListing *l = &in->files.listings[i];
                if (!b->request || l->request != b->request) continue;
                if (b->listed && Str_EqualNoCase(BrowsePath(l->path), b->path)) {
                    // --- listed again: the view stays put on its top row, and what's new or changed lights up for a
                    //     moment (by name, against the old listing, which is still here) ---
                    FileEntry *top = BrowserEntry(b, b->top);
                    b->top_name_len = top ? (s32)CopyInto(b->top_name, sizeof(b->top_name), top->name).count : 0;
                    s64 cap = 16;
                    while (cap < b->count * 2) cap <<= 1;
                    u64 *keys = Arena_PushArray(fa, u64, cap);
                    s32 *at = Arena_PushArray(fa, s32, cap);
                    for (s64 k = 0; k < b->count; k++) {
                        u64 h = NameKey(b->entries[k].name) | 1;
                        s64 slot = (s64)(h & (u64)(cap - 1));
                        while (keys[slot]) slot = (slot + 1) & (cap - 1);
                        keys[slot] = h;
                        at[slot] = (s32)k;
                    }
                    for (s64 k = 0; k < l->count; k++) {
                        const FileEntry *e = &l->entries[k];
                        u64 h = NameKey(e->name) | 1;
                        s64 slot = (s64)(h & (u64)(cap - 1));
                        while (keys[slot] && keys[slot] != h) slot = (slot + 1) & (cap - 1);
                        const FileEntry *old = keys[slot] ? &b->entries[at[slot]] : NULL;
                        b32 changed = !old || (!(e->flags & File_Dir) && (old->size != e->size || old->modified != e->modified));
                        if (!changed) continue;
                        b->flashes[b->flash_next].key = h;
                        b->flashes[b->flash_next].at_us = now;
                        b->flash_next = (b->flash_next + 1) % (s32)ArrayCount(b->flashes);
                    }
                }
                Arena_Clear(&b->list_arena);
                b->entries = Arena_PushArray(&b->list_arena, FileEntry, l->count + 1);
                for (s64 k = 0; k < l->count; k++) {
                    b->entries[k] = l->entries[k];
                    b->entries[k].name = Str_PushCopy(&b->list_arena, l->entries[k].name);
                    b->entries[k].label = Str_PushCopy(&b->list_arena, l->entries[k].label);
                }
                b->count = l->count;
                b->error = l->error;
                b->listed = 1;
                b->request = 0;
                b->view_dirty = 1;
                for (s64 k = 0; k < b->count; k++) {
                    if (!(b->entries[k].flags & File_Dir)) continue;
                    u64 key = NameKey(b->entries[k].name);
                    for (s64 s = 0; s < b->sizes.count; s++) if (b->sizes.data[s].key == key) { b->entries[k].size = b->sizes.data[s].bytes; b->entries[k].flags |= File_Sized; }
                }
            }
            for (s64 i = 0; i < in->files.size_count; i++) {
                // a subfolder's size: kept by name (a listing again keeps it), and in its entry
                const FolderSize *fs = &in->files.sizes[i];
                if (!b->path.count || !Str_EqualNoCase(fs->folder, ShellPath(fa, b->path))) continue;
                u64 key = NameKey(fs->name);
                s64 at = 0;
                while (at < b->sizes.count && b->sizes.data[at].key != key) at++;
                if (at == b->sizes.count) Array_Push(&b->size_arena, b->sizes)->key = key;
                b->sizes.data[at].bytes = fs->bytes;
                for (s64 k = 0; k < b->count; k++) if (NameKey(b->entries[k].name) == key) { b->entries[k].size = fs->bytes; b->entries[k].flags |= File_Sized; }
                if (app->browse_sort / 2 == 2) b->view_dirty = 1;
            }
            String shell_path = ShellPath(fa, b->path);
            for (s64 i = 0; i < in->files.changed_count && b->listed; i++) if (Str_EqualNoCase(in->files.changed[i], shell_path)) b->stale = 1;
            u64 key = Hash_Bytes(b->filter, (u64)b->filter_len, 0x7A11 + app->browse_sort * 2 + (u64)app->hide_hidden);
            if (b->view_dirty || b->view_key != key) {
                // --- the rows: folders first, sorted, hidden ones left out when asked, narrowed by the typed text; the
                //     selection stays on its name ---
                String want = { b->filter, b->filter_len };
                Arena_Clear(&b->view_arena);
                b->view = Arena_PushArray(&b->view_arena, s32, b->count + 1);
                b->view_count = b->view_dirs = b->view_files = 0;
                for (s64 k = 0; k < b->count; k++) {
                    const FileEntry *e = &b->entries[k];
                    if ((app->hide_hidden && (e->flags & File_Hidden)) || (want.count && !FilterMatch(e->name, want, NULL))) continue;
                    b->view[b->view_count++] = (s32)k;
                    if (e->flags & File_Dir) b->view_dirs++; else b->view_files++;
                }
                SortContext sc = { b->entries, b->path.count ? app->browse_sort : 0 };
                qsort_s(b->view, (size_t)b->view_count, sizeof(s32), CompareFileRows, &sc);
                b->view_dirty = 0;
                b->view_key = key;
                String keep = b->select_len ? (String){ b->select, b->select_len } : (String){ b->cursor_name, b->cursor_name_len };
                for (s64 r = 0; keep.count && r < b->view_count; r++) {
                    if (Str_EqualNoCase(BrowserEntry(b, r)->name, keep)) { b->cursor = r; b->reveal = b->select_len > 0; break; }
                }
                if (b->select_len && b->listed) b->select_len = 0;
                for (s64 r = 0; b->top_name_len && r < b->view_count; r++) {
                    if (Str_EqualNoCase(BrowserEntry(b, r)->name, (String){ b->top_name, b->top_name_len })) { b->top = r; break; }
                }
                b->top_name_len = 0;
            }
            b->cursor = Clamp(b->cursor, 0, Max(0, b->view_count - 1));
        }
    }
    END_TIMED_BLOCK(Browsers);

    // --- keys: bound commands first (each event on its own), then the overlays, then the focused terminal. `consumed`
    //     keeps a key from the terminal; ui->consumed keeps it from text edits too ---
    s64 event_count = Min(in->input.event_count, (s64)ArrayCount(ui->consumed));
    const UiKeyEvent *events = in->input.events;
    b32 *consumed = Arena_PushArray(fa, b32, event_count + 1);
    for (s64 i = 0; i < event_count; i++) {
        const UiKeyEvent *e = &events[i];
        if (!e->down || !e->key) continue;
        u8 mods = e->modifiers & (Mod_Ctrl | Mod_Alt | Mod_Shift);
        if (app->palette_open && app->palette_mode == PaletteMode_GoTo && e->key == Key_Up && mods == Mod_Alt) continue;   // the box's: the parent folder
        for (u32 cmd = 1; cmd < Cmd_Count; cmd++) {
            if (!app->bind_key[cmd] || app->bind_key[cmd] != e->key || app->bind_mods[cmd] != mods) continue;
            // focus: only what works inside the pane (its shell or browser); the rest reaches the program
            if (app->focus_mode && !(cmd == Cmd_FocusMode || cmd == Cmd_Copy || cmd == Cmd_Paste || cmd == Cmd_ZoomIn || cmd == Cmd_ZoomOut ||
                                     cmd == Cmd_ZoomReset || cmd == Cmd_Fullscreen || cmd == Cmd_ScrollPageUp || cmd == Cmd_ScrollPageDown ||
                                     cmd == Cmd_ScrollBottom || cmd == Cmd_ToggleBrowser || cmd == Cmd_Find || cmd == Cmd_Search ||
                                     cmd == Cmd_PrevCommand || cmd == Cmd_NextCommand || cmd == Cmd_GoTo || cmd == Cmd_Bookmark ||
                                     cmd == Cmd_ClearScrollback || cmd == Cmd_CopyAll || cmd == Cmd_Profile || cmd == Cmd_DebugOverlay)) break;
            consumed[i] = ui->consumed[i] = 1;
            if (app->queued_count < (s32)ArrayCount(app->queued)) app->queued[app->queued_count++] = cmd;
            break;
        }
        // Ctrl+C with a selection copies it (and the program doesn't get ^C)
        if (!consumed[i] && e->key == Key_A + 'C' - 'A' && mods == Mod_Ctrl && app->sel.visible) {
            consumed[i] = ui->consumed[i] = 1;
            if (app->queued_count < (s32)ArrayCount(app->queued)) app->queued[app->queued_count++] = Cmd_Copy;
            app->sel.visible = 0;
        }
    }
    for (s32 i = 0; i < app->queued_count; i++) RunCommand(app, &f, app->queued[i]);
    app->queued_count = 0;
    if (app->palette_open || app->confirm_tab || app->paste_confirm || app->reset_confirm || app->tab_menu) {
        for (s64 i = 0; i < event_count; i++) consumed[i] = 1;   // the popup takes the keyboard
    }
    b32 modal = app->palette_open || app->confirm_tab || app->paste_confirm || app->reset_confirm || app->tab_menu;

    // --- a tab being renamed has the keyboard: Enter keeps the name (empty: the server's again), Esc drops it ---
    b32 rename_done = 0;
    PaneTab *renaming = FindPaneTab(panes, app->rename_tab);
    if (!renaming) app->rename_tab = 0;
    if (app->rename_tab) {
        for (s64 i = 0; i < event_count; i++) {
            if (consumed[i]) continue;
            consumed[i] = 1;
            if (!events[i].down) continue;
            if (events[i].key == Key_Enter || events[i].key == Key_KeypadEnter) rename_done = 1;
            if (events[i].key == Key_Escape) app->rename_tab = 0;
        }
        if (in->input.button[Mouse_Left].pressed && ui->hot != Ui_Id("RenameTab")) rename_done = 1;   // a click elsewhere keeps it too
    }
    if (rename_done && renaming) {
        String name = TextEdit_String(&app->rename_edit);
        while (name.count && name.data[0] == ' ') { name.data++; name.count--; }
        while (name.count && name.data[name.count - 1] == ' ') name.count--;
        renaming->title_len = (s32)CopyInto(renaming->title, sizeof(renaming->title), name).count;
        app->rename_tab = 0;
    }

    // --- a web page's address being edited: Enter goes there, Esc drops it, a click elsewhere too ---
    PaneTab *web_editing = FindPaneTab(panes, app->web_edit_tab);
    if (!web_editing || !web_editing->web) app->web_edit_tab = 0;
    if (app->web_edit_tab) {
        for (s64 i = 0; i < event_count; i++) {
            if (consumed[i]) continue;
            consumed[i] = 1;
            if (!events[i].down) continue;
            if (events[i].key == Key_Enter || events[i].key == Key_KeypadEnter) { WebGo(web_editing, TextEdit_String(&app->web_edit)); app->web_edit_tab = 0; }
            if (events[i].key == Key_Escape) app->web_edit_tab = 0;
        }
        if (in->input.button[Mouse_Left].pressed && ui->hot != Ui_Id("WebAddress")) app->web_edit_tab = 0;
    }

    // --- the find bar: a click in the pane gives the keyboard back to the terminal, one on the bar takes it; while it
    //     has it, it takes every key the commands left ---
    PaneTab *find_tab = app->find_open ? Panes_FindTerm(panes, app->find_term) : NULL;
    if (app->find_open && (!find_tab || find_tab->hidden || find_tab->browsing)) app->find_open = 0;   // its tab went away, behind another, or to its files
    s32 find_step = 0;        // 2 older, 4 newer
    b32 find_close = 0;
    if (app->find_open && in->input.button[Mouse_Left].pressed && !modal) app->find_focus = RectHas(app->find_rect, mouse_x, mouse_y);
    if (app->find_open && app->find_focus && !modal) {
        for (s64 i = 0; i < event_count; i++) {
            const UiKeyEvent *e = &events[i];
            if (consumed[i]) continue;
            consumed[i] = 1;
            if (!e->down) continue;
            // Enter (Ctrl+F's bar): the next match, older; Shift+Enter the newer one; the bar keeps the keyboard
            if ((e->key == Key_Enter || e->key == Key_KeypadEnter) && !app->find_vim) {
                find_step = (e->modifiers & Mod_Shift) ? 4 : 2;
            }
            // Enter (Ctrl+/'s): done typing (typing went to the newest match already); then n / N step, as in vim
            else if (e->key == Key_Enter || e->key == Key_KeypadEnter) {
                app->find_focus = 0;
                if ((e->modifiers & Mod_Shift) || (!app->find_history && !(in->find_tab == app->find_term && in->find_index))) find_step = (e->modifiers & Mod_Shift) ? 4 : 2;
            }
            else if (e->key == Key_Up) find_step = 2;
            else if (e->key == Key_Down) find_step = 4;
            else if (e->key == Key_Escape) find_close = 1;
        }
    } else if (app->find_open && !modal && panes->selected && panes->selected->term_id == app->find_term && !panes->selected->browsing) {
        // --- after Enter, as vim: n older, N newer, / types again, Esc closes; any other key closes it and goes on ---
        for (s64 i = 0; i < event_count; i++) {
            const UiKeyEvent *e = &events[i];
            if (consumed[i] || !e->down || !e->key) continue;
            u8 m = e->modifiers & (Mod_Ctrl | Mod_Alt | Mod_Shift);
            if (e->key == Key_A + 'N' - 'A' && !(m & (Mod_Ctrl | Mod_Alt))) find_step = (m & Mod_Shift) ? 4 : 2;
            else if (e->key == Key_Slash && !m) { app->find_focus = 1; app->find_edit.mark = 0; app->find_edit.cursor = app->find_edit.count; }
            else if (e->key == Key_Escape && !m) find_close = 1;
            else { app->find_open = 0; continue; }
            consumed[i] = ui->consumed[i] = 1;
        }
    }
    // --- the reader: its tab gone, to its files or another tab: closed; read again when a turn ends. While its tab is the
    //     focused one it takes the keys (nothing types into the claude under it): Esc back, the arrows and pages scroll ---
    PaneTab *reader_tab = app->reader_open ? Panes_FindTerm(panes, app->reader_term) : NULL;
    const TermTab *reader_st = reader_tab ? FindServerTab(app, app->reader_term) : NULL;
    if (app->reader_open && (!reader_tab || !reader_st || reader_tab->browsing || reader_tab->viewing)) app->reader_open = 0;
    if (app->reader_open) {
        b32 busy = (reader_st->flags & TermTab_Busy) != 0;
        if (busy != app->reader_busy && !busy) out->history_get = app->reader_term;
        app->reader_busy = busy;
    }
    if (app->reader_open && reader_tab == panes->selected && !modal && !(app->compose_open && app->compose_term == app->reader_term) &&
        !(app->find_open && app->find_focus)) {
        s32 page = Max(1, (in->client_h - caption_h) * 4 / 5);
        for (s64 i = 0; i < event_count; i++) {
            const UiKeyEvent *e = &events[i];
            if (consumed[i]) continue;
            consumed[i] = 1;
            if (!e->down) continue;
            u8 m = e->modifiers & (Mod_Ctrl | Mod_Alt | Mod_Shift);
            if (e->key == Key_Escape && !m) app->reader_open = 0;
            else if (e->key == Key_Up) app->hist_scroll -= (f32)R(1.55) * 3;
            else if (e->key == Key_Down) app->hist_scroll += (f32)R(1.55) * 3;
            else if (e->key == Key_PageUp) app->hist_scroll -= (f32)page;
            else if (e->key == Key_PageDown || e->key == Key_Space) app->hist_scroll += (f32)page;
            else if (e->key == Key_Home) app->hist_scroll = 0;
            else if (e->key == Key_End) app->hist_scroll = 1e9f;
        }
    }
    // --- the composer has the keyboard while open on the focused tab: every key the commands left is its; Ctrl+Enter
    //     sends (a paste, then Enter), Esc puts it away (the draft kept), Ctrl+V asks for a picture too ---
    PaneTab *compose_tab = app->compose_open ? Panes_FindTerm(panes, app->compose_term) : NULL;
    if (app->compose_open && (!compose_tab || compose_tab->browsing || compose_tab->viewing)) app->compose_open = 0;
    if (in->pasted_image.count && app->compose_open) {
        b32 quote = memchr(in->pasted_image.data, ' ', (size_t)in->pasted_image.count) != NULL;
        TextArea_Insert(&app->compose, Str_PushFormat(fa, quote ? "\"%.*s\" " : "%.*s ", STR_ARG(in->pasted_image)));
    }
    if (app->compose_open && compose_tab == panes->selected && !modal) {
        for (s64 i = 0; i < event_count; i++) {
            const UiKeyEvent *e = &events[i];
            if (consumed[i]) continue;
            consumed[i] = 1;
            if (!e->down) continue;
            u8 m = e->modifiers & (Mod_Ctrl | Mod_Alt | Mod_Shift);
            if ((e->key == Key_Enter || e->key == Key_KeypadEnter) && m == Mod_Ctrl) {
                const TermGrid *g = FindGrid(in, app->compose_term);
                String text = TextArea_String(&app->compose);
                if (text.count) {
                    TermSend(&f, app->compose_term, PasteBytes(fa, text, g && (g->flags & TermGrid_BracketedPaste)));
                    TermSend(&f, app->compose_term, S("\r"));
                    Logf("app", "composer: sent %lld bytes", (long long)text.count);
                }
                app->compose.count = app->compose.cursor = app->compose.mark = app->compose.top = app->compose.undo_len = 0;
                app->compose_open = 0;
                ui->consumed[i] = 1;
            } else if (e->key == Key_Escape && !m) {
                app->compose_open = 0;
                ui->consumed[i] = 1;
            } else if ((e->key == Key_A + 'V' - 'A' && m == Mod_Ctrl) || (e->key == Key_Insert && m == Mod_Shift)) {
                out->save_clipboard_image = 1;   // the widget pastes the text; a picture's path comes next frame
            }
        }
    }
    // smartcase, as vim's: an uppercase letter matches case
    app->find_case = 0;
    for (s32 k = 0; k < app->find_edit.count; k++) app->find_case |= app->find_edit.buffer[k] >= 'A' && app->find_edit.buffer[k] <= 'Z';

    // --- the sessions sidebar: Claude Code's sessions newest first, narrowed by what's typed (project, first prompt,
    //     folder). While it has the keyboard it takes every key the commands left ---
    s64 side_count = 0;
    s32 *side_rows = NULL;   // indices into in->sessions
    s32 side_row_h = R(3.4), side_head_h = R(2.2);
    if (app->sidebar_open) {
        side_rows = Arena_PushArray(fa, s32, in->session_count + 1);
        String want = { app->side_filter, app->side_filter_len };
        if (in->sessions_meta_text.count) {
            // sessions.txt, read once at start
            String text = in->sessions_meta_text;
            for (s64 at = 0; at < text.count;) {
                s64 end = at;
                while (end < text.count && text.data[end] != '\n') end++;
                String line = { text.data + at, end - at };
                at = end + 1;
                while (line.count && strchr("\r \t", line.data[line.count - 1])) line.count--;
                s64 sp = 0;
                while (sp < line.count && line.data[sp] != ' ') sp++;
                if (sp >= line.count || line.data[0] == '#') continue;
                String kind = { line.data, sp }, rest = { line.data + sp + 1, line.count - sp - 1 };
                s64 eq = 0;
                while (eq < rest.count && rest.data[eq] != ' ') eq++;
                s32 mi = SessionMetaIndex(app, (String){ rest.data, eq }, 1);
                if (mi < 0) continue;
                if (Str_Equal(kind, S("pin"))) app->side_meta[mi].pin = 1;
                else if (Str_Equal(kind, S("archive"))) app->side_meta[mi].archive = 1;
                else if (Str_Equal(kind, S("name")) && eq + 3 <= rest.count) {
                    String name = { rest.data + eq + 3, rest.count - eq - 3 };
                    app->side_meta[mi].name_len = (s32)CopyInto(app->side_meta[mi].name, sizeof(app->side_meta[mi].name), name).count;
                }
            }
        }
        // every transcript searched once the text has been still a moment (3 characters or more; fewer: none)
        if (!Str_Equal(want, (String){ app->side_sent, app->side_sent_len })) {
            if (want.count >= 3 && (now - app->side_typed_us >= 250000 || (app->headless && ui->frame - app->side_typed_frame >= 2))) {
                out->session_search_set = 1;
                out->session_search = want;
                app->side_sent_len = (s32)CopyInto(app->side_sent, sizeof(app->side_sent), want).count;
            } else if (want.count < 3 && app->side_sent_len) {
                out->session_search_set = 1;
                app->side_sent_len = 0;
            } else if (want.count >= 3 && (!out->wake_at_us || app->side_typed_us + 250000 < out->wake_at_us)) {
                out->wake_at_us = app->side_typed_us + 250000;
            }
        }
        b32 hits_now = want.count >= 3 && Str_Equal(in->session_hits_text, want);
        for (s32 pass = 0; pass < 2; pass++) {   // the pinned first
            for (s64 i = 0; i < in->session_count; i++) {
                const TermSession *ss = &in->sessions[i];
                s32 mi = SessionMetaIndex(app, ss->id, 0);
                b32 pinned = mi >= 0 && app->side_meta[mi].pin, archived = mi >= 0 && app->side_meta[mi].archive;
                if (pinned != (pass == 0) || (archived && !want.count)) continue;
                String name = mi >= 0 ? (String){ app->side_meta[mi].name, app->side_meta[mi].name_len } : (String){0};
                b32 hit = 0;
                for (s64 h = 0; hits_now && h < in->session_hit_count && !hit; h++) hit = Str_Equal(in->session_hits[h].id, ss->id);
                if (want.count && !hit && !ContainsNoCase(ss->project, want) && !ContainsNoCase(ss->title, want) && !ContainsNoCase(ss->cwd, want) &&
                    !ContainsNoCase(name, want)) continue;
                side_rows[side_count++] = (s32)i;
            }
        }
        if (in->input.button[Mouse_Left].pressed && !modal) app->side_focus = mouse_x < sidebar_w;
        if (app->side_focus) {
            app->side_cursor = Clamp(app->side_cursor, 0, Max(0, side_count - 1));
        } else {
            // without the keyboard it marks the focused tab's session (none for a tab that isn't a claude), in view
            const TermTab *ft = panes->selected ? FindServerTab(app, panes->selected->term_id) : NULL;
            s64 at = -1;
            for (s64 k = 0; ft && ft->key.count && k < side_count && at < 0; k++) if (Str_Equal(in->sessions[side_rows[k]].id, ft->key)) at = k;
            if (at >= 0 && at != app->side_cursor) app->side_reveal = 1;
            app->side_cursor = at;
        }
    }
    if (app->sidebar_open && app->side_rename_len && !modal) {
        // --- a session's name edited in its row: Enter keeps it (empty: its first prompt again), Esc drops it ---
        for (s64 i = 0; i < event_count; i++) {
            const UiKeyEvent *e = &events[i];
            if (consumed[i]) continue;
            consumed[i] = 1;
            if (!e->down) continue;
            if (e->key == Key_Enter || e->key == Key_KeypadEnter) {
                s32 mi = SessionMetaIndex(app, (String){ app->side_rename_id, app->side_rename_len }, 1);
                if (mi >= 0) app->side_meta[mi].name_len = (s32)CopyInto(app->side_meta[mi].name, sizeof(app->side_meta[mi].name), TextEdit_String(&app->side_rename_edit)).count;
                out->sessions_meta_text = SessionMetaText(app, fa);
                app->side_rename_len = 0;
                ui->consumed[i] = 1;
            } else if (e->key == Key_Escape) {
                app->side_rename_len = 0;
                ui->consumed[i] = 1;
            }
        }
    }
    if (app->sidebar_open && app->side_focus && !app->side_rename_len && !modal) {
        s64 page = Max(1, (in->client_h - caption_h - R(3)) / side_row_h - 1);
        for (s64 i = 0; i < event_count; i++) {
            const UiKeyEvent *e = &events[i];
            if (consumed[i] || !e->down) continue;
            consumed[i] = 1;
            u8 mods = e->modifiers & (Mod_Ctrl | Mod_Alt | Mod_Shift);
            s64 to = app->side_cursor;
            if (e->key == Key_Up) to--;
            else if (e->key == Key_Down) to++;
            else if (e->key == Key_PageUp) to -= page;
            else if (e->key == Key_PageDown) to += page;
            else if (e->key == Key_Home) to = 0;
            else if (e->key == Key_End) to = side_count - 1;
            else if (e->key == Key_Enter || e->key == Key_KeypadEnter) {
                if (side_count) { OpenSession(app, &f, &in->sessions[side_rows[app->side_cursor]]); app->side_focus = 0; }
            } else if (e->key == Key_Escape) {
                if (app->side_filter_len) { app->side_filter_len = 0; app->side_typed_us = now; app->side_typed_frame = ui->frame; }
                else app->side_focus = 0;
            } else if (e->key == Key_Backspace) {
                if (app->side_filter_len) {
                    do app->side_filter_len--; while (app->side_filter_len > 0 && (app->side_filter[app->side_filter_len] & 0xC0) == 0x80);
                    to = 0;
                    app->side_typed_us = now; app->side_typed_frame = ui->frame;
                }
            } else if (e->text.count && !(mods & (Mod_Ctrl | Mod_Alt)) && e->text.data[0] >= 0x20 && e->text.data[0] != 0x7F &&
                       app->side_filter_len + e->text.count <= (s64)sizeof(app->side_filter)) {
                memcpy(app->side_filter + app->side_filter_len, e->text.data, (size_t)e->text.count);
                app->side_filter_len += (s32)e->text.count;
                to = 0;
                app->side_typed_us = now; app->side_typed_frame = ui->frame;
                app->side_top = 0;
            }
            if (to != app->side_cursor) { app->side_cursor = Clamp(to, 0, Max(0, side_count - 1)); app->side_reveal = 1; }
        }
    }

    // --- keys for the focused pane ---
    PaneTab *focus = panes->selected;
    const TermGrid *focus_grid = focus ? FindGrid(in, focus->term_id) : NULL;
    if (focus && focus->web) {
        // --- a web page: the page has the keyboard (the platform's); what reaches the window here is Ctrl+L / F6 (the
        //     address), F5 (reload), Alt+Left / Right stay pane focus. Nothing goes to the shell under it ---
        for (s64 i = 0; i < event_count; i++) {
            const UiKeyEvent *e = &events[i];
            if (consumed[i] || !e->down) continue;
            consumed[i] = ui->consumed[i] = 1;
            if ((e->key == Key_A + 'L' - 'A' && (e->modifiers & Mod_Ctrl)) || e->key == Key_F1 + 5) StartWebEdit(app, in, focus);
            if (e->key == Key_F1 + 4) focus->web_reload = 1;
        }
    } else if (focus && focus->viewing && focus->viewer) {
        // --- the picture view: arrows, Space / Backspace, PgUp / PgDn, Home / End step through the folder's pictures
        //     (round); Esc, Enter or q go back; every other key stays here ---
        Viewer *v = focus->viewer;
        for (s64 i = 0; i < event_count; i++) {
            const UiKeyEvent *e = &events[i];
            if (consumed[i] || !e->down) continue;
            consumed[i] = ui->consumed[i] = 1;
            s64 at = 0;
            while (at < v->count && !Str_EqualNoCase(v->names[at], v->name)) at++;
            s64 to = at;
            if (e->key == Key_Right || e->key == Key_Down || e->key == Key_Space || e->key == Key_PageDown) to = at + 1;
            else if (e->key == Key_Left || e->key == Key_Up || e->key == Key_Backspace || e->key == Key_PageUp) to = at - 1;
            else if (e->key == Key_Home) to = 0;
            else if (e->key == Key_End) to = v->count - 1;
            else if (e->key == Key_Escape || e->key == Key_Enter || e->key == Key_KeypadEnter || (e->key == Key_A + 'Q' - 'A' && !(e->modifiers & (Mod_Ctrl | Mod_Alt)))) {
                focus->viewing = 0;
                if (!v->from_browser && focus->browsing) CloseBrowser(app, &f, focus);
                break;
            }
            if (v->count && at < v->count && to != at) v->name = CopyInto(v->name_buf, sizeof(v->name_buf), v->names[(to % v->count + v->count) % v->count]);
        }
    } else if (focus && focus->browsing && focus->browser) {
        // --- the browser: move, open, up, type to filter. Delete, rename (F2), new folder (Ctrl+Shift+N) and paste
        //     change the disk: refused while the tab is busy ---
        Browser *b = focus->browser;
        const TermTab *st = FindServerTab(app, focus->term_id);
        for (s64 i = 0; i < event_count; i++) {
            const UiKeyEvent *e = &events[i];
            if (consumed[i] || !e->down) continue;
            u8 mods = e->modifiers & (Mod_Ctrl | Mod_Alt | Mod_Shift);
            FileEntry *cur = BrowserEntry(b, b->cursor);
            if (b->renaming) {
                // the row's text edit takes the other keys; Enter renames, Escape doesn't
                if (e->key == Key_Escape) { b->renaming = 0; consumed[i] = ui->consumed[i] = 1; }
                if ((e->key == Key_Enter || e->key == Key_KeypadEnter) && cur) {
                    String name = TextEdit_String(&b->rename);
                    b32 valid = name.count > 0 && !Str_Equal(name, cur->name);
                    for (s64 k = 0; k < name.count && valid; k++) valid = !strchr("\\/:*?\"<>|", name.data[k]) && name.data[k] >= 0x20;
                    if (valid && !TabBusy(st)) {
                        FileOp *op = Array_Push(fa, f.file_ops);
                        op->kind = FileOp_Rename;
                        op->path = EntryPath(fa, b, cur);
                        op->name = Str_PushCopy(fa, name);
                        b->select_len = (s32)CopyInto(b->select, sizeof(b->select), name).count;
                    }
                    b->renaming = 0;
                    consumed[i] = ui->consumed[i] = 1;
                }
                continue;
            }
            consumed[i] = 1;
            s64 to = b->cursor;
            b32 handled = 1;
            u32 change = 0;   // a FileOp_* this key asks for
            if (e->key == Key_Up) to = b->cursor - 1;
            else if (e->key == Key_Down) to = b->cursor + 1;
            else if (e->key == Key_PageUp) to = b->cursor - Max(1, browser_rows - 1);
            else if (e->key == Key_PageDown) to = b->cursor + Max(1, browser_rows - 1);
            else if (e->key == Key_Home) to = 0;
            else if (e->key == Key_End) to = b->view_count - 1;
            else if (e->key == Key_Enter || e->key == Key_KeypadEnter) { if (cur) f.nav_tab = focus, f.nav_open = b->view[b->cursor]; }
            else if (e->key == Key_Backspace) {
                if (b->filter_len) {
                    do b->filter_len--; while (b->filter_len > 0 && (b->filter[b->filter_len] & 0xC0) == 0x80);
                    b->cursor_name_len = 0;
                    to = 0;
                } else {
                    f.nav_tab = focus;
                    f.nav_up = 1;
                }
            } else if (e->key == Key_A && mods == Mod_Ctrl) {
                SelRange(b, 0, b->view_count - 1);
            } else if (e->key == Key_A + 'H' - 'A' && mods == Mod_Ctrl) {
                app->hide_hidden = !app->hide_hidden;
                BrowserNotice(b, now, app->hide_hidden ? S("Hidden files left out") : S("Hidden files shown"));
                SetSetting(out, S("hidden_files"), app->hide_hidden ? S("off") : S("on"));
            } else if (e->key == Key_Escape) {
                if (b->sel.count) b->sel.count = 0;
                else if (b->filter_len) b->filter_len = 0;
                else CloseBrowser(app, &f, focus);
            } else if (e->key == Key_A + 'C' - 'A' && mods == Mod_Ctrl) BrowserCopy(&f, focus);
            else if (e->key == Key_Delete && !mods) change = FileOp_Delete;
            else if (e->key == Key_F1 + 1 && !mods) change = FileOp_Rename;
            else if (e->key == Key_A + 'N' - 'A' && mods == (Mod_Ctrl | Mod_Shift)) change = FileOp_NewFolder;
            else if (e->key == Key_A + 'V' - 'A' && mods == Mod_Ctrl) change = FileOp_Paste;
            // the go-to box: Ctrl+L / Alt+D with this folder typed in, Ctrl+P / F4 empty (the groups)
            else if ((e->key == Key_A + 'L' - 'A' && mods == Mod_Ctrl) || (e->key == Key_A + 'D' - 'A' && mods == Mod_Alt))
                OpenGoTo(app, focus, b->path.count ? Str_PushFormat(fa, "%.*s\\", STR_ARG(b->path)) : (String){0});
            else if ((e->key == Key_A + 'P' - 'A' && mods == Mod_Ctrl) || (e->key == Key_F1 + 3 && !mods)) OpenGoTo(app, focus, (String){0});
            else if (e->key == Key_A + 'D' - 'A' && mods == Mod_Ctrl && b->path.count) {
                ToggleBookmark(app, b->path);
                BrowserNotice(b, now, IsBookmarked(app, b->path) ? S("Bookmarked") : S("Bookmark taken away"));
            }
            else handled = 0;

            if (change && TabBusy(st)) {
                static const char *const what[5] = { "", "Delete", "Rename", "New folder", "Paste" };
                BrowserNotice(b, now, Str_PushFormat(fa, "%s is off: the tab is busy", what[change]));
            } else if (change && b->path.count) {
                b32 on_item = cur && !(cur->flags & File_Drive);
                if (change == FileOp_Delete) {
                    String *paths = NULL, first = {0};
                    s64 n = SelectedPaths(fa, b, &paths, &first);
                    for (s64 k = 0; k < n; k++) {
                        FileOp *op = Array_Push(fa, f.file_ops);
                        op->kind = FileOp_Delete;
                        op->path = paths[k];
                    }
                    if (n) BrowserNotice(b, now, n == 1 ? Str_PushFormat(fa, "%.*s went to the Recycle Bin", STR_ARG(first)) :
                                                          Str_PushFormat(fa, "%lld items went to the Recycle Bin", (long long)n));
                    b->sel.count = 0;
                } else if (change == FileOp_Rename && on_item) {
                    // the name selected up to its extension
                    TextEdit_Set(&b->rename, cur->name);
                    s64 dot = cur->name.count;
                    while (!(cur->flags & File_Dir) && dot > 1 && cur->name.data[dot - 1] != '.') dot--;
                    b->rename.cursor = (cur->flags & File_Dir) || dot <= 1 ? b->rename.count : (s32)dot - 1;
                    b->renaming = 1;
                } else if (change == FileOp_NewFolder) {
                    String name = S("New folder");
                    for (s32 k = 2; k < 100; k++) {
                        b32 taken = 0;
                        for (s64 x = 0; x < b->count && !taken; x++) taken = Str_EqualNoCase(b->entries[x].name, name);
                        if (!taken) break;
                        name = Str_PushFormat(fa, "New folder (%d)", k);
                    }
                    FileOp *op = Array_Push(fa, f.file_ops);
                    op->kind = FileOp_NewFolder;
                    op->path = Str_PushFormat(fa, "%.*s\\%.*s", STR_ARG(b->path), STR_ARG(name));
                    b->select_len = (s32)CopyInto(b->select, sizeof(b->select), name).count;
                } else if (change == FileOp_Paste) {
                    FileOp *op = Array_Push(fa, f.file_ops);
                    op->kind = FileOp_Paste;
                    op->path = ShellPath(fa, b->path);
                }
            }
            if (!handled && e->text.count && !(mods & (Mod_Ctrl | Mod_Alt)) && e->text.data[0] >= 0x20 && e->text.data[0] != 0x7F &&
                b->filter_len + e->text.count <= (s64)sizeof(b->filter)) {
                memcpy(b->filter + b->filter_len, e->text.data, (size_t)e->text.count);
                b->filter_len += (s32)e->text.count;
                b->cursor_name_len = 0;   // the first match comes first
                to = 0;
                b->top = 0;
            }
            if (to != b->cursor) {
                // Shift extends the selection from the anchor, a plain move leaves just the new row
                to = Clamp(to, 0, Max(0, b->view_count - 1));
                if (mods & Mod_Shift) SelRange(b, b->anchor, to);
                else { b->sel.count = 0; b->anchor = to; }
                b->cursor = to;
                b->reveal = 1;
            }
        }
    } else if (focus && focus->term_id) {
        // --- the terminal: special keys as sequences, the rest as typed text ---
        b32 app_cursor = focus_grid && (focus_grid->flags & TermGrid_AppCursorKeys);
        // win32-input-mode (ConPTY asks for it): every key as the console key event it is, presses and releases, so
        // console programs (PSReadLine, far, vim) see what was really pressed
        b32 win32 = focus_grid && (focus_grid->flags & TermGrid_Win32Input) && !app->win32_input_off;
        for (s64 i = 0; i < event_count; i++) {
            const UiKeyEvent *e = &events[i];
            u32 bit = 1u << (e->vk % 32);
            if (consumed[i]) { if (e->down && e->vk) app->vk_withheld[(e->vk / 32) % 8] |= bit; continue; }
            if (win32 && e->up && e->vk) {
                b32 withheld = (app->vk_withheld[(e->vk / 32) % 8] & bit) != 0;
                app->vk_withheld[(e->vk / 32) % 8] &= ~bit;
                if (!withheld) TermSend(&f, focus->term_id, Str_PushFormat(fa, "\x1b[%u;%u;0;0;%u;1_", e->vk, e->scan, e->control_state));
                continue;
            }
            if (!e->down) continue;
            String bytes = e->key ? SpecialKey(fa, e, app_cursor) : (String){0};
            if (!bytes.count && e->text.count) {
                b32 alt_only = (e->modifiers & Mod_Alt) && !(e->modifiers & Mod_Ctrl);
                bytes = alt_only ? Str_PushFormat(fa, "\x1b%.*s", STR_ARG(e->text)) : e->text;
            }
            b32 newline = (e->key == Key_Enter || e->key == Key_KeypadEnter) && (e->modifiers & (Mod_Shift | Mod_Alt));
            if (win32 && e->vk && !newline) {
                // the key itself (its text as the character, UTF-16)
                u32 uc = 0;
                if (e->text.count) { s64 at = 0; uc = Utf8_Decode(e->text, &at); }
                bytes = Str_PushFormat(fa, "\x1b[%u;%u;%u;1;%u;1_", e->vk, e->scan, uc < 0x10000 ? uc : 0, e->control_state);
            } else if (win32 && bytes.count) {
                // characters with no key behind them (an IME's, Shift+Enter's ESC CR for agents): each pressed and let go
                TextBuilder tb = { fa, NULL, 0, 0 };
                for (s64 at = 0; at < bytes.count;) {
                    u32 cp = Utf8_Decode(bytes, &at);
                    u32 units[2] = { cp, 0 };
                    s32 n = 1;
                    if (cp >= 0x10000) { units[0] = 0xD800 + ((cp - 0x10000) >> 10); units[1] = 0xDC00 + ((cp - 0x10000) & 0x3FF); n = 2; }
                    for (s32 k = 0; k < n; k++) TB_Format(&tb, "\x1b[0;0;%u;1;0;1_\x1b[0;0;%u;0;0;1_", units[k], units[k]);
                }
                bytes = TB_String(&tb);
            }
            if (!bytes.count) continue;
            for (u32 r = 0; r < Max(1u, e->repeat_count); r++) TermSend(&f, focus->term_id, bytes);
            for (PaneNode *n = app->broadcast ? panes->root : NULL; n; n = Panes_NextNode(n)) {
                // broadcast: every other terminal shown in the window gets it too
                PaneTab *o = n->first_child ? NULL : Panes_VisibleTab(n);
                if (!o || !o->term_id || o->browsing || o == focus) continue;
                for (u32 r = 0; r < Max(1u, e->repeat_count); r++) TermSend(&f, o->term_id, bytes);
            }
            app->blink_start_us = now;
            if (app->sel.visible && app->sel.term_id == focus->term_id) app->sel.visible = 0;   // typing clears the selection
            if (focus_grid && focus_grid->scroll_offset) TermScrollBy(&f, focus->term_id, TermScroll_ToBottom);
        }
    }

    // --- files dragged in from another program: copied into a browser's folder (refused while its tab is busy), or
    //     their paths typed into a terminal, quoted where they have spaces ---
    if (in->drag_over || in->drop_count) {
        PaneTab *target = NULL;
        for (PaneNode *n = panes->root; n; n = Panes_NextNode(n)) {
            if (!n->first_child && mouse_x >= n->px[0] && mouse_x < n->px[2] && mouse_y >= n->px[1] && mouse_y < n->px[3]) target = Panes_VisibleTab(n);
        }
        const TermTab *st = target ? FindServerTab(app, target->term_id) : NULL;
        u32 effect = 0;
        if (target && target->browsing && target->browser) {
            effect = !TabBusy(st) && target->browser->path.count ? 1 : 0;
            if (TabBusy(st)) BrowserNotice(target->browser, now, Str_PushFormat(fa, "Drop is off: read-only while %.*s", STR_ARG(BusyReason(fa, st))));
        } else if (target && target->term_id) {
            effect = 2;
        }
        out->drop_effect = effect;
        if (in->drop_count && effect) {
            Panes_Select(panes, target);
            if (effect == 1) {
                TextBuilder list = { fa, NULL, 0, 0 };
                for (s64 i = 0; i < in->drop_count; i++) TB_Format(&list, "%s%.*s", i ? "\n" : "", STR_ARG(in->dropped[i]));
                FileOp *op = Array_Push(fa, f.file_ops);
                op->kind = FileOp_CopyIn;
                op->path = ShellPath(fa, target->browser->path);
                op->name = TB_String(&list);
                BrowserNotice(target->browser, now, in->drop_count == 1 ? S("Copying 1 item here") : Str_PushFormat(fa, "Copying %lld items here", (long long)in->drop_count));
            } else {
                TextBuilder typed = { fa, NULL, 0, 0 };
                for (s64 i = 0; i < in->drop_count; i++) {
                    b32 quote = memchr(in->dropped[i].data, ' ', (size_t)in->dropped[i].count) != NULL;
                    TB_Format(&typed, quote ? "\"%.*s\" " : "%.*s ", STR_ARG(in->dropped[i]));
                }
                if (app->compose_open && app->compose_term == target->term_id) TextArea_Insert(&app->compose, TB_String(&typed));
                else TermSend(&f, target->term_id, TB_String(&typed));
            }
        }
    }

    // --- a paste: the clipboard's text came in; several lines into a program that didn't ask for bracketed paste wait
    //     for a yes (a shell would run each line) ---
    if (app->paste_wait && in->clipboard_text.count) {
        const TermGrid *pg = FindGrid(in, app->paste_wait);
        b32 bracketed = pg && (pg->flags & TermGrid_BracketedPaste);
        s32 lines = 1;
        for (s64 i = 0; i + 1 < in->clipboard_text.count; i++) lines += in->clipboard_text.data[i] == '\n';
        if (lines > 1 && !bracketed && !app->paste_warning_off) {
            Arena_Clear(&app->paste_arena);
            app->paste_text = Str_PushCopy(&app->paste_arena, in->clipboard_text);
            app->paste_confirm = app->paste_wait;
        } else {
            TermSend(&f, app->paste_wait, PasteBytes(fa, in->clipboard_text, bracketed));
        }
        app->paste_wait = 0;
    }

    // --- tab drag over the panes: over a strip the tab goes into it (a gap opens); elsewhere drop buttons appear where a
    //     drop can land: the window's edges, a cross in the pane under the pointer (split it, or join it), and the
    //     borders of every group around it (a new pane between two). Only a button takes the drop; its preview grows
    //     from where the new pane will be. Worked out before the panes are drawn (their strips open the gap), drawn
    //     after them ---
    typedef struct DropButton {
        Rect r;
        u64 node_id;            // the node the new pane goes next to (0: the root, whatever it is then)
        b32 into;               // join the node's pane as a tab
        b32 vertical;           // the new pane beside (else above / below)
        b32 after;              // right of / below the node
        s32 cells;              // 1 the whole button, 2 new + pane (or pane + new), 3 pane + new + pane
        b32 new_first;          // with 2 cells: the new one first
        f32 preview[4];         // where the tab lands (pixels)
        f32 grow_scale[2], grow_pivot[2];   // its preview grows from there
        const char *name;
    } DropButton;
    DropButton buttons[64];
    s32 button_count = 0, chosen = -1;
    b32 strip_drop = 0;
    u64 strip_drop_node = 0, strip_drop_before = 0;
    PaneTab *drag_tab = FindPaneTab(panes, app->tab_drag_id);
    b32 drag_outside = mouse_x < 0 || mouse_y < 0 || mouse_x >= in->client_w || mouse_y >= in->client_h;
    b32 foreign = in->foreign_drag || in->foreign_drop;   // another window's tab: the same buttons, no tab of ours lifted
    if (app->tab_drag_id || foreign) {
        s32 mx = mouse_x, my = mouse_y;
        b32 in_strip = 0;
        PaneNode *strip_target = NULL;
        for (PaneNode *n = panes->root; n && (drag_tab || foreign) && !drag_outside && !in_strip; n = Panes_NextNode(n)) {
            if (!n->first_tab || mx < n->px[0] || mx >= n->px[2] || my < n->px[1] || my >= n->px[1] + caption_h) continue;
            in_strip = 1;
            b32 own = drag_tab && drag_tab->node == n, none = 0;
            PaneTab *before = NULL;
            for (PaneTab *x = n->first_tab; x && !before && !none; x = x->next) {
                if (x == drag_tab && mx >= x->strip_x[0] && mx < x->strip_x[1]) none = own;
                else if (mx < (x->strip_x[0] + x->strip_x[1]) / 2) before = x;
            }
            if (own && ((before && (before == drag_tab || before == drag_tab->next)) || (!before && !drag_tab->next))) none = 1;
            if (!none) {
                strip_target = n;
                strip_drop = 1;
                strip_drop_node = n->id;
                strip_drop_before = before ? before->id : 0;
            }
        }

        // --- the buttons: where each sits, the cells drawn in it (the new pane's cell lit), what a drop does ---
        s32 W6 = R(6), W8 = R(8), W10 = R(10), gap = R(1), edge_pad = R(0.6), band = R(8);
        b32 only_tab = drag_tab && !panes->root->first_child && panes->root->first_tab == drag_tab && !drag_tab->next;
        PaneNode *leaf = NULL;   // the pane under the pointer
        for (PaneNode *n = panes->root; n && !drag_outside; n = Panes_NextNode(n)) {
            if (!n->first_child && mx >= n->px[0] && mx < n->px[2] && my >= n->px[1] && my < n->px[3]) leaf = n;
        }
#define ALONE(n) ((n) && drag_tab && !(n)->first_child && (n)->first_tab == drag_tab && !drag_tab->next)
        if ((drag_tab || foreign) && !drag_outside && !only_tab) {
            f32 top = (f32)caption_h, W = (f32)area_w, H = (f32)area_h;
            // --- the window's edges: a pane the whole window wide (or high) at that side, across the root's split ---
            for (s32 k = 0; k < 4; k++) {
                b32 side_by = k < 2;
                b32 root_stacked = panes->root->first_child && !panes->root->vertical;
                if (side_by != root_stacked) continue;
                s32 bw = side_by ? W6 : W10, bh = side_by ? W10 : W6;
                s32 x0 = area_x + (k == 0 ? edge_pad : k == 1 ? area_w - edge_pad - bw : (area_w - bw) / 2);
                s32 y0 = k == 2 ? caption_h - R(0.5) : k == 3 ? area_h - edge_pad - bh : (caption_h + area_h - bh) / 2;
                if (button_count >= (s32)ArrayCount(buttons)) continue;
                DropButton *b = &buttons[button_count++];
                memset(b, 0, sizeof(*b));
                b->r = R4(x0, y0, x0 + bw, y0 + bh);
                b->vertical = side_by;
                b->after = k == 1 || k == 3;
                b->cells = 2;
                b->new_first = !b->after;
                f32 *pv = b->preview;
                pv[0] = (f32)area_x; pv[1] = top; pv[2] = (f32)area_x + W; pv[3] = H;
                if (k == 0) pv[2] = (f32)area_x + (f32)band;
                if (k == 1) pv[0] = (f32)area_x + W - (f32)band;
                if (k == 2) pv[3] = top + (f32)band;
                if (k == 3) pv[1] = H - (f32)band;
                b->grow_scale[0] = side_by ? 0.2f : 0.8f; b->grow_scale[1] = side_by ? 0.8f : 0.2f;
                b->grow_pivot[0] = k == 0 ? 0.0f : k == 1 ? 1.0f : 0.5f; b->grow_pivot[1] = k == 2 ? 0.0f : k == 3 ? 1.0f : 0.5f;
                static const char *const root_names[4] = { "drop root left", "drop root right", "drop root top", "drop root bottom" };
                b->name = root_names[k];
            }
            // --- the borders of each group around the pane: a new pane before / after the child it's in ---
            for (PaneNode *cn = leaf; cn && cn->parent; cn = cn->parent) {
                PaneNode *g = cn->parent;
                for (s32 after = 0; after < 2; after++) {
                    PaneNode *other = after ? cn->next : cn->prev;
                    if (ALONE(cn) || ALONE(other)) continue;   // next to a pane that leaves with the tab
                    b32 side_by = g->vertical;
                    s32 bw = side_by ? W8 : W6, bh = side_by ? W6 : W8;
                    f32 at = side_by ? (after ? cn->px[2] : cn->px[0]) : (after ? cn->px[3] : cn->px[1]);
                    f32 mid = side_by ? (cn->px[1] + caption_h + cn->px[3]) * 0.5f : (cn->px[0] + cn->px[2]) * 0.5f;
                    s32 x0 = side_by ? RoundS32(at) - bw / 2 : RoundS32(mid) - bw / 2;
                    s32 y0 = side_by ? RoundS32(mid) - bh / 2 : RoundS32(at) - bh / 2;
                    if (!other) {   // the group's first / last edge: kept inside it
                        if (side_by) x0 = after ? RoundS32(at) - bw - edge_pad : RoundS32(at) + edge_pad;
                        else y0 = after ? RoundS32(at) - bh - edge_pad : RoundS32(at) + caption_h + edge_pad;
                    }
                    if (button_count >= (s32)ArrayCount(buttons)) continue;
                    DropButton *b = &buttons[button_count++];
                    memset(b, 0, sizeof(*b));
                    b->r = R4(x0, y0, x0 + bw, y0 + bh);
                    b->node_id = cn->id;
                    b->vertical = side_by;
                    b->after = after;
                    b->cells = other ? 3 : 2;
                    b->new_first = !after;
                    f32 *pv = b->preview;
                    if (side_by) { pv[0] = at - (f32)band / 2; pv[2] = at + (f32)band / 2; pv[1] = cn->px[1]; pv[3] = cn->px[3]; }
                    else { pv[1] = at - (f32)band / 2; pv[3] = at + (f32)band / 2; pv[0] = cn->px[0]; pv[2] = cn->px[2]; }
                    if (!other && side_by) { if (after) { pv[0] = at - (f32)band; pv[2] = at; } else { pv[0] = at; pv[2] = at + (f32)band; } }
                    if (!other && !side_by) { if (after) { pv[1] = at - (f32)band; pv[3] = at; } else { pv[1] = at; pv[3] = at + (f32)band; } }
                    b->grow_scale[0] = side_by ? 0.2f : 0.8f; b->grow_scale[1] = side_by ? 0.8f : 0.2f;
                    b->grow_pivot[0] = 0.5f; b->grow_pivot[1] = 0.5f;
                    b->name = after ? "drop border after" : "drop border before";
                }
            }
            // --- the pane's cross: its halves (across its group's split, or left / right) and its middle ---
            if (leaf && !ALONE(leaf)) {
                b32 across = leaf->parent && leaf->parent->vertical;   // in a side-by-side group it splits top / bottom (borders add beside)
                f32 cx = (leaf->px[0] + leaf->px[2]) * 0.5f, cy = (leaf->px[1] + caption_h + leaf->px[3]) * 0.5f;
                for (s32 k = 0; k < 3; k++) {
                    s32 bw = k == 1 ? W8 : across ? W8 : W6, bh = k == 1 ? W8 : across ? W6 : W8;
                    s32 off = k == 1 ? 0 : (k == 0 ? -1 : 1) * (W8 / 2 + gap + W6 / 2);
                    s32 x0 = RoundS32(cx) - bw / 2 + (across ? 0 : off), y0 = RoundS32(cy) - bh / 2 + (across ? off : 0);
                    if (button_count >= (s32)ArrayCount(buttons)) continue;
                    DropButton *b = &buttons[button_count++];
                    memset(b, 0, sizeof(*b));
                    b->r = R4(x0, y0, x0 + bw, y0 + bh);
                    b->node_id = leaf->id;
                    b->into = k == 1;
                    b->vertical = !across;
                    b->after = k == 2;
                    b->cells = k == 1 ? 1 : 2;
                    b->new_first = k == 0;
                    f32 *pv = b->preview;
                    memcpy(pv, leaf->px, sizeof(leaf->px));
                    if (k != 1) {
                        if (!across && k == 0) pv[2] = cx;
                        if (!across && k == 2) pv[0] = cx;
                        if (across && k == 0) pv[3] = (pv[1] + pv[3]) * 0.5f;
                        if (across && k == 2) pv[1] = (pv[1] + pv[3]) * 0.5f;
                    }
                    b->grow_scale[0] = k == 1 ? 0.8f : across ? 0.8f : 0.2f; b->grow_scale[1] = k == 1 ? 0.8f : across ? 0.2f : 0.8f;
                    b->grow_pivot[0] = k == 1 || across ? 0.5f : k == 0 ? 0.0f : 1.0f; b->grow_pivot[1] = k == 1 || !across ? 0.5f : k == 0 ? 0.0f : 1.0f;
                    b->name = k == 1 ? "drop marker center" : across ? (k == 0 ? "drop marker top" : "drop marker bottom") : (k == 0 ? "drop marker left" : "drop marker right");
                }
            }
        }
#undef ALONE
        // --- the button under the pointer takes the drop (the last built wins: the pane's cross over borders), over a
        //     tab strip too ---
        for (s32 i = 0; i < button_count; i++) if (RectHas(buttons[i].r, mx, my)) chosen = i;
        if (chosen >= 0) { strip_target = NULL; strip_drop = 0; }
        for (s32 i = 0; i < button_count; i++) Named(app, Str_FromCStr(buttons[i].name), buttons[i].r);

        TabDrop drop = {0};
        if (strip_target) { drop.strip = 1; drop.node_id = strip_drop_node; drop.before_id = strip_drop_before; }
        if (chosen >= 0) { drop.node_id = buttons[chosen].node_id; drop.into = buttons[chosen].into; drop.vertical = buttons[chosen].vertical; drop.after = buttons[chosen].after; }
        b32 drop_spot = strip_target || chosen >= 0;
        if (drag_tab && drag_outside && ui->dragging && in->input.button[Mouse_Left].down) {
            out->drag_out = 1;
            out->drag_out_label = PaneTabLabel(app, drag_tab);
        }
        if (in->foreign_drop) {
            // --- another window's tab dropped here: a server tab lands at the spot once the server hands it over (it may
            //     have already), a web tab's page opens here again ---
            PaneTab *t = in->foreign_tab ? Panes_FindTerm(panes, in->foreign_tab) : NULL;
            if (!in->foreign_tab) {
                PaneNode *web_leaf = panes->selected ? panes->selected->node : panes->root;
                while (web_leaf->first_child) web_leaf = web_leaf->first_child;
                t = NewWebTab(app, web_leaf, panes->selected, Str_Equal(in->foreign_url, S("about:blank")) ? (String){0} : in->foreign_url, 0);
            }
            if (t && drop_spot) DropTabAt(app, t, drop);
            else if (drop_spot) { app->arrive_tab = in->foreign_tab; app->arrive_drop = drop; }
        }
        if (in->input.button[Mouse_Left].released || !in->input.button[Mouse_Left].down || !drag_tab) {
            if (drag_tab && in->input.button[Mouse_Left].released && drag_outside && (drag_tab->term_id || drag_tab->web)) {
                // --- dropped outside the window: into the window there, else a new one ---
                out->tear_off_tab = drag_tab->term_id;
                if (drag_tab->web) { out->tear_off_web = drag_tab->id; out->tear_off_url = (String){ drag_tab->web_url, drag_tab->web_url_len }; }
                out->tear_off_drop = 1;
                out->tear_off_last = Panes_TabCount(panes) == 1;
            }
            // --- the drop: into a strip at its marker, into a pane, or a new pane beside a node ---
            if (drag_tab && in->input.button[Mouse_Left].released && drop_spot) DropTabAt(app, drag_tab, drop);
            app->tab_drag_id = 0;
            drag_tab = NULL;
        }
    }

    // --- the panes again: keys and drops may have split, closed or moved them ---
    LayoutPanes(app, area_x, area_w, area_h);

    // --- splitters: a pane's left / top edge where it meets the one before it in a side-by-side / stacked group. A
    //     drag moves the edge, a double click evens the group. Hit here (before the panes draw, so they lay out at the
    //     new place this frame) and again after the panes (so the splitter wins the hover over them) ---
    typedef struct Splitter { u64 id; Rect r; s32 axis; PaneNode *a; s32 leaf_index; } Splitter;
    Splitter splitters[128];
    s32 splitter_count = 0;
    {
        s32 leaf_index = -1;
        for (PaneNode *leaf = app->zoom_node ? NULL : panes->root; leaf; leaf = Panes_NextNode(leaf)) {
            if (leaf->first_child) continue;
            leaf_index++;
            for (s32 axis = 0; axis < 2 && splitter_count < (s32)ArrayCount(splitters); axis++) {
                // the nearest ancestor A in a side-by-side (axis 0) / stacked (axis 1) parent with a previous sibling
                PaneNode *a = leaf;
                while (a->parent && !(a->parent->vertical == (axis == 0) && a->prev)) a = a->parent;
                if (!a->parent) continue;
                PaneNode *prev = a->prev;
                s32 half = R(0.25);
                Rect r = axis == 0 ? R4(RoundS32(a->px[0]) - half, RoundS32(leaf->px[1]) + caption_h, RoundS32(a->px[0]) + half, RoundS32(leaf->px[3]))
                                   : R4(RoundS32(leaf->px[0]), RoundS32(a->px[1]) - half, RoundS32(leaf->px[2]), RoundS32(a->px[1]) + half);
                Splitter *sp = &splitters[splitter_count++];
                sp->id = Ui_IdN(Ui_Id(axis == 0 ? "LeftBorder" : "TopBorder"), leaf->id);
                sp->r = r;
                sp->axis = axis;
                sp->a = a;
                sp->leaf_index = leaf_index;
                UiSignal s = Ui_Hit(ui, sp->id, r);
                if (s.hovered || s.held) ui->cursor = axis == 0 ? Cursor_SizeWE : Cursor_SizeNS;
                s32 m = axis == 0 ? mouse_x : mouse_y;
                if (s.pressed && s.clicks >= 2) {
                    Panes_EvenChildren(a->parent, 0);
                    LayoutPanes(app, area_x, area_w, area_h);
                } else if (s.pressed) {
                    app->grab_offset = m - RoundS32(a->px[axis]);
                } else if (s.held && ui->click_count < 2) {
                    f32 span = a->px[axis + 2] - prev->px[axis];
                    if (span > 1) {
                        f32 t = Clamp((f32)(m - app->grab_offset - prev->px[axis]) / span, 0.05f, 0.95f);
                        f32 sum = prev->split_ratio + a->split_ratio;
                        prev->split_ratio = sum * t;
                        a->split_ratio = sum * (1 - t);
                        LayoutPanes(app, area_x, area_w, area_h);
                        app->snap_until_frame = ui->frame + 1;
                        app->layout_changed_us = now;
                    }
                }
            }
        }
    }

    // --- pane rects ease to their place; they snap during a drag and while the window resizes ---
    b32 snap_rects = ui->frame <= app->snap_until_frame || in->resized;
    if (in->resized) app->layout_changed_us = now;
    f32 ease = 1.0f - expf(-25.0f * ui->dt);
    for (PaneNode *n = panes->root; n; n = Panes_NextNode(n)) {
        if (n->first_child) continue;
        for (s32 i = 0; i < 4; i++) {
            if (snap_rects || !n->anim_ready || fabsf(n->px[i] - n->anim_px[i]) < 0.5f) n->anim_px[i] = n->px[i];
            else { n->anim_px[i] += (n->px[i] - n->anim_px[i]) * ease; ui->animating = 1; }
        }
        n->anim_ready = 1;
    }

    // --- the frame: the window is see-through behind the terminals when their opacity is below 100% ---
    out->draw = dl;
    out->clear_color = app->opacity < 1.0f ? 0 : c->caption;
    PaneNode *top_right = NULL;   // the pane under the caption buttons
    for (PaneNode *n = panes->root; n; n = Panes_NextNode(n)) {
        if (n->first_child || n->px[1] > 0.5f || (app->zoom_node && n->id != app->zoom_node)) continue;
        if (n->px[2] >= (f32)(area_x + area_w) - 0.5f) top_right = n;
    }
    Rect caption_rects[App_MaxCaptionClientRects];   // the parts of the title band that are buttons and tabs
    s32 caption_rect_count = 0;

    // --- the sessions sidebar: a header in the title band, the filter, the sessions by day. A click resumes one (or
    //     brings up the tab running it); a click in it gives it the keyboard, one in a pane takes it back ---
    if (app->sidebar_open && !app->focus_mode) {
        Rect side = R4(0, 0, sidebar_w - line_w, in->client_h);
        Draw_Rect(dl, side, c->caption, 0);
        Draw_Rect(dl, R4(sidebar_w - line_w, 0, sidebar_w, in->client_h), c->line, 0);
        Rect body = side;
        Rect head = CutTop(&body, caption_h);
        CutLeft(&head, R(1.0));
        CutRight(&head, R(0.3));
        Rect close = RectCenter(CutRight(&head, R(2.4)), R(2.4), caption_h - R(0.8));
        UiSignal cl = IconButton(app, Ui_Id("CloseSessions"), close, Icon_Cancel, icons_small, c->dim, c->hover, radius_small, S("close sessions"));
        if (caption_rect_count < App_MaxCaptionClientRects) caption_rects[caption_rect_count++] = close;
        if (cl.clicked) { app->sidebar_open = 0; app->side_focus = 0; }
        Draw_TextIn(dl, bold, head, in->foreground || app->headless ? c->text : c->dim, S("Sessions"), 0);
        // the filter: what's typed while the sidebar has the keyboard
        Rect filter = CutTop(&body, R(3));
        filter = RectShrink(filter, R(0.6));
        filter.y1 = filter.y0 + R(2.2);
        Draw_RectEx(dl, filter, c->bg, app->side_focus ? c->accent : c->line, 1, radius_small, NULL, 0, 0, 0);
        Rect fin = filter;
        Draw_Icon(dl, icons_small, CutLeft(&fin, R(2)), c->dim, Icon_Search);
        Draw_TextIn(dl, small, fin, app->side_filter_len ? c->text : c->dim,
                    app->side_filter_len ? (String){ app->side_filter, app->side_filter_len } : S("Filter"), 0);

        // --- the list: a day heading where the day changes, then each session (its project and age, its first
        //     prompt); only the rows in view are drawn ---
        Rect list = body;
        list.x0 += R(0.4);
        list.x1 -= R(0.4);
        s32 list_h = Max(1, RectH(list));
        s64 now_s = (s64)(in->wall_time / 10000000ull) + (s64)in->utc_offset_minutes * 60;
        s64 today = now_s / 86400;
        static const char *const days[6] = { "Today", "Yesterday", "This week", "This month", "Earlier", "Pinned" };
        s32 *row_y = Arena_PushArray(fa, s32, side_count + 1);
        s32 *row_head = Arena_PushArray(fa, s32, side_count + 1);   // the day heading above it (-1 none)
        s32 y = 0, last_bucket = -1;
        for (s64 k = 0; k < side_count; k++) {
            const TermSession *ss = &in->sessions[side_rows[k]];
            s64 age_days = today - ((s64)(ss->modified / 10000000ull) + (s64)in->utc_offset_minutes * 60) / 86400;
            s32 bucket = age_days <= 0 ? 0 : age_days == 1 ? 1 : age_days < 7 ? 2 : age_days < 30 ? 3 : 4;
            s32 pmi = SessionMetaIndex(app, ss->id, 0);
            if (pmi >= 0 && app->side_meta[pmi].pin) bucket = 5;
            row_head[k] = bucket != last_bucket ? bucket : -1;
            if (bucket != last_bucket) y += side_head_h;
            last_bucket = bucket;
            row_y[k] = y;
            y += side_row_h;
        }
        s32 total_h = y;
        if (in->input.wheel[1] && RectHas(list, mouse_x, mouse_y) && !modal) app->side_top -= (s64)in->input.wheel[1] * side_row_h * 3 / 120;
        if (app->side_reveal && side_count) {
            s32 cy = row_y[app->side_cursor];
            if (cy < app->side_top) app->side_top = cy - (row_head[app->side_cursor] >= 0 ? side_head_h : 0);
            if (cy + side_row_h > app->side_top + list_h) app->side_top = cy + side_row_h - list_h;
            app->side_reveal = 0;
        }
        app->side_top = Clamp(app->side_top, 0, Max(0, total_h - list_h));
        Draw_PushClip(dl, list);
        for (s64 k = 0; k < side_count; k++) {
            s32 ry = list.y0 + row_y[k] - (s32)app->side_top;
            if (ry + side_row_h <= list.y0 - side_head_h) continue;
            if (ry >= list.y1) break;
            const TermSession *ss = &in->sessions[side_rows[k]];
            if (row_head[k] >= 0) Draw_TextIn(dl, small, R4(list.x0 + R(0.6), ry - side_head_h, list.x1, ry), c->dim, Str_FromCStr(days[row_head[k]]), 0);
            Rect row = R4(list.x0, ry, list.x1, ry + side_row_h);
            u64 session_id = Ui_IdN(Ui_Id("Session"), Ui_IdS(ss->id));
            UiSignal rs = Ui_Hit(ui, session_id, row);
            b32 is_cursor = k == app->side_cursor;
            if (is_cursor) Draw_Rect(dl, row, app->side_focus ? c->selection : ColorAlpha(c->selection, 0.6f), radius_small);
            else Draw_Rect(dl, row, HoverFill(app, session_id, rs, c->hover), radius_small);
            // running in a tab: a dot
            const TermTab *running = NULL;
            for (s64 i = 0; i < app->tab_count && !running; i++) if (Str_Equal(app->tabs[i].key, ss->id)) running = &app->tabs[i];
            Rect in_row = RectShrink(row, R(0.6));
            in_row.y0 = ry + R(0.3);
            Rect l1 = CutTop(&in_row, R(1.5));
            // its age: 5m, 3h, 2d, then the date
            s64 secs = ((s64)in->wall_time - (s64)ss->modified) / 10000000;
            String age = secs < 60 ? S("now") : secs < 3600 ? Str_PushFormat(fa, "%lldm", (long long)(secs / 60)) :
                         secs < 86400 ? Str_PushFormat(fa, "%lldh", (long long)(secs / 3600)) :
                         secs < 30 * 86400 ? Str_PushFormat(fa, "%lldd", (long long)(secs / 86400)) : (String){0};
            if (!age.count) {
                s32 yy = 0, mm = 0, dd = 0;
                CivilFromDays(((s64)(ss->modified / 10000000ull) - 11644473600ll + (s64)in->utc_offset_minutes * 60) / 86400, &yy, &mm, &dd);
                age = Str_PushFormat(fa, "%s %d", g_Months[(mm - 1 + 12) % 12], dd);
            }
            Draw_TextIn(dl, small, CutRight(&l1, Font_Measure(small, age)), c->dim, age, 1);
            if (ss->config_dir.count) {
                // another account's session: its name
                s32 ai = AccountIndex(app, ss->config_dir);
                String name = ai > 0 ? app->accounts[ai].name : S("other account");
                CutRight(&l1, R(0.6));
                Draw_TextIn(dl, small, CutRight(&l1, Min(Font_Measure(small, name), RectW(l1))), c->accent, name, 1);
            }
            if (running) {
                Rect dot = CutLeft(&l1, R(1.1));
                Draw_Rect(dl, RectCenter(dot, R(0.6), R(0.6)), c->accent, (f32)R(0.3));
            }
            s32 mi = SessionMetaIndex(app, ss->id, 0);
            String name = mi >= 0 && app->side_meta[mi].name_len ? (String){ app->side_meta[mi].name, app->side_meta[mi].name_len } : ss->title;
            if (mi >= 0 && app->side_meta[mi].archive) Draw_TextIn(dl, small, CutRight(&l1, Font_Measure(small, S("archived")) + R(0.6)), c->dim, S("archived"), 0);
            Draw_TextIn(dl, font, l1, c->text, ss->project.count ? ss->project : S("(no folder)"), 0);
            Rect l2 = CutTop(&in_row, R(1.4));
            String snippet = {0};
            for (s64 h = 0; app->side_filter_len >= 3 && Str_Equal(in->session_hits_text, (String){ app->side_filter, app->side_filter_len }) && h < in->session_hit_count; h++) {
                if (Str_Equal(in->session_hits[h].id, ss->id)) snippet = in->session_hits[h].snippet;
            }
            if (app->side_rename_len && Str_Equal((String){ app->side_rename_id, app->side_rename_len }, ss->id)) {
                Draw_RectEx(dl, l2, c->bg, c->accent, 1, radius_small, NULL, 0, 0, 0);
                Ui_TextEdit(ui, Ui_Id("SessionRename"), &app->side_rename_edit, RectShrink(l2, R(0.2)), small, 1, S("a name (empty: its first prompt)"), c->text, c->dim, c->selection);
            } else if (snippet.count) {
                Draw_TextIn(dl, small, l2, c->accent, snippet, 0);
            } else {
                Draw_TextIn(dl, small, l2, mi >= 0 && app->side_meta[mi].name_len ? c->text : c->dim, name, 0);
            }
            Named(app, Str_PushFormat(fa, "session %.*s", STR_ARG(ss->title)), row);
            if (rs.pressed && rs.button == Mouse_Right) {
                app->side_menu_len = (s32)CopyInto(app->side_menu_id, sizeof(app->side_menu_id), ss->id).count;
                app->side_menu_x = mouse_x;
                app->side_menu_y = mouse_y;
            }
            if (rs.clicked && rs.button == Mouse_Left) {
                app->side_cursor = k;
                OpenSession(app, &f, ss);
                app->side_focus = 0;
            }
        }
        Draw_PopClip(dl);
        if (app->side_menu_len) {
            // --- a session's menu (right-click): rename it, pin it, archive it ---
            String id = { app->side_menu_id, app->side_menu_len };
            s32 mi = SessionMetaIndex(app, id, 0);
            b32 pinned = mi >= 0 && app->side_meta[mi].pin, archived = mi >= 0 && app->side_meta[mi].archive;
            String items[3] = { S("Rename"), pinned ? S("Unpin") : S("Pin to the top"), archived ? S("Unarchive") : S("Archive") };
            s32 item_h = R(2.2), mw = R(13);
            Rect menu = R4(app->side_menu_x, app->side_menu_y, app->side_menu_x + mw, app->side_menu_y + 3 * item_h + R(0.6));
            Draw_RectEx(dl, menu, c->surface, c->line, 1, radius_medium, NULL, 0, 0, (f32)R(1));
            s32 pick = -1;
            for (s32 k = 0; k < 3; k++) {
                Rect ir = R4(menu.x0 + R(0.3), menu.y0 + R(0.3) + k * item_h, menu.x1 - R(0.3), menu.y0 + R(0.3) + (k + 1) * item_h);
                u64 iid = Ui_IdN(Ui_Id("SessionMenu"), (u64)k);
                UiSignal is = Ui_Hit(ui, iid, ir);
                Draw_Rect(dl, ir, HoverFill(app, iid, is, c->hover), radius_small);
                Draw_TextIn(dl, font, RectShrink(ir, R(0.6)), c->text, items[k], 0);
                Named(app, Str_PushFormat(fa, "session menu %.*s", STR_ARG(items[k])), ir);
                if (is.clicked) pick = k;
            }
            b32 away = (in->input.button[Mouse_Left].pressed || in->input.button[Mouse_Right].pressed) && !RectHas(menu, mouse_x, mouse_y) && pick < 0;
            for (s64 i = 0; i < in->input.event_count; i++) away |= in->input.events[i].down && in->input.events[i].key == Key_Escape;
            if (pick == 0) {
                memcpy(app->side_rename_id, id.data, (size_t)id.count);
                app->side_rename_len = (s32)id.count;
                TextEdit_Set(&app->side_rename_edit, mi >= 0 ? (String){ app->side_meta[mi].name, app->side_meta[mi].name_len } : (String){0});
            }
            if (pick == 1 || pick == 2) {
                s32 m2 = SessionMetaIndex(app, id, 1);
                if (m2 >= 0 && pick == 1) app->side_meta[m2].pin = !pinned;
                if (m2 >= 0 && pick == 2) app->side_meta[m2].archive = !archived;
                out->sessions_meta_text = SessionMetaText(app, fa);
            }
            if (pick >= 0 || away) app->side_menu_len = 0;
        }
        if (!side_count && in->connected) {
            String msg = app->side_filter_len ? S("No matches") : in->session_count ? S("") : S("No Claude Code sessions yet");
            Draw_TextIn(dl, small, R4(list.x0, list.y0, list.x1, list.y0 + R(3)), c->dim, msg, 0.5f);
        }
    }

    // --- panes: the tab strip, then the terminal or the browser ---
    Array(TermWatch) watches = {0};
    s32 pane_index = 0;
    b32 blink_wanted = 0;
    PaneTab *close_later = NULL;   // a tab closed from its strip: after the panes are drawn (it may take its pane along)
    // the find bar's matches: the theme's yellow a third of the way in from the background; the current one bright
    const TermPalette term_palette = TermPaletteFor(tt, app->ligatures);
    // --- web tabs: new windows pages asked for become tabs beside their opener ---
    for (s32 i = 0; i < in->web_popup_count; i++) {
        const AppWebPopup *pop = &in->web_popups[i];
        if (pop->id <= app->web_popup_taken) continue;
        PaneTab *opener = FindPaneTab(panes, pop->opener);
        if (!opener) continue;
        app->web_popup_taken = pop->id;
        NewWebTab(app, opener->node, opener, pop->url, pop->id);
    }
    // --- every web tab is listed (the platform closes pages no longer listed; the shown ones get their rect below).
    //     Their address and title follow the page; one that closed itself goes; a notification while it isn't in front
    //     goes to the tray and puts a dot on its tab until it's shown ---
    b32 web_shown = 0;
    PaneTab *web_closed = NULL;
    for (PaneNode *n = panes->root; n; n = Panes_NextNode(n)) {
        for (PaneTab *o = n->first_tab; o; o = o->next) {
            if (!o->web || out->web_count >= App_MaxWebs) continue;
            AppWeb *aw = &out->webs[out->web_count++];
            aw->id = o->id;
            aw->popup = o->web_popup;
            aw->zoom = g_WebZooms[WebZoom_Base + o->web_zoom];
            b32 in_front = o == panes->selected && !o->hidden && in->foreground;
            if (in_front) o->web_needs = 0;
            const AppWebState *ws = FindWebState(in, o->id);
            if (!ws) continue;
            o->web_title_len = (s32)CopyInto(o->web_title, sizeof(o->web_title), ws->title).count;
            if (ws->url.count) o->web_url_len = (s32)CopyInto(o->web_url, sizeof(o->web_url), ws->url).count;
            if (ws->close_requested) web_closed = o;
            if (ws->notices != o->web_notices_seen) {
                if (ws->notices > o->web_notices_seen && !in_front) {
                    o->web_needs = 1;
                    out->notify_title = ws->notice_title.count ? ws->notice_title : PaneTabLabel(app, o);
                    out->notify_body = ws->notice_body;
                }
                o->web_notices_seen = ws->notices;
            }
        }
    }
    if (in->web_torn_off && FindPaneTab(panes, in->web_torn_off)) web_closed = FindPaneTab(panes, in->web_torn_off);   // its page opened in another window
    if (web_closed) {
        // gone from the list this frame too: its page closes
        for (s32 i = 0; i < out->web_count; i++) if (out->webs[i].id == web_closed->id) out->webs[i] = out->webs[--out->web_count];
        CloseTab(app, &f, web_closed, 1);
    }
    for (PaneNode *n = panes->root; n; n = Panes_NextNode(n)) {
        if (n->first_child) continue;
        c = &app->c;   // the window's chrome (an account's theme may have taken the pane before)
        s32 cols = Max(2, ((s32)(n->px[2] - n->px[0]) - 2 * term_margin) / cell_w);
        s32 rows = Max(2, ((s32)(n->px[3] - n->px[1]) - caption_h - 2 * term_margin) / cell_h);
        s32 account_rows = Max(2, ((s32)(n->px[3] - n->px[1]) - caption_h - account_h - 2 * term_margin) / cell_h);   // a claude's
        if (app->zoom_node && n->id != app->zoom_node) {
            // behind the zoomed pane: its tabs watched at their own size, not shown
            for (PaneTab *o = n->first_tab; o; o = o->next) {
                if (!o->term_id) continue;
                const TermTab *ost = FindServerTab(app, o->term_id);
                TermWatch *w = Array_Push(fa, watches);
                w->tab_id = o->term_id;
                w->cols = cols;
                w->rows = ost && ost->account.count ? account_rows : rows;
            }
            continue;
        }
        PaneTab *t = Panes_VisibleTab(n);
        // --- a claude of an account with its own theme (accounts.txt): this pane in it, strip, terminal and all ---
        TermPalette pane_palette = term_palette;
        Chrome pane_chrome = {0};
        {
            const TermTab *pst = t ? FindServerTab(app, t->term_id) : NULL;
            s32 ai = pst && pst->account.count ? AccountIndex(app, pst->config_dir) : 0;
            if (ai >= 0 && app->accounts[ai].theme >= 0) {
                const TermTheme *pt = &g_TermThemes[app->accounts[ai].theme];
                pane_chrome = ChromeFor(pt);
                c = &pane_chrome;
                pane_palette = TermPaletteFor(pt, app->ligatures);
            }
        }
        b32 focused_pane = t && t == panes->selected;
        b32 at_top = n->px[1] <= 0.5f;
        b32 titlebar = in->maximized && at_top;
        s32 px0 = RoundS32(n->anim_px[0]), py0 = RoundS32(n->anim_px[1]);
        s32 pw = Max(1, RoundS32(n->anim_px[2]) - px0), ph = Max(1, RoundS32(n->anim_px[3]) - py0);
        Rect pane = R4(px0, py0, px0 + pw, py0 + ph);
        Draw_PushClip(dl, pane);

        // --- the tab strip: the shown tab merges into its pane (rounded top, concave corners at its foot), the others
        //     sit on the strip; then the new tab button, and the files / shell button on the right ---
        {
            Rect strip = R4(px0, py0, px0 + pw, py0 + caption_h);
            s32 pad_t = titlebar ? 0 : resize_border;
            UiSignal ss = Ui_Hit(ui, Ui_IdN(Ui_Id("Strip"), n->id), strip);
            if (ss.pressed && ss.button == Mouse_Right) RunCommand(app, &f, Cmd_Settings);   // a right-click on the strip opens the settings
            Draw_Rect(dl, strip, c->caption, 0);
            Draw_Rect(dl, R4(strip.x0, strip.y1 - line_w, strip.x1, strip.y1), c->line, 0);
            Rect row = strip;
            row.y0 += pad_t;   // tabs and buttons the strip's full height (below the window's resize border)
            CutLeft(&row, R(0.5));
            if (n == top_right) CutRight(&row, R(3.4) * 3);
            s32 button_w = RectH(row) - line_w;
            // --- right: the zoom's way back and the files / shell button ---
            if (t) {
                CutRight(&row, R(0.3));
                Rect face = RectCenter(CutRight(&row, button_w), button_w, button_w);
                UiSignal fs = IconButton(app, Ui_IdN(Ui_Id("Face"), n->id), face, t->browsing ? Icon_Prompt : Icon_Folder, icons_small, c->dim, c->hover,
                                         radius_medium, Str_PushFormat(fa, "face %d", pane_index));
                Ui_Tooltip(ui, Ui_IdN(Ui_Id("Face"), n->id), t->browsing ? S("Shell   Ctrl+Shift+E") : S("Files   Ctrl+Shift+E"));
                if (at_top && caption_rect_count < App_MaxCaptionClientRects) caption_rects[caption_rect_count++] = face;
                if (fs.clicked) {
                    Panes_Select(panes, t);
                    RunCommand(app, &f, Cmd_ToggleBrowser);
                }
                if (app->zoom_node == n->id) {
                    Rect zr = RectCenter(CutRight(&row, button_w), button_w, button_w);
                    UiSignal zs = IconButton(app, Ui_Id("Unzoom"), zr, Icon_BackToWindow, icons_small, c->dim, c->hover, radius_medium, S("unzoom"));
                    Ui_Tooltip(ui, Ui_Id("Unzoom"), S("Back to every pane   Ctrl+Shift+Z"));
                    if (at_top && caption_rect_count < App_MaxCaptionClientRects) caption_rects[caption_rect_count++] = zr;
                    if (zs.clicked) app->zoom_node = 0;
                }
            }
            if (at_top) CutRight(&row, R(5));   // window drag room
            s32 tab_count = 0;
            for (PaneTab *o = n->first_tab; o; o = o->next) tab_count++;
            s32 tab_gap = R(0.45);
            s32 avail = RectW(row) - button_w - R(0.4) - tab_gap * tab_count;
            s32 tab_w = Clamp(avail / Max(1, tab_count), R(9), R(16));
            f32 r_tab = (radius_medium + radius_large) * 0.5f;
            s32 fs = Max(1, RoundS32(radius_medium) - R(0.2));   // the concave corners' size
            Draw_PushClip(dl, R4(row.x0 - fs, strip.y0, row.x1, strip.y1));
            s32 x = row.x0, gap_px = 0, index = 0;
            for (PaneTab *o = n->first_tab;; o = o->next, index++) {
                // --- the drop gap before this tab, or after the last one ---
                if (strip_drop && strip_drop_node == n->id && strip_drop_before == (o ? o->id : 0)) {
                    f32 g = Ui_Animate(ui, Ui_Id("TabDropGap"), 0, 1, 25);
                    Rect gr = R4(x, row.y0, x + RoundS32((f32)tab_w * g), strip.y1);
                    f32 radii[4] = { r_tab, r_tab, 0, 0 };
                    Draw_RectEx(dl, gr, ColorAlpha(c->accent, 0.18f), c->accent, 1, 0, radii, 0, 0, 0);
                    gap_px = RectW(gr) + tab_gap;
                    x += gap_px;
                }
                if (!o) break;
                const TermTab *st = FindServerTab(app, o->term_id);
                b32 shown = !o->hidden;
                b32 tab_focused = shown && o == panes->selected;
                u64 tid = Ui_IdN(Ui_Id("Tab"), o->id), cid = Ui_IdN(Ui_Id("TabClose"), o->id);
                Rect tr = R4(x, row.y0, x + tab_w, strip.y1);
                Rect close = RectCenter(R4(tr.x1 - R(2.4), tr.y0, tr.x1 - R(0.4), tr.y1), R(2.0), Min(R(2.0), RectH(tr) - R(0.4)));
                UiSignal ts = Ui_Hit(ui, tid, tr);
                UiSignal cs = Ui_Hit(ui, cid, close);
                b32 hovered = ts.hovered || cs.hovered || cs.held;
                o->strip_x[0] = tr.x0 - gap_px;
                o->strip_x[1] = tr.x1 - gap_px;
                String label = PaneTabLabel(app, o);
                Named(app, Str_PushFormat(fa, "tab %.*s", STR_ARG(label)), tr);
                Named(app, Str_PushFormat(fa, "close tab %.*s", STR_ARG(label)), close);
                b32 lifted = app->tab_drag_id == o->id;
                if (shown) {
                    // the pane's color from the rounded top down into the pane (the content covers what's below the
                    // strip), a hairline round it, and concave corners at its foot
                    f32 radii[4] = { r_tab, r_tab, 0, 0 };
                    Draw_RectEx(dl, R4(tr.x0, tr.y0, tr.x1, tr.y1 + R(1)), c->bg, c->line, (f32)line_w, 0, radii, 0, 0, 0);
                    for (s32 side = 0; side < 2; side++) {
                        Rect sq = side ? R4(tr.x1, tr.y1 - fs, tr.x1 + fs, tr.y1) : R4(tr.x0 - fs, tr.y1 - fs, tr.x0, tr.y1);
                        Draw_Rect(dl, sq, c->bg, 0);
                        Draw_PushClip(dl, sq);
                        Rect circle = side ? R4(tr.x1, tr.y1 - 2 * fs, tr.x1 + 2 * fs, tr.y1) : R4(tr.x0 - 2 * fs, tr.y1 - 2 * fs, tr.x0, tr.y1);
                        Draw_RectEx(dl, circle, c->caption, c->line, (f32)line_w, (f32)fs, NULL, 0, 0, 0);
                        Draw_PopClip(dl);
                    }
                } else {
                    UiSignal both = ts;   // the tab or its close button under the mouse
                    both.hovered = hovered;
                    Draw_Rect(dl, R4(tr.x0, tr.y0, tr.x1, tr.y1 - R(0.3)), lifted ? c->press : HoverFill(app, tid, both, c->hover), r_tab);
                }
                if (o->color) Draw_Rect(dl, R4(tr.x0 + R(0.5), tr.y0, tr.x1 - R(0.5), tr.y0 + Max(2, R(0.2))), 0xFF000000u | tt->ansi[o->color], 1);
                // --- icon: an agent's robot or the shell's prompt; busy spins, needs-input shows a dot ---
                b32 agent = st && (st->flags & TermTab_Agent);
                b32 busy = st && (st->flags & TermTab_Busy);
                b32 needs = (st && (st->flags & TermTab_NeedsInput)) || (o->web && o->web_needs);
                u32 text_color = tab_focused || hovered ? c->text : c->dim;
                Rect inner = tr;
                CutLeft(&inner, R(0.7));
                inner.y1 -= shown ? 0 : R(0.3);
                Rect icon = CutLeft(&inner, R(1.6));
                if (busy) {
                    static const u32 spinner[4] = { 0x25D0, 0x25D3, 0x25D1, 0x25D2 };
                    Draw_Icon(dl, font, icon, text_color, spinner[(now / 120000) % 4]);
                    blink_wanted = 1;
                } else {
                    b32 held = st && (st->flags & TermTab_AccountHeld);
                    Draw_Icon(dl, icons_small, icon, held ? c->danger : text_color, o->web ? Icon_Globe : o->browsing ? Icon_Folder : agent ? Icon_Robot : Icon_Prompt);
                }
                if (needs) Draw_Rect(dl, R4(icon.x1 - R(0.3), icon.y0 + RectH(icon) / 2 - R(0.8), icon.x1 + R(0.3), icon.y0 + RectH(icon) / 2 - R(0.2)), c->accent, (f32)R(0.3));
                CutLeft(&inner, R(0.4));
                inner.x1 = close.x0 - R(0.2);
                if (st && st->account.count && app->account_count > 1 && !o->browsing) {
                    // its account's name, in that account's theme's accent when it has one
                    s32 ai = AccountIndex(app, st->config_dir);
                    String an = ai >= 0 ? app->accounts[ai].name : S("other");
                    u32 tint = ai >= 0 && app->accounts[ai].theme >= 0 ? 0xFF000000u | g_TermThemes[app->accounts[ai].theme].accent : c->text;
                    Rect pill = CutLeft(&inner, Min(Font_Measure(small, an) + R(1.0), RectW(inner)));
                    Draw_Rect(dl, RectCenter(pill, RectW(pill), R(1.5)), ColorAlpha(tint, 0.22f), radius_small);
                    Draw_TextIn(dl, small, pill, text_color, an, 0.5f);
                    CutLeft(&inner, Min(R(0.4), RectW(inner)));
                }
                if (app->rename_tab == o->id) {
                    // --- its name being edited ---
                    Rect er = RectCenter(inner, RectW(inner), R(1.9));
                    Draw_RectEx(dl, er, c->bg, c->accent, 1, radius_small, NULL, 0, 0, 0);
                    Ui_TextEdit(ui, Ui_Id("RenameTab"), &app->rename_edit, RectShrink(er, R(0.3)), font, 1, S("Name (empty: automatic)"), c->text, c->dim, c->selection);
                } else {
                    Draw_TextIn(dl, font, inner, text_color, label, 0);
                }
                if (shown || hovered) {
                    if (cs.hovered || cs.held) Draw_Rect(dl, close, cs.held ? c->press : c->hover, radius_small);
                    Draw_Icon(dl, icons_small, close, text_color, Icon_Cancel);
                }
                if (cs.clicked) close_later = o;
                if (ts.hovered && st && st->cwd.count) Ui_Tooltip(ui, tid, st->cwd);
                // --- separator between two tabs not shown ---
                if (!shown && o->next && o->next->hidden && !hovered) {
                    s32 sx = tr.x1 + tab_gap / 2;
                    Draw_Rect(dl, R4(sx, tr.y0 + RectH(tr) / 4, sx + line_w, tr.y1 - RectH(tr) / 4 - R(0.3)), c->line, 0);
                }

                // --- press shows, middle closes, right opens its menu, double click renames, a horizontal drag reorders,
                //     a vertical one picks the tab up ---
                if (ts.pressed && ts.button == Mouse_Left) Panes_Select(panes, o);
                if (ts.clicked && ts.button == Mouse_Middle) close_later = o;
                if (ts.clicked && ts.button == Mouse_Right) {
                    app->tab_menu = o->id;
                    app->tab_menu_at[0] = mouse_x;
                    app->tab_menu_at[1] = tr.y1;
                }
                if (ts.pressed && ts.button == Mouse_Left && ts.clicks >= 2 && !app->rename_tab) RunCommand(app, &f, Cmd_RenameTab);
                if (ts.held && ts.button == Mouse_Left && !app->tab_drag_id) {
                    if (!ui->dragging && abs(ui->press_x - mouse_x) > R(1)) ui->dragging = 1;
                    if (abs(mouse_y - ui->press_y) > caption_h) { ui->dragging = 1; app->tab_drag_id = o->id; }
                    if (ui->dragging && !app->tab_drag_id) {
                        // reorder: o goes where the mouse is among the others (their places last frame)
                        s32 to = 0, k = 0;
                        for (PaneTab *xo = n->first_tab; xo; xo = xo->next, k++) {
                            if (xo != o && mouse_x > (xo->strip_x[0] + xo->strip_x[1]) / 2) to = k + (k < index ? 1 : 0);
                        }
                        if (to != index) {
                            PaneTab **link = &n->first_tab;
                            while (*link != o) link = &(*link)->next;
                            *link = o->next;
                            PaneTab **at = &n->first_tab;
                            for (s32 i = 0; i < to && *at; i++) at = &(*at)->next;
                            o->next = *at;
                            *at = o;
                        }
                    }
                }
                x = tr.x1 + tab_gap;
            }
            Draw_PopClip(dl);
            // --- the new tab button ---
            Rect nb = R4(x, row.y0 + R(0.1), x + button_w, row.y0 + R(0.1) + button_w);
            UiSignal ns = IconButton(app, Ui_IdN(Ui_Id("NewTab"), n->id), nb, Icon_Add, icons_small, c->dim, c->hover, radius_medium,
                                     Str_PushFormat(fa, "new tab %d", pane_index));
            if (ns.clicked) {
                if (t) Panes_Select(panes, t);
                RequestTab(app, &f, n, TermNew_Shell, FocusedCwd(app));
            }
            if (at_top && caption_rect_count < App_MaxCaptionClientRects) caption_rects[caption_rect_count++] = R4(strip.x0, strip.y0, nb.x1, strip.y1);
        }

        // --- the size the pane gives its tabs: from the target rect, not the animated one. Every tab is watched; a
        //     browsing one isn't shown (the server keeps it running and sends no cells) ---
        for (PaneTab *o = n->first_tab; o; o = o->next) {
            if (!o->term_id) continue;
            const TermTab *ost = FindServerTab(app, o->term_id);
            TermWatch *w = Array_Push(fa, watches);
            w->tab_id = o->term_id;
            w->cols = cols;
            w->rows = ost && ost->account.count ? account_rows : rows;
            w->shown = !o->hidden && !o->browsing && !o->viewing && !o->web;
        }

        Rect content = R4(px0, py0 + caption_h, px0 + pw, py0 + ph);
        const TermTab *account_tab = t && !t->browsing && !t->viewing && !t->web ? FindServerTab(app, t->term_id) : NULL;
        if (account_tab && account_tab->account.count) {
            // --- a claude's account line: who's billed and whose email it reports with (the server reads its login,
            //     organization, email and model as its /status would), the terminal above it ---
            rows = account_rows;
            Rect bar = CutBottom(&content, account_h);
            Draw_Rect(dl, bar, account_tab->flags & TermTab_AccountHeld ? ColorMix(c->caption, c->danger, 0.3f) : c->caption, 0);
            Draw_Rect(dl, R4(bar.x0, bar.y0, bar.x1, bar.y0 + line_w), c->line, 0);
            String parts[4] = {0};
            for (s32 k = 0; k < 4; k++) parts[k] = AccountPart(account_tab->account, k);
            static const char *labels[4] = { "", "Organization", "Email", "Model" };
            Rect ar = bar;
            CutLeft(&ar, R(1.0));
            CutRight(&ar, R(1.0));
            // its account's name (accounts.txt), once there is more than one or it isn't the default's
            s32 ai = AccountIndex(app, account_tab->config_dir);
            if (app->account_count > 1 || ai != 0) {
                String name = ai >= 0 ? app->accounts[ai].name : account_tab->config_dir;
                Rect pill = CutLeft(&ar, Min(Font_Measure(bold, name) + R(1.2), RectW(ar)));
                Draw_Rect(dl, RectShrink(pill, R(0.2)), ColorAlpha(c->accent, 0.25f), radius_small);
                Draw_TextIn(dl, bold, pill, c->text, name, 0.5f);
                CutLeft(&ar, Min(R(1.0), RectW(ar)));
            }
            // --- on the right: the context's size, and claude's permission mode (read off its screen's bottom rows; a click
            //     cycles it, Shift+Tab) ---
            String ctx = AccountPart(account_tab->account, 4);
            if (ctx.count) {
                u64 tokens = strtoull((const char *)Str_PushCopy(fa, ctx).data, NULL, 10);
                String text = tokens >= 1000 ? Str_PushFormat(fa, "%lluk", (unsigned long long)((tokens + 500) / 1000)) : ctx;
                Draw_TextIn(dl, small, CutRight(&ar, Min(Font_Measure(small, text), RectW(ar))), c->text, text, 1);
                Draw_TextIn(dl, small, CutRight(&ar, Min(Font_Measure(small, S("Context ")), RectW(ar))), c->dim, S("Context "), 1);
                CutRight(&ar, Min(R(1.5), RectW(ar)));
            }
            const TermGrid *ag = FindGrid(in, account_tab->id);
            if (ag && ag->cols > 0) {
                static const char *const modes[][2] = { { "plan mode on", "plan" }, { "accept edits on", "accept edits" }, { "auto mode on", "auto" },
                                                         { "bypass permissions on", "bypass permissions" } };
                String mode = S("default");
                u8 *row = Arena_PushArray(fa, u8, ag->cols + 1);
                for (s32 y = Max(0, ag->rows - 4); y < ag->rows; y++) {
                    for (s32 x = 0; x < ag->cols; x++) {
                        u32 cp = ag->cells[(s64)y * ag->cols + x].codepoint;
                        row[x] = cp >= 'A' && cp <= 'Z' ? (u8)(cp + 32) : cp >= 0x20 && cp < 0x7F ? (u8)cp : ' ';
                    }
                    for (s32 k = 0; k < (s32)ArrayCount(modes); k++) if (ContainsNoCase((String){ row, ag->cols }, Str_FromCStr(modes[k][0]))) mode = Str_FromCStr(modes[k][1]);
                }
                Rect mr = CutRight(&ar, Min(Font_Measure(small, mode) + R(1.2), RectW(ar)));
                u64 mid = Ui_IdN(Ui_Id("ClaudeMode"), account_tab->id);
                UiSignal ms = Ui_Hit(ui, mid, mr);
                Draw_Rect(dl, RectShrink(mr, R(0.2)), HoverFill(app, mid, ms, c->hover), radius_small);
                Draw_TextIn(dl, small, mr, c->text, mode, 0.5f);
                Draw_TextIn(dl, small, CutRight(&ar, Min(Font_Measure(small, S("Mode ")), RectW(ar))), c->dim, S("Mode "), 1);
                CutRight(&ar, Min(R(1.5), RectW(ar)));
                Named(app, S("claude mode"), mr);
                if (ms.clicked && !(account_tab->flags & TermTab_AccountHeld)) TermSend(&f, account_tab->id, S("\x1b[Z"));
                Ui_Tooltip(ui, mid, S("Claude's permission mode: a click goes to the next one (Shift+Tab)"));
            }
            for (s32 k = 0; k < 4 && RectW(ar) > 0; k++) {
                if (!parts[k].count) continue;
                if (labels[k][0]) {
                    String label = Str_PushFormat(fa, "%s ", labels[k]);
                    Draw_TextIn(dl, small, CutLeft(&ar, Min(Font_Measure(small, label), RectW(ar))), c->dim, label, 0);
                }
                if (k == 3) {
                    // the model: a click picks another (/model)
                    Rect mr = CutLeft(&ar, Min(Font_Measure(small, parts[k]) + R(1.0), RectW(ar)));
                    u64 mid = Ui_IdN(Ui_Id("ClaudeModel"), account_tab->id);
                    UiSignal ms = Ui_Hit(ui, mid, mr);
                    Draw_Rect(dl, RectShrink(mr, R(0.2)), HoverFill(app, mid, ms, c->hover), radius_small);
                    Draw_TextIn(dl, small, mr, c->text, parts[k], 0.5f);
                    Named(app, S("claude model"), mr);
                    Ui_Tooltip(ui, mid, S("Pick another model (/model)"));
                    if (ms.clicked) {
                        app->palette_open = 1;
                        app->palette_mode = PaletteMode_Model;
                        app->model_term = account_tab->id;
                        memset(&app->palette_edit, 0, sizeof(app->palette_edit));
                        app->palette_sel = 0;
                        app->palette_top = 0;
                    }
                } else {
                    Draw_TextIn(dl, small, CutLeft(&ar, Min(Font_Measure(small, parts[k]), RectW(ar))), c->text, parts[k], 0);
                }
                CutLeft(&ar, Min(R(1.5), RectW(ar)));
            }
            Named(app, Str_PushFormat(fa, "account %.*s", STR_ARG(parts[2].count ? parts[2] : parts[0])), bar);
        }
        if (t && t->web) {
            // --- a web page: the address row (back, forward, reload, the address: a click edits it), then the page,
            //     which the platform draws under this frame: nothing is drawn there, the frame is see-through ---
            const AppWebState *ws = FindWebState(in, t->id);
            Rect body = content;
            Rect bar = CutTop(&body, browse_header_h);
            Draw_Rect(dl, bar, c->bg, 0);
            CutLeft(&bar, R(0.5));
            CutRight(&bar, R(0.6));
            Rect back = RectCenter(CutLeft(&bar, R(2.0)), R(2.0), R(2.0));
            Rect fwd = RectCenter(CutLeft(&bar, R(2.0)), R(2.0), R(2.0));
            Rect reload = RectCenter(CutLeft(&bar, R(2.0)), R(2.0), R(2.0));
            CutLeft(&bar, R(0.4));
            b32 can_back = ws && ws->can_back, can_forward = ws && ws->can_forward;
            UiSignal bs = IconButton(app, Ui_IdN(Ui_Id("WebBack"), n->id), back, Icon_Back, icons_small, can_back ? c->dim : c->faint, c->hover, radius_small, Str_PushFormat(fa, "web back %d", pane_index));
            UiSignal fs = IconButton(app, Ui_IdN(Ui_Id("WebForward"), n->id), fwd, Icon_Forward, icons_small, can_forward ? c->dim : c->faint, c->hover, radius_small, Str_PushFormat(fa, "web forward %d", pane_index));
            UiSignal rs = IconButton(app, Ui_IdN(Ui_Id("WebReload"), n->id), reload, ws && ws->loading ? Icon_Cancel : Icon_Refresh, icons_small, c->dim, c->hover, radius_small, Str_PushFormat(fa, "web reload %d", pane_index));
            if (bs.clicked && can_back) t->web_step = -1;
            if (fs.clicked && can_forward) t->web_step = 1;
            if (rs.clicked) t->web_reload = 1;
            if (t->web_zoom) {
                // --- the page's zoom when it isn't 100%: a click puts it back ---
                String zoom = Str_PushFormat(fa, "%d%%", RoundS32(g_WebZooms[WebZoom_Base + t->web_zoom] * 100));
                Rect zr = RectCenter(CutRight(&bar, Font_Measure(small, zoom) + R(1.6)), Font_Measure(small, zoom) + R(1.2), R(2.0));
                CutRight(&bar, R(0.4));
                UiSignal zs = Ui_Hit(ui, Ui_IdN(Ui_Id("WebZoom"), n->id), zr);
                Draw_Rect(dl, zr, zs.hovered ? c->hover : ColorMix(c->bg, c->text, 0.06f), radius_small);
                Draw_TextIn(dl, small, zr, c->dim, zoom, 0.5f);
                Ui_Tooltip(ui, Ui_IdN(Ui_Id("WebZoom"), n->id), S("Back to 100%   Ctrl+0"));
                Named(app, Str_PushFormat(fa, "web zoom %d", pane_index), zr);
                if (zs.clicked) t->web_zoom = 0;
            }
            Rect ar = RectCenter(bar, RectW(bar), R(2.0));
            b32 editing = app->web_edit_tab == t->id;
            Draw_RectEx(dl, ar, ColorMix(c->bg, c->text, 0.06f), editing ? c->accent : c->line, 1, radius_small, NULL, 0, 0, 0);
            if (editing) {
                Ui_TextEdit(ui, Ui_Id("WebAddress"), &app->web_edit, RectShrink(ar, R(0.4)), font, 1, S("Address or search"), c->text, c->dim, c->selection);
            } else {
                UiSignal as = Ui_Hit(ui, Ui_IdN(Ui_Id("WebBar"), n->id), ar);
                if (as.hovered) ui->cursor = Cursor_IBeam;
                if (as.clicked) { Panes_Select(panes, t); StartWebEdit(app, in, t); }
                String url = { t->web_url, t->web_url_len };
                if (Str_Equal(url, S("about:blank"))) url = (String){0};
                Rect tr = ar;
                CutLeft(&tr, R(0.6));
                Draw_TextIn(dl, font, tr, url.count ? c->text : c->dim, url.count ? url : S("Address or search (Ctrl+L)"), 0);
            }
            Named(app, Str_PushFormat(fa, "web address %d", pane_index), ar);
            if (ws && ws->failed) {
                Draw_Rect(dl, body, c->bg, 0);
                Draw_TextIn(dl, font, R4(body.x0, body.y0 + R(2), body.x1, body.y0 + R(4)), c->dim, S("Web pages need WebView2 (Microsoft Edge's runtime)"), 0.5f);
            }
            UiSignal ps = Ui_Hit(ui, Ui_IdN(Ui_Id("Web"), n->id), body);
            if (ps.pressed && t != panes->selected) Panes_Select(panes, t);
            if (ps.hovered && in->input.wheel[1] && (in->input.modifiers & Mod_Ctrl) && !modal) {
                // Ctrl+wheel zooms the page (the platform keeps it from the page)
                t->web_zoom = Clamp(t->web_zoom + (in->input.wheel[1] > 0 ? 1 : -1), -WebZoom_Base, (s32)ArrayCount(g_WebZooms) - 1 - WebZoom_Base);
            }
            for (s32 i = 0; i < out->web_count; i++) {
                if (out->webs[i].id != t->id) continue;
                out->webs[i].rect = body;
                out->webs[i].shown = 1;
                out->webs[i].mouse = ps.hovered;
            }
            web_shown = 1;
            Named(app, Str_PushFormat(fa, "web page %d", pane_index), body);
        } else if (t && t->viewing && t->viewer) {
            // --- the picture view: the picture fitted (never above its own size at this DPI) on a dark ground, a caption
            //     row: its name, which of the folder's, its size, the keys. The wheel steps too ---
            Viewer *v = t->viewer;
            Rect body = content;
            Rect cap = CutBottom(&body, R(2.4));
            Draw_Rect(dl, content, ColorMix(c->bg, 0xFF000000u, 0.45f), 0);
            Draw_Rect(dl, cap, c->caption, 0);
            UiSignal vs = Ui_Hit(ui, Ui_IdN(Ui_Id("Viewer"), n->id), content);
            if (vs.pressed && t != panes->selected) Panes_Select(panes, t);
            s64 at = 0;
            while (at < v->count && !Str_EqualNoCase(v->names[at], v->name)) at++;
            if (vs.hovered && in->input.wheel[1] && v->count && !modal) {
                s64 to = at + (in->input.wheel[1] > 0 ? -1 : 1);
                v->name = CopyInto(v->name_buf, sizeof(v->name_buf), v->names[(to % v->count + v->count) % v->count]);
                at = (to % v->count + v->count) % v->count;
            }
            String path = Str_PushFormat(fa, "%.*s\\%.*s", STR_ARG(v->folder), STR_ARG(v->name));
            const AppImage *img = NULL;
            for (s32 i = 0; i < in->image_count; i++) if (Str_EqualNoCase(in->images[i].path, path)) img = &in->images[i];
            Rect area = RectShrink(body, R(1));
            if (img && img->texture && img->full_w > 0 && img->full_h > 0) {
                f32 s = Min((f32)RectW(area) / (f32)img->full_w, (f32)RectH(area) / (f32)img->full_h);
                s = Min(s, (f32)in->dpi / 96.0f);
                s32 w = (s32)(img->full_w * s), h = (s32)(img->full_h * s);
                Draw_Image(dl, RectCenter(area, w, h), img->texture, 0xFFFFFFFFu);
            } else {
                Draw_TextIn(dl, font, area, c->dim, !v->name.count && v->listed ? S("No pictures here") : img && img->failed ? S("This picture can't be read") : S("Loading..."), 0.5f);
            }
            Rect ci = cap;
            CutLeft(&ci, R(1));
            CutRight(&ci, R(1));
            String keys = S("\xE2\x86\x90 \xE2\x86\x92 next  \xC2\xB7  Esc back");
            Draw_TextIn(dl, small, CutRight(&ci, Font_Measure(small, keys)), c->dim, keys, 1);
            Draw_TextIn(dl, bold, CutLeft(&ci, Min(Font_Measure(bold, v->name), RectW(ci))), c->text, v->name, 0);
            CutLeft(&ci, R(1));
            String where = v->count && at < v->count ? Str_PushFormat(fa, "%lld / %lld", (long long)(at + 1), (long long)v->count) : (String){0};
            if (img && img->full_w) where = Str_PushFormat(fa, "%.*s%s%d \xC3\x97 %d", STR_ARG(where), where.count ? "   " : "", img->full_w, img->full_h);
            Draw_TextIn(dl, small, ci, c->dim, where, 0);
            Named(app, Str_PushFormat(fa, "view %.*s", STR_ARG(v->name)), content);
        } else if (t && t->browsing && t->browser) {
            // --- the browser: path, column heads, rows, footer. Read-only while the tab is busy. Moves wait for the end
            //     of the frame (the rows' text points into the listing a move releases) ---
            Browser *b = t->browser;
            const TermTab *st = FindServerTab(app, t->term_id);
            b32 busy = TabBusy(st);
            Draw_Rect(dl, content, c->bg, 0);
            UiSignal ps = Ui_Hit(ui, Ui_IdN(Ui_Id("Browser"), n->id), content);
            if (ps.pressed && t != panes->selected) Panes_Select(panes, t);
            Rect body = content;
            Rect header = CutTop(&body, browse_header_h);
            Rect footer = CutBottom(&body, browse_footer_h);
            CutLeft(&header, R(0.5));
            CutRight(&header, R(0.6));
            // --- header: back, forward, up, the path's parts (a click goes there; on the rest of it, the go-to box with
            //     the path typed in), the bookmark star, the filter and read-only chips. The mouse's side buttons are
            //     back and forward too ---
            Rect back = RectCenter(CutLeft(&header, R(2.0)), R(2.0), R(2.0));
            Rect fwd = RectCenter(CutLeft(&header, R(2.0)), R(2.0), R(2.0));
            Rect up = RectCenter(CutLeft(&header, R(2.0)), R(2.0), R(2.0));
            UiSignal bs = IconButton(app, Ui_IdN(Ui_Id("Back"), n->id), back, Icon_Back, icons_small, b->back_count ? c->dim : c->faint, c->hover, radius_small, Str_PushFormat(fa, "back %d", pane_index));
            UiSignal fs = IconButton(app, Ui_IdN(Ui_Id("Forward"), n->id), fwd, Icon_Forward, icons_small, b->forward_count ? c->dim : c->faint, c->hover, radius_small, Str_PushFormat(fa, "forward %d", pane_index));
            UiSignal us = IconButton(app, Ui_IdN(Ui_Id("Up"), n->id), up, Icon_Up, icons_small, b->path.count ? c->dim : c->faint, c->hover, radius_small, Str_PushFormat(fa, "up %d", pane_index));
            if (bs.clicked || fs.clicked) { f.nav_tab = t; f.nav_history = bs.clicked ? -1 : 1; }
            if (us.clicked) { f.nav_tab = t; f.nav_up = 1; }
            if (in->input.side && RectHas(content, mouse_x, mouse_y) && !modal) { f.nav_tab = t; f.nav_history = in->input.side; }
            CutLeft(&header, R(0.2));
            if (b->path.count) {
                b32 marked = IsBookmarked(app, b->path);
                Rect star = RectCenter(CutRight(&header, R(2.0)), R(2.0), R(2.0));
                CutRight(&header, R(0.3));
                UiSignal ss = IconButton(app, Ui_IdN(Ui_Id("Star"), n->id), star, marked ? Icon_StarFilled : Icon_Star, icons_small, marked ? c->accent : c->dim, c->hover, radius_small,
                                         Str_PushFormat(fa, "bookmark %d", pane_index));
                Ui_Tooltip(ui, Ui_IdN(Ui_Id("Star"), n->id), marked ? S("Bookmarked (Ctrl+D)") : S("Bookmark this folder (Ctrl+D)"));
                if (ss.clicked) ToggleBookmark(app, b->path);
            }
            // the chips, right to left
            for (s32 chip = 1; chip >= 0; chip--) {
                if (chip == 0 && !b->filter_len) continue;
                if (chip == 1 && !busy) continue;
                String text = chip == 0 ? (String){ b->filter, b->filter_len } : Str_PushFormat(fa, "Read-only while %.*s", STR_ARG(BusyReason(fa, st)));
                Rect cr = RectCenter(CutRight(&header, Font_Measure(small, text) + R(3.2)), Font_Measure(small, text) + R(3.2), R(1.9));
                CutRight(&header, R(0.4));
                Draw_RectEx(dl, cr, chip == 0 ? ColorAlpha(c->accent, 0.15f) : c->hover, chip == 0 ? c->accent : c->line, 1, radius_small, NULL, 0, 0, 0);
                Rect ci = cr;
                CutLeft(&ci, R(0.5));
                Draw_Icon(dl, icons_small, CutLeft(&ci, R(1.5)), chip == 0 ? c->accent : c->dim, chip == 0 ? Icon_Search : Icon_Lock);
                Draw_TextIn(dl, small, ci, c->text, text, 0);
                Named(app, chip == 0 ? S("browser filter") : S("browser read-only"), cr);
            }
            {
                // the crumbs: This PC, then each part of the path
                String path = b->path;
                s64 start = 0;
                for (s64 k = -1; ; k++) {
                    s64 end = k == 0 ? Max(PathRoot(path), 1) : start;   // the first part is the root: "C:", or a whole "\\server\share"
                    while (k >= 0 && end < path.count && path.data[end] != '\\') end++;
                    b32 last = k >= 0 ? end >= path.count : !path.count;
                    String label = k < 0 ? S("This PC") : (String){ path.data + start, end - start };
                    if (k >= 0) Draw_Icon(dl, icons_small, CutLeft(&header, R(1.1)), c->faint, Icon_ChevronRight);
                    Font *cf = last ? bold : font;
                    s32 w = Min(Font_Measure(cf, label) + R(0.9), Max(0, RectW(header)));
                    Rect cr = RectCenter(CutLeft(&header, w), w, R(2.0));
                    UiSignal crs = Ui_Hit(ui, Ui_IdN(Ui_IdN(Ui_Id("Crumb"), n->id), (u64)(k + 1)), cr);
                    if (crs.hovered && !last) Draw_Rect(dl, cr, c->hover, radius_small);
                    Draw_TextIn(dl, cf, cr, last ? c->text : c->dim, label, 0.5f);
                    Named(app, Str_PushFormat(fa, "crumb %.*s", STR_ARG(label)), cr);
                    if (crs.clicked && !last) {
                        f.nav_tab = t;
                        f.nav_path = k < 0 ? (String){0} : Str_PushCopy(fa, (String){ path.data, end });
                        s64 next_end = end + 1;
                        while (next_end < path.count && path.data[next_end] != '\\') next_end++;
                        f.nav_select = k < 0 ? (path.data[0] == '\\' ? (String){0} : Str_PushFormat(fa, "%.*s\\", (int)Min(path.count, 2), (const char *)path.data))   // the drive's row
                                             : Str_PushCopy(fa, (String){ path.data + end + 1, Max(0, next_end - end - 1) });
                    }
                    if (last || end >= path.count) break;
                    start = k < 0 ? 0 : end + 1;
                }
                // the rest of the bar: a click there types the path into the go-to box
                if (RectW(header) > 0) {
                    UiSignal es = Ui_Hit(ui, Ui_IdN(Ui_Id("PathEdit"), n->id), header);
                    if (es.hovered) ui->cursor = Cursor_IBeam;
                    Named(app, Str_PushFormat(fa, "path %d", pane_index), header);
                    if (es.clicked) {
                        if (t != panes->selected) Panes_Select(panes, t);
                        OpenGoTo(app, t, path.count ? Str_PushFormat(fa, "%.*s\\", STR_ARG(path)) : (String){0});
                    }
                }
            }

            // --- column heads: a click sorts by it (names A-Z first, dates and sizes biggest first), again turns it ---
            Rect heads = CutTop(&body, browse_heads_h);
            heads.x0 += R(0.9) + R(1.9) + R(0.4);
            heads.x1 -= R(1.1);
            static const char *const head_names[3] = { "Name", "Modified", "Size" };
            Rect head_rects[3];
            head_rects[2] = CutRight(&heads, R(6.5));
            head_rects[1] = CutRight(&heads, R(8.5));
            head_rects[0] = heads;
            for (s32 col = 0; col < 3; col++) {
                b32 on = app->browse_sort / 2 == col;
                b32 down = (col == 0) == ((app->browse_sort & 1) != 0);   // the arrow: which way the list runs
                Rect hr = head_rects[col];
                UiSignal hs = Ui_Hit(ui, Ui_IdN(Ui_IdN(Ui_Id("Head"), n->id), (u64)col), hr);
                String name = Str_FromCStr(head_names[col]);
                s32 tw = Font_Measure(small, name);
                Rect text_r = col == 0 ? R4(hr.x0, hr.y0, hr.x0 + tw, hr.y1) : R4(hr.x1 - tw - (on ? R(1.3) : 0), hr.y0, hr.x1 - (on ? R(1.3) : 0), hr.y1);
                Draw_TextIn(dl, small, text_r, on || hs.hovered ? c->text : c->dim, name, 0);
                if (on) Draw_Icon(dl, icons_small, R4(text_r.x1, hr.y0, text_r.x1 + R(1.3), hr.y1), c->text, down ? Icon_Down : Icon_Up);
                Named(app, Str_PushFormat(fa, "sort %s", head_names[col]), hr);
                if (hs.clicked) app->browse_sort = (u8)(on ? app->browse_sort ^ 1 : col * 2);
            }

            // --- rows: the wheel scrolls, a key move keeps the cursor in view, a click selects, a double click opens ---
            Rect list = body;
            list.x0 += R(0.4);
            list.x1 -= R(0.4);
            s32 visible = Max(1, RectH(list) / browse_row_h);
            if (in->input.wheel[1] && RectHas(list, mouse_x, mouse_y) && !modal) {
                app->wheel_accum += in->input.wheel[1];
                s32 notches = app->wheel_accum / 120;
                app->wheel_accum -= notches * 120;
                b->top -= notches * Max(1, in->input.wheel_scroll_lines);
            }
            if (b->reveal) {
                if (b->cursor < b->top) b->top = b->cursor;
                if (b->cursor >= b->top + visible) b->top = b->cursor - visible + 1;
                b->reveal = 0;
            }
            b->top = Clamp(b->top, 0, Max(0, b->view_count - visible));
            s64 now_secs = (s64)(in->wall_time / 10000000ull) - 11644473600ll + (s64)in->utc_offset_minutes * 60;
            s32 today_y = 0, today_m = 0, today_d = 0;
            s64 today = (now_secs >= 0 ? now_secs : now_secs - 86399) / 86400;
            CivilFromDays(today, &today_y, &today_m, &today_d);
            String filter = { b->filter, b->filter_len };
            Draw_PushClip(dl, list);
            for (s64 r = b->top; r < b->view_count && r <= b->top + visible; r++) {
                FileEntry *e = BrowserEntry(b, r);
                b32 is_dir = (e->flags & File_Dir) != 0, is_drive = (e->flags & File_Drive) != 0;
                b32 dim = (e->flags & File_Hidden) != 0;
                b32 is_cursor = r == b->cursor, is_selected = RowSelected(b, r);
                Rect row = R4(list.x0, list.y0 + (s32)(r - b->top) * browse_row_h, list.x1, list.y0 + (s32)(r - b->top + 1) * browse_row_h);
                u64 row_id = Ui_IdN(Ui_IdN(Ui_Id("Row"), n->id), NameKey(e->name));
                UiSignal rs = Ui_Hit(ui, row_id, row);
                if (is_selected) Draw_Rect(dl, row, focused_pane ? c->selection : ColorAlpha(c->selection, 0.55f), radius_small);
                else Draw_Rect(dl, row, HoverFill(app, row_id, rs, c->hover), radius_small);
                for (s32 k = 0; k < (s32)ArrayCount(b->flashes); k++) {
                    // new or changed in the last listing: a tint that fades over 1.5 s
                    u64 age = now - b->flashes[k].at_us;
                    if (b->flashes[k].key != (NameKey(e->name) | 1) || !b->flashes[k].at_us || age >= 1500000) continue;
                    Draw_Rect(dl, row, ColorAlpha(c->accent, 0.25f * (1.0f - (f32)age / 1500000.0f)), radius_small);
                    ui->animating = 1;
                }
                if (is_cursor && b->sel.count > 1 && focused_pane) Draw_RectEx(dl, row, 0, c->accent, 1, radius_small, NULL, 0, 0, 0);
                Named(app, e->name, row);
                Rect in_row = row;
                CutLeft(&in_row, R(0.5));
                CutRight(&in_row, R(0.7));
                Draw_Icon(dl, icons, CutLeft(&in_row, R(1.9)), is_dir && !dim ? c->accent : c->dim, is_drive ? Icon_HardDrive : is_dir ? Icon_Folder : Icon_Page);
                CutLeft(&in_row, R(0.4));
                // size: files', and a drive's free space
                String size = {0};
                if (!is_dir || (is_drive && e->size) || (e->flags & File_Sized)) {
                    size = SizeText(fa, is_drive ? e->free : e->size);
                    if (is_drive) size = Str_PushFormat(fa, "%.*s free", STR_ARG(size));
                }
                Draw_TextIn(dl, small, CutRight(&in_row, R(6.5)), c->dim, size, 1);
                // modified: "Today 14:03", "Yesterday 09:12", "Oct 12", "Mar 1, 2024"
                String date = {0};
                if (e->modified && !is_drive) {
                    s64 secs = (s64)(e->modified / 10000000ull) - 11644473600ll + (s64)in->utc_offset_minutes * 60;
                    s64 days = (secs >= 0 ? secs : secs - 86399) / 86400;
                    s32 yy = 0, mo = 0, dd = 0;
                    CivilFromDays(days, &yy, &mo, &dd);
                    s32 hh = (s32)((secs - days * 86400) / 3600), mi = (s32)((secs - days * 86400) / 60 % 60);
                    if (days == today) date = Str_PushFormat(fa, "Today %02d:%02d", hh, mi);
                    else if (days == today - 1) date = Str_PushFormat(fa, "Yesterday %02d:%02d", hh, mi);
                    else if (yy == today_y) date = Str_PushFormat(fa, "%s %d", g_Months[(mo - 1 + 12) % 12], dd);
                    else date = Str_PushFormat(fa, "%s %d, %d", g_Months[(mo - 1 + 12) % 12], dd, yy);
                }
                Draw_TextIn(dl, small, CutRight(&in_row, R(8.5)), c->dim, date, 1);
                if (e->flags & File_Cloud) Draw_Icon(dl, icons_small, CutRight(&in_row, R(1.8)), c->dim, Icon_Cloud);   // online only: opening it downloads it
                // the name (a drive's with its label), with the typed text lit
                u32 name_color = dim ? c->dim : c->text;
                if (b->renaming && is_cursor) {
                    Rect er = RectCenter(in_row, RectW(in_row), browse_row_h - R(0.3));
                    Draw_RectEx(dl, er, c->bg, c->accent, 1, radius_small, NULL, 0, 0, 0);
                    Ui_TextEdit(ui, Ui_IdN(Ui_Id("Rename"), n->id), &b->rename, RectShrink(er, R(0.2)), font, focused_pane, (String){0}, c->text, c->dim, c->selection);
                } else if (is_drive) {
                    String shown_name = e->label.count ? Str_PushFormat(fa, "%.*s (%.*s)", STR_ARG(e->label), (int)Min(e->name.count, 2), (const char *)e->name.data) : e->name;
                    Draw_TextIn(dl, font, in_row, name_color, shown_name, 0);
                } else {
                    u32 *marks = filter.count ? Arena_PushArray(fa, u32, filter.count) : NULL;
                    s64 mark_count = 0;
                    if (marks && FilterMatch(e->name, filter, marks)) for (s64 k = 0; k < filter.count; k++) mark_count += (filter.data[k] & 0xC0) != 0x80;
                    if (Font_Measure(font, e->name) <= RectW(in_row)) {
                        Draw_TextMarked(dl, font, in_row.x0, in_row.y0 + (RectH(in_row) - (s32)(font->line_h + 0.5f)) / 2, name_color, e->name, marks, mark_count, c->accent);
                    } else {
                        Draw_TextIn(dl, font, in_row, name_color, e->name, 0);
                    }
                }
                // --- a drag out of the list takes the selected rows (or this one) along as a copy ---
                if (rs.held && rs.button == Mouse_Left && !b->renaming && !ui->dragging &&
                    abs(mouse_x - ui->press_x) + abs(mouse_y - ui->press_y) > R(0.8) && !is_drive) {
                    if (!RowSelected(b, r)) { b->sel.count = 0; b->cursor = r; }
                    String *paths = NULL, first = {0};
                    s64 cnt = SelectedPaths(fa, b, &paths, &first);
                    if (cnt) { out->drag_file_count = cnt; out->drag_files = paths; }
                    ui->dragging = 1;
                }
                if (rs.pressed && rs.button == Mouse_Left) {
                    // Ctrl toggles the row, Shift selects from the anchor, a plain click just this row
                    u8 cm = in->input.modifiers;
                    if (r != b->cursor) b->renaming = 0;
                    if (cm & Mod_Ctrl) {
                        if (!b->sel.count) { FileEntry *c0 = BrowserEntry(b, b->cursor); if (c0) SelSet(b, c0->name, 1); }
                        SelSet(b, e->name, !RowSelected(b, r));
                        b->anchor = r;
                    } else if (cm & Mod_Shift) {
                        SelRange(b, b->anchor, r);
                    } else {
                        b->sel.count = 0;
                        b->anchor = r;
                    }
                    b->cursor = r;
                    if (rs.clicks >= 2) { f.nav_tab = t; f.nav_open = b->view[r]; }
                }
            }
            Draw_PopClip(dl);
            if (!b->view_count && b->listed) {
                String msg = b->error == 5 ? S("Access denied") : b->error && b->error != 2 && b->error != 18 ? S("Can't read this folder") :
                             b->filter_len ? S("No matches") : S("Empty folder");
                Draw_TextIn(dl, font, R4(list.x0, list.y0, list.x1, list.y0 + R(4)), c->dim, msg, 0.5f);
            }

            // --- footer: what's here, and a short notice (why something was refused) ---
            CutLeft(&footer, R(1.0));
            CutRight(&footer, R(1.0));
            String counts = !b->listed ? S("Loading...") :
                            Str_PushFormat(fa, "%lld folder%s \xC2\xB7 %lld file%s", (long long)b->view_dirs, b->view_dirs == 1 ? "" : "s",
                                           (long long)b->view_files, b->view_files == 1 ? "" : "s");
            if (b->sel.count > 1) counts = Str_PushFormat(fa, "%.*s \xC2\xB7 %lld selected", STR_ARG(counts), (long long)b->sel.count);
            Draw_TextIn(dl, small, footer, c->dim, counts, 0);
            if (b->notice_len && now < b->notice_until_us) {
                String notice = { b->notice, b->notice_len };
                Rect nr = CutRight(&footer, Min(Font_Measure(small, notice), RectW(footer)));
                Draw_TextIn(dl, small, nr, c->text, notice, 1);
                Named(app, S("browser notice"), nr);
                if (!out->wake_at_us || b->notice_until_us < out->wake_at_us) out->wake_at_us = b->notice_until_us;
            }
            if (app->pane_rect_count < App_MaxPanes) {
                app->pane_rects[app->pane_rect_count].rect = list;
                app->pane_rects[app->pane_rect_count].term_id = t->term_id;
                app->pane_rect_count++;
            }
        } else {

            // --- the terminal: its cells (an external draw), then what goes over them; mouse and selection ---
            const TermGrid *g = t ? FindGrid(in, t->term_id) : NULL;
            s32 ox = px0 + term_margin, oy = py0 + caption_h + term_margin;
            UiSignal cs = Ui_Hit(ui, Ui_IdN(Ui_Id("Term"), n->id), content);
            if (cs.hovered) ui->cursor = Cursor_IBeam;
            if (app->pane_rect_count < App_MaxPanes) {
                app->pane_rects[app->pane_rect_count].rect = R4(ox, oy, ox + cols * cell_w, oy + rows * cell_h);
                app->pane_rects[app->pane_rect_count].term_id = t ? t->term_id : 0;
                app->pane_rect_count++;
            }
            if (!g) {
                // --- a pane with no terminal yet, or no server ---
                Draw_Rect(dl, content, ColorAlpha(c->bg, app->opacity), 0);
                String msg = !in->connected ? (in->status.count ? in->status : S("connecting to the moss server...")) : !t ? S("empty pane") : S("starting...");
                Draw_TextIn(dl, font, R4(content.x0, content.y0 + R(2), content.x1, content.y0 + R(4)), c->dim, msg, 0.5f);
                if (cs.pressed && t) Panes_Select(panes, t);
            } else {
                // --- the cells, with the selection and cursor on top ---
                TermCellsDraw *draw = Arena_PushStruct(fa, TermCellsDraw);
                draw->tab_id = g->tab_id;
                draw->origin[0] = ox;
                draw->origin[1] = oy;
                draw->palette = pane_palette;
                draw->opacity = app->opacity;
                if (app->sel.visible && app->sel.term_id == g->tab_id) {
                    draw->sel_on = 1;
                    draw->sel_block = app->sel.block;
                    SelectionRange(&app->sel, &draw->sel[0], &draw->sel[1], &draw->sel[2], &draw->sel[3]);
                }
                // the program's cursor, else the settings' (DECSCUSR: 1/2 block, 3/4 underline, 5/6 bar; odd blinks)
                draw->cursor_shape = g->cursor_shape ? g->cursor_shape : (app->cursor == 1 ? 6 : app->cursor == 2 ? 4 : 2) - (app->cursor_blink ? 1 : 0);
                b32 blinks = (draw->cursor_shape & 1) != 0;
                b32 phase_on = !blinks || app->headless || ((now - app->blink_start_us) / 530000) % 2 == 0;
                if (blinks && focused_pane) blink_wanted = 1;
                draw->cursor_on = !(g->flags & TermGrid_CursorHidden) && phase_on;
                draw->cursor_hollow = !focused_pane || !(in->focused || app->headless);
                if (focused_pane) {
                    out->ime_caret[0] = ox + g->cursor_x * cell_w;
                    out->ime_caret[1] = oy + g->cursor_y * cell_h;
                    out->ime_caret[2] = cell_w;
                    out->ime_caret[3] = cell_h;
                }

                // --- mouse: a program that asked gets reports (Shift keeps the mouse for selecting); otherwise drags
                //     select, Ctrl+click opens links, the wheel scrolls back ---
                s32 cx = (mouse_x - ox) / cell_w, cy = (mouse_y - oy) / cell_h;
                s32 ccx = Clamp(cx, 0, g->cols - 1), ccy = Clamp(cy, 0, g->rows - 1);
                b32 takes = g->mouse_mode && !(in->input.modifiers & Mod_Shift);
                u8 mods = in->input.modifiers;
                if (cs.pressed) {
                    if (t != panes->selected) Panes_Select(panes, t);
                    app->blink_start_us = now;
                    if (app->sel.visible && !(mods & Mod_Shift)) app->sel.visible = 0;
                }
                // --- Ctrl+click: an OSC 8 link (the server knows where it goes), an http(s) address, or a path (the
                //     platform opens it if it's there); Ctrl over a link underlines it ---
                u32 link_id = (u32)(g->cells[(s64)ccy * g->cols + ccx].flags >> TermCell_LinkShift);
                b32 on_cells = cx >= 0 && cy >= 0 && cx < g->cols && cy < g->rows && mouse_x >= ox && mouse_y >= oy;
                if ((mods & Mod_Ctrl) && !link_id && on_cells && cs.hovered) {
                    s32 lx = 0;
                    String word = LinkAt(fa, g, ccx, ccy, &lx);
                    if ((word.count > 7 && memcmp(word.data, "http://", 7) == 0) || (word.count > 8 && memcmp(word.data, "https://", 8) == 0)) {
                        draw->palette.underline_row = ccy;
                        draw->palette.underline_x = lx;
                        draw->palette.underline_len = (s32)word.count;
                        ui->cursor = Cursor_Hand;
                    }
                }
                if ((mods & Mod_Ctrl) && link_id && on_cells && cs.hovered) {
                    draw->palette.link_hover = link_id;
                    ui->cursor = Cursor_Hand;
                }
                b32 opened = 0;
                if (cs.pressed && cs.button == Mouse_Left && (mods & Mod_Ctrl) && on_cells) {
                    s32 lx = 0;
                    String link = LinkAt(fa, g, ccx, ccy, &lx);
                    b32 http = (link.count > 7 && memcmp(link.data, "http://", 7) == 0) || (link.count > 8 && memcmp(link.data, "https://", 8) == 0);
                    const TermTab *lt = FindServerTab(app, g->tab_id);
                    if (link_id) { out->link_tab = g->tab_id; out->link_id = link_id; opened = 1; }
                    else if (http) { out->open_url = link; opened = 1; }
                    else if (link.count) { out->open_path = link; out->open_path_cwd = lt ? lt->cwd : (String){0}; opened = 1; }
                }
                if (takes && !opened) {
                    for (s32 b = 0; b < Mouse_Count; b++) {
                        s32 code = b == Mouse_Left ? 0 : b == Mouse_Middle ? 1 : 2;
                        if (cs.pressed && cs.button == b) {
                            TermSend(&f, g->tab_id, MouseBytes(fa, g, code, ccx + 1, ccy + 1, 0, mods));
                            app->mouse_tab = g->tab_id;
                            app->mouse_button = code + 1;
                            app->mouse_cx = ccx + 1;
                            app->mouse_cy = ccy + 1;
                        }
                    }
                    if (app->mouse_tab == g->tab_id && app->mouse_button) {
                        s32 b = app->mouse_button - 1;
                        s32 ui_b = b == 0 ? Mouse_Left : b == 1 ? Mouse_Middle : Mouse_Right;
                        if (in->input.button[ui_b].released || !in->input.button[ui_b].down) {
                            TermSend(&f, g->tab_id, MouseBytes(fa, g, b, ccx + 1, ccy + 1, 1, mods));
                            app->mouse_button = 0;
                        } else if (g->mouse_mode >= 1002 && (ccx + 1 != app->mouse_cx || ccy + 1 != app->mouse_cy)) {
                            TermSend(&f, g->tab_id, MouseBytes(fa, g, b | 32, ccx + 1, ccy + 1, 0, mods));
                            app->mouse_cx = ccx + 1;
                            app->mouse_cy = ccy + 1;
                        }
                    } else if (g->mouse_mode == 1003 && cs.hovered && on_cells && (cx + 1 != app->mouse_cx || cy + 1 != app->mouse_cy)) {
                        TermSend(&f, g->tab_id, MouseBytes(fa, g, 3 | 32, cx + 1, cy + 1, 0, mods));
                        app->mouse_cx = cx + 1;
                        app->mouse_cy = cy + 1;
                    }
                } else if (!opened) {
                    // --- selection: drag; double-click a word, triple-click a row ---
                    if (cs.pressed && cs.button == Mouse_Left) {
                        Selection *s = &app->sel;
                        b32 again = now - s->click_us <= (u64)Max(1, in->input.double_click_ms) * 1000 && ccx == s->click_x && ccy == s->click_y && s->term_id == g->tab_id;
                        s->clicks = again ? s->clicks % 3 + 1 : 1;
                        s->click_us = now;
                        s->click_x = ccx;
                        s->click_y = ccy;
                        s->term_id = g->tab_id;
                        s->ax = s->bx = ccx;
                        s->ay = s->by = ccy;
                        s->dragging = 1;
                        s->block = (mods & Mod_Alt) != 0;
                        s->visible = 0;
                        const TermCell *crow = g->cells + (s64)ccy * g->cols;
#define WORD_STOP(k) (!((k).flags & TermCell_WideCont) && ((k).codepoint <= ' ' || ((k).codepoint < 0x80 && strchr("\"'`()[]{}<>|,;", (int)(k).codepoint))))
                        if (s->clicks == 2 && !WORD_STOP(crow[ccx])) {
                            while (s->ax > 0 && !WORD_STOP(crow[s->ax - 1])) s->ax--;
                            while (s->bx + 1 < g->cols && !WORD_STOP(crow[s->bx + 1])) s->bx++;
                            s->visible = 1;
                        }
#undef WORD_STOP
                        if (s->clicks == 3) { s->ax = 0; s->bx = g->cols - 1; s->visible = 1; }
                    }
                    if (app->sel.dragging && app->sel.term_id == g->tab_id) {
                        Selection *s = &app->sel;
                        if (in->input.button[Mouse_Left].down && !(s->clicks > 1 && ccx == s->click_x && ccy == s->click_y) && (ccx != s->bx || ccy != s->by)) {
                            s->bx = ccx;
                            s->by = ccy;
                            s->visible = s->visible || ccx != s->ax || ccy != s->ay;
                        }
                        if (!in->input.button[Mouse_Left].down) {
                            s->dragging = 0;
                            if (s->visible && !app->copy_on_select_off) {
                                out->clipboard_set = 1;
                                out->clipboard_text = SelectionText(fa, s, g);
                                const TermTab *sst = FindServerTab(app, g->tab_id);
                                if (sst && (sst->flags & TermTab_Agent) && !s->block) out->clipboard_text = CleanAgentCopy(fa, out->clipboard_text, g->cols);
                            }   // copy on select
                        }
                        if (s->visible) {
                            draw->sel_on = 1;
                            draw->sel_block = s->block;
                            SelectionRange(s, &draw->sel[0], &draw->sel[1], &draw->sel[2], &draw->sel[3]);
                        }
                    }
                    // right-click pastes
                    if (cs.pressed && cs.button == Mouse_Right) { app->paste_wait = g->tab_id; out->read_clipboard = 1; }
                }
                // --- wheel: reports to a program that takes the mouse, arrows in an alt screen, else scrollback; Ctrl+wheel
                //     zooms ---
                if (in->input.wheel[1] && cs.hovered && !(mods & Mod_Ctrl)) {
                    app->wheel_accum += in->input.wheel[1];
                    s32 notches = app->wheel_accum / 120;
                    app->wheel_accum -= notches * 120;
                    for (s32 k = 0; k < abs(notches); k++) {
                        b32 up = notches > 0;
                        if (takes) TermSend(&f, g->tab_id, MouseBytes(fa, g, up ? 64 : 65, ccx + 1, ccy + 1, 0, mods));
                        else if (g->flags & TermGrid_AltScreen) {
                            String arrow = (g->flags & TermGrid_AppCursorKeys) ? (up ? S("\x1bOA") : S("\x1bOB")) : (up ? S("\x1b[A") : S("\x1b[B"));
                            for (s32 r = 0; r < 3; r++) TermSend(&f, g->tab_id, arrow);
                        } else {
                            TermScrollBy(&f, g->tab_id, (up ? 1 : -1) * Max(1, in->input.wheel_scroll_lines));
                        }
                    }
                }
                if (in->input.wheel[1] && cs.hovered && (mods & Mod_Ctrl)) app->term_font_pt = Clamp(app->term_font_pt + (in->input.wheel[1] > 0 ? 1 : -1), 6, 72);
                if (in->background && app->background_strength) {
                    // --- the background image: across the whole pane area (it goes on from pane to pane), filling it
                    //     (cropped, never stretched), seen through the theme'"'"'s background at its strength; the cells'"'"'
                    //     default backgrounds go see-through over it ---
                    f32 s = Max((f32)area_w / (f32)Max(1, in->background_w), (f32)area_h / (f32)Max(1, in->background_h));
                    s32 iw = RoundS32((f32)in->background_w * s), ih = RoundS32((f32)in->background_h * s);
                    Rect img = R4(area_x + (area_w - iw) / 2, (area_h - ih) / 2, area_x + (area_w - iw) / 2 + iw, (area_h - ih) / 2 + ih);
                    Draw_Rect(dl, content, ColorAlpha(c->bg, app->opacity), 0);
                    Draw_PushClip(dl, content);
                    Draw_Image(dl, img, in->background, ColorAlpha(0xFFFFFFFFu, (f32)app->background_strength / 100.0f * app->opacity));
                    Draw_PopClip(dl);
                    draw->opacity = 0;
                }
                Draw_External(dl, content, draw, Hash_Bytes(draw, sizeof(*draw), g->version));

                // --- the find bar's matches: ticks along the right edge, where they are in the scrollback ---
                s32 track_y = oy, track_h = g->rows * cell_h;
                if (app->find_open && app->find_term == g->tab_id && in->find_tab == g->tab_id && in->find_count) {
                    for (s64 i = 0; i < in->find_mark_count; i++) {
                        s32 ty = track_y + (s32)((s64)in->find_marks[i] * (track_h - 2) / 65535);
                        Draw_Rect(dl, R4(px0 + pw - R(0.7), ty, px0 + pw - R(0.1), ty + Max(2, R(0.15))), 0xFF000000u | tt->ansi[3], 0);
                    }
                }
                // --- the scrollbar: a thin thumb at the right edge while scrolled back, with the mouse near it, or
                //     dragged; a drag moves through the scrollback ---
                if (!(g->flags & TermGrid_AltScreen) && g->scrollback > 0) {
                    s32 total = g->scrollback + g->rows;
                    s32 thumb_h = Max(R(2), (s32)((s64)track_h * g->rows / total));
                    b32 dragging = app->sb_drag_tab == g->tab_id;
                    s32 offset = dragging ? app->sb_target : g->scroll_offset;
                    s32 thumb_y = track_y + (s32)((s64)(track_h - thumb_h) * (g->scrollback - offset) / g->scrollback);
                    b32 by_bar = mouse_x >= px0 + pw - R(2.5) && mouse_x < px0 + pw && mouse_y >= track_y && mouse_y < track_y + track_h;
                    if (offset > 0 || by_bar || dragging) {
                        b32 wide = by_bar || dragging;
                        Rect thumb = R4(px0 + pw - R(wide ? 1.1 : 0.8), thumb_y, px0 + pw - R(wide ? 1.1 : 0.8) + R(wide ? 0.8 : 0.5), thumb_y + thumb_h);
                        UiSignal ss = Ui_Hit(ui, Ui_IdN(Ui_Id("Scroll"), g->tab_id), R4(thumb.x0 - R(0.3), thumb.y0, thumb.x1 + R(0.3), thumb.y1));
                        Draw_Rect(dl, thumb, ss.hovered || dragging ? ColorAlpha(c->text, 0.55f) : ColorAlpha(c->text, 0.3f), (f32)R(0.4));
                        Named(app, S("scrollbar"), thumb);
                        if (ss.pressed && ss.button == Mouse_Left) {
                            app->sb_drag_tab = g->tab_id;
                            app->sb_grab = mouse_y - thumb.y0;
                            app->sb_target = g->scroll_offset;
                            dragging = 1;
                        }
                    }
                    if (dragging && !in->input.button[Mouse_Left].down) app->sb_drag_tab = 0;
                    else if (dragging) {
                        f32 frac = (f32)(mouse_y - app->sb_grab - track_y) / (f32)Max(1, track_h - thumb_h);
                        s32 want = g->scrollback - (s32)(Clamp(frac, 0.0f, 1.0f) * (f32)g->scrollback + 0.5f);
                        if (want != app->sb_target) {
                            TermScrollBy(&f, g->tab_id, want - app->sb_target);
                            app->sb_target = want;
                        }
                    }
                }
                // broadcasting: every terminal that gets the keys has an accent edge
                if (app->broadcast) Draw_RectEx(dl, RectShrink(content, 1), 0, c->accent, 2, radius_small, NULL, 0, 0, 0);
            }

            // --- search in a claude's history: the history view covers the terminal: its messages wrapped to the pane,
            //     built again when they, the width or the text change; matches lit, the current one framed ---
            b32 searching = t && app->find_open && app->find_history && app->find_term == t->term_id;
            if (t && (searching || (app->reader_open && app->reader_term == t->term_id))) {
                String q = searching ? TextEdit_String(&app->find_edit) : (String){0};
                Font *mono = &in->fonts[Font_Mono];
                s32 view_h = Max(1, RectH(content));
                s32 text_w = Max(R(10), pw - 2 * R(1.2));
                s32 lh = R(1.55);
                u64 qh = Hash_Bytes(q.data, (u64)q.count, 0x415 + (u64)app->find_case);
                if (in->history_tab == t->term_id && in->history_version &&
                    (in->history_version != app->hist_version || text_w != app->hist_width || qh != app->hist_query || app->hist_term != t->term_id)) {
                    b32 fresh = in->history_version != app->hist_version || app->hist_term != t->term_id;
                    app->hist_version = in->history_version;
                    app->hist_term = t->term_id;
                    app->hist_width = text_w;
                    app->hist_query = qh;
                    Arena_Clear(&app->hist_arena);
                    memset(&app->hist_lines, 0, sizeof(app->hist_lines));
                    memset(&app->hist_matches, 0, sizeof(app->hist_matches));
                    memset(&app->hist_blocks, 0, sizeof(app->hist_blocks));
                    for (s64 i = 0; i < in->history_count; i++) {
                        const TermHistoryEntry *he = &in->history[i];
                        HistLine *hl = Array_Push(&app->hist_arena, app->hist_lines);
                        hl->kind = he->from_user ? HistLine_User : HistLine_Claude;
                        String when = he->time.count >= 16 ? (String){ he->time.data, 16 } : he->time;
                        hl->text = Str_PushFormat(&app->hist_arena, "%s  %.*s", he->from_user ? "You" : "Claude", STR_ARG(when));
                        for (s64 k = 0; k < hl->text.count; k++) if (hl->text.data[k] == 'T') hl->text.data[k] = ' ';
                        String text = Str_PushCopy(&app->hist_arena, he->text);
                        // --- its markdown, line by line: fenced code (a block, kept for its copy button), tables (cells
                        //     in columns), headings, rules, list items (a marker, their lines indented), paragraphs; bold
                        //     and `code` runs taken out of the text and kept as spans; each wrapped to the width ---
                        b32 in_code = 0;
                        s64 code_from = 0;
                        for (s64 at = 0; at <= text.count;) {
                            s64 end = at;
                            while (end < text.count && text.data[end] != '\n') end++;
                            String raw = { text.data + at, end - at };
                            if (raw.count && raw.data[raw.count - 1] == '\r') raw.count--;
                            String trim = raw;
                            s32 lead = 0;
                            while (trim.count && (trim.data[0] == ' ' || trim.data[0] == '\t')) { trim.data++; trim.count--; lead++; }
                            if (trim.count >= 3 && !memcmp(trim.data, "```", 3)) {
                                // a fence: a block opens, or the open one closes (its text kept for the copy button)
                                if (!in_code) { code_from = end + 1; in_code = 1; }
                                else {
                                    *Array_Push(&app->hist_arena, app->hist_blocks) = (String){ text.data + code_from, Max(0, at - 1 - code_from) };
                                    in_code = 0;
                                }
                                at = end + 1;
                                continue;
                            }
                            if (in_code) {
                                // a line of code: mono, wrapped where it stops fitting
                                String rest = raw;
                                b32 first_of_block = at == code_from;
                                do {
                                    s64 cut = rest.count ? Max((s64)1, Font_Fit(mono, rest, text_w - R(1.6))) : 0;
                                    while (cut < rest.count && (rest.data[cut] & 0xC0) == 0x80) cut++;
                                    HistLine *cl = Array_Push(&app->hist_arena, app->hist_lines);
                                    cl->kind = HistLine_Code;
                                    cl->text = (String){ rest.data, cut };
                                    cl->block = (s32)app->hist_blocks.count;
                                    cl->first = first_of_block;
                                    first_of_block = 0;
                                    rest.data += cut;
                                    rest.count -= cut;
                                } while (rest.count);
                                at = end + 1;
                                continue;
                            }
                            if (trim.count && trim.data[0] == '|') {
                                // --- a table: its rows (the |---| row left out), cells trimmed, columns as wide as their
                                //     widest cell, narrowed together to fit ---
                                s64 tat = at;
                                s32 *widths = Arena_PushArray(&app->hist_arena, s32, 32);
                                s32 tcols = 0;
                                HistLine *rows_first = NULL;
                                b32 header = 1;
                                while (tat <= text.count) {
                                    s64 tend = tat;
                                    while (tend < text.count && text.data[tend] != '\n') tend++;
                                    String row = { text.data + tat, tend - tat };
                                    if (row.count && row.data[row.count - 1] == '\r') row.count--;
                                    while (row.count && row.data[0] == ' ') { row.data++; row.count--; }
                                    if (!row.count || row.data[0] != '|') break;
                                    tat = tend + 1;
                                    b32 rule = 1;
                                    for (s64 k = 0; k < row.count; k++) rule &= strchr("|-: \t", row.data[k]) != NULL;
                                    if (rule) continue;
                                    HistLine *tl = Array_Push(&app->hist_arena, app->hist_lines);
                                    if (!rows_first) rows_first = tl;
                                    tl->kind = HistLine_Table;
                                    tl->first = header;
                                    tl->cells = Arena_PushArray(&app->hist_arena, String, 32);
                                    tl->widths = widths;
                                    for (s64 k = 1; k < row.count && tl->cell_count < 32;) {
                                        s64 e2 = k;
                                        while (e2 < row.count && row.data[e2] != '|') e2++;
                                        String cell = { row.data + k, e2 - k };
                                        while (cell.count && cell.data[0] == ' ') { cell.data++; cell.count--; }
                                        while (cell.count && cell.data[cell.count - 1] == ' ') cell.count--;
                                        if (e2 >= row.count && !cell.count) break;
                                        tl->cells[tl->cell_count] = cell;
                                        widths[tl->cell_count] = Max(widths[tl->cell_count], Font_Measure(header ? bold : font, cell));
                                        tl->cell_count++;
                                        k = e2 + 1;
                                    }
                                    tcols = Max(tcols, tl->cell_count);
                                    header = 0;
                                }
                                s32 gap = R(1.2), total = 0;
                                for (s32 k = 0; k < tcols; k++) total += widths[k] + gap;
                                if (total > text_w) for (s32 k = 0; k < tcols; k++) widths[k] = (s32)((s64)widths[k] * (text_w - tcols * gap) / Max(1, total - tcols * gap));
                                at = tat;
                                continue;
                            }
                            if (!trim.count) { Array_Push(&app->hist_arena, app->hist_lines)->kind = HistLine_Blank; at = end + 1; continue; }
                            b32 rule = trim.count >= 3;
                            for (s64 k = 0; k < trim.count; k++) rule &= trim.data[k] == trim.data[0] && strchr("-*_", trim.data[0]) != NULL;
                            if (rule) { Array_Push(&app->hist_arena, app->hist_lines)->kind = HistLine_Rule; at = end + 1; continue; }
                            // a heading, a list item, or a paragraph line
                            u32 kind = HistLine_Text;
                            String marker = {0};
                            s32 indent = 0;
                            if (trim.data[0] == '#') {
                                s64 k = 0;
                                while (k < trim.count && trim.data[k] == '#') k++;
                                if (k <= 6 && k < trim.count && trim.data[k] == ' ') { kind = HistLine_Heading; trim.data += k + 1; trim.count -= k + 1; }
                            } else if (trim.count >= 2 && strchr("-*+", trim.data[0]) && trim.data[1] == ' ') {
                                marker = S("\xE2\x80\xA2");
                                trim.data += 2; trim.count -= 2;
                            } else {
                                s64 k = 0;
                                while (k < trim.count && k < 4 && trim.data[k] >= '0' && trim.data[k] <= '9') k++;
                                if (k && k + 1 < trim.count && (trim.data[k] == '.' || trim.data[k] == ')') && trim.data[k + 1] == ' ') {
                                    marker = (String){ trim.data, k + 1 };
                                    trim.data += k + 2; trim.count -= k + 2;
                                }
                            }
                            if (marker.count) indent = (lead / 2) * R(1.4) + R(1.6);
                            // bold and code taken out of the text, kept as spans
                            u8 *plain = Arena_PushArray(&app->hist_arena, u8, trim.count + 1);
                            HistSpan *spans = Arena_PushArray(&app->hist_arena, HistSpan, trim.count / 2 + 2);
                            s32 span_count = 0, pn = 0;
                            u32 style = 0;
                            s32 span_from = 0;
                            for (s64 k = 0; k < trim.count; k++) {
                                u32 flip = 0;
                                if (trim.data[k] == '`') flip = HistSpan_Code;
                                else if (!(style & HistSpan_Code) && k + 1 < trim.count && trim.data[k] == '*' && trim.data[k + 1] == '*') { flip = HistSpan_Bold; k++; }
                                if (!flip) { plain[pn++] = trim.data[k]; continue; }
                                if (style && pn > span_from) spans[span_count++] = (HistSpan){ span_from, pn - span_from, style };
                                style ^= flip;
                                span_from = pn;
                            }
                            if (style && pn > span_from) spans[span_count++] = (HistSpan){ span_from, pn - span_from, style };
                            String para = { plain, pn };
                            Font *pf = kind == HistLine_Heading ? bold : font;
                            s32 line_from = 0;
                            while (line_from < para.count || (!para.count && !line_from)) {
                                String rest = { para.data + line_from, para.count - line_from };
                                s64 cut = Font_Fit(pf, rest, text_w - indent);
                                if (cut < rest.count) {
                                    s64 space = cut;
                                    while (space > 0 && rest.data[space - 1] != ' ') space--;
                                    if (space > 0) cut = space;
                                    if (cut <= 0) { cut = 1; while (cut < rest.count && (rest.data[cut] & 0xC0) == 0x80) cut++; }
                                }
                                HistLine *tl = Array_Push(&app->hist_arena, app->hist_lines);
                                tl->kind = kind;
                                tl->text = (String){ rest.data, cut };
                                tl->indent = indent;
                                tl->marker = line_from == 0 ? marker : (String){0};
                                tl->spans = Arena_PushArray(&app->hist_arena, HistSpan, span_count + 1);
                                for (s32 k = 0; k < span_count; k++) {
                                    s32 a = Max(spans[k].start, line_from), b = Min(spans[k].start + spans[k].len, line_from + (s32)cut);
                                    if (b > a) tl->spans[tl->span_count++] = (HistSpan){ a - line_from, b - a, spans[k].style };
                                }
                                line_from += (s32)cut;
                                if (!cut) break;
                            }
                            at = end + 1;
                        }
                        if (in_code) *Array_Push(&app->hist_arena, app->hist_blocks) = (String){ text.data + code_from, Max(0, text.count - code_from) };
                        Array_Push(&app->hist_arena, app->hist_lines)->kind = HistLine_Blank;
                    }
                    for (s64 li = 0; q.count && li < app->hist_lines.count; li++) {
                        String lt = app->hist_lines.data[li].text;
                        u32 lk = app->hist_lines.data[li].kind;
                        if (lk != HistLine_Text && lk != HistLine_Heading && lk != HistLine_Code) continue;
                        for (s64 at = 0; at + q.count <= lt.count; at++) {
                            b32 same = 1;
                            for (s64 k = 0; k < q.count && same; k++) {
                                u8 a = lt.data[at + k], ch = q.data[k];
                                if (!app->find_case && a >= 'A' && a <= 'Z') a += 32;
                                if (!app->find_case && ch >= 'A' && ch <= 'Z') ch += 32;
                                same = a == ch;
                            }
                            if (!same) continue;
                            HistMatch *m = Array_Push(&app->hist_arena, app->hist_matches);
                            m->line = (s32)li;
                            m->start = (s32)at;
                            m->len = (s32)q.count;
                            at += q.count - 1;
                        }
                    }
                    // typing goes to the newest match; a fresh read starts at the end
                    app->hist_current = app->hist_matches.count ? (s32)app->hist_matches.count - 1 : -1;
                    app->hist_follow = 1;
                    if (fresh && !q.count && app->hist_at_end) app->hist_scroll = 1e9f;
                }
                f32 content_h = (f32)(app->hist_lines.count * lh + 2 * R(0.6));
                s32 mc = (s32)app->hist_matches.count;
                if (app->hist_follow) {
                    app->hist_follow = 0;
                    if (app->hist_current >= 0 && app->hist_current < mc) app->hist_scroll = (f32)(app->hist_matches.data[app->hist_current].line * lh) - (f32)view_h / 2;
                    else if (!q.count) app->hist_scroll = 1e9f;
                }
                UiSignal vs = Ui_Hit(ui, Ui_Id("History"), content);
                if (in->input.wheel[1] && vs.hovered) app->hist_scroll -= (f32)in->input.wheel[1] / 120.0f * 3.0f * (f32)lh;
                app->hist_scroll = Clamp(app->hist_scroll, 0.0f, Max(0.0f, content_h - (f32)view_h));
                app->hist_at_end = app->hist_scroll >= content_h - (f32)view_h - (f32)lh;
                Draw_Rect(dl, content, c->bg, 0);
                Draw_PushClip(dl, content);
                if (in->history_tab != t->term_id || !in->history_version || app->hist_term != t->term_id || !app->hist_lines.count) {
                    String msg = app->hist_term == t->term_id && in->history_version ? S("no history yet") : S("reading the history...");
                    Draw_TextIn(dl, font, R4(content.x0, content.y0 + R(4), content.x1, content.y0 + R(6)), c->dim, msg, 0.5f);
                }
                s32 first = Max(0, (s32)(app->hist_scroll / (f32)lh) - 1);
                s32 last = Min((s32)app->hist_lines.count, first + view_h / lh + 3);
                s32 mi = 0;
                while (mi < mc && app->hist_matches.data[mi].line < first) mi++;
                for (s32 li = first; li < last; li++) {
                    const HistLine *hl = &app->hist_lines.data[li];
                    s32 ly = content.y0 + R(0.6) + li * lh - (s32)app->hist_scroll;
                    s32 lx = content.x0 + R(1.2);
                    // the matches lit, the current one framed
                    s64 mark_count = 0;
                    for (s32 k = mi; k < mc && app->hist_matches.data[k].line == li; k++) mark_count += app->hist_matches.data[k].len;
                    u32 *marks = mark_count ? Arena_PushArray(fa, u32, mark_count) : NULL;
                    mark_count = 0;
                    for (; mi < mc && app->hist_matches.data[mi].line == li; mi++) {
                        const HistMatch *m = &app->hist_matches.data[mi];
                        s64 cp = 0, bi = 0;
                        while (bi < m->start) { Utf8_Decode(hl->text, &bi); cp++; }
                        for (s64 e = m->start; e < m->start + m->len; cp++) { marks[mark_count++] = (u32)cp; Utf8_Decode(hl->text, &e); }
                        if (mi == app->hist_current) {
                            s32 fx = Font_Measure(font, (String){ hl->text.data, m->start });
                            s32 fw = Font_Measure(font, (String){ hl->text.data + m->start, m->len });
                            Draw_RectEx(dl, R4(lx + fx - R(0.2), ly, lx + fx + fw + R(0.2), ly + lh), 0, c->accent, 1, radius_small, NULL, 0, 0, 0);
                        }
                    }
                    if (hl->kind == HistLine_Blank) continue;
                    u32 code_bg = ColorMix(c->bg, c->text, 0.07f);
                    if (hl->kind == HistLine_Rule) { Draw_Rect(dl, R4(lx, ly + lh / 2, lx + text_w, ly + lh / 2 + line_w), c->line, 0); continue; }
                    if (hl->kind == HistLine_Code) {
                        // a code block's line on its shade, mono; its first line has the block's copy button
                        Draw_Rect(dl, R4(lx, ly, lx + text_w, ly + lh), code_bg, 0);
                        Draw_TextMarked(dl, mono, lx + R(0.8), ly + (lh - (s32)(mono->line_h + 0.5f)) / 2, c->text, hl->text, marks, mark_count, 0xFF000000u | tt->ansi[3]);
                        if (hl->first && hl->block < app->hist_blocks.count) {
                            Rect cb = R4(lx + text_w - R(2.2), ly + R(0.1), lx + text_w - R(0.2), ly + lh - R(0.1));
                            UiSignal ccs = IconButton(app, Ui_IdN(Ui_Id("CopyCode"), (u64)hl->block), cb, Icon_Copy, icons_small, c->dim, c->hover, radius_small,
                                                     Str_PushFormat(fa, "copy code %d", hl->block + 1));
                            if (ccs.clicked) { out->clipboard_set = 1; out->clipboard_text = app->hist_blocks.data[hl->block]; app->hist_copied = hl->block + 1; app->hist_copied_us = now; }
                            if (app->hist_copied == hl->block + 1 && now - app->hist_copied_us < 1500000) {
                                Draw_TextIn(dl, small, R4(cb.x0 - R(5), cb.y0, cb.x0 - R(0.3), cb.y1), c->accent, S("Copied"), 1);
                                if (!out->wake_at_us || app->hist_copied_us + 1500000 < out->wake_at_us) out->wake_at_us = app->hist_copied_us + 1500000;
                            }
                        }
                        continue;
                    }
                    if (hl->kind == HistLine_Table) {
                        // cells in their columns, the header row bold and underlined
                        s32 x = lx;
                        for (s32 k = 0; k < hl->cell_count; k++) {
                            Draw_TextIn(dl, hl->first ? bold : font, R4(x, ly, x + hl->widths[k], ly + lh), c->text, hl->cells[k], 0);
                            x += hl->widths[k] + R(1.2);
                        }
                        if (hl->first) Draw_Rect(dl, R4(lx, ly + lh - line_w, x - R(1.2), ly + lh), c->line, 0);
                        continue;
                    }
                    if (hl->kind == HistLine_User || hl->kind == HistLine_Claude) {
                        if (hl->kind == HistLine_User) Draw_Rect(dl, R4(lx - R(0.6), ly, lx + text_w, ly + lh), ColorAlpha(c->accent, 0.10f), radius_small);
                        Draw_Text(dl, small, lx, ly + (lh - (s32)(small->line_h + 0.5f)) / 2, c->dim, hl->text);
                        continue;
                    }
                    // a paragraph's, heading's or list item's line: its marker, then its runs (bold, `code` on a shade)
                    Font *lf = hl->kind == HistLine_Heading ? bold : font;
                    s32 tx = lx + hl->indent, ty = ly + (lh - (s32)(lf->line_h + 0.5f)) / 2;
                    if (hl->marker.count) Draw_Text(dl, font, tx - Font_Measure(font, hl->marker) - R(0.5), ty, c->dim, hl->marker);
                    if (mark_count || !hl->span_count) { Draw_TextMarked(dl, lf, tx, ty, c->text, hl->text, marks, mark_count, 0xFF000000u | tt->ansi[3]); continue; }
                    s32 at = 0;
                    for (s32 k = 0; k <= hl->span_count; k++) {
                        s32 to = k < hl->span_count ? hl->spans[k].start : (s32)hl->text.count;
                        if (to > at) tx = Draw_Text(dl, lf, tx, ty, c->text, (String){ hl->text.data + at, to - at });
                        if (k == hl->span_count) break;
                        const HistSpan *sp = &hl->spans[k];
                        String run = { hl->text.data + sp->start, sp->len };
                        if (sp->style & HistSpan_Code) {
                            s32 w = Font_Measure(mono, run);
                            Draw_Rect(dl, R4(tx - R(0.15), ly + R(0.15), tx + w + R(0.15), ly + lh - R(0.15)), code_bg, radius_small);
                            tx = Draw_Text(dl, mono, tx, ly + (lh - (s32)(mono->line_h + 0.5f)) / 2, c->text, run);
                        } else {
                            tx = Draw_Text(dl, bold, tx, ty, c->text, run);
                        }
                        at = sp->start + sp->len;
                    }
                }
                Draw_PopClip(dl);
            }
        }
        // --- the composer over the bottom of its terminal: as tall as its text (3 lines to half the pane), the keys
        //     under it ---
        if (app->compose_open && t && t->term_id == app->compose_term && !t->browsing && !t->viewing) {
            s32 lh = (s32)(font->line_h + 0.5f), pad = R(0.9), keys_h = R(1.8);
            s32 inner_w = RectW(content) - 2 * R(1.0) - 2 * pad;
            s32 lines = Clamp(TextArea_Lines(ui, &app->compose, font, inner_w), 3, Max(3, (RectH(content) / 2 - 2 * pad - keys_h) / Max(1, lh)));
            Rect box = R4(content.x0 + R(1.0), content.y1 - R(0.8) - (lines * lh + 2 * pad + keys_h), content.x1 - R(1.0), content.y1 - R(0.8));
            Ui_Hit(ui, Ui_IdN(Ui_Id("Compose"), n->id), box);
            Draw_RectEx(dl, box, c->surface, focused_pane ? c->accent : c->line, (f32)Max(1, R(0.1)), radius_medium, NULL, 0, 0, (f32)R(1));
            Rect body = RectShrink(box, pad);
            Rect keys = CutBottom(&body, keys_h);
            Ui_TextArea(ui, Ui_IdN(Ui_Id("ComposeText"), n->id), &app->compose, body, font, focused_pane && !modal,
                        S("Write a prompt..."), c->text, c->dim, c->selection);
            Draw_TextIn(dl, small, keys, c->dim, S("Ctrl+Enter sends  \xC2\xB7  Enter a new line  \xC2\xB7  Ctrl+V text or a picture  \xC2\xB7  drop files  \xC2\xB7  Esc away"), 0);
            Named(app, S("composer"), box);
        }
        // --- the panes not in focus a shade darker (settings `dim_unfocused = off`: not), when more than one shows ---
        if (!app->dim_unfocused_off && !focused_pane && panes->root->first_child && !app->zoom_node) {
            Draw_Rect(dl, pane, ColorAlpha(0xFF000000u, tt->light ? 0.10f : 0.32f), 0);
        }
        // --- a held claude (the server stopped its turn and drops what's typed into it) until "Go on" here: its login,
        //     organization or email changed under it (what it was, what it is now), or it breaks a folder rule (why) ---
        if (account_tab && (account_tab->flags & TermTab_AccountHeld)) {
            b32 by_rule = account_tab->hold_why.count > 0;
            Rect card = RectCenter(content, Max(1, Min(R(56), RectW(content) - R(2))), Max(1, Min(R(13), RectH(content) - R(1))));
            Ui_Hit(ui, Ui_IdN(Ui_Id("AccountHeld"), n->id), card);
            Draw_RectEx(dl, card, c->surface, c->danger, (f32)Max(1, R(0.1)), radius_large, NULL, 0, 0, (f32)R(1));
            Rect body = RectShrink(card, R(1.2));
            Draw_TextIn(dl, bold, CutTop(&body, R(2)), c->text, by_rule ? S("Wrong Claude account for this folder") : S("Claude's account changed"), 0);
            Draw_TextIn(dl, small, CutTop(&body, R(1.8)), c->dim, by_rule ? account_tab->hold_why :
                        S("Its turn was stopped and typing is held back, so nothing goes to the new account."), 0);
            CutTop(&body, R(0.4));
            for (s32 k = by_rule ? 1 : 0; k < 2; k++) {
                String acct = k ? account_tab->account : account_tab->account_was;
                String m = AccountPart(acct, 0), o = AccountPart(acct, 1), e = AccountPart(acct, 2);
                String who = Str_PushFormat(fa, "%.*s%s%.*s%s%.*s", STR_ARG(m), e.count ? "  \xC2\xB7  " : "", STR_ARG(e),
                                            o.count ? "  \xC2\xB7  " : "", STR_ARG(o));   // the email before a long organization
                Rect line = CutTop(&body, R(1.9));
                Draw_TextIn(dl, small, CutLeft(&line, R(3)), c->dim, by_rule ? S("Uses") : k ? S("Now") : S("Was"), 0);
                Draw_TextIn(dl, font, line, k ? c->text : c->dim, who, 0);
            }
            Rect brow = CutBottom(&body, R(2.4));
            String go = by_rule ? S("Go on anyway") : S("Go on with the new account");
            Rect gb = CutRight(&brow, Min(Font_Measure(font, go) + R(2), RectW(brow)));
            UiSignal gs = Ui_Hit(ui, Ui_IdN(Ui_Id("AccountGoOn"), n->id), gb);
            Draw_Rect(dl, gb, gs.hovered ? ColorMix(c->accent, 0xFFFFFFFFu, 0.12f) : c->accent, (f32)R(0.35));
            Draw_TextIn(dl, font, gb, c->on_accent, go, 0.5f);
            Draw_TextIn(dl, small, brow, c->dim, S("or close the tab"), 0);
            Named(app, by_rule ? S("wrong account") : S("account changed"), card);
            Named(app, S("account go on"), gb);
            if (gs.clicked) out->account_ok = account_tab->id;
        }
        Draw_PopClip(dl);   // pane
        pane_index++;
    }

    c = &app->c;
    if (close_later) CloseTab(app, &f, close_later, 0);

    // --- web pages: where they go this frame (asked in the address row or by its keys), the see-through frame over
    //     them, and the keyboard for the focused one while nothing of moss's wants it ---
    for (s32 i = 0; i < out->web_count; i++) {
        PaneTab *o = FindPaneTab(panes, out->webs[i].id);
        if (!o) continue;
        if (o->web_go_len) out->webs[i].navigate = Str_PushCopy(fa, (String){ o->web_go, o->web_go_len });
        out->webs[i].step = o->web_step;
        out->webs[i].reload = o->web_reload;
        o->web_go_len = 0;
        o->web_step = 0;
        o->web_reload = 0;
    }
    if (web_shown) out->clear_color = 0;
    PaneTab *web_focus = panes->selected;
    if (web_focus && web_focus->web && !web_focus->hidden && !modal && !app->web_edit_tab && !app->rename_tab && !app->side_focus && !app->find_open) out->web_focus = web_focus->id;

    // --- the splitters again, on top: their lines, and their hit areas over the panes' ---
    for (s32 i = 0; i < splitter_count; i++) {
        Splitter *sp = &splitters[i];
        UiSignal s = Ui_Hit(ui, sp->id, sp->r);
        b32 lit = s.hovered || s.held;
        s32 w = lit ? 2 * line_w : line_w;
        Rect line = sp->axis == 0 ? R4((sp->r.x0 + sp->r.x1 - w) / 2, sp->r.y0 - caption_h, (sp->r.x0 + sp->r.x1 + w) / 2, sp->r.y1)
                                  : R4(sp->r.x0, (sp->r.y0 + sp->r.y1 - w) / 2, sp->r.x1, (sp->r.y0 + sp->r.y1 + w) / 2);
        Draw_Rect(dl, line, lit ? c->accent : c->line, 0);
        Named(app, Str_PushFormat(fa, "splitter %d %s", sp->leaf_index, sp->axis == 0 ? "left" : "top"), sp->r);
    }

    // --- the caption buttons, over the top-right pane's strip (none in focus); focus's hint for a moment ---
    if (app->focus_mode && now - app->focus_since_us < 1800000) {
        f32 fade = Min(1.0f, (f32)(1800000 - (now - app->focus_since_us)) / 400000.0f);
        String hint = Str_PushFormat(fa, "Focus  \xC2\xB7  %.*s to leave", STR_ARG(HotkeyName(fa, app->bind_key[Cmd_FocusMode], app->bind_mods[Cmd_FocusMode])));
        s32 hw = Font_Measure(small, hint) + R(2);
        Rect hr = R4(in->client_w - hw - R(1), in->client_h - R(3.2), in->client_w - R(1), in->client_h - R(1));
        dl->fade = 1.0f - fade;
        PopupBox(app, hr, radius_medium);
        Draw_TextIn(dl, small, hr, c->text, hint, 0.5f);
        dl->fade = 0;
        Named(app, S("focus hint"), hr);
        ui->animating = 1;
    }
    if (!app->focus_mode) {
        s32 bw = R(3.4);
        Rect bar = R4(in->client_w - 3 * bw, 0, in->client_w, caption_h);
        Rect rmin = CutLeft(&bar, bw), rmax = CutLeft(&bar, bw), rclose = bar;
        u32 fg = in->foreground || app->headless ? c->text : c->dim;
        if (IconButton(app, Ui_Id("Minimize"), rmin, Icon_Minimize, icons_small, fg, c->hover, 0, S("minimize")).clicked) out->window_command = AppWindow_Minimize;
        if (IconButton(app, Ui_Id("Maximize"), rmax, in->maximized ? Icon_Restore : Icon_Maximize, icons_small, fg, c->hover, 0, S("maximize")).clicked) out->window_command = AppWindow_ToggleMaximize;
        UiSignal cls = Ui_Hit(ui, Ui_Id("CloseWindow"), rclose);
        if (cls.hovered || cls.held) Draw_Rect(dl, rclose, cls.held ? ColorMix(c->danger, 0xFF000000u, 0.15f) : c->danger, 0);
        Draw_Icon(dl, icons_small, rclose, cls.hovered ? 0xFFFFFFFFu : fg, Icon_Close);
        Named(app, S("close window"), rclose);
        if (cls.clicked) out->window_command = AppWindow_Close;
        for (s32 k = 0; k < 3 && caption_rect_count < App_MaxCaptionClientRects; k++) caption_rects[caption_rect_count++] = k == 0 ? rmin : k == 1 ? rmax : rclose;
    }

    // --- the find bar: top right of its pane, under the tab strip. Its text, how many matches and which is current,
    //     older / newer, close ---
    if (app->find_open) {
        PaneTab *ft = Panes_FindTerm(panes, app->find_term);
        PaneNode *fn = ft ? ft->node : NULL;
        s32 bar_w = Min(R(30), fn ? (s32)(fn->anim_px[2] - fn->anim_px[0]) - R(1) : R(30));
        s32 bx = fn ? RoundS32(fn->anim_px[2]) - bar_w - R(0.8) : 0, by = fn ? RoundS32(fn->anim_px[1]) + caption_h + R(0.5) : caption_h;
        Rect bar = R4(bx, by, bx + bar_w, by + R(2.8));
        s32 slide = PopupAppear(app, Ui_Id("FindBarIn"));
        bar.y0 += slide;
        bar.y1 += slide;
        app->find_rect = bar;
        Ui_Hit(ui, Ui_Id("FindBar"), bar);
        PopupBox(app, bar, radius_medium);
        Named(app, S("find bar"), bar);
        Rect in_bar = RectShrink(bar, R(0.4));
        Rect bclose = CutRight(&in_bar, R(2.2)), bdown = CutRight(&in_bar, R(2.2)), bup = CutRight(&in_bar, R(2.2));
        CutRight(&in_bar, R(0.3));
        Rect field = in_bar;
        Draw_RectEx(dl, field, c->bg, app->find_focus ? c->accent : c->line, 1, radius_small, NULL, 0, 0, 0);
        CutLeft(&field, R(0.5));
        CutRight(&field, R(0.5));
        s32 found = app->find_history ? (s32)app->hist_matches.count : in->find_count;
        s32 found_at = app->find_history ? app->hist_current + 1 : in->find_index;
        String count = !app->find_edit.count ? (String){0} : !app->find_history && in->find_tab != app->find_term ? S("...") :
                       !found ? S("No results") : Str_PushFormat(fa, "%d of %d%s", found_at, found, app->find_focus ? "" : "  n / N");
        Rect count_r = CutRight(&field, Font_Measure(small, count));
        Draw_TextIn(dl, small, count_r, c->dim, count, 1);
        Named(app, S("find count"), count_r);
        CutRight(&field, R(0.4));
        Ui_TextEdit(ui, Ui_Id("FindText"), &app->find_edit, field, font, app->find_focus && !modal,
                    app->find_history ? S("Search Claude's history") : S("Search"), c->text, c->dim, c->selection);
        if (IconButton(app, Ui_Id("FindOlder"), bup, Icon_Up, icons_small, c->dim, c->hover, radius_small, S("find older")).clicked) find_step = 2;
        if (IconButton(app, Ui_Id("FindNewer"), bdown, Icon_Down, icons_small, c->dim, c->hover, radius_small, S("find newer")).clicked) find_step = 4;
        if (IconButton(app, Ui_Id("FindClose"), bclose, Icon_Cancel, icons_small, c->dim, c->hover, radius_small, S("find close")).clicked) find_close = 1;
        dl->fade = 0;
        // --- to the server: the text when it (or the case) changed, and steps ---
        u64 h = Hash_Bytes(app->find_edit.buffer, (u64)app->find_edit.count, 0xF1D + (u64)app->find_case) | 1;
        s32 mc = (s32)app->hist_matches.count;
        if (app->find_history && find_step && mc) {
            // the history's: older (up) / newer, around the ends
            app->hist_current = app->hist_current < 0 ? mc - 1 : (app->hist_current + (find_step == 2 ? mc - 1 : 1)) % mc;
            app->hist_follow = 1;
        }
        if (find_close) {
            app->find_open = 0;
        } else if (!app->find_history && (h != app->find_sent || find_step)) {
            app->find_sent = h;
            out->find_send = 1;
            out->find_tab = app->find_term;
            out->find_flags = (app->find_case ? 1u : 0u) | (u32)find_step;
            out->find_text = Str_PushCopy(fa, TextEdit_String(&app->find_edit));
        }
    }
    if (!app->find_open && app->find_sent) {
        // closed: the marks go
        app->find_sent = 0;
        out->find_send = 1;
        out->find_tab = app->find_term;
    }

    // --- the tab drag: its preview under the buttons, the buttons, the tab by the pointer ---
    if ((drag_tab || in->foreign_drag) && !drag_outside) {
        u32 bg = c->bg, fg = c->text, accent = c->accent;
        u32 button_bg = ColorMix(bg, 0xFF000000u, 0.28f), button_edge = ColorMix(bg, fg, 0.35f);
        u32 cell_bg = ColorMix(bg, 0xFF000000u, 0.15f), cell_edge = ColorMix(bg, fg, 0.3f);
        u32 new_bg = ColorMix(bg, accent, 0.45f), new_hot_bg = ColorMix(bg, accent, 0.8f);
        if (chosen >= 0) {
            // keyed by the button, so a new one grows in afresh
            DropButton *b = &buttons[chosen];
            f32 a = Ui_Animate(ui, Ui_IdN(Ui_IdN(Ui_Id("TabDropArea"), b->node_id), (u64)(chosen * 4 + b->after * 2 + b->into)), 0, 1, 18);
            f32 pv[4] = { b->preview[0] + (f32)R(0.6), b->preview[1] + (f32)R(0.6), b->preview[2] - (f32)R(0.6), b->preview[3] - (f32)R(0.6) };
            for (s32 k = 0; k < 2; k++) {
                f32 s = LerpF32(b->grow_scale[k], 1, a);
                f32 pivot = LerpF32(pv[k], pv[k + 2], b->grow_pivot[k]);
                pv[k] = pivot + (pv[k] - pivot) * s;
                pv[k + 2] = pivot + (pv[k + 2] - pivot) * s;
            }
            Draw_Rect(dl, R4(RoundS32(pv[0]), RoundS32(pv[1]), RoundS32(pv[2]), RoundS32(pv[3])), ColorAlpha(accent, 0.14f * a + 0.02f), radius_medium);
        }
        s32 cell_pad = R(0.45), cell_gap = R(0.7);
        f32 appear = Ui_Animate(ui, Ui_Id("TabDropButtons"), 0, 1, 18);
        for (s32 i = 0; i < button_count; i++) {
            DropButton *b = &buttons[i];
            b32 hot = i == chosen;
            f32 s = LerpF32(0.9f, 1, appear);
            s32 cx = (b->r.x0 + b->r.x1) / 2, cy = (b->r.y0 + b->r.y1) / 2;
            s32 hw = RoundS32((f32)RectW(b->r) * s / 2), hh = RoundS32((f32)RectH(b->r) * s / 2);
            Rect br = R4(cx - hw, cy - hh, cx + hw, cy + hh);
            Draw_RectEx(dl, br, ColorAlpha(button_bg, appear), ColorAlpha(button_edge, appear), (f32)line_w, radius_large, NULL, (f32)Max(3, R(0.4)), (f32)Max(2, R(0.3)), 0);
            // the cells: the pane(s) it goes next to, and the new one (lit; brighter under the pointer)
            Rect inner = RectShrink(br, cell_pad);
            s32 length = (b->vertical ? RectW(inner) : RectH(inner)) - (b->cells - 1) * cell_gap;
            s32 at = b->vertical ? inner.x0 : inner.y0;
            for (s32 k = 0; k < b->cells; k++) {
                b32 is_new = b->cells == 1 || (b->cells == 2 && (k == 0) == b->new_first) || (b->cells == 3 && k == 1);
                f32 share = b->cells == 3 ? (k == 1 ? 0.25f : 0.375f) : b->cells == 2 ? 0.5f : 1.0f;
                s32 len = k == b->cells - 1 ? (b->vertical ? inner.x1 : inner.y1) - at : RoundS32((f32)length * share);
                Rect cr = b->vertical ? R4(at, inner.y0, at + len, inner.y1) : R4(inner.x0, at, inner.x1, at + len);
                at += len + cell_gap;
                Draw_RectEx(dl, cr, ColorAlpha(is_new ? (hot ? new_hot_bg : new_bg) : cell_bg, appear), ColorAlpha(is_new ? accent : cell_edge, appear),
                            (f32)line_w, Max(1.0f, radius_large - (f32)cell_pad), NULL, 0, 0, 0);
            }
        }
        if (ui->dragging || in->foreign_drag) {
            // --- by the pointer: the tab dragged, and what a drop does ---
            Rect card = R4(mouse_x + R(0.25), mouse_y + R(0.75), mouse_x + R(0.25) + R(13), mouse_y + R(0.75) + R(4.2));
            PopupBox(app, card, radius_medium);
            Rect top = card;
            Rect name_r = CutTop(&top, R(2.2));
            f32 radii[4] = { radius_medium, radius_medium, 0, 0 };
            Draw_RectEx(dl, name_r, c->bg, 0, 0, 0, radii, 0, 0, 0);
            CutLeft(&name_r, R(0.7));
            Draw_TextIn(dl, font, name_r, c->text, drag_tab ? PaneTabLabel(app, drag_tab) : in->foreign_label, 0);
            Draw_TextIn(dl, font, top, c->dim, S("Move"), 0.5f);
        }
    }

    // --- a tab's menu (right-click on it): rename, a color, to a new window, close others / to the right, close ---
    if (app->tab_menu) {
        PaneTab *mt = FindPaneTab(panes, app->tab_menu);
        b32 outside = Blocker(app, 0);
        s32 mw = R(19), row_h = R(2.2), pad = R(0.4);
        s32 mx = Min(app->tab_menu_at[0], in->client_w - mw - R(0.5));
        Rect menu = R4(mx, app->tab_menu_at[1], mx + mw, app->tab_menu_at[1] + 2 * pad + 5 * row_h + R(2.4));
        s32 slide = PopupAppear(app, Ui_IdN(Ui_Id("TabMenuIn"), app->tab_menu));
        menu.y0 += slide;
        menu.y1 += slide;
        Ui_Hit(ui, Ui_Id("TabMenu"), menu);
        PopupBox(app, menu, radius_medium);
        Rect rows_r = RectShrink(menu, pad);
        s32 pick = -1;
        if (MenuRow(app, CutTop(&rows_r, row_h), S("Rename"), S("double-click"))) pick = 0;
        // --- colors: none, then red .. cyan from the theme ---
        Rect swatches = CutTop(&rows_r, R(2.4));
        CutLeft(&swatches, R(0.6));
        for (u8 k = 0; k <= 6; k++) {
            Rect sw = RectCenter(CutLeft(&swatches, R(1.6)), R(1.6), R(1.6));
            CutLeft(&swatches, R(0.5));
            UiSignal s = Ui_Hit(ui, Ui_IdN(Ui_Id("TabColor"), k), sw);
            u32 fill = k ? 0xFF000000u | tt->ansi[k] : c->bg;
            b32 on = mt && mt->color == k;
            Draw_RectEx(dl, sw, fill, on ? c->text : s.hovered ? c->dim : c->line, on ? 2.0f : 1.0f, (f32)R(0.8), NULL, 0, 0, 0);
            Named(app, Str_PushFormat(fa, "tab color %d", k), sw);
            if (s.clicked && mt) { mt->color = k; pick = 9; }
        }
        if (MenuRow(app, CutTop(&rows_r, row_h), S("Move to a new window"), (String){0})) pick = 1;
        if (MenuRow(app, CutTop(&rows_r, row_h), S("Close the tabs to the right"), (String){0})) pick = 2;
        if (MenuRow(app, CutTop(&rows_r, row_h), S("Close the other tabs"), (String){0})) pick = 3;
        if (MenuRow(app, CutTop(&rows_r, row_h), S("Close"), HotkeyName(fa, app->bind_key[Cmd_CloseTab], app->bind_mods[Cmd_CloseTab]))) pick = 4;
        b32 esc = 0;
        for (s64 i = 0; i < event_count; i++) if (events[i].down && events[i].key == Key_Escape) esc = 1;
        if (mt && pick >= 0 && pick != 9) {
            Panes_Select(panes, mt);
            if (pick == 0) RunCommand(app, &f, Cmd_RenameTab);
            if (pick == 1) RunCommand(app, &f, Cmd_TearOff);
            if (pick == 2 || pick == 3) {
                // the pane's others (or those after it): closed, each asking first if something runs there
                for (PaneTab *o = mt->node->first_tab, *next = NULL; o; o = next) {
                    next = o->next;
                    b32 after = 0;
                    for (PaneTab *x = mt->next; x && !after; x = x->next) after = x == o;
                    if (o != mt && (pick == 3 || after)) CloseTab(app, &f, o, 0);
                }
            }
            if (pick == 4) CloseTab(app, &f, mt, 0);
        }
        if (pick >= 0 || esc || !mt || outside) app->tab_menu = 0;
        dl->fade = 0;
    }

    // --- the command palette: type to filter, Enter or a click runs; in settings mode the settings by group ---
    if (app->palette_open) {
        b32 settings = app->palette_mode == PaletteMode_Settings, go = app->palette_mode == PaletteMode_GoTo;
        b32 accounts_mode = app->palette_mode == PaletteMode_Account, model_mode = app->palette_mode == PaletteMode_Model;
        b32 outside = Blocker(app, 0);
        s32 box_w = Min(settings ? R(52) : go ? R(58) : app->palette_mode == PaletteMode_Account ? R(56) : R(40), in->client_w - R(4));
        s32 row_h = settings ? R(2.4) : R(2.2), pad = R(0.5);
        s32 fit = Max(3, (in->client_h - caption_h - R(8)) / row_h);
        String filter = TextEdit_String(&app->palette_edit);
        u64 filter_hash = Hash_Bytes(filter.data, (u64)filter.count, 0x5E1) | 1;
        if (filter_hash != app->palette_filter_hash) { app->palette_sel = 0; app->palette_top = 0; app->palette_filter_hash = filter_hash; }   // new text: the first match
        b32 run = 0, esc = 0, go_select = 0;   // go_select: Ctrl+Enter, the row selected in the browser rather than opened
        s32 act = -1, step = 0;
        s32 account_act = -1, account_step = 0;   // the accounts palette: an account's theme stepped (arrows)   // settings: the setting to change, and how: +1/-1 steps, 0 activates (Enter, click), 2 an edit is done
        // --- the rows: settings (a group's heading, -1 - its first row, before its rows), or tabs whose name or folder
        //     has the text (not without text) and then commands ---
        typedef struct PaletteRow { s32 item; u32 cmd; PaneTab *tab; } PaletteRow;   // item: a setting, or -1 - a heading's setting (go-to: a GotoRow, or -1 - a group)
        s64 row_cap = 2 * Setting_Count + Cmd_Count + 16 + (go ? app->goto_count + app->recent_count + app->bookmark_count + app->drive_count + app->place_count + 16 : 0);
        PaletteRow *rows = Arena_PushArray(fa, PaletteRow, row_cap);
        s32 row_count = 0, pickable = 0;
        s32 *pick_row = Arena_PushArray(fa, s32, row_cap);   // the rows the selection steps through
        // --- the go-to box's rows: a typed path (%NAME% and a leading ~ filled in) lists its folder, by the text after
        //     its last separator (with no text after it, the folder itself first); else the groups (recents, bookmarks,
        //     the drives, places), all of them ranked by the text when there is some ---
        typedef struct GotoRow { String label, path, note; u32 icon, flags; u64 size, free; s32 group; } GotoRow;   // group: Goto_*
        enum { Goto_Recents, Goto_Bookmarks, Goto_Storage, Goto_Places, Goto_Groups, Goto_Folder = 8, Goto_Entry = 9 };
        static const char *const group_names[Goto_Groups] = { "Recents", "Bookmarks", "Storage", "Places" };
        static const u32 group_icons[Goto_Groups] = { Icon_History, Icon_StarFilled, Icon_HardDrive, Icon_Home };
        GotoRow *go_rows = go ? Arena_PushArray(fa, GotoRow, row_cap) : NULL;
        s32 go_count = 0;
        String go_text = {0}, go_dir = {0};
        b32 go_path = 0;
        if (go) {
            u8 *ex = Arena_PushArray(fa, u8, 4200);
            s64 n = 0;
            for (s64 k = 0; k < filter.count && n < 4096; k++) {
                u8 ch = filter.data[k];
                s64 close = k + 1;
                while (ch == '%' && close < filter.count && filter.data[close] != '%') close++;
                String value = ch == '%' && close < filter.count && close > k + 1 ? EnvValue(app->environment, (String){ filter.data + k + 1, close - k - 1 }) : (String){0};
                if (!value.count && k == 0 && ch == '~' && (filter.count == 1 || filter.data[1] == '\\' || filter.data[1] == '/')) value = EnvValue(app->environment, S("USERPROFILE"));
                if (value.count) {
                    s64 take = Min(value.count, 4096 - n);
                    memcpy(ex + n, value.data, (size_t)take);
                    n += take;
                    if (ch == '%') k = close;
                    continue;
                }
                ex[n++] = ch == '/' ? '\\' : ch;
            }
            go_text = (String){ ex, n };
            go_path = (n >= 2 && ex[1] == ':') || (n >= 2 && ex[0] == '\\' && ex[1] == '\\');
            if (go_path) {
                s64 cut = -1;
                for (s64 k = 0; k < n; k++) if (ex[k] == '\\') cut = k;
                go_dir = cut < 0 ? (String){0} : BrowsePath((String){ ex, cut });
                String rest = { ex + cut + 1, n - cut - 1 };
                if (!Str_EqualNoCase(go_dir, app->goto_dir) || (!app->goto_listed && !app->goto_request)) {
                    // a new folder typed: listed (what the last one had goes)
                    app->goto_dir = CopyInto(app->goto_dir_buf, sizeof(app->goto_dir_buf), go_dir);
                    app->goto_request = ++app->next_file_request;
                    app->goto_listed = 0;
                    app->goto_count = 0;
                    FileRequest *r = Array_Push(fa, f.file_requests);
                    r->request = app->goto_request;
                    r->path = ShellPath(fa, go_dir);
                }
                if (!rest.count) {
                    pick_row[pickable++] = row_count;
                    rows[row_count++] = (PaletteRow){ go_count, 0, NULL };
                    go_rows[go_count++] = (GotoRow){ go_dir.count ? go_dir : S("This PC"), go_dir, S("this folder"), Icon_OpenFolder, File_Dir, 0, 0, Goto_Folder };
                }
                GotoSort *order = Arena_PushArray(fa, GotoSort, app->goto_count + 1);
                s64 order_count = 0;
                for (s64 k = 0; k < app->goto_count; k++) {
                    const FileEntry *e = &app->goto_entries[k];
                    if (app->hide_hidden && (e->flags & File_Hidden)) continue;
                    s32 score = rest.count ? FuzzyScore(e->name, rest) : 0;
                    if (score < 0) continue;
                    order[order_count++] = (GotoSort){ score, (e->flags & File_Dir) != 0, e->name, (s32)k };
                }
                qsort(order, (size_t)order_count, sizeof(GotoSort), CompareGotoSort);
                for (s64 k = 0; k < order_count; k++) {
                    const FileEntry *e = &app->goto_entries[order[k].index];
                    b32 drive = (e->flags & File_Drive) != 0;
                    String path = go_dir.count ? Str_PushFormat(fa, "%.*s\\%.*s", STR_ARG(go_dir), STR_ARG(e->name)) : BrowsePath(e->name);
                    String label = drive && e->label.count ? Str_PushFormat(fa, "%.*s (%.*s)", STR_ARG(e->label), (int)Min(e->name.count, 2), (const char *)e->name.data) : e->name;
                    pick_row[pickable++] = row_count;
                    rows[row_count++] = (PaletteRow){ go_count, 0, NULL };
                    go_rows[go_count++] = (GotoRow){ label, path, {0}, drive ? Icon_HardDrive : (e->flags & File_Dir) ? Icon_Folder : Icon_Page, e->flags, e->size, e->free, Goto_Entry };
                }
            } else {
                // --- the groups, in order; with text, every item ranked by it (its name, or 3/4 for its path) ---
                if (!app->drives_request) {
                    app->drives_request = ++app->next_file_request;
                    FileRequest *r = Array_Push(fa, f.file_requests);
                    r->request = app->drives_request;
                    r->path = (String){0};
                }
                s32 *scores = Arena_PushArray(fa, s32, row_cap);
                for (s32 g = 0; g < Goto_Groups; g++) {
                    s32 items = g == Goto_Recents ? app->recent_count : g == Goto_Bookmarks ? app->bookmark_count : g == Goto_Storage ? (s32)app->drive_count : app->place_count;
                    if (!items) continue;
                    b32 closed = (app->goto_closed >> g) & 1;
                    if (!go_text.count) rows[row_count++] = (PaletteRow){ -1 - g, 0, NULL };
                    if (!go_text.count && closed) continue;
                    s32 shown_items = !go_text.count && g == Goto_Recents && !app->goto_all_recents ? Min(items, 10) : items;
                    for (s32 k = 0; k < shown_items; k++) {
                        GotoRow r = { .group = g, .flags = File_Dir, .icon = group_icons[g] };
                        if (g == Goto_Recents || g == Goto_Bookmarks) {
                            r.path = g == Goto_Recents ? (String){ app->recent[k], app->recent_len[k] } : (String){ app->bookmark[k], app->bookmark_len[k] };
                            s64 cut = r.path.count;
                            while (cut > 0 && r.path.data[cut - 1] != '\\') cut--;
                            r.label = cut > 0 && cut < r.path.count ? (String){ r.path.data + cut, r.path.count - cut } : r.path;
                            r.icon = Icon_Folder;
                        } else if (g == Goto_Storage) {
                            const FileEntry *e = &app->drives[k];
                            r.path = BrowsePath(e->name);
                            r.label = e->label.count ? Str_PushFormat(fa, "%.*s (%.*s)", STR_ARG(e->label), STR_ARG(r.path)) : Str_PushFormat(fa, "Drive (%.*s)", STR_ARG(r.path));
                            r.size = e->size;
                            r.free = e->free;
                            r.flags = e->flags;
                        } else {
                            r.path = app->places[k].path;
                            r.label = app->places[k].name;
                            String nm = r.label;
                            r.icon = Str_EqualNoCase(nm, S("Desktop")) ? Icon_Desktop : Str_EqualNoCase(nm, S("Documents")) ? Icon_Document : Str_EqualNoCase(nm, S("Downloads")) ? Icon_Download :
                                     Str_EqualNoCase(nm, S("Pictures")) ? Icon_Picture : Str_EqualNoCase(nm, S("Music")) ? Icon_Music : Str_EqualNoCase(nm, S("Videos")) ? Icon_Video : Icon_Home;
                        }
                        s32 score = 0;
                        if (go_text.count) {
                            s32 by_path = FuzzyScore(r.path, go_text);
                            score = Max(FuzzyScore(r.label, go_text), by_path < 0 ? -1 : by_path * 3 / 4);
                            if (score < 0) continue;
                        }
                        r.note = g == Goto_Storage ? (String){0} : r.path;
                        scores[go_count] = score;
                        pick_row[pickable++] = row_count;
                        rows[row_count++] = (PaletteRow){ go_count, 0, NULL };
                        go_rows[go_count++] = r;
                    }
                }
                for (s32 i = 1; go_text.count && i < row_count; i++) {
                    // best first; ties keep the groups' order
                    PaletteRow r = rows[i];
                    s32 j = i;
                    while (j > 0 && scores[rows[j - 1].item] < scores[r.item]) { rows[j] = rows[j - 1]; j--; }
                    rows[j] = r;
                }
                if (go_text.count) for (s32 i = 0; i < row_count; i++) pick_row[i] = i;
            }
        } else if (model_mode) {
            // --- the models (the text filters them), then the name typed when it's none of them ---
            b32 known = 0;
            for (s32 k = 0; k < (s32)ArrayCount(g_Models); k++) {
                known |= Str_EqualNoCase(Str_FromCStr(g_Models[k][0]), filter);
                if (filter.count && FuzzyScore(Str_FromCStr(g_Models[k][0]), filter) < 0) continue;
                pick_row[pickable++] = row_count;
                rows[row_count++] = (PaletteRow){ k, 0, NULL };
            }
            if (filter.count && !known) { pick_row[pickable++] = row_count; rows[row_count++] = (PaletteRow){ (s32)ArrayCount(g_Models), 0, NULL }; }
        } else if (accounts_mode) {
            // --- the accounts (the text filters them), then "add" for a new name typed (letters, digits, - and _) ---
            b32 taken = 0, name_ok = filter.count > 0 && filter.count < (s64)sizeof(app->pending_account);
            for (s64 k = 0; k < filter.count; k++) {
                u8 ch = filter.data[k];
                name_ok &= (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
            }
            for (s32 k = 0; k < app->account_count; k++) {
                taken |= Str_EqualNoCase(app->accounts[k].name, filter);
                if (filter.count && FuzzyScore(app->accounts[k].name, filter) < 0) continue;
                if (app->account_adding && !Str_EqualNoCase(app->accounts[k].name, filter)) continue;   // adding: only a name that's taken
                pick_row[pickable++] = row_count;
                rows[row_count++] = (PaletteRow){ k, 0, NULL };
            }
            if (name_ok && !taken) { pick_row[pickable++] = row_count; rows[row_count++] = (PaletteRow){ app->account_count, 0, NULL }; }
        } else if (settings) {
            const char *group = NULL;
            for (s32 k = 0; k < Setting_Count; k++) {
                b32 match = !filter.count || ContainsNoCase(Str_FromCStr(g_Settings[k].label), filter) ||
                            ContainsNoCase(Str_FromCStr(g_Settings[k].group), filter) || ContainsNoCase(Str_FromCStr(g_Settings[k].note), filter);
                if (!match) continue;
                if (!filter.count && (!group || strcmp(group, g_Settings[k].group) != 0)) { group = g_Settings[k].group; rows[row_count++] = (PaletteRow){ -1 - k, 0, NULL }; }
                pick_row[pickable++] = row_count;
                rows[row_count++] = (PaletteRow){ k, 0, NULL };
            }
        } else {
            // fuzzy: the typed characters in order, best matches first (tabs, then commands); `item` holds the score
            for (PaneNode *n = filter.count ? panes->root : NULL; n; n = Panes_NextNode(n)) {
                for (PaneTab *o = n->first_tab; o && pickable < 12; o = o->next) {
                    const TermTab *st = FindServerTab(app, o->term_id);
                    s32 score = FuzzyScore(PaneTabLabel(app, o), filter);
                    if (score < 0 && st && ContainsNoCase(st->cwd, filter)) score = 0;
                    if (score < 0) continue;
                    pick_row[pickable++] = row_count;
                    rows[row_count++] = (PaletteRow){ score, 0, o };
                }
            }
            s32 first_command = row_count;
            for (u32 k = 1; k < Cmd_Count; k++) {
                s32 score = filter.count ? FuzzyScore(Str_FromCStr(g_Commands[k].label), filter) : 0;
                if (!g_Commands[k].label[0] || score < 0) continue;
                pick_row[pickable++] = row_count;
                rows[row_count++] = (PaletteRow){ score, k, NULL };
            }
            for (s32 i = 1; i < row_count; i++) {
                // highest score first within the tabs and within the commands; ties keep table order
                PaletteRow r = rows[i];
                s32 j = i, floor = i >= first_command ? first_command : 0;
                while (j > floor && rows[j - 1].item < r.item) { rows[j] = rows[j - 1]; j--; }
                rows[j] = r;
            }
        }
        // --- keys: up / down, Enter, Esc; Left / Right step a setting (not the caret) ---
        for (s64 i = 0; i < event_count; i++) {
            const UiKeyEvent *e = &events[i];
            if (!e->down || ui->consumed[i]) continue;
            if (settings && app->setting_editing) {
                if (e->key == Key_Enter || e->key == Key_KeypadEnter) { act = app->setting_editing - 1; step = 2; }
                if (e->key == Key_Escape) { app->setting_editing = 0; ui->consumed[i] = 1; }
                continue;
            }
            u8 kmods = e->modifiers & (Mod_Ctrl | Mod_Alt | Mod_Shift);
            TextEdit *te = &app->palette_edit;
            if (go && ((e->key == Key_Tab && !kmods) || (e->key == Key_Right && !kmods && te->cursor == te->count && te->mark == te->cursor))) {
                // --- Tab (or Right at the end): the selected row into the text, a folder with a separator (its
                //     contents come up) ---
                ui->consumed[i] = 1;
                const GotoRow *g = pickable ? &go_rows[rows[pick_row[Clamp(app->palette_sel, 0, pickable - 1)]].item] : NULL;
                if (g && g->group != Goto_Folder) {
                    TextEdit_Set(te, (g->flags & File_Dir) ? Str_PushFormat(fa, "%.*s\\", STR_ARG(g->path)) : g->path);
                    te->mark = te->cursor = te->count;
                    ui->animating = 1;   // listed and drawn next frame
                }
                continue;
            }
            if (go && e->key == Key_Up && kmods == Mod_Alt) {
                // --- Alt+Up: the typed folder's parent (a drive's: the groups) ---
                ui->consumed[i] = 1;
                s64 root = PathRoot(go_dir), cut = go_dir.count;
                while (cut > root && go_dir.data[cut - 1] != '\\') cut--;
                String parent = go_dir.count > root ? (String){ go_dir.data, Max(cut - 1, root) } : (String){0};
                TextEdit_Set(te, parent.count ? Str_PushFormat(fa, "%.*s\\", STR_ARG(parent)) : (String){0});
                te->mark = te->cursor = te->count;
                ui->animating = 1;
                continue;
            }
            if (e->key == Key_Down) app->palette_sel++;
            if (e->key == Key_Up) app->palette_sel--;
            if (go && (e->key == Key_PageDown || e->key == Key_PageUp)) app->palette_sel += e->key == Key_PageDown ? fit - 1 : 1 - fit;
            if (e->key == Key_Enter || e->key == Key_KeypadEnter) { run = 1; go_select = go && kmods == Mod_Ctrl; }
            if (e->key == Key_Escape) esc = 1;
            if (accounts_mode && !app->account_adding && (e->key == Key_Left || e->key == Key_Right) && pickable && !kmods) {
                account_act = rows[pick_row[Clamp(app->palette_sel, 0, pickable - 1)]].item;
                account_step = e->key == Key_Left ? -1 : 1;
                ui->consumed[i] = 1;
            }
            if (settings && (e->key == Key_Left || e->key == Key_Right) && pickable) {
                act = rows[pick_row[Clamp(app->palette_sel, 0, pickable - 1)]].item;
                step = e->key == Key_Left ? -1 : 1;
                ui->consumed[i] = 1;
            }
        }
        app->palette_sel = Clamp(app->palette_sel, 0, Max(0, pickable - 1));
        s32 sel_row = pickable ? pick_row[app->palette_sel] : 0;
        if (in->input.wheel[1] && RectHas(R4((in->client_w - box_w) / 2, caption_h, (in->client_w + box_w) / 2, in->client_h), mouse_x, mouse_y)) app->palette_top -= in->input.wheel[1] * 3 / 120;
        if (app->palette_sel != app->palette_sel_seen) {
            s32 above = settings && sel_row > 0 && rows[sel_row - 1].item < 0 ? 1 : 0;   // its group's heading comes into view too
            if (sel_row - above < app->palette_top) app->palette_top = sel_row - above;
            if (sel_row >= app->palette_top + fit) app->palette_top = sel_row - fit + 1;
            app->palette_sel_seen = app->palette_sel;
        }
        app->palette_top = Clamp(app->palette_top, 0, Max(0, row_count - fit));
        s32 shown = Min(row_count - app->palette_top, fit);
        Rect box = R4((in->client_w - box_w) / 2, caption_h + R(1), (in->client_w + box_w) / 2, caption_h + R(1) + 2 * pad + R(2.4) + R(0.4) + shown * row_h + (go ? R(2.2) : 0));
        s32 slide = PopupAppear(app, Ui_IdN(Ui_Id("PaletteIn"), app->palette_mode));
        box.y0 += slide;
        box.y1 += slide;
        Ui_Hit(ui, Ui_Id("Palette"), box);
        PopupBox(app, box, radius_large);
        Rect body = RectShrink(box, pad);
        Rect edit = CutTop(&body, R(2.4));
        CutTop(&body, R(0.4));
        if (go) {
            // the keys, along the bottom
            Rect foot = CutBottom(&body, R(2.2));
            Draw_TextIn(dl, small, R4(foot.x0 + R(0.7), foot.y0 + R(0.4), foot.x1 - R(0.7), foot.y1), c->dim,
                        S("Enter open  \xC2\xB7  Tab expand  \xC2\xB7  Alt+\xE2\x86\x91 up  \xC2\xB7  Ctrl+Enter select in the folder  \xC2\xB7  %TEMP% and ~ work  \xC2\xB7  Esc close"), 0);
        }
        Draw_RectEx(dl, edit, c->bg, c->accent, 1, radius_small, NULL, 0, 0, 0);
        if (go) Draw_Icon(dl, icons_small, R4(edit.x0 + R(0.4), edit.y0, edit.x0 + R(2.2), edit.y1), c->dim, Icon_Folder);
        Ui_TextEdit(ui, Ui_Id("PaletteFilter"), &app->palette_edit, R4(edit.x0 + (go ? R(2.4) : R(0.6)), edit.y0, edit.x1 - R(0.6), edit.y1), font, !app->setting_editing,
                    settings ? S("Type a setting") : go ? S("Go to a folder, or search recents, bookmarks, drives and places") :
                    accounts_mode && app->account_adding ? S("Name the new account (e.g. work, personal)") :
                    model_mode ? S("Pick a model for this claude (/model), or type one") :
                    accounts_mode ? S("Enter: a claude tab as the account  \xC2\xB7  arrows: its theme  \xC2\xB7  a new name: add one") : S("Type a command"), c->text, c->dim, c->selection);
        Named(app, go ? S("goto edit") : S("palette edit"), edit);
        Draw_PushClip(dl, body);
        for (s32 j = app->palette_top; j < app->palette_top + shown; j++) {
            Rect row = CutTop(&body, row_h);
            const PaletteRow *pr = &rows[j];
            b32 selected = j == sel_row;
            if (settings && pr->item < 0) {
                Draw_TextIn(dl, small, R4(row.x0 + R(0.7), row.y0 + R(0.5), row.x1, row.y1), c->dim, Str_FromCStr(g_Settings[-1 - pr->item].group), 0);
                continue;
            }
            if (go && pr->item < 0) {
                // --- a group's heading: a click folds it; Recents can show them all and be cleared, Bookmarks cleared ---
                s32 g = -1 - pr->item;
                b32 closed = (app->goto_closed >> g) & 1;
                Rect hr = row;
                CutLeft(&hr, R(0.5));
                CutRight(&hr, R(0.5));
                b32 actions = (g == Goto_Recents || g == Goto_Bookmarks) && !closed;
                for (s32 a = actions ? (g == Goto_Recents && app->recent_count > 10 ? 1 : 0) : -1; a >= 0; a--) {
                    // right to left: Clear, then Show all / Show fewer
                    String label = a == 0 ? S("Clear") : app->goto_all_recents ? S("Show fewer") : S("Show all");
                    Rect ar = RectCenter(CutRight(&hr, Font_Measure(small, label) + R(1.2)), Font_Measure(small, label) + R(1.2), R(1.8));
                    u64 aid = Ui_IdN(Ui_IdN(Ui_Id("GotoGroupAct"), (u64)g), (u64)a);
                    UiSignal as = Ui_Hit(ui, aid, ar);
                    Draw_Rect(dl, ar, HoverFill(app, aid, as, c->hover), radius_small);
                    Draw_TextIn(dl, small, ar, as.hovered ? c->text : c->dim, label, 0.5f);
                    Named(app, Str_PushFormat(fa, "goto %s %.*s", group_names[g], STR_ARG(label)), ar);
                    if (as.clicked && a == 1) app->goto_all_recents = !app->goto_all_recents;
                    if (as.clicked && a == 0 && g == Goto_Recents) { app->recent_count = 0; app->paths_dirty = 1; }
                    if (as.clicked && a == 0 && g == Goto_Bookmarks) { app->bookmark_count = 0; app->paths_dirty = 1; }
                }
                u64 hid = Ui_IdN(Ui_Id("GotoGroup"), (u64)g);
                UiSignal hs = Ui_Hit(ui, hid, hr);
                Draw_Icon(dl, icons_small, CutLeft(&hr, R(1.4)), c->dim, closed ? Icon_ChevronRight : Icon_Down);
                Draw_Icon(dl, icons_small, CutLeft(&hr, R(1.8)), c->dim, group_icons[g]);
                Draw_TextIn(dl, small, hr, hs.hovered ? c->text : c->dim, Str_FromCStr(group_names[g]), 0);
                Named(app, Str_PushFormat(fa, "goto group %s", group_names[g]), hr);
                if (hs.clicked) app->goto_closed ^= (u8)(1 << g);
                continue;
            }
            s32 pick_index = 0;
            while (pick_index < pickable && pick_row[pick_index] != j) pick_index++;
            u64 row_id = settings ? Ui_IdN(Ui_Id("SettingRow"), (u64)pr->item) : go ? Ui_IdN(Ui_Id("GotoRow"), Hash_Bytes(go_rows[pr->item].path.data, (u64)go_rows[pr->item].path.count, (u64)go_rows[pr->item].group)) :
                         pr->tab ? Ui_IdN(Ui_Id("PaletteTab"), pr->tab->id) :
                         accounts_mode || model_mode ? Ui_IdN(Ui_Id("PaletteItem"), (u64)pr->item + ((u64)app->palette_mode << 32)) : Ui_IdN(Ui_Id("PaletteRow"), pr->cmd);
            UiSignal rs = Ui_Hit(ui, row_id, row);
            if (selected) Draw_Rect(dl, row, c->selection, radius_small);
            else Draw_Rect(dl, row, HoverFill(app, row_id, rs, c->hover), radius_small);
            Rect in_row = row;
            CutLeft(&in_row, R(0.7));
            CutRight(&in_row, R(0.7));
            if (go) {
                // --- a go-to row: icon, name, its path (or a drive's space, with a bar), a remove button on recents and
                //     bookmarks ---
                const GotoRow *g = &go_rows[pr->item];
                Draw_Icon(dl, icons_small, CutLeft(&in_row, R(1.9)), (g->flags & File_Dir) ? c->accent : c->dim, g->icon);
                CutLeft(&in_row, R(0.3));
                if ((g->group == Goto_Recents || g->group == Goto_Bookmarks) && (rs.hovered || selected)) {
                    Rect xr = RectCenter(CutRight(&in_row, R(1.9)), R(1.8), R(1.8));
                    u64 xid = Ui_IdN(Ui_IdN(Ui_Id("GotoRemove"), (u64)g->group), Hash_Bytes(g->path.data, (u64)g->path.count, 7));
                    UiSignal xs = IconButton(app, xid, xr, Icon_Close, icons_small, c->dim, c->hover, radius_small, Str_PushFormat(fa, "goto remove %.*s", STR_ARG(g->label)));
                    if (xs.clicked) {
                        // out of its list (by path)
                        u8 (*list)[520] = g->group == Goto_Recents ? app->recent : app->bookmark;
                        s32 *lens = g->group == Goto_Recents ? app->recent_len : app->bookmark_len;
                        s32 *count = g->group == Goto_Recents ? &app->recent_count : &app->bookmark_count;
                        for (s32 k = 0; k < *count; k++) {
                            if (!Str_EqualNoCase((String){ list[k], lens[k] }, g->path)) continue;
                            memmove(list[k], list[k + 1], sizeof(list[0]) * (size_t)(*count - k - 1));
                            memmove(lens + k, lens + k + 1, sizeof(lens[0]) * (size_t)(*count - k - 1));
                            (*count)--;
                            break;
                        }
                        app->paths_dirty = 1;
                        continue;
                    }
                }
                if (g->flags & File_Drive && g->size) {
                    // a drive: how full (a bar) and what's free
                    String note = Str_PushFormat(fa, "%.*s free of %.*s", STR_ARG(SizeText(fa, g->free)), STR_ARG(SizeText(fa, g->size)));
                    Rect bar = RectCenter(CutRight(&in_row, R(7)), R(7), R(0.6));
                    CutRight(&in_row, R(0.6));
                    f32 used = 1.0f - (f32)((f64)g->free / (f64)g->size);
                    Draw_Rect(dl, bar, c->hover, (f32)R(0.3));
                    Draw_Rect(dl, R4(bar.x0, bar.y0, bar.x0 + (s32)(RectW(bar) * used), bar.y1), used > 0.9f ? c->danger : c->accent, (f32)R(0.3));
                    Draw_TextIn(dl, small, CutRight(&in_row, Min(Font_Measure(small, note), R(16))), c->dim, note, 1);
                } else if (!(g->flags & File_Dir) && g->group == Goto_Entry) {
                    Draw_TextIn(dl, small, CutRight(&in_row, R(6)), c->dim, SizeText(fa, g->size), 1);
                }
                s32 label_w = Min(Font_Measure(font, g->label), RectW(in_row));
                Draw_TextIn(dl, font, CutLeft(&in_row, label_w), c->text, g->label, 0);
                CutLeft(&in_row, R(0.8));
                if (g->note.count) Draw_TextIn(dl, small, in_row, c->dim, g->note, 0);
                Named(app, Str_PushFormat(fa, "goto %.*s", STR_ARG(g->label)), row);
                if (rs.clicked) { app->palette_sel = pick_index; run = 1; }
                continue;
            }
            if (model_mode) {
                b32 typed = pr->item >= (s32)ArrayCount(g_Models);
                String label = typed ? filter : Str_FromCStr(g_Models[pr->item][0]);
                Draw_Icon(dl, icons_small, CutLeft(&in_row, R(1.8)), c->dim, Icon_Robot);
                Draw_TextIn(dl, font, CutLeft(&in_row, Min(Font_Measure(font, label), RectW(in_row))), c->text, label, 0);
                CutLeft(&in_row, R(0.8));
                Draw_TextIn(dl, small, in_row, c->dim, typed ? S("a model name of your own") : Str_FromCStr(g_Models[pr->item][1]), 0);
                Named(app, Str_PushFormat(fa, "palette model %.*s", STR_ARG(label)), row);
                if (rs.clicked) { app->palette_sel = pick_index; run = 1; }
                continue;
            }
            if (accounts_mode) {
                b32 add = pr->item >= app->account_count;
                String label = add ? Str_PushFormat(fa, "Add the account \"%.*s\"", STR_ARG(filter)) : app->accounts[pr->item].name;
                String note = add ? S("its own config folder, your settings copied; log in in the tab that opens") :
                              pr->item == 0 ? S("Claude Code's own (~\\.claude)") : app->accounts[pr->item].dir;
                if (!add) {
                    // what it is: the login of a claude of it that's open, and the folders kept for it
                    for (s64 i = 0; i < app->tab_count; i++) {
                        if (!app->tabs[i].account.count || AccountIndex(app, app->tabs[i].config_dir) != pr->item) continue;
                        String e = AccountPart(app->tabs[i].account, 2), o = AccountPart(app->tabs[i].account, 1);
                        note = e.count ? Str_PushFormat(fa, "%.*s  \xC2\xB7  %.*s", STR_ARG(e), STR_ARG(o)) : AccountPart(app->tabs[i].account, 0);
                        break;
                    }
                    for (s32 r = 0; r < app->folder_rule_count; r++) {
                        if (Str_EqualNoCase(app->folder_rules[r].account, app->accounts[pr->item].name)) note = Str_PushFormat(fa, "%.*s  \xC2\xB7  %.*s", STR_ARG(note), STR_ARG(app->folder_rules[r].folder));
                    }
                }
                if (!add && !app->account_adding) {
                    // its theme: arrows either side, like a setting's choice
                    s32 th = app->accounts[pr->item].theme;
                    Rect more = RectCenter(CutRight(&in_row, R(2)), R(2), R(1.8));
                    Rect val = CutRight(&in_row, R(9));
                    Rect less = RectCenter(CutRight(&in_row, R(2)), R(2), R(1.8));
                    String an = app->accounts[pr->item].name;
                    UiSignal ls = IconButton(app, Ui_IdN(Ui_Id("AccountThemeLess"), (u64)pr->item), less, Icon_ChevronLeft, icons_small, c->dim, c->hover, radius_small,
                                             Str_PushFormat(fa, "account %.*s theme less", STR_ARG(an)));
                    UiSignal ms = IconButton(app, Ui_IdN(Ui_Id("AccountThemeMore"), (u64)pr->item), more, Icon_ChevronRight, icons_small, c->dim, c->hover, radius_small,
                                             Str_PushFormat(fa, "account %.*s theme more", STR_ARG(an)));
                    if (th >= 0) Draw_Rect(dl, RectCenter(CutLeft(&val, R(1.2)), R(0.9), R(0.9)), 0xFF000000u | g_TermThemes[th].accent, (f32)R(0.45));
                    Draw_TextIn(dl, small, val, th >= 0 ? c->text : c->dim, th >= 0 ? Str_FromCStr(g_TermThemes[th].name) : S("window's theme"), 0.5f);
                    if (ls.clicked) { account_act = pr->item; account_step = -1; app->palette_sel = pick_index; }
                    if (ms.clicked) { account_act = pr->item; account_step = 1; app->palette_sel = pick_index; }
                }
                Draw_Icon(dl, icons_small, CutLeft(&in_row, R(1.8)), c->dim, add ? Icon_Add : Icon_Robot);
                s32 label_w = Min(Font_Measure(font, label), RectW(in_row));
                Draw_TextIn(dl, font, CutLeft(&in_row, label_w), c->text, label, 0);
                CutLeft(&in_row, R(0.8));
                Draw_TextIn(dl, small, in_row, c->dim, note, 0);
                Named(app, add ? Str_PushFormat(fa, "palette add account %.*s", STR_ARG(filter)) : Str_PushFormat(fa, "palette account %.*s", STR_ARG(label)), row);
                if (rs.clicked) { app->palette_sel = pick_index; run = 1; }
                continue;
            }
            if (!settings) {
                String label = pr->tab ? PaneTabLabel(app, pr->tab) : Str_FromCStr(g_Commands[pr->cmd].label);
                if (pr->tab) {
                    const TermTab *st = FindServerTab(app, pr->tab->term_id);
                    Draw_Icon(dl, icons_small, CutLeft(&in_row, R(1.8)), c->dim, st && (st->flags & TermTab_Agent) ? Icon_Robot : Icon_Prompt);
                }
                String keys = pr->tab ? (String){0} : HotkeyName(fa, app->bind_key[pr->cmd], app->bind_mods[pr->cmd]);
                if (keys.count) Draw_TextIn(dl, small, CutRight(&in_row, Font_Measure(small, keys)), c->dim, keys, 1);
                Draw_TextIn(dl, font, in_row, c->text, label, 0);
                Named(app, Str_PushFormat(fa, pr->tab ? "palette tab %.*s" : "palette %.*s", STR_ARG(label)), row);
                if (rs.clicked) { app->palette_sel = pick_index; run = 1; }
                continue;
            }
            // --- a setting: its label and note, then its value: a choice between arrows, a check box, a text (an edit
            //     while editing), or an arrow out ---
            s32 k = pr->item;
            const SettingDef *sd = &g_Settings[k];
            b32 editing = app->setting_editing == k + 1;
            String value = {0};
            if (k == Setting_Theme) value = Str_FromCStr(g_TermThemes[app->term_theme].name);
            if (k == Setting_Font) value = app->term_font;
            if (k == Setting_Cursor) value = app->cursor == 1 ? S("Bar") : app->cursor == 2 ? S("Underline") : S("Block");
            if (k == Setting_Backdrop) value = app->backdrop == 1 ? S("Acrylic") : app->backdrop == 2 ? S("Mica") : S("The desktop");
            if (k == Setting_FontSize) value = Str_PushFormat(fa, "%g pt", (double)app->term_font_pt);
            if (k == Setting_Opacity) value = Str_PushFormat(fa, "%d%%", (int)(app->opacity * 100 + 0.5f));
            if (k == Setting_BackgroundStrength) value = app->background_strength ? Str_PushFormat(fa, "%d%%", app->background_strength) : S("Off");
            if (k == Setting_Agent) value = app->agent_command.count ? app->agent_command : S("claude");
            if (k == Setting_Agents) value = app->agents.count ? app->agents : S("the usual ones");
            b32 on = k == Setting_HiddenFiles ? !app->hide_hidden : k == Setting_FolderSizes ? !app->folder_sizes_off : k == Setting_CursorBlink ? app->cursor_blink : k == Setting_Ligatures ? app->ligatures :
                     k == Setting_CopyOnSelect ? !app->copy_on_select_off : k == Setting_DimUnfocused ? !app->dim_unfocused_off : k == Setting_ExplorerMenu ? app->explorer_menu :
                     k == Setting_StartWithWindows ? app->start_with_windows : k == Setting_Notifications ? !app->notifications_off :
                     k == Setting_VsEnv ? !app->vs_env_off : k == Setting_GoodProgrammer ? app->good_programmer : 0;
            b32 clicked_value = 0;
            if (sd->kind == SettingKind_Choice) {
                s32 wide = k == Setting_Theme || k == Setting_Font || k == Setting_Backdrop ? R(11) : R(5);
                Rect more = RectCenter(CutRight(&in_row, R(2)), R(2), R(1.8));
                Rect val = CutRight(&in_row, wide);
                Rect less = RectCenter(CutRight(&in_row, R(2)), R(2), R(1.8));
                UiSignal ls = IconButton(app, Ui_IdN(Ui_Id("SettingLess"), (u64)k), less, Icon_ChevronLeft, icons_small, c->dim, c->hover, radius_small,
                                         Str_PushFormat(fa, "setting %s less", sd->label));
                UiSignal ms = IconButton(app, Ui_IdN(Ui_Id("SettingMore"), (u64)k), more, Icon_ChevronRight, icons_small, c->dim, c->hover, radius_small,
                                         Str_PushFormat(fa, "setting %s more", sd->label));
                Draw_TextIn(dl, font, val, c->text, value, 0.5f);
                if (ls.clicked) { act = k; step = -1; app->palette_sel = pick_index; clicked_value = 1; }
                else if (ms.clicked) { act = k; step = 1; app->palette_sel = pick_index; clicked_value = 1; }
            } else if (sd->kind == SettingKind_Bool) {
                CutRight(&in_row, R(0.3));
                Rect box2 = RectCenter(CutRight(&in_row, R(1.6)), R(1.6), R(1.6));
                Draw_RectEx(dl, box2, on ? c->accent : c->bg, on ? c->accent : c->dim, 1, radius_small, NULL, 0, 0, 0);
                if (on) Draw_Icon(dl, icons_small, box2, c->on_accent, Icon_CheckMark);
            } else if (sd->kind == SettingKind_Text && editing) {
                Rect er = RectCenter(CutRight(&in_row, R(18)), R(18), R(1.9));
                Draw_RectEx(dl, er, c->bg, c->accent, 1, radius_small, NULL, 0, 0, 0);
                Ui_TextEdit(ui, Ui_Id("SettingEdit"), &app->setting_edit, R4(er.x0 + R(0.4), er.y0, er.x1 - R(0.4), er.y1), font, 1,
                            k == Setting_Agent ? S("claude") : S("codex gemini aider ..."), c->text, c->dim, c->selection);
            } else if (sd->kind == SettingKind_Text) {
                CutRight(&in_row, R(0.3));
                Draw_TextIn(dl, font, CutRight(&in_row, Min(Font_Measure(font, value), R(18))), c->dim, value, 1);
            } else {
                Draw_Icon(dl, icons_small, CutRight(&in_row, R(2)), c->dim, k == Setting_Reset ? Icon_ChevronRight : Icon_OpenInWindow);
                if (k == Setting_Background) {
                    // the image's file name (its folder dropped)
                    String file = in->background_path;
                    for (s64 i = file.count; i > 0; i--) if (file.data[i - 1] == '\\') { file = (String){ file.data + i, file.count - i }; break; }
                    CutRight(&in_row, R(0.4));
                    Draw_TextIn(dl, font, CutRight(&in_row, Min(Font_Measure(font, file.count ? file : S("none")), R(18))), c->dim, file.count ? file : S("none"), 1);
                }
            }
            CutRight(&in_row, R(0.6));
            Rect label_r = CutLeft(&in_row, Font_Measure(font, Str_FromCStr(sd->label)));
            Draw_TextIn(dl, font, label_r, c->text, Str_FromCStr(sd->label), 0);
            CutLeft(&in_row, R(0.8));
            Draw_TextIn(dl, small, in_row, c->dim, Str_FromCStr(sd->note), 0);
            Named(app, Str_PushFormat(fa, "setting %s", sd->label), row);
            if (act < 0 && !editing && !clicked_value && rs.clicked) { act = k; step = 0; app->palette_sel = pick_index; }
        }
        Draw_PopClip(dl);
        if (go && run) {
            // --- go: a folder opens in the tab's browser (a shell's flips to it), a file opens with its program
            //     (Ctrl+Enter: selected in its folder instead); nothing picked, a typed path goes there ---
            PaneTab *gt = FindPaneTab(panes, app->goto_tab);
            if (!gt) gt = panes->selected;
            const GotoRow *g = pickable ? &go_rows[rows[pick_row[Clamp(app->palette_sel, 0, pickable - 1)]].item] : NULL;
            String target = g ? g->path : go_path ? BrowsePath(go_text) : (String){0};
            b32 file = g && !(g->flags & File_Dir);
            if (gt && gt->term_id && (g || go_path)) {
                if (!gt->browsing) OpenBrowser(app, gt);
                Panes_Select(panes, gt);
                s64 cut = target.count;
                while (cut > 0 && target.data[cut - 1] != '\\') cut--;
                if (file && !go_select) OpenInTab(app, &f, gt, Str_PushCopy(fa, target));
                else {
                    f.nav_tab = gt;
                    f.nav_path = Str_PushCopy(fa, file || go_select ? (String){ target.data, Max(cut - 1, 0) } : target);
                    f.nav_select = file || go_select ? Str_PushCopy(fa, (String){ target.data + cut, target.count - cut }) : (String){0};
                }
                app->palette_open = 0;
            }
            run = 0;
        }
        // --- the accounts palette's arrows: the account's theme steps (the window's, then each), into accounts.txt ---
        if (account_act >= 0 && account_act < app->account_count) {
            s32 th = app->accounts[account_act].theme + 1 + account_step;   // -1..Count-1 as 0..Count
            th = ((th % (TermTheme_Count + 1)) + TermTheme_Count + 1) % (TermTheme_Count + 1) - 1;
            app->accounts[account_act].theme = th;
            out->account_theme_name = app->accounts[account_act].name;
            out->account_theme = th >= 0 ? Str_FromCStr(g_TermThemes[th].name) : (String){0};
        }
        if (run && pickable && act < 0) {
            const PaletteRow *pr = &rows[pick_row[Clamp(app->palette_sel, 0, pickable - 1)]];
            if (settings) { act = pr->item; step = 0; }
            else if (model_mode) {
                // typed into its claude's prompt: /model and the name, Enter (not while a turn runs)
                app->palette_open = 0;
                String name = pr->item < (s32)ArrayCount(g_Models) ? Str_FromCStr(g_Models[pr->item][0]) : filter;
                const TermTab *mt = FindServerTab(app, app->model_term);
                if (mt && !(mt->flags & (TermTab_Busy | TermTab_AccountHeld))) {
                    TermSend(&f, app->model_term, Str_PushFormat(fa, "/model %.*s\r", STR_ARG(name)));
                    Logf("app", "model: %.*s", STR_ARG(name));
                }
            }
            else if (accounts_mode) {
                // a claude tab under the account; or a new account (made by the platform), whose tab opens once it's listed
                app->palette_open = 0;
                if (pr->item < app->account_count) RequestClaudeTab(app, &f, app->accounts[pr->item].dir);
                else {
                    out->add_account = Str_PushCopy(fa, filter);
                    app->pending_account_len = (s32)CopyInto(app->pending_account, sizeof(app->pending_account), filter).count;
                    Logf("app", "add account %.*s", STR_ARG(filter));
                }
            } else {
                app->palette_open = 0;
                if (pr->tab) Panes_Select(panes, pr->tab);   // shown in its pane and focused
                else RunCommand(app, &f, pr->cmd);
            }
        }

        // --- a setting changed: the new value at once, and into settings.txt ---
        if (settings && act >= 0) {
            String key = {0}, val = {0};
            u8 kind = g_Settings[act].kind;
            if (step != 2) app->setting_editing = 0;
            if (act == Setting_Theme) {
                app->term_theme = (app->term_theme + (step < 0 ? TermTheme_Count - 1 : 1)) % TermTheme_Count;
                key = S("theme");
                val = Str_FromCStr(g_TermThemes[app->term_theme].name);
            } else if (act == Setting_Font && in->mono_font_count) {
                s64 count = in->mono_font_count, at = -1;
                for (s64 j = 0; j < count; j++) if (Str_EqualNoCase(in->mono_fonts[j], app->term_font)) at = j;
                at = at < 0 ? (step < 0 ? count - 1 : 0) : (at + (step < 0 ? count - 1 : 1)) % count;
                app->term_font = CopyInto(app->term_font_buf, sizeof(app->term_font_buf), in->mono_fonts[at]);
                key = S("font");
                val = app->term_font;
                out->term_font = app->term_font;
            } else if (act == Setting_Ligatures) {
                app->ligatures = !app->ligatures;
                key = S("ligatures"); val = app->ligatures ? S("on") : S("off");
            } else if (act == Setting_StartWithWindows) {
                app->start_with_windows = !app->start_with_windows;
                out->start_with_windows = app->start_with_windows ? 1 : 2;
            } else if (act == Setting_ExplorerMenu) {
                app->explorer_menu = !app->explorer_menu;
                out->explorer_menu = app->explorer_menu ? 1 : 2;
            } else if (act == Setting_GoodProgrammer) {
                app->good_programmer = !app->good_programmer;
                out->good_programmer = app->good_programmer ? 1 : 2;
            } else if (act == Setting_Reset && step == 0) {
                app->reset_confirm = 1;
                app->palette_open = 0;
                reset_asked = 1;
            } else if (act == Setting_Cursor) {
                app->cursor = (u8)((app->cursor + (step < 0 ? 2 : 1)) % 3);
                key = S("cursor"); val = app->cursor == 1 ? S("bar") : app->cursor == 2 ? S("underline") : S("block");
            } else if (act == Setting_CursorBlink) {
                app->cursor_blink = !app->cursor_blink;
                key = S("cursor_blink"); val = app->cursor_blink ? S("on") : S("off");
            } else if (act == Setting_CopyOnSelect) {
                app->copy_on_select_off = !app->copy_on_select_off;
                key = S("copy_on_select"); val = app->copy_on_select_off ? S("off") : S("on");
            } else if (act == Setting_DimUnfocused) {
                app->dim_unfocused_off = !app->dim_unfocused_off;
                key = S("dim_unfocused"); val = app->dim_unfocused_off ? S("off") : S("on");
            } else if (act == Setting_Backdrop) {
                app->backdrop = (u8)((app->backdrop + (step < 0 ? 2 : 1)) % 3);
                key = S("backdrop"); val = app->backdrop == 1 ? S("acrylic") : app->backdrop == 2 ? S("mica") : S("none");
                if (app->backdrop && app->opacity > 0.95f) {
                    // seen only through see-through parts: some see-through to start with
                    app->opacity = 0.8f;
                    SetSetting(out, S("opacity"), S("80"));
                }
            } else if (act == Setting_FontSize) {
                app->term_font_pt = Clamp(app->term_font_pt + (step < 0 ? -1 : 1), 6, 72);
                key = S("font_size"); val = Str_PushFormat(fa, "%g", (double)app->term_font_pt);
            } else if (act == Setting_Opacity) {
                s32 pct = (s32)(app->opacity * 100 + 0.5f) / 5 * 5 + (step < 0 ? -5 : 5);
                if (step == 0 && pct > 100) pct = 20;   // Enter goes round
                pct = Clamp(pct, 20, 100);
                app->opacity = (f32)pct / 100.0f;
                key = S("opacity"); val = Str_PushFormat(fa, "%d", pct);
            } else if (kind == SettingKind_Bool) {
                b32 *off = act == Setting_HiddenFiles ? &app->hide_hidden : act == Setting_FolderSizes ? &app->folder_sizes_off : act == Setting_Notifications ? &app->notifications_off : &app->vs_env_off;
                *off = !*off;
                key = Str_FromCStr(act == Setting_HiddenFiles ? "hidden_files" : act == Setting_FolderSizes ? "folder_sizes" : act == Setting_Notifications ? "notifications" : "vs_env");
                val = *off ? S("off") : S("on");
            } else if (kind == SettingKind_Text && step == 0) {
                TextEdit_Set(&app->setting_edit, act == Setting_Agent ? app->agent_command : app->agents);
                app->setting_editing = act + 1;
            } else if (kind == SettingKind_Text && step == 2) {
                String edited = TextEdit_String(&app->setting_edit);
                if (act == Setting_Agent) app->agent_command = CopyInto(app->agent_buf, sizeof(app->agent_buf), edited);
                else app->agents = CopyInto(app->agents_buf, sizeof(app->agents_buf), edited);
                app->setting_editing = 0;
                key = Str_FromCStr(act == Setting_Agent ? "agent" : "agents");
                val = act == Setting_Agent ? app->agent_command : app->agents;
            } else if (act == Setting_Background && step == 0) {
                out->choose_background = 1;
            } else if (act == Setting_BackgroundStrength) {
                app->background_strength = app->background_strength + (step < 0 ? -5 : 5);
                if (step == 0 && app->background_strength > 100) app->background_strength = 0;   // Enter goes round
                app->background_strength = Clamp(app->background_strength, 0, 100);
                key = S("background_strength"); val = Str_PushFormat(fa, "%d", app->background_strength);
            } else if (kind == SettingKind_Action && step == 0) {
                out->open_settings = 2;   // the settings folder (settings.txt and the shell setup are in it)
            }
            if (key.count) SetSetting(out, key, val);
        }
        if (esc || outside) app->palette_open = 0;
        dl->fade = 0;
    }

    // --- the confirm popups: a paste of several lines, closing a tab where something runs ---
    if (app->paste_confirm && !app->confirm_tab) {
        s32 lines = 1;
        for (s64 i = 0; i + 1 < app->paste_text.count; i++) lines += app->paste_text.data[i] == '\n';
        s64 first = 0;
        while (first < app->paste_text.count && app->paste_text.data[first] != '\n' && app->paste_text.data[first] != '\r') first++;
        String text = Str_PushFormat(fa, "%.*s%s", (int)Min(first, 60), app->paste_text.data, first > 60 ? "..." : "");
        s32 answer = ConfirmBox(app, Str_PushFormat(fa, "Paste %d lines?", lines), text, S("Paste"), S("paste anyway"), 0);
        if (answer > 0) TermSend(&f, app->paste_confirm, PasteBytes(fa, app->paste_text, 0));
        if (answer) app->paste_confirm = 0;
    }
    if (app->reset_confirm && !reset_asked && !app->paste_confirm && !app->confirm_tab) {
        s32 answer = ConfirmBox(app, S("Reset every setting?"), S("The old ones are kept in settings-old.txt."), S("Reset"), S("reset settings"), 1);
        if (answer > 0) out->reset_settings = 1;
        if (answer) app->reset_confirm = 0;
    }
    if (app->confirm_tab) {
        PaneTab *ct = FindPaneTab(panes, app->confirm_tab);
        if (!ct) app->confirm_tab = 0;
        else {
            const TermTab *st = FindServerTab(app, ct->term_id);
            String what = st && (st->flags & TermTab_Busy) ? Str_PushFormat(fa, "%.*s is in the middle of a turn.", STR_ARG(st->agent)) : S("A program is running in it.");
            s32 answer = ConfirmBox(app, S("Close this tab?"), what, S("Close"), S("close anyway"), 1);
            if (answer > 0) { app->confirm_tab = 0; CloseTab(app, &f, ct, 1); }
            else if (answer < 0) app->confirm_tab = 0;
        }
    }

    // --- the debug overlay: frame times (CPU, GPU), IPC, glyphs ---
    if (app->debug_overlay) {
        String lines[6] = {
            Str_PushFormat(fa, "frame work %.2f ms avg, %.2f max", (double)in->debug.work_ms_avg, (double)in->debug.work_ms_max),
            Str_PushFormat(fa, "gpu %.3f ms, cells %.3f ms", (double)in->debug.gpu_ms, (double)in->debug.gpu_cells_ms),
            Str_PushFormat(fa, "frames drawn %llu, skipped %llu", (unsigned long long)in->debug.frames_drawn, (unsigned long long)in->debug.frames_skipped),
            Str_PushFormat(fa, "ipc in %.1f KB, out %.1f KB", (double)in->debug.ipc_in / 1024.0, (double)in->debug.ipc_out / 1024.0),
            Str_PushFormat(fa, "glyphs %d, atlas %d%%, %d quads", in->debug.glyphs, in->debug.atlas_percent, (int)dl->quads.count),
            Str_PushFormat(fa, "%lld tabs, %d panes, %dx%d px cells", (long long)in->tab_count, app->pane_rect_count, in->cell_w, in->cell_h),
        };
        s32 lh = R(1.7);
        Rect box = R4(in->client_w - R(1) - R(24), in->client_h - R(1) - 6 * lh - R(1.2), in->client_w - R(1), in->client_h - R(1));
        PopupBox(app, box, radius_medium);
        Rect inner = RectShrink(box, R(0.6));
        for (s32 i = 0; i < 6; i++) Draw_TextIn(dl, small, CutTop(&inner, lh), c->text, lines[i], 0);
    }

    Ui_EndFrame(ui);
    if (ui->copied.count) { out->clipboard_set = 1; out->clipboard_text = ui->copied; }

    // --- a browser move asked this frame (its rows were drawn from the listing it releases) ---
    if (f.nav_tab && f.nav_tab->browser) {
        Browser *b = f.nav_tab->browser;
        if (f.nav_up) BrowseUp(app, f.nav_tab);
        else if (f.nav_history) BrowseHistory(app, f.nav_tab, f.nav_history);
        else if (f.nav_open >= 0 && f.nav_open < b->count) {
            FileEntry e = b->entries[f.nav_open];
            e.name = Str_PushCopy(fa, e.name);
            BrowserOpen(app, &f, f.nav_tab, &e);
        }
        else Browse(app, f.nav_tab, f.nav_path, f.nav_select);
    }
    // --- the picture views: their folders listed once; the pictures shown, then their neighbours, decoded ---
    for (PaneNode *n = panes->root; n; n = Panes_NextNode(n)) {
        for (PaneTab *t = n->first_tab; t; t = t->next) {
            Viewer *v = t->viewer;
            if (!v || !t->viewing) continue;
            if (!v->listed && !v->request) {
                v->request = ++app->next_file_request;
                FileRequest *r = Array_Push(fa, f.file_requests);
                r->request = v->request;
                r->path = ShellPath(fa, v->folder);
            }
            if (t->hidden || !v->name.count) continue;
            s64 at = 0;
            while (at < v->count && !Str_EqualNoCase(v->names[at], v->name)) at++;
            for (s32 k = 0; k < 3 && out->image_want_count < (s32)ArrayCount(out->image_wants); k++) {
                if (k > 0 && (!v->count || at >= v->count)) break;
                String name = k == 0 ? v->name : v->names[((at + (k == 1 ? 1 : -1)) % v->count + v->count) % v->count];
                out->image_wants[out->image_want_count++] = Str_PushFormat(fa, "%.*s\\%.*s", STR_ARG(v->folder), STR_ARG(name));
            }
        }
    }
    // --- shown browsers' folders: listed when new, when they changed, or when shown again after a while hidden
    //     (changes went by unwatched); watched while shown ---
    for (PaneNode *n = panes->root; n; n = Panes_NextNode(n)) {
        for (PaneTab *t = n->first_tab; t; t = t->next) {
            Browser *b = t->browser;
            if (!b || !t->browsing || t->hidden || (app->zoom_node && n->id != app->zoom_node)) continue;
            b32 was_hidden = b->shown_frame && b->shown_frame + 1 < ui->frame;
            if ((!b->listed && !b->request) || b->stale || was_hidden) {
                b->request = ++app->next_file_request;
                b->stale = 0;
                FileRequest *r = Array_Push(fa, f.file_requests);
                r->request = b->request;
                r->path = ShellPath(fa, b->path);
            }
            b->shown_frame = ui->frame;
            if (b->path.count) { FileWatch *w = Array_Push(fa, f.file_watches); w->path = ShellPath(fa, b->path); w->sizes = !app->folder_sizes_off; }
            FileEntry *cur = BrowserEntry(b, b->cursor);
            if (cur) b->cursor_name_len = (s32)CopyInto(b->cursor_name, sizeof(b->cursor_name), cur->name).count;
        }
    }

    // --- the watch list goes out when it changed and the layout has been still for a moment (sizes), at once when
    //     only what's shown changed ---
    {
        b32 differs = watches.count != app->sent_watch_count;
        b32 sizes_differ = 0;
        for (s64 i = 0; i < watches.count && !differs; i++) {
            const TermWatch *a = &watches.data[i], *b = &app->sent_watch[i];
            if (a->tab_id != b->tab_id || a->shown != b->shown) differs = 1;
            if (a->cols != b->cols || a->rows != b->rows) sizes_differ = 1;
        }
        b32 still = now - app->layout_changed_us > 80000 && !(ui->active && ui->dragging);
        if (differs || (sizes_differ && (still || !app->watch_ever_sent))) {
            out->term_watch_changed = 1;
            app->sent_watch_count = Min(watches.count, (s64)ArrayCount(app->sent_watch));
            if (app->sent_watch_count) memcpy(app->sent_watch, watches.data, sizeof(TermWatch) * (size_t)app->sent_watch_count);
            app->watch_ever_sent = 1;
        } else if (sizes_differ) {
            out->wake_at_us = now + 90000;
        }
        out->term_watch_count = app->sent_watch_count;
        out->term_watches = app->sent_watch;
    }

    // --- the layout: saved at most every second, only when it changed ---
    if (now - app->session_check_us >= 1000000 && app->tabs_seen) {
        app->session_check_us = now;
        String text = BuildSession(app, fa);
        if (!Str_Equal(text, app->session_saved)) {
            Arena_Clear(&app->session_arena);
            app->session_saved = Str_PushCopy(&app->session_arena, text);
            out->session_text = text;
        }
    }

    // --- outputs ---
    out->input_count = f.inputs.count;
    out->inputs = f.inputs.data;
    out->new_tab_count = f.new_tabs.count;
    out->new_tabs = f.new_tabs.data;
    out->close_count = f.closes.count;
    out->closes = f.closes.data;
    out->scroll_count = f.scrolls.count;
    out->scrolls = f.scrolls.data;
    out->file_request_count = f.file_requests.count;
    out->file_requests = f.file_requests.data;
    if (app->paths_dirty) {
        // --- recents and bookmarks changed: paths.txt again ---
        app->paths_dirty = 0;
        s64 cap = 64 + (s64)(app->recent_count + app->bookmark_count) * 530, n = 0;
        u8 *text = Arena_PushArray(fa, u8, cap);
        for (s32 i = 0; i < app->recent_count; i++) n += snprintf((char *)text + n, (size_t)(cap - n), "recent %.*s\n", app->recent_len[i], (const char *)app->recent[i]);
        for (s32 i = 0; i < app->bookmark_count; i++) n += snprintf((char *)text + n, (size_t)(cap - n), "bookmark %.*s\n", app->bookmark_len[i], (const char *)app->bookmark[i]);
        out->paths_text = n ? (String){ text, n } : S("\n");   // empty lists: a file with nothing in it
    }
    out->file_watch_count = f.file_watches.count;
    out->file_watches = f.file_watches.data;
    out->file_op_count = f.file_ops.count;
    out->file_ops = f.file_ops.data;
    out->focus_tab = in->focused && panes->selected ? panes->selected->term_id : 0;
    out->term_font_px = app->term_font_pt * (f32)dpi / 72.0f;
    out->want_sessions = app->sidebar_open && in->connected;
    out->want_fonts = app->palette_open && app->palette_mode == PaletteMode_Settings;
    out->backdrop = app->backdrop;
    out->dark = !tt->light;
    if (app->placement_pending) {
        app->placement_pending = 0;
        out->set_window_placement = 1;
        out->placement_rect[0] = app->restore.x;
        out->placement_rect[1] = app->restore.y;
        out->placement_rect[2] = app->restore.w;
        out->placement_rect[3] = app->restore.h;
        out->placement_maximized = app->restore.maximized;
    }
    out->caption_height = caption_h;
    out->resize_border = resize_border;
    out->caption_client_rect_count = caption_rect_count;
    memcpy(out->caption_client_rects, caption_rects, sizeof(Rect) * (size_t)caption_rect_count);
    out->min_width = R(25.0);
    out->min_height = R(15.0);
    const TermTab *ft = panes->selected ? FindServerTab(app, panes->selected->term_id) : NULL;
    out->window_title = ft ? Str_PushFormat(fa, "%.*s - moss", STR_ARG(PaneTabLabel(app, panes->selected))) : S("moss");
    if (app->debug_overlay && !app->headless) { u64 next = now + 250000; if (!out->wake_at_us || next < out->wake_at_us) out->wake_at_us = next; }
    out->keep_animating = ui->animating || app->tab_drag_id || out->new_tab_count > 0;
    if (blink_wanted && !app->headless) {
        // the blink's next toggle, and the busy spinner's next step
        u64 next = now + 120000 - now % 120000;
        if (!out->wake_at_us || next < out->wake_at_us) out->wake_at_us = next;
    }
    out->cursor = ui->cursor;
    END_TIMED_BLOCK(AppFrame);
}

// --- for the headless runner -------------------------------------------------------------------------------------

b32 App_FindLabel(App *app, String label, s32 *x, s32 *y)
{
    for (s32 i = 0; i < app->named_count; i++) {
        if (!Str_EqualNoCase(app->named[i].name, label)) continue;
        *x = (app->named[i].rect.x0 + app->named[i].rect.x1) / 2;
        *y = (app->named[i].rect.y0 + app->named[i].rect.y1) / 2;
        return 1;
    }
    return 0;
}

b32 App_PaneRect(App *app, s32 pane, s32 rect[4], u32 *term_id)
{
    if (pane < 0 || pane >= app->pane_rect_count) return 0;
    Rect r = app->pane_rects[pane].rect;
    rect[0] = r.x0; rect[1] = r.y0; rect[2] = r.x1; rect[3] = r.y1;
    *term_id = app->pane_rects[pane].term_id;
    return 1;
}

static void DumpNode(App *app, TextBuilder *tb, PaneNode *n, s32 depth, s32 *pane_index)
{
    for (s32 i = 0; i < depth; i++) TB_Append(tb, S("  "));
    if (n->first_child) {
        TB_Format(tb, "%s split, ratio %.3f, px %d %d %d %d\n", n->vertical ? "side by side" : "stacked", (double)n->split_ratio,
                  (int)n->px[0], (int)n->px[1], (int)n->px[2], (int)n->px[3]);
        for (PaneNode *ch = n->first_child; ch; ch = ch->next) DumpNode(app, tb, ch, depth + 1, pane_index);
        return;
    }
    TB_Format(tb, "pane %d, ratio %.3f, px %d %d %d %d%s:", (*pane_index)++, (double)n->split_ratio, (int)n->px[0], (int)n->px[1],
              (int)n->px[2], (int)n->px[3], app->zoom_node == n->id ? " (zoomed)" : "");
    for (PaneTab *t = n->first_tab; t; t = t->next) {
        TB_Format(tb, " %s%s[%u] %.*s", t == app->panes.selected ? "*" : "", t->hidden ? "" : "+", t->term_id, STR_ARG(PaneTabLabel(app, t)));
        if (t->browsing && t->browser) {
            Browser *b = t->browser;
            FileEntry *cur = BrowserEntry(b, b->cursor);
            TB_Format(tb, " (files: %.*s, %lld rows, on %.*s%s)", STR_ARG(b->path), (long long)b->view_count, STR_ARG(cur ? cur->name : S("-")),
                      TabBusy(FindServerTab(app, t->term_id)) ? ", read-only" : "");
            if (b->sel.count) TB_Format(tb, " %lld selected", (long long)b->sel.count);
        }
    }
    TB_Append(tb, S("\n"));
}

String App_DumpLayout(App *app, Arena *arena)
{
    TextBuilder tb = { arena, NULL, 0, 0 };
    s32 pane_index = 0;
    if (app->panes.root) DumpNode(app, &tb, app->panes.root, 0, &pane_index);
    TB_Format(&tb, "(* focused, + shown in its pane, [server tab id])\n");
    return TB_String(&tb);
}
