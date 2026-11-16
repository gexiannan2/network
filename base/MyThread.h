#pragma once
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace MyNameSpace
{
    class MyThread
    {
        public:
            MyThread() = default;

            virtual ~MyThread()
            {
                terminate();
                join();
            }

            MyThread(const MyThread &) = delete;
            MyThread &operator=(const MyThread &) = delete;
            bool start();

            void terminate()
            {
                finished_.store(true);
            }

            bool isFini() const
            {
                return finished_.load();
            }

            bool isStart() const
            {
                return started_.load();
            }

            void join();
            virtual void run() = 0;

        private:
            std::thread thread_;
            std::atomic<bool> finished_{false}, started_{false};
    };

    class MyThreadPool
    {
        public:
            void putThread(std::unique_ptr<MyThread> thread)
            {
                threads_.push_back(std::move(thread));
            }

            MyThread *getThreadByIndex(std::size_t n)
            {
                return threads_.at(n).get();
            }

            std::size_t getThreadCount() const
            {
                return threads_.size();
            }

            void joinAll()
            {
                for (auto &t : threads_)
                {
                    t->terminate();
                }

                for (auto &t : threads_)
                {
                    t->join();
                }
            }

            void delAllThread()
            {
                threads_.clear();
            }

        private:
            std::vector<std::unique_ptr<MyThread>> threads_;
    };
}
