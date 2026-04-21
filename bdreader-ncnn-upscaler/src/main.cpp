#include "engine_factory.hpp"
#include "modes/file_mode.hpp"
#include "modes/stdin_mode.hpp"
#include "options.hpp"
#include "utils/logger.hpp"

#if NCNN_VULKAN
#include "gpu.h"
#endif

int main(int argc, char** argv) {
    Options opts;
    if (!parse_options(argc, argv, opts)) {
        return 1;
    }

    logger::set_level((opts.verbose || opts.profiling || opts.log_protocol) ? logger::Level::Info : logger::Level::Warn);

    int exit_code = 0;
    {
        auto engine = make_engine(opts);
        if (!engine) {
            logger::error("Failed to initialize engine");
            return 1;
        }

        switch (opts.mode) {
            case Options::Mode::File:
                exit_code = run_file_mode(engine.get(), opts);
                break;
            case Options::Mode::Stdin:
                exit_code = run_stdin_mode(engine.get(), opts);
                break;
        }
        // Engine destructor runs here, releasing Vulkan/NCNN resources
        // BEFORE ncnn::destroy_gpu_instance() tears down the global Vulkan context.
    }

#if NCNN_VULKAN
    ncnn::destroy_gpu_instance();
#endif

    return exit_code;
}
