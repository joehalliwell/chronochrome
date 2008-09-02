/*
 * gcc -o slitscan slitscan.c  -L/usr/X11R6/lib -lX11 -lXext -lXv -lfreeimage -O3
 * TODO:
 * - FPS monitoring
 * - Break into separate files (main, xv, camera)
 * - Add frame dumping (BMP?)
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

#include <FreeImage.h>

extern XvImage  *XvShmCreateImage(Display*, XvPortID, int, char*, int, int, XShmSegmentInfo*);

#define CLEAR(x) memset (&(x), 0, sizeof (x))

//#define XV_IMAGE_FORMAT 0x3
#define XV_IMAGE_FORMAT 0x59565955 

// FOURCCs I HAVE KNOWN AND LOVED
// RGB  0x3
// YUY2 0x32595559 Packed
// YV12 0x32315659
// I420 0x30323449
// UYVY 0x59565955 Packed (YVYU, surely?)

// 320x240
// 640x480
// 960x720
#define WIDTH 640
#define HEIGHT 480
#define NUM_FRAMES ((WIDTH<HEIGHT)?HEIGHT:WIDTH)
#define BPP 2
#define FRAME_SIZE ((WIDTH * HEIGHT * BPP))
#define TC_SIZE (FRAME_SIZE * NUM_FRAMES)
#define NUM_MODES 10

unsigned char *timecube;
int ring_index = -1;

char *video_dev = "/dev/video0";
int xv_port = -1;

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
XShmSegmentInfo shminfo;


void fatal(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    printf("\n\nFatal error: ");
    vprintf(format, ap);
    printf("\n");
    va_end(ap);
    exit(-1);
}

void debug(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vprintf(format, ap);
    va_end(ap);
    printf("\n");
}

enum
{
  _NET_WM_STATE_REMOVE =0,
  _NET_WM_STATE_ADD = 1,
  _NET_WM_STATE_TOGGLE =2

};

void toggle_fullscreen() {
    static int fullscreen = 0;
    fullscreen = !fullscreen;
    debug("Switching to %s mode", fullscreen?"fullscreen":"windowed");
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

int get_xv_port() {

    XvAdaptorInfo		*ai;
    XvImageFormatValues *ifv;

    unsigned int num_adaptors;
    int num_formats;
    int i, p, j;
    int ret;

    ret = XvQueryAdaptors(dpy, DefaultRootWindow(dpy), &num_adaptors, &ai);
    if (ret != Success)
        fatal("Query adaptors failed");

    debug("Found %d adaptors.", num_adaptors);
    for (i = 0; i < num_adaptors && xv_port == -1 ; i++) {
        for (p = ai[i].base_id; p < ai[i].base_id + ai[i].num_ports && xv_port == -1; p++) {
            ifv = XvListImageFormats(dpy, p, &num_formats);
            for (j = 0; j < num_formats; j++) {
                if (ifv[j].id == XV_IMAGE_FORMAT) {
                    xv_port = p;
                    break;
                }
            }
            XFree(ifv);
        }
    }
    XvFreeAdaptorInfo(ai);
    if (xv_port == -1)
        fatal("Could not locate suitable Xv port.");
    else
        debug("Using Xv port at %d.", xv_port);
    return xv_port;
}

int create_window() {
    dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        fatal("Cannot open Display.");
    }

    int screen = DefaultScreen(dpy);
    XVisualInfo vinfo;

    if (!XMatchVisualInfo(dpy, screen, 24, TrueColor, &vinfo)) {
        fatal("Could not get 24-bit display.\n");
    }

    XSetWindowAttributes xswa = {
        .colormap =  XCreateColormap(dpy, DefaultRootWindow(dpy), vinfo.visual, AllocNone),
        .event_mask = StructureNotifyMask | ExposureMask | KeyPressMask,
        .background_pixel = 0,
        .border_pixel = 0
    };

    unsigned long mask = CWBackPixel | CWBorderPixel | CWColormap | CWEventMask;

    window = XCreateWindow(dpy, DefaultRootWindow(dpy),
			 0, 0,
			 WIDTH,
			 HEIGHT,
			 0, vinfo.depth,
			 InputOutput,
			 vinfo.visual,
			 mask, &xswa);

    XStoreName(dpy, window, "C H R O N O C H R O M E");
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

    int xv_port = get_xv_port();

    gc = XCreateGC(dpy, window, 0, 0);

    image = XvShmCreateImage(dpy, xv_port, XV_IMAGE_FORMAT, 0, WIDTH, HEIGHT, &shminfo);

    shminfo.shmid = shmget(IPC_PRIVATE, image->data_size, IPC_CREAT | 0777);
    shminfo.shmaddr = image->data = shmat(shminfo.shmid, 0, 0);
    shminfo.readOnly = False;

    if (!XShmAttach(dpy, &shminfo))
        fatal("XShmAttach failed !\n");

    printf("Attempting to write to SHM\n");

    XvShmPutImage(dpy, xv_port, window, gc, image,
        0, 0, image->width, image->height,
        0, 0, WIDTH, HEIGHT, True);
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
    fmt.fmt.pix.width       = WIDTH;
    fmt.fmt.pix.height      = HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field       = V4L2_FIELD_INTERLACED;

    if (-1 == xioctl (fd, VIDIOC_S_FMT, &fmt))
        fatal("VIDIOC_S_FMT");

    /* Note VIDIOC_S_FMT may change width and height. */
    if (fmt.fmt.pix.width != WIDTH)
        fatal("Unexpected width %d", fmt.fmt.pix.width);
    if (fmt.fmt.pix.height != HEIGHT)
        fatal("Unexpected height %d", fmt.fmt.pix.height);

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

void save_frame_color(int buf_index) {
{
    ring_index++;
    if (ring_index >= NUM_FRAMES) {
        ring_index = 0;
    }
    memcpy(timecube + (ring_index * FRAME_SIZE), buffers[buf_index].start, FRAME_SIZE);
    return;
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
    Window _win;
    int _i;
    unsigned int _ui;
    unsigned int w, h;
    XGetGeometry(dpy, window, &_win, &_i, &_i, &w, &h, &_ui, &_ui);

    XvShmPutImage(dpy, xv_port, window, gc, image,
        0, 0, image->width, image->height,
		0, 0, w, h, True);
}

void print_help() {
    char *help_text = \
        "  q    Exit\n"
        "  f    Toggle fullscreen\n"
        "  g    Toggle framegrab\n"
        "  p    Toggle animation timer\n"
        "  h    Toggle HUD\n"
        "  l    Toggle logo\n"
        "  m,n  Switch mode (up, down)\n"
        "  +,-  Adjust timer speed (faster, slower)\n";
    printf("Commands:\n%s", help_text);
}

#define index_of(x,y,t) ((((t) % NUM_FRAMES) * FRAME_SIZE) + ((y) * (WIDTH) * BPP) + (WIDTH-x) * BPP)


void main_loop() {
    XEvent event;

    int x, y;
    fd_set fds;
    struct timeval tv;
    int r;
    int mode = 4;
    int grab = 1;
    int hud = 0;
    int pause = 0;
    int delta = 1;
    int die = 0;
    int time = ring_index;
    int show_logo = 1;

    /* Clear buffer to UYVY black */
    for (x = 0; x<TC_SIZE; x+=4) {
        *((int *) ((char *) (timecube + x))) = 0x80008000; //Black
    }

    /* Prepare the logo */
    FIBITMAP *bmp = FreeImage_Load(FIF_BMP, "logo.bmp", BMP_DEFAULT);
    int logo_w = FreeImage_GetWidth(bmp);
    int logo_h = FreeImage_GetHeight(bmp);
    unsigned char *logo = malloc(logo_w*logo_h);
    RGBQUAD value;
    for (y = 0; y < logo_h; y++) {
        for (x = 0; x < logo_w; x++) {
            FreeImage_GetPixelColor(bmp, x, y, &value);
            logo[(logo_h-1-y)*logo_w + x] = value.rgbRed;
        }
    }
    FreeImage_Unload(bmp);

    bmp = FreeImage_Load(FIF_BMP, "map.bmp", BMP_DEFAULT);
    if (WIDTH != FreeImage_GetWidth(bmp) || HEIGHT != FreeImage_GetHeight(bmp))
        fatal("Map has wrong size");
    int *map = malloc(WIDTH * HEIGHT * sizeof(int)); 
    for (y = 0; y < HEIGHT; y++) {
        for (x = 0; x < WIDTH; x++) {
            FreeImage_GetPixelColor(bmp, x, y, &value);
            map[(HEIGHT - 1 - y)*WIDTH + x] = ((value.rgbRed >> 1) << 2) + ((value.rgbGreen >> 1)<<1) + (value.rgbBlue >> 1);
        }
    }
    FreeImage_Unload(bmp);

    while (!die) {
        /* Wait for new frame to become available */
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        tv.tv_sec  = 1;
        tv.tv_usec = 0;
        r = select(fd + 1, &fds, NULL, NULL, &tv);
        if (-1 == r) {
            if (EINTR == errno)
                continue;
            fatal("Select failed");
        }
        if (0 == r) {
            debug("Select timeout");
            continue;
        }

        /* Do the grab */
        if (grab)
            grab_frame();

        int x1 = 3 * WIDTH/4;
        int y1 = 3 * HEIGHT/4;

        if (!pause) {
            time += delta;
        }

        int xb_time = (time%(2*WIDTH - 4))<WIDTH?(time%(2*WIDTH - 4))+1:2*WIDTH - 4 - (time%(2*WIDTH - 4))+1;
        //debug("%d - %d", time, xb_time);

        /* Create image from timecube */
        int index;
        for (y = 0; y < HEIGHT; y++) {
            for (x=0; x < WIDTH; x++) {
                 // Cheesy picture-in-picture
                if (hud && x > x1 && y > y1) {
                    index = ((y - y1) * 4 * BPP * WIDTH) + (x - x1) * 4 *  BPP + (ring_index * FRAME_SIZE);
                }
                else {
                    switch(mode) {
                    case 1:
                        index = index_of(x, y, time - (x/10)); // rapid horizontal slitscan
                        break;
                    case 2:
                        index = index_of(x, y, time - (x/10 + y/10)); // rapid diagonal slitscan
                        break;
                    case 3:
                        index = index_of(x, y, time - y/10); // rapid vertical slitscan
                        break;
                    case 4:
                        index = index_of(xb_time, y, ring_index + 1 + x); // lardus effect
                        break;
                    case 5:
                        index = index_of(x, time, ring_index + 1 + y); // vertical lardus
                        break;
                    case 6:
                        index = index_of(x, y, time - x); // horizontal slitscan
                        break;
                    case 7:
                        index = index_of(x, y, time - (x + y)); // diagonal slitscan
                        break;
                    case 8:
                        index = index_of(x, y, time - y); // vertical slitscan
                        break;
                    case 9:
                        index = index_of(x, y, (time - map[y * WIDTH + x])); // displacement map
                        break;
                    case 0:
                    default:
                        index = index_of(x, y, time); // identity
                        break;
                    }
                }
                if (index < 0) {
                    index = (index % TC_SIZE) + TC_SIZE;
                }

                /* In packed formats, there are two types of pixel U and V -- so source and dest either match or don't...*/
                /* Also XV and V4L modes have different byte orders within the macro pixel */

                if ((x % 2) == ((index/2) % 2)) {
                    *(((char *) image->data + ((WIDTH * y * BPP) + x * BPP ))) = timecube[(index + 1) % TC_SIZE];
                    *(((char *) image->data + ((WIDTH * y * BPP) + x * BPP + 1))) = timecube[index % TC_SIZE];
                }
                else {
                    *(((char *) image->data + ((WIDTH * y * BPP) + x * BPP ))) = timecube[(index + 3) % TC_SIZE];
                    *(((char *) image->data + ((WIDTH * y * BPP) + x * BPP + 1))) = timecube[index % TC_SIZE];
                }
            }
        }
        if (show_logo) {
            /* Composite logo */
            for (y = 0; y<logo_h; y++) {
                for (x = 0; x<logo_w; x++) {
                    r =  *(((unsigned char *) image -> data + ((WIDTH * (y + 5) * BPP) + (WIDTH - 5 - logo_w + x) * BPP + 1)));
                    r += logo[y * logo_w + x]/2;
                    *(((unsigned char *) image -> data + ((WIDTH * (y + 5) * BPP) + (WIDTH - 5 - logo_w + x) * BPP + 1))) = r>0xff?0xff:r;
                }
            }
        }
        /* Push it out to Xv */
        push_frame();


        /* Check for keypress */
        while (XPending(dpy)) {
            XNextEvent(dpy, &event);
            if (event.type != KeyPress)
                continue;
            XKeyEvent *kev = (XKeyEvent *) &event;
            unsigned int keycode = kev->keycode;
            switch (keycode) {
                /* escape/q */
                case 24:
                case 9:
                    debug("Bye!");
                    die = 1;
                    break;
                /* p */
                case 33:
                    pause = ! pause;
                    debug("Animation timer %s", pause?"paused":"running");
                    if (!pause) {
                        time = ring_index;
                    }
                    break;
                /* h */
                case 43:
                    hud = ! hud;
                    debug("HUD %s", hud?"enabled":"disabled");
                    break;
                /* g */
                case 42:
                    grab = ! grab;
                    debug("Grab %s", grab?"enabled":"disabled");
                    if (grab)
                        time = ring_index;
                    break;
                /* l */
                case 46:
                    show_logo = ! show_logo;
                    break;
                /* m */
                case 58:
                    mode = (mode + 1) % NUM_MODES;
                    debug("Switching to mode %d", mode);
                    break;
                /* n */
                case 57:
                    mode = mode - 1;
                    if (mode < 0)
                        mode = NUM_MODES - 1;
                    debug("Switching to mode %d", mode);
                    break;
                /* f */
                case 41:
                    toggle_fullscreen();
                    break;
                /* + */
                case 21:
                    delta++;
                    debug("Delta %d", delta);
                    break;
                /* - */
                case 20:
                    delta--;
                    debug("Delta %d", delta);
                    break;
                /* i */
                case 31:
                    debug("Time: %d, Index: %d", time, ring_index);
                    break;
                default:
                    debug("Invalid keypress %d", keycode);
                    print_help();
            }
        }
    }
    free(logo);
    free(map);
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
    print_help();
    timecube = malloc(TC_SIZE);
    if (timecube == NULL)
        fatal("Could not allocate memory for timecube");
    main_loop();
    free(timecube);
    stop_capture();
    destroy_window();
    shutdown_camera();
    close_device();
    return 0;
}
