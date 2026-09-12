// Isolated metadata/order coverage for the private Storm group-staging
// plugin.  The test process discovers Groom and the staged hdSt metadata via
// PXR_PLUGINPATH_NAME; it does not manually append the staging index.
#include "pxr/pxr.h"

#include "pxr/base/arch/demangle.h"
#include "pxr/base/tf/errorMark.h"
#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/imaging/hd/retainedSceneIndex.h"
#include "pxr/imaging/hd/sceneIndexPluginRegistry.h"
#include "pxr/imaging/hdsi/sceneGlobalsSceneIndex.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

int failures = 0;

void
Check(bool const condition, char const *const message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

struct _Node {
    std::string type;
    std::string display;
    HdSceneIndexBaseRefPtr sceneIndex;
};

std::string
_ShortName(HdSceneIndexBaseRefPtr const &sceneIndex)
{
    if (!sceneIndex) {
        return "<null>";
    }
    std::string type = ArchGetDemangled(typeid(*sceneIndex).name());
    size_t const separator = type.rfind("::");
    return separator == std::string::npos ? type : type.substr(separator + 1);
}

std::vector<_Node>
_Walk(HdSceneIndexBaseRefPtr const &terminal)
{
    std::vector<_Node> result;
    std::vector<HdSceneIndexBaseRefPtr> pending;
    std::set<HdSceneIndexBase const *> visited;
    if (terminal) {
        pending.push_back(terminal);
    }
    for (size_t i = 0; i != pending.size(); ++i) {
        HdSceneIndexBaseRefPtr const &node = pending[i];
        if (!visited.insert(node.operator->()).second) {
            continue;
        }
        result.push_back({_ShortName(node), node->GetDisplayName(), node});
        auto const *filter = dynamic_cast<HdFilteringSceneIndexBase const *>(
            node.operator->());
        if (!filter) {
            continue;
        }
        for (HdSceneIndexBaseRefPtr const &input : filter->GetInputScenes()) {
            if (input) {
                pending.push_back(input);
            }
        }
    }
    return result;
}

std::vector<HdSceneIndexBaseRefPtr>
_FindAll(std::vector<_Node> const &nodes, char const *needle)
{
    std::vector<HdSceneIndexBaseRefPtr> result;
    for (size_t i = 0; i != nodes.size(); ++i) {
        if (nodes[i].type.find(needle) != std::string::npos ||
            nodes[i].display.find(needle) != std::string::npos) {
            result.push_back(nodes[i].sceneIndex);
        }
    }
    return result;
}

bool
_HasInputPath(HdSceneIndexBaseRefPtr const &downstream,
              HdSceneIndexBase const *const upstream)
{
    if (!downstream) {
        return false;
    }
    if (downstream.operator->() == upstream) {
        return true;
    }
    auto const *filter = dynamic_cast<HdFilteringSceneIndexBase const *>(
        downstream.operator->());
    if (!filter) {
        return false;
    }
    for (HdSceneIndexBaseRefPtr const &input : filter->GetInputScenes()) {
        if (_HasInputPath(input, upstream)) {
            return true;
        }
    }
    return false;
}

HdSceneIndexBaseRefPtr
_Build()
{
    HdSceneIndexBaseRefPtr const input = HdRetainedSceneIndex::New();
    HdSceneIndexBaseRefPtr const globals = HdsiSceneGlobalsSceneIndex::New(input);
    return HdSceneIndexPluginRegistry::GetInstance().AppendSceneIndicesForRenderer(
        "GL", globals);
}

void
_CheckOrder(char const *label)
{
    std::vector<_Node> const nodes = _Walk(_Build());
    std::fprintf(stderr, "%s chain (%zu nodes):\n", label, nodes.size());
    for (size_t i = 0; i != nodes.size(); ++i) {
        std::fprintf(stderr, "  %zu %s%s%s\n", i, nodes[i].type.c_str(),
                     nodes[i].display.empty() ? "" : " [",
                     nodes[i].display.empty() ? "" :
                         (nodes[i].display + "]").c_str());
    }

    std::vector<HdSceneIndexBaseRefPtr> const groom =
        _FindAll(nodes, "UsdGenGroomSceneIndex");
    std::vector<HdSceneIndexBaseRefPtr> const staging =
        _FindAll(nodes, "HdStBasisCurvesGpuGroupStagingSceneIndex");
    std::vector<HdSceneIndexBaseRefPtr> const pruning =
        _FindAll(nodes, "HdsiUnboundMaterialPruningSceneIndex");
    Check(groom.size() == 1, "exactly one Groom phase-0 node");
    Check(staging.size() == 1, "exactly one GPU group staging node");
    Check(pruning.size() == 1, "exactly one unbound-material pruning node");
    if (groom.size() == 1 && staging.size() == 1 && pruning.size() == 1) {
        // Assert real input edges, not a breadth-first presentation order:
        // prune <- staging <- Groom on the same renderer branch.
        Check(_HasInputPath(pruning.front(), staging.front().operator->()),
              "GPU group staging is an upstream input of material pruning");
        Check(_HasInputPath(staging.front(), groom.front().operator->()),
              "Groom is an upstream input of GPU group staging");
    }
}

} // anonymous namespace

int
main()
{
    TfErrorMark errors;
    HdSceneIndexPluginRegistry::GetInstance().SetPluginOrderingPolicy(
        HdSceneIndexPluginRegistry::PluginOrderingPolicy::Hybrid);
    _CheckOrder("Hybrid");

    HdSceneIndexPluginRegistry::GetInstance().SetPluginOrderingPolicy(
        HdSceneIndexPluginRegistry::PluginOrderingPolicy::JsonMetadataOnly);
    _CheckOrder("JsonMetadataOnly");

    Check(errors.IsClean(), "plugin discovery and chain construction are clean");
    return failures == 0 ? 0 : 1;
}
