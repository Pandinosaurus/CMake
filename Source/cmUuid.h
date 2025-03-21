/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <string>
#include <vector>

/** \class cmUuid
 * \brief Utility class to generate UUIDs as defined by RFC4122
 *
 */
class cmUuid
{
public:
  std::string FromMd5(std::vector<unsigned char> const& uuidNamespace,
                      std::string const& name) const;

  std::string FromSha1(std::vector<unsigned char> const& uuidNamespace,
                       std::string const& name) const;

  bool StringToBinary(std::string const& input,
                      std::vector<unsigned char>& output) const;

private:
  std::string ByteToHex(unsigned char byte) const;

  void CreateHashInput(std::vector<unsigned char> const& uuidNamespace,
                       std::string const& name,
                       std::vector<unsigned char>& output) const;

  std::string FromDigest(unsigned char const* digest,
                         unsigned char version) const;

  bool StringToBinaryImpl(std::string const& input,
                          std::vector<unsigned char>& output) const;

  std::string BinaryToString(unsigned char const* input) const;

  bool IntFromHexDigit(char input, char& output) const;
};
