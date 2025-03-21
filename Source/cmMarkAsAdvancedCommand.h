/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <string>
#include <vector>

class cmExecutionStatus;

/**
 * \brief mark_as_advanced command
 *
 * cmMarkAsAdvancedCommand implements the mark_as_advanced CMake command
 */
bool cmMarkAsAdvancedCommand(std::vector<std::string> const& args,
                             cmExecutionStatus& status);
