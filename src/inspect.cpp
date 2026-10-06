#include "inspect.hpp"

#include <ctime>
#include <fstream>
#include <iostream>
#include <map>

#include "collect_js.hpp"
#include "util.hpp"

namespace ghost {

fs::path debug_dir() { return fs::current_path() / "debug"; }

// The accessibility tree as an indented outline, like Playwright's aria snapshot.
static std::string aria_outline(Browser& b) {
  auto tree = b.cdp().call("Accessibility.getFullAXTree", json::object(), b.page_session(), 60s);
  std::map<std::string, const json*> nodes;
  for (auto& n : tree["nodes"]) nodes[n["nodeId"].get<std::string>()] = &n;
  std::string out;
  std::function<void(const json&, int)> walk = [&](const json& n, int depth) {
    std::string role = n.contains("role") ? n["role"].value("value", "") : "";
    std::string name = n.contains("name") ? n["name"].value("value", "") : "";
    bool ignored = n.value("ignored", false) || role == "generic" || role == "none" || role == "InlineTextBox";
    int next = depth;
    if (!ignored && !(role == "StaticText" && name.empty())) {
      out += std::string(depth * 2, ' ') + "- " + (role == "StaticText" ? "text" : role);
      if (!name.empty()) out += " " + json(squash(name)).dump();
      out += "\n";
      next = depth + 1;
    }
    if (role == "StaticText") return;
    for (auto& c : n.value("childIds", json::array())) {
      auto it = nodes.find(c.get<std::string>());
      if (it != nodes.end()) walk(*it->second, next);
    }
  };
  if (!tree["nodes"].empty()) walk(tree["nodes"][0], 0);
  return out;
}

fs::path dump_page(Browser& b, const std::string& label, const fs::path& dir) {
  char stamp[32];
  std::time_t t = std::time(nullptr);
  std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&t));
  std::string slug;
  for (char c : lower(label)) slug += std::isalnum((unsigned char)c) ? c : '-';
  if (slug.empty()) slug = "view";
  fs::path out = dir / (std::string(stamp) + "-" + slug);
  fs::create_directories(out);
  std::ofstream(out / "url.txt") << b.url() << "\n";
  try {
    std::ofstream(out / "aria.yaml") << aria_outline(b);
  } catch (const std::exception& e) {
    std::ofstream(out / "aria.yaml") << "# failed: " << e.what() << "\n";
  }
  std::ofstream(out / "page.html") << b.eval("document.documentElement.outerHTML").get<std::string>();
  std::ofstream(out / "elements.json") << b.eval("(" + std::string(js::collect) + ")()").dump(1);
  auto size = b.eval("({w: innerWidth, h: innerHeight})");
  std::ofstream(out / "screenshot.png", std::ios::binary) << b.screenshot_png({0, 0, size["w"], size["h"]});
  return out;
}

int run_inspect(const Config& cfg) {
  Browser b(cfg);
  b.open_headed();
  std::cout << "Inspect mode. In the browser, go to the view you want captured\n"
               "(chat list, an open chat, a snap, stories, ...), then come back here.\n"
               "  <label> + Enter  dump the current page under that label\n"
               "  q + Enter        quit\n"
               "Dumps go to " << debug_dir().string() << "/ (contains your messages; it is gitignored).\n";
  std::string label;
  while (std::cout << "label> " << std::flush, std::getline(std::cin, label)) {
    label = trim(label);
    if (label == "q" || label == "quit" || label == "exit") break;
    if (b.closed()) {
      std::cout << "Browser window was closed.\n";
      break;
    }
    auto path = dump_page(b, label, debug_dir());
    std::cout << "  dumped " << b.url() << " -> " << path.string() << "\n";
  }
  b.close();
  return 0;
}

}  // namespace ghost
