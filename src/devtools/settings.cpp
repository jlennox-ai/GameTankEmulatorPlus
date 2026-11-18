#include "settings.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include "../SDL_inc.h"

#ifndef PREFS_TITLE
#define PREFS_TITLE "Emulator"
#endif

#define SETTINGS_FILE_NAME "settings.toml"

bool Settings::fileRead = false;
toml::table Settings::values;

std::string Settings::FilePath() {
    char* prefPathText = SDL_GetPrefPath("GameTank", PREFS_TITLE);
    if(prefPathText == nullptr) {
        throw std::runtime_error(std::string("No preferences folder for the settings: ") + SDL_GetError());
    }
    std::filesystem::path prefPath(prefPathText);
    SDL_free(prefPathText);
    return (prefPath / SETTINGS_FILE_NAME).string();
}

// The file is only preferences: one that can't be read is reported and replaced on the next save,
// rather than stopping the emulator.
void Settings::ReadFile() {
    fileRead = true;
    std::string path = FilePath();
    if(!std::filesystem::exists(path)) {
        return;
    }
    try {
        values = toml::parse_file(path);
    } catch(const toml::parse_error& error) {
        std::cerr << "Ignoring " << path << ", which can't be read: " << error.description() << std::endl;
        values = toml::table();
    }
}

bool Settings::LoadPair(const std::string& windowName, const char* firstKey, const char* secondKey,
    int64_t& first, int64_t& second) {
    if(!fileRead) {
        ReadFile();
    }
    std::optional<int64_t> savedFirst = values[windowName][firstKey].value<int64_t>();
    std::optional<int64_t> savedSecond = values[windowName][secondKey].value<int64_t>();
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

void Settings::SavePair(const std::string& windowName, const char* firstKey, int first, const char* secondKey,
    int second) {
    SaveValues(windowName, { { firstKey, first }, { secondKey, second } });
}

bool Settings::LoadValue(const std::string& windowName, const char* key, int& value) {
    if(!fileRead) {
        ReadFile();
    }
    std::optional<int64_t> saved = values[windowName][key].value<int64_t>();
    if(!saved.has_value()) {
        return false;
    }
    value = (int) *saved;
    return true;
}

void Settings::SaveValue(const std::string& windowName, const char* key, int value) {
    SaveValues(windowName, { { key, value } });
}

bool Settings::LoadStrings(const std::string& windowName, const char* key, std::vector<std::string>& strings) {
    if(!fileRead) {
        ReadFile();
    }
    const toml::array* saved = values[windowName][key].as_array();
    if(saved == nullptr) {
        return false;
    }
    strings.clear();
    for(auto& element : *saved) {
        std::optional<std::string> text = element.value<std::string>();
        if(!text.has_value()) {
            std::cerr << "Ignoring a value in the " << windowName << " window's " << key << ", which isn't text" << std::endl;
            continue;
        }
        strings.push_back(*text);
    }
    return true;
}

void Settings::SaveStrings(const std::string& windowName, const char* key, const std::vector<std::string>& strings) {
    toml::array saved;
    for(auto& text : strings) {
        saved.push_back(text);
    }
    WindowTable(windowName).insert_or_assign(key, saved);
    WriteFile();
}

void Settings::SaveValues(const std::string& windowName, std::initializer_list<std::pair<const char*, int>> entries) {
    toml::table& window = WindowTable(windowName);
    for(auto& entry : entries) {
        window.insert_or_assign(entry.first, entry.second);
    }
    WriteFile();
}

toml::table& Settings::WindowTable(const std::string& windowName) {
    if(!fileRead) {
        ReadFile();
    }
    if(values[windowName].as_table() == nullptr) {
        values.insert_or_assign(windowName, toml::table());
    }
    return *values[windowName].as_table();
}

void Settings::WriteFile() {
    std::string path = FilePath();
    std::ofstream file(path, std::ios_base::out | std::ios_base::trunc);
    file << values << "\n";
    if(!file) {
        std::cerr << "Couldn't save the settings to " << path << std::endl;
    }
}

bool Settings::LoadSize(const std::string& windowName, int& width, int& height) {
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

void Settings::SaveSize(const std::string& windowName, int width, int height) {
    SavePair(windowName, "width", width, "height", height);
}

bool Settings::LoadPosition(const std::string& windowName, int& x, int& y) {
    int64_t savedX;
    int64_t savedY;
    if(!LoadPair(windowName, "x", "y", savedX, savedY)) {
        return false;
    }
    x = (int) savedX;
    y = (int) savedY;
    return true;
}

void Settings::SavePosition(const std::string& windowName, int x, int y) {
    SavePair(windowName, "x", x, "y", y);
}
