/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmCTestResourceSpec.h"

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <cmext/string_view>

#include <cm3p/json/value.h>

#include "cmsys/RegularExpression.hxx"

#include "cmJSONHelpers.h"

namespace {
using JSONHelperBuilder = cmJSONHelperBuilder;
cmsys::RegularExpression const IdentifierRegex{ "^[a-z_][a-z0-9_]*$" };
cmsys::RegularExpression const IdRegex{ "^[a-z0-9_]+$" };

struct Version
{
  int Major = 1;
  int Minor = 0;
};

struct TopVersion
{
  struct Version Version;
};

auto const VersionFieldHelper =
  JSONHelperBuilder::Int(cmCTestResourceSpecErrors::INVALID_VERSION);

auto const VersionHelper = JSONHelperBuilder::Required<Version>(
  cmCTestResourceSpecErrors::NO_VERSION,
  JSONHelperBuilder::Object<Version>()
    .Bind("major"_s, &Version::Major, VersionFieldHelper)
    .Bind("minor"_s, &Version::Minor, VersionFieldHelper));

auto const RootVersionHelper = JSONHelperBuilder::Object<TopVersion>().Bind(
  "version"_s, &TopVersion::Version, VersionHelper, false);

bool ResourceIdHelper(std::string& out, Json::Value const* value,
                      cmJSONState* state)
{
#if defined(__GNUC__) && __GNUC__ >= 15
#  define CM_GCC_diagnostic_push_Wmaybe_uninitialized
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
  if (!JSONHelperBuilder::String(cmCTestResourceSpecErrors::INVALID_RESOURCE)(
        out, value, state)) {
    return false;
  }
#ifdef CM_GCC_diagnostic_push_Wmaybe_uninitialized
#  pragma GCC diagnostic pop
#  undef CM_GCC_diagnostic_push_Wmaybe_uninitialized
#endif
  cmsys::RegularExpressionMatch match;
  if (!IdRegex.find(out.c_str(), match)) {
    cmCTestResourceSpecErrors::INVALID_RESOURCE(value, state);
    return false;
  }
  return true;
}

auto const ResourceHelper =
  JSONHelperBuilder::Object<cmCTestResourceSpec::Resource>()
    .Bind("id"_s, &cmCTestResourceSpec::Resource::Id, ResourceIdHelper)
    .Bind(
      "slots"_s, &cmCTestResourceSpec::Resource::Capacity,
      JSONHelperBuilder::UInt(cmCTestResourceSpecErrors::INVALID_RESOURCE, 1),
      false);

auto const ResourceListHelper =
  JSONHelperBuilder::Vector<cmCTestResourceSpec::Resource>(
    cmCTestResourceSpecErrors::INVALID_RESOURCE_TYPE, ResourceHelper);

auto const ResourceMapHelper =
  JSONHelperBuilder::MapFilter<std::vector<cmCTestResourceSpec::Resource>>(
    cmCTestResourceSpecErrors::INVALID_SOCKET_SPEC, ResourceListHelper,
    [](std::string const& key) -> bool {
      cmsys::RegularExpressionMatch match;
      return IdentifierRegex.find(key.c_str(), match);
    });

auto const SocketSetHelper = JSONHelperBuilder::Vector<
  std::map<std::string, std::vector<cmCTestResourceSpec::Resource>>>(
  cmCTestResourceSpecErrors::INVALID_SOCKET_SPEC, ResourceMapHelper);

bool SocketHelper(cmCTestResourceSpec::Socket& out, Json::Value const* value,
                  cmJSONState* state)
{
  std::vector<
    std::map<std::string, std::vector<cmCTestResourceSpec::Resource>>>
    sockets;
  if (!SocketSetHelper(sockets, value, state)) {
    return false;
  }
  if (sockets.size() > 1) {
    cmCTestResourceSpecErrors::INVALID_SOCKET_SPEC(value, state);
    return false;
  }
  if (sockets.empty()) {
    out.Resources.clear();
  } else {
    out.Resources = std::move(sockets[0]);
  }
  return true;
}

auto const LocalRequiredHelper =
  JSONHelperBuilder::Required<cmCTestResourceSpec::Socket>(
    cmCTestResourceSpecErrors::INVALID_SOCKET_SPEC, SocketHelper);

auto const RootHelper = JSONHelperBuilder::Object<cmCTestResourceSpec>().Bind(
  "local", &cmCTestResourceSpec::LocalSocket, LocalRequiredHelper, false);
}

bool cmCTestResourceSpec::ReadFromJSONFile(std::string const& filename)
{
  Json::Value root;

  this->parseState = cmJSONState(filename, &root);
  if (!this->parseState.errors.empty()) {
    return false;
  }

  TopVersion version;
  bool result;
  if ((result = RootVersionHelper(version, &root, &parseState)) != true) {
    return result;
  }
  if (version.Version.Major != 1 || version.Version.Minor != 0) {
    return false;
  }

  return RootHelper(*this, &root, &parseState);
}

bool cmCTestResourceSpec::operator==(cmCTestResourceSpec const& other) const
{
  return this->LocalSocket == other.LocalSocket;
}

bool cmCTestResourceSpec::operator!=(cmCTestResourceSpec const& other) const
{
  return !(*this == other);
}

bool cmCTestResourceSpec::Socket::operator==(
  cmCTestResourceSpec::Socket const& other) const
{
  return this->Resources == other.Resources;
}

bool cmCTestResourceSpec::Socket::operator!=(
  cmCTestResourceSpec::Socket const& other) const
{
  return !(*this == other);
}

bool cmCTestResourceSpec::Resource::operator==(
  cmCTestResourceSpec::Resource const& other) const
{
  return this->Id == other.Id && this->Capacity == other.Capacity;
}

bool cmCTestResourceSpec::Resource::operator!=(
  cmCTestResourceSpec::Resource const& other) const
{
  return !(*this == other);
}
