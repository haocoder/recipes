#include <map>
#include <string>
#include <vector>

#include <boost/shared_ptr.hpp>

#include "../Mutex.h"

using std::string;

// 通过shared_ptr实现copy-on-write进行多线程并发读写，缺点是需要拷贝数据
// 但如果大多数情况下是在原数据上进行读操作，拷贝的比例很低，还是很高效地
class CustomerData : boost::noncopyable
{
 public:
  CustomerData()
    : data_(new Map)
  { }

  int query(const string& customer, const string& stock) const;

 private:
  typedef std::pair<string, int> Entry;
  typedef std::vector<Entry> EntryList;
  typedef std::map<string, EntryList> Map;
  typedef boost::shared_ptr<Map> MapPtr;
  void update(const string& customer, const EntryList& entries);
  void update(const string& message);

  static int findEntry(const EntryList& entries, const string& stock);
  static MapPtr parseData(const string& message);

  MapPtr getData() const
  {
    muduo::MutexLockGuard lock(mutex_);
    return data_;
  }

  mutable muduo::MutexLock mutex_;
  MapPtr data_;
};

int CustomerData::query(const string& customer, const string& stock) const
{
  MapPtr data = getData(); // 用栈变量保存data_的一份拷贝，防止并发修改
  // 此时不需要加锁了，可以直接读
  Map::const_iterator entries = data->find(customer);
  if (entries != data->end())
    return findEntry(entries->second, stock);
  else
    return -1;
}

void CustomerData::update(const string& customer, const EntryList& entries)
{
  muduo::MutexLockGuard lock(mutex_);   // 写之前加锁
  if (!data_.unique())   // 有其它线程在读
  {
    MapPtr newData(new Map(*data_));  // 不能再原数据上修改，得创建一个副本，在副本上修改后替换原数据
    data_.swap(newData);              // data_指向副本，也即新的数据，此时只有栈上的shared_ptr指向原始数据，一旦栈对象销毁，原始数据也就销毁了
  }
  assert(data_.unique());                // 此时还在临界区，不会有其它线程来读取
  (*data_)[customer] = entries;
}

void CustomerData::update(const string& message)
{
  // 解析新数据，在临界区之外
  MapPtr newData = parseData(message);
  if (newData)
  {
    muduo::MutexLockGuard lock(mutex_);
    // 不要使用data_ = newData，因为如果data_的引用计数为1，这会导致旧数据的析构，从而加长了临界区
    data_.swap(newData);
  }
  // 旧数据的析构在临界区之外，进一步缩短了临界区
}

int main()
{
  CustomerData data;
}
