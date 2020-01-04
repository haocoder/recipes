#include "../Mutex.h"
#include "../Thread.h"
#include <vector>
#include <boost/shared_ptr.hpp>
#include <stdio.h>

using namespace muduo;

class Foo
{
 public:
  void doit() const;
};

typedef std::vector<Foo> FooList;
typedef boost::shared_ptr<FooList> FooListPtr;
FooListPtr g_foos;
MutexLock mutex;

void post(const Foo& f)
{
  printf("post\n");
  MutexLockGuard lock(mutex);
  if (!g_foos.unique())  // 有其它线程正在读取FooList，不能原地修改
  {
      // 复制一份FooList，g_foos指向这个副本，并在它上面做修改
      // 避免死锁
    g_foos.reset(new FooList(*g_foos));
    printf("copy the whole list\n");  // copy操作
  }
  assert(g_foos.unique());
  // write操作
  g_foos->push_back(f);   // 只有一个线程访问g_foos，原地(in-place)修改FooList
  // 即如果多线程访问FooList时，想要修改FooList，必须先copy一份，然后在这个副本上做write操作
  // 而对于源FooList由栈上的Shared_ptr管理，一旦这个栈对象销毁，源FooList也会销毁
}

void traverse()
{
  FooListPtr foos;
  {  // 临界区，保护多线程并发读写共享shared_ptr g_foos
    MutexLockGuard lock(mutex);
    foos = g_foos;   // 导致g_foos的引用计数器增加1
    assert(!g_foos.unique());
  }

  // assert(!foos.unique()); this may not hold
  // 开始遍历
  for (std::vector<Foo>::const_iterator it = foos->begin();
      it != foos->end(); ++it)
  {
    it->doit();
  }
}

void Foo::doit() const
{
  Foo f;
  post(f);
}

int main()
{
  g_foos.reset(new FooList);
  Foo f;
  post(f);
  traverse();
}

