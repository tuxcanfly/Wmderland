#ifndef WM_HPP_
#define WM_HPP_

#include "property_manager.hpp"
#include "workspace.hpp"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <cstring>
#include <vector>

class WindowManager {
public:
    /* Singleton since only a single WM is required at once. */
    static WindowManager* GetInstance();
    ~WindowManager();
    void Run();
private:
    static WindowManager* instance_;
    WindowManager(Display* dpy);
    
    /* XEvent handlers. */
    static int OnXError(Display* dpy, XErrorEvent* e);
    void OnCreateNotify();
    void OnDestroyNotify();
    void OnMapRequest();
    void OnKeyPress();
    void OnButtonPress();
    void OnButtonRelease();
    void OnMotionNotify();
    void OnPropertyNotify();
    void OnFocusIn();
    void OnFocusOut();

    /* Workspace manipulation */
    void GotoWorkspace(short n);

    /* Client window placement */
    void Center(Window w);
    
    Display* dpy_;
    XEvent event_;
    XWindowAttributes attr_;
    XButtonEvent start_;
    Cursor cursor_;
    bool fullscreen_;

    void SetCursor(Window w, Cursor c);

    /* Properties */
    PropertyManager* property_mgr_;

    /* Cursors */
    Cursor cursors_[4];

    /* Bar */
    short bar_height_;

    /* Workspaces */
    std::vector<Workspace*> workspaces_;
    short current_;
};

#endif
