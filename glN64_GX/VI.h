#ifndef VI_H
#define VI_H
#include "Types.h"

struct VIInfo
{
	u32 width, height;
	u32 lastOrigin;
#ifdef __GX__
	unsigned int* xfb[2];
	int which_fb;
	bool updateOSD;
	bool copy_fb;
	bool doFrameBufferRender;
#endif // __GX__
};

extern VIInfo VI;

void VI_UpdateSize();
void VI_UpdateScreen();

#ifdef __GX__

void VI_GX_init();
void VI_GX_setFB(unsigned int* fb1, unsigned int* fb2);
unsigned int* VI_GX_getScreenPointer();
void VI_GX_showFPS();
void VI_GX_showLoadProg(float percent);
void VI_GX_updateDEBUG();
void VI_GX_showDEBUG();
void VI_GX_showStats();
void VI_GX_cleanUp();
void VI_GX_PreRetraceCallback(u32 retraceCnt);

#endif // __GX__

#endif

