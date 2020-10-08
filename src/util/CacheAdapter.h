//
// Created by johannes on 01.10.20.
//

#ifndef QLEVER_CACHEADAPTER_H
#define QLEVER_CACHEADAPTER_H
#include <utility>
#include <memory>
#include "./Synchronized.h"
#include "../util/HashMap.h"

namespace ad_utility {

using std::shared_ptr;
using std::make_shared;

template <typename Key, typename Value, typename OnFinishedAction>
struct TryEmplaceResult {
  TryEmplaceResult(Key key, shared_ptr<Value> todo, shared_ptr<const Value> done, OnFinishedAction action):
 _val{std::move(todo), std::move(done)}, _key{std::move(key)}, _onFinishedAction{action} {}
  std::pair<shared_ptr<Value>, shared_ptr<const Value>> _val;
  void finish() {
    if (_val.first) {
      (_onFinishedAction)(_key, _val.first);
    }
  }

 private:
  Key _key;
  OnFinishedAction _onFinishedAction;

};

template <typename Cache, typename OnFinishedAction>
class CacheAdapter {
 public:
  using Value = typename Cache::value_type;
  using Key = typename Cache::key_type;

  struct S {
    Cache _cache;
    // values that are currently being computed.
    // the bool tells us whether this result will be pinned in the cache
    HashMap<Key, std::pair<bool, shared_ptr<Value>>> _inProgress;
    template<typename... Args>
    S(Args&&... args) : _cache{std::forward<Args>(args)...} {}
  };
  using SyncCache = ad_utility::Synchronized<S, std::mutex>;

  template <typename... CacheArgs>
  CacheAdapter(OnFinishedAction action, CacheArgs&&... cacheArgs ):
    _v{std::forward<CacheArgs>(cacheArgs)...}, _onFinishedAction{action} {}
 private:
  class Action {
   public:
    Action(SyncCache* cPtr, OnFinishedAction* action):
    _cachePtr{cPtr}, _singleAction{action}{}

    void operator()(Key k, shared_ptr<Value> vPtr) {
      auto l = _cachePtr->wlock();  // lock for the whole time, so no one gets confused.
      (*_singleAction)(*vPtr);
      auto& pinned = l->_inProgress[k].first;
      if (pinned) {
        // TODO: implement pinned insert
        throw std::runtime_error("pinnedInsert is not implemented, TODO");
        //l->_cache.insertPinned(std::move(k), std::move(vPtr));
      } else {
        l->_cache.insert(std::move(k), std::move(vPtr));
      }
      l->_inProgress.erase(k);
    }
   private:
    SyncCache * _cachePtr;
    OnFinishedAction* _singleAction;
  };

 public:
  using Res = TryEmplaceResult<Key, Value, Action>;

  template <class... Args>
  Res tryEmplace(const Key& key, Args&&... args) {
    return tryEmplaceImpl(false, key, std::forward<Args>(args)...);
  }

  auto& getStorage() { return _v;}
 private:
  SyncCache _v;
  OnFinishedAction _onFinishedAction;


  Action makeAction() {
    return Action(&_v, &_onFinishedAction);
  }

  // ______________________________________________________________________________
  template <typename... Args>
  Res tryEmplaceImpl(bool pinned, const Key& key, Args&&... args) {
    return _v.withWriteLock([&](auto& s) {
      bool contained = pinned ? s._cache.containsPinnedIncludingUpgrade(key) : s._cache.contains(key);
      if (contained) {
        return Res{key, shared_ptr<Value>{}, static_cast<shared_ptr<const Value>>(s._cache[key]), makeAction()};
      } else if (s._inProgress.contains(key)) {
        s._inProgress[key].first = s._inProgress[key].first || pinned;
        return Res{key, shared_ptr<Value>{}, s._inProgress[key].second, makeAction()};
      }
      auto empl = make_shared<Value>(std::forward<Args>(args)...);

      s._inProgress[key] = {pinned, empl};
      return Res{key, empl, empl, makeAction()};
    });
  }
};
}



#endif  // QLEVER_CACHEADAPTER_H
