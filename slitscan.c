/*
 * gcc -o slitscan slitscan.c  -L/usr/X11R6/lib -lX11 -lXext -lXv
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xv.h>
#include <X11/extensions/Xvlib.h>
#include <X11/extensions/XShm.h>


int width = 640;
int height = 480;

char *video_dev = "/dev/video0";

Display *dpy;
XvImage *image;

void fatal(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    printf(format, ap);
    va_end(ap);
    exit(-1);
}

void debug(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    printf(format, ap);
    va_end(ap);
}

int create_window() {
    dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        fatal("Cannot open Display.\n");
    }

    int screen = DefaultScreen(dpy);
    XVisualInfo vinfo;

    if (!XMatchVisualInfo(dpy, screen, 24, TrueColor, &vinfo)) {
        fatal("Could not get 24-bit display.\n");
    }

    XSizeHints hint = {
        .x = 1,
        .y = 1,
        .width = width,
        .height = height,
        .flags = PPosition | PSize
    };

    XSetWindowAttributes xswa = {
        .colormap =  XCreateColormap(dpy, DefaultRootWindow(dpy), vinfo.visual, AllocNone),
        .event_mask = StructureNotifyMask | ExposureMask | KeyPressMask,
        .background_pixel = 0,
        .border_pixel = 0
    };

    unsigned long mask = CWBackPixel | CWBorderPixel | CWColormap | CWEventMask;

    Window window = XCreateWindow(dpy, DefaultRootWindow(dpy),
			 0, 0,
			 width,
			 height,
			 0, vinfo.depth,
			 InputOutput,
			 vinfo.visual,
			 mask, &xswa);

    XStoreName(dpy, window, "Slitscan demo");
    XSetIconName(dpy, window, "Slitscan demo");

    XSelectInput(dpy, window, StructureNotifyMask | KeyPressMask);

    /** Map window */
    XMapWindow(dpy, window);

    XEvent event;

    /** Wait for map. */
    do {
        XNextEvent(dpy, &event);
    }
    while (event.type != MapNotify || event.xmap.event != window);

    return 0;
}

void init_camera() {

}


void main_loop() {
    XEvent event;

    debug("*** PRESS ANY KEY TO EXIT ***\n");

    while (1) {
        //grab_frame();
        //create_frame();
        //push_frame();
        XNextEvent(dpy, &event);
        if (event.type == KeyPress) {
            break;
        }
    }
}

void shutdown_camera() {

}

void destroy_window() {

}

int main(int argc, char* argv[]) {
    init_camera();
    create_window();
    // allocate buffers? prepare map?
    main_loop();
    destroy_window();
    shutdown_camera();
    return 0;
}
