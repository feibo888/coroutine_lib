#include "thread.h"
#include <iostream>
#include <vector>

void func()
{
    std::cout << "id: " << Thread::getThreadId() << " name: " << Thread::getCurrentThreadName();
    std::cout << ", this id: " << Thread::getThis()->getId() << ", this name: " << Thread::getThis()->getName() << std::endl;

    sleep(60);
}

int main(void)
{
    std::vector<std::shared_ptr<Thread>> thrs;

    for(int i = 0; i < 5; ++i)
    {
        std::shared_ptr<Thread> thr = std::make_shared<Thread>(&func, "thread_" + std::to_string(i));
        thrs.push_back(thr);
    }

    for(int i = 0; i < 5; ++i)
    {
        thrs[i]->join();
    }


    return 0;
}