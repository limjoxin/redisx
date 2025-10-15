#pragma once
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace redisx {

    class ThreadPool {
    public:
        explicit ThreadPool(size_t n) : stop_(false) {
            if (n == 0) n = 1;
            workers_.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                workers_.emplace_back([this] {
                    for (;;) {
                        std::function<void()> job;
                        {
                            std::unique_lock<std::mutex> lk(m_);
                            cv_.wait(lk, [&] { return stop_ || !q_.empty(); });
                            if (stop_ && q_.empty()) return;
                            job = std::move(q_.front()); q_.pop();
                        }
                        job();
                    }
                    });
            }
        }
        ~ThreadPool() {
            { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
            cv_.notify_all();
            for (auto& t : workers_) t.join();
        }

        template<class F, class... A>
        auto enqueue(F&& f, A&&... a) -> std::future<std::invoke_result_t<F, A...>> {
            using R = std::invoke_result_t<F, A...>;
            auto task = std::make_shared<std::packaged_task<R()>>(std::bind(std::forward<F>(f), std::forward<A>(a)...));
            std::future<R> fut = task->get_future();
            {
                std::lock_guard<std::mutex> lk(m_);
                q_.emplace([task] { (*task)(); });
            }
            cv_.notify_one();
            return fut;
        }

    private:
        std::mutex m_;
        std::condition_variable cv_;
        std::queue<std::function<void()>> q_;
        std::vector<std::thread> workers_;
        bool stop_;
    };

} // namespace redisx

