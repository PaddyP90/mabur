#pragma once
// Writes a measured wall-equalization table into a running unit's
// /etc/mabur.toml.
//
// The failure mode this file exists to prevent: CLAUDE.md and
// docs/deploy.md both record that a daemon which sees a config it cannot
// load exits, and its wrapper respawns it forever at 2 s. The drone has no
// console -- a calibration run that leaves an unloadable config behind
// turns a 75-second convenience into a device that needs a laptop and a
// serial cable to recover. Every step below exists to make that
// unreachable:
//
//   1. validate  -- run the candidate walls through the same derivation
//      config.cpp's loader applies (parse_radio's wall checks) and
//      refuse to touch anything if a derived diff would leave the 8822E's
//      7-bit rate-diff field range [-64,63].
//   2. patch -- rewrite the three keys in memory and check that every
//      REQUIRED one was actually found and rewritten (see PatchStatus).
//      patch_toml has no TOML-writer fallback: if a key's line can't be
//      matched -- missing, reformatted, a multi-line array -- silently
//      leaving it alone would mean load_config() below still accepts the
//      file (an unmodified original value is still a valid one) and
//      apply_calibration reports success on a calibration that never
//      actually applied. That is the silent-miscalibration failure this
//      whole kit exists to prevent, arriving through the back door, so it
//      is checked explicitly and refused loudly (KeyNotFound) rather than
//      left to slip through.
//   3-6. publish_config (config_write.h): write "<path>.new" + fsync,
//      verify it with mabur::load_config -- the same function maburd boots
//      with -- back the original up to "<path>.pre-cal", then rename the
//      verified candidate into place. config_write.h's header explains why
//      verification happens before publishing and why the backup goes
//      through write-then-rename.
//
// patch_toml is line-surgical for the same reason patch_toml_key is (see
// config_write.h): /etc/mabur.toml is bundle/mabur.default.toml verbatim,
// a heavily commented file a re-serialize would flatten, so it rewrites
// only the three keys this file owns, in place, preserving every other
// line -- comments included -- byte for byte.

#include <array>
#include <string>

#include "config_write.h"
#include "mabur/rc_proto.h"
#include "mabur/toml.h"

namespace mabur {

// kWallUndetermined in walls[r] (or legacy_wall) means "undetermined: keep
// the config's value": that rate wasn't swept this session, so the
// existing config's value for it is kept. Never write a fabricated number
// over a real measurement.
struct CalWrite {
  std::array<int, 8> walls{
      mabur::rc::kWallUndetermined, mabur::rc::kWallUndetermined,
      mabur::rc::kWallUndetermined, mabur::rc::kWallUndetermined,
      mabur::rc::kWallUndetermined, mabur::rc::kWallUndetermined,
      mabur::rc::kWallUndetermined, mabur::rc::kWallUndetermined};
  int legacy_wall =
      mabur::rc::kWallUndetermined;  // derived from the MCS0 sweep result,
                                      // not swept itself
};

// Which of the three keys patch_toml actually found and rewrote. A flag
// stays false either because the key's line was never matched at all
// (missing, reformatted, a multi-line array patch_toml doesn't parse) or,
// for legacy_wall_rel only, because CalWrite::legacy_wall was
// kWallUndetermined and the line was deliberately left untouched.
// apply_calibration_impl uses this to tell those two cases apart from each
// other and from "found and rewritten", since only the first is an error.
struct PatchStatus {
  bool rate_walls_rel = false;
  bool legacy_wall_rel = false;
  bool power_mode = false;
};

enum class ApplyResult {
  Ok,
  OutOfRange,     // a derived diff would leave the hardware field's [-64,63]
  KeyNotFound,    // patch_toml couldn't find/rewrite a required key -- the
                  // file's structure wasn't what this line-surgical patcher
                  // expects; nothing touched
  BackupFailed,   // couldn't write "<path>.pre-cal" -- nothing else touched
  WriteFailed,    // couldn't write/fsync "<path>.new", or the final publish
                  // rename failed after verification already succeeded
  ReparseFailed,  // candidate failed mabur::load_config -- never published
};

// Rewrites only rate_walls_rel, legacy_wall_rel and power_mode (set to
// "offset") in `text`, preserving every other line, every comment, and
// each rewritten line's original spacing before '=' and any trailing
// comment. An entry left at kWallUndetermined keeps whatever value the
// original text had for it. If `status` is non-null, it is filled in with
// which keys were actually found and rewritten -- see PatchStatus. Never
// throws; a key it can't find is simply left unmatched in `*status` for
// the caller to act on.
std::string patch_toml(const std::string& text, const CalWrite& w,
                        PatchStatus* status = nullptr);

// Mirrors config.cpp: every determined rel in [-64,63] and
// rel - lround(margin_db*4) >= -64, for every swept rate
// (walls[r] != kWallUndetermined) and for legacy_wall (if determined). On
// failure, *why names the offending key, matching config.cpp's message so
// this refusal reads the same as a boot failure would.
//
// This stays even though apply_calibration()'s load_config() verification
// re-derives the same numbers: this check refuses a bad table before the
// disk is touched at all, with a specific OutOfRange result and a message
// naming the exact derivation that failed. load_config() is the backstop
// that catches anything this model does not -- a mistyped key, a value
// outside the three keys this file patches, a config field this function
// knows nothing about. Belt and braces, not redundancy.
bool walls_in_range(const CalWrite& w, double margin_db, std::string* why);

// Validate -> patch (+ required-key check) -> write "<path>.new" + fsync
// -> verify with mabur::load_config -> back up -> rename into place. See
// the file header for why each step exists and why verification happens
// before publishing rather than after. On anything but Ok, `path` is left
// exactly as it was found: it is never replaced by anything that has not
// already been proven loadable by the same function maburd boots with.
// `err`, if non-null, is always set on a non-Ok result.
ApplyResult apply_calibration(const std::string& path, const CalWrite& w,
                               double margin_db, std::string* err);

// Test-only seam: same as apply_calibration, but writes deliberate garbage
// to "<path>.new" instead of the patched config (bypassing patch_toml and
// its required-key check entirely), so the verify-then-refuse path can be
// exercised without needing a real writer bug to trigger it.
ApplyResult apply_calibration_for_test_with_corrupt_writer(
    const std::string& path, const CalWrite& w, double margin_db,
    std::string* err);

}  // namespace mabur
