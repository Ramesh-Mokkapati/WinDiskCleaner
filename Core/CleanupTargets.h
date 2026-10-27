// CleanupTargets.h : the built-in cleanup locations from the runbook
#pragma once
#include "CleanupEngine.h"

struct TargetOptions
{
    bool deep = false;               // include Windows Update cache and system logs
    bool allUsers = false;           // user temp / INetCache of every local profile, not just the caller's
    int  minAgeHoursOverride = -1;   // -1 keeps each target's default
};

// Builds the target list. Standard targets come first, then the /DEEP ones.
// Targets whose folder cannot be resolved are left out.
std::vector<CleanupTarget> BuildTargets(const TargetOptions& options);

bool IsProcessElevated();
