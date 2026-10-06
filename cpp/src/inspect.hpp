// `ghostctl inspect`: dump the live page so selectors can be rebuilt from reality.
#pragma once

#include <string>

#include "browser.hpp"

namespace ghost {

fs::path debug_dir();  // ./debug
// Writes url.txt, aria.yaml (accessibility tree), page.html, elements.json and
// screenshot.png into dir/<time>-<label>/. Returns that folder.
fs::path dump_page(Browser& b, const std::string& label, const fs::path& dir);
int run_inspect(const Config& cfg);

}  // namespace ghost
