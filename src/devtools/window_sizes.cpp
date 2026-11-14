#include "window_sizes.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include "../SDL_inc.h"

#ifndef PREFS_TITLE
#define PREFS_TITLE "Emulator"
#endif

bool WindowSizes::fileRead = false;
toml::table WindowSizes::sizes;

std::string WindowSizes::FilePath() {
    char* prefPathText = SDL_GetPrefPath("GameTank", PREFS_TITLE);
    if(prefPathText == nullptr) {
        throw std::runtime_error(std::string("No preferences folder for the window sizes: ") + SDL_GetError());
    }
    std::filesystem::path prefPath(prefPathText);
    SDL_free(prefPathText);
    return (prefPath / "window_sizes.toml").string();
}

// The file is only preferences: one that can't be read is reported and replaced on the next save,
// rather than stopping the emulator.
void WindowSizes::ReadFile() {
    fileRead = true;
    std::string path = FilePath();
    if(!std::filesystem::exists(path)) {
        return;
    }
    try {
        sizes = toml::parse_file(path);
    } catch(const toml::parse_error& error) {
        std::cerr << "Ignoring " << path << ", which can't be read: " << error.description() << std::endl;
        sizes = toml::table();
    }
}

bool WindowSizes::LoadPair(const std::string& windowName, const char* firstKey, const char* secondKey,
    int64_t& first, int64_t& second) {
    if(!fileRead) {
        ReadFile();
    }
    std::optional<int64_t> savedFirst = sizes[windowName][firstKey].value<int64_t>();
    std::optional<int64_t> savedSecond = sizes[windowName][secondKey].value<int64_t>();
    if(!savedFirst.has_value() && !savedSecond.has_value()) {
        return false;
    }
    if(!savedFirst.has_value() || !savedSecond.has_value()) {
        std::cerr << "Ignoring the " << windowName << " window's " << firstKey << " and " << secondKey
            << ", which aren't both saved" << std::endl;
        return false;
    }
    first = *savedFirst;
    second = *savedSecond;
    return true;
}

void WindowSizes::SavePair(const std::string& windowName, const char* firstKey, int first, const char* secondKey,
    int second) {
    SaveValues(windowName, { { firstKey, first }, { secondKey, second } });
}

bool WindowSizes::LoadValue(const std::string& windowName, const char* key, int& value) {
    if(!fileRead) {
        ReadFile();
    }
    std::optional<int64_t> saved = sizes[windowName][key].value<int64_t>();
    if(!saved.has_value()) {
        return false;
    }
    value = (int) *saved;
    return true;
}

void WindowSizes::SaveValue(const std::string& windowName, const char* key, int value) {
    SaveValues(windowName, { { key, value } });
}

void WindowSizes::SaveValues(const std::string& windowName, std::initializer_list<std::pair<const char*, int>> values) {
    if(!fileRead) {
        ReadFile();
    }
    if(sizes[windowName].as_table() == nullptr) {
        sizes.insert_or_assign(windowName, toml::table());
    }
    toml::table& window = *sizes[windowName].as_table();
    for(auto& value : values) {
        window.insert_or_assign(value.first, value.second);
    }

    std::string path = FilePath();
    std::ofstream file(path, std::ios_base::out | std::ios_base::trunc);
    file << sizes << "\n";
    if(!file) {
        std::cerr << "Couldn't save the window sizes to " << path << std::endl;
    }
}

bool WindowSizes::Load(const std::string& windowName, int& width, int& height) {
    int64_t savedWidth;
    int64_t savedHeight;
    if(!LoadPair(windowName, "width", "height", savedWidth, savedHeight)) {
        return false;
    }
    if((savedWidth <= 0) || (savedHeight <= 0)) {
        std::cerr << "Ignoring the saved size of the " << windowName << " window, which isn't a valid size" << std::endl;
        return false;
    }
    width = (int) savedWidth;
    height = (int) savedHeight;
    return true;
}

void WindowSizes::Save(const std::string& windowName, int width, int height) {
    SavePair(windowName, "width", width, "height", height);
}

bool WindowSizes::LoadPosition(const std::string& windowName, int& x, int& y) {
    int64_t savedX;
    int64_t savedY;
    if(!LoadPair(windowName, "x", "y", savedX, savedY)) {
        return false;
    }
    x = (int) savedX;
    y = (int) savedY;
    return true;
}

void WindowSizes::SavePosition(const std::string& windowName, int x, int y) {
    SavePair(windowName, "x", x, "y", y);
}
