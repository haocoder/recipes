#include "../Mutex.h"

class Request
{
 public:
  void process() // __attribute__ ((noinline))
  {
    muduo::MutexLockGuard lock(mutex_);
    print();            // 重复加锁，导致死锁
  }

  void print() const // __attribute__ ((noinline))
  {
    muduo::MutexLockGuard lock(mutex_);
  }

 private:
  mutable muduo::MutexLock mutex_;
};

int main()
{
  Request req;
  req.process();
}
