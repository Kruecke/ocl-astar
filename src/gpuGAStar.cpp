#define BOOST_COMPUTE_DEBUG_KERNEL_COMPILATION

#include "astar.h"

#include <algorithm>
#include <boost/compute.hpp>
#include <cassert>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

//#define DEBUG_LISTS

namespace {
// Helper for pritty printing bytes
std::string bytes(unsigned long long bytes) {
    if (bytes > (1 << 20))
        return std::to_string(bytes >> 20) + " MBytes";
    if (bytes > (1 << 10))
        return std::to_string(bytes >> 10) + " KBytes";
    return std::to_string(bytes) + " bytes";
}
} // namespace

std::vector<Node> gpuGAStar(const Graph &graph, const Position &source, const Position &destination,
                            const boost::compute::device &clDevice) {
    namespace compute = boost::compute;

    // Check 64 bit atomic capability
    const auto        extensions = clDevice.extensions();
    const std::string requiredExtension = "cl_khr_int64_base_atomics";
    if (std::find(extensions.begin(), extensions.end(), requiredExtension) == extensions.end())
        throw std::logic_error("CL device " + clDevice.name() + " does not support extension " +
                               requiredExtension);

    // Just so we don't have to handle this case in the kernels...
    if (source == destination)
        return {{graph, destination}};

    const std::size_t numberOfQueues = 16; // TODO: How to pick this number?
    const std::size_t sizeOfAQueue =
        (std::size_t)(1 << (int) std::ceil(std::log2((double) graph.size() / numberOfQueues)));
    assert(sizeOfAQueue <= std::numeric_limits<compute::uint_>::max());

#ifdef GRAPH_DIAGONAL_MOVEMENT
    const std::size_t maxSuccessorsPerNode = 8;
#else
    const std::size_t maxSuccessorsPerNode = 4;
#endif

#ifdef DEBUG_OUTPUT
    const auto maxMemAllocSize = clDevice.get_info<CL_DEVICE_MAX_MEM_ALLOC_SIZE>();
    const auto maxWorkGroupSize = clDevice.get_info<CL_DEVICE_MAX_WORK_GROUP_SIZE>();
    const auto maxWorkItemDimensions = clDevice.get_info<CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS>();
    const auto maxWorkItemSizes = clDevice.get_info<CL_DEVICE_MAX_WORK_ITEM_SIZES>();

    std::cout << "OpenCL device: " << clDevice.name()
              << "\n - Compute units: " << clDevice.compute_units()
              << "\n - Global memory: " << bytes(clDevice.global_memory_size())
              << "\n - Local memory: " << bytes(clDevice.local_memory_size())
              << "\n - Max. memory allocation: " << bytes(maxMemAllocSize)
              << "\n - Max. work group size: " << maxWorkGroupSize << "\n - Max. work item sizes:";
    for (unsigned i = 0; i < maxWorkItemDimensions; ++i)
        std::cout << ' ' << maxWorkItemSizes[i];
    std::cout << std::endl;
#endif

    // Set up OpenCL environment and build program
    compute::context       context(clDevice);
    compute::command_queue queue(context, clDevice);

    auto program = compute::program::create_with_source_file("src/gpuGAStar.cl", context);
    program.build();

    // Set up data structures on host
    // Let's just use similar strucutures to the other GPU A* implementation.
    using uint_float = std::pair<compute::uint_, compute::float_>;
    std::vector<compute::int2_>  h_nodes;        // x, y
    std::vector<uint_float>      h_edges;        // destination index, cost
    std::vector<compute::uint2_> h_adjacencyMap; // edges_begin, edges_end

    // Convert graph data
    auto index = [width = graph.width()](int x, int y) { return y * width + x; };

    for (int y = 0; y < graph.height(); ++y) {
        for (int x = 0; x < graph.width(); ++x) {
            h_nodes.emplace_back(x, y);

            const Node current(graph, x, y);
            const auto begin = h_edges.size();

            for (const auto &neighbor : current.neighbors()) {
                const auto &nbPosition = neighbor.first.position();
                const float nbCost = neighbor.second;

                h_edges.emplace_back(index(nbPosition.x, nbPosition.y), nbCost);
            }

            const auto end = h_edges.size();
            assert(begin <= std::numeric_limits<compute::uint_>::max());
            assert(end <= std::numeric_limits<compute::uint_>::max());
            h_adjacencyMap.emplace_back((compute::uint_) begin, (compute::uint_) end);
        }
    }

    // Device memory
    const std::size_t maxPathLength = 2 * (graph.width() + graph.height()); // TODO: correct size
    compute::vector<compute::int2_>  d_nodes(h_nodes.size(), context);
    compute::vector<uint_float>      d_edges(h_edges.size(), context);
    compute::vector<compute::uint2_> d_adjacencyMap(h_adjacencyMap.size(), context);
    compute::vector<uint_float>      d_openLists(numberOfQueues * sizeOfAQueue, context);
    compute::vector<compute::uint_>  d_openSizes(numberOfQueues, context);

    // std::tuple<...> has it's members in inverse order! :(
    struct Info {
        compute::uint_  closed; // only one of the first two members is used here
        compute::uint_  node;   // the other one gives padding for memory alignment
        compute::float_ totalCost;
        compute::uint_  predecessor;
    };
    static_assert(sizeof(Info) == sizeof(compute::uint4_), "Type size check failed!");

    compute::vector<Info>           d_info(h_nodes.size(), context);
    compute::vector<Info>           d_slistChunks(numberOfQueues * maxSuccessorsPerNode, context);
    compute::vector<compute::uint_> d_slistSizes(numberOfQueues, context);
    compute::vector<Info>           d_tlistChunks(numberOfQueues * maxSuccessorsPerNode, context);
    compute::vector<compute::uint_> d_tlistSizes(numberOfQueues, context);

    compute::vector<compute::uint_> d_returnCode(1, context);

#ifdef DEBUG_OUTPUT
    std::cout << "Global memory used:"
              << "\n - Nodes: " << bytes(h_nodes.size() * sizeof(compute::int2_))
              << "\n - Edges: " << bytes(h_edges.size() * sizeof(uint_float))
              << "\n - Adjacency map: " << bytes(h_adjacencyMap.size() * sizeof(compute::uint2_))
              << "\n - Open lists: " << bytes(d_openLists.size() * sizeof(uint_float))
              << "\n - Open list sizes: " << bytes(d_openSizes.size() * sizeof(compute::uint_))
              << "\n - Info table: " << bytes(d_info.size() * sizeof(Info))
              << "\n - \"S\"-list chunks: " << bytes(d_slistChunks.size() * sizeof(Info))
              << "\n - \"S\"-list sizes: " << bytes(d_slistSizes.size() * sizeof(compute::uint_))
              << "\n - \"T\"-list chunks: " << bytes(d_tlistChunks.size() * sizeof(Info))
              << "\n - \"T\"-list sizes: " << bytes(d_tlistSizes.size() * sizeof(compute::uint_))
              << std::endl;
#endif

    // Create kernels
    compute::kernel clearSList(program, "clearList");
    compute::kernel extractAndExpand(program, "extractAndExpand");
    compute::kernel clearTList(program, "clearList");
    compute::kernel duplicateDetection(program, "duplicateDetection");
    compute::kernel computeAndPushBack(program, "computeAndPushBack");

    // Set kernel arguments
    clearSList.set_arg(0, d_slistSizes);
    clearSList.set_arg<compute::ulong_>(1, d_slistSizes.size());

    extractAndExpand.set_arg(0, d_edges);
    extractAndExpand.set_arg<compute::ulong_>(1, d_edges.size());
    extractAndExpand.set_arg(2, d_adjacencyMap);
    extractAndExpand.set_arg<compute::ulong_>(3, d_adjacencyMap.size());
    extractAndExpand.set_arg<compute::ulong_>(4, numberOfQueues);
    extractAndExpand.set_arg<compute::ulong_>(5, sizeOfAQueue);
    extractAndExpand.set_arg<compute::uint_>(6, index(destination.x, destination.y));
    extractAndExpand.set_arg(7, d_openLists);
    extractAndExpand.set_arg(8, d_openSizes);
    extractAndExpand.set_arg(9, d_info);
    extractAndExpand.set_arg(10, d_slistChunks);
    extractAndExpand.set_arg(11, d_slistSizes);
    extractAndExpand.set_arg<compute::ulong_>(12, maxSuccessorsPerNode);
    extractAndExpand.set_arg(13, d_returnCode);

    clearTList.set_arg(0, d_tlistSizes);
    clearTList.set_arg<compute::ulong_>(1, d_tlistSizes.size());

    duplicateDetection.set_arg<compute::ulong_>(0, numberOfQueues);
    duplicateDetection.set_arg(1, d_info);
    duplicateDetection.set_arg(2, d_slistChunks);
    duplicateDetection.set_arg(3, d_slistSizes);
    duplicateDetection.set_arg<compute::ulong_>(4, maxSuccessorsPerNode);
    duplicateDetection.set_arg(5, d_tlistChunks);
    duplicateDetection.set_arg(6, d_tlistSizes);

    computeAndPushBack.set_arg(0, d_nodes);
    computeAndPushBack.set_arg<compute::ulong_>(1, d_nodes.size());
    computeAndPushBack.set_arg<compute::ulong_>(2, numberOfQueues);
    computeAndPushBack.set_arg<compute::ulong_>(3, sizeOfAQueue);
    computeAndPushBack.set_arg<compute::uint_>(4, index(destination.x, destination.y));
    computeAndPushBack.set_arg(5, d_openLists);
    computeAndPushBack.set_arg(6, d_openSizes);
    computeAndPushBack.set_arg(7, d_info);
    computeAndPushBack.set_arg(8, d_tlistChunks);
    computeAndPushBack.set_arg(9, d_tlistSizes);
    computeAndPushBack.set_arg<compute::ulong_>(10, maxSuccessorsPerNode);

    // Data initialization
    std::vector<uint_float>     h_openLists(1, std::make_pair(index(source.x, source.y), 0.0f));
    std::vector<compute::uint_> h_openSizes(d_openSizes.size(), 0);
    h_openSizes.front() = 1; // only the first list contains one node: source

    std::vector<Info> h_info(d_info.size(), {0, 0, 0.0f, 0});
    const auto        sourceIndex = index(source.x, source.y);
    h_info[sourceIndex].closed = 1;                // close source node
    h_info[sourceIndex].predecessor = sourceIndex; // source is it's own predecessor

    // Upload data
    const auto uploadStart = std::chrono::high_resolution_clock::now();
    compute::copy(h_nodes.begin(), h_nodes.end(), d_nodes.begin(), queue);
    compute::copy(h_edges.begin(), h_edges.end(), d_edges.begin(), queue);
    compute::copy(h_adjacencyMap.begin(), h_adjacencyMap.end(), d_adjacencyMap.begin(), queue);
    compute::copy(h_openLists.begin(), h_openLists.end(), d_openLists.begin(), queue); // source
    compute::copy(h_openSizes.begin(), h_openSizes.end(), d_openSizes.begin(), queue);
    compute::copy(h_info.begin(), h_info.end(), d_info.begin(), queue);
    const auto uploadStop = std::chrono::high_resolution_clock::now();

    // TODO: Figure these out!
    const std::size_t globalWorkSize[2] = {numberOfQueues, maxSuccessorsPerNode};
    const std::size_t localWorkSize[2] = {numberOfQueues / maxSuccessorsPerNode,
                                          maxSuccessorsPerNode};

    // Run kernels
    const auto     kernelsStart = std::chrono::high_resolution_clock::now();
    compute::uint_ h_returnCode = 1; // still running
    while (h_returnCode == 1) {
        h_returnCode = 2; // no path found, as initial value
        compute::copy(&h_returnCode, std::next(&h_returnCode), d_returnCode.begin(), queue);
        queue.enqueue_1d_range_kernel(clearSList, 0, globalWorkSize[0], localWorkSize[0]);
        queue.enqueue_1d_range_kernel(extractAndExpand, 0, globalWorkSize[0], localWorkSize[0]);
        compute::copy(d_returnCode.begin(), d_returnCode.end(), &h_returnCode, queue);

#ifdef DEBUG_LISTS
        std::vector<Info>           h_slistChunks(d_slistChunks.size());
        std::vector<compute::uint_> h_slistSizes(d_slistSizes.size());
        compute::copy(d_slistChunks.begin(), d_slistChunks.end(), h_slistChunks.begin(), queue);
        compute::copy(d_slistSizes.begin(), d_slistSizes.end(), h_slistSizes.begin(), queue);
        queue.finish();

        for (std::size_t i = 0; i < h_slistSizes.size(); ++i) {
            const auto begin = h_slistChunks.begin() + i * maxSuccessorsPerNode;
            const auto end = begin + h_slistSizes[i];
            std::cout << "S-chunk " << i << ":";
            for (auto it = begin; it != end; ++it)
                std::cout << " (" << it->node << ", " << it->totalCost << ", " << it->predecessor
                          << ")";
            std::cout << "\n";
        }
#endif

        queue.enqueue_1d_range_kernel(clearTList, 0, globalWorkSize[0], localWorkSize[0]);
        queue.enqueue_nd_range_kernel(duplicateDetection, 2, 0, globalWorkSize, localWorkSize);

#ifdef DEBUG_LISTS
        std::vector<Info>           h_tlistChunks(d_slistChunks.size());
        std::vector<compute::uint_> h_tlistSizes(d_slistSizes.size());
        compute::copy(d_tlistChunks.begin(), d_tlistChunks.end(), h_tlistChunks.begin(), queue);
        compute::copy(d_tlistSizes.begin(), d_tlistSizes.end(), h_tlistSizes.begin(), queue);
        queue.finish();

        for (std::size_t i = 0; i < h_tlistSizes.size(); ++i) {
            const auto begin = h_tlistChunks.begin() + i * maxSuccessorsPerNode;
            const auto end = begin + h_tlistSizes[i];
            std::cout << "T-chunk " << i << ":";
            for (auto it = begin; it != end; ++it)
                std::cout << " (" << it->node << ", " << it->totalCost << ", " << it->predecessor
                          << ")";
            std::cout << "\n";
        }
#endif

        queue.enqueue_1d_range_kernel(computeAndPushBack, 0, globalWorkSize[0], localWorkSize[0]);

#ifdef DEBUG_LISTS
        std::vector<uint_float>     h_openLists(d_openLists.size());
        std::vector<compute::uint_> h_openSizes(d_openSizes.size());
        compute::copy(d_openLists.begin(), d_openLists.end(), h_openLists.begin(), queue);
        compute::copy(d_openSizes.begin(), d_openSizes.end(), h_openSizes.begin(), queue);
        queue.finish();

        for (std::size_t i = 0; i < h_openSizes.size(); ++i) {
            const auto begin = h_openLists.begin() + i * sizeOfAQueue;
            const auto end = begin + h_openSizes[i];
            std::cout << "Open list " << i << ":";
            for (auto it = begin; it != end; ++it)
                std::cout << " (" << it->first << ", " << it->second << ")";
            std::cout << "\n";
        }

        // DEBUG: Detect queue overflow
        if (std::any_of(h_openSizes.begin(), h_openSizes.end(),
                        [&](compute::uint_ size) { return size >= sizeOfAQueue; }))
            throw std::overflow_error("Open list overflow!");
#endif

        queue.finish(); // make sure we have the returnCode downloaded
        // std::cout << "Return code: " << h_returnCode << std::endl;
    }
    const auto kernelsStop = std::chrono::high_resolution_clock::now();

    // Download data
    const auto downloadStart = std::chrono::high_resolution_clock::now();
    compute::copy(d_info.begin(), d_info.end(), h_info.begin(), queue);
    const auto downloadStop = std::chrono::high_resolution_clock::now();

    std::vector<Node> path;
    if (h_returnCode == 0) {
        // Recreate path
        compute::uint_ nodeIndex = index(destination.x, destination.y);
        compute::uint_ predecessor = h_info[nodeIndex].predecessor;

        while (nodeIndex != predecessor) {
            const auto node = h_nodes[nodeIndex];
            path.emplace_back(graph, node[0], node[1]);

            nodeIndex = predecessor;
            predecessor = h_info[nodeIndex].predecessor;
        }
        const auto node = h_nodes[nodeIndex];
        path.emplace_back(graph, node[0], node[1]);

        // Path is in inverse order. Reverse it.
        std::reverse(path.begin(), path.end());
        assert(path.front().position() == source);
        assert(path.back().position() == destination);
    }

    // Print timings
    std::cout << "GPU time for graph (" << graph.width() << ", " << graph.height() << "):"
              << "\n - Upload time: "
              << std::chrono::duration<double>(uploadStop - uploadStart).count() << " seconds"
              << "\n - Kernels runtime: "
              << std::chrono::duration<double>(kernelsStop - kernelsStart).count() << " seconds"
              << "\n - Download time: "
              << std::chrono::duration<double>(downloadStop - downloadStart).count() << " seconds"
              << std::endl;

    return path;
}
