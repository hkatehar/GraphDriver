#include <string>

#include "configuration.hpp"

using namespace std;

mutex _log_mutex;

static Configuration g_configuration;
Configuration& configuration(){ return g_configuration; }

Configuration::Configuration() {}

Configuration::~Configuration() {}

void Configuration::set_n_threads(int threads) {
    n_threads = threads;
}

int Configuration::get_n_threads() {
    return n_threads;
}

void Configuration::set_batch_size(uint64_t batch_size) {
    this->batch_size = batch_size;
}

uint64_t Configuration::get_batch_size() {
    return batch_size;
}

void Configuration::set_repetitions(int repetitions) {
    this->repetitions = repetitions;
}

int Configuration::get_repetitions() {
    return repetitions;
}