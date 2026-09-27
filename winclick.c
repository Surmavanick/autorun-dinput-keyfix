/*
 * winclick.exe - click a button in another Wine/Windows window by title and
 * button text (BM_CLICK), or list windows. Runs inside the same Wine prefix.
 *
 *   winclick.exe list
 *   winclick.exe click "<window title substring>" "<button text>"
 *   winclick.exe watch <seconds> "<button text>"   ; click that button on every
 *                                                  ; dialog that appears, print titles
 *
 * Build: zig cc -target x86-windows-gnu -O1 -o winclick.exe winclick.c -luser32 -lkernel32
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static HWND g_found; static const char *g_text;

static BOOL CALLBACK find_button(HWND h, LPARAM lp)
{
    char cls[64], txt[256];
    GetClassNameA(h, cls, sizeof cls); GetWindowTextA(h, txt, sizeof txt);
    if (strstr(txt, g_text) && (lstrcmpiA(cls, "Button") == 0 || strstr(cls, "Button")))
    {
        if (IsWindowEnabled(h) && IsWindowVisible(h)) { g_found = h; return FALSE; }
    }
    return TRUE;
}

static BOOL CALLBACK list_top(HWND h, LPARAM lp)
{
    char cls[64], txt[256];
    if (!IsWindowVisible(h)) return TRUE;
    GetClassNameA(h, cls, sizeof cls); GetWindowTextA(h, txt, sizeof txt);
    if (txt[0]) printf("%p [%s] \"%s\"\n", (void *)h, cls, txt);
    return TRUE;
}

static BOOL CALLBACK list_children(HWND h, LPARAM lp)
{
    char cls[64], txt[256];
    GetClassNameA(h, cls, sizeof cls); GetWindowTextA(h, txt, sizeof txt);
    printf("    child %p [%s] \"%s\" %s%s\n", (void *)h, cls, txt, IsWindowVisible(h) ? "visible" : "hidden", IsWindowEnabled(h) ? "" : " disabled");
    return TRUE;
}

typedef struct { const char *title; HWND hwnd; } FIND;
static BOOL CALLBACK find_top(HWND h, LPARAM lp)
{
    FIND *f = (FIND *)lp; char txt[256];
    if (!IsWindowVisible(h)) return TRUE;
    GetWindowTextA(h, txt, sizeof txt);
    if (strstr(txt, f->title)) { f->hwnd = h; return FALSE; }
    return TRUE;
}

static int click_in(HWND top, const char *button)
{
    g_found = NULL; g_text = button;
    EnumChildWindows(top, find_button, 0);
    if (!g_found) return 0;
    SendMessageA(g_found, BM_CLICK, 0, 0);
    return 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc >= 2 && strcmp(argv[1], "list") == 0)
    {
        EnumWindows(list_top, 0);
        if (argc >= 3) { FIND f = { argv[2], NULL }; EnumWindows(find_top, (LPARAM)&f); if (f.hwnd) EnumChildWindows(f.hwnd, list_children, 0); }
        return 0;
    }
    if (argc >= 4 && strcmp(argv[1], "click") == 0)
    {
        FIND f = { argv[2], NULL }; EnumWindows(find_top, (LPARAM)&f);
        if (!f.hwnd) { printf("window not found: %s\n", argv[2]); return 1; }
        if (!click_in(f.hwnd, argv[3])) { printf("button not found: %s\n", argv[3]); return 2; }
        printf("clicked \"%s\" in \"%s\"\n", argv[3], argv[2]); return 0;
    }
    if (argc >= 4 && strcmp(argv[1], "watch") == 0)
    {
        int secs = atoi(argv[2]), i; char last[256] = "";
        for (i = 0; i < secs * 2; i++)
        {
            HWND h = GetForegroundWindow(); char txt[256] = "";
            if (h) GetWindowTextA(h, txt, sizeof txt);
            if (strcmp(txt, last) != 0) { printf("[%2ds] foreground: \"%s\"\n", i / 2, txt); strcpy(last, txt); }
            if (h && click_in(h, argv[3])) printf("[%2ds] clicked \"%s\" in \"%s\"\n", i / 2, argv[3], txt);
            Sleep(500);
        }
        return 0;
    }
    printf("usage: winclick list [title] | click <title> <button> | watch <secs> <button>\n");
    return 1;
}
