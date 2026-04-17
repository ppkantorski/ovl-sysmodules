#include "gui_main.hpp"

constexpr const char* const amsContentsPath = "/atmosphere/contents";
constexpr const char* const boot2FlagFormat = "/atmosphere/contents/%016lX/flags/boot2.flag";
constexpr const char* const boot2FlagFolder = "/atmosphere/contents/%016lX/flags";

static char pathBuffer[FS_MAX_PATH];

constexpr const char* const descriptions[2][2] = {
    [0] = {
        [0] = "Off",
        [1] = "Off",
    },
    [1] = {
        [0] = "On",
        [1] = "On",
    },
};

// Pre-allocate buffer for file reading to avoid repeated allocations
static char fileBuffer[4096];

GuiMain::GuiMain() {
    // Pre-allocate vector for typical number of modules (avoids reallocations)
    m_sysmoduleListItems.reserve(32);
    
    DIR* dir = opendir(amsContentsPath);
    if (!dir)
        return;

    SystemModule module;
    std::string listItemText;
    listItemText.reserve(64);

    struct dirent* entry;
    /* Iterate over contents folder. */
    while ((entry = readdir(dir)) != nullptr) {
        // Skip . and .. entries
        if (entry->d_name[0] == '.')
            continue;

        // Only process directories
        if (entry->d_type != DT_DIR)
            continue;

        // Fast path filtering using pointer comparison
        if (*(uint32_t*)entry->d_name == *(uint32_t*)&"0100" && *(uint64_t*)(&entry->d_name[4]) != *(uint64_t*)&"00000000")
            continue;

        // Build toolbox.json path
        std::snprintf(pathBuffer, FS_MAX_PATH, "/atmosphere/contents/%s/toolbox.json", entry->d_name);
        
        // Use FILE* for file reading
        FILE* fp = std::fopen(pathBuffer, "rb");
        if (!fp)
            continue;

        // Get file size
        std::fseek(fp, 0, SEEK_END);
        const long size = std::ftell(fp);
        if (size <= 0 || size > 4096) { // Sanity check
            std::fclose(fp);
            continue;
        }
        std::fseek(fp, 0, SEEK_SET);

        // Read directly into static buffer - no allocation
        const size_t bytesRead = std::fread(fileBuffer, 1, size, fp);
        std::fclose(fp);
        
        if (bytesRead != static_cast<size_t>(size))
            continue;

        // Null-terminate for cJSON
        fileBuffer[size] = '\0';

        // Parse JSON using cJSON - parse directly from static buffer
        cJSON* toolboxFileContent = cJSON_ParseWithLength(fileBuffer, size);
        if (!toolboxFileContent)
            continue;

        // Get tid field
        cJSON* tidItem = cJSON_GetObjectItem(toolboxFileContent, "tid");
        if (!tidItem || !cJSON_IsString(tidItem)) {
            cJSON_Delete(toolboxFileContent);
            continue;
        }

        const u64 sysmoduleProgramId = std::strtoul(tidItem->valuestring, nullptr, 16);

        /* Let's not allow Tesla to be killed with this. */
        if (sysmoduleProgramId == 0x420000000007E51AULL) {
            cJSON_Delete(toolboxFileContent);
            continue;
        }

        // Get name field
        cJSON* nameItem = cJSON_GetObjectItem(toolboxFileContent, "name");
        if (!nameItem || !cJSON_IsString(nameItem)) {
            cJSON_Delete(toolboxFileContent);
            continue;
        }

        // Get requires_reboot field
        cJSON* rebootItem = cJSON_GetObjectItem(toolboxFileContent, "requires_reboot");
        if (!rebootItem || !cJSON_IsBool(rebootItem)) {
            cJSON_Delete(toolboxFileContent);
            continue;
        }

        // Build list item text efficiently - assign directly without clearing
        listItemText.assign(nameItem->valuestring);
        
        cJSON* versionItem = cJSON_GetObjectItem(toolboxFileContent, "version");
        if (versionItem && cJSON_IsString(versionItem)) {
            listItemText += "";
            listItemText += versionItem->valuestring;
        }

        // Create formatted title ID string
        char titleIdBuffer[32];
        std::snprintf(titleIdBuffer, sizeof(titleIdBuffer), "%016lX", sysmoduleProgramId);

        module = {
            .listItem = new tsl::elm::ListItem(listItemText),
            .programId = sysmoduleProgramId,
            .needReboot = static_cast<bool>(cJSON_IsTrue(rebootItem)),
            .displayName = listItemText,
            .titleIdStr = titleIdBuffer,
        };

        cJSON_Delete(toolboxFileContent);

        // Pre-build and cache the flag path
        std::snprintf(module.flagPath, FS_MAX_PATH, boot2FlagFormat, module.programId);
        
        // Pre-build and cache the folder path
        std::snprintf(module.folderPath, FS_MAX_PATH, boot2FlagFolder, module.programId);

        module.listItem->setClickListener([this, module](u64 click) -> bool {
            if (module.needReboot) {
                module.listItem->isLocked = true;
            }
            
            if (click & KEY_A && !module.needReboot) {
                if (this->isRunning(module)) {
                    /* Kill process. */
                    pmshellTerminateProgram(module.programId);
                } else {
                    /* Start process. */
                    const NcmProgramLocation programLocation{
                        .program_id = module.programId,
                        .storageID = NcmStorageId_None,
                    };
                    u64 pid = 0;
                    pmshellLaunchProgram(0, &programLocation, &pid);
                }
                return true;
            }

            if (click & KEY_Y) {
                // Use cached paths
                if (this->hasFlag(module)) {
                    /* Remove boot2 flag file. */
                    std::remove(module.flagPath);
                } else {
                    /* Create flags directory if needed (cached path). */
                    mkdir(module.folderPath, 0777);
                    
                    /* Create boot2 flag file. */
                    FILE* flagFile = std::fopen(module.flagPath, "wb");
                    if (flagFile)
                        std::fclose(flagFile);
                }
                triggerSettingsFeedback();
                return true;
            }

            return false;
        });
        this->m_sysmoduleListItems.push_back(std::move(module));
    }

    closedir(dir);

    /* Sort modules alphabetically by name using std::sort (faster than list::sort) */
    std::sort(this->m_sysmoduleListItems.begin(), this->m_sysmoduleListItems.end(),
        [](const SystemModule &a, const SystemModule &b) {
            return a.listItem->getText() < b.listItem->getText();
        });

    this->m_scanned = true;
}

GuiMain::~GuiMain() {
    // Signal that we're shutting down to skip any pending updates
    m_isActive = false;
    
    // Fast cleanup - vector destructor handles the rest
    //m_sysmoduleListItems.clear();
}

// Method to draw available RAM only
inline void drawMemoryWidget(auto renderer) {
    static char ramString[24];
    static tsl::Color ramColor = {0,0,0,0};
    static u64 lastUpdateTick = 0;
    const u64 ticksPerSecond = armGetSystemTickFreq();
    
    const u64 currentTick = armGetSystemTick();
    
    // Update every second
    if (lastUpdateTick == 0 || currentTick - lastUpdateTick >= ticksPerSecond) {
        u64 RAM_Used_system_u, RAM_Total_system_u;
        svcGetSystemInfo(&RAM_Used_system_u, 1, INVALID_HANDLE, 2);
        svcGetSystemInfo(&RAM_Total_system_u, 0, INVALID_HANDLE, 2);
        
        const u64 freeRamBytes = RAM_Total_system_u - RAM_Used_system_u;
        
        float value;
        const char* unit;
        
        if (freeRamBytes >= 1024ULL * 1024 * 1024) {
            value = static_cast<float>(freeRamBytes) / (1024.0f * 1024.0f * 1024.0f);
            unit = "GB";
        } else {
            value = static_cast<float>(freeRamBytes) / (1024.0f * 1024.0f);
            unit = "MB";
        }
        
        int decimalPlaces;
        if (value >= 1000.0f) {
            decimalPlaces = 0;
        } else if (value >= 100.0f) {
            decimalPlaces = 1;
        } else if (value >= 10.0f) {
            decimalPlaces = 2;
        } else {
            decimalPlaces = 3;
        }
        
        std::snprintf(ramString, sizeof(ramString), "%.*f %s %s", decimalPlaces, value, unit, ult::FREE.c_str());
        
        const float freeRamMB = static_cast<float>(freeRamBytes) / (1024.0f * 1024.0f);
        
        if (freeRamMB >= 9.0f){
            ramColor = tsl::healthyRamTextColor;
        } else if (freeRamMB >= 5.0f) {
            ramColor = tsl::neutralRamTextColor;
        } else {
            ramColor = tsl::badRamTextColor;
        }
        
        lastUpdateTick = currentTick;
    }
    
    renderer->drawRect(239, 15, 1, 66, renderer->aWithOpacity(tsl::separatorColor));
    
    if (!ult::hideWidgetBackdrop) {
        renderer->drawUniformRoundedRect(247, 15, (ult::extendedWidgetBackdrop) ? tsl::cfg::FramebufferWidth - 255 : tsl::cfg::FramebufferWidth - 255 + 40, 66, renderer->a(tsl::widgetBackdropColor));
    }
    
    const int backdropCenterX = 247 + ((tsl::cfg::FramebufferWidth - 255) >> 1);
    
    // First line: "System" label
    size_t y_offset = 44 + 2 - 1;  // Same as the clock y_offset in the reference code
    const char* systemLabel = ult::SYSTEM_RAM.c_str();
    
    if (ult::centerWidgetAlignment) {
        const int labelWidth = renderer->getTextDimensions(systemLabel, false, 20).first;
        renderer->drawString(systemLabel, false, backdropCenterX - (labelWidth >> 1), y_offset, 20, tsl::headerTextColor);
    } else {
        const int labelWidth = renderer->getTextDimensions(systemLabel, false, 20).first;
        renderer->drawString(systemLabel, false, tsl::cfg::FramebufferWidth - labelWidth - 25, y_offset, 20, tsl::headerTextColor);
    }
    
    // Second line: RAM info
    y_offset += 22;  // Same spacing as in the reference code
    
    if (ult::centerWidgetAlignment) {
        const int ramWidth = renderer->getTextDimensions(ramString, false, 20).first;
        const int currentX = backdropCenterX - (ramWidth >> 1);
        renderer->drawString(ramString, false, currentX, y_offset, 20, ramColor);
    } else {
        const s32 ramWidth = renderer->getTextDimensions(ramString, false, 20).first;
        renderer->drawString(ramString, false, tsl::cfg::FramebufferWidth - ramWidth - 25, y_offset, 20, ramColor);
    }
}

tsl::elm::Element* GuiMain::createUI() {
    auto* rootFrame = new tsl::elm::HeaderOverlayFrame(97);
    rootFrame->setHeader(new tsl::elm::CustomDrawer([this](tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 w, s32 h) {
        renderer->drawString("Sysmodules", false, 20, 52, 32, tsl::defaultOverlayColor);
        renderer->drawString(VERSION, false, 20, 75, 15, tsl::bannerVersionTextColor);

        drawMemoryWidget(renderer);
    }));

    if (this->m_sysmoduleListItems.size() == 0) {
        const char* description = this->m_scanned ? "No sysmodules found!" : "Scan failed!";

        auto* warning = new tsl::elm::CustomDrawer([description](tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 w, s32 h) {
            renderer->drawString("\uE150", false, 180, 250, 90, tsl::headerTextColor);
            renderer->drawString(description, false, 110, 340, 25, tsl::headerTextColor);
        });

        rootFrame->setContent(warning);
    } else {
        tsl::elm::List* sysmoduleList = new tsl::elm::List();

        sysmoduleList->addItem(new tsl::elm::CategoryHeader("Dynamic   Auto Start   Toggle", true));
        sysmoduleList->addItem(new tsl::elm::CustomDrawer([](tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 w, s32 h) {
            renderer->drawString(" These sysmodules can be toggled at any time.", false, x + 5, y + 13, 15, tsl::warningTextColor);
        }), 30);
        for (const auto& module : this->m_sysmoduleListItems) {
            if (!module.needReboot) {
                module.listItem->enableShortHoldKey();
                sysmoduleList->addItem(module.listItem);
            }
        }

        sysmoduleList->addItem(new tsl::elm::CategoryHeader("Static   Auto Start", true));
        sysmoduleList->addItem(new tsl::elm::CustomDrawer([](tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 w, s32 h) {
            renderer->drawString(" These sysmodules need a reboot to work.", false, x + 5, y + 13, 15, tsl::warningTextColor);
        }), 30);
        for (const auto& module : this->m_sysmoduleListItems) {
            if (module.needReboot) {
                module.listItem->enableShortHoldKey();
                module.listItem->disableClickAnimation();
                sysmoduleList->addItem(module.listItem);
            }
        }
        rootFrame->setContent(sysmoduleList);
    }

    return rootFrame;
}

void GuiMain::update() {
    // Early exit if shutting down - avoids unnecessary work during cleanup
    if (!m_isActive)
        return;
        
    static u32 counter = 0;

    // Check every 30 frames (~0.5 seconds at 60fps)
    if (counter++ % 30 != 0)
        return;

    for (const auto& module : this->m_sysmoduleListItems) {
        this->updateStatus(module);
    }
}

bool GuiMain::handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos, HidAnalogStickState leftJoyStick, HidAnalogStickState rightJoyStick) {
    if (keysDown & KEY_MINUS) {
        toggleTitleIdDisplay();
        return true;
    }

    // Side-note: Not sure why it is needed, but for some reason the Overlay handleInput is being canabolized. Added to ensnsure behavior.
    // Navigational boundary cases for handling wrapping
    static bool lastDirectionPressed = true;
    const bool directionPressed = ((keysHeld & KEY_UP) || (keysHeld & KEY_DOWN) || (keysHeld & KEY_LEFT) || (keysHeld & KEY_RIGHT));

    if (!directionPressed && lastDirectionPressed)
        tsl::elm::s_directionalKeyReleased.store(true, std::memory_order_release);
    else if (directionPressed && lastDirectionPressed)
        tsl::elm::s_directionalKeyReleased.store(false, std::memory_order_release);

    lastDirectionPressed = directionPressed;

    return false;
}

void GuiMain::toggleTitleIdDisplay() {
    m_showTitleIds = !m_showTitleIds;
    
    // Update all list items with either title ID or display name
    for (auto& module : this->m_sysmoduleListItems) {
        if (m_showTitleIds) {
            module.listItem->setText(module.titleIdStr);
        } else {
            module.listItem->setText(module.displayName);
        }
    }
    
    // Trigger feedback
    triggerSettingsFeedback();
}

void GuiMain::updateStatus(const SystemModule &module) {
    const bool running = this->isRunning(module);
    const bool hasFlag = this->hasFlag(module);

    const char* desc = descriptions[running][hasFlag];
    module.listItem->setValue(desc, !running);
}

bool GuiMain::hasFlag(const SystemModule &module) {
    // Use access() for fastest file existence check
    return access(module.flagPath, F_OK) == 0;
}

bool GuiMain::isRunning(const SystemModule &module) {
    u64 pid = 0;
    return R_SUCCEEDED(pmdmntGetProcessId(&pid, module.programId)) && pid > 0;
}