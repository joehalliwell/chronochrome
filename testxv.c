
/* -------------------------------------------
 * ---		  XV Testcode		   ---
 * ---		    by AW		   ---*/


#include <stdlib.h>
#include <stdio.h>
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


extern int 	XShmQueryExtension(Display*);
extern int 	XShmGetEventBase(Display*);
extern XvImage  *XvShmCreateImage(Display*, XvPortID, int, char*, int, int, XShmSegmentInfo*);

void put_pixel(char* data, int x, int y, int maxx, int p) {
  data[y*maxx + x] = p;
}
	

int main (int argc, char* argv[]) {
  int		yuv_width = 1024;
  int		yuv_height = 768;
  
  int		xv_port = -1;
  int		adaptor, encodings, attributes, formats;
  int		i, j, ret, p, _d, _w, _h;
  long		secsb, secsa, frames;
  
  XvAdaptorInfo		*ai;
  XvEncodingInfo	*ei;
  XvAttribute		*at;
  XvImageFormatValues	*fo;

  XvImage		*yuv_image;

#define GUID_YUV12_PLANAR 0x32315659

  unsigned int		p_version, p_release,
  			p_request_base, p_event_base, p_error_base;
  int			p_num_adaptors;
   	
  Display		*dpy;
  Window		window, _dw;
  XSizeHints		hint;
  XSetWindowAttributes	xswa;
  XVisualInfo		vinfo;
  int			screen;
  unsigned long		mask;
  XEvent		event;
  GC			gc;

  /** for shm */
  int 			shmem_flag = 0;
  XShmSegmentInfo	yuv_shminfo;
  int			CompletionType;


  printf("starting up video testapp...\n\n");
  
  adaptor = -1;
	
  dpy = XOpenDisplay(NULL);
  if (dpy == NULL) {
    printf("Cannot open Display.\n");
    exit (-1);
  }
  
  screen = DefaultScreen(dpy);
  
  /** find best display */
  if (XMatchVisualInfo(dpy, screen, 24, TrueColor, &vinfo)) {
    printf(" found 24bit TrueColor\n");
  } else
    if (XMatchVisualInfo(dpy, screen, 16, TrueColor, &vinfo)) {
      printf(" found 16bit TrueColor\n");
    } else
      if (XMatchVisualInfo(dpy, screen, 15, TrueColor, &vinfo)) {
	printf(" found 15bit TrueColor\n");
      } else
  	if (XMatchVisualInfo(dpy, screen, 8, PseudoColor, &vinfo)) {
	  printf(" found 8bit PseudoColor\n");
  	} else
	  if (XMatchVisualInfo(dpy, screen, 8, GrayScale, &vinfo)) {
	    printf(" found 8bit GrayScale\n");
	  } else
	    if (XMatchVisualInfo(dpy, screen, 8, StaticGray, &vinfo)) {
	      printf(" found 8bit StaticGray\n");
	    } else
	      if (XMatchVisualInfo(dpy, screen, 1, StaticGray, &vinfo)) {
  		printf(" found 1bit StaticGray\n");
	      } else {
  		printf("requires 16 bit display\n");
  		exit (-1);
	      }
  
  CompletionType = -1;	
  
  hint.x = 1;
  hint.y = 1;
  hint.width = yuv_width;
  hint.height = yuv_height;
  hint.flags = PPosition | PSize;
  
  xswa.colormap =  XCreateColormap(dpy, DefaultRootWindow(dpy), vinfo.visual, AllocNone);
  xswa.event_mask = StructureNotifyMask | ExposureMask;
  xswa.background_pixel = 0;
  xswa.border_pixel = 0;
  
  mask = CWBackPixel | CWBorderPixel | CWColormap | CWEventMask;
  
  window = XCreateWindow(dpy, DefaultRootWindow(dpy),
			 0, 0,
			 yuv_width,
			 yuv_height,
			 0, vinfo.depth,
			 InputOutput,
			 vinfo.visual,
			 mask, &xswa);
  
  XStoreName(dpy, window, "XV");
  XSetIconName(dpy, window, "XV");
  
  XSelectInput(dpy, window, StructureNotifyMask);
  
  /** Map window */
  XMapWindow(dpy, window);
  
  /** Wait for map. */
  do {
    XNextEvent(dpy, &event);
  }
  while (event.type != MapNotify || event.xmap.event != window);
  
  if (XShmQueryExtension(dpy)) shmem_flag = 1;
  if (!shmem_flag) {
    printf("no shmem available.\n");
    exit (-1);
  }
  
  if (shmem_flag==1) CompletionType = XShmGetEventBase(dpy) + ShmCompletion;
  
  
  /**--------------------------------- XV ------------------------------------*/
  printf("beginning to parse the Xvideo extension...\n\n");
  
  /** query and print Xvideo properties */
  ret = XvQueryExtension(dpy, &p_version, &p_release, &p_request_base,
			 &p_event_base, &p_error_base);
  if (ret != Success) {
    if (ret == XvBadExtension)
      printf("XvBadExtension returned at XvQueryExtension.\n");
    else
      if (ret == XvBadAlloc)
	printf("XvBadAlloc returned at XvQueryExtension.\n");
      else
	printf("other error happened at XvQueryExtension.\n");
  }
  printf("========================================\n");
  printf("XvQueryExtension returned the following:\n");
  printf("p_version      : %u\n", p_version);
  printf("p_release      : %u\n", p_release);
  printf("p_request_base : %u\n", p_request_base);
  printf("p_event_base   : %u\n", p_event_base);
  printf("p_error_base   : %u\n", p_error_base);
  printf("========================================\n");
  
  ret = XvQueryAdaptors(dpy, DefaultRootWindow(dpy),
			&p_num_adaptors, &ai);
  
  if (ret != Success) {
    if (ret == XvBadExtension)
      printf("XvBadExtension returned at XvQueryExtension.\n");
    else
      if (ret == XvBadAlloc)
	printf("XvBadAlloc returned at XvQueryExtension.\n");
      else
	printf("other error happaned at XvQueryAdaptors.\n");
  }
  printf("=======================================\n");
  printf("XvQueryAdaptors returned the following:\n");
  printf("%d adaptors available.\n", p_num_adaptors);
  for (i = 0; i < p_num_adaptors; i++) {
    printf(" name:        %s\n"
	   " type:        %s%s%s%s%s\n"
	   " ports:       %ld\n"
	   " first port:  %ld\n",
	   ai[i].name,
	   (ai[i].type & XvInputMask)	? "input | "	: "",
	   (ai[i].type & XvOutputMask)	? "output | "	: "",
	   (ai[i].type & XvVideoMask)	? "video | "	: "",
	   (ai[i].type & XvStillMask)	? "still | "	: "",
	   (ai[i].type & XvImageMask)	? "image | "	: "",
	   ai[i].num_ports,
	   ai[i].base_id);
    xv_port = ai[i].base_id;
    
    printf("adaptor %d ; format list:\n", i);
    for (j = 0; j < ai[i].num_formats; j++) {
      printf(" depth=%d, visual=%ld\n",
	     ai[i].formats[j].depth,
	     ai[i].formats[j].visual_id);
    }
    for (p = ai[i].base_id; p < ai[i].base_id+ai[i].num_ports; p++) {
      
      printf(" encoding list for port %d\n", p);
      if (XvQueryEncodings(dpy, p, &encodings, &ei) != Success) {
	printf("XvQueryEncodings failed.\n");
	continue;
      }
      for (j = 0; j < encodings; j++) {
	printf("  id=%ld, name=%s, size=%ldx%ld, numerator=%d, denominator=%d\n",
	       ei[j].encoding_id, ei[j].name, ei[j].width, ei[j].height,
	       ei[j].rate.numerator, ei[j].rate.denominator);
      }
      XvFreeEncodingInfo(ei);
      
      printf(" attribute list for port %d\n", p);
      at = XvQueryPortAttributes(dpy, p, &attributes);
      for (j = 0; j < attributes; j++) {
	printf("  name:       %s\n"
	       "  flags:     %s%s\n"
	       "  min_color:  %i\n"
	       "  max_color:  %i\n",
	       at[j].name,
	       (at[j].flags & XvGettable) ? " get" : "",
	       (at[j].flags & XvSettable) ? " set" : "",						
	       at[j].min_value, at[j].max_value);
      }
      if (at)
	XFree(at);
      
      printf(" image format list for port %d\n", p);
      fo = XvListImageFormats(dpy, p, &formats);
      for (j = 0; j < formats; j++) {
	printf("  0x%x (%4.4s) %s\n",
	       fo[j].id,
	       (char *)&fo[j].id,
	       (fo[j].format == XvPacked) ? "packed" : "planar");
      }
      if (fo)
	XFree(fo);
    }
    printf("\n");
  }
  if (p_num_adaptors > 0)
    XvFreeAdaptorInfo(ai);
  if (xv_port == -1)
    exit (0);
  
  gc = XCreateGC(dpy, window, 0, 0);		
  
  yuv_image = XvShmCreateImage(dpy, xv_port, GUID_YUV12_PLANAR, 0, yuv_width, yuv_height, &yuv_shminfo);
  yuv_shminfo.shmid = shmget(IPC_PRIVATE, yuv_image->data_size, IPC_CREAT | 0777);
  yuv_shminfo.shmaddr = yuv_image->data = shmat(yuv_shminfo.shmid, 0, 0);
  yuv_shminfo.readOnly = False;
  
  if (!XShmAttach(dpy, &yuv_shminfo)) {
    printf("XShmAttach failed !\n");
    exit (-1);
  }
  
  for (i = 0; i < yuv_image->height; i++) {
    for (j = 0; j < yuv_image->width; j++) {
      yuv_image->data[yuv_image->width*i + j] = i*j;
    }
  }
  
  printf("%d\n", yuv_image->data_size);
  int joe = 0;
  while (1) {
    frames = secsa = secsb = 0;
    time(&secsa);
    while (frames < 200) {	
        XGetGeometry(dpy, window, &_dw, &_d, &_d, &_w, &_h, &_d, &_d);
        for (i = 0; i < yuv_image->height * 1.5; i++) {
            for (j = 0; j < yuv_image->width; j++) {
                yuv_image->data[yuv_image->width*i + j] = (i + j + joe / 5);
            }
        }
   
      XvShmPutImage(dpy, xv_port, window, gc, yuv_image,
		    0, 0, yuv_image->width, yuv_image->height,
		    0, 0, _w, _h, True);
      
      /* XFlush(dpy); */
      joe++;
      frames++;
    }
    time(&secsb);
    printf("%ld frames in %ld seconds; %.4f fps\n", frames, secsb-secsa, (double) frames/(secsb-secsa));
  }
  
  return 0;
}
