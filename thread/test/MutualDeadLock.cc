#include "../Mutex.h"
#include "../Thread.h"
#include <set>
#include <stdio.h>

class Request;

class Inventory
{
 // Inventory作为共享对象，内部通过加锁实现多线程同步访问该对象
 // 不需要用户进行并发控制
 public:
  void add(Request* req)
  {
    muduo::MutexLockGuard lock(mutex_);
    requests_.insert(req);
  }

  void remove(Request* req) __attribute__ ((noinline))
  {
    muduo::MutexLockGuard lock(mutex_);
    requests_.erase(req);
  }

  void printAll() const;

 private:
  mutable muduo::MutexLock mutex_;
  std::set<Request*> requests_;
};

Inventory g_inventory;

class Request
{
 public:
  void process() // __attribute__ ((noinline))
  {
    muduo::MutexLockGuard lock(mutex_);
    g_inventory.add(this);
    // ...
  }

  ~Request() __attribute__ ((noinline))
  {
    muduo::MutexLockGuard lock(mutex_);
    sleep(1);           // 为了容易复现死锁，使用延时
    g_inventory.remove(this);
  }

  void print() const __attribute__ ((noinline))
  {
    muduo::MutexLockGuard lock(mutex_);
    // ...
  }

 private:
  mutable muduo::MutexLock mutex_;
};

void Inventory::printAll() const
{
  muduo::MutexLockGuard lock(mutex_);
  sleep(1);     // 为了容易复现死锁，使用延时
  for (std::set<Request*>::const_iterator it = requests_.begin();
      it != requests_.end();
      ++it)
  {
    (*it)->print();
  }
  printf("Inventory::printAll() unlocked\n");
}

/*
void Inventory::printAll() const
{
  std::set<Request*> requests
  {
    muduo::MutexLockGuard lock(mutex_);
    requests = requests_;
  }
  for (std::set<Request*>::const_iterator it = requests.begin();
      it != requests.end();
      ++it)
  {
    (*it)->print();
  }
}
*/

void threadFunc()
{
  Request* req = new Request;
  // 调用process时加锁顺序： Request's mutex -> Inventory's mutex
  req->process();
  // delete时加锁顺序： Request's mutex -> Inventory's mutex
  delete req;
}

int main()
{
  muduo::Thread thread(threadFunc);
  thread.start();
  usleep(500 * 1000);       // 为了让另一个线程等待在sleep上
  // Main线程加锁顺序：Inventory's mutex -> Request's mutex
  g_inventory.printAll();
  thread.join();
}

/*
 * 以上： main()线程先调用Inventory::printAll()获取Inventory's mutex，然后调用
 * Request::print()尝试获取Request's mutex；而
 * threadFunc()线程先调用Request::~Request()获取Request's mutex，然后调用
 * Inventory::remove()尝试获取Inventory's mutex，这两个调用序列加锁顺序正好相反，
 * 于是造成了经典的死锁
 *
 * 思考：为什么Request内部要加锁？
 */