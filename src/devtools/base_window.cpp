#include "base_window.h"

bool BaseWindow::IsOpen() {
    return open;
}

void BaseWindow::Raise() {
    SDL_RaiseWindow(window);
}

bool BaseWindow::OwnsWindow(Uint32 windowID) {
    return SDL_GetWindowID(window) == windowID;
}

BaseWindow::BaseWindow(const char* title, int width, int height, Uint32 flags) {
    open = true;
    window = SDL_CreateWindow(title,
		SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
	 	width, height, flags);
}

BaseWindow::~BaseWindow() {
    SDL_DestroyWindow(window);
}