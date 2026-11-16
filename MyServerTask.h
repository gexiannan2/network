#pragma once
#include "base/MySockTask.h"

namespace MyNameSpace
{
    class MyServerTask final : public MySockTask
    {
        public:
            MyServerTask(net::Socket fd, std::uint64_t id) : MySockTask(fd, id)
            {
            }

            bool cmdParse(const char *payload, int length) override;
    };
}
