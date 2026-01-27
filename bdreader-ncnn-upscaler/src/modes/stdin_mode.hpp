#pragma once

#include "../options.hpp"
#include "../engines/base_engine.hpp"

int run_stdin_mode(BaseEngine* engine, const Options& opts);
int run_batch_stdin(BaseEngine* engine, const Options& opts);
