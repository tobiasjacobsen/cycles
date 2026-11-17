/* SPDX-FileCopyrightText: 2025 RenderStudio
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <cstdint>
#include <string>

CCL_NAMESPACE_BEGIN

/* Worker-lifetime meter of actual Cycles compute, plus the authorization and observation facts
 * the paid remote preview is billed from.
 *
 * The meter is process-wide because the work it measures is: `PathTrace` reports the sampling,
 * adaptive filtering and denoising it really runs, and those stages are a property of the render
 * core rather than of one adapter. A host binds the usage it pays for, grants a cumulative ceiling
 * and reads the counter; adapters and accumulations come and go beneath the same meter, so a
 * scene reset never resets what was already earned.
 *
 * Stages are the boundaries of the measured work, not an estimate of it:
 * - sampling/adaptive/denoise are billable and their wall-time union is what the counter reports,
 *   whether the work has finished or is still running when it is read;
 * - preparation/device-wait/display/cleanup are traced but never add billable time.
 *
 * The meter starts closed: without an accepted grant nothing is billable, and without a bound
 * usage it records nothing at all, so the local preview and every host that does not buy time
 * render exactly as before. Timestamps are monotonic nanoseconds from one process clock. */
enum class CreditStage {
  Sampling,
  Adaptive,
  Denoise,
  Preparation,
  DeviceWait,
  Display,
  Cleanup,
};

const char *credit_stage_name(CreditStage stage);

class CreditMeter {
 public:
  static CreditMeter &get();

  /* Binds the usage a paid session is billed as, resetting the meter for it. Returns a token that
   * later grants, revocations and observations must present, or 0 when this process is already
   * bound to a different usage. Binding the same usage again returns the same token. */
  uint64_t bind_usage(const std::string &usage_id);
  /* Releases the binding held by `token`; later stages and controls record nothing. */
  void unbind_usage(uint64_t token);

  /* Accepts a durable checkpoint and the cumulative ceiling it funds. `committed_ns` is the
   * durable watermark, `balance_micro`/`remainder`/`rate_micro` the account state the ceiling was
   * derived from; the ceiling must stay within one second of funded work past the watermark.
   * Rejected grants leave the previous authorization and counter untouched. */
  bool grant(uint64_t token,
             int64_t committed_ns,
             int64_t ceiling_ns,
             int64_t balance_micro,
             int64_t remainder,
             int64_t rate_micro);
  /* Ends authorization immediately and permanently. The interval up to this call is billable;
   * every later nanosecond is not, and a revoked usage never accepts another grant. */
  void revoke();
  /* Ends authorization like `revoke`, but only for as long as the work is held: a later `grant`
   * may resume the same usage. The interval up to this call is billable and the held interval is
   * not, so a pause never bills the time it kept the work parked. */
  void suspend();
  /* Billable nanoseconds so far, including the stage currently running. Returns -1 when `token`
   * is not the bound usage. Every call is observed in the trace. */
  int64_t active_ns(uint64_t token);
  /* Whether the usage behind `token` may start or resume work right now: true once a grant was
   * accepted and until the usage is revoked. A token of 0 is the free (unbound) path and is always
   * authorized. */
  bool work_authorized(uint64_t token);
  /* Whether the usage behind `token` was funded and may still accept the grant that resumes it:
   * true once a grant was accepted and until the usage is terminally revoked. A usage held by a
   * pause is resumable; an ended one is not. A token of 0 is the free (unbound) path. */
  bool resumable(uint64_t token);

  /* Control observations of the same meter, reported from their real boundaries. */
  void note_reset();
  void note_idle();

  /* A host function run at a real stage point, on the thread doing that work, with the bound usage.
   * It may block or throw: a throw leaves through the render and session worker boundaries like
   * any other rendering failure. Cycles knows nothing of what the host does there. */
  using StageHook = void (*)(CreditStage stage, const std::string &usage_id);
  void set_stage_hook(StageHook hook);
  /* Runs the host's hook for `stage`; does nothing when no hook is set or no usage is bound, so
   * the free path never reaches it. Called after the stage's `CreditScope` is open, so a throw
   * still closes the scope. */
  void run_stage_hook(CreditStage stage);

  /* Scope handles. Both return/accept -1 when no usage is bound, which is the free path. */
  int64_t scope_enter(CreditStage stage);
  void scope_exit(int64_t scope_id);

 private:
  CreditMeter();
  CreditMeter(const CreditMeter &) = delete;
  CreditMeter &operator=(const CreditMeter &) = delete;

  struct State;
  State *state_;
};

/* RAII entry/exit of one real stage. Cheap when no usage is bound. */
class CreditScope {
 public:
  explicit CreditScope(CreditStage stage) : id_(CreditMeter::get().scope_enter(stage)) {}
  ~CreditScope() { CreditMeter::get().scope_exit(id_); }

  CreditScope(const CreditScope &) = delete;
  CreditScope &operator=(const CreditScope &) = delete;

 private:
  int64_t id_;
};

CCL_NAMESPACE_END
