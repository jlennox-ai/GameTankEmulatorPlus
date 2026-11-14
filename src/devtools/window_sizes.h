#pragma once
#include <initializer_list>
#include <string>
#include <utility>
#include "../toml/toml.hpp"

// The sizes and positions the resizable tool windows were last left at, kept between runs in
// window_sizes.toml in the emulator's preferences folder, one table per window name.
class WindowSizes {
public:
    // False: no size is saved for this window yet.
    static bool Load(const std::string& windowName, int& width, int& height);
    static void Save(const std::string& windowName, int width, int height);
    // The window's top left corner on the desktop (it can be negative, on a monitor left of or
    // above the main one). False: none saved yet.
    static bool LoadPosition(const std::string& windowName, int& x, int& y);
    static void SavePosition(const std::string& windowName, int x, int y);
    // Anything else a window keeps between runs (the memory browser's scroll position). False:
    // none saved yet.
    static bool LoadValue(const std::string& windowName, const char* key, int& value);
    static void SaveValue(const std::string& windowName, const char* key, int value);

private:
    static bool fileRead;
    static toml::table sizes;

    static std::string FilePath();
    static void ReadFile();
    // A pair of the window's values, like its width and height. False if neither is saved, or
    // (reported) only one is.
    static bool LoadPair(const std::string& windowName, const char* firstKey, const char* secondKey,
        int64_t& first, int64_t& second);
    static void SavePair(const std::string& windowName, const char* firstKey, int first, const char* secondKey,
        int second);
    // Sets the window's values and writes the file.
    static void SaveValues(const std::string& windowName, std::initializer_list<std::pair<const char*, int>> values);
};
