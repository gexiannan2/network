#include "MySockTask.h"
#include "MySockTaskManager.h"

namespace MyNameSpace
{
    int MySockTask::getMsg()
    {
        int messages = 0;
        for (;;)
        {
            std::vector<char> payload;
            int size = sock_.readBuffer(payload);
            if (size < 0)
            {
                return -1;
            }

            if (size == 0)
            {
                return messages;
            }

            if (!cmdParse(payload.data(), size))
            {
                return -1;
            }

            MySockTaskManager::getInstance().recordMessage(static_cast<std::size_t>(size));
            ++messages;
        }
    }
}
