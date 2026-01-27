#include "engine_factory.hpp"

std::unique_ptr<BaseEngine> make_engine(const Options& opts) {
    if (opts.engine == Options::EngineType::RealESRGAN) {
        auto engine = std::make_unique<RealESRGANEngine>();
        if (engine->init(opts)) {
            return engine;
        }
    } else {
        auto engine = std::make_unique<RealCUGANEngine>();
        if (engine->init(opts)) {
            return engine;
        }
    }
    return nullptr;
}
