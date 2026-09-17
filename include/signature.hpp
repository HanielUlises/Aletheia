#pragma once
#include "task.hpp"

#include <ostream>

// Structural signature of a task as one JSON object: task features, topology of
// the initial model (Betti numbers of its simplicial complex's 2-skeleton over
// GF(2)), fixed-point depths (bisimulation refinement, knowledge relaxation),
// one-step model growth and agent symmetry.
void print_signature(const PlanningTask& task, std::ostream& out);
