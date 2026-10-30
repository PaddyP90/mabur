// ConfigApplier: a parameter set applied to the running process through
// fake ConfigApplyHooks, against a real copy of the bundle config and the real
// loader and publisher.
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "config.h"
#include "config_apply.h"
#include "config_write.h"
#include "mabur/config_params.h"
#include "mtest.h"
#include "scratch.h"

using namespace mabur;

namespace {

constexpr auto npos = std::string::npos;

std::string slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void spit(const std::string& path, const std::string& content) {
  std::ofstream f(path, std::ios::binary);
  f << content;
}

bool exists(const std::string& path) { return std::ifstream(path).good(); }

std::string bundle_text() {
  const std::string t = slurp(std::string(MABUR_BUNDLE_DIR) + "/mabur.default.toml");
  REQUIRE(!t.empty());
  return t;
}

// A scratch copy of bundle/mabur.default.toml; removes the publisher's
// siblings (backup, scratch candidate) with it.
struct ScratchBundle {
  ScratchFile f{"test_config_apply", ".toml"};
  ScratchBundle() { spit(f.path, bundle_text()); }
  ~ScratchBundle() {
    std::remove((f.path + ".pre-param").c_str());
    std::remove((f.path + ".pre-param.tmp").c_str());
    std::remove((f.path + ".new").c_str());
  }
  const std::string& path() const { return f.path; }
};

// Every hook records its call (into `order` and a per-hook vector) and
// returns a settable result. pipeline_restart can be made to block on a
// latch until release().
struct Fake {
  // Results.
  std::vector<bool> restart_ok;  // consumed one per pipeline_restart; true once exhausted
  bool cold_ok = true;
  bool power_ok = true;
  bool cal_active = false;
  bool restart_block = false;
  std::function<void()> on_set_knobs;  // runs inside set_knobs, on the apply thread

  // Calls.
  std::vector<std::string> order;
  std::vector<Config> set_knobs, power_plan, record_configure, pipeline_restart, after_rebuild;
  std::vector<bool> ldpc;
  std::vector<std::pair<bool, Config>> msp;
  int recorder_stop = 0, cold = 0, exit = 0;

  std::mutex mu;
  std::condition_variable cv;
  bool released = false;

  void release() {
    std::lock_guard<std::mutex> l(mu);
    released = true;
    cv.notify_all();
  }

  ConfigApplyHooks hooks() {
    ConfigApplyHooks h;
    h.set_knobs = [this](const Config& c) {
      order.push_back("set_knobs");
      set_knobs.push_back(c);
      if (on_set_knobs) on_set_knobs();
    };
    h.power_plan = [this](const Config& c, std::string* err) {
      order.push_back("power_plan");
      power_plan.push_back(c);
      if (!power_ok) *err = "walls out of range";
      return power_ok;
    };
    h.ldpc = [this](bool on) {
      order.push_back("ldpc");
      ldpc.push_back(on);
    };
    h.msp = [this](bool enable, const Config& c) {
      order.push_back("msp");
      msp.emplace_back(enable, c);
    };
    h.recorder_stop = [this] {
      order.push_back("recorder_stop");
      ++recorder_stop;
    };
    h.record_configure = [this](const Config& c) {
      order.push_back("record_configure");
      record_configure.push_back(c);
    };
    h.pipeline_restart = [this](const Config& c) {
      order.push_back("pipeline_restart");
      pipeline_restart.push_back(c);
      if (restart_block) {
        std::unique_lock<std::mutex> l(mu);
        cv.wait(l, [this] { return released; });
      }
      const size_t i = pipeline_restart.size() - 1;
      return i < restart_ok.size() ? static_cast<bool>(restart_ok[i]) : true;
    };
    h.pipeline_cold_restart = [this](const Config&) {
      order.push_back("pipeline_cold_restart");
      ++cold;
      return cold_ok;
    };
    h.after_rebuild = [this](const Config& c) {
      order.push_back("after_rebuild");
      after_rebuild.push_back(c);
    };
    h.request_exit = [this] {
      order.push_back("request_exit");
      ++exit;
    };
    h.calibration_active = [this] { return cal_active; };
    return h;
  }
};

// One applier over a scratch bundle, its log collected instead of printed.
struct Rig {
  ScratchBundle b;
  std::string original = slurp(b.path());
  Fake f;
  std::vector<std::string> lines;
  ConfigApplier a{b.path(), load_config(b.path()), f.hooks(),
                  [this](const std::string& l) { lines.push_back(l); }};
  const std::string& path() const { return b.path(); }
};

// Polls take_result for up to 2 s.
bool wait_result(ConfigApplier& a, ConfigApplyResult* r) {
  for (int i = 0; i < 2000; ++i) {
    if (a.take_result(r)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

ConfigApplyResult apply(ConfigApplier& a, size_t row, const std::string& value) {
  std::string why;
  REQUIRE(a.submit(row, value, &why));
  ConfigApplyResult r{};
  REQUIRE(wait_result(a, &r));
  return r;
}

constexpr size_t kWallMargin = 0, kVencSize = 3, kVencSlices = 4, kBitrateMax = 5, kMsp = 12;

}  // namespace

TEST(every_row_has_the_path_from_the_table) {
  using P = ConfigApplyPath;
  const ConfigApplyPath want[13] = {P::Reprogram, P::Reprogram, P::Rebuild, P::Rebuild, P::Rebuild,
                              P::Knob,      P::Knob,      P::Rebuild, P::Rebuild, P::Rebuild,
                              P::Rebuild,   P::Knob,      P::Reprogram};
  REQUIRE(cfgparam::kCount == 13);
  for (size_t i = 0; i < cfgparam::kCount; ++i) CHECK(apply_path_for(i) == want[i]);
}

TEST(knob_set_calls_set_knobs_then_publishes) {
  Rig g;
  const ConfigApplyResult r = apply(g.a, kBitrateMax, "9000");
  CHECK(r.ok && r.path == ConfigApplyPath::Knob);
  CHECK(g.f.set_knobs.size() == 1 && g.f.set_knobs[0].encoder.bitrate_max_kbps == 9000);
  CHECK(g.f.order == std::vector<std::string>{"set_knobs"});
  CHECK(slurp(g.path()).find("bitrate_max_kbps   = 9000") != npos);
  CHECK(exists(g.path() + ".pre-param") && slurp(g.path() + ".pre-param") == g.original);
  CHECK(!exists(g.path() + ".new"));
  CHECK(g.a.live().encoder.bitrate_max_kbps == 9000);
  CHECK(g.a.stats().ok == 1 && g.a.stats().failed == 0);
  CHECK(!g.a.busy());
  REQUIRE(!g.lines.empty());
  CHECK(g.lines.back().find("apply ENC_BITRATE_MAX knob ok ") == 0);
}

TEST(rebuild_set_runs_stop_configure_restart_after_in_order) {
  Rig g;
  const ConfigApplyResult r = apply(g.a, kVencSlices, "2");
  CHECK(r.ok && r.path == ConfigApplyPath::Rebuild);
  CHECK((g.f.order == std::vector<std::string>{"recorder_stop", "record_configure",
                                               "pipeline_restart", "after_rebuild"}));
  REQUIRE(g.f.pipeline_restart.size() == 1);
  CHECK(g.f.pipeline_restart[0].venc.core.slices == 2);
  CHECK(g.f.record_configure[0].venc.core.slices == 2);
  CHECK(g.f.after_rebuild[0].venc.core.slices == 2);
  CHECK(g.a.live().venc.core.slices == 2);
  CHECK(exists(g.path() + ".pre-param"));
  CHECK(g.a.stats().rebuilds == 1);
}

TEST(rebuild_failure_restores_previous_and_publishes_nothing) {
  Rig g;
  g.f.restart_ok = {false, true};  // new fails, old succeeds
  const ConfigApplyResult r = apply(g.a, kVencSlices, "2");
  CHECK(!r.ok && r.reason.find("restored previous") != npos);
  CHECK(r.reason == "pipeline refused venc.slices: restored previous");
  REQUIRE(g.f.pipeline_restart.size() == 2);
  CHECK(g.f.pipeline_restart[0].venc.core.slices == 2);
  CHECK(g.f.pipeline_restart[1].venc.core.slices == 4);
  CHECK((g.f.order == std::vector<std::string>{"recorder_stop", "record_configure",
                                               "pipeline_restart", "record_configure",
                                               "pipeline_restart", "after_rebuild"}));
  CHECK(g.f.record_configure[1].venc.core.slices == 4);
  CHECK(g.f.after_rebuild.size() == 1 && g.f.after_rebuild[0].venc.core.slices == 4);
  CHECK(slurp(g.path()) == g.original && !exists(g.path() + ".pre-param"));
  CHECK(!exists(g.path() + ".new"));
  CHECK(g.a.live().venc.core.slices == 4);
  CHECK(g.a.stats().failed == 1 && g.a.stats().rebuilds == 2);
}

TEST(ladder_runs_every_rung) {
  Rig g;
  g.f.restart_ok = {false, false};
  g.f.cold_ok = false;
  const ConfigApplyResult r = apply(g.a, kVencSlices, "2");
  CHECK(!r.ok && g.f.cold == 1 && g.f.exit == 1);
  CHECK(r.reason == "pipeline lost: exiting");
  CHECK(g.f.after_rebuild.empty());
  CHECK(slurp(g.path()) == g.original && !exists(g.path() + ".pre-param"));

  g.f.restart_ok = {false, false, false, false};
  g.f.cold_ok = true;
  const ConfigApplyResult r2 = apply(g.a, kVencSlices, "2");
  CHECK(!r2.ok && g.f.cold == 2 && g.f.exit == 1 && r2.reason.find("(cold)") != npos);
  CHECK(r2.reason == "pipeline refused venc.slices: restored previous (cold)");
  CHECK(g.f.after_rebuild.size() == 1 && g.f.after_rebuild[0].venc.core.slices == 4);
  CHECK(slurp(g.path()) == g.original && !exists(g.path() + ".pre-param"));
  CHECK(g.a.stats().failed == 2 && g.a.stats().rebuilds == 4);
}

TEST(second_submit_during_apply_is_refused) {
  Rig g;
  g.f.restart_block = true;  // the fake blocks on a latch
  std::string why;
  REQUIRE(g.a.submit(kVencSize, "\"1920x1080\"", &why));
  CHECK(g.a.busy());
  CHECK(!g.a.submit(kBitrateMax, "9000", &why) && why == "apply in progress");
  g.f.release();
  ConfigApplyResult r{};
  REQUIRE(wait_result(g.a, &r));
  CHECK(r.ok && r.path == ConfigApplyPath::Rebuild);
  CHECK(g.f.set_knobs.empty());
  CHECK(!g.a.busy());
}

TEST(untaken_result_keeps_the_applier_busy) {
  Rig g;
  std::string why;
  REQUIRE(g.a.submit(kBitrateMax, "9000", &why));
  for (int i = 0; i < 2000 && g.a.stats().ok == 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  REQUIRE(g.a.stats().ok == 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));  // the result is posted
  CHECK(g.a.busy());
  CHECK(!g.a.submit(kBitrateMax, "8000", &why) && why == "apply in progress");
  ConfigApplyResult r{};
  REQUIRE(g.a.take_result(&r));
  CHECK(r.ok && !g.a.busy());
  CHECK(!g.a.take_result(&r));
  CHECK(g.f.set_knobs.size() == 1);
}

TEST(submit_during_calibration_is_refused) {
  Rig g;
  g.f.cal_active = true;
  std::string why;
  CHECK(!g.a.submit(kWallMargin, "2.0", &why) && why == "calibration in progress");
  CHECK(!g.a.busy());
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  ConfigApplyResult r{};
  CHECK(!g.a.take_result(&r));
  CHECK(g.f.power_plan.empty());
  CHECK(slurp(g.path()) == g.original);
}

TEST(publish_failure_after_apply_reverts) {
  if (::geteuid() == 0) {
    std::printf("skip publish_failure_after_apply_reverts: root ignores a 0555 directory\n");
    return;
  }
  // Its own directory, so making it read-only touches nothing shared.
  ::mkdir(MABUR_TEST_SCRATCH_DIR, 0777);
  const std::string dir = std::string(MABUR_TEST_SCRATCH_DIR) + "/test_config_apply-ro-" +
                          std::to_string(::getpid());
  REQUIRE(::mkdir(dir.c_str(), 0755) == 0);
  const std::string path = dir + "/mabur.toml";
  const std::string original = bundle_text();
  spit(path, original);
  const int original_max = load_config(path).encoder.bitrate_max_kbps;

  Fake f;
  // The applier has read the file and loaded its candidate by the time it
  // applies; read-only from here, so only the publish fails.
  f.on_set_knobs = [&] {
    if (f.set_knobs.size() == 1) ::chmod(dir.c_str(), 0555);
  };
  std::vector<std::string> lines;
  {
    ConfigApplier a{path, load_config(path), f.hooks(),
                    [&](const std::string& l) { lines.push_back(l); }};
    const ConfigApplyResult r = apply(a, kBitrateMax, "9000");
    CHECK(!r.ok && r.reason.find("publish failed") != npos);
    CHECK(f.set_knobs.size() == 2 && f.set_knobs[1].encoder.bitrate_max_kbps == original_max);
    CHECK(f.set_knobs[0].encoder.bitrate_max_kbps == 9000);
    CHECK(a.live().encoder.bitrate_max_kbps == original_max);
    CHECK(slurp(path) == original && !exists(path + ".pre-param"));
  }
  ::chmod(dir.c_str(), 0755);
  std::remove((path + ".new").c_str());
  std::remove((path + ".pre-param").c_str());
  std::remove((path + ".pre-param.tmp").c_str());
  std::remove(path.c_str());
  ::rmdir(dir.c_str());
}

TEST(loader_refusal_applies_nothing) {
  Rig g;
  const ConfigApplyResult r = apply(g.a, kVencSize, "\"1x\"");
  CHECK(!r.ok && g.f.pipeline_restart.empty() && r.reason.find("venc.size") != npos);
  CHECK(r.reason.find("config: ") == npos && r.reason.find(g.path()) == npos);
  CHECK(g.f.order.empty());
  CHECK(slurp(g.path()) == g.original && !exists(g.path() + ".pre-param"));
  CHECK(!exists(g.path() + ".new"));
}

TEST(wall_margin_reprogram_failure_is_reported_and_unpublished) {
  Rig g;
  g.f.power_ok = false;
  const ConfigApplyResult r = apply(g.a, kWallMargin, "2.0");
  CHECK(!r.ok && slurp(g.path()) == g.original);
  CHECK(r.path == ConfigApplyPath::Reprogram && r.reason.find("walls out of range") != npos);
  CHECK(g.f.power_plan.size() == 1 && g.f.power_plan[0].radio.wall_margin_db == 2.0);
  CHECK(!exists(g.path() + ".pre-param"));
  CHECK(g.a.live().radio.wall_margin_db == 1.0);
}

TEST(wall_margin_reprogram_publishes) {
  Rig g;
  const ConfigApplyResult r = apply(g.a, kWallMargin, "2.0");
  CHECK(r.ok && r.path == ConfigApplyPath::Reprogram);
  CHECK(g.f.order == std::vector<std::string>{"power_plan"});
  CHECK(g.a.live().radio.wall_margin_db == 2.0);
  CHECK(load_config(g.path()).radio.wall_margin_db == 2.0);
}

TEST(ldpc_calls_ldpc_hook_with_candidate) {
  Rig g;
  const ConfigApplyResult r = apply(g.a, 1, "false");
  CHECK(r.ok && r.path == ConfigApplyPath::Reprogram);
  CHECK(g.f.ldpc == std::vector<bool>{false});
  CHECK(g.f.order == std::vector<std::string>{"ldpc"});
}

TEST(msp_enable_calls_msp_hook_with_candidate) {
  Rig g;
  REQUIRE(!load_config(g.path()).low_power.enable);
  const ConfigApplyResult r = apply(g.a, kMsp, "false");
  CHECK(r.ok && r.path == ConfigApplyPath::Reprogram);
  CHECK(g.f.msp.size() == 1 && g.f.msp[0].first == false);
  CHECK(!g.f.msp[0].second.msp.enable);
}

TEST(apply_waits_for_the_config_write_lock) {
  Rig g;
  {
    // Another writer (calibration) holding the lock: the applier must not
    // read, apply or publish until it lets go.
    auto lock = lock_config_writes();
    std::string why;
    REQUIRE(g.a.submit(kBitrateMax, "9000", &why));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ConfigApplyResult r{};
    CHECK(!g.a.take_result(&r));
    CHECK(g.f.set_knobs.empty());
    CHECK(slurp(g.path()) == g.original);
  }
  ConfigApplyResult r{};
  REQUIRE(wait_result(g.a, &r));
  CHECK(r.ok && g.f.set_knobs.size() == 1);
}

TEST(external_rewrite_refreshes_live_before_next_apply) {
  Rig g;
  // Calibration publishes a new wall margin and applies it itself.
  std::string text = g.original;
  REQUIRE(patch_toml_key(&text, "radio", "wall_margin_db", "3.0"));
  std::string err;
  REQUIRE(publish_config(g.path(), text, "pre-cal", &err) == PublishResult::Ok);
  std::remove((g.path() + ".pre-cal").c_str());
  g.a.note_file_rewritten_externally();
  // A failed set that reverts must revert to the calibrated value.
  g.f.power_ok = true;
  g.f.on_set_knobs = nullptr;
  g.f.restart_ok = {false, true};
  const ConfigApplyResult r = apply(g.a, kVencSlices, "2");
  CHECK(!r.ok);
  REQUIRE(g.f.pipeline_restart.size() == 2);
  CHECK(g.f.pipeline_restart[1].radio.wall_margin_db == 3.0);
  CHECK(g.a.live().radio.wall_margin_db == 3.0);
}

TEST(unknown_row_is_refused) {
  Rig g;
  std::string why;
  CHECK(!g.a.submit(cfgparam::kCount, "1", &why) && !why.empty());
  CHECK(!g.a.busy());
}

// Teardown: shutdown() waits for the apply in flight, joins the thread and
// refuses every later submit, so no apply can start against a daemon that
// is tearing down. A second call (and the destructor's) is a no-op.
TEST(shutdown_finishes_the_apply_in_flight_then_refuses_submits) {
  Rig g;
  g.f.restart_block = true;
  std::string why;
  REQUIRE(g.a.submit(kVencSlices, "2", &why));
  std::thread releaser([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    g.f.release();
  });
  g.a.shutdown();
  releaser.join();
  CHECK(g.f.pipeline_restart.size() == 1);
  ConfigApplyResult r{};
  CHECK(g.a.take_result(&r) && r.ok);
  CHECK(!g.a.submit(kBitrateMax, "9000", &why));
  CHECK(why == "shutting down");
  CHECK(g.f.set_knobs.empty());
  g.a.shutdown();
}

MTEST_MAIN
