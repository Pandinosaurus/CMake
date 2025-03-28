/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <iosfwd>
#include <string>

/** \class cmXMLSafe
 * \brief Write strings to XML with proper escapes
 */
class cmXMLSafe
{
public:
  /** Construct with the data to be written.  This assumes the data
      will exist for the duration of this object's life.  */
  cmXMLSafe(char const* s);
  cmXMLSafe(std::string const& s);

  /** Specify whether to escape quotes too.  This is needed when
      writing the content of an attribute value.  By default quotes
      are escaped.  */
  cmXMLSafe& Quotes(bool b = true);

  /** Get the escaped data as a string.  */
  std::string str() const;

private:
  char const* Data;
  unsigned long Size;
  bool DoQuotes;
  friend std::ostream& operator<<(std::ostream&, cmXMLSafe const&);
};
