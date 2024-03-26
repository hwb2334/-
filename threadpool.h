#ifndef THREADPOOL_H
#define THREADPOOL_H

#include<iostream>
#include<vector>
#include<queue>
#include<memory>
#include<condition_variable>
#include<mutex>
#include<atomic>
#include<functional>
#include<thread>
#include<chrono>
#include<unordered_map>
#include<future>

const int TASK_MAX_THRESHHOLD = 2; // INT32_MAX;
const int THREAD_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_IDLE_TIME = 60;

enum class PoolMode 
{
	MODE_FIXED, // 固定线程数量
	MODE_CACHED // 线程数量动态增长
};
// 线程
class Thread {
public:
	// 线程函数对象类型
	using ThreadFunc = std::function<void(int)>;
	// 构造函数
	Thread(ThreadFunc func) 
		: func_(func)
		, threadId_(generateId_++)
	{}
	// 析构函数
	~Thread()
	{}
	int getId() const {
		return threadId_;
	}
	// 启动线程
	void start()
	{
		// 创建一个线程来执行一个线程函数
		std::thread t(func_, threadId_); // C++11来说 线程对象t 和线程函数func_
		t.detach(); // 设置分离线程
	}
private:
	ThreadFunc func_;
	static int generateId_;
	int threadId_;
};

int Thread::generateId_ = 0;

// 线程池
class ThreadPool
{
public:
	// 线程池构造
	ThreadPool() 
		: initThreadSize_(0)
		, threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
		, taskSize_(0)
		, curThreadSize_(0)
		, idleThreadSize_(0)
		, taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
		, poolMode_(PoolMode::MODE_FIXED)
		, isPoolRunning_(false)
	{}

	// 线程池析构
	~ThreadPool() 
	{
		isPoolRunning_ = false;
		// 等待线程池里面所有的线程返回   有两种状态：阻塞 & 增在执行任务中
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		notEmpty_.notify_all();
		exitCond_.wait(lock, [&]()->bool {return threads_.size() == 0; });
	}

	// 设置线程池的工作模式
	void setMode(PoolMode mode) 
	{
		if (checkRunningState())
			return;
		poolMode_ = mode;
	}

	// 设置task任务队列上限阈值
	void setTaskQueMaxThreshHold(size_t threshold)
	{
		taskQueMaxThreshHold_ = threshold;
	}

	// cached模式设置线程池线程阈值
	void setThreadSizeThreshHold(size_t threshold)
	{
		if (checkRunningState())
			return;
		if (poolMode_ == PoolMode::MODE_CACHED) {
			threadSizeThreshHold_ = threshold;
		}
	}

	// 给线程池提交任务
	// 使用可变参数模板，让submitTask可以接受任意函数和任意数量的参数
	// pool.submitTask(sum1, 10, 20);   csdn  大秦坑王  右值引用+引用折叠原理
	// 返回值future<>
	template<typename Func, typename... Args>
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
	{
		// 打包任务，放入任务队列
		using RType = decltype(func(args...));
		auto task = std::make_shared<std::packaged_task<RType()>>(
			std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
		std::future<RType> result = task->get_future();
		// 获取锁
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		// 线程通讯，等待任务队列有空位
		// 用户提交任务，最长不能超过1S 否则判断提交任务失败，返回 wait wait_for wait_until
		if (!notFull_.wait_for(lock, std::chrono::seconds(1), [&]()-> bool {return taskQue_.size() < taskQueMaxThreshHold_; }))
		{
			// 表示notFull_等待1s钟，条件依然没有满足
			std::cerr << "task queue is full, submit task fail" << std::endl;
			// return task->getResult();  // Task Result 线程执行完task task对象就被析构掉啦
			task = std::make_shared<std::packaged_task<RType()>>([]() -> RType { return RType(); });
			(*task)();
			return task->get_future();
		}
		// 如果有空位，把任务加入队列中
		
		taskSize_++;
		// 新放任务，任务队列不为空，在not_empty上通知
		// using Task = std::function<void()>;
		taskQue_.emplace([task]() {(*task)();});
		notEmpty_.notify_all();

		// 需要根据任务数量和空闲线程的数量，判断是否需要创建新的线程出来？
		// cached模式 任务处理比较紧急 场景：小而快的任务
		if (poolMode_ == PoolMode::MODE_CACHED
			&& taskSize_ > idleThreadSize_
			&& curThreadSize_ < threadSizeThreshHold_) {
			std::cout << "create new thread..." << std::endl;
			// 新建线程对象
			std::unique_ptr<Thread> ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));
			// 启动线程

			// ptr->start(); std::move(ptr) 此时已经转移所有权啦
			threads_[threadId]->start();
			// 更新变量
			curThreadSize_++;
			idleThreadSize_++;
		}

		// return task->getReult(); // Task Result 线程执行完task task对象就被析构掉啦
		return result;
	}

	// 开启线程池
	void start(size_t size = std::thread::hardware_concurrency())
	{
		isPoolRunning_ = true;
		// 记录初始线程个数
		initThreadSize_ = size;
		curThreadSize_ = size;

		// 创建线程对象
		for (size_t i = 0; i < initThreadSize_; i++) {
			std::unique_ptr<Thread> ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));
		}

		// 启动所有线程 std::vector<Thread*> threads_;
		for (size_t i = 0; i < initThreadSize_; i++) {
			threads_[i]->start(); // 需要去执行一个线程函数
			idleThreadSize_++; // 记录初始空闲线程的数量
		}
	}

	ThreadPool(const ThreadPool&) = delete;

	ThreadPool& operator=(const ThreadPool&) = delete;
private:
	void threadFunc(int threadId) // 线程函数
	{
		auto lastTime = std::chrono::high_resolution_clock().now();
		for (;;)
		{
			Task task;
			{
				// 先获取锁
				std::unique_lock<std::mutex> lock(taskQueMtx_);
				std::cout << "tid:" << std::this_thread::get_id()
					<< "尝试获取任务。。。。" << std::endl;

				// cached模式下，有可能已经创建了很多的线程，但是空闲时间超过60s，应该把多余的线程
				// 结束回收掉(超过initThreadSize_数量的线程要回收)
				// 当前时间 - 上一次线程执行时间
				// 锁 + 双重绑定
				while (taskQue_.size() == 0) {
					if (!isPoolRunning_) {
						threads_.erase(threadId);
						std::cout << "threadid：" << std::this_thread::get_id() << "exit"
							<< std::endl;
						exitCond_.notify_all();
						return;
					}// 线程池要结束，回收线程资源
					if (poolMode_ == PoolMode::MODE_CACHED)
					{
						// 每一秒返回一次   怎么区分：超时返回？还是有任务待执行返回
						// 条件变量 超时返回了
						if (std::cv_status::timeout ==
							notEmpty_.wait_for(lock, std::chrono::seconds(1)))
						{
							auto now = std::chrono::high_resolution_clock().now();
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
							if (dur.count() >= THREAD_MAX_IDLE_TIME
								&& curThreadSize_ > initThreadSize_)
							{
								// 开始回收当前线程
								// 记录线程数量的相关变量的值修改
								// 把线程对象从线程列表容器中删除   没有办法 threadFunc 《=》 thread
								// threadId => thread对象 => 删除
								threads_.erase(threadId); // std::this_thread::get_id();
								curThreadSize_--;
								idleThreadSize_--;
								std::cout << "threadid：" << std::this_thread::get_id() << "exit"
									<< std::endl;
								return;
							}
						}
					}
					else {
						// 等待notEmpty条件
						notEmpty_.wait(lock);
					}
				}

				idleThreadSize_--;
				std::cout << "tid:" << std::this_thread::get_id()
					<< "获取任务成功。。。。" << std::endl;
				// 从任务队列中取一个任务出来
				task = taskQue_.front();
				taskQue_.pop();
				taskSize_--;

				// 如果依然有任务，继续通知其他线程继续取任务
				if (taskQue_.size() > 0) {
					notEmpty_.notify_all();
				}

				// 取出一个任务进行通知
				notFull_.notify_all();
			}// 就应该把锁释放掉

			// 当前线程负责执行这个任务
			if (task != nullptr) {
				task();
			}
			lastTime = std::chrono::high_resolution_clock().now(); // 更新线程执行完任务的时间
			idleThreadSize_++;
		}
	}
	bool checkRunningState() const 
	{
		return isPoolRunning_;
	}
private:
	//std::vector<std::unique_ptr<Thread>> threads_; // 线程列表
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;// 线程列表
	size_t initThreadSize_; // 初始的线程数量
	size_t threadSizeThreshHold_; // 线程数量上线阈值
	std::atomic_int idleThreadSize_; // 记录空闲线程的数量
	std::atomic_int curThreadSize_; // 记录当前线程池里面线程的总数量

	// Task任务 =》 函数对象
	using Task = std::function<void()>;
	std::queue<Task> taskQue_; // 任务队列
	std::atomic_int taskSize_; // 任务的数量
	size_t taskQueMaxThreshHold_; // 任务队列数量上线阈值

	std::mutex taskQueMtx_; // 保证任务队列的线程安全
	std::condition_variable notFull_; // 表示任务队列不满
	std::condition_variable notEmpty_; // 表示任务队列不空
	std::condition_variable exitCond_; // 等到线程资源全部回收

	PoolMode poolMode_; // 当前线程池工作模式
	std::atomic_bool isPoolRunning_; // 表示当前线程池的运行状态
};
#endif


