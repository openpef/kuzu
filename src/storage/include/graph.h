#pragma once

#include <iostream>
#include <vector>

#include "src/storage/include/buffer_manager.h"
#include "src/storage/include/store/nodes_store.h"
#include "src/storage/include/store/rels_store.h"

using namespace std;

namespace spdlog {
class logger;
}

namespace graphflow {
namespace loader {

class GraphLoader;
class RelsLoader;

} // namespace loader
} // namespace graphflow

namespace graphflow {
namespace storage {

class Graph {
    friend class graphflow::loader::GraphLoader;
    friend class graphflow::loader::RelsLoader;
    friend class bitsery::Access;

public:
    Graph(const string& path, uint64_t bufferPoolSize = DEFAULT_BUFFER_POOL_SIZE);

    virtual ~Graph();

    virtual inline const Catalog& getCatalog() const { return *catalog; }

    inline const RelsStore& getRelsStore() const { return *relsStore; }

    inline const NodesStore& getNodesStore() const { return *nodesStore; }

    inline const vector<uint64_t>& getNumNodesPerLabel() const { return numNodesPerLabel; };

    virtual inline uint64_t getNumNodes(label_t label) const { return numNodesPerLabel[label]; }

    virtual inline uint64_t getNumRelsForDirBoundLabelRelLabel(
        Direction direction, label_t boundNodeLabel, label_t relLabel) const {
        return numRelsPerDirBoundLabelRelLabel[FWD == direction ? 0 : 1][boundNodeLabel][relLabel];
    }

    inline const string& getPath() const { return path; }

    unique_ptr<nlohmann::json> debugInfo();

protected:
    Graph();

private:
    template<typename S>
    void serialize(S& s);

    void saveToFile(const string& directory);
    void readFromFile(const string& directory);

private:
    shared_ptr<spdlog::logger> logger;
    const string path;

    unique_ptr<Catalog> catalog;
    unique_ptr<NodesStore> nodesStore;
    unique_ptr<RelsStore> relsStore;
    unique_ptr<BufferManager> bufferManager;

    vector<uint64_t> numNodesPerLabel;
    vector<vector<vector<uint64_t>>> numRelsPerDirBoundLabelRelLabel;
};

} // namespace storage
} // namespace graphflow
