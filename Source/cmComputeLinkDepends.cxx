/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmComputeLinkDepends.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include <cm/memory>
#include <cm/string_view>
#include <cmext/string_view>

#include "cmsys/RegularExpression.hxx"

#include "cmComputeComponentGraph.h"
#include "cmGeneratorExpression.h"
#include "cmGeneratorExpressionDAGChecker.h"
#include "cmGeneratorTarget.h"
#include "cmGlobalGenerator.h"
#include "cmList.h"
#include "cmListFileCache.h"
#include "cmLocalGenerator.h"
#include "cmMakefile.h"
#include "cmMessageType.h"
#include "cmPolicies.h"
#include "cmRange.h"
#include "cmState.h"
#include "cmStateTypes.h"
#include "cmStringAlgorithms.h"
#include "cmTarget.h"
#include "cmValue.h"
#include "cmake.h"

/*

This file computes an ordered list of link items to use when linking a
single target in one configuration.  Each link item is identified by
the string naming it.  A graph of dependencies is created in which
each node corresponds to one item and directed edges lead from nodes to
those which must *follow* them on the link line.  For example, the
graph

  A -> B -> C

will lead to the link line order

  A B C

The set of items placed in the graph is formed with a breadth-first
search of the link dependencies starting from the main target.

There are two types of items: those with known direct dependencies and
those without known dependencies.  We will call the two types "known
items" and "unknown items", respectively.  Known items are those whose
names correspond to targets (built or imported) and those for which an
old-style <item>_LIB_DEPENDS variable is defined.  All other items are
unknown and we must infer dependencies for them.  For items that look
like flags (beginning with '-') we trivially infer no dependencies,
and do not include them in the dependencies of other items.

Known items have dependency lists ordered based on how the user
specified them.  We can use this order to infer potential dependencies
of unknown items.  For example, if link items A and B are unknown and
items X and Y are known, then we might have the following dependency
lists:

  X: Y A B
  Y: A B

The explicitly known dependencies form graph edges

  X -> Y  ,  X -> A  ,  X -> B  ,  Y -> A  ,  Y -> B

We can also infer the edge

  A -> B

because *every* time A appears B is seen on its right.  We do not know
whether A really needs symbols from B to link, but it *might* so we
must preserve their order.  This is the case also for the following
explicit lists:

  X: A B Y
  Y: A B

Here, A is followed by the set {B,Y} in one list, and {B} in the other
list.  The intersection of these sets is {B}, so we can infer that A
depends on at most B.  Meanwhile B is followed by the set {Y} in one
list and {} in the other.  The intersection is {} so we can infer that
B has no dependencies.

Let's make a more complex example by adding unknown item C and
considering these dependency lists:

  X: A B Y C
  Y: A C B

The explicit edges are

  X -> Y  ,  X -> A  ,  X -> B  ,  X -> C  ,  Y -> A  ,  Y -> B  ,  Y -> C

For the unknown items, we infer dependencies by looking at the
"follow" sets:

  A: intersect( {B,Y,C} , {C,B} ) = {B,C} ; infer edges  A -> B  ,  A -> C
  B: intersect( {Y,C}   , {}    ) = {}    ; infer no edges
  C: intersect( {}      , {B}   ) = {}    ; infer no edges

Note that targets are never inferred as dependees because outside
libraries should not depend on them.

------------------------------------------------------------------------------

The initial exploration of dependencies using a BFS associates an
integer index with each link item.  When the graph is built outgoing
edges are sorted by this index.

After the initial exploration of the link interface tree, any
transitive (dependent) shared libraries that were encountered and not
included in the interface are processed in their own BFS.  This BFS
follows only the dependent library lists and not the link interfaces.
They are added to the link items with a mark indicating that the are
transitive dependencies.  Then cmComputeLinkInformation deals with
them on a per-platform basis.

The complete graph formed from all known and inferred dependencies may
not be acyclic, so an acyclic version must be created.
The original graph is converted to a directed acyclic graph in which
each node corresponds to a strongly connected component of the
original graph.  For example, the dependency graph

  X -> A -> B -> C -> A -> Y

contains strongly connected components {X}, {A,B,C}, and {Y}.  The
implied directed acyclic graph (DAG) is

  {X} -> {A,B,C} -> {Y}

We then compute a topological order for the DAG nodes to serve as a
reference for satisfying dependencies efficiently.  We perform the DFS
in reverse order and assign topological order indices counting down so
that the result is as close to the original BFS order as possible
without violating dependencies.

------------------------------------------------------------------------------

The final link entry order is constructed as follows.  We first walk
through and emit the *original* link line as specified by the user.
As each item is emitted, a set of pending nodes in the component DAG
is maintained.  When a pending component has been completely seen, it
is removed from the pending set and its dependencies (following edges
of the DAG) are added.  A trivial component (those with one item) is
complete as soon as its item is seen.  A non-trivial component (one
with more than one item; assumed to be static libraries) is complete
when *all* its entries have been seen *twice* (all entries seen once,
then all entries seen again, not just each entry twice).  A pending
component tracks which items have been seen and a count of how many
times the component needs to be seen (once for trivial components,
twice for non-trivial).  If at any time another component finishes and
re-adds an already pending component, the pending component is reset
so that it needs to be seen in its entirety again.  This ensures that
all dependencies of a component are satisfied no matter where it
appears.

After the original link line has been completed, we append to it the
remaining pending components and their dependencies.  This is done by
repeatedly emitting the first item from the first pending component
and following the same update rules as when traversing the original
link line.  Since the pending components are kept in topological order
they are emitted with minimal repeats (we do not want to emit a
component just to have it added again when another component is
completed later).  This process continues until no pending components
remain.  We know it will terminate because the component graph is
guaranteed to be acyclic.

The final list of items produced by this procedure consists of the
original user link line followed by minimal additional items needed to
satisfy dependencies.  The final list is then filtered to de-duplicate
items that we know the linker will reuse automatically (shared libs).

*/

namespace {
// LINK_LIBRARY helpers
bool IsFeatureSupported(cmMakefile* makefile, std::string const& linkLanguage,
                        std::string const& feature)
{
  auto featureSupported = cmStrCat(
    "CMAKE_", linkLanguage, "_LINK_LIBRARY_USING_", feature, "_SUPPORTED");
  if (cmValue perLangVar = makefile->GetDefinition(featureSupported)) {
    return perLangVar.IsOn();
  }

  featureSupported =
    cmStrCat("CMAKE_LINK_LIBRARY_USING_", feature, "_SUPPORTED");
  return makefile->GetDefinition(featureSupported).IsOn();
}

// LINK_LIBRARY feature attributes management
struct LinkLibraryFeatureAttributeSet
{
  std::set<cmStateEnums::TargetType> LibraryTypes = {
    cmStateEnums::EXECUTABLE, cmStateEnums::STATIC_LIBRARY,
    cmStateEnums::SHARED_LIBRARY, cmStateEnums::MODULE_LIBRARY,
    cmStateEnums::UNKNOWN_LIBRARY
  };
  std::set<std::string> Override;

  enum DeduplicationKind
  {
    Default,
    Yes,
    No
  };
  DeduplicationKind Deduplication = Default;
};
std::map<std::string, LinkLibraryFeatureAttributeSet>
  LinkLibraryFeatureAttributes;
LinkLibraryFeatureAttributeSet const& GetLinkLibraryFeatureAttributes(
  cmMakefile* makefile, std::string const& linkLanguage,
  std::string const& feature)
{
  auto it = LinkLibraryFeatureAttributes.find(feature);
  if (it != LinkLibraryFeatureAttributes.end()) {
    return it->second;
  }

  auto featureAttributesVariable =
    cmStrCat("CMAKE_", linkLanguage, "_LINK_LIBRARY_", feature, "_ATTRIBUTES");
  auto featureAttributesValues =
    makefile->GetDefinition(featureAttributesVariable);
  if (featureAttributesValues.IsEmpty()) {
    // try language agnostic definition
    featureAttributesVariable =
      cmStrCat("CMAKE_LINK_LIBRARY_", feature, "_ATTRIBUTES");
    featureAttributesValues =
      makefile->GetDefinition(featureAttributesVariable);
  }
  if (!featureAttributesValues.IsEmpty()) {
    LinkLibraryFeatureAttributeSet featureAttributes;
    cmsys::RegularExpression processingOption{
      "^(LIBRARY_TYPE|DEDUPLICATION|OVERRIDE)=((STATIC|SHARED|MODULE|"
      "EXECUTABLE)(,("
      "STATIC|"
      "SHARED|MODULE|EXECUTABLE)"
      ")*|YES|NO|DEFAULT|[A-Za-z0-9_]+(,[A-Za-z0-9_]+)*)$"
    };
    std::string errorMessage;
    for (auto const& option : cmList{ featureAttributesValues }) {
      if (processingOption.find(option)) {
        if (processingOption.match(1) == "LIBRARY_TYPE") {
          featureAttributes.LibraryTypes.clear();
          for (auto const& value :
               cmTokenize(processingOption.match(2), ',')) {
            if (value == "STATIC") {
              featureAttributes.LibraryTypes.emplace(
                cmStateEnums::STATIC_LIBRARY);
            } else if (value == "SHARED") {
              featureAttributes.LibraryTypes.emplace(
                cmStateEnums::SHARED_LIBRARY);
            } else if (value == "MODULE") {
              featureAttributes.LibraryTypes.emplace(
                cmStateEnums::MODULE_LIBRARY);
            } else if (value == "EXECUTABLE") {
              featureAttributes.LibraryTypes.emplace(cmStateEnums::EXECUTABLE);
            } else {
              errorMessage += cmStrCat("  ", option, '\n');
              break;
            }
          }
          // Always add UNKNOWN type
          featureAttributes.LibraryTypes.emplace(
            cmStateEnums::UNKNOWN_LIBRARY);
        } else if (processingOption.match(1) == "DEDUPLICATION") {
          if (processingOption.match(2) == "YES") {
            featureAttributes.Deduplication =
              LinkLibraryFeatureAttributeSet::Yes;
          } else if (processingOption.match(2) == "NO") {
            featureAttributes.Deduplication =
              LinkLibraryFeatureAttributeSet::No;
          } else if (processingOption.match(2) == "DEFAULT") {
            featureAttributes.Deduplication =
              LinkLibraryFeatureAttributeSet::Default;
          } else {
            errorMessage += cmStrCat("  ", option, '\n');
          }
        } else if (processingOption.match(1) == "OVERRIDE") {
          featureAttributes.Override.clear();
          std::vector<std::string> values =
            cmTokenize(processingOption.match(2), ',');
          featureAttributes.Override.insert(values.begin(), values.end());
        }
      } else {
        errorMessage += cmStrCat("  ", option, '\n');
      }
    }
    if (!errorMessage.empty()) {
      makefile->GetCMakeInstance()->IssueMessage(
        MessageType::FATAL_ERROR,
        cmStrCat("Erroneous option(s) for '", featureAttributesVariable,
                 "':\n", errorMessage));
    }
    return LinkLibraryFeatureAttributes.emplace(feature, featureAttributes)
      .first->second;
  }
  return LinkLibraryFeatureAttributes
    .emplace(feature, LinkLibraryFeatureAttributeSet{})
    .first->second;
}

// LINK_GROUP helpers
cm::string_view const LG_BEGIN = "<LINK_GROUP:"_s;
cm::string_view const LG_END = "</LINK_GROUP:"_s;
cm::string_view const LG_ITEM_BEGIN = "<LINK_GROUP>"_s;
cm::string_view const LG_ITEM_END = "</LINK_GROUP>"_s;

inline std::string ExtractGroupFeature(std::string const& item)
{
  return item.substr(LG_BEGIN.length(),
                     item.find(':', LG_BEGIN.length()) - LG_BEGIN.length());
}

bool IsGroupFeatureSupported(cmMakefile* makefile,
                             std::string const& linkLanguage,
                             std::string const& feature)
{
  auto featureSupported = cmStrCat(
    "CMAKE_", linkLanguage, "_LINK_GROUP_USING_", feature, "_SUPPORTED");
  if (makefile->GetDefinition(featureSupported).IsOn()) {
    return true;
  }

  featureSupported =
    cmStrCat("CMAKE_LINK_GROUP_USING_", feature, "_SUPPORTED");
  return makefile->GetDefinition(featureSupported).IsOn();
}

class EntriesProcessing
{
public:
  using LinkEntry = cmComputeLinkDepends::LinkEntry;
  using EntryVector = cmComputeLinkDepends::EntryVector;

  EntriesProcessing(cmGeneratorTarget const* target,
                    std::string const& linkLanguage, EntryVector& entries,
                    EntryVector& finalEntries)
    : Target(target)
    , LinkLanguage(linkLanguage)
    , Entries(entries)
    , FinalEntries(finalEntries)
  {
    auto const* makefile = target->Makefile;

    switch (target->GetPolicyStatusCMP0156()) {
      case cmPolicies::WARN:
        if (!makefile->GetCMakeInstance()->GetIsInTryCompile() &&
            makefile->PolicyOptionalWarningEnabled(
              "CMAKE_POLICY_WARNING_CMP0156")) {
          makefile->GetCMakeInstance()->IssueMessage(
            MessageType::AUTHOR_WARNING,
            cmStrCat(cmPolicies::GetPolicyWarning(cmPolicies::CMP0156),
                     "\nSince the policy is not set, legacy libraries "
                     "de-duplication strategy will be applied."),
            target->GetBacktrace());
        }
        CM_FALLTHROUGH;
      case cmPolicies::OLD:
        // rely on default initialization of the class
        break;
      case cmPolicies::NEW: {
        // Policy 0179 applies only when policy 0156 is new
        if (target->GetPolicyStatusCMP0179() == cmPolicies::WARN &&
            !makefile->GetCMakeInstance()->GetIsInTryCompile() &&
            makefile->PolicyOptionalWarningEnabled(
              "CMAKE_POLICY_WARNING_CMP0179")) {
          makefile->GetCMakeInstance()->IssueMessage(
            MessageType::AUTHOR_WARNING,
            cmStrCat(cmPolicies::GetPolicyWarning(cmPolicies::CMP0179),
                     "\nSince the policy is not set, static libraries "
                     "de-duplication will keep the last occurrence of the "
                     "static libraries."),
            target->GetBacktrace());
        }

        if (auto libProcessing = makefile->GetDefinition(cmStrCat(
              "CMAKE_", linkLanguage, "_LINK_LIBRARIES_PROCESSING"))) {
          // UNICITY keyword is just for compatibility with previous
          // implementation
          cmsys::RegularExpression processingOption{
            "^(ORDER|UNICITY|DEDUPLICATION)=(FORWARD|REVERSE|ALL|NONE|SHARED)$"
          };
          std::string errorMessage;
          for (auto const& option : cmList{ libProcessing }) {
            if (processingOption.find(option)) {
              if (processingOption.match(1) == "ORDER") {
                if (processingOption.match(2) == "FORWARD") {
                  this->Order = Forward;
                } else if (processingOption.match(2) == "REVERSE") {
                  this->Order = Reverse;
                } else {
                  errorMessage += cmStrCat("  ", option, '\n');
                }
              } else if (processingOption.match(1) == "UNICITY" ||
                         processingOption.match(1) == "DEDUPLICATION") {
                if (processingOption.match(2) == "ALL") {
                  this->Deduplication = All;
                } else if (processingOption.match(2) == "NONE") {
                  this->Deduplication = None;
                } else if (processingOption.match(2) == "SHARED") {
                  this->Deduplication = Shared;
                } else {
                  errorMessage += cmStrCat("  ", option, '\n');
                }
              }
            } else {
              errorMessage += cmStrCat("  ", option, '\n');
            }
          }
          if (!errorMessage.empty()) {
            makefile->GetCMakeInstance()->IssueMessage(
              MessageType::FATAL_ERROR,
              cmStrCat("Erroneous option(s) for 'CMAKE_", linkLanguage,
                       "_LINK_LIBRARIES_PROCESSING':\n", errorMessage),
              target->GetBacktrace());
          }
          // For some environments, deduplication should be activated only if
          // both policies CMP0156 and CMP0179 are NEW
          if (makefile->GetDefinition(cmStrCat(
                "CMAKE_", linkLanguage, "_PLATFORM_LINKER_ID")) == "LLD"_s &&
              makefile->GetDefinition("CMAKE_EXECUTABLE_FORMAT") == "ELF"_s &&
              target->GetPolicyStatusCMP0179() != cmPolicies::NEW &&
              this->Deduplication == All) {
            this->Deduplication = Shared;
          }
        }
      }
    }
  }

  void AddGroups(std::map<size_t, std::vector<size_t>> const& groups)
  {
    if (!groups.empty()) {
      this->Groups = &groups;
      // record all libraries as part of groups to ensure correct
      // deduplication: libraries as part of groups are always kept.
      for (auto const& g : groups) {
        for (auto index : g.second) {
          this->Emitted.insert(index);
        }
      }
    }
  }

  void AddLibraries(std::vector<size_t> const& libEntries)
  {
    if (this->Order == Reverse) {
      std::vector<size_t> entries;
      if (this->Deduplication == All &&
          this->Target->GetPolicyStatusCMP0179() == cmPolicies::NEW) {
        // keep the first occurrence of the static libraries
        std::set<size_t> emitted{ this->Emitted };
        for (auto index : libEntries) {
          LinkEntry const& entry = this->Entries[index];
          if (!entry.Target ||
              entry.Target->GetType() != cmStateEnums::STATIC_LIBRARY) {
            entries.emplace_back(index);
            continue;
          }
          if (this->IncludeEntry(entry) || emitted.insert(index).second) {
            entries.emplace_back(index);
          }
        }
      } else {
        entries = libEntries;
      }
      // Iterate in reverse order so we can keep only the last occurrence
      // of the shared libraries.
      this->AddLibraries(cmReverseRange(entries));
    } else {
      this->AddLibraries(cmMakeRange(libEntries));
    }
  }

  void AddObjects(std::vector<size_t> const& objectEntries)
  {
    // Place explicitly linked object files in the front.  The linker will
    // always use them anyway, and they may depend on symbols from libraries.
    if (this->Order == Reverse) {
      // Append in reverse order at the end since we reverse the final order.
      for (auto index : cmReverseRange(objectEntries)) {
        this->FinalEntries.emplace_back(this->Entries[index]);
      }
    } else {
      // Append in reverse order to ensure correct final order
      for (auto index : cmReverseRange(objectEntries)) {
        this->FinalEntries.emplace(this->FinalEntries.begin(),
                                   this->Entries[index]);
      }
    }
  }

  void Finalize()
  {
    if (this->Order == Reverse) {
      // Reverse the resulting order since we iterated in reverse.
      std::reverse(this->FinalEntries.begin(), this->FinalEntries.end());
    }

    // expand groups
    if (this->Groups) {
      for (auto const& g : *this->Groups) {
        LinkEntry const& groupEntry = this->Entries[g.first];
        auto it = this->FinalEntries.begin();
        while (true) {
          it = std::find_if(it, this->FinalEntries.end(),
                            [&groupEntry](LinkEntry const& entry) -> bool {
                              return groupEntry.Item == entry.Item;
                            });
          if (it == this->FinalEntries.end()) {
            break;
          }
          it->Item.Value = std::string(LG_ITEM_END);
          for (auto index = g.second.rbegin(); index != g.second.rend();
               ++index) {
            it = this->FinalEntries.insert(it, this->Entries[*index]);
          }
          it = this->FinalEntries.insert(it, groupEntry);
          it->Item.Value = std::string(LG_ITEM_BEGIN);
        }
      }
    }
  }

private:
  enum OrderKind
  {
    Forward,
    Reverse
  };

  enum DeduplicationKind
  {
    None,
    Shared,
    All
  };

  bool IncludeEntry(LinkEntry const& entry) const
  {
    if (entry.Feature != cmComputeLinkDepends::LinkEntry::DEFAULT) {
      auto const& featureAttributes = GetLinkLibraryFeatureAttributes(
        this->Target->Makefile, this->LinkLanguage, entry.Feature);
      if ((!entry.Target ||
           featureAttributes.LibraryTypes.find(entry.Target->GetType()) !=
             featureAttributes.LibraryTypes.end()) &&
          featureAttributes.Deduplication !=
            LinkLibraryFeatureAttributeSet::Default) {
        return featureAttributes.Deduplication ==
          LinkLibraryFeatureAttributeSet::No;
      }
    }

    return this->Deduplication == None ||
      (this->Deduplication == Shared &&
       (!entry.Target ||
        entry.Target->GetType() != cmStateEnums::SHARED_LIBRARY)) ||
      (this->Deduplication == All && entry.Kind != LinkEntry::Library);
  }

  template <typename Range>
  void AddLibraries(Range const& libEntries)
  {
    for (auto index : libEntries) {
      LinkEntry const& entry = this->Entries[index];
      if (this->IncludeEntry(entry) || this->Emitted.insert(index).second) {
        this->FinalEntries.emplace_back(entry);
      }
    }
  }

  OrderKind Order = Reverse;
  DeduplicationKind Deduplication = Shared;
  cmGeneratorTarget const* Target;
  std::string const& LinkLanguage;
  EntryVector& Entries;
  EntryVector& FinalEntries;
  std::set<size_t> Emitted;
  std::map<size_t, std::vector<size_t>> const* Groups = nullptr;
};
}

std::string const& cmComputeLinkDepends::LinkEntry::DEFAULT =
  cmLinkItem::DEFAULT;

cmComputeLinkDepends::cmComputeLinkDepends(cmGeneratorTarget const* target,
                                           std::string const& config,
                                           std::string const& linkLanguage,
                                           LinkLibrariesStrategy strategy)
  : Target(target)
  , Makefile(this->Target->Target->GetMakefile())
  , GlobalGenerator(this->Target->GetLocalGenerator()->GetGlobalGenerator())
  , CMakeInstance(this->GlobalGenerator->GetCMakeInstance())
  , Config(config)
  , DebugMode(this->Makefile->IsOn("CMAKE_LINK_DEPENDS_DEBUG_MODE") ||
              this->Target->GetProperty("LINK_DEPENDS_DEBUG_MODE").IsOn())
  , LinkLanguage(linkLanguage)
  , LinkType(ComputeLinkType(
      this->Config, this->Makefile->GetCMakeInstance()->GetDebugConfigs()))
  , Strategy(strategy)

{
  // target oriented feature override property takes precedence over
  // global override property
  cm::string_view lloPrefix = "LINK_LIBRARY_OVERRIDE_"_s;
  auto const& keys = this->Target->GetPropertyKeys();
  std::for_each(
    keys.cbegin(), keys.cend(),
    [this, &lloPrefix, &config, &linkLanguage](std::string const& key) {
      if (cmHasPrefix(key, lloPrefix)) {
        if (cmValue feature = this->Target->GetProperty(key)) {
          if (!feature->empty() && key.length() > lloPrefix.length()) {
            auto item = key.substr(lloPrefix.length());
            cmGeneratorExpressionDAGChecker dagChecker{
              this->Target,
              "LINK_LIBRARY_OVERRIDE",
              nullptr,
              nullptr,
              this->Target->GetLocalGenerator(),
              config,
              this->Target->GetBacktrace(),
            };
            auto overrideFeature = cmGeneratorExpression::Evaluate(
              *feature, this->Target->GetLocalGenerator(), config,
              this->Target, &dagChecker, this->Target, linkLanguage);
            this->LinkLibraryOverride.emplace(item, overrideFeature);
          }
        }
      }
    });
  // global override property
  if (cmValue linkLibraryOverride =
        this->Target->GetProperty("LINK_LIBRARY_OVERRIDE")) {
    cmGeneratorExpressionDAGChecker dagChecker{
      target,
      "LINK_LIBRARY_OVERRIDE",
      nullptr,
      nullptr,
      target->GetLocalGenerator(),
      config,
      target->GetBacktrace(),
    };
    auto overrideValue = cmGeneratorExpression::Evaluate(
      *linkLibraryOverride, target->GetLocalGenerator(), config, target,
      &dagChecker, target, linkLanguage);

    std::vector<std::string> overrideList =
      cmTokenize(overrideValue, ',', cmTokenizerMode::New);
    if (overrideList.size() >= 2) {
      auto const& feature = overrideList.front();
      std::for_each(overrideList.cbegin() + 1, overrideList.cend(),
                    [this, &feature](std::string const& item) {
                      this->LinkLibraryOverride.emplace(item, feature);
                    });
    }
  }
}

cmComputeLinkDepends::~cmComputeLinkDepends() = default;

std::vector<cmComputeLinkDepends::LinkEntry> const&
cmComputeLinkDepends::Compute()
{
  // Follow the link dependencies of the target to be linked.
  this->AddDirectLinkEntries();

  // Complete the breadth-first search of dependencies.
  while (!this->BFSQueue.empty()) {
    // Get the next entry.
    BFSEntry qe = this->BFSQueue.front();
    this->BFSQueue.pop();

    // Follow the entry's dependencies.
    this->FollowLinkEntry(qe);
  }

  // Complete the search of shared library dependencies.
  while (!this->SharedDepQueue.empty()) {
    // Handle the next entry.
    this->HandleSharedDependency(this->SharedDepQueue.front());
    this->SharedDepQueue.pop();
  }

  // Infer dependencies of targets for which they were not known.
  this->InferDependencies();

  // finalize groups dependencies
  // All dependencies which are raw items must be replaced by the group
  // it belongs to, if any.
  this->UpdateGroupDependencies();

  // Cleanup the constraint graph.
  this->CleanConstraintGraph();

  // Display the constraint graph.
  if (this->DebugMode) {
    fprintf(stderr,
            "---------------------------------------"
            "---------------------------------------\n");
    fprintf(stderr, "Link dependency analysis for target %s, config %s\n",
            this->Target->GetName().c_str(),
            this->Config.empty() ? "noconfig" : this->Config.c_str());
    this->DisplayConstraintGraph();
  }

  // Compute the DAG of strongly connected components.  The algorithm
  // used by cmComputeComponentGraph should identify the components in
  // the same order in which the items were originally discovered in
  // the BFS.  This should preserve the original order when no
  // constraints disallow it.
  this->CCG =
    cm::make_unique<cmComputeComponentGraph>(this->EntryConstraintGraph);
  this->CCG->Compute();

  if (!this->CheckCircularDependencies()) {
    return this->FinalLinkEntries;
  }

  // Compute the final ordering.
  this->OrderLinkEntries();

  // Display the final ordering.
  if (this->DebugMode) {
    this->DisplayOrderedEntries();
  }

  // Compute the final set of link entries.
  EntriesProcessing entriesProcessing{ this->Target, this->LinkLanguage,
                                       this->EntryList,
                                       this->FinalLinkEntries };
  // Add groups first, to ensure that libraries of the groups are always kept.
  entriesProcessing.AddGroups(this->GroupItems);
  entriesProcessing.AddLibraries(this->FinalLinkOrder);
  entriesProcessing.AddObjects(this->ObjectEntries);
  entriesProcessing.Finalize();

  // Display the final set.
  if (this->DebugMode) {
    this->DisplayFinalEntries();
  }

  return this->FinalLinkEntries;
}

std::string const& cmComputeLinkDepends::GetCurrentFeature(
  std::string const& item, std::string const& defaultFeature) const
{
  auto it = this->LinkLibraryOverride.find(item);
  return it == this->LinkLibraryOverride.end() ? defaultFeature : it->second;
}

std::pair<std::map<cmLinkItem, size_t>::iterator, bool>
cmComputeLinkDepends::AllocateLinkEntry(cmLinkItem const& item)
{
  std::map<cmLinkItem, size_t>::value_type index_entry(
    item, static_cast<size_t>(this->EntryList.size()));
  auto lei = this->LinkEntryIndex.insert(index_entry);
  if (lei.second) {
    this->EntryList.emplace_back();
    this->InferredDependSets.emplace_back();
    this->EntryConstraintGraph.emplace_back();
  }
  return lei;
}

std::pair<size_t, bool> cmComputeLinkDepends::AddLinkEntry(
  cmLinkItem const& item, cm::optional<size_t> groupIndex)
{
  // Allocate a spot for the item entry.
  auto lei = this->AllocateLinkEntry(item);

  // Check if the item entry has already been added.
  if (!lei.second) {
    // Yes.  We do not need to follow the item's dependencies again.
    return { lei.first->second, false };
  }

  // Initialize the item entry.
  size_t index = lei.first->second;
  LinkEntry& entry = this->EntryList[index];
  entry.Item = BT<std::string>(item.AsStr(), item.Backtrace);
  entry.Target = item.Target;
  entry.Feature = item.Feature;
  if (!entry.Target && entry.Item.Value[0] == '-' &&
      entry.Item.Value[1] != 'l' &&
      entry.Item.Value.substr(0, 10) != "-framework") {
    entry.Kind = LinkEntry::Flag;
    entry.Feature = LinkEntry::DEFAULT;
  } else if (cmHasPrefix(entry.Item.Value, LG_BEGIN) &&
             cmHasSuffix(entry.Item.Value, '>')) {
    entry.Kind = LinkEntry::Group;
  }

  if (entry.Kind != LinkEntry::Group) {
    // If the item has dependencies queue it to follow them.
    if (entry.Target) {
      // Target dependencies are always known.  Follow them.
      BFSEntry qe = { index, groupIndex, nullptr };
      this->BFSQueue.push(qe);
    } else {
      // Look for an old-style <item>_LIB_DEPENDS variable.
      std::string var = cmStrCat(entry.Item.Value, "_LIB_DEPENDS");
      if (cmValue val = this->Makefile->GetDefinition(var)) {
        // The item dependencies are known.  Follow them.
        BFSEntry qe = { index, groupIndex, val->c_str() };
        this->BFSQueue.push(qe);
      } else if (entry.Kind != LinkEntry::Flag) {
        // The item dependencies are not known.  We need to infer them.
        this->InferredDependSets[index].Initialized = true;
      }
    }
  }

  return { index, true };
}

void cmComputeLinkDepends::AddLinkObject(cmLinkItem const& item)
{
  assert(!item.Target); // The item is an object file, not its target.

  // Allocate a spot for the item entry.
  auto lei = this->AllocateLinkEntry(item);

  // Check if the item entry has already been added.
  if (!lei.second) {
    return;
  }

  // Initialize the item entry.
  size_t index = lei.first->second;
  LinkEntry& entry = this->EntryList[index];
  entry.Item = BT<std::string>(item.AsStr(), item.Backtrace);
  entry.Kind = LinkEntry::Object;
  entry.ObjectSource = item.ObjectSource;

  // Record explicitly linked object files separately.
  this->ObjectEntries.emplace_back(index);
}

void cmComputeLinkDepends::FollowLinkEntry(BFSEntry qe)
{
  // Get this entry representation.
  size_t depender_index = qe.GroupIndex ? *qe.GroupIndex : qe.Index;
  LinkEntry const& entry = this->EntryList[qe.Index];

  // Follow the item's dependencies.
  if (entry.Target) {
    // Follow the target dependencies.
    if (cmLinkInterface const* iface =
          entry.Target->GetLinkInterface(this->Config, this->Target)) {
      bool const isIface =
        entry.Target->GetType() == cmStateEnums::INTERFACE_LIBRARY;
      // This target provides its own link interface information.
      this->AddLinkEntries(depender_index, iface->Libraries);
      this->AddLinkObjects(iface->Objects);
      for (auto const& language : iface->Languages) {
        auto runtimeEntries = iface->LanguageRuntimeLibraries.find(language);
        if (runtimeEntries != iface->LanguageRuntimeLibraries.end()) {
          this->AddLinkEntries(depender_index, runtimeEntries->second);
        }
      }

      if (isIface) {
        return;
      }

      // Handle dependent shared libraries.
      this->FollowSharedDeps(depender_index, iface);
    }
  } else {
    // Follow the old-style dependency list.
    this->AddVarLinkEntries(depender_index, qe.LibDepends);
  }
}

void cmComputeLinkDepends::FollowSharedDeps(size_t depender_index,
                                            cmLinkInterface const* iface,
                                            bool follow_interface)
{
  // Follow dependencies if we have not followed them already.
  if (this->SharedDepFollowed.insert(depender_index).second) {
    if (follow_interface) {
      this->QueueSharedDependencies(depender_index, iface->Libraries);
    }
    this->QueueSharedDependencies(depender_index, iface->SharedDeps);
  }
}

void cmComputeLinkDepends::QueueSharedDependencies(
  size_t depender_index, std::vector<cmLinkItem> const& deps)
{
  for (cmLinkItem const& li : deps) {
    SharedDepEntry qe;
    qe.Item = li;
    qe.DependerIndex = depender_index;
    this->SharedDepQueue.push(qe);
  }
}

void cmComputeLinkDepends::HandleSharedDependency(SharedDepEntry const& dep)
{
  // Allocate a spot for the item entry.
  auto lei = this->AllocateLinkEntry(dep.Item);
  size_t index = lei.first->second;

  // Check if the target does not already has an entry.
  if (lei.second) {
    // Initialize the item entry.
    LinkEntry& entry = this->EntryList[index];
    entry.Item = BT<std::string>(dep.Item.AsStr(), dep.Item.Backtrace);
    entry.Target = dep.Item.Target;

    // This item was added specifically because it is a dependent
    // shared library.  It may get special treatment
    // in cmComputeLinkInformation.
    entry.Kind = LinkEntry::SharedDep;
  }

  // Get the link entry for this target.
  LinkEntry& entry = this->EntryList[index];

  // This shared library dependency must follow the item that listed
  // it.
  this->EntryConstraintGraph[dep.DependerIndex].emplace_back(
    index, true, false, cmListFileBacktrace());

  // Target items may have their own dependencies.
  if (entry.Target) {
    if (cmLinkInterface const* iface =
          entry.Target->GetLinkInterface(this->Config, this->Target)) {
      // Follow public and private dependencies transitively.
      this->FollowSharedDeps(index, iface, true);
    }
  }
}

void cmComputeLinkDepends::AddVarLinkEntries(
  cm::optional<size_t> depender_index, char const* value)
{
  // This is called to add the dependencies named by
  // <item>_LIB_DEPENDS.  The variable contains a semicolon-separated
  // list.  The list contains link-type;item pairs and just items.
  cmList deplist{ value };

  // Look for entries meant for this configuration.
  std::vector<cmLinkItem> actual_libs;
  cmTargetLinkLibraryType llt = GENERAL_LibraryType;
  bool haveLLT = false;
  for (std::string const& d : deplist) {
    if (d == "debug") {
      llt = DEBUG_LibraryType;
      haveLLT = true;
    } else if (d == "optimized") {
      llt = OPTIMIZED_LibraryType;
      haveLLT = true;
    } else if (d == "general") {
      llt = GENERAL_LibraryType;
      haveLLT = true;
    } else if (!d.empty()) {
      // If no explicit link type was given prior to this entry then
      // check if the entry has its own link type variable.  This is
      // needed for compatibility with dependency files generated by
      // the export_library_dependencies command from CMake 2.4 and
      // lower.
      if (!haveLLT) {
        std::string var = cmStrCat(d, "_LINK_TYPE");
        if (cmValue val = this->Makefile->GetDefinition(var)) {
          if (*val == "debug") {
            llt = DEBUG_LibraryType;
          } else if (*val == "optimized") {
            llt = OPTIMIZED_LibraryType;
          }
        }
      }

      // If the library is meant for this link type then use it.
      if (llt == GENERAL_LibraryType || llt == this->LinkType) {
        actual_libs.emplace_back(this->ResolveLinkItem(depender_index, d));
      }

      // Reset the link type until another explicit type is given.
      llt = GENERAL_LibraryType;
      haveLLT = false;
    }
  }

  // Add the entries from this list.
  this->AddLinkEntries(depender_index, actual_libs);
}

void cmComputeLinkDepends::AddDirectLinkEntries()
{
  // Add direct link dependencies in this configuration.
  cmLinkImplementation const* impl = this->Target->GetLinkImplementation(
    this->Config, cmGeneratorTarget::UseTo::Link);
  this->AddLinkEntries(cm::nullopt, impl->Libraries);
  this->AddLinkObjects(impl->Objects);

  for (auto const& language : impl->Languages) {
    auto runtimeEntries = impl->LanguageRuntimeLibraries.find(language);
    if (runtimeEntries != impl->LanguageRuntimeLibraries.end()) {
      this->AddLinkEntries(cm::nullopt, runtimeEntries->second);
    }
  }
}

template <typename T>
void cmComputeLinkDepends::AddLinkEntries(cm::optional<size_t> depender_index,
                                          std::vector<T> const& libs)
{
  // Track inferred dependency sets implied by this list.
  std::map<size_t, DependSet> dependSets;

  cm::optional<std::pair<size_t, bool>> group;
  std::vector<size_t> groupItems;

  // Loop over the libraries linked directly by the depender.
  for (T const& l : libs) {
    // Skip entries that will resolve to the target getting linked or
    // are empty.
    cmLinkItem const& item = l;
    if (item.AsStr() == this->Target->GetName() || item.AsStr().empty()) {
      continue;
    }

    // emit a warning if an undefined feature is used as part of
    // an imported target
    if (item.Feature != LinkEntry::DEFAULT && depender_index) {
      auto const& depender = this->EntryList[*depender_index];
      if (depender.Target && depender.Target->IsImported() &&
          !IsFeatureSupported(this->Makefile, this->LinkLanguage,
                              item.Feature)) {
        this->CMakeInstance->IssueMessage(
          MessageType::AUTHOR_ERROR,
          cmStrCat("The 'IMPORTED' target '", depender.Target->GetName(),
                   "' uses the generator-expression '$<LINK_LIBRARY>' with "
                   "the feature '",
                   item.Feature,
                   "', which is undefined or unsupported.\nDid you miss to "
                   "define it by setting variables \"CMAKE_",
                   this->LinkLanguage, "_LINK_LIBRARY_USING_", item.Feature,
                   "\" and \"CMAKE_", this->LinkLanguage,
                   "_LINK_LIBRARY_USING_", item.Feature, "_SUPPORTED\"?"),
          this->Target->GetBacktrace());
      }
    }

    if (cmHasPrefix(item.AsStr(), LG_BEGIN) &&
        cmHasSuffix(item.AsStr(), '>')) {
      group = this->AddLinkEntry(item, cm::nullopt);
      if (group->second) {
        LinkEntry& entry = this->EntryList[group->first];
        entry.Feature = ExtractGroupFeature(item.AsStr());
      }
      if (depender_index) {
        this->EntryConstraintGraph[*depender_index].emplace_back(
          group->first, false, false, cmListFileBacktrace());
      } else {
        // This is a direct dependency of the target being linked.
        this->OriginalEntries.push_back(group->first);
      }
      continue;
    }

    size_t dependee_index;

    if (cmHasPrefix(item.AsStr(), LG_END) && cmHasSuffix(item.AsStr(), '>')) {
      assert(group);
      dependee_index = group->first;
      if (group->second) {
        this->GroupItems.emplace(group->first, std::move(groupItems));
      }
      group = cm::nullopt;
      groupItems.clear();
      continue;
    }

    if (depender_index && group) {
      auto const& depender = this->EntryList[*depender_index];
      auto const& groupFeature = this->EntryList[group->first].Feature;
      if (depender.Target && depender.Target->IsImported() &&
          !IsGroupFeatureSupported(this->Makefile, this->LinkLanguage,
                                   groupFeature)) {
        this->CMakeInstance->IssueMessage(
          MessageType::AUTHOR_ERROR,
          cmStrCat("The 'IMPORTED' target '", depender.Target->GetName(),
                   "' uses the generator-expression '$<LINK_GROUP>' with "
                   "the feature '",
                   groupFeature,
                   "', which is undefined or unsupported.\nDid you miss to "
                   "define it by setting variables \"CMAKE_",
                   this->LinkLanguage, "_LINK_GROUP_USING_", groupFeature,
                   "\" and \"CMAKE_", this->LinkLanguage, "_LINK_GROUP_USING_",
                   groupFeature, "_SUPPORTED\"?"),
          this->Target->GetBacktrace());
      }
    }

    // Add a link entry for this item.
    auto ale = this->AddLinkEntry(
      item, group ? cm::optional<size_t>(group->first) : cm::nullopt);
    dependee_index = ale.first;
    LinkEntry& entry = this->EntryList[dependee_index];
    bool supportedItem = true;
    auto const& itemFeature =
      this->GetCurrentFeature(entry.Item.Value, item.Feature);
    if (group && ale.second && entry.Target &&
        (entry.Target->GetType() == cmStateEnums::TargetType::OBJECT_LIBRARY ||
         entry.Target->GetType() ==
           cmStateEnums::TargetType::INTERFACE_LIBRARY)) {
      supportedItem = false;
      auto const& groupFeature = this->EntryList[group->first].Feature;
      this->CMakeInstance->IssueMessage(
        MessageType::AUTHOR_WARNING,
        cmStrCat(
          "The feature '", groupFeature,
          "', specified as part of a generator-expression "
          "'$",
          LG_BEGIN, groupFeature, ">', will not be applied to the ",
          (entry.Target->GetType() == cmStateEnums::TargetType::OBJECT_LIBRARY
             ? "OBJECT"
             : "INTERFACE"),
          " library '", entry.Item.Value, "'."),
        this->Target->GetBacktrace());
    }
    // check if feature is applicable to this item
    if (itemFeature != LinkEntry::DEFAULT && entry.Target) {
      auto const& featureAttributes = GetLinkLibraryFeatureAttributes(
        this->Makefile, this->LinkLanguage, itemFeature);
      if (featureAttributes.LibraryTypes.find(entry.Target->GetType()) ==
          featureAttributes.LibraryTypes.end()) {
        supportedItem = false;
        this->CMakeInstance->IssueMessage(
          MessageType::AUTHOR_WARNING,
          cmStrCat("The feature '", itemFeature,
                   "', specified as part of a generator-expression "
                   "'$<LINK_LIBRARY:",
                   itemFeature, ">', will not be applied to the ",
                   cmState::GetTargetTypeName(entry.Target->GetType()), " '",
                   entry.Item.Value, "'."),
          this->Target->GetBacktrace());
      }
    }
    if (ale.second) {
      // current item not yet defined
      entry.Feature = itemFeature;
      if (!supportedItem) {
        entry.Feature = LinkEntry::DEFAULT;
      }
    }

    if (supportedItem) {
      if (group) {
        auto const& currentFeature = this->EntryList[group->first].Feature;
        for (auto const& g : this->GroupItems) {
          auto const& groupFeature = this->EntryList[g.first].Feature;
          if (groupFeature == currentFeature) {
            continue;
          }
          if (std::find(g.second.cbegin(), g.second.cend(), dependee_index) !=
              g.second.cend()) {
            this->CMakeInstance->IssueMessage(
              MessageType::FATAL_ERROR,
              cmStrCat("Impossible to link target '", this->Target->GetName(),
                       "' because the link item '", entry.Item.Value,
                       "', specified with the group feature '", currentFeature,
                       "', has already occurred with the feature '",
                       groupFeature, "', which is not allowed."),
              this->Target->GetBacktrace());
            continue;
          }
        }
      }
      if (entry.Feature != itemFeature) {
        bool incompatibleFeatures = true;
        // check if an override is possible
        auto const& entryFeatureAttributes = GetLinkLibraryFeatureAttributes(
          this->Makefile, this->LinkLanguage, entry.Feature);
        auto const& itemFeatureAttributes = GetLinkLibraryFeatureAttributes(
          this->Makefile, this->LinkLanguage, itemFeature);
        if (itemFeatureAttributes.Override.find(entry.Feature) !=
              itemFeatureAttributes.Override.end() &&
            entryFeatureAttributes.Override.find(itemFeature) !=
              entryFeatureAttributes.Override.end()) {
          // features override each other
          this->CMakeInstance->IssueMessage(
            MessageType::FATAL_ERROR,
            cmStrCat("Impossible to link target '", this->Target->GetName(),
                     "' because the link item '", entry.Item.Value,
                     "' is specified with the features '", itemFeature,
                     "' and '", entry.Feature,
                     "'"
                     ", and both have an 'OVERRIDE' attribute that overrides "
                     "the other. Such cycles are not allowed."),
            this->Target->GetBacktrace());
        } else {
          if (itemFeatureAttributes.Override.find(entry.Feature) !=
              itemFeatureAttributes.Override.end()) {
            entry.Feature = itemFeature;
            incompatibleFeatures = false;
          } else if (entryFeatureAttributes.Override.find(itemFeature) !=
                     entryFeatureAttributes.Override.end()) {
            incompatibleFeatures = false;
          }
          if (incompatibleFeatures) {
            // incompatibles features occurred
            this->CMakeInstance->IssueMessage(
              MessageType::FATAL_ERROR,
              cmStrCat(
                "Impossible to link target '", this->Target->GetName(),
                "' because the link item '", entry.Item.Value, "', specified ",
                (itemFeature == LinkEntry::DEFAULT
                   ? "without any feature or 'DEFAULT' feature"
                   : cmStrCat("with the feature '", itemFeature, '\'')),
                ", has already occurred ",
                (entry.Feature == LinkEntry::DEFAULT
                   ? "without any feature or 'DEFAULT' feature"
                   : cmStrCat("with the feature '", entry.Feature, '\'')),
                ", which is not allowed."),
              this->Target->GetBacktrace());
          }
        }
      }
    }

    if (group) {
      // store item index for dependencies handling
      groupItems.push_back(dependee_index);
    } else {
      std::vector<size_t> indexes;
      bool entryHandled = false;
      // search any occurrence of the library in already defined groups
      for (auto const& g : this->GroupItems) {
        for (auto index : g.second) {
          if (entry.Item.Value == this->EntryList[index].Item.Value) {
            indexes.push_back(g.first);
            entryHandled = true;
            break;
          }
        }
      }
      if (!entryHandled) {
        indexes.push_back(dependee_index);
      }

      for (auto index : indexes) {
        // The dependee must come after the depender.
        if (depender_index) {
          this->EntryConstraintGraph[*depender_index].emplace_back(
            index, false, false, cmListFileBacktrace());
        } else {
          // This is a direct dependency of the target being linked.
          this->OriginalEntries.push_back(index);
        }

        // Update the inferred dependencies for earlier items.
        for (auto& dependSet : dependSets) {
          // Add this item to the inferred dependencies of other items.
          // Target items are never inferred dependees because unknown
          // items are outside libraries that should not be depending on
          // targets.
          if (!this->EntryList[index].Target &&
              this->EntryList[index].Kind != LinkEntry::Flag &&
              this->EntryList[index].Kind != LinkEntry::Group &&
              dependee_index != dependSet.first) {
            dependSet.second.insert(index);
          }
        }

        // If this item needs to have dependencies inferred, do so.
        if (this->InferredDependSets[index].Initialized) {
          // Make sure an entry exists to hold the set for the item.
          dependSets[index];
        }
      }
    }
  }

  // Store the inferred dependency sets discovered for this list.
  for (auto const& dependSet : dependSets) {
    this->InferredDependSets[dependSet.first].push_back(dependSet.second);
  }
}

void cmComputeLinkDepends::AddLinkObjects(std::vector<cmLinkItem> const& objs)
{
  for (cmLinkItem const& obj : objs) {
    this->AddLinkObject(obj);
  }
}

cmLinkItem cmComputeLinkDepends::ResolveLinkItem(
  cm::optional<size_t> depender_index, std::string const& name)
{
  // Look for a target in the scope of the depender.
  cmGeneratorTarget const* from = this->Target;
  if (depender_index) {
    if (cmGeneratorTarget const* depender =
          this->EntryList[*depender_index].Target) {
      from = depender;
    }
  }
  return from->ResolveLinkItem(BT<std::string>(name));
}

void cmComputeLinkDepends::InferDependencies()
{
  // The inferred dependency sets for each item list the possible
  // dependencies.  The intersection of the sets for one item form its
  // inferred dependencies.
  for (size_t depender_index = 0;
       depender_index < this->InferredDependSets.size(); ++depender_index) {
    // Skip items for which dependencies do not need to be inferred or
    // for which the inferred dependency sets are empty.
    DependSetList& sets = this->InferredDependSets[depender_index];
    if (!sets.Initialized || sets.empty()) {
      continue;
    }

    // Intersect the sets for this item.
    DependSet common = sets.front();
    for (DependSet const& i : cmMakeRange(sets).advance(1)) {
      DependSet intersection;
      std::set_intersection(common.begin(), common.end(), i.begin(), i.end(),
                            std::inserter(intersection, intersection.begin()));
      common = intersection;
    }

    // Add the inferred dependencies to the graph.
    cmGraphEdgeList& edges = this->EntryConstraintGraph[depender_index];
    edges.reserve(edges.size() + common.size());
    for (auto const& c : common) {
      edges.emplace_back(c, true, false, cmListFileBacktrace());
    }
  }
}

void cmComputeLinkDepends::UpdateGroupDependencies()
{
  if (this->GroupItems.empty()) {
    return;
  }

  // Walks through all entries of the constraint graph to replace dependencies
  // over raw items by the group it belongs to, if any.
  for (auto& edgeList : this->EntryConstraintGraph) {
    for (auto& edge : edgeList) {
      size_t index = edge;
      if (this->EntryList[index].Kind == LinkEntry::Group ||
          this->EntryList[index].Kind == LinkEntry::Flag ||
          this->EntryList[index].Kind == LinkEntry::Object) {
        continue;
      }
      // search the item in the defined groups
      for (auto const& groupItems : this->GroupItems) {
        auto pos = std::find(groupItems.second.cbegin(),
                             groupItems.second.cend(), index);
        if (pos != groupItems.second.cend()) {
          // replace lib dependency by the group it belongs to
          edge = cmGraphEdge{ groupItems.first, false, false,
                              cmListFileBacktrace() };
        }
      }
    }
  }
}

void cmComputeLinkDepends::CleanConstraintGraph()
{
  for (cmGraphEdgeList& edgeList : this->EntryConstraintGraph) {
    // Sort the outgoing edges for each graph node so that the
    // original order will be preserved as much as possible.
    std::sort(edgeList.begin(), edgeList.end());

    // Make the edge list unique.
    edgeList.erase(std::unique(edgeList.begin(), edgeList.end()),
                   edgeList.end());
  }
}

bool cmComputeLinkDepends::CheckCircularDependencies() const
{
  std::vector<NodeList> const& components = this->CCG->GetComponents();
  size_t nc = components.size();
  for (size_t c = 0; c < nc; ++c) {
    // Get the current component.
    NodeList const& nl = components[c];

    // Skip trivial components.
    if (nl.size() < 2) {
      continue;
    }

    // no group must be evolved
    bool cycleDetected = false;
    for (size_t ni : nl) {
      if (this->EntryList[ni].Kind == LinkEntry::Group) {
        cycleDetected = true;
        break;
      }
    }
    if (!cycleDetected) {
      continue;
    }

    // Construct the error message.
    auto formatItem = [](LinkEntry const& entry) -> std::string {
      if (entry.Kind == LinkEntry::Group) {
        auto items =
          entry.Item.Value.substr(entry.Item.Value.find(':', 12) + 1);
        items.pop_back();
        std::replace(items.begin(), items.end(), '|', ',');
        return cmStrCat("group \"", ExtractGroupFeature(entry.Item.Value),
                        ":{", items, "}\"");
      }
      return cmStrCat('"', entry.Item.Value, '"');
    };

    std::ostringstream e;
    e << "The inter-target dependency graph, for the target \""
      << this->Target->GetName()
      << "\", contains the following strongly connected component "
         "(cycle):\n";
    std::vector<size_t> const& cmap = this->CCG->GetComponentMap();
    for (size_t i : nl) {
      // Get the depender.
      LinkEntry const& depender = this->EntryList[i];

      // Describe the depender.
      e << "  " << formatItem(depender) << "\n";

      // List its dependencies that are inside the component.
      EdgeList const& el = this->EntryConstraintGraph[i];
      for (cmGraphEdge const& ni : el) {
        size_t j = ni;
        if (cmap[j] == c) {
          LinkEntry const& dependee = this->EntryList[j];
          e << "    depends on " << formatItem(dependee) << "\n";
        }
      }
    }
    this->CMakeInstance->IssueMessage(MessageType::FATAL_ERROR, e.str(),
                                      this->Target->GetBacktrace());

    return false;
  }

  return true;
}

void cmComputeLinkDepends::DisplayConstraintGraph()
{
  // Display the graph nodes and their edges.
  std::ostringstream e;
  for (size_t i = 0; i < this->EntryConstraintGraph.size(); ++i) {
    EdgeList const& nl = this->EntryConstraintGraph[i];
    e << "item " << i << " is [" << this->EntryList[i].Item << "]\n";
    e << cmWrap("  item ", nl, " must follow it", "\n") << "\n";
  }
  fprintf(stderr, "%s\n", e.str().c_str());
}

void cmComputeLinkDepends::OrderLinkEntries()
{
  // The component graph is guaranteed to be acyclic.  Start a DFS
  // from every entry to compute a topological order for the
  // components.
  Graph const& cgraph = this->CCG->GetComponentGraph();
  size_t n = cgraph.size();
  this->ComponentVisited.resize(cgraph.size(), 0);
  this->ComponentOrder.resize(cgraph.size(), n);
  this->ComponentOrderId = n;
  // Run in reverse order so the topological order will preserve the
  // original order where there are no constraints.
  for (size_t c = n; c > 0; --c) {
    this->VisitComponent(c - 1);
  }

  // Display the component graph.
  if (this->DebugMode) {
    this->DisplayComponents();
  }

  // Start with the original link line.
  switch (this->Strategy) {
    case LinkLibrariesStrategy::REORDER_MINIMALLY: {
      // Emit the direct dependencies in their original order.
      // This gives projects control over ordering.
      for (size_t originalEntry : this->OriginalEntries) {
        this->VisitEntry(originalEntry);
      }
    } break;
    case LinkLibrariesStrategy::REORDER_FREELY: {
      // Schedule the direct dependencies for emission in topo order.
      // This may produce more efficient link lines.
      for (size_t originalEntry : this->OriginalEntries) {
        this->MakePendingComponent(
          this->CCG->GetComponentMap()[originalEntry]);
      }
    } break;
  }

  // Now explore anything left pending.  Since the component graph is
  // guaranteed to be acyclic we know this will terminate.
  while (!this->PendingComponents.empty()) {
    // Visit one entry from the first pending component.  The visit
    // logic will update the pending components accordingly.  Since
    // the pending components are kept in topological order this will
    // not repeat one.
    size_t e = *this->PendingComponents.begin()->second.Entries.begin();
    this->VisitEntry(e);
  }
}

void cmComputeLinkDepends::DisplayComponents()
{
  fprintf(stderr, "The strongly connected components are:\n");
  std::vector<NodeList> const& components = this->CCG->GetComponents();
  for (size_t c = 0; c < components.size(); ++c) {
    fprintf(stderr, "Component (%zu):\n", c);
    NodeList const& nl = components[c];
    for (size_t i : nl) {
      fprintf(stderr, "  item %zu [%s]\n", i,
              this->EntryList[i].Item.Value.c_str());
    }
    EdgeList const& ol = this->CCG->GetComponentGraphEdges(c);
    for (cmGraphEdge const& oi : ol) {
      size_t i = oi;
      fprintf(stderr, "  followed by Component (%zu)\n", i);
    }
    fprintf(stderr, "  topo order index %zu\n", this->ComponentOrder[c]);
  }
  fprintf(stderr, "\n");
}

void cmComputeLinkDepends::VisitComponent(size_t c)
{
  // Check if the node has already been visited.
  if (this->ComponentVisited[c]) {
    return;
  }

  // We are now visiting this component so mark it.
  this->ComponentVisited[c] = 1;

  // Visit the neighbors of the component first.
  // Run in reverse order so the topological order will preserve the
  // original order where there are no constraints.
  EdgeList const& nl = this->CCG->GetComponentGraphEdges(c);
  for (cmGraphEdge const& edge : cmReverseRange(nl)) {
    this->VisitComponent(edge);
  }

  // Assign an ordering id to this component.
  this->ComponentOrder[c] = --this->ComponentOrderId;
}

void cmComputeLinkDepends::VisitEntry(size_t index)
{
  // Include this entry on the link line.
  this->FinalLinkOrder.push_back(index);

  // This entry has now been seen.  Update its component.
  bool completed = false;
  size_t component = this->CCG->GetComponentMap()[index];
  auto mi = this->PendingComponents.find(this->ComponentOrder[component]);
  if (mi != this->PendingComponents.end()) {
    // The entry is in an already pending component.
    PendingComponent& pc = mi->second;

    // Remove the entry from those pending in its component.
    pc.Entries.erase(index);
    if (pc.Entries.empty()) {
      // The complete component has been seen since it was last needed.
      --pc.Count;

      if (pc.Count == 0) {
        // The component has been completed.
        this->PendingComponents.erase(mi);
        completed = true;
      } else {
        // The whole component needs to be seen again.
        NodeList const& nl = this->CCG->GetComponent(component);
        assert(nl.size() > 1);
        pc.Entries.insert(nl.begin(), nl.end());
      }
    }
  } else {
    // The entry is not in an already pending component.
    NodeList const& nl = this->CCG->GetComponent(component);
    if (nl.size() > 1) {
      // This is a non-trivial component.  It is now pending.
      PendingComponent& pc = this->MakePendingComponent(component);

      // The starting entry has already been seen.
      pc.Entries.erase(index);
    } else {
      // This is a trivial component, so it is already complete.
      completed = true;
    }
  }

  // If the entry completed a component, the component's dependencies
  // are now pending.
  if (completed) {
    EdgeList const& ol = this->CCG->GetComponentGraphEdges(component);
    for (cmGraphEdge const& oi : ol) {
      // This entire component is now pending no matter whether it has
      // been partially seen already.
      this->MakePendingComponent(oi);
    }
  }
}

cmComputeLinkDepends::PendingComponent&
cmComputeLinkDepends::MakePendingComponent(size_t component)
{
  // Create an entry (in topological order) for the component.
  PendingComponent& pc =
    this->PendingComponents[this->ComponentOrder[component]];
  pc.Id = component;
  NodeList const& nl = this->CCG->GetComponent(component);

  if (nl.size() == 1) {
    // Trivial components need be seen only once.
    pc.Count = 1;
  } else {
    // This is a non-trivial strongly connected component of the
    // original graph.  It consists of two or more libraries
    // (archives) that mutually require objects from one another.  In
    // the worst case we may have to repeat the list of libraries as
    // many times as there are object files in the biggest archive.
    // For now we just list them twice.
    //
    // The list of items in the component has been sorted by the order
    // of discovery in the original BFS of dependencies.  This has the
    // advantage that the item directly linked by a target requiring
    // this component will come first which minimizes the number of
    // repeats needed.
    pc.Count = this->ComputeComponentCount(nl);
  }

  // Store the entries to be seen.
  pc.Entries.insert(nl.begin(), nl.end());

  return pc;
}

size_t cmComputeLinkDepends::ComputeComponentCount(NodeList const& nl)
{
  size_t count = 2;
  for (size_t ni : nl) {
    if (cmGeneratorTarget const* target = this->EntryList[ni].Target) {
      if (cmLinkInterface const* iface =
            target->GetLinkInterface(this->Config, this->Target)) {
        if (iface->Multiplicity > count) {
          count = iface->Multiplicity;
        }
      }
    }
  }
  return count;
}

namespace {
void DisplayLinkEntry(int& count, cmComputeLinkDepends::LinkEntry const& entry)
{
  if (entry.Kind == cmComputeLinkDepends::LinkEntry::Group) {
    if (entry.Item.Value == LG_ITEM_BEGIN) {
      fprintf(stderr, "  start group");
      count = 4;
    } else if (entry.Item.Value == LG_ITEM_END) {
      fprintf(stderr, "  end group");
      count = 2;
    } else {
      fprintf(stderr, "  group");
    }
  } else if (entry.Target) {
    fprintf(stderr, "%*starget [%s]", count, "",
            entry.Target->GetName().c_str());
  } else {
    fprintf(stderr, "%*sitem [%s]", count, "", entry.Item.Value.c_str());
  }
  if (entry.Feature != cmComputeLinkDepends::LinkEntry::DEFAULT) {
    fprintf(stderr, ", feature [%s]", entry.Feature.c_str());
  }
  fprintf(stderr, "\n");
}
}

void cmComputeLinkDepends::DisplayOrderedEntries()
{
  fprintf(stderr, "target [%s] link dependency ordering:\n",
          this->Target->GetName().c_str());
  int count = 2;
  for (auto index : this->FinalLinkOrder) {
    DisplayLinkEntry(count, this->EntryList[index]);
  }
  fprintf(stderr, "\n");
}

void cmComputeLinkDepends::DisplayFinalEntries()
{
  fprintf(stderr, "target [%s] link line:\n", this->Target->GetName().c_str());
  int count = 2;
  for (LinkEntry const& entry : this->FinalLinkEntries) {
    DisplayLinkEntry(count, entry);
  }
  fprintf(stderr, "\n");
}
