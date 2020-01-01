#include <map>

#include <boost/bind.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/weak_ptr.hpp>

#include "../Mutex.h"

#include <assert.h>
#include <stdio.h>

using std::string;

class Stock : boost::noncopyable
{
 public:
  Stock(const string& name)
    : name_(name)
  {
    printf(" Stock[%p] %s\n", this, name_.c_str());
  }

  ~Stock()
  {
    printf("~Stock[%p] %s\n", this, name_.c_str());
  }

  const string& key() const { return name_; }

 private:
  string name_;
};

namespace version1
{

// questionable code
class StockFactory : boost::noncopyable
{
 public:

  boost::shared_ptr<Stock> get(const string& key)
  {
    muduo::MutexLockGuard lock(mutex_);
    boost::shared_ptr<Stock>& pStock = stocks_[key];
    if (!pStock)
    {
      pStock.reset(new Stock(key));
    }
    return pStock;
  }


 private:
  mutable muduo::MutexLock mutex_;
  // 存在的问题：当没有用户使用Stock时，它应该被释放，
  // 但是这里Stock对象永远不会被销毁，因为map里存的是shared_ptr，
  // 除非StockFactory销毁
  std::map<string, boost::shared_ptr<Stock> > stocks_;   // 对象池
};

}

namespace version2
{
// 解决version1的问题：用weak_ptr代替shared_ptr
class StockFactory : boost::noncopyable
{
 public:
  boost::shared_ptr<Stock> get(const string& key)
  {
    boost::shared_ptr<Stock> pStock;
    muduo::MutexLockGuard lock(mutex_);
    boost::weak_ptr<Stock>& wkStock = stocks_[key];  // 如果不存在，会构造一个weak_ptr
    pStock = wkStock.lock();            // 尝试把weak_ptr提升为shared_ptr,如果weak_ptr指向有效Stock，提升会成功
    if (!pStock)
    {
      pStock.reset(new Stock(key));
      wkStock = pStock;         // 更新了stocks[key],因为wkStock是个引用
    }
    return pStock;
  }

 private:
  mutable muduo::MutexLock mutex_;
  // 用了weak_ptr代替shared_ptr，stocks_中的weak_ptr不会增加stock的引用计数，
  // 解决了当没有用户使用Stock时，Stock自动销毁的问题，但是引入了新问题：轻微的内存泄漏,
  // 因为即使stocks_中weak_ptr指向的Stock被销毁了，但是weak_ptr一直存在，没有
  // 被销毁，stocks_大小只增不减
  std::map<string, boost::weak_ptr<Stock> > stocks_;
};

}

namespace version3
{
// 解决version2遇到的stocks_大小只增不减的问题，利用shared_ptr的定制析构功能：
// 即当指向对象的最后一个shared_ptr销毁时调用定制的析构函数释放对象资源（我个人理解的说法）
class StockFactory : boost::noncopyable
{
 public:

  boost::shared_ptr<Stock> get(const string& key)
  {
    boost::shared_ptr<Stock> pStock;
    muduo::MutexLockGuard lock(mutex_);
    boost::weak_ptr<Stock>& wkStock = stocks_[key];
    pStock = wkStock.lock();
    if (!pStock)
    {
      // 定制析构函数销毁Stock
      // 引入的新问题，这里把StockFactory this指针保存在了boost::function中，
      // 如果StockFactory先于Stock对象析构，那么Stock析构时去回调StockFactory
      // 的成员函数，就会coredump
      // 解决的思路是：确保StockFactory的生命周期比Stock长，即使用一个指向当前
      // StockFactory的shared_ptr来管理StockFactory对象生命周期
      pStock.reset(new Stock(key),
                   boost::bind(&StockFactory::deleteStock, this, _1));
      wkStock = pStock;
    }
    return pStock;
  }

 private:
  //指向Stock的最后一个shared_ptr销毁时析构Stock时调用该函数
  void deleteStock(Stock* stock)
  {
    printf("deleteStock[%p]\n", stock);
    if (stock)
    {
      muduo::MutexLockGuard lock(mutex_);
      // 删除stocks_中的weak_ptr
      stocks_.erase(stock->key());  // This is wrong, see removeStock below for correct implementation.
    }
    // 释放Stock
    delete stock;  // sorry, I lied
  }
  mutable muduo::MutexLock mutex_;
  std::map<string, boost::weak_ptr<Stock> > stocks_;
};

}

namespace version4
{
// 解决version3存在的问题； 使用enable_shared_from_this，这是一个以其派生类为模板类型
// 实参的基类模板，继承它，this指针就能变身为shared_ptr.
// 这里StockFactory继承自boost::enable_shared_from_this，StockFactory再不能作为
// stack object使用了，必须是heap object且由shared_ptr管理其生命周期，即：
// shared_ptr<StockFactory>  stockFactory(new StockFactory);
class StockFactory : public boost::enable_shared_from_this<StockFactory>,
                     boost::noncopyable
{
 public:

  boost::shared_ptr<Stock> get(const string& key)
  {
    boost::shared_ptr<Stock> pStock;
    muduo::MutexLockGuard lock(mutex_);
    boost::weak_ptr<Stock>& wkStock = stocks_[key];
    pStock = wkStock.lock();
    if (!pStock)
    {
      // 调用shared_from_this()，使得this(StackFactory对象)变成shared_ptr<StockFactory>,
      // boost::function里保存了一份shared_ptr<StackFactory>,可以确保调用StackFactory::deleteStock
      // 时那个StackFactory对象还活着
      // 引入的新问题：StackFactory的生命周期被意外延长了，当所有Stock中最后一个销毁时，StackFactory才会被销毁
      pStock.reset(new Stock(key),
                   boost::bind(&StockFactory::deleteStock,
                               shared_from_this(),
                               _1));
      wkStock = pStock;
    }
    return pStock;
  }

 private:

  void deleteStock(Stock* stock)
  {
    printf("deleteStock[%p]\n", stock);
    if (stock)
    {
      muduo::MutexLockGuard lock(mutex_);
      stocks_.erase(stock->key());  // This is wrong, see removeStock below for correct implementation.
    }
    delete stock;  // sorry, I lied
  }
  mutable muduo::MutexLock mutex_;
  std::map<string, boost::weak_ptr<Stock> > stocks_;
};

}

// 使用"弱回调"技术解决StackFactory对象生命周期被延长的问题，即如果对象还活着，
// 就调用它的成员函数，否则忽略之
class StockFactory : public boost::enable_shared_from_this<StockFactory>,
                     boost::noncopyable
{
 public:
  boost::shared_ptr<Stock> get(const string& key)
  {
    boost::shared_ptr<Stock> pStock;
    muduo::MutexLockGuard lock(mutex_);
    boost::weak_ptr<Stock>& wkStock = stocks_[key];
    pStock = wkStock.lock();
    if (!pStock)
    {
      // 使用weak_ptr,把weak_ptr绑到boost::function里，这样对象的生命周期就不会被延长了
      // 然后在回调的时候先尝试把weak_ptr提升为shared_ptr，如果提升成功，说明接受回调的
      // 对象还存在，就执行回调函数；如果提升失败，就不回调
      pStock.reset(new Stock(key),
                   boost::bind(&StockFactory::weakDeleteCallback,
                               boost::weak_ptr<StockFactory>(shared_from_this()),
                               _1));
      // 上面必须强制把shared_from_this()转型为weak_ptr，才不会延长生命周期，因为boost::bind
      // 拷贝的是实参类型，不是形参类型
      wkStock = pStock;
    }
    return pStock;
  }

 private:
  static void weakDeleteCallback(const boost::weak_ptr<StockFactory>& wkFactory,
                                 Stock* stock)
  {
    printf("weakDeleteStock[%p]\n", stock);
    boost::shared_ptr<StockFactory> factory(wkFactory.lock());  // 尝试将weak_ptr提升为shared_ptr
    if (factory)
    {
      factory->removeStock(stock);
    }
    else
    {
      printf("factory died.\n");
    }
    delete stock;  // sorry, I lied
  }

  void removeStock(Stock* stock)
  {
    if (stock)
    {
      muduo::MutexLockGuard lock(mutex_);
      auto it = stocks_.find(stock->key());
      assert(it != stocks_.end());
      if (it->second.expired())
      {
        stocks_.erase(stock->key());
      }
    }
  }

 private:
  mutable muduo::MutexLock mutex_;
  std::map<string, boost::weak_ptr<Stock> > stocks_;
};

void testLongLifeFactory()
{
  boost::shared_ptr<StockFactory> factory(new StockFactory);
  {
    boost::shared_ptr<Stock> stock = factory->get("NYSE:IBM");
    boost::shared_ptr<Stock> stock2 = factory->get("NYSE:IBM");
    assert(stock == stock2);
    // stock destructs here
  }
  // factory destructs here
}

void testShortLifeFactory()
{
  boost::shared_ptr<Stock> stock;
  {
    boost::shared_ptr<StockFactory> factory(new StockFactory);
    stock = factory->get("NYSE:IBM");
    boost::shared_ptr<Stock> stock2 = factory->get("NYSE:IBM");
    assert(stock == stock2);
    // factory destructs here
  }
  // stock destructs here
}

int main()
{
  version1::StockFactory sf1;
  version2::StockFactory sf2;
  version3::StockFactory sf3;
  boost::shared_ptr<version3::StockFactory> sf4(new version3::StockFactory);
  boost::shared_ptr<StockFactory> sf5(new StockFactory);

  {
  boost::shared_ptr<Stock> s1 = sf1.get("stock1");
  }

  {
  boost::shared_ptr<Stock> s2 = sf2.get("stock2");
  }

  {
  boost::shared_ptr<Stock> s3 = sf3.get("stock3");
  }

  {
  boost::shared_ptr<Stock> s4 = sf4->get("stock4");
  }

  {
  boost::shared_ptr<Stock> s5 = sf5->get("stock5");
  }

  testLongLifeFactory();
  testShortLifeFactory();
}
