#include "update_stream.hpp"
#include "../reader/log_reader.hpp"
#include "../configuration.hpp"

EdgeUpdate::EdgeUpdate() {}
EdgeUpdate::EdgeUpdate(uint64_t src, uint64_t dst, bool insert) : src(src), dst(dst), insert(insert) {}
EdgeUpdate::~EdgeUpdate() {}

UpdateStream::UpdateStream() { }
UpdateStream::~UpdateStream() { }

UpdateStream::UpdateStream(const string& path) {
    LOG("Using log path: " << path);
    LogReader reader {path};
    
    uint64_t src = -1, dst = -1;
    double wt = -1;
    uint64_t limit = 100000000;
    while(reader.read_edge(src, dst, wt) && limit > 0) {
        EdgeUpdate update {src, dst, wt==0.0};
        updates.push_back(update);
        limit--;
    }
}

void UpdateStream::add_update(EdgeUpdate update) {
    updates.push_back(update);
}

vector<EdgeUpdate>& UpdateStream::get_updates() {
    // return vector<EdgeUpdate>(updates.begin(), updates.begin() + 1000);
    return updates;
}