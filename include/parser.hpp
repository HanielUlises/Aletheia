#pragma once
#include "task.hpp"
#include <string>

// Load a ground epistemic planning task from the JSON file
PlanningTask load_task(const std::string& json_path);
