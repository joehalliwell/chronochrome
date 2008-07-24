/*
 * gcc -o slitscan slitscan.c  -L/usr/X11R6/lib -lX11 -lXext -lXv
 *
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

#define XV_IMAGE_FORMAT 0x32595559


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
    xv_port = ai[0].base_id;
    printf("xv_port: %d\n", xv_port);

    gc = XCreateGC(dpy, window, 0, 0);

    image = XvShmCreateImage(dpy, xv_port, XV_IMAGE_FORMAT, 0, width, height, &shminfo);
    shminfo.shmid = shmget(IPC_PRIVATE, image->data_size, IPC_CREAT | 0777);
    shminfo.shmaddr = image->data = shmat(shminfo.shmid, 0, 0);
    shminfo.readOnly = False;

    if (!XShmAttach(dpy, &shminfo))
        fatal("XShmAttach failed !\n");

    XvShmPutImage(dpy, xv_port, window, gc, image,
        0, 0, image->width, image->height,
        0, 0, width, height, True);

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

void grab_frame() {
    struct v4l2_buffer buf;

    CLEAR (buf);
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (-1 == xioctl (fd, VIDIOC_DQBUF, &buf)) {
        switch (errno) {
            case EAGAIN:
                return;
			case EIO:
			default:
				fatal("VIDIOC_DQBUF");
		}
	}

    if (buf.index >= n_buffers)
        fatal("buffer index");

    //process_frame
    //

    int i, j;
    for (i = 0; i < image->height * 2; i++) {
        for (j = 0; j < image->width; j++) {
            image->data[image->width*i + j] = ((char *)buffers[buf.index].start)[width * i + j];
        }
    }

	if (-1 == xioctl (fd, VIDIOC_QBUF, &buf))
	    fatal("VIDIOC_QBUF");

    return;
}

void push_frame(int foo) {
    printf(".\n");
    Window _dw;
    int _d, _w, _h;
    XGetGeometry(dpy, window, &_dw, &_d, &_d, &_w, &_h, &_d, &_d);

    XvShmPutImage(dpy, xv_port, window, gc, image,
        0, 0, image->width, image->height,
		0, 0, _w, _h, True);

}


void main_loop() {
    XEvent event;

    debug("*** PRESS ANY KEY TO EXIT ***\n");
    int buf_index;

    while (1) {
        grab_frame();
        push_frame(buf_index);
        XNextEvent(dpy, &event);
        if (event.type == KeyPress) {
            break;
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
