#pragma once
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>
#include "../toml/toml.hpp"

// The emulator's settings kept between runs, in settings.toml in its preferences folder: a table
// per window name, with the size and position the window was last left at and whatever else it
// keeps (the memory views' scroll positions and goto history, whether the debugger shows its
// memory pane, whether the main window shows the screen's sides).
class Settings {
public:
    // False: no size is saved for this window yet.
    static bool LoadSize(const std::string& windowName, int& width, int& height);
    static void SaveSize(const std::string& windowName, int width, int height);
    // The window's top left corner on the desktop (it can be negative, on a monitor left of or
    // above the main one). False: none saved yet.
    static bool LoadPosition(const std::string& windowName, int& x, int& y);
    static void SavePosition(const std::string& windowName, int x, int y);
    // Anything else a window keeps between runs. False: none saved yet.
    static bool LoadValue(const std::string& windowName, const char* key, int& value);
    static void SaveValue(const std::string& windowName, const char* key, int value);
    // A list of text a window keeps between runs (the memory views' goto history). False: none
    // saved yet.
    static bool LoadStrings(const std::string& windowName, const char* key, std::vector<std::string>& strings);
    static void SaveStrings(const std::string& windowName, const char* key, const std::vector<std::string>& strings);

private:
    static bool fileRead;
    static toml::table values;

    static std::string FilePath();
    static void ReadFile();
    // A pair of the window's values, like its width and height. False if neither is saved, or
    // (reported) only one is.
    static bool LoadPair(const std::string& windowName, const char* firstKey, const char* secondKey,
        int64_t& first, int64_t& second);
    static void SavePair(const std::string& windowName, const char* firstKey, int first, const char* secondKey,
        int second);
    // Sets the window's values and writes the file.
    static void SaveValues(const std::string& windowName, std::initializer_list<std::pair<const char*, int>> entries);
    // The window's table, made if it has none yet.
    static toml::table& WindowTable(const std::string& windowName);
    static void WriteFile();
};
