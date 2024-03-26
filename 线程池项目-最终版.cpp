#include "threadpool.h"
using namespace std;
/*
如何能让线程池提交任务更加方便
1. pool.submitTask(sum1, 10, 20);
   pool.submitTask(sum2, 1, 2, 3);
   submitTask; 可变模板编程
2. 我们自己造了一个Result以及相关的类型，代码挺多
   c++11 线程库    thread     packaged_task(function函数对象)  async
   使用future来代替Result节省线程池代码
*/
int sum1(int a, int b)
{
    // 比较耗时
    std::this_thread::sleep_for(std::chrono::seconds(2));
    return a + b;
}
int sum2(int a, int b, int c)
{
    std::this_thread::sleep_for(std::chrono::seconds(2));
    return a + b + c;
}
int main()
{
    ThreadPool pool;
    // pool.setMode(PoolMode::MODE_CACHED);
    pool.start(2);
    future<int> res1 = pool.submitTask(sum1, 1, 2);
    future<int> res2 = pool.submitTask(sum2, 1, 2, 3);
    future<int> res3 = pool.submitTask([](int b, int e)->int{
        int sum = 0;
        for (int i = b; i <= e; i++) {
            sum += i;
        }
        return sum;
    },1,100000000);
    future<int> res4 = pool.submitTask([](int b, int e)->int {
        int sum = 0;
        for (int i = b; i <= e; i++) {
            sum += i;
        }
        return sum;
        }, 1, 100000000);
    future<int> res5 = pool.submitTask([](int b, int e)->int {
        int sum = 0;
        for (int i = b; i <= e; i++) {
            sum += i;
        }
        return sum;
        }, 1, 100000000);
    cout << res1.get() << endl;
    cout << res2.get() << endl;
    cout << res3.get() << endl;
    cout << res4.get() << endl;
    cout << res5.get() << endl;

    //packaged_task<int(int, int)> task(sum1);
    //// future <=> Result
    //future<int> res = task.get_future();
    ////task(10, 20);
    //thread t3(std::move(task), 10, 20);
    //t3.detach();
    //cout << res.get() << endl;

    /*thread t1(sum1, 10, 20);
    thread t2(sum2, 1, 2, 3);
    t1.join();
    t2.join();*/
    return 0;
}