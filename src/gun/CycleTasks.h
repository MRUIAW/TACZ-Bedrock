#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace tacz::gun {

// CycleTaskHelper.java:18-99. Float seconds, strict >, and even the delayed
// branch's cumulative elapsed-time/zero-before-subtraction quirks are intentional.
class CycleTasks {
public:
    using Clock = std::function<int64_t()>;
    using Task = std::function<bool()>;
    enum class Kind { Shot, Async };
    static constexpr size_t Limit = 256;

    explicit CycleTasks(Clock clock) : now(std::move(clock)) {}
    bool add(Task task, int64_t delay, int64_t period, int cycles, Kind kind) {
        if (tasks.size() >= Limit) return false;
        auto ticker = std::make_shared<Ticker>();
        ticker->task = std::move(task);
        ticker->period = static_cast<float>(period) / 1000.f;
        ticker->cycles = cycles;
        ticker->kind = kind;
        tasks.push_back(ticker); // reserve capacity before a reentrant immediate call
        if (delay > 0) {
            ticker->delay = static_cast<float>(delay) / 1000.f;
            ticker->timestamp = now();
        } else ticker->active = advance(*ticker);
        prune();
        return true;
    }
    void tick() {
        auto snapshot = tasks; // tasks created by callbacks wait until the next tick
        for (auto const& t : snapshot) if (t->active) t->active = advance(*t);
        prune();
    }
    void clear() {
        for (auto const& t : tasks) t->active = false;
        tasks.clear();
    }
    void cancelShots() {
        for (auto const& t : tasks) if (t->kind == Kind::Shot) t->active = false;
        prune();
    }
    [[nodiscard]] bool pendingShots() const {
        return std::any_of(tasks.begin(), tasks.end(), [](auto const& t) { return t->active && t->kind == Kind::Shot; });
    }
    [[nodiscard]] size_t size() const { return tasks.size(); }

private:
    struct Ticker {
        Task task;
        Kind kind = Kind::Async;
        float period = 0, delay = 0, compensation = 0;
        int64_t timestamp = -1;
        int cycles = 0, count = 0;
        bool active = true;
    };
    Clock now;
    std::vector<std::shared_ptr<Ticker>> tasks;
    void prune() { std::erase_if(tasks, [](auto const& t) { return !t->active; }); }
    bool run(Ticker& t) {
        if (!t.active || (t.cycles > 0 && ++t.count > t.cycles)) return false;
        return t.task() && t.active;
    }
    bool advance(Ticker& t) {
        if (t.timestamp == -1) {
            t.timestamp = now();
            return run(t);
        }
        float duration = static_cast<float>(now() - t.timestamp) / 1000.f + t.compensation;
        if (t.delay > 0) {
            if (t.delay > duration) { t.delay -= duration; return true; }
            t.delay = 0;
            duration = duration - t.delay + t.period;
        }
        if (duration > t.period) {
            t.compensation = duration;
            t.timestamp = now();
            while (t.compensation > t.period) {
                if (!run(t)) return false;
                t.compensation -= t.period;
            }
        }
        return t.active;
    }
};

} // namespace tacz::gun
