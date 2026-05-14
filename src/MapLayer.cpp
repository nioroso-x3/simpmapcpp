#include "MapLayer.h"

std::string LayerStore::add(std::shared_ptr<MapLayer> layer) {
    if (!layer) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    if (layer->id.empty()) {
        layer->id = "layer-" + std::to_string(next_auto_id_++);
    }
    layers_.push_back(std::move(layer));
    return layers_.back()->id;
}

bool LayerStore::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = layers_.begin(); it != layers_.end(); ++it) {
        if ((*it)->id == id) {
            layers_.erase(it);
            return true;
        }
    }
    return false;
}

void LayerStore::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    layers_.clear();
}

std::vector<std::shared_ptr<MapLayer>> LayerStore::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return layers_;
}

std::shared_ptr<MapLayer> LayerStore::get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& l : layers_) {
        if (l->id == id) return l;
    }
    return nullptr;
}
