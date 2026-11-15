#pragma once
#include "MySockTask.h"
#include "MyThread.h"
#include <cstddef>
#include <memory>

namespace MyNameSpace
{
    // Contract shared by platform-specific I/O implementations.
    class IoBackend : public MyThread
    {
        public:
            virtual bool ready() const = 0;
            virtual bool enqueue(const std::shared_ptr<MySockTask>& task) = 0;
            virtual std::size_t load() const = 0;
    };
}
