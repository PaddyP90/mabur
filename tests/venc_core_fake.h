/* A host stand-in for the star6e runtime under drone/venc/venc_core.c: the
 * MI_SYS session and the pipeline are two flags, the encoder loop waits for
 * its stop request. test_venc_core.cpp links the real venc_core.c against it
 * so the facade's lifecycle guards run on the host. */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  /* State, as the SDK would see it. */
  int session_up;       /* between MI_SYS_Init and MI_SYS_Exit */
  int pipeline_up;      /* a built pipeline */
  unsigned width;       /* the built pipeline's width */
  /* Counters. */
  int sys_init;         /* MI_SYS_Init calls */
  int sys_init_on_live; /* MI_SYS_Init while a session was already up */
  int sys_exit;
  int restarts;         /* star6e_runtime_pipeline_restart calls */
  int injected;         /* of those, with inject_fail set */
  /* Script: the next fail_restarts restarts fail in the SDK. */
  int fail_restarts;
} VencCoreFake;

extern VencCoreFake g_venc_fake;
void venc_core_fake_reset_counters(void);

#ifdef __cplusplus
}
#endif
