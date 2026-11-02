/* Native X11 integration fixture for the verified desktop smoke session. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <Xm/MwmUtil.h>
#include <assert.h>
#include <stdio.h>
#include <time.h>
static int errors;
static int Error(Display *d, XErrorEvent *e)
{ (void)d; ++errors; fprintf(stderr,"X11 error %u\n",e->error_code); return 0; }
static int Managed(Display *d, Window w, Atom state)
{
    for (int attempt=0; attempt<100; ++attempt) {
        Atom type; int format; unsigned long count, remaining;
        unsigned char *data=NULL;
        int status=XGetWindowProperty(d,w,state,0,2,False,state,
                                     &type,&format,&count,&remaining,&data);
        int valid=status==Success && data && type==state && format==32 &&
                  count==2 && remaining==0;
        if (data) XFree(data);
        if (valid) return 1;
        struct timespec delay={0,100000000}; nanosleep(&delay,NULL);
    }
    return 0;
}
int main(void)
{
    Display *d=XOpenDisplay(NULL); assert(d); XSetErrorHandler(Error);
    Window root=DefaultRootWindow(d);
    Atom hints=XInternAtom(d,"_MOTIF_WM_HINTS",False);
    Atom state=XInternAtom(d,"WM_STATE",False);
    Atom utf8=XInternAtom(d,"UTF8_STRING",False);
    Atom name=XInternAtom(d,"_NET_WM_NAME",False);
    Atom colormaps=XInternAtom(d,"WM_COLORMAP_WINDOWS",False);
    for (int scenario=0; scenario<6; ++scenario) {
        Window w=XCreateSimpleWindow(d,root,60,60,240,120,0,0,0xffffff);
        XStoreName(d,w,"CDE property regression");
        unsigned long words[18]={0};
        if (scenario==0) {
            words[0]=MWM_HINTS_DECORATIONS; words[2]=MWM_DECOR_ALL;
            XChangeProperty(d,w,hints,hints,32,PropModeReplace,(unsigned char *)words,5);
        } else if (scenario==1) {
            unsigned char byte=0xff;
            XChangeProperty(d,w,hints,hints,8,PropModeReplace,&byte,1);
        } else if (scenario==2) {
            words[0]=MWM_HINTS_STATUS;
            XChangeProperty(d,w,hints,hints,32,PropModeReplace,(unsigned char *)words,1);
        } else if (scenario==3) {
            words[0]=PBaseSize;
            XChangeProperty(d,w,XA_WM_NORMAL_HINTS,XA_WM_SIZE_HINTS,
                            32,PropModeReplace,(unsigned char *)words,16);
        } else if (scenario==4) {
            unsigned short bad=0x41;
            XChangeProperty(d,w,name,utf8,16,PropModeReplace,(unsigned char *)&bad,1);
        } else {
            Window children[3];
            for (int i=0; i<3; ++i)
                children[i]=XCreateSimpleWindow(d,w,i*30,0,20,20,0,0,0);
            XChangeProperty(d,w,colormaps,XA_WINDOW,32,PropModeReplace,(unsigned char *)children,3);
        }
        XMapWindow(d,w); XFlush(d); assert(Managed(d,w,state));
        XDestroyWindow(d,w); XSync(d,False); assert(errors==0);
    }
    XCloseDisplay(d);
    puts("Native dtwm: valid/short/wrong-format hints, UTF-8 and colormap cases passed");
    return 0;
}
