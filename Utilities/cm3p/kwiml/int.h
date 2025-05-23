/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

/* Use the KWIML library configured for CMake.  */
#include "cmThirdParty.h"
#ifdef CMAKE_USE_SYSTEM_KWIML
#  include <kwiml/int.h> // IWYU pragma: export
#else
#  include <KWIML/include/kwiml/int.h> // IWYU pragma: export
#endif
