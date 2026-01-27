#pragma once

#include "engines/base_engine.hpp"
#include "engines/realcugan_engine.hpp"
#include "engines/realesrgan_engine.hpp"
#include "options.hpp"

#include <memory>

std::unique_ptr<BaseEngine> make_engine(const Options& opts);
