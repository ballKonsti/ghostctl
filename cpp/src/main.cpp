// ghostctl: terminal client for Snapchat Web (C++ version).
#include <cstdio>
#include <iostream>
#include <string>

#include <mutex>

#include "bridge.hpp"
#include "browser.hpp"
#include "config.hpp"
#include "inspect.hpp"
#include "util.hpp"

using namespace ghost;

static int cmd_check() {
  auto cfg = Config::load();
  Browser b(cfg);
  auto st = b.start();
  std::printf("mode:    %s\nsession: %s\n", to_string(st.mode), to_string(st.session));
  if (!st.note.empty()) std::printf("note:    %s\n", st.note.c_str());
  b.close();
  return st.session == Session::LoggedIn ? 0 : 1;
}

// Developer check: list chats and read one already-read chat (nothing gets marked read).
static int cmd_debug_chats() {
  auto cfg = Config::load();
  Browser b(cfg);
  auto st = b.start();
  if (st.session != Session::LoggedIn) return std::printf("not logged in\n"), 1;
  std::mutex mu;
  std::vector<Chat> chats;
  Conversation conv;
  int pushes = 0;
  Bridge br(b, [&](BridgeEvent ev) {
    std::lock_guard lk(mu);
    ++pushes;
    if (ev.kind == BridgeEvent::Feed) chats = ev.feed;
    if (ev.kind == BridgeEvent::Conv) conv = ev.conv;
  });
  br.start();
  std::unique_lock lk(mu);
  std::printf("live: %d, chats: %zu\n", br.live(), chats.size());
  for (size_t i = 0; i < chats.size() && i < 6; ++i)
    std::printf("  %-24s %-12s %-6s streak=%s group=%d\n", chats[i].name.c_str(), chats[i].status.c_str(),
                ago(chats[i].time).c_str(), chats[i].streak().c_str(), chats[i].group);
  const Chat* pick = nullptr;
  for (auto& c : chats)
    if (!c.unread() && c.status == "Opened") { pick = &c; break; }
  if (pick) {
    std::string id = pick->id, name = pick->name;
    lk.unlock();
    br.open_chat(id);
    sleep_ms(1500);
    lk.lock();
    int msgs = 0, quotes = 0, media = 0;
    for (auto& it : conv.items)
      if (it.kind == ConvItem::Msg) msgs++, quotes += !it.msg.quote_text.empty(), media += !it.msg.media.empty();
    std::printf("opened %s: %zu items, %d messages, %d replies, %d media, seen_by=%zu, pushes=%d\n", name.c_str(),
                conv.items.size(), msgs, quotes, media, conv.seen_by.size(), pushes);
    lk.unlock();
    br.close_chat();
    lk.lock();
  }
  return 0;
}

int main(int argc, char** argv) {
  std::string cmd = argc > 1 ? argv[1] : "";
  try {
    if (cmd == "check") return cmd_check();
    if (cmd == "debug-chats") return cmd_debug_chats();
    if (cmd == "inspect") return run_inspect(Config::load());
    std::fprintf(stderr, "usage: ghostctl check\n");
    return 2;
  } catch (const ProfileLocked& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 2;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
