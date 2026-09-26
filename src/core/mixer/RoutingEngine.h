#pragma once

#include <cstdint>
#include <vector>

#include "MixerTypes.h"

namespace onda::mixer {

/// Static routing graph for the mixer. Nodes describe the processing
/// elements; edges describe which taps feed which bus. The MixerEngine
/// builds the default topology at prepare() and the render loop follows
/// it; new sources (sampler, external in, FX returns, mic) are added by
/// registering extra nodes/edges — no structural rewrite needed.
///
/// This is an introspection/extension container: it does not run audio
/// itself, it just keeps the connection description in one place so
/// future routing UI / DVS / FX-send work has a single source of truth.
class RoutingEngine {
public:
    enum class NodeType : uint8_t {
        Source,      // deck, sampler, external in, mic...
        ChannelStrip,
        Crossfader,
        MasterBus,
        CueBus,
        FxSend,      // reserved
        FxReturn,    // reserved
        Output,
    };

    enum class Tap : uint8_t {
        PostFader = 0, // channel output -> crossfader -> master
        PreFader  = 1, // channel PFL tap -> cue bus
        PostStrip = 2, // post-strip pre-fader (reserved for FX sends)
    };

    struct Node {
        int      id;
        NodeType type;
        int      index;    // channel/source index, or -1
        uint32_t tag;      // free-form (e.g. 'deck','smpl','ext ','mic ')
    };
    struct Edge {
        int fromId, toId;
        Tap tap;
    };

    void clear() noexcept { nodes_.clear(); edges_.clear(); }

    int addNode(NodeType t, int index, uint32_t tag) noexcept {
        const int id = (int)nodes_.size();
        nodes_.push_back({ id, t, index, tag });
        return id;
    }
    void addEdge(int from, int to, Tap tap) noexcept {
        edges_.push_back({ from, to, tap });
    }

    const std::vector<Node>& nodes() const noexcept { return nodes_; }
    const std::vector<Edge>& edges() const noexcept { return edges_; }

    int findNode(NodeType t, int index) const noexcept {
        for (const auto& n : nodes_)
            if (n.type == t && n.index == index) return n.id;
        return -1;
    }

private:
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
};

} // namespace onda::mixer
