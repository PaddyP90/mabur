#include "cal_apply.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace mabur {

namespace {

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

// Parses the 8 ints out of a "[91, 91, ...]" array's inner text. A
// malformed original (should never happen -- it's bundle/mabur.default.toml
// verbatim) falls back to 0 for the unparseable slot rather than throwing;
// walls_in_range() and the load_config() verification step are the layers
// that actually guard correctness, not this cosmetic fallback.
std::array<int, 8> parse_int_array(const std::string& inner) {
  std::array<int, 8> vals{};
  size_t pos = 0;
  for (int idx = 0; idx < 8; ++idx) {
    size_t comma = inner.find(',', pos);
    const std::string tok =
        trim(comma == std::string::npos ? inner.substr(pos)
                                         : inner.substr(pos, comma - pos));
    try {
      vals[static_cast<size_t>(idx)] = std::stoi(tok);
    } catch (...) {
      vals[static_cast<size_t>(idx)] = 0;
    }
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return vals;
}

// Rewrites one matched line's value, preserving the leading whitespace
// before the key, the exact original spacing between the key and '=', and
// every byte from the end of the old value onward (trailing whitespace and
// any "# comment"). `value_end` is where the new value text goes; the
// caller supplies where the old value ended in `old_value_end`.
std::string rewrite_line(const std::string& line, size_t key_begin,
                          size_t key_len, size_t eq_pos,
                          const std::string& new_value,
                          size_t old_value_end) {
  const std::string leading = line.substr(0, key_begin);
  const std::string key = line.substr(key_begin, key_len);
  const std::string sep = line.substr(key_begin + key_len, eq_pos - (key_begin + key_len));
  const std::string trailing = line.substr(old_value_end);
  return leading + key + sep + "= " + new_value + trailing;
}

}  // namespace

std::string patch_toml(const std::string& text, const CalWrite& w,
                        PatchStatus* status) {
  std::vector<std::string> lines;
  {
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) lines.push_back(line);
  }
  const bool trailing_newline = !text.empty() && text.back() == '\n';

  for (std::string& line : lines) {
    size_t key_begin, eq_pos;

    if (match_toml_key(line, "rate_walls_rel", &key_begin, &eq_pos)) {
      size_t v = eq_pos + 1;
      while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
      if (v < line.size() && line[v] == '[') {
        const size_t close = line.find(']', v);
        if (close != std::string::npos) {
          const auto orig = parse_int_array(line.substr(v + 1, close - v - 1));
          std::string arr = "[";
          for (int i = 0; i < 8; ++i) {
            const int val =
                w.walls[static_cast<size_t>(i)] == mabur::rc::kWallUndetermined
                    ? orig[static_cast<size_t>(i)]
                    : w.walls[static_cast<size_t>(i)];
            if (i) arr += ", ";
            arr += std::to_string(val);
          }
          arr += "]";
          line = rewrite_line(line, key_begin, std::string("rate_walls_rel").size(),
                               eq_pos, arr, close + 1);
          if (status) status->rate_walls_rel = true;
        }
      }
      continue;
    }

    if (match_toml_key(line, "legacy_wall_rel", &key_begin, &eq_pos)) {
      // kWallUndetermined = not swept this session (derived from MCS0, per
      // the header comment) -- leave the line untouched rather than write
      // a fabricated number.
      if (w.legacy_wall == mabur::rc::kWallUndetermined) continue;
      size_t v = eq_pos + 1;
      while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
      size_t e = v;
      if (e < line.size() && line[e] == '-') ++e;
      while (e < line.size() && std::isdigit(static_cast<unsigned char>(line[e]))) ++e;
      line = rewrite_line(line, key_begin, std::string("legacy_wall_rel").size(),
                           eq_pos, std::to_string(w.legacy_wall), e);
      if (status) status->legacy_wall_rel = true;
      continue;
    }

    if (match_toml_key(line, "power_mode", &key_begin, &eq_pos)) {
      size_t v = eq_pos + 1;
      while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
      if (v < line.size() && line[v] == '"') {
        const size_t close = line.find('"', v + 1);
        if (close != std::string::npos) {
          line = rewrite_line(line, key_begin, std::string("power_mode").size(),
                               eq_pos, "\"offset\"", close + 1);
          if (status) status->power_mode = true;
        }
      }
      continue;
    }
  }

  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    out += lines[i];
    if (i + 1 < lines.size()) out += "\n";
  }
  if (trailing_newline) out += "\n";
  return out;
}

bool walls_in_range(const CalWrite& w, double margin_db, std::string* why) {
  // Mirrors drone/src/config.cpp exactly, including its message wording, so
  // a refusal here reads the same way a boot-time refusal would. This
  // pre-check and the load_config() verification step later in
  // apply_calibration() serve different purposes and both stay: this one
  // refuses a bad table before the disk is touched at all, with a specific
  // OutOfRange result and a message naming the exact derivation that
  // failed; load_config() is the backstop that catches anything this
  // model does not -- a mistyped key, a value outside the three keys we
  // patch, a config field this function knows nothing about.
  const int m = static_cast<int>(std::lround(margin_db * 4.0));
  auto check = [&](int v, const std::string& key) {
    if (v == mabur::rc::kWallUndetermined) return true;
    if (v < rc::kRelMin || v > rc::kRelMax) {
      if (why) *why = key + ": " + std::to_string(v) +
                      " is outside the 7-bit hardware field range [-64,63]";
      return false;
    }
    if (v - m < rc::kRelMin) {
      if (why) *why = key + ": " + std::to_string(v) +
                      " - wall_margin_db*4 = " + std::to_string(v - m) +
                      " is below the hardware field floor -64";
      return false;
    }
    return true;
  };
  for (size_t i = 0; i < w.walls.size(); ++i)
    if (!check(w.walls[i], "radio.rate_walls_rel[" + std::to_string(i) + "]"))
      return false;
  return check(w.legacy_wall, "radio.legacy_wall_rel");
}

// Checks that patch_toml() actually found and rewrote every key this file
// is required to have touched. Required-ness mirrors CalWrite's own
// sentinel rule: rate_walls_rel and power_mode are always rewritten, so
// their PatchStatus flag must always be set; legacy_wall_rel is only
// required when CalWrite::legacy_wall is determined (not
// kWallUndetermined) -- when it's kWallUndetermined the line is
// deliberately left untouched and its flag is expected to stay false.
bool patch_covered_required_keys(const PatchStatus& s, const CalWrite& w,
                                  std::string* why) {
  if (!s.rate_walls_rel) {
    if (why) *why = "radio.rate_walls_rel: line not found in config -- unexpected file structure, nothing written";
    return false;
  }
  if (!s.power_mode) {
    if (why) *why = "radio.power_mode: line not found in config -- unexpected file structure, nothing written";
    return false;
  }
  if (w.legacy_wall != mabur::rc::kWallUndetermined && !s.legacy_wall_rel) {
    if (why) *why = "radio.legacy_wall_rel: line not found in config -- unexpected file structure, nothing written";
    return false;
  }
  return true;
}

namespace {

// Shared body of apply_calibration() and its corrupt-writer test seam.
// `content_override`, when set, replaces the real patch_toml() output --
// the seam tests need a way to get deliberate garbage onto disk without a
// real writer bug, to prove verification actually catches it. The
// publisher's verify-before-publish order is what makes that refusal
// leave `path` untouched; see config_write.h.
ApplyResult apply_calibration_impl(const std::string& path, const CalWrite& w,
                                    double margin_db, std::string* err,
                                    const std::string* content_override) {
  // Step 1: validate the derived diffs. Nothing on disk is touched if
  // this fails -- see the walls_in_range() comment for why this pre-check
  // stays even though load_config() below re-derives the same numbers.
  std::string why;
  if (!walls_in_range(w, margin_db, &why)) {
    if (err) *err = why;
    return ApplyResult::OutOfRange;
  }

  // Held from the read through the publish, so a parameter write on
  // another thread can't land between them and be overwritten by a
  // candidate patched from the text read here.
  const auto lock = lock_config_writes();
  std::string original;
  if (!read_text_file(path, &original)) {
    if (err) *err = "failed to read " + path;
    return ApplyResult::WriteFailed;
  }

  // Step 2: patch in memory and check every REQUIRED key was actually
  // found and rewritten. patch_toml has no error channel of its own -- a
  // key it can't match (missing, reformatted, a multi-line array) is
  // silently left as the original's value, which load_config() below
  // would still accept (an unmodified original value is still a valid
  // one). Without this check, apply_calibration could report Ok on a
  // calibration that never actually applied -- the exact
  // silent-miscalibration failure this file exists to prevent, arriving
  // through the back door. Skipped for content_override: that seam
  // bypasses patch_toml on purpose to inject raw garbage.
  std::string content;
  if (content_override) {
    content = *content_override;
  } else {
    PatchStatus status;
    content = patch_toml(original, w, &status);
    std::string key_why;
    if (!patch_covered_required_keys(status, w, &key_why)) {
      if (err) *err = key_why;
      return ApplyResult::KeyNotFound;
    }
  }

  // Steps 3-6: write "<path>.new", verify it with load_config, back up,
  // rename -- the shared publisher, see config_write.h.
  std::string pub_err;
  switch (publish_config(path, content, "pre-cal", &pub_err)) {
    case PublishResult::Ok:
      return ApplyResult::Ok;
    case PublishResult::BackupFailed:
      if (err) *err = pub_err;
      return ApplyResult::BackupFailed;
    case PublishResult::WriteFailed:
      if (err) *err = pub_err;
      return ApplyResult::WriteFailed;
    case PublishResult::ReparseFailed:
      if (err) *err = "candidate failed load_config, not published: " + pub_err;
      return ApplyResult::ReparseFailed;
  }
  if (err) *err = "unknown publish result";
  return ApplyResult::WriteFailed;
}

}  // namespace

ApplyResult apply_calibration(const std::string& path, const CalWrite& w,
                               double margin_db, std::string* err) {
  return apply_calibration_impl(path, w, margin_db, err, nullptr);
}

ApplyResult apply_calibration_for_test_with_corrupt_writer(
    const std::string& path, const CalWrite& w, double margin_db,
    std::string* err) {
  static const std::string kGarbage = "this is not valid toml [[[ ===\n";
  return apply_calibration_impl(path, w, margin_db, err, &kGarbage);
}

}  // namespace mabur
