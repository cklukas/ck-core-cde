#ifndef DSDM_H
#define DSDM_H

#include <X11/Xlib.h>

unsigned char *GetInterestProperty(Display *dpy, Window win,
                                   unsigned long *nitems);
void ProxyInit(Display *dpy, Window dsdm_win);
void ProxyMain(Display *dpy, XEvent *event);

#endif /* DSDM_H */
