/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

class cmStandardLevel
{
  size_t index_;

public:
  cmStandardLevel(size_t index)
    : index_(index)
  {
  }
  size_t Index() const { return index_; }
  friend bool operator<(cmStandardLevel l, cmStandardLevel r)
  {
    return l.index_ < r.index_;
  }
};
