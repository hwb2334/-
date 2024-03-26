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
	MODE_FIXED, // �̶��߳�����
	MODE_CACHED // �߳�������̬����
};
// �߳�
class Thread {
public:
	// �̺߳�����������
	using ThreadFunc = std::function<void(int)>;
	// ���캯��
	Thread(ThreadFunc func) 
		: func_(func)
		, threadId_(generateId_++)
	{}
	// ��������
	~Thread()
	{}
	int getId() const {
		return threadId_;
	}
	// �����߳�
	void start()
	{
		// ����һ���߳���ִ��һ���̺߳���
		std::thread t(func_, threadId_); // C++11��˵ �̶߳���t ���̺߳���func_
		t.detach(); // ���÷����߳�
	}
private:
	ThreadFunc func_;
	static int generateId_;
	int threadId_;
};

int Thread::generateId_ = 0;

// �̳߳�
class ThreadPool
{
public:
	// �̳߳ع���
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

	// �̳߳�����
	~ThreadPool() 
	{
		isPoolRunning_ = false;
		// �ȴ��̳߳��������е��̷߳���   ������״̬������ & ����ִ��������
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		notEmpty_.notify_all();
		exitCond_.wait(lock, [&]()->bool {return threads_.size() == 0; });
	}

	// �����̳߳صĹ���ģʽ
	void setMode(PoolMode mode) 
	{
		if (checkRunningState())
			return;
		poolMode_ = mode;
	}

	// ����task�������������ֵ
	void setTaskQueMaxThreshHold(size_t threshold)
	{
		taskQueMaxThreshHold_ = threshold;
	}

	// cachedģʽ�����̳߳��߳���ֵ
	void setThreadSizeThreshHold(size_t threshold)
	{
		if (checkRunningState())
			return;
		if (poolMode_ == PoolMode::MODE_CACHED) {
			threadSizeThreshHold_ = threshold;
		}
	}

	// ���̳߳��ύ����
	// ʹ�ÿɱ����ģ�壬��submitTask���Խ������⺯�������������Ĳ���
	// pool.submitTask(sum1, 10, 20);   csdn  ���ؿ���  ��ֵ����+�����۵�ԭ��
	// ����ֵfuture<>
	template<typename Func, typename... Args>
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
	{
		// ������񣬷����������
		using RType = decltype(func(args...));
		auto task = std::make_shared<std::packaged_task<RType()>>(
			std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
		std::future<RType> result = task->get_future();
		// ��ȡ��
		std::unique_lock<std::mutex> lock(taskQueMtx_);
		// �߳�ͨѶ���ȴ���������п�λ
		// �û��ύ��������ܳ���1S �����ж��ύ����ʧ�ܣ����� wait wait_for wait_until
		if (!notFull_.wait_for(lock, std::chrono::seconds(1), [&]()-> bool {return taskQue_.size() < taskQueMaxThreshHold_; }))
		{
			// ��ʾnotFull_�ȴ�1s�ӣ�������Ȼû������
			std::cerr << "task queue is full, submit task fail" << std::endl;
			// return task->getResult();  // Task Result �߳�ִ����task task����ͱ���������
			task = std::make_shared<std::packaged_task<RType()>>([]() -> RType { return RType(); });
			(*task)();
			return task->get_future();
		}
		// ����п�λ����������������
		
		taskSize_++;
		// �·�����������в�Ϊ�գ���not_empty��֪ͨ
		// using Task = std::function<void()>;
		taskQue_.emplace([task]() {(*task)();});
		notEmpty_.notify_all();

		// ��Ҫ�������������Ϳ����̵߳��������ж��Ƿ���Ҫ�����µ��̳߳�����
		// cachedģʽ ������ȽϽ��� ������С���������
		if (poolMode_ == PoolMode::MODE_CACHED
			&& taskSize_ > idleThreadSize_
			&& curThreadSize_ < threadSizeThreshHold_) {
			std::cout << "create new thread..." << std::endl;
			// �½��̶߳���
			std::unique_ptr<Thread> ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));
			// �����߳�

			// ptr->start(); std::move(ptr) ��ʱ�Ѿ�ת������Ȩ��
			threads_[threadId]->start();
			// ���±���
			curThreadSize_++;
			idleThreadSize_++;
		}

		// return task->getReult(); // Task Result �߳�ִ����task task����ͱ���������
		return result;
	}

	// �����̳߳�
	void start(size_t size = std::thread::hardware_concurrency())
	{
		isPoolRunning_ = true;
		// ��¼��ʼ�̸߳���
		initThreadSize_ = size;
		curThreadSize_ = size;

		// �����̶߳���
		for (size_t i = 0; i < initThreadSize_; i++) {
			std::unique_ptr<Thread> ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			int threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));
		}

		// ���������߳� std::vector<Thread*> threads_;
		for (size_t i = 0; i < initThreadSize_; i++) {
			threads_[i]->start(); // ��Ҫȥִ��һ���̺߳���
			idleThreadSize_++; // ��¼��ʼ�����̵߳�����
		}
	}

	ThreadPool(const ThreadPool&) = delete;

	ThreadPool& operator=(const ThreadPool&) = delete;
private:
	void threadFunc(int threadId) // �̺߳���
	{
		auto lastTime = std::chrono::high_resolution_clock().now();
		for (;;)
		{
			Task task;
			{
				// �Ȼ�ȡ��
				std::unique_lock<std::mutex> lock(taskQueMtx_);
				std::cout << "tid:" << std::this_thread::get_id()
					<< "���Ի�ȡ���񡣡�����" << std::endl;

				// cachedģʽ�£��п����Ѿ������˺ܶ���̣߳����ǿ���ʱ�䳬��60s��Ӧ�ðѶ�����߳�
				// �������յ�(����initThreadSize_�������߳�Ҫ����)
				// ��ǰʱ�� - ��һ���߳�ִ��ʱ��
				// �� + ˫�ذ�
				while (taskQue_.size() == 0) {
					if (!isPoolRunning_) {
						threads_.erase(threadId);
						std::cout << "threadid��" << std::this_thread::get_id() << "exit"
							<< std::endl;
						exitCond_.notify_all();
						return;
					}// �̳߳�Ҫ�����������߳���Դ
					if (poolMode_ == PoolMode::MODE_CACHED)
					{
						// ÿһ�뷵��һ��   ��ô���֣���ʱ���أ������������ִ�з���
						// �������� ��ʱ������
						if (std::cv_status::timeout ==
							notEmpty_.wait_for(lock, std::chrono::seconds(1)))
						{
							auto now = std::chrono::high_resolution_clock().now();
							auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
							if (dur.count() >= THREAD_MAX_IDLE_TIME
								&& curThreadSize_ > initThreadSize_)
							{
								// ��ʼ���յ�ǰ�߳�
								// ��¼�߳���������ر�����ֵ�޸�
								// ���̶߳�����߳��б�������ɾ��   û�а취 threadFunc ��=�� thread
								// threadId => thread���� => ɾ��
								threads_.erase(threadId); // std::this_thread::get_id();
								curThreadSize_--;
								idleThreadSize_--;
								std::cout << "threadid��" << std::this_thread::get_id() << "exit"
									<< std::endl;
								return;
							}
						}
					}
					else {
						// �ȴ�notEmpty����
						notEmpty_.wait(lock);
					}
				}

				idleThreadSize_--;
				std::cout << "tid:" << std::this_thread::get_id()
					<< "��ȡ����ɹ���������" << std::endl;
				// �����������ȡһ���������
				task = taskQue_.front();
				taskQue_.pop();
				taskSize_--;

				// �����Ȼ�����񣬼���֪ͨ�����̼߳���ȡ����
				if (taskQue_.size() > 0) {
					notEmpty_.notify_all();
				}

				// ȡ��һ���������֪ͨ
				notFull_.notify_all();
			}// ��Ӧ�ð����ͷŵ�

			// ��ǰ�̸߳���ִ���������
			if (task != nullptr) {
				task();
			}
			lastTime = std::chrono::high_resolution_clock().now(); // �����߳�ִ���������ʱ��
			idleThreadSize_++;
		}
	}
	bool checkRunningState() const 
	{
		return isPoolRunning_;
	}
private:
	//std::vector<std::unique_ptr<Thread>> threads_; // �߳��б�
	std::unordered_map<int, std::unique_ptr<Thread>> threads_;// �߳��б�
	size_t initThreadSize_; // ��ʼ���߳�����
	size_t threadSizeThreshHold_; // �߳�����������ֵ
	std::atomic_int idleThreadSize_; // ��¼�����̵߳�����
	std::atomic_int curThreadSize_; // ��¼��ǰ�̳߳������̵߳�������

	// Task���� =�� ��������
	using Task = std::function<void()>;
	std::queue<Task> taskQue_; // �������
	std::atomic_int taskSize_; // ���������
	size_t taskQueMaxThreshHold_; // �����������������ֵ

	std::mutex taskQueMtx_; // ��֤������е��̰߳�ȫ
	std::condition_variable notFull_; // ��ʾ������в���
	std::condition_variable notEmpty_; // ��ʾ������в���
	std::condition_variable exitCond_; // �ȵ��߳���Դȫ������

	PoolMode poolMode_; // ��ǰ�̳߳ع���ģʽ
	std::atomic_bool isPoolRunning_; // ��ʾ��ǰ�̳߳ص�����״̬
};
#endif


