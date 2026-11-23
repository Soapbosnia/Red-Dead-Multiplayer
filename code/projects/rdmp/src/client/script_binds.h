#pragma once
/*
 * Key binds made by client scripts (bindKey / unbindKey, api_input.cpp), MTA-style: the Binds settings list every bind a
 * running resource made with a description, and the player can move it to another key. The player's keys are kept in
 * rdmp.ini [resource_binds] as "<resource>/<description>=<key>". Main thread.
 */

#include <string>
#include <vector>

namespace RDMP::ScriptBinds {
    struct Entry {
        std::string resource;
        std::string description;
        std::string key;        // the key it fires on now (the player's choice, else the script's)
        std::string defaultKey; // the script's
    };
    // Described binds of running resources, one row per resource + description.
    std::vector<Entry> List();
    // The player's key for a bind ("" = back to the script's key).
    void SetKey(const std::string &resource, const std::string &description, const std::string &key);

    // Key names (isKeyDown's) <-> virtual-key codes.
    int KeyCode(const std::string &name);
    std::string KeyName(int vk);
} // namespace RDMP::ScriptBinds
