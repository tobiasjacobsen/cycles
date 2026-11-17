/* SPDX-FileCopyrightText: 2025 RenderStudio
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "util/credit_meter.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

CCL_NAMESPACE_BEGIN

namespace {

/* Minutes and the ceiling's one-second bound, in nanoseconds; the same units the server's
 * accounting uses, so a grant and the work it funds are compared without conversion. */
constexpr int64_t CREDIT_MINUTE_NS = 60'000'000'000LL;
constexpr int64_t CREDIT_MAX_GRANT_NS = 1'000'000'000LL;

int64_t credit_now_ns()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool credit_stage_is_billable(CreditStage stage)
{
  return stage == CreditStage::Sampling || stage == CreditStage::Adaptive ||
         stage == CreditStage::Denoise;
}

/* The development trace is a Debug-only diagnostic: it is opened only inside the owned check root
 * the guarded probe contract names, and only when the check asked for it. A release host, a host
 * without the root, or a path outside it records nothing. */
bool credit_path_within(const std::filesystem::path &path, const std::filesystem::path &root)
{
  auto path_it = path.begin();
  for (auto root_it = root.begin(); root_it != root.end(); ++root_it, ++path_it) {
    if (path_it == path.end() || *path_it != *root_it) {
      return false;
    }
  }
  return true;
}

std::FILE *credit_open_trace()
{
#ifdef NDEBUG
  return nullptr;
#else
  const char *trace = std::getenv("RENDERSTUDIO_CHECK_CREDITS_TRACE");
  const char *root = std::getenv("RENDERSTUDIO_CHECK_ROOT");
  if (trace == nullptr || *trace == '\0' || root == nullptr || *root == '\0') {
    return nullptr;
  }
  std::error_code error;
  const std::filesystem::path trace_path = std::filesystem::weakly_canonical(trace, error);
  const std::filesystem::path root_path = std::filesystem::weakly_canonical(root, error);
  if (error || !trace_path.is_absolute() || !root_path.is_absolute() ||
      !credit_path_within(trace_path, root_path)) {
    return nullptr;
  }
  return std::fopen(trace_path.string().c_str(), "a");
#endif
}

}  // namespace

const char *credit_stage_name(CreditStage stage)
{
  switch (stage) {
    case CreditStage::Sampling:
      return "sampling";
    case CreditStage::Adaptive:
      return "adaptive";
    case CreditStage::Denoise:
      return "denoise";
    case CreditStage::Preparation:
      return "preparation";
    case CreditStage::DeviceWait:
      return "device-wait";
    case CreditStage::Display:
      return "display";
    case CreditStage::Cleanup:
      return "cleanup";
  }
  return "preparation";
}

struct CreditMeter::State {
  std::mutex mutex;
  bool bound = false;
  uint64_t token = 0;
  uint64_t next_token = 1;
  std::string usage_id;

  /* One process clock. Everything below is advanced to an operation's timestamp before the
   * operation is applied, so the counter and the trace describe the same instant. */
  int64_t last_at = 0;
  int64_t total = 0;
  int billable = 0;
  bool authorized = false;
  bool granted = false;
  bool ended = false;
  int64_t ceiling = 0;
  int64_t committed = 0;
  int64_t rate = 0;
  int64_t next_scope = 1;
  std::vector<std::pair<int64_t, CreditStage>> open_scopes;

  /* Complete JSON lines waiting to be appended, written outside this mutex. */
  std::vector<std::string> pending;
  std::mutex file_mutex;
  std::FILE *file = nullptr;
  bool file_checked = false;

  std::atomic<StageHook> hook{nullptr};

  void advance(int64_t at);
  void emit(int64_t at, const char *kind, const char *fields = nullptr);
  void flush();
  std::FILE *trace_file_locked();
};

void CreditMeter::State::advance(int64_t at)
{
  if (billable > 0 && authorized) {
    const int64_t room = ceiling - total;
    const int64_t elapsed = at - last_at;
    if (elapsed > 0 && room > 0) {
      total += std::min(elapsed, room);
    }
  }
  last_at = at;
}

void CreditMeter::State::emit(int64_t at, const char *kind, const char *fields)
{
  char line[512];
  const int written = fields == nullptr
                          ? std::snprintf(line,
                                          sizeof(line),
                                          "{\"atNs\":%lld,\"usageId\":\"%s\",\"kind\":\"%s\"}\n",
                                          static_cast<long long>(at),
                                          usage_id.c_str(),
                                          kind)
                          : std::snprintf(line,
                                          sizeof(line),
                                          "{\"atNs\":%lld,\"usageId\":\"%s\",\"kind\":\"%s\",%s}\n",
                                          static_cast<long long>(at),
                                          usage_id.c_str(),
                                          kind,
                                          fields);
  if (written > 0 && static_cast<size_t>(written) < sizeof(line)) {
    pending.emplace_back(line, static_cast<size_t>(written));
  }
}

std::FILE *CreditMeter::State::trace_file_locked()
{
  if (!file_checked) {
    file_checked = true;
    file = credit_open_trace();
  }
  return file;
}

void CreditMeter::State::flush()
{
  /* The file lock is taken before the state lock so a concurrent writer can never append this
   * event's line ahead of an earlier one, while no lock is held across compute. */
  std::lock_guard<std::mutex> file_lock(file_mutex);
  std::vector<std::string> lines;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (pending.empty()) {
      return;
    }
    lines.swap(pending);
  }
  std::FILE *out = trace_file_locked();
  if (out == nullptr) {
    return;
  }
  for (const std::string &line : lines) {
    std::fwrite(line.data(), 1, line.size(), out);
  }
  std::fflush(out);
}

CreditMeter::CreditMeter() : state_(new State()) {}

CreditMeter &CreditMeter::get()
{
  /* Deliberately never destroyed: the meter outlives every adapter and is reached from the
   * render library's own worker threads during teardown. */
  static CreditMeter *meter = new CreditMeter();
  return *meter;
}

uint64_t CreditMeter::bind_usage(const std::string &usage_id)
{
  if (usage_id.empty()) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->bound) {
    return state_->usage_id == usage_id ? state_->token : 0;
  }
  state_->bound = true;
  state_->token = state_->next_token++;
  state_->usage_id = usage_id;
  state_->last_at = credit_now_ns();
  state_->total = 0;
  state_->billable = 0;
  state_->authorized = false;
  state_->granted = false;
  state_->ended = false;
  state_->ceiling = 0;
  state_->committed = 0;
  state_->rate = 0;
  state_->open_scopes.clear();
  return state_->token;
}

void CreditMeter::unbind_usage(uint64_t token)
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (!state_->bound || state_->token != token) {
    return;
  }
  state_->bound = false;
  state_->usage_id.clear();
  state_->authorized = false;
  state_->granted = false;
  state_->billable = 0;
  state_->open_scopes.clear();
}

bool CreditMeter::grant(uint64_t token,
                        int64_t committed_ns,
                        int64_t ceiling_ns,
                        int64_t balance_micro,
                        int64_t remainder,
                        int64_t rate_micro)
{
  bool accepted = false;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->bound || state_->token != token || state_->ended) {
      return false;
    }

    const int64_t at = credit_now_ns();
    state_->advance(at);

    if (committed_ns < state_->committed || committed_ns > state_->total || rate_micro < 1 ||
        (state_->rate != 0 && rate_micro != state_->rate) || remainder < 0 ||
        remainder >= CREDIT_MINUTE_NS || balance_micro < 0 ||
        (balance_micro == 0 && remainder != 0) || ceiling_ns < state_->total) {
      return false;
    }

    /* Funded duration in ns is ceil((balance * minute - remainder) / rate), computed wide because
     * the product is allowed to exceed Int64 even though every account quantity fits it. Only the
     * one-second part past the watermark can be granted, so the result is bounded to that. */
    int64_t funded = 0;
    if (balance_micro > 0) {
#if defined(__SIZEOF_INT128__)
      const __int128 numerator = static_cast<__int128>(balance_micro) * CREDIT_MINUTE_NS -
                                 remainder + rate_micro - 1;
      const __int128 funded_ns = numerator / rate_micro;
      funded = funded_ns > CREDIT_MAX_GRANT_NS ? CREDIT_MAX_GRANT_NS
                                              : static_cast<int64_t>(funded_ns);
#else
      const long double funded_ns =
          (static_cast<long double>(balance_micro) * static_cast<long double>(CREDIT_MINUTE_NS) -
           static_cast<long double>(remainder) + static_cast<long double>(rate_micro) - 1.0L) /
          static_cast<long double>(rate_micro);
      funded = funded_ns > static_cast<long double>(CREDIT_MAX_GRANT_NS)
                   ? CREDIT_MAX_GRANT_NS
                   : static_cast<int64_t>(funded_ns);
#endif
    }
    const int64_t grant = std::min(CREDIT_MAX_GRANT_NS, funded);
    const int64_t limit = committed_ns > std::numeric_limits<int64_t>::max() - grant
                              ? std::numeric_limits<int64_t>::max()
                              : committed_ns + grant;
    if (ceiling_ns > limit) {
      return false;
    }

    state_->committed = committed_ns;
    state_->ceiling = ceiling_ns;
    state_->rate = rate_micro;
    state_->authorized = true;
    state_->granted = true;

    char fields[256];
    std::snprintf(fields,
                  sizeof(fields),
                  "\"activeNs\":%lld,\"committedNs\":%lld,\"ceilingNs\":%lld,"
                  "\"balanceMicro\":%lld,\"remainder\":%lld,\"rateMicro\":%lld",
                  static_cast<long long>(state_->total),
                  static_cast<long long>(committed_ns),
                  static_cast<long long>(ceiling_ns),
                  static_cast<long long>(balance_micro),
                  static_cast<long long>(remainder),
                  static_cast<long long>(rate_micro));
    state_->emit(at, "grant", fields);
    accepted = true;
  }
  if (accepted) {
    state_->flush();
  }
  return accepted;
}

void CreditMeter::revoke()
{
  bool accepted = false;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    /* Ending an ended usage is not a second acceptance: only the first call closes the interval. */
    if (!state_->bound || state_->ended) {
      return;
    }
    const int64_t at = credit_now_ns();
    state_->advance(at);
    state_->authorized = false;
    state_->ended = true;
    state_->emit(at, "revoke");
    accepted = true;
  }
  if (accepted) {
    state_->flush();
  }
}

void CreditMeter::suspend()
{
  bool accepted = false;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->bound || !state_->authorized) {
      return;
    }
    const int64_t at = credit_now_ns();
    state_->advance(at);
    state_->authorized = false;
    state_->emit(at, "revoke");
    accepted = true;
  }
  if (accepted) {
    state_->flush();
  }
}

int64_t CreditMeter::active_ns(uint64_t token)
{
  int64_t total = -1;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->bound || state_->token != token) {
      return -1;
    }
    const int64_t at = credit_now_ns();
    state_->advance(at);
    total = state_->total;

    char fields[64];
    std::snprintf(fields, sizeof(fields), "\"activeNs\":%lld", static_cast<long long>(total));
    state_->emit(at, "observe", fields);
  }
  state_->flush();
  return total;
}

bool CreditMeter::work_authorized(uint64_t token)
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (!state_->bound || token == 0) {
    return true;
  }
  return state_->token == token && state_->authorized && !state_->ended;
}

bool CreditMeter::resumable(uint64_t token)
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (!state_->bound || token == 0) {
    return true;
  }
  return state_->token == token && state_->granted && !state_->ended;
}

void CreditMeter::note_reset()
{
  bool observed = false;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->bound) {
      return;
    }
    const int64_t at = credit_now_ns();
    state_->advance(at);

    char fields[64];
    std::snprintf(fields, sizeof(fields), "\"activeNs\":%lld", static_cast<long long>(state_->total));
    state_->emit(at, "reset", fields);
    observed = true;
  }
  if (observed) {
    state_->flush();
  }
}

void CreditMeter::note_idle()
{
  bool observed = false;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->bound) {
      return;
    }
    const int64_t at = credit_now_ns();
    state_->advance(at);

    char fields[64];
    std::snprintf(fields, sizeof(fields), "\"activeNs\":%lld", static_cast<long long>(state_->total));
    state_->emit(at, "idle", fields);
    observed = true;
  }
  if (observed) {
    state_->flush();
  }
}

void CreditMeter::set_stage_hook(StageHook hook)
{
  state_->hook.store(hook, std::memory_order_release);
}

void CreditMeter::run_stage_hook(CreditStage stage)
{
  const StageHook hook = state_->hook.load(std::memory_order_acquire);
  if (hook == nullptr) {
    return;
  }
  /* The usage is read under the meter's lock and the hook runs outside it, so a hook that blocks
   * never delays the counter, observations or revocation. */
  std::string usage_id;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->bound) {
      return;
    }
    usage_id = state_->usage_id;
  }
  hook(stage, usage_id);
}

int64_t CreditMeter::scope_enter(CreditStage stage)
{
  int64_t scope_id = -1;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->bound) {
      return -1;
    }
    const int64_t at = credit_now_ns();
    state_->advance(at);

    scope_id = state_->next_scope++;
    const bool billable = credit_stage_is_billable(stage);
    if (billable) {
      state_->billable++;
    }
    state_->open_scopes.emplace_back(scope_id, stage);

    char fields[96];
    std::snprintf(fields,
                  sizeof(fields),
                  "\"scopeId\":%lld,\"stage\":\"%s\"",
                  static_cast<long long>(scope_id),
                  credit_stage_name(stage));
    state_->emit(at, "scope-enter", fields);
  }
  state_->flush();
  return scope_id;
}

void CreditMeter::scope_exit(int64_t scope_id)
{
  if (scope_id < 0) {
    return;
  }
  bool exited = false;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->bound) {
      return;
    }
    auto found = std::find_if(state_->open_scopes.begin(),
                              state_->open_scopes.end(),
                              [scope_id](const std::pair<int64_t, CreditStage> &scope) {
                                return scope.first == scope_id;
                              });
    if (found == state_->open_scopes.end()) {
      return;
    }
    const CreditStage stage = found->second;
    const int64_t at = credit_now_ns();
    /* The interval up to the actual end of the stage is still billable work. */
    state_->advance(at);
    if (credit_stage_is_billable(stage)) {
      state_->billable--;
    }
    state_->open_scopes.erase(found);

    char fields[96];
    std::snprintf(fields,
                  sizeof(fields),
                  "\"scopeId\":%lld,\"stage\":\"%s\"",
                  static_cast<long long>(scope_id),
                  credit_stage_name(stage));
    state_->emit(at, "scope-exit", fields);
    exited = true;
  }
  if (exited) {
    state_->flush();
  }
}

CCL_NAMESPACE_END
