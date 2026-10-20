#pragma once
#include "app_state.h"
#include <string>
#include <vector>

std::vector<DiskInfo> discover_disks();
bool start_scan(AppState& state, const std::string& path);
void pause_scan(AppState& state);
void resume_scan(AppState& state);
void stop_scan(AppState& state);
const char* state_name(ScanState s);
