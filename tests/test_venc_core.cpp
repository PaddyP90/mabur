// venc_core's lifecycle guards -- the real drone/venc/venc_core.c over a
// fake star6e runtime (venc_core_fake.c) -- as the rebuild failure ladder
// uses them: a failed venc_core_restart leaves the MI_SYS session open and
// a second restart may still succeed on it; venc_core_stop closes the
// session whatever state a failed restart left, so the cold venc_core_start
// never runs MI_SYS_Init on a live session; MABUR_VENC_FAIL_RESTARTS arms
// once, at the process's first start.
#include <cstdlib>

#include "mtest.h"
#include "venc_core.h"
#include "venc_core_fake.h"

namespace {

VencCfg cfg(unsigned width) {
  VencCfg c{};
  c.width = width;
  c.height = 720;
  c.fps = 60;
  return c;
}

const VencCfg kOld = cfg(1280), kNew = cfg(1920);

bool running() { return venc_set_fps(60) == 0; }

}  // namespace

// First in the file: the variable is read by the process's first
// venc_core_start only.
TEST(fail_restarts_env_fails_the_next_n_restarts_then_is_spent) {
  setenv("MABUR_VENC_FAIL_RESTARTS", "2", 1);
  venc_core_fake_reset_counters();
  REQUIRE(venc_core_start(&kOld, nullptr) == 0);
  CHECK(venc_core_restart(&kNew) == -1);   // rung 1
  CHECK(g_venc_fake.injected == 1 && !g_venc_fake.pipeline_up && !running());
  CHECK(venc_core_restart(&kOld) == -1);   // rung 2
  CHECK(g_venc_fake.injected == 2 && g_venc_fake.session_up);
  venc_core_stop();                        // rung 3
  CHECK(!g_venc_fake.session_up && g_venc_fake.sys_exit == 1);
  REQUIRE(venc_core_start(&kOld, nullptr) == 0);
  CHECK(g_venc_fake.sys_init_on_live == 0 && g_venc_fake.width == 1280);
  CHECK(venc_core_restart(&kNew) == 0);    // spent
  CHECK(g_venc_fake.injected == 2 && g_venc_fake.width == 1920 && running());
  // A later start does not read the environment again.
  setenv("MABUR_VENC_FAIL_RESTARTS", "5", 1);
  venc_core_stop();
  REQUIRE(venc_core_start(&kOld, nullptr) == 0);
  CHECK(venc_core_restart(&kNew) == 0 && g_venc_fake.injected == 2);
  venc_core_stop();
  unsetenv("MABUR_VENC_FAIL_RESTARTS");
}

TEST(restart_after_a_failed_restart_runs_on_the_open_session) {
  venc_core_fake_reset_counters();
  REQUIRE(venc_core_start(&kOld, nullptr) == 0);
  g_venc_fake.fail_restarts = 1;
  CHECK(venc_core_restart(&kNew) == -1);
  CHECK(!running() && g_venc_fake.session_up && !g_venc_fake.pipeline_up);
  CHECK(venc_core_restart(&kOld) == 0);
  CHECK(running() && g_venc_fake.pipeline_up && g_venc_fake.width == 1280);
  CHECK(g_venc_fake.sys_init == 1 && g_venc_fake.sys_exit == 0);
  CHECK(g_venc_fake.restarts == 2);
  venc_core_stop();
  CHECK(!g_venc_fake.session_up);
}

TEST(stop_after_failed_restarts_closes_the_session_for_the_cold_start) {
  venc_core_fake_reset_counters();
  REQUIRE(venc_core_start(&kOld, nullptr) == 0);
  g_venc_fake.fail_restarts = 2;
  CHECK(venc_core_restart(&kNew) == -1);
  CHECK(venc_core_restart(&kOld) == -1);
  venc_core_stop();
  CHECK(g_venc_fake.sys_exit == 1 && !g_venc_fake.session_up);
  CHECK(venc_core_start(&kOld, nullptr) == 0);
  CHECK(g_venc_fake.sys_init == 2 && g_venc_fake.sys_init_on_live == 0);
  CHECK(running() && g_venc_fake.width == 1280);
  venc_core_stop();
}

TEST(start_while_a_failed_restart_left_the_session_open_is_refused) {
  venc_core_fake_reset_counters();
  REQUIRE(venc_core_start(&kOld, nullptr) == 0);
  g_venc_fake.fail_restarts = 1;
  CHECK(venc_core_restart(&kNew) == -1);
  CHECK(venc_core_start(&kOld, nullptr) == -1);
  CHECK(g_venc_fake.sys_init == 1 && g_venc_fake.sys_init_on_live == 0);
  venc_core_stop();
  CHECK(!g_venc_fake.session_up);
}

TEST(restart_and_stop_without_a_session_do_nothing) {
  venc_core_fake_reset_counters();
  CHECK(venc_core_restart(&kOld) == -1);
  CHECK(g_venc_fake.restarts == 0);
  venc_core_stop();
  CHECK(g_venc_fake.sys_exit == 0);
}

MTEST_MAIN
