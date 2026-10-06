// Emoji search for the composer: :shortcodes:, suggestions and the picker.
// Names in English (plus GitHub/Slack aliases) and German: :fire: and :feuer:.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ghost::emoji {

using Hit = std::pair<std::string, std::string>;  // emoji, matched name

// Best matches: exact, prefix, word prefix, substring; plain before skin tones.
// An empty query returns the recently used emoji.
std::vector<Hit> search(const std::string& query, size_t limit = 60);
// Emoji for a shortcode without colons ("fire", "thumbs_up", "+1", "feuer").
std::optional<std::string> lookup(const std::string& code);
// Replace every known :code: in the text.
std::string emojize(const std::string& text);
// ":fi" at the end of the draft (start or after a space): returns "fi" and its byte offset of ':'.
std::optional<std::pair<std::string, size_t>> trailing_partial(const std::string& text);
// A finished ":fire:" at the very end: its code and byte offset.
std::optional<std::pair<std::string, size_t>> trailing_code(const std::string& text);

std::vector<std::string> recent();
void remember(const std::string& e);

}  // namespace ghost::emoji
