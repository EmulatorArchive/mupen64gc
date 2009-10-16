#include "MenuContext.h"
#include "LoadRomFrame.h"
#include "../libgui/Button.h"
#include "../libgui/resources.h"
#include "../libgui/FocusManager.h"
#include "../libgui/CursorManager.h"

extern "C" {
#include "../fileBrowser/fileBrowser.h"
#include "../fileBrowser/fileBrowser-libfat.h"
#include "../fileBrowser/fileBrowser-DVD.h"
#include "../fileBrowser/fileBrowser-CARD.h"
#ifdef WII
#include "../fileBrowser/fileBrowser-WiiFS.h"
#endif
}

void Func_LoadFromSD();
void Func_LoadFromDVD();
void Func_LoadFromFS();
void Func_ReturnFromLoadRomFrame();

#define NUM_FRAME_BUTTONS 3
#define FRAME_BUTTONS loadRomFrameButtons
#define FRAME_STRINGS loadRomFrameStrings

static char FRAME_STRINGS[3][25] =
	{ "Load from SD",
	  "Load from DVD",
	  "Load from Wii Filesystem"};

struct ButtonInfo
{
	menu::Button	*button;
	char*			buttonString;
	float			x;
	float			y;
	float			width;
	float			height;
	int				focusUp;
	int				focusDown;
	int				focusLeft;
	int				focusRight;
	ButtonFunc		clickedFunc;
	ButtonFunc		returnFunc;
} FRAME_BUTTONS[NUM_FRAME_BUTTONS] =
{ //	button	buttonString		x		y		width	height	Up	Dwn	Lft	Rt	clickFunc			returnFunc
	{	NULL,	FRAME_STRINGS[0],	150.0,	100.0,	340.0,	40.0,	 2,	 1,	-1,	-1,	Func_LoadFromSD,	Func_ReturnFromLoadRomFrame }, // Load From SD
	{	NULL,	FRAME_STRINGS[1],	150.0,	200.0,	340.0,	40.0,	 0,	 2,	-1,	-1,	Func_LoadFromDVD,	Func_ReturnFromLoadRomFrame }, // Load From DVD
	{	NULL,	FRAME_STRINGS[2],	150.0,	300.0,	340.0,	40.0,	 1,	 0,	-1,	-1,	Func_LoadFromFS,	Func_ReturnFromLoadRomFrame }, // Load From Wii FS
};

LoadRomFrame::LoadRomFrame()
{
	buttonImage = new menu::Image(ButtonTexture, 16, 16, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	buttonFocusImage = new menu::Image(ButtonFocusTexture, 16, 16, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	for (int i = 0; i < NUM_FRAME_BUTTONS; i++)
		FRAME_BUTTONS[i].button = new menu::Button(buttonImage, &FRAME_BUTTONS[i].buttonString, 
										FRAME_BUTTONS[i].x, FRAME_BUTTONS[i].y, 
										FRAME_BUTTONS[i].width, FRAME_BUTTONS[i].height);

	for (int i = 0; i < NUM_FRAME_BUTTONS; i++)
	{
		FRAME_BUTTONS[i].button->setFocusImage(buttonFocusImage);
		if (FRAME_BUTTONS[i].focusUp != -1) FRAME_BUTTONS[i].button->setNextFocus(menu::Focus::DIRECTION_UP, FRAME_BUTTONS[FRAME_BUTTONS[i].focusUp].button);
		if (FRAME_BUTTONS[i].focusDown != -1) FRAME_BUTTONS[i].button->setNextFocus(menu::Focus::DIRECTION_DOWN, FRAME_BUTTONS[FRAME_BUTTONS[i].focusDown].button);
		if (FRAME_BUTTONS[i].focusLeft != -1) FRAME_BUTTONS[i].button->setNextFocus(menu::Focus::DIRECTION_LEFT, FRAME_BUTTONS[FRAME_BUTTONS[i].focusLeft].button);
		if (FRAME_BUTTONS[i].focusRight != -1) FRAME_BUTTONS[i].button->setNextFocus(menu::Focus::DIRECTION_RIGHT, FRAME_BUTTONS[FRAME_BUTTONS[i].focusRight].button);
		FRAME_BUTTONS[i].button->setActive(true);
		if (FRAME_BUTTONS[i].clickedFunc) FRAME_BUTTONS[i].button->setClicked(FRAME_BUTTONS[i].clickedFunc);
		if (FRAME_BUTTONS[i].returnFunc) FRAME_BUTTONS[i].button->setReturn(FRAME_BUTTONS[i].returnFunc);
		add(FRAME_BUTTONS[i].button);
		menu::Cursor::getInstance().addComponent(this, FRAME_BUTTONS[i].button, FRAME_BUTTONS[i].x, 
												FRAME_BUTTONS[i].x+FRAME_BUTTONS[i].width, FRAME_BUTTONS[i].y, 
												FRAME_BUTTONS[i].y+FRAME_BUTTONS[i].height);
	}
	setDefaultFocus(FRAME_BUTTONS[0].button);
	setEnabled(true);

}

LoadRomFrame::~LoadRomFrame()
{
	for (int i = 0; i < NUM_FRAME_BUTTONS; i++)
	{
		menu::Cursor::getInstance().removeComponent(this, FRAME_BUTTONS[i].button);
		delete FRAME_BUTTONS[i].button;
	}
	delete buttonFocusImage;
	delete buttonImage;

}

extern MenuContext *pMenuContext;
extern void fileBrowserFrame_OpenDirectory(fileBrowser_file* dir);

void Func_LoadFromSD()
{
	// Deinit any existing romFile state
	if(romFile_deinit) romFile_deinit( romFile_topLevel );
	// Change all the romFile pointers
	romFile_topLevel = &topLevel_libfat_Default;
	romFile_readDir  = fileBrowser_libfat_readDir;
	romFile_readFile = fileBrowser_libfatROM_readFile;
	romFile_seekFile = fileBrowser_libfat_seekFile;
	romFile_init     = fileBrowser_libfat_init;
	romFile_deinit   = fileBrowser_libfatROM_deinit;
	// Make sure the romFile system is ready before we browse the filesystem
	romFile_init( romFile_topLevel );

	fileBrowserFrame_OpenDirectory(romFile_topLevel);
	pMenuContext->setActiveFrame(MenuContext::FRAME_FILEBROWSER);
}

void Func_LoadFromDVD()
{
	// Deinit any existing romFile state
	if(romFile_deinit) romFile_deinit( romFile_topLevel );
	// Change all the romFile pointers
	romFile_topLevel = &topLevel_DVD;
	romFile_readDir  = fileBrowser_DVD_readDir;
	romFile_readFile = fileBrowser_DVD_readFile;
	romFile_seekFile = fileBrowser_DVD_seekFile;
	romFile_init     = fileBrowser_DVD_init;
	romFile_deinit   = fileBrowser_DVD_deinit;
	// Make sure the romFile system is ready before we browse the filesystem
	romFile_init( romFile_topLevel );

	fileBrowserFrame_OpenDirectory(romFile_topLevel);
	pMenuContext->setActiveFrame(MenuContext::FRAME_FILEBROWSER);
}

void Func_LoadFromFS()
{
#ifdef WII
	// Deinit any existing romFile state
	if(romFile_deinit) romFile_deinit( romFile_topLevel );
	// Change all the romFile pointers
	romFile_topLevel = &topLevel_WiiFS;
	romFile_readDir  = fileBrowser_WiiFS_readDir;
	romFile_readFile = fileBrowser_WiiFSROM_readFile;
	romFile_seekFile = fileBrowser_WiiFS_seekFile;
	romFile_init     = fileBrowser_WiiFSROM_init;
	romFile_deinit   = fileBrowser_WiiFSROM_deinit;
	// Make sure the romFile system is ready before we browse the filesystem
	romFile_init( romFile_topLevel );

	fileBrowserFrame_OpenDirectory(romFile_topLevel);
	pMenuContext->setActiveFrame(MenuContext::FRAME_FILEBROWSER);
#endif
}

void Func_ReturnFromLoadRomFrame()
{
	pMenuContext->setActiveFrame(MenuContext::FRAME_MAIN);
}
