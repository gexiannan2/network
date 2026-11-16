#include "MyThread.h"

namespace MyNameSpace
{
    bool MyThread::start()
    {
        if (started_.exchange(true))
        {
            return true;
        }

        try
        {
            thread_ = std::thread(
                [this]()
                {
                    run();
                });
        }
        catch (...)
        {
            started_.store(false);
            return false;
        }

        return true;
    }

    void MyThread::join()
    {
        if (thread_.joinable())
        {
            thread_.join();
        }
    }
}
