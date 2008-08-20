/*
 * gcc -o slitscan slitscan.c  -L/usr/X11R6/lib -lX11 -lXext -lXv
 * TODO:
 * - Pick up XV port correctly
 * - FPS monitoring
 * - Add fullscreen mode
 * - Break into separate files (main, xv, camera)
 * - Add frame dumping (BMP?)
 * - Add watermarking
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <malloc.h>
#include <string.h>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xv.h>
#include <X11/extensions/Xvlib.h>
#include <X11/extensions/XShm.h>

#include <asm/types.h>          /* for videodev2.h */

#include <linux/videodev2.h>

extern XvImage  *XvShmCreateImage(Display*, XvPortID, int, char*, int, int, XShmSegmentInfo*);

#define CLEAR(x) memset (&(x), 0, sizeof (x))

#define XV_IMAGE_FORMAT 0x3

// FOURCCs I HAVE KNOWN AND LOVED
// RGB  0x3
// YUY2 0x32595559 Packed
// YV12 0x32315659
// I420 0x30323449
// UYVY 0x59565955 Packed

int width = 640;
int height = 480;
char *video_dev = "/dev/video0";


struct buffer {
        void *                  start;
        size_t                  length;
};


int fd                        = -1;
struct buffer *buffers        = NULL;
static unsigned int n_buffers = 0;

Display *dpy;
XvImage *image;
Window   window;
GC       gc;
int      xv_port = -1;
XShmSegmentInfo shminfo;

int32_t *timecube;
int ring_index = 0;
int frame_size;
int tc_size;

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

enum
{
  _NET_WM_STATE_REMOVE =0,
  _NET_WM_STATE_ADD = 1,
  _NET_WM_STATE_TOGGLE =2

};

void toggle_fullscreen() {
    static int fullscreen;
    fullscreen = !fullscreen;
    printf("Going to %s", fullscreen?"fullscreen":"windowed");
    // FIXME: Do this in init?
    Atom wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom fs_state = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);

    XEvent xev;
    xev.xclient.type=ClientMessage;
    xev.xclient.serial = 0;
    xev.xclient.send_event=True;
    xev.xclient.window=window;
    xev.xclient.message_type=wm_state;
    xev.xclient.format=32;
    xev.xclient.data.l[0] = (fullscreen ? _NET_WM_STATE_ADD : _NET_WM_STATE_REMOVE);
    xev.xclient.data.l[1] = fs_state;
    xev.xclient.data.l[2] = 0;
    XSendEvent(dpy, DefaultRootWindow(dpy), False,
        SubstructureRedirectMask | SubstructureNotifyMask,
        &xev);
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

    window = XCreateWindow(dpy, DefaultRootWindow(dpy),
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

    XvAdaptorInfo		*ai;
    int p_num_adaptors;
    int ret = XvQueryAdaptors(dpy, DefaultRootWindow(dpy), &p_num_adaptors, &ai);
    if (ret != Success)
        fatal("Query adaptors failed");
    xv_port = 387; //ai[0].base_id; // FIXME: HACK
    //xv_port = ai[0].base_id;
    printf("xv_port: %d\n", xv_port);

    gc = XCreateGC(dpy, window, 0, 0);

    image = XvShmCreateImage(dpy, xv_port, XV_IMAGE_FORMAT, 0, width, height, &shminfo);

    shminfo.shmid = shmget(IPC_PRIVATE, image->data_size, IPC_CREAT | 0777);
    shminfo.shmaddr = image->data = shmat(shminfo.shmid, 0, 0);
    shminfo.readOnly = False;

    if (!XShmAttach(dpy, &shminfo))
        fatal("XShmAttach failed !\n");

    printf("Attempting to write to SHM\n");

    XvShmPutImage(dpy, xv_port, window, gc, image,
        0, 0, image->width, image->height,
        0, 0, width, height, True);
    printf("Successfully initialized video\n");
    return 0;
}

static int xioctl(int fd, int request, void * arg)
{
    int r;
    do r = ioctl (fd, request, arg);
    while (-1 == r && EINTR == errno);
    return r;
}

void open_device() {
        struct stat st;

    if (-1 == stat (video_dev, &st))
        fatal("Cannot identify '%s': %d, %s\n", video_dev, errno, strerror (errno));

    if (!S_ISCHR (st.st_mode))
        fatal("%s is no device\n", video_dev);

    fd = open (video_dev, O_RDWR /* required */ | O_NONBLOCK, 0);

    if (-1 == fd)
        fatal("Cannot open '%s': %d, %s\n", video_dev, errno, strerror (errno));
}

void close_device() {
    if (-1 == close(fd))
        fatal("Could not close device");
    fd = -1;
}

void init_camera() {
    struct v4l2_capability cap;
    struct v4l2_cropcap cropcap;
    struct v4l2_crop crop;
    struct v4l2_format fmt;
	unsigned int min;

    if (-1 == xioctl (fd, VIDIOC_QUERYCAP, &cap)) {
        if (EINVAL == errno)
            fatal("%s is no V4L2 device\n", video_dev);
        else
            fatal("VIDIOC_QUERYCAP");
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
        fatal("%s is no video capture device\n", video_dev);

    if (!(cap.capabilities & V4L2_CAP_STREAMING))
        fatal("%s does not support streaming i/o\n", video_dev);

    if (cap.capabilities & V4L2_CAP_TIMEPERFRAME)
        printf("Can set time per frame.\n");

    /* Select video input, video standard and tune here. */

    CLEAR (fmt);

    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = width;
    fmt.fmt.pix.height      = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field       = V4L2_FIELD_INTERLACED;

    if (-1 == xioctl (fd, VIDIOC_S_FMT, &fmt))
        fatal("VIDIOC_S_FMT");

    /* Note VIDIOC_S_FMT may change width and height. */

	/* Buggy driver paranoia. */
	min = fmt.fmt.pix.width * 2;
	if (fmt.fmt.pix.bytesperline < min)
		fmt.fmt.pix.bytesperline = min;
	min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
	if (fmt.fmt.pix.sizeimage < min)
		fmt.fmt.pix.sizeimage = min;

    /* Setup mmap io */
	struct v4l2_requestbuffers req;

    CLEAR (req);
    req.count   = 4;
    req.type    = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory  = V4L2_MEMORY_MMAP;

	if (-1 == xioctl (fd, VIDIOC_REQBUFS, &req)) {
        if (EINVAL == errno)
            fatal("%s does not support memory mapping\n", video_dev);
        else
            fatal("VIDIOC_REQBUFS");
    }

    if (req.count < 2)
        fatal("Insufficient buffer memory on %s\n", video_dev);

    buffers = calloc (req.count, sizeof (*buffers));

    if (!buffers)
        fatal("Out of memory\n");

    for (n_buffers = 0; n_buffers < req.count; ++n_buffers) {
        struct v4l2_buffer buf;
        CLEAR (buf);
        buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory      = V4L2_MEMORY_MMAP;
        buf.index       = n_buffers;

        if (-1 == xioctl (fd, VIDIOC_QUERYBUF, &buf))
            fatal("VIDIOC_QUERYBUF");

        buffers[n_buffers].length = buf.length;
        buffers[n_buffers].start =
        mmap (NULL /* start anywhere */,
            buf.length,
            PROT_READ | PROT_WRITE /* required */,
            MAP_SHARED /* recommended */,
            fd, buf.m.offset);

        if (MAP_FAILED == buffers[n_buffers].start)
            fatal("mmap");
    }
}

void start_capture() {
    int i;
    enum v4l2_buf_type type;
    for (i = 0; i < n_buffers; ++i) {
        struct v4l2_buffer buf;
   		CLEAR (buf);
        buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory      = V4L2_MEMORY_MMAP;
        buf.index       = i;

        if (-1 == xioctl (fd, VIDIOC_QBUF, &buf))
            fatal("VIDIOC_QBUF");
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (-1 == xioctl (fd, VIDIOC_STREAMON, &type))
        fatal("VIDIOC_STREAMON");

}

void save_frame_bw(int buf_index) {
    int col,row;
    int y;
    // Save frame to timecube slot
    for (row = 0; row < height; row++) {
        for (col = 0; col < width; col++) {
            y = (((unsigned short *)buffers[buf_index].start)[width * row + (width - col)]) & 0xff;
            timecube[(ring_index * frame_size) + (row * width) + col] =
                (y << 16) + (y << 8) + y;

        }
    }
}

void save_frame_color(int buf_index) {
{
	unsigned char* start = buffers[buf_index].start;
	/* FIXME: Optimize please */
    int r, g, b;
    int y, u, v;
    int c, d, e;

    int rgbp;

    for (rgbp = 0; rgbp < height * width; rgbp ++) {
        if (rgbp % 2 == 0) {
            y = start[rgbp * 2 + 0];
            u = start[rgbp * 2 + 1];
            v = start[rgbp * 2 + 3];
        }
        else {
            y = start[(rgbp - 1) * 2 + 2];
            u = start[(rgbp - 1) * 2 + 1];
            v = start[(rgbp - 1) * 2 + 3];
        }

        c = y - 16;
        d = u - 128;
        e = v - 128;

        r = ((298 * c + 409 * e + 128) >> 8);
        if (r < 0) r = 0;
        if (r > 0xff) r = 0xff;
        g = ((298 * c - 100 * d - 208 * e + 128) >> 8);
        if (g < 0) g = 0;
        if (g > 0xff) g = 0xff;
        b = ((298 * c + 516 * d + 128) >> 8);
        if (b < 0) b = 0;
        if (b > 0xff) b = 0xff;
        timecube[(ring_index * frame_size) + rgbp] = (r << 16) + (g << 8) + b;
    }
}


}

int grab_frame() {
    struct v4l2_buffer buf;

    CLEAR (buf);
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (-1 == xioctl (fd, VIDIOC_DQBUF, &buf)) {
        switch (errno) {
            case EAGAIN:
                return -1;
			case EIO:
			default:
				fatal("VIDIOC_DQBUF");
		}
	}

    if (buf.index >= n_buffers)
        fatal("Invalid buffer index");

    save_frame_color(buf.index);

    //printf("Enqueuing buffer %d\n", buf.index);
	if (-1 == xioctl (fd, VIDIOC_QBUF, &buf))
	    fatal("VIDIOC_QBUF");

    return 0;
}

void push_frame() {
    printf(".\n");
    Window _dw;
    int _d, _w, _h;
    XGetGeometry(dpy, window, &_dw, &_d, &_d, &_w, &_h, &_d, &_d);

    XvShmPutImage(dpy, xv_port, window, gc, image,
        0, 0, image->width, image->height,
		0, 0, _w, _h, True);
}

#define index_of(x,y,t) (((t) * frame_size) + ((y) * width) + x)


void main_loop() {
    XEvent event;

    debug("*** PRESS ANY KEY TO EXIT ***\n");
    int buf_index;

int duration = 640;

    frame_size = width * height;
    tc_size = frame_size * duration;
    int x, y;
    fd_set fds;
    struct timeval tv;
    int r;
    int mode = 0;
    int grab = 1;
    int hud = 1;

    timecube = malloc(tc_size * 4);

    while (1) {
        /* Wait for new frame to become available */
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        tv.tv_sec  = 1;
        tv.tv_usec = 0;
        r = select(fd + 1, &fds, NULL, NULL, &tv);
        if (-1 == r) {
            if (EINTR == errno)
                continue;
            fatal("Select");
        }
        if (0 == r) {
            printf("Select timeout\n");
        }

        /* Do the grab */
        if (grab)
            grab_frame();

        int x1 = 3 * width/4;
        int y1 = 3 * height/4;

        /* Create image from timecube */
        int index;
        for (y = 0; y < height; y++) {
            for (x=0; x < width; x++) {
                 // Cheesy picture-in-picture
                if (hud && x > x1 && y > y1) {
                    index = ((y - y1) * 4 * width) + (x - x1) * 4 + (ring_index * frame_size);
                }
                else {
                    switch(mode % 6) {
                    case 0: 
                        index = index_of(x, y, ring_index - x) % tc_size; // horizontal slitscan
                        break;
                    case 1:
                        index = index_of(x, y, ring_index - (x/10 + y/10)) % tc_size; // diagonal slitscan
                        break;
                    case 2:
                        index = index_of(x, y, ring_index - y/10) % tc_size; // vertical slitscan
                        break;
                    case 3:
                        index = index_of(ring_index, y, x) % tc_size; // lardus effect
                        break;
                    case 4:
                        index = index_of(x, ring_index, y) % tc_size; // vertical lardus
                        break;
                    default:
                        index = index_of(x, y, ring_index); // identity
                    }
                }
                if (index < 0) {
                    index += tc_size;
                }
                *((int32_t *) ((char *) image->data + ((width * y * 4) + x * 4))) = timecube[index];
            }
        }

        /* Push it out to Xv */
        push_frame();

        if (grab) {
            ring_index++;
            if (ring_index == duration)
                ring_index = 0;
        }
        /* Check for keypress */
        while (XPending(dpy)) {
            XNextEvent(dpy, &event);
            if (event.type == KeyPress) {
                XKeyEvent *kev = (XKeyEvent *) &event;
                unsigned int keycode = kev->keycode;
                if (keycode == 9) // escape
                    return;
                if (keycode == 43) // h key
                    hud = !hud;
                if (keycode == 42) // g gkey
                    grab = !grab;
                if (keycode == 58) // M key
                    mode++;
                if (keycode == 41) // f key
                    toggle_fullscreen();
            }
        }
    }
}

void stop_capture() {
    enum v4l2_buf_type type;
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (-1 == xioctl (fd, VIDIOC_STREAMOFF, &type))
	    fatal("VIDIOC_STREAMOFF");
}

void shutdown_camera() {
    int i;
    for (i = 0; i < n_buffers; ++i)
	    if (-1 == munmap (buffers[i].start, buffers[i].length))
	        fatal("munmap");
}

void destroy_window() {

}

int main(int argc, char* argv[]) {
    open_device();
    init_camera();
    create_window();
    start_capture();
    // allocate buffers? prepare map?
    main_loop();
    stop_capture();
    destroy_window();
    shutdown_camera();
    close_device();
    return 0;
}
