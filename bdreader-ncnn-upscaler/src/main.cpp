#include "engine_factory.hpp"
#include "modes/batch_mode.hpp"
#include "modes/file_mode.hpp"
#include "modes/stdin_mode.hpp"
#include "options.hpp"
#include "utils/logger.hpp"

#include <memory>
#include <iostream>

int main(int argc, char** argv) {
    Options opts;
    if (!parse_options(argc, argv, opts)) {
        return 1;
    }

    logger::set_level((opts.verbose || opts.profiling) ? logger::Level::Info : logger::Level::Warn);

    auto engine = make_engine(opts);
    if (!engine) {
        logger::error("Failed to initialize engine");
        return 1;
    }

    int exit_code = 0;
    switch (opts.mode) {
        case Options::Mode::File:
            exit_code = run_file_mode(engine.get(), opts);
            break;
        case Options::Mode::Stdin:
            exit_code = run_stdin_mode(engine.get(), opts);
            break;
        case Options::Mode::Batch:
            exit_code = run_batch_mode(engine.get(), opts);
            break;
    }

    engine->cleanup();

#if NCNN_VULKAN
    // Libère explicitement les ressources Vulkan/NCNN globales
    // pour éviter les fuites lorsque le binaire est invoqué en boucle.
    ncnn::destroy_gpu_instance();
#endif

    return exit_code;
}
