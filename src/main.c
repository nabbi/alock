/*
 * alock - alock.c
 * Copyright (c) 2005 - 2007 Mathias Gumz <akira at fluxbox dot org>
 *               2014 - 2018 Arkadiusz Bokowy
 *
 * This file is a part of an alock.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "alock.h"

#if ENABLE_XRENDER
#include <X11/extensions/Xrender.h>
#endif
#include <ctype.h>
#include <getopt.h>
#include <locale.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <X11/Xatom.h>
#include <X11/Xos.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>


extern char **environ;


#if ENABLE_XRENDER

struct swCursor {
    Display     *display;
    Window       bg_win;
    Pixmap       src_pm;    /* cursor pixels uploaded to server (depth 32) */
    Picture      src_pic;   /* XRender picture wrapping src_pm */
    Pixmap       save_pm;   /* bg pixels under cursor before stamping */
    Picture      bg_pic;    /* picture wrapping bg_win for compositing */
    GC           gc;        /* for XCopyArea save/restore */
    unsigned int w, h;
    int          hot_x, hot_y;
    int          cur_x, cur_y; /* position of last stamp */
    int          valid;        /* non-zero once cursor has been stamped once */
};

static struct swCursor *swCursorCreate(Display *dpy, Window bg_win,
                                       const struct aCursorImage *img) {
    int screen = DefaultScreen(dpy);

    XRenderPictFormat *fmt32 = XRenderFindStandardFormat(dpy, PictStandardARGB32);
    if (!fmt32)
        return NULL;

    struct swCursor *swc = calloc(1, sizeof(*swc));
    if (!swc)
        return NULL;

    swc->display = dpy;
    swc->bg_win  = bg_win;
    swc->w       = img->width;
    swc->h       = img->height;
    swc->hot_x   = img->hot_x;
    swc->hot_y   = img->hot_y;

    /* Upload cursor pixels to a depth-32 server-side pixmap. */
    swc->src_pm = XCreatePixmap(dpy, bg_win, img->width, img->height, 32);
    GC gc32 = XCreateGC(dpy, swc->src_pm, 0, NULL);
    XImage ximg = {0};
    ximg.width            = (int)img->width;
    ximg.height           = (int)img->height;
    ximg.format           = ZPixmap;
    ximg.data             = (char *)img->pixels;
    ximg.byte_order       = alock_native_byte_order();
    ximg.bitmap_unit      = 32;
    ximg.bitmap_bit_order = ximg.byte_order;
    ximg.bitmap_pad       = 32;
    ximg.depth            = 32;
    ximg.bits_per_pixel   = 32;
    ximg.bytes_per_line   = (int)img->width * 4;
    ximg.red_mask         = 0x00ff0000;
    ximg.green_mask       = 0x0000ff00;
    ximg.blue_mask        = 0x000000ff;
    XInitImage(&ximg);
    XPutImage(dpy, swc->src_pm, gc32, &ximg, 0, 0, 0, 0, img->width, img->height);
    XFreeGC(dpy, gc32);
    swc->src_pic = XRenderCreatePicture(dpy, swc->src_pm, fmt32, 0, NULL);

    /* Save pixmap (same depth as bg_win) for bg save/restore under cursor. */
    int depth = DefaultDepth(dpy, screen);
    swc->save_pm = XCreatePixmap(dpy, bg_win, img->width, img->height, depth);

    XRenderPictFormat *bg_fmt = XRenderFindVisualFormat(dpy, DefaultVisual(dpy, screen));

    /* ClipByChildren (default): stamp goes to bg_win only; the frame child sits on top. */
    swc->bg_pic = XRenderCreatePicture(dpy, bg_win, bg_fmt, 0, NULL);

    swc->gc = XCreateGC(dpy, bg_win, 0, NULL);

    return swc;
}

/* Restore bg at old position, save bg at new position, stamp cursor. */
static void swCursorMove(struct swCursor *swc, int x, int y) {
    Display *dpy = swc->display;
    int dst_x = x - swc->hot_x;
    int dst_y = y - swc->hot_y;

    if (swc->valid) {
        int old_x = swc->cur_x - swc->hot_x;
        int old_y = swc->cur_y - swc->hot_y;
        XCopyArea(dpy, swc->save_pm, swc->bg_win, swc->gc,
                  0, 0, swc->w, swc->h, old_x, old_y);
    }

    XCopyArea(dpy, swc->bg_win, swc->save_pm, swc->gc,
              dst_x, dst_y, swc->w, swc->h, 0, 0);

    XRenderComposite(dpy, PictOpOver,
                     swc->src_pic, None, swc->bg_pic,
                     0, 0, 0, 0,
                     dst_x, dst_y, swc->w, swc->h);

    swc->cur_x = x;
    swc->cur_y = y;
    swc->valid = 1;
}

/* Re-stamp cursor after bg repaint. save_pm is left untouched — it already holds the clean bg
 * from the last swCursorMove; overwriting it here would corrupt the restore on the next move.
 * Falls back to XQueryPointer if no prior move was recorded. */
static void swCursorRestamp(struct swCursor *swc) {
    if (!swc->valid) {
        Window root_ret, child_ret;
        int root_x, root_y, win_x, win_y;
        unsigned int mask;
        if (XQueryPointer(swc->display, DefaultRootWindow(swc->display),
                          &root_ret, &child_ret,
                          &root_x, &root_y, &win_x, &win_y, &mask))
            swCursorMove(swc, root_x, root_y);
        return;
    }
    Display *dpy = swc->display;
    int dst_x = swc->cur_x - swc->hot_x;
    int dst_y = swc->cur_y - swc->hot_y;

    XRenderComposite(dpy, PictOpOver,
                     swc->src_pic, None, swc->bg_pic,
                     0, 0, 0, 0,
                     dst_x, dst_y, swc->w, swc->h);
}

static void swCursorDestroy(struct swCursor *swc) {
    if (!swc) return;
    Display *dpy = swc->display;
    XRenderFreePicture(dpy, swc->src_pic);
    XFreePixmap(dpy, swc->src_pm);
    XFreePixmap(dpy, swc->save_pm);
    XRenderFreePicture(dpy, swc->bg_pic);
    XFreeGC(dpy, swc->gc);
    free(swc);
}
static inline Window swCursorGetBgWin(struct swCursor *swc) { return swc->bg_win; }

#else /* !ENABLE_XRENDER */

struct swCursor;
static inline struct swCursor *swCursorCreate(Display *d, Window w,
                                              const struct aCursorImage *i)
    { (void)d; (void)w; (void)i; return NULL; }
static inline void swCursorMove(struct swCursor *s, int x, int y)
    { (void)s; (void)x; (void)y; }
static inline void swCursorRestamp(struct swCursor *s) { (void)s; }
static inline void swCursorDestroy(struct swCursor *s) { (void)s; }
static inline Window swCursorGetBgWin(struct swCursor *s) { (void)s; return None; }

#endif /* ENABLE_XRENDER */

static struct aModuleAuth *alock_modules_auth[] = {
#if ENABLE_PAM
    &alock_auth_pam,
#endif
#if ENABLE_PASSWD
    &alock_auth_passwd,
#endif
#if ENABLE_HASH
    &alock_auth_hash,
#endif
    &alock_auth_none,
    NULL
};

static struct aModuleBackground *alock_modules_background[] = {
    &alock_bg_blank,
#if ENABLE_IMLIB2
    &alock_bg_image,
#endif
#if ENABLE_XRENDER
    &alock_bg_shade,
#endif
    &alock_bg_none,
    NULL
};

static struct aModuleCursor *alock_modules_cursor[] = {
    &alock_cursor_none,
    &alock_cursor_blank,
    &alock_cursor_glyph,
#if ENABLE_XCURSOR
    &alock_cursor_xcursor,
#if (ENABLE_XRENDER && (ENABLE_XPM || ENABLE_IMLIB2))
    &alock_cursor_image,
#endif
#endif
    NULL
};

static struct aModuleInput *alock_modules_input[] = {
    &alock_input_frame,
    &alock_input_none,
    NULL
};


/* Register alock instance. This function returns 0 on success or -1 when
 * another instance is already registered. Note, that this function does
 * not guarantee 100% assurance, it is NOT multi-process safe! */
static int registerInstance(Display *display) {

    Window root = DefaultRootWindow(display);
    Atom atom = XInternAtom(display, "ALOCK_INSTANCE_PID", False);
    pid_t pid = 0;

    { /* detect previous instance */

        Atom ret_type;
        int ret_fmt;
        unsigned long ret_nb;
        unsigned long ret_bleft;
        pid_t *ret_data;
        int rv;

        rv = XGetWindowProperty(display, root, atom,
                0L, 1L, False, XA_CARDINAL, &ret_type, &ret_fmt,
                &ret_nb, &ret_bleft, (unsigned char **)&ret_data);
        if (rv == Success && ret_type != None && ret_data) {
            pid = *ret_data;
            XFree(ret_data);
        }

        if (pid && kill(pid, 0) == 0)
            /* atom was registered and the found process is alive */
            return -1;

    }

    debug("registering instance");
    pid = getpid();
    XChangeProperty(display, root, atom, XA_CARDINAL,
            sizeof(pid_t) * 8, PropModeReplace, (unsigned char *)&pid, 1);
    XFlush(display);

    return 0;
}

/* Unregister alock instance. */
static void unregisterInstance(Display *display) {

    Window root = DefaultRootWindow(display);
    Atom atom = XInternAtom(display, "ALOCK_INSTANCE_PID", True);

    if (atom != None) {
        debug("unregistering instance");
        XDeleteProperty(display, root, atom);
    }

}

#if WITH_XBLIGHT
/* Get the current backlight brightness value. If such a parameter can not be
 * obtained - display output is not compatible, xbacklight is not available,
 * etc. - this function returns -1. */
static float getBacklightBrightness(void) {

    FILE *f;
    char str[16];
    float value = -1;

    if ((f = popen("xbacklight", "r")) == NULL)
        return -1;

    if (fgets(str, sizeof(str), f) != NULL)
        value = strtof(str, NULL);

    pclose(f);
    return value;
}
#endif /* WITH_XBLIGHT */

#if WITH_XBLIGHT
/* Set current backlight brightness to the given value. */
static void setBacklightBrightness(float value) {

    char _value[16];
    char xbacklight[] = "xbacklight";
    char *argv[] = { xbacklight, "-set", _value, NULL };
    int status;
    pid_t pid;

    /* We're going to use the approach based on the explicit fork and exec
     * instead of the standard library system() call, because we need the
     * control to be returned to our own process immediately - we're not
     * interested in the return value of the child process very much. */

    snprintf(_value, sizeof(_value), "%.2f", value);
    if (posix_spawnp(&pid, xbacklight, NULL, NULL, argv, environ) == 0)
        waitpid(pid, &status, WNOHANG);

}
#endif /* WITH_XBLIGHT */

/* Polls up to timeout_ms for Expose/VisibilityNotify from any of wins[]. */
static void waitForPaint(Display *display, Window *wins, int nwins,
                         unsigned long timeout_ms) {

    unsigned long start = alock_mtime();

    while (alock_mtime() - start < timeout_ms) {

        XEvent ev;

        while (XCheckMaskEvent(display,
                    ExposureMask | StructureNotifyMask | VisibilityChangeMask,
                    &ev)) {

            Window src = None;
            if (ev.type == Expose)
                src = ev.xexpose.window;
            else if (ev.type == VisibilityNotify)
                src = ev.xvisibility.window;
            else
                /* drain StructureNotify; not relevant to paint state */
                continue;

            for (int j = 0; j < nwins; j++)
                if (wins[j] == src)
                    return;
        }

        usleep(10000);
    }
}

/* Lock current display and grab pointer and keyboard. On successful
 * lock this function returns 0, otherwise -1. */
static int lockDisplay(Display *display, struct aModules *modules) {

    Window window;
    Cursor cursor;

    int nscreens = ScreenCount(display);
    Window *bgwins = calloc(nscreens, sizeof(*bgwins));

    int bgcount = 0;

    if (bgwins == NULL) {
        fprintf(stderr, "alock: error window out of memory\n");
        return -1;
    }

    /* Raise/map background windows and remember them */
    for (int i = 0; i < nscreens; i++) {

        Window bg = modules->background->getwindow(i);
        if (bg != None) {

            Window window_input = modules->input->getwindow(i);
            if (window_input != None)
                XReparentWindow(display, window_input, bg, 0, 0);

            /* Listen for paint-related events from the background window */
            XSelectInput(display, bg, ExposureMask | StructureNotifyMask | VisibilityChangeMask);

            XMapWindow(display, bg);
            XRaiseWindow(display, bg);

            bgwins[bgcount++] = bg;
        }

        /* receive notification about root window geometry change */
        XSelectInput(display, RootWindow(display, i), StructureNotifyMask);
    }

    /* Flush requests, then wait for bg windows to paint before grabbing. */
    XSync(display, False);

    if (bgcount > 0)
        waitForPaint(display, bgwins, bgcount, 500);

    /* Final sync before cursor/grab */
    XSync(display, False);

    free(bgwins);

    /* grab pointer and keyboard from the default screen */
    window = DefaultRootWindow(display);

    int use_swcursor = (modules->cursor->getimage() != NULL);

    if (use_swcursor) {
        /* Blank hw cursor + PointerMotionMask: no save-under for NVIDIA to corrupt. */
        char no_data[8] = { 0 };
        XColor black = { 0 };
        Pixmap blank_pm = XCreateBitmapFromData(display, window, no_data, 8, 8);
        cursor = XCreatePixmapCursor(display, blank_pm, blank_pm, &black, &black, 0, 0);
        XFreePixmap(display, blank_pm);

        if (XGrabPointer(display, window, False, PointerMotionMask,
                    GrabModeAsync, GrabModeAsync, None,
                    cursor, CurrentTime) != GrabSuccess) {
            XFreeCursor(display, cursor);
            fprintf(stderr, "error: grab pointer failed\n");
            return -1;
        }
        XFreeCursor(display, cursor);
    } else {
        /* Legacy path: grab without cursor first, then swap to ARGB cursor. */
        if (XGrabPointer(display, window, False, 0,
                    GrabModeAsync, GrabModeAsync, None,
                    None, CurrentTime) != GrabSuccess) {
            fprintf(stderr, "error: grab pointer failed\n");
            return -1;
        }

        XSync(display, False);

        cursor = modules->cursor->getcursor();
        XChangeActivePointerGrab(display, 0, cursor, CurrentTime);
    }

    /* try to grab 2 times, another process (windowmanager) may have grabbed
     * the keyboard already */
    if (XGrabKeyboard(display, window, True, GrabModeAsync, GrabModeAsync,
                CurrentTime) != GrabSuccess) {
        sleep(1);
        if (XGrabKeyboard(display, window, True, GrabModeAsync, GrabModeAsync,
                    CurrentTime) != GrabSuccess) {
            fprintf(stderr, "error: grab keyboard failed\n");
            return -1;
        }
    }

    return 0;
}

#if ENABLE_XRENDER
/* Repaint bg windows and re-stamp software cursor; blank hw cursor means no NVIDIA sprite corruption. */
static void refreshSwCursorAfterResume(Display *display, struct aModules *modules,
                                       struct swCursor *swc) {
    int nscreens = ScreenCount(display);
    Window bgwins[nscreens];
    int bgcount = 0;

    for (int i = 0; i < nscreens; i++) {
        Window bg = modules->background->getwindow(i);
        if (bg != None) {
            XRaiseWindow(display, bg);
            XClearArea(display, bg, 0, 0, 0, 0, True);
            bgwins[bgcount++] = bg;
        }
    }
    XSync(display, False);
    if (bgcount > 0)
        waitForPaint(display, bgwins, bgcount, 1000);

    swCursorRestamp(swc);
    XFlush(display);
}

/* Detect CLOCK_BOOTTIME jump → refresh sw cursor; no-op when swc is NULL (opaque cursors have no bleed-through). */
static void pollResumeState(Display *display, struct aModules *modules,
                            struct swCursor *swc,
                            unsigned long *last_time) {
    unsigned long now = alock_mtime();
    if (now - *last_time > 500 && swc) {
        refreshSwCursorAfterResume(display, modules, swc);
        now = alock_mtime();
    }
    *last_time = now;
}

#else /* !ENABLE_XRENDER */

static inline void pollResumeState(Display *display, struct aModules *modules,
                                   struct swCursor *swc,
                                   unsigned long *last_time)
    { (void)display; (void)modules; (void)swc; *last_time = alock_mtime(); }

#endif /* ENABLE_XRENDER */

static void eventLoop(Display *display, struct aModules *modules,
                      struct swCursor *swc) {

    XEvent ev;
    KeySym ks;
    char cbuf[10];
    wchar_t pass[128];
    unsigned int clen;
    unsigned int pass_pos = 0, pass_len = 0;
    unsigned long keypress_time = 0;
    unsigned long last_time = alock_mtime();

    /* if possible do not page this address to the swap area */
    mlock(pass, sizeof(pass));

    debug("entering event main loop");
    for (;;) {

        if (keypress_time) {
            /* check for any key press event (or root window state change) */
            if (XCheckMaskEvent(display, KeyPressMask | StructureNotifyMask, &ev) == False) {

                /* user fell asleep while typing (5 seconds inactivity) */
                if (alock_mtime() - keypress_time > 5000) {
                    modules->input->setstate(AINPUT_STATE_NONE);
                    keypress_time = 0;
                }

                /* Drain motion and Expose events; restamp cursor if bg was repainted. */
                if (swc) {
                    XEvent mev;
                    while (XCheckMaskEvent(display, PointerMotionMask, &mev))
                        swCursorMove(swc, mev.xmotion.x, mev.xmotion.y);
                    int need_restamp = 0;
                    while (XCheckWindowEvent(display, swCursorGetBgWin(swc), ExposureMask, &mev))
                        need_restamp = 1;
                    if (need_restamp)
                        swCursorRestamp(swc);
                }

                pollResumeState(display, modules, swc, &last_time);

                /* wait a bit */
                usleep(25000);
                continue;
            }
        }
        else {

#if WITH_XBLIGHT
            /* dim out display backlight */
            if (modules->backlight != -1)
                setBacklightBrightness(0);
#endif /* WITH_XBLIGHT */

            /* poll for events so resume can be detected via clock jump */
            while (XCheckMaskEvent(display, KeyPressMask | StructureNotifyMask, &ev) == False) {
                /* Drain motion and Expose events; restamp cursor if bg was repainted. */
                if (swc) {
                    XEvent mev;
                    while (XCheckMaskEvent(display, PointerMotionMask, &mev))
                        swCursorMove(swc, mev.xmotion.x, mev.xmotion.y);
                    int need_restamp = 0;
                    while (XCheckWindowEvent(display, swCursorGetBgWin(swc), ExposureMask, &mev))
                        need_restamp = 1;
                    if (need_restamp)
                        swCursorRestamp(swc);
                }
                pollResumeState(display, modules, swc, &last_time);
                usleep(swc ? 16000 : 100000);
            }

#if WITH_XBLIGHT
            /* restore original backlight brightness value */
            if (modules->backlight != -1)
                setBacklightBrightness(modules->backlight);
#endif /* WITH_XBLIGHT */
        }

        last_time = alock_mtime();

        switch (ev.type) {
        case KeyPress:

            /* swallow up first key press to indicate "enter mode" */
            if (keypress_time == 0) {
                modules->input->setstate(AINPUT_STATE_INIT);
                keypress_time = alock_mtime();
                pass_pos = pass_len = 0;
                pass[0] = '\0';
                break;
            }

            keypress_time = alock_mtime();
            clen = XLookupString(&ev.xkey, cbuf, sizeof(cbuf), &ks, NULL);
            debug("key input: %lx, %d, `%.*s`", ks, clen, clen, cbuf);

            /* terminal-like key remapping */
            if (clen == 1 && iscntrl(cbuf[0]))
                switch (cbuf[0]) {
                case 0x03 /* Ctrl-C */ :
                    ks = XK_Escape;
                    break;
                case 0x08 /* Ctrl-H */ :
                    ks = XK_BackSpace;
                    break;
                case 0x0A /* Ctrl-J */ :
                case 0x0D /* Ctrl-M */ :
                    ks = XK_Return;
                    break;
                }

            /* translate key press symbol */
            ks = modules->input->keypress(ks);

            switch (ks) {
            case NoSymbol:
                break;

            /* clear/initialize input buffer */
            case XK_Escape:
            case XK_Clear:
                pass_pos = pass_len = 0;
                pass[0] = '\0';
                break;

            /* input position navigation */
            case XK_Begin:
            case XK_Home:
                pass_pos = 0;
                break;
            case XK_End:
                pass_pos = pass_len;
                break;
            case XK_Left:
                if (pass_pos > 0)
                    pass_pos--;
                break;
            case XK_Right:
                if (pass_pos < pass_len)
                    pass_pos++;
                break;

            /* remove entered characters */
            case XK_Delete:
                if (pass_pos < pass_len) {
                    wmemmove(&pass[pass_pos], &pass[pass_pos + 1], pass_len - pass_pos);
                    pass_len--;
                }
                break;
            case XK_BackSpace:
                if (pass_pos > 0) {
                    wmemmove(&pass[pass_pos - 1], &pass[pass_pos], pass_len - pass_pos + 1);
                    pass_pos--;
                    pass_len--;
                }
                break;

            /* input confirmation and authentication test */
            case XK_KP_Enter:
            case XK_Linefeed:
            case XK_Return: {

                char rbuf[sizeof(pass)];
                int rv;

                modules->input->setstate(AINPUT_STATE_CHECK);

                wcstombs(rbuf, pass, sizeof(rbuf));
                rv = modules->auth->authenticate(rbuf);

                memset(rbuf, 0, sizeof(rbuf));
                memset(pass, 0, sizeof(pass));
                pass_pos = pass_len = 0;

                if (rv == 0) { /* successful authentication */
                    modules->input->setstate(AINPUT_STATE_VALID);
                    return;
                }

                modules->input->setstate(AINPUT_STATE_ERROR);
                modules->input->setstate(AINPUT_STATE_INIT);
                keypress_time = alock_mtime();

                break;
            }

            /* input new character at the current input position */
            default:
                if (clen > 0 && !iscntrl(cbuf[0]) && pass_len < (sizeof(pass) / sizeof(*pass) - 1)) {
                    wmemmove(&pass[pass_pos + 1], &pass[pass_pos], pass_len - pass_pos + 1);
                    mbtowc(&pass[pass_pos], cbuf, clen);
                    pass_pos++;
                    pass_len++;
                }
                break;
            }

            debug("entered phrase [%zu]: `%ls`", wcslen(pass), pass);
            break;

        case ConfigureNotify:
            /* NOTE: This event should be generated for the root window upon
             *       the display reconfiguration (e.g. resolution change). */

            debug("received configure notify event");
            break;

        }
    }
}

int main(int argc, char **argv) {

    int opt;
    struct option longopts[] = {
        {"help", no_argument, NULL, 'h'},
        {"modules", no_argument, NULL, 'm'},
        {"auth", required_argument, NULL, 'a'},
        {"bg", required_argument, NULL, 'b'},
        {"cursor", required_argument, NULL, 'c'},
        {"input", required_argument, NULL, 'i'},
        {0, 0, 0, 0},
    };

    Display *display;
    struct aModules modules;
    int retval;

    const char *args_auth = NULL;
    const char *args_background = NULL;
    const char *args_cursor = NULL;
    const char *args_input = NULL;

    /* set-up default modules */
    modules.auth = alock_modules_auth[0];
    modules.background = alock_modules_background[0];
    modules.cursor = alock_modules_cursor[0];
    modules.input = alock_modules_input[0];

#if WITH_XBLIGHT
    modules.backlight = -1;
#endif

    /* parse options */
    while ((opt = getopt_long_only(argc, argv, "hma:b:c:i:", longopts, NULL)) != -1)
        switch (opt) {
        case 'h':
            printf("%s [-help] [-modules] [-auth type:options] [-bg type:options]"
                    " [-cursor type:options] [-input type:options]\n", argv[0]);
            return EXIT_SUCCESS;

        case 'm': { /* list available modules */

            struct aModuleAuth **ia;
            struct aModuleBackground **ib;
            struct aModuleCursor **ic;
            struct aModuleInput **ii;

            printf("authentication modules:\n");
            for (ia = alock_modules_auth; *ia; ++ia)
                printf("  %s\n", (*ia)->m.name);

            printf("background modules:\n");
            for (ib = alock_modules_background; *ib; ++ib)
                printf("  %s\n", (*ib)->m.name);

            printf("cursor modules:\n");
            for (ic = alock_modules_cursor; *ic; ++ic)
                printf("  %s\n", (*ic)->m.name);

            printf("input modules:\n");
            for (ii = alock_modules_input; *ii; ++ii)
                printf("  %s\n", (*ii)->m.name);

            return EXIT_SUCCESS;
        }

        case 'a': { /* authentication module */

            struct aModuleAuth **i;
            for (i = alock_modules_auth; *i; ++i)
                if (strstr(optarg, (*i)->m.name) == optarg) {
                    args_auth = optarg;
                    modules.auth = *i;
                    break;
                }

            if (*i == NULL) {
                fprintf(stderr, "alock: authentication module `%s` not found\n", optarg);
                return EXIT_FAILURE;
            }

            break;
        }

        case 'b': { /* background module */

            struct aModuleBackground **i;
            for (i = alock_modules_background; *i; ++i)
                if (strstr(optarg, (*i)->m.name) == optarg) {
                    args_background = optarg;
                    modules.background = *i;
                    break;
                }

            if (*i == NULL) {
                fprintf(stderr, "alock: background module `%s` not found\n", optarg);
                return EXIT_FAILURE;
            }

            break;
        }

        case 'c': { /* cursor module */

            struct aModuleCursor **i;
            for (i = alock_modules_cursor; *i; ++i)
                if (strstr(optarg, (*i)->m.name) == optarg) {
                    args_cursor = optarg;
                    modules.cursor = *i;
                    break;
                }

            if (*i == NULL) {
                fprintf(stderr, "alock: cursor module `%s` not found\n", optarg);
                return EXIT_FAILURE;
            }

            break;
        }

        case 'i': { /* input module */

            struct aModuleInput **i;
            for (i = alock_modules_input; *i; ++i)
                if (strstr(optarg, (*i)->m.name) == optarg) {
                    args_input = optarg;
                    modules.input = *i;
                    break;
                }

            if (*i == NULL) {
                fprintf(stderr, "alock: input module `%s` not found\n", optarg);
                return EXIT_FAILURE;
            }

            break;
        }

        default:
            fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
            return EXIT_FAILURE;
        }

    /* required for correct input handling */
    setlocale(LC_ALL, "");

    if ((display = XOpenDisplay(NULL)) == NULL) {
        fprintf(stderr, "error: unable to connect to the X display\n");
        return EXIT_FAILURE;
    }

    /* make sure, that only one instance of alock is running */
    if (registerInstance(display)) {
        fprintf(stderr, "error: another instance seems to be running\n");
        XCloseDisplay(display);
        return EXIT_FAILURE;
    }

#if WITH_DUNST
    /* pause notification daemon */
    system("pkill -SIGUSR1 -x dunst");
#endif

    { /* try to initialize selected modules */

        int rv = 0;

        XrmInitialize();
        const char *data = XResourceManagerString(display);
        XrmDatabase xrdb = XrmGetStringDatabase(data != NULL ? data : "");

        modules.auth->m.loadxrdb(xrdb);
        modules.background->m.loadxrdb(xrdb);
        modules.cursor->m.loadxrdb(xrdb);
        modules.input->m.loadxrdb(xrdb);

#if WITH_XBLIGHT
        XrmValue value;
        char *type;

        if (XrmGetResource(xrdb, "alock.backlight", "ALock.Backlight",
                    &type, &value) && strcmp(value.addr, "true") == 0)
            modules.backlight = getBacklightBrightness();
#endif /* WITH_XBLIGHT */

        XrmDestroyDatabase(xrdb);

        modules.auth->m.loadargs(args_auth);
        modules.background->m.loadargs(args_background);
        modules.cursor->m.loadargs(args_cursor);
        modules.input->m.loadargs(args_input);

        if (modules.auth->m.init(display)) {
            fprintf(stderr, "alock: failed init of [%s] with [%s]\n",
                    modules.auth->m.name, args_auth);
            rv |= 1;
        }

#if ENABLE_PASSWD
        /* We can be installed setuid root to support shadow passwords,
         * and we don't need root privileges any longer.  --marekm */
        if (setuid(getuid()) != 0)
            perror("alock: root privilege drop failed");
#endif

        if (modules.background->m.init(display)) {
            fprintf(stderr, "alock: failed init of [%s] with [%s]\n",
                    modules.background->m.name, args_background);
            rv |= 1;
        }
        if (modules.cursor->m.init(display)) {
            fprintf(stderr, "alock: failed init of [%s] with [%s]\n",
                    modules.cursor->m.name, args_cursor);
            rv |= 1;
        }
        if (modules.input->m.init(display)) {
            fprintf(stderr, "alock: failed init of [%s] with [%s]\n",
                    modules.input->m.name, args_input);
            rv |= 1;
        }

        if (rv) /* initialization failed */
            goto return_failure;

    }

    /* raise our background window and grab input, if this action has failed,
     * we are not able to lock the screen, then we're fucked... */
    if (lockDisplay(display, &modules))
        goto return_failure;

    /* If the cursor module provides image data, set up the software overlay
     * on the primary screen's background window. */
    struct swCursor *swc = NULL;
    {
        const struct aCursorImage *img = modules.cursor->getimage();
        if (img) {
            Window bg0 = modules.background->getwindow(0);
            if (bg0 != None)
                swc = swCursorCreate(display, bg0, img);
            if (!swc)
                fprintf(stderr, "alock: software cursor setup failed, using hardware cursor\n");
        }
    }

    /* Stamp the cursor at the current pointer position so it is visible
     * immediately without waiting for the first MotionNotify. */
    if (swc) {
        Window root_ret, child_ret;
        int root_x, root_y, win_x, win_y;
        unsigned int mask;
        if (XQueryPointer(display, DefaultRootWindow(display),
                          &root_ret, &child_ret,
                          &root_x, &root_y, &win_x, &win_y, &mask))
            swCursorMove(swc, root_x, root_y);
        XFlush(display);
    }

    debug("entering main event loop");
    eventLoop(display, &modules, swc);

    retval = EXIT_SUCCESS;
    goto return_success;

return_failure:
    retval = EXIT_FAILURE;

return_success:

    swCursorDestroy(swc);
    modules.auth->m.free();
    modules.cursor->m.free();
    modules.input->m.free();
    modules.background->m.free();

    unregisterInstance(display);
    XCloseDisplay(display);

#if WITH_DUNST
    /* resume notification daemon */
    system("pkill -SIGUSR2 -x dunst");
#endif

    return retval;
}
