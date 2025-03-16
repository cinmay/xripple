/*****************************************************************************/
/* This Code is Copyright (C) 1996 by the Rasterman ...... It's small, simple*/
/* and does the job well... read the REAME file!!!!!                         */
/*****************************************************************************/

#include <stdio.h>
#include <stdlib.h>    // for exit and free
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <math.h>

/* DEFINES */
#define M_PI_2          1.57079632679489661923

Display *disp;
Window Root; 
Visual *vis; 
Pixmap pmap;
Pixmap pmap_buff;
XImage *xim;
int scr; 
GC gc; 
unsigned long wht; 
unsigned long blk; 
int width;
int height;
int waterh;
int depth;

// Forward declarations
int GetWinWidth();
int GetWinHeight();

void OpenWindow(int h)
{
  XGCValues gcvals;
  int i;

  disp = XOpenDisplay(NULL);
  if (disp) {
      Root = DefaultRootWindow(disp);
      scr = DefaultScreen(disp);
      blk = BlackPixel(disp, scr);
      wht = WhitePixel(disp, scr);
      vis = DefaultVisual(disp, scr);
      depth = DefaultDepth(disp, scr);
      gc = XCreateGC(disp, Root, 0, &gcvals);
      XSetForeground(disp, gc, blk);
      XSetBackground(disp, gc, wht);
      width = GetWinWidth();
      height = GetWinHeight();
      pmap = XCreatePixmap(disp, Root, width, h, depth);
      pmap_buff = XCreatePixmap(disp, Root, width, h * 2, depth);
  } else {
      printf("Cannot open display\n");
      exit(1);
  }
}

int GetWinWidth()
{
  XWindowAttributes wa;
  XGetWindowAttributes(disp, Root, &wa);
  return wa.width;
}

int GetWinHeight()
{
  XWindowAttributes wa;
  XGetWindowAttributes(disp, Root, &wa);
  return wa.height;
}

void doit()
{
  int y;
  int yy;
  int off;
  int yoff;
  double a;
  double incv;
  double inch;
  double aa;
  double p;
  int i;

  inch = 0;
  incv = 0;
  i = 0;

  for (;;) {
      xim = XGetImage(disp, Root, 0, (height - (waterh * 3)) + i, width, 1, 0xffffffff, ZPixmap);
      XPutImage(disp, pmap_buff, gc, xim, 0, 0, 0, i++, width, 1);
      if (xim->data) free(xim->data);
      if (xim->obdata) free(xim->obdata);
      if (xim) free(xim);
      i = i % (waterh * 2);
      incv += 0.09;
      if (incv > (M_PI_2 * 4)) {
          incv = 0;
      }
      inch += 0.06;
      if (inch > (M_PI_2 * 4)) {
          inch = 0;
      }
      for (y = 0; y < waterh; y++) {
          p = ((double)(waterh - y)) / ((double)waterh);
          a = p * p * 48 + incv;
          yoff = y + (int)(sin(a) * 7) + 1;
          yy = (waterh * 2) - yoff;
          aa = p * p * 64 + inch;
          off = (int)(sin(aa) * 10 * (1 - p));
          XCopyArea(disp, pmap_buff, pmap, gc, 0, yy, width, 1, off, y);
      }
      XCopyArea(disp, pmap, Root, gc, 0, 0, width, waterh, 0, height - waterh);
      XSync(disp, False);
  }
}

int main(int argc, char **argv)
{  
  waterh = 255;
  OpenWindow(waterh);
  doit();
  return 0;
}
