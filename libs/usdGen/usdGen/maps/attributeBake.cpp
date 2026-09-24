// usdGen — painted attribute maps: the .ptx bake and its worker.
// See attributeBake.h for the handoff contract.
#include "usdGen/maps/attributeBake.h"

// Ptexture.h comes from the private usdGen_ptex archive (PTEX_STATIC,
// PTEX_VENDOR=usdGen); only this translation unit (and ptexMap.cpp) sees it.
// On ELF/Mach-O its declarations are made hidden, so the Ptex code
// instantiated here stays out of libusdGen.so's dynamic table (gate B-1,
// cmake/CheckNoThirdPartyExports.cmake).
#include <cstdint>
#include <cstdio>
#if defined(__GNUC__) && !defined(_WIN32)
#  pragma GCC visibility push(hidden)
#endif
#include <Ptexture.h>
#if defined(__GNUC__) && !defined(_WIN32)
#  pragma GCC visibility pop
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace usdGen {

namespace {

namespace fs = std::filesystem;

int Log2Int(int value)
{
    int bits = 0;
    while ((1 << (bits + 1)) <= value) ++bits;
    return bits;
}

// Quad-edge adjacency, the bakePtex.cpp derivation reduced to the quad/quad
// case: manifold edges only, boundary and non-manifold edges stay (-1, 0).
// Edge e of face f runs from corner e to corner (e+1)%4.
class QuadAdjacency {
public:
    QuadAdjacency(std::vector<int> const &counts, std::vector<int> const &indices)
    {
        for (int f = 0; f < int(counts.size()); ++f) {
            for (int e = 0; e < 4; ++e) {
                uint64_t const a = uint32_t(indices[size_t(f) * 4 + size_t(e)]);
                uint64_t const b = uint32_t(indices[size_t(f) * 4 + size_t((e + 1) % 4)]);
                _uses[a < b ? (a << 32) | b : (b << 32) | a].push_back({f, e});
            }
        }
    }

    void Resolve(std::vector<int> const &indices, int face, int adjFaces[4],
                 int adjEdges[4]) const
    {
        for (int e = 0; e < 4; ++e) {
            adjFaces[e] = -1;
            adjEdges[e] = 0;
            uint64_t const a = uint32_t(indices[size_t(face) * 4 + size_t(e)]);
            uint64_t const b = uint32_t(indices[size_t(face) * 4 + size_t((e + 1) % 4)]);
            auto const found = _uses.find(a < b ? (a << 32) | b : (b << 32) | a);
            // Exactly two uses (this face plus one neighbour) is manifold.
            if (found == _uses.end() || found->second.size() != 2) continue;
            auto const &uses = found->second;
            auto const other = (uses[0].first == face && uses[0].second == e)
                                   ? uses[1]
                                   : uses[0];
            if (other.first == face) continue;  // a folded edge is no neighbour
            adjFaces[e] = other.first;
            adjEdges[e] = other.second;
        }
    }

private:
    std::unordered_map<uint64_t, std::vector<std::pair<int, int>>> _uses;
};

bool ParseVersionedName(std::string const &baseName, std::string const &fileName,
                        uint64_t *version)
{
    // baseName.v<digits>.ptx, nothing else.
    std::string const prefix = baseName + ".v";
    std::string const suffix = ".ptx";
    if (fileName.size() <= prefix.size() + suffix.size()) return false;
    if (fileName.compare(0, prefix.size(), prefix) != 0) return false;
    if (fileName.compare(fileName.size() - suffix.size(), suffix.size(), suffix) != 0)
        return false;
    std::string const digits =
        fileName.substr(prefix.size(), fileName.size() - prefix.size() - suffix.size());
    if (digits.empty() ||
        !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return false;
    try {
        *version = std::stoull(digits);
    } catch (...) {
        return false;
    }
    return true;
}

}  // namespace

std::string UsdGenAttributeBakeFileName(std::string const &baseName, uint64_t version)
{
    return baseName + ".v" + std::to_string(version) + ".ptx";
}

bool UsdGenAttributeBakePtex(UsdGenAttributeBakeInput const &input,
                             std::string const &outPath,
                             UsdGenAttributeBakeStats *stats, std::string *err)
{
    auto fail = [&](std::string const &message) {
        if (err) *err = message;
        return false;
    };
    if (!input.map) return fail("attribute bake needs a map");
    int const faces = input.map->NumFaces();
    if (input.faceVertexCounts.size() != size_t(faces))
        return fail("attribute bake topology has " +
                    std::to_string(input.faceVertexCounts.size()) + " faces for a " +
                    std::to_string(faces) + "-face map");
    for (int c : input.faceVertexCounts) {
        if (c != 4)
            return fail("attribute bake v1 needs a quad mesh (found a non-quad face)");
    }
    if (input.faceVertexIndices.size() != size_t(faces) * 4)
        return fail("attribute bake indices do not match the quad topology");
    for (int index : input.faceVertexIndices) {
        if (index < 0) return fail("attribute bake indices must be >= 0");
    }
    if (outPath.empty()) return fail("attribute bake needs an out path");

    int const res = input.map->Resolution();
    int const channels = input.map->Channels();
    // Ptex::Res takes the log2 as int8_t. A functional cast in the FaceInfo
    // initializer below is parsed as a parameter declaration (int8_t resLog2),
    // so the second argument redefines it. Store the value in that type and
    // pass the name.
    int8_t const resLog2 = int8_t(Log2Int(res));  // res is a validated power of two
    float const *src = input.map->Data();

    // The out directory is the worker's to own: create it rather than fail a
    // bake on a missing folder.
    {
        std::error_code ec;
        fs::path const parent = fs::path(outPath).parent_path();
        if (!parent.empty()) fs::create_directories(parent, ec);
        if (ec) return fail("attribute bake cannot create " + parent.string());
    }

    std::string const tmp = outPath + ".tmp";
    {
        Ptex::String error;
        PtexPtr<PtexWriter> writer(PtexWriter::open(
            tmp.c_str(), Ptex::mt_quad, Ptex::dt_float, channels, /*alphachan*/ -1,
            faces, error, /*genmipmaps*/ true));
        if (!writer)
            return fail("attribute bake cannot open " + tmp + ": " + error.c_str());
        bool ok = true;
        writer->writeMeta("usdGen:attributeBake:baseName", input.baseName.c_str());
        QuadAdjacency const adjacency(input.faceVertexCounts, input.faceVertexIndices);
        for (int f = 0; f < faces && ok; ++f) {
            int adjFaces[4], adjEdges[4];
            adjacency.Resolve(input.faceVertexIndices, f, adjFaces, adjEdges);
            Ptex::FaceInfo const info(Ptex::Res(resLog2, resLog2),
                                      adjFaces, adjEdges, /*isSubface*/ false);
            // Ptex face id == coarse face id on a quad mesh, and the texel
            // order matches the map's ((t * res) + s) grid, so each face
            // copies verbatim: texel (s, t) sits at u = (s + 0.5) / res on
            // both sides.
            ok = writer->writeFace(
                f, info, src + size_t(f) * size_t(res) * size_t(res) * size_t(channels));
        }
        ok = writer->close(error) && ok;
        if (!ok) {
            std::error_code ec;
            fs::remove(tmp, ec);
            return fail("attribute bake write failed: " + std::string(error.c_str()));
        }
    }
    std::error_code ec;
    fs::rename(tmp, outPath, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return fail("attribute bake cannot rename to " + outPath);
    }
    if (stats) {
        stats->faces = size_t(faces);
        stats->texels = size_t(faces) * size_t(res) * size_t(res);
        stats->channels = channels;
    }
    return true;
}

UsdGenAttributeBakeWorker::UsdGenAttributeBakeWorker()
{
    // _worker is the first data member, so starting it from the initializer
    // list runs _WorkerLoop before _mutex and _wake exist. Under ctest -j
    // that race throws std::system_error "Invalid argument" from the worker
    // thread and std::terminate takes the process down. Default-construct
    // the thread with the other members, then start it once they are live.
    // Same pattern as TonicBakeWorker.
    _worker = std::thread(&UsdGenAttributeBakeWorker::_WorkerLoop, this);
}

UsdGenAttributeBakeWorker::~UsdGenAttributeBakeWorker()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _stop = true;
    }
    _wake.notify_all();
    if (_worker.joinable()) _worker.join();
}

void UsdGenAttributeBakeWorker::Enqueue(uint64_t version, UsdGenAttributeBakeInput input)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_hasPending) _dropCount.fetch_add(1);  // coalesced, never baked
        _pendingVersion = version;
        _pendingInput = std::move(input);
        _hasPending = true;
        _pendingVersionAtomic.store(version);
    }
    _wake.notify_one();
}

bool UsdGenAttributeBakeWorker::TakeCompleted(uint64_t *version, std::string *path)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_hasCompleted) return false;
    if (version) *version = _completedVersion;
    if (path) *path = _completedPath;
    _hasCompleted = false;
    return true;
}

void UsdGenAttributeBakeWorker::NoteSwapped(uint64_t version)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (version > _swappedVersion) _swappedVersion = version;
        _sweepRequested = true;
    }
    _wake.notify_one();
}

std::string UsdGenAttributeBakeWorker::TakeDiagnostic()
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::string out = _diagnostic;
    _diagnostic.clear();
    return out;
}

void UsdGenAttributeBakeWorker::PauseWorker(bool pause)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _paused = pause;
    }
    _wake.notify_all();
}

bool UsdGenAttributeBakeWorker::WaitCompleted(uint64_t version, int timeoutMs)
{
    auto const start = std::chrono::steady_clock::now();
    for (;;) {
        if (_completedVersionAtomic.load() >= version) return true;
        if (std::chrono::steady_clock::now() - start >
            std::chrono::milliseconds(timeoutMs))
            return _completedVersionAtomic.load() >= version;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void UsdGenAttributeBakeWorker::_WorkerLoop()
{
    for (;;) {
        UsdGenAttributeBakeInput input;
        uint64_t version = 0;
        bool haveBake = false;
        bool sweep = false;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _wake.wait(lock, [&] {
                return _stop || (!_paused && (_hasPending || _sweepRequested));
            });
            if (_stop) return;
            if (_hasPending) {
                version = _pendingVersion;
                input = std::move(_pendingInput);
                _hasPending = false;
                haveBake = true;
            } else if (_sweepRequested) {
                sweep = true;
                _sweepRequested = false;
            }
        }
        if (sweep) {
            std::string outDir, baseName;
            uint64_t keepFrom = 0;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                outDir = _lastOutDir;
                baseName = _lastBaseName;
                keepFrom = _swappedVersion;
            }
            if (!outDir.empty()) _SweepStale(outDir, baseName, keepFrom);
            _sweepCount.fetch_add(1);
            continue;
        }
        if (!haveBake) continue;
        if (input.outDir.empty()) {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_diagnostic.empty()) _diagnostic += "; ";
            _diagnostic += "attribute bake needs an out directory";
            continue;
        }
        std::string const outPath =
            (fs::path(input.outDir) / UsdGenAttributeBakeFileName(input.baseName, version))
                .string();
        UsdGenAttributeBakeStats stats;
        std::string err;
        bool ok = false;
        try {
            ok = UsdGenAttributeBakePtex(input, outPath, &stats, &err);
        } catch (std::exception const &e) {
            err = std::string("attribute bake threw: ") + e.what();
        } catch (...) {
            err = "attribute bake threw";
        }
        std::lock_guard<std::mutex> lock(_mutex);
        _lastOutDir = input.outDir;
        _lastBaseName = input.baseName;
        if (ok) {
            _completedVersion = version;
            _completedPath = outPath;
            _hasCompleted = true;
            _completedVersionAtomic.store(version);
            _bakeCount.fetch_add(1);
        } else if (!_diagnostic.empty()) {
            _diagnostic += "; " + err;
        } else {
            _diagnostic = err;
        }
    }
}

void UsdGenAttributeBakeWorker::_SweepStale(std::string const &outDir,
                                            std::string const &baseName, uint64_t keepFrom)
{
    // Older versioned files the stage no longer references go away; the
    // swapped file and anything newer stay. Anything that is not a versioned
    // bake file is left alone.
    std::error_code ec;
    fs::directory_iterator it(outDir, ec);
    if (ec) return;
    for (fs::directory_entry const &entry : it) {
        uint64_t version = 0;
        if (!ParseVersionedName(baseName, entry.path().filename().string(), &version))
            continue;
        if (version < keepFrom) fs::remove(entry.path(), ec);
    }
}

}  // namespace usdGen
