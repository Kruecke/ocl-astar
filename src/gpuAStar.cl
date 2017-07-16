// GPU A* program

typedef struct {
    uint  x;
    float y;
} uint_float;

void push(__local uint_float *heap, size_t *size, uint value, float cost) {
    size_t index = (*size)++;

    while (index > 0) {
        size_t parent = (index - 1) / 2;

        if (cost < heap[parent].y) {
            heap[index] = heap[parent];
            index = parent;
        } else
            break;
    }

    heap[index] = (uint_float){value, cost};
}

void pop(__local uint_float *heap, size_t *size) {
    uint_float value = heap[--(*size)];
    size_t     index = 0;

    while (index < *size / 2) {
        size_t child = index * 2 + 1;

        if (child + 1 < *size && heap[child + 1].y < heap[child].y)
            ++child;

        if (heap[child].y < value.y) {
            heap[index] = heap[child];
            index = child;
        } else
            break;
    }

    heap[index] = value;
}

// Debugging / testing
bool is_heap(__local uint_float *heap, size_t *size) {
    for (size_t index = 0; index < *size / 2; ++index) {
        size_t left  = index * 2 + 1;
        size_t right = index * 2 + 2;

        if (heap[left].y < heap[index].y)
            return false;
        if (right < *size && heap[right].y < heap[index].y)
            return false;
    }

    return true;
}

__kernel void gpuAStar(__global const int2       *nodes,            // x, y
                                const ulong       nodesSize,
                       __global const uint_float *edges,            // destination index, cost
                                const ulong       edgesSize,
                       __global const uint2      *adjacencyMap,     // edges_begin, edges_end
                                const ulong       adjacencyMapSize,
                                const ulong       numberOfAgents,
                       __global const uint2      *srcDstList,       // source id, destination id
                       __global       int2       *paths,            // x, y; offset = GID * maxPathLength
                                const ulong       maxPathLength,
                       __local        uint_float *open,             // id, cost
                       __local        float      *info)             // total cost; or -1.0f ^= visited
{
    const size_t GID = get_global_id(0);
    size_t openSize = 0;

    if (GID >= numberOfAgents)
        return;

    const uint source      = srcDstList[GID].x;
    const uint destination = srcDstList[GID].y;

    // Initialize result in case no path is found.
    paths[GID * maxPathLength] = nodes[destination];

    // Begin at source
    push(open, &openSize, source, 0.0f);

    while (openSize > 0) {
        uint current = open[0].x;
        pop(open, &openSize);

        // TODO...
    }
}
