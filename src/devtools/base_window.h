#pragma once
#include "../SDL_inc.h"
#include <string>

class BaseWindow {
protected:
    SDL_Window* window;
    bool open = true;
public:
    // title: shown in the window's title bar.
    BaseWindow(const char* title, int width, int height, Uint32 flags = 0);
    virtual ~BaseWindow();
    bool IsOpen();
    bool OwnsWindow(Uint32 windowID);
    void Raise();
    // True while the window has a text box taking typed keys.
    virtual bool IsTypingText() { return false; }
    virtual void Draw() = 0;
    virtual void HandleEvent(SDL_Event& e) = 0;
};