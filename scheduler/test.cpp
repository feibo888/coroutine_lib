#include "scheduler.h"


static unsigned int test_number;
std::mutex mutex_cout;


void task()
{
    {
        std::lock_guard<std::mutex> lock(mutex_cout);
        std::cout << "task: " << test_number++ << " is under processing in thread: " << Thread::getThreadId() << std::endl;
    }
    sleep(1);
}



int main(void)
{
    {
        std::shared_ptr<Scheduler> scheduler = std::make_shared<Scheduler>(8, true, "scheduler_1");

        scheduler->start();

        sleep(2);

        /*
        一个协程从创建到执行完毕的引用计数变化：

        1、创建时：初始引用计数 = 1（由创建者持有）
        2、加入队列：引用计数 = 2（创建者 + 队列）
        3、创建者释放：引用计数 = 1（仅队列持有）
        4、从队列取出：引用计数 = 1（局部变量 task 持有）
        5、执行时获取引用：引用计数 = 2（task + curr）
        6、curr.reset()：引用计数 = 1（仅 task 持有）
        7、task.reset()：引用计数 = 0（对象被销毁）
        */
        std::cout << "\nbegin post\n\n";
        for(int i = 0; i < 5; ++i)
        {
            std::shared_ptr<Fiber> fiber = std::make_shared<Fiber>(task);
            scheduler->scheduleLock(fiber);
        }

        sleep(6);

        std::cout << "\npost again\n\n";
        for(int i = 0; i < 15; ++i)
        {
            std::shared_ptr<Fiber> fiber = std::make_shared<Fiber>(task);
            scheduler->scheduleLock(fiber);
        }

        sleep(3);
        // scheduler如果有设置将加入工作处理
        scheduler->stop();

    }



    return 0;
}
