#include <iostream>
#include <omp.h>
#include <chrono>

#include "livegraph_driver.hpp"
#include "configuration.hpp"
#include "third_party_lib/gapbs/gapbs.hpp"

using namespace lg;
using namespace std;
using namespace std::chrono;

LiveGraphDriver::LiveGraphDriver() {
    graph = new Graph();
}

Graph* LiveGraphDriver::get_graph() {
    return graph;
}

uint64_t LiveGraphDriver::int2ext(uint64_t& internal_id, Transaction& tx) {
    string_view payload = tx.get_vertex(internal_id);
    if(payload.empty()){ // the vertex does not exist
        return numeric_limits<uint64_t>::max();
    } else {
        return *(reinterpret_cast<const uint64_t*>(payload.data()));
    }
}

uint64_t LiveGraphDriver::ext2int(uint64_t& external_id, Transaction& tx) {
    VertexDictionaryAccessor a;
    bool exists = ext2int_map.find(a, external_id);
    if(!exists) return numeric_limits<uint64_t>::max(); 
    return a->second;
}

uint64_t LiveGraphDriver::ext2int_with_insert(uint64_t& external_id, Transaction& tx) {
    VertexDictionaryAccessor a;
    const auto isNew = ext2int_map.insert(a, external_id);
    // Create new vertex if new entry to hashmap
    if(isNew) {
        n_vertices++;
        auto internal_id = tx.new_vertex();
        string_view data { (char*) &external_id, sizeof(external_id) };
        tx.put_vertex(internal_id, data);
        a->second = internal_id;
    }
    return a->second;
}

bool LiveGraphDriver::vertex_exists(uint64_t& external_id, Transaction& tx) {
    VertexDictionaryAccessor a;
    return ext2int_map.find(a, external_id);
}

void LiveGraphDriver::load_graph(EdgeStream* stream, int n_threads, bool validate) {
    auto sources = stream->get_sources();
    auto destinations = stream->get_destinations();

    auto start = high_resolution_clock::now();
    auto tx = graph->begin_batch_loader();
    LOG("Number of writer Threads: " << n_threads);
    # pragma omp parallel for num_threads(n_threads)
    for(uint64_t i = 0; i < sources.size(); i++) {
        auto src_internal = ext2int_with_insert(sources[i], tx);
        auto dst_internal = ext2int_with_insert(destinations[i], tx);
        // Undirected graph, so 2 directed edges
        tx.put_edge(src_internal, 0, dst_internal, "1");
        tx.put_edge(dst_internal, 0, src_internal, "1");
        n_edges++;
    }
    tx.commit();
    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start);
    LOG("Graph loading time (in ms): " << duration.count());
    LOG("Vertices Added: " << n_vertices);

    if(validate) validate_load_graph(stream, n_threads);
}

void LiveGraphDriver::update_graph(UpdateStream* update_stream, int n_threads = 1) {
    auto updates = update_stream->get_updates();

    LOG("Number of writer Threads: " << n_threads);
    # pragma omp parallel for num_threads(n_threads)
    for(uint64_t i = 0; i < updates.size(); i++) {
        auto update = updates[i];
        if(update->insert) add_edge(update->src, 0, update->dst);
        else remove_edge(update->src, 0, update->dst);

        tmp_cnt++;
    }

    cout << "Updates Applied: " << tmp_cnt << endl;
}

bool LiveGraphDriver::add_edge(uint64_t ext_id1, uint16_t label, uint64_t ext_id2) {
    bool done = false;
    // TODO: Confirm do we need to abort
    do {
        auto tx = graph->begin_transaction();
        try{
            if(!(vertex_exists(ext_id1, tx) && vertex_exists(ext_id2, tx))) { // Vertices DNE
                tx.abort();
                return false;
            }
            auto int_id1 = ext2int(ext_id1, tx);
            auto int_id2 = ext2int(ext_id2, tx);
            tx.put_edge(int_id1, label, int_id2, "");
            tx.put_edge(int_id2, label, int_id1, "");
            tx.commit();
            done = true;
            n_edges++;
        }
        catch(lg::Transaction::RollbackExcept& e) {
            tx.abort();
        }
    } while(!done);

    return true;
}

bool LiveGraphDriver::remove_edge(uint64_t ext_id1, uint16_t label, uint64_t ext_id2) {
    bool done = false;
    // TODO: Confirm do we need to abort
    do {
        auto tx = graph->begin_transaction();
        try{
            if(!(vertex_exists(ext_id1, tx) && vertex_exists(ext_id2, tx))) { // Vertices DNE
                tx.abort();
                return false;
            }
            auto int_id1 = ext2int(ext_id1, tx);
            auto int_id2 = ext2int(ext_id2, tx);
            tx.del_edge(int_id1, label, int_id2);
            tx.del_edge(int_id2, label, int_id1);
            tx.commit();
            done = true;
            n_edges--;
        }
        catch(lg::Transaction::RollbackExcept& e) {
            tx.abort();
        }
    } while(!done);

    return true;
}

void LiveGraphDriver::update_graph_batch(UpdateStream* update_stream, uint64_t batch_size, int n_threads = 1) {
    auto updates = update_stream->get_updates();

    LOG("Batch update with threads: " << n_threads);
    LOG("Batch update with batch_size: " << batch_size);

    // uint64_t batch_size = 1ull<<20;
    uint64_t num_batches = updates.size()/batch_size + (updates.size() % batch_size != 0);
    uint64_t applied_updates = 0;

    for(uint64_t b_no = 0; b_no < num_batches; b_no++){
        LOG("Batch: " << b_no);
        uint64_t start = applied_updates;
        uint64_t end = min(applied_updates + batch_size, updates.size());

        auto tx = graph->begin_batch_loader();
        # pragma omp parallel for num_threads(n_threads)
        for(uint64_t i = start; i < end; i++) {
            auto update = updates[i];
            if(update->insert) add_edge_batch(update->src, 0, update->dst, tx);
            else remove_edge_batch(update->src, 0, update->dst, tx);
        }
        tx.commit();
        // graph->compact();

        applied_updates += batch_size;
    }

    // cout << "Updates Applied: " << tmp_cnt << endl;

    // auto updates = update_stream->get_updates();
    // auto tx = graph->begin_batch_loader();

    // LOG("Batch update with threads: " << n_threads);
    // # pragma omp parallel for num_threads(n_threads)
    // for(uint64_t i = 0; i < updates.size(); i++) {
    //     auto update = updates[i];
    //     if(update->insert) add_edge_batch(update->src, 0, update->dst, tx);
    //     else remove_edge_batch(update->src, 0, update->dst, tx);
    //     tmp_cnt++;
    // }

    // tx.commit();

    // cout << "Updates Applied: " << tmp_cnt << endl;
}

bool LiveGraphDriver::add_edge_batch(uint64_t ext_id1, uint16_t label, uint64_t ext_id2, Transaction& tx) {
    if(!(vertex_exists(ext_id1, tx) && vertex_exists(ext_id2, tx))) { // Vertices DNE
        tx.abort();
        LOG("WRONG");
        return false;
    }
    auto int_id1 = ext2int(ext_id1, tx);
    auto int_id2 = ext2int(ext_id2, tx);
    tx.put_edge(int_id1, label, int_id2, "");
    tx.put_edge(int_id2, label, int_id1, "");
    n_edges++;
    return true;
}

bool LiveGraphDriver::remove_edge_batch(uint64_t ext_id1, uint16_t label, uint64_t ext_id2, Transaction& tx) {
    if(!(vertex_exists(ext_id1, tx) && vertex_exists(ext_id2, tx))) { // Vertices DNE
        tx.abort();
        LOG("WRONG");
        return false;
    }
    auto int_id1 = ext2int(ext_id1, tx);
    auto int_id2 = ext2int(ext_id2, tx);
    tx.del_edge(int_id1, label, int_id2);
    tx.del_edge(int_id2, label, int_id1);
    n_edges--;
    return true;
}

void LiveGraphDriver::validate_load_graph(EdgeStream* stream, int n_threads) {
    LOG("Validating Load Graph");
    auto sources = stream->get_sources();
    auto destinations = stream->get_destinations();

    auto tx = graph->begin_read_only_transaction();

    # pragma omp parallel for num_threads(n_threads)
    for(uint64_t i = 0; i < sources.size(); i++) {
        auto src = sources[i];
        auto dst = destinations[i];
        if(!(vertex_exists(src, tx) && vertex_exists(dst, tx))) { // Vertices DNE
            LOG("ERROR-no-vertex: " << src << dst);
        }
        auto src_internal = ext2int(src, tx);
        auto dst_internal = ext2int(dst, tx);
        // Undirected graph, so 2 directed edges
        auto data = tx.get_edge(src_internal, 0, dst_internal);
        if(data.size() <= 0) LOG("ERROR-no-edge: " << src << "->" << dst);
        data = tx.get_edge(dst_internal, 0, src_internal);
        if(data.size() <= 0) LOG("ERROR-no-edge: " << dst << "->" << src);
    }

    tx.abort();
    LOG("Validation finished");
}


/*
    BFS
*/
static
unique_ptr<int64_t[]> do_bfs_init_distances(Transaction& tx, uint64_t max_vertex_id) {
    unique_ptr<int64_t[]> distances{ new int64_t[max_vertex_id] };
    #pragma omp parallel for
    for (uint64_t n = 0; n < max_vertex_id; n++){
        if(tx.get_vertex(n).empty()){ // the vertex does not exist
            distances[n] = numeric_limits<int64_t>::max();
        } else { // the vertex exists
            // Retrieve the out degree for the vertex n
            uint64_t out_degree = 0;
            auto iterator = tx.get_edges(n, /* label */ 0);
            while(iterator.valid()){
                out_degree++;
                iterator.next();
            }

            distances[n] = out_degree != 0 ? - out_degree : -1;
        }
    }

    return distances;
}

static
void do_bfs_QueueToBitmap(uint64_t max_vertex_id, const gapbs::SlidingQueue<int64_t> &queue, gapbs::Bitmap &bm) {
    #pragma omp parallel for
    for (auto q_iter = queue.begin(); q_iter < queue.end(); q_iter++) {
        int64_t u = *q_iter;
        bm.set_bit_atomic(u);
    }
}

static
void do_bfs_BitmapToQueue(uint64_t max_vertex_id, const gapbs::Bitmap &bm, gapbs::SlidingQueue<int64_t> &queue) {
    #pragma omp parallel
    {
        gapbs::QueueBuffer<int64_t> lqueue(queue);
        #pragma omp for
        for (uint64_t n=0; n < max_vertex_id; n++)
            if (bm.get_bit(n))
                lqueue.push_back(n);
        lqueue.flush();
    }
    queue.slide_window();
}

static
int64_t do_bfs_BUStep(Transaction& tx, uint64_t max_vertex_id, int64_t* distances, int64_t distance, gapbs::Bitmap &front, gapbs::Bitmap &next) {
    int64_t awake_count = 0;
    next.reset();

    #pragma omp parallel for schedule(dynamic, 1024) reduction(+ : awake_count)
    for (uint64_t u = 0; u < max_vertex_id; u++) {
        if(distances[u] == numeric_limits<int64_t>::max()) continue; // the vertex does not exist
        // COUT_DEBUG_BFS("explore: " << u << ", distance: " << distances[u]);

        if (distances[u] < 0){ // the node has not been visited yet
            auto iterator = tx.get_edges(u, /* label */ 0); 

            while(iterator.valid()){
                uint64_t dst = iterator.dst_id();
                // COUT_DEBUG_BFS("\tincoming edge: " << dst);

                if(front.get_bit(dst)) {
                    // COUT_DEBUG_BFS("\t-> distance updated to " << distance << " via vertex #" << dst);
                    distances[u] = distance; // on each BUStep, all nodes will have the same distance
                    awake_count++;
                    next.set_bit(u);
                    break;
                }

                iterator.next();
            }
        }
    }

    return awake_count;
}

static
int64_t do_bfs_TDStep(Transaction& tx, uint64_t max_vertex_id, int64_t* distances, int64_t distance, gapbs::SlidingQueue<int64_t>& queue) {
    int64_t scout_count = 0;

    #pragma omp parallel reduction(+ : scout_count)
    {
        gapbs::QueueBuffer<int64_t> lqueue(queue);

        #pragma omp for schedule(dynamic, 64)
        for (auto q_iter = queue.begin(); q_iter < queue.end(); q_iter++) {
            int64_t u = *q_iter;
            // COUT_DEBUG_BFS("explore: " << u);
            auto iterator = tx.get_edges(u, /* label */ 0);
            while(iterator.valid()){
                uint64_t dst = iterator.dst_id();
                // COUT_DEBUG_BFS("\toutgoing edge: " << dst);

                int64_t curr_val = distances[dst];
                if (curr_val < 0 && gapbs::compare_and_swap(distances[dst], curr_val, distance)) {
                    // COUT_DEBUG_BFS("\t-> distance updated to " << distance << " via vertex #" << dst);
                    lqueue.push_back(dst);
                    scout_count += -curr_val;
                }

                iterator.next();
            }
        }

        lqueue.flush();
    }

    return scout_count;
}

unique_ptr<int64_t[]> LiveGraphDriver::execute_bfs(uint64_t ext_root, int alpha, int beta) {
    int64_t edges_to_check = n_edges;
    int64_t num_vertices = n_vertices;
    uint64_t max_vertex_id = graph->get_max_vertex_id();

    auto tx = graph->begin_read_only_transaction();
    uint64_t root = ext2int_with_insert(ext_root, tx);

    unique_ptr<int64_t[]> ptr_distances = do_bfs_init_distances(tx, max_vertex_id);
    int64_t* __restrict distances = ptr_distances.get();
    distances[root] = 0;

    gapbs::SlidingQueue<int64_t> queue(max_vertex_id);
    queue.push_back(root);
    queue.slide_window();
    gapbs::Bitmap curr(max_vertex_id);
    curr.reset();
    gapbs::Bitmap front(max_vertex_id);
    front.reset();

    int64_t scout_count = 0;
    { // retrieve the out degree of the root
        auto iterator = tx.get_edges(root, 0);
        while(iterator.valid()){ scout_count++; iterator.next(); }
    }
    int64_t distance = 1; // current distance

    while (!queue.empty()) {

        if (scout_count > edges_to_check / alpha) {
            int64_t awake_count, old_awake_count;
            do_bfs_QueueToBitmap(max_vertex_id, queue, front);
            awake_count = queue.size();
            queue.slide_window();
            do {
                old_awake_count = awake_count;
                awake_count = do_bfs_BUStep(tx, max_vertex_id, distances, distance, front, curr);
                front.swap(curr);
                distance++;
            } while ((awake_count >= old_awake_count) || (awake_count > (int64_t) num_vertices / beta));
            do_bfs_BitmapToQueue(max_vertex_id, front, queue);
            scout_count = 1;
        } else {
            edges_to_check -= scout_count;
            scout_count = do_bfs_TDStep(tx, max_vertex_id, distances, distance, queue);
            queue.slide_window();
            distance++;
        }
    }

    // print_bfs_output(ptr_distances.get(), get_graph()->get_max_vertex_id(), tx);
    tx.abort();

    return ptr_distances;
}

void LiveGraphDriver::print_bfs_output(int64_t* dist, uint64_t max_vertex_id, Transaction& tx){
    for(uint64_t logical_id = 0; logical_id < max_vertex_id; logical_id++){
        uint64_t external_id = int2ext(logical_id, tx);
        if(external_id == numeric_limits<uint64_t>::max()) { // the vertex does not exist
            LOG(external_id << ": " << -1);
        } else {
            LOG(external_id << ": " << dist[logical_id]);
        }
    }
}