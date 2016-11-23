// Copyright 2016 Yahoo Inc. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#pragma once

#include "array_store.h"
#include "datastore.hpp"
#include <atomic>

namespace search {
namespace datastore {

constexpr size_t MIN_BUFFER_CLUSTERS = 1024;

template <typename EntryT, typename RefT>
ArrayStore<EntryT, RefT>::LargeArrayType::LargeArrayType()
    : BufferType<LargeArray>(1, MIN_BUFFER_CLUSTERS, RefT::offsetSize())
{
}

template <typename EntryT, typename RefT>
void
ArrayStore<EntryT, RefT>::LargeArrayType::cleanHold(void *buffer, uint64_t offset, uint64_t len, CleanContext cleanCtx)
{
    LargeArray *elem = static_cast<LargeArray *>(buffer) + offset;
    for (size_t i = 0; i < len; ++i) {
        cleanCtx.extraBytesCleaned(sizeof(EntryT) * elem->size());
        *elem = _emptyEntry;
        ++elem;
    }
}

template <typename EntryT, typename RefT>
void
ArrayStore<EntryT, RefT>::initArrayTypes(size_t minClusters, size_t maxClusters)
{
    _largeArrayTypeId = _store.addType(&_largeArrayType);
    assert(_largeArrayTypeId == 0);
    for (uint32_t arraySize = 1; arraySize <= _maxSmallArraySize; ++arraySize) {
        _smallArrayTypes.push_back(std::make_unique<SmallArrayType>(arraySize, minClusters, maxClusters));
        uint32_t typeId = _store.addType(_smallArrayTypes.back().get());
        assert(typeId == arraySize); // Enforce 1-to-1 mapping between type ids and sizes for small arrays
    }
}

template <typename EntryT, typename RefT>
ArrayStore<EntryT, RefT>::ArrayStore(uint32_t maxSmallArraySize)
    : ArrayStore<EntryT,RefT>(maxSmallArraySize, MIN_BUFFER_CLUSTERS, RefT::offsetSize())
{
}

template <typename EntryT, typename RefT>
ArrayStore<EntryT, RefT>::ArrayStore(uint32_t maxSmallArraySize, size_t minClusters, size_t maxClusters)
    : _store(),
      _maxSmallArraySize(maxSmallArraySize),
      _smallArrayTypes(),
      _largeArrayType(),
      _largeArrayTypeId()
{
    maxClusters = std::min(maxClusters, RefT::offsetSize());
    minClusters = std::min(minClusters, maxClusters);
    initArrayTypes(minClusters, maxClusters);
    _store.initActiveBuffers();
}

template <typename EntryT, typename RefT>
ArrayStore<EntryT, RefT>::~ArrayStore()
{
    _store.clearHoldLists();
    _store.dropBuffers();
}

template <typename EntryT, typename RefT>
EntryRef
ArrayStore<EntryT, RefT>::add(const ConstArrayRef &array)
{
    if (array.size() == 0) {
        return EntryRef();
    }
    if (array.size() <= _maxSmallArraySize) {
        return addSmallArray(array);
    } else {
        return addLargeArray(array);
    }
}

template <typename EntryT, typename RefT>
EntryRef
ArrayStore<EntryT, RefT>::addSmallArray(const ConstArrayRef &array)
{
    uint32_t typeId = getTypeId(array.size());
    _store.ensureBufferCapacity(typeId, array.size());
    uint32_t activeBufferId = _store.getActiveBufferId(typeId);
    BufferState &state = _store.getBufferState(activeBufferId);
    assert(state.isActive());
    size_t oldBufferSize = state.size();
    EntryT *buf = _store.template getBufferEntry<EntryT>(activeBufferId, oldBufferSize);
    for (size_t i = 0; i < array.size(); ++i) {
        new (static_cast<void *>(buf + i)) EntryT(array[i]);
    }
    state.pushed_back(array.size());
    return RefT((oldBufferSize / array.size()), activeBufferId);
}

template <typename EntryT, typename RefT>
EntryRef
ArrayStore<EntryT, RefT>::addLargeArray(const ConstArrayRef &array)
{
    _store.ensureBufferCapacity(_largeArrayTypeId, 1);
    uint32_t activeBufferId = _store.getActiveBufferId(_largeArrayTypeId);
    BufferState &state = _store.getBufferState(activeBufferId);
    assert(state.isActive());
    size_t oldBufferSize = state.size();
    LargeArray *buf = _store.template getBufferEntry<LargeArray>(activeBufferId, oldBufferSize);
    new (static_cast<void *>(buf)) LargeArray(array.cbegin(), array.cend());
    state.pushed_back(1, sizeof(EntryT) * array.size());
    return RefT(oldBufferSize, activeBufferId);
}

template <typename EntryT, typename RefT>
typename ArrayStore<EntryT, RefT>::ConstArrayRef
ArrayStore<EntryT, RefT>::get(EntryRef ref) const
{
    if (!ref.valid()) {
        return ConstArrayRef();
    }
    RefT internalRef(ref);
    uint32_t typeId = _store.getTypeId(internalRef.bufferId());
    if (typeId != _largeArrayTypeId) {
        size_t arraySize = getArraySize(typeId);
        return getSmallArray(internalRef, arraySize);
    } else {
        return getLargeArray(internalRef);
    }
}

template <typename EntryT, typename RefT>
typename ArrayStore<EntryT, RefT>::ConstArrayRef
ArrayStore<EntryT, RefT>::getSmallArray(RefT ref, size_t arraySize) const
{
    size_t bufferOffset = ref.offset() * arraySize;
    const EntryT *buf = _store.template getBufferEntry<EntryT>(ref.bufferId(), bufferOffset);
    return ConstArrayRef(buf, arraySize);
}

template <typename EntryT, typename RefT>
typename ArrayStore<EntryT, RefT>::ConstArrayRef
ArrayStore<EntryT, RefT>::getLargeArray(RefT ref) const
{
    const LargeArray *buf = _store.template getBufferEntry<LargeArray>(ref.bufferId(), ref.offset());
    assert(buf->size() > 0);
    return ConstArrayRef(&(*buf)[0], buf->size());
}

template <typename EntryT, typename RefT>
void
ArrayStore<EntryT, RefT>::remove(EntryRef ref)
{
    if (ref.valid()) {
        RefT internalRef(ref);
        uint32_t typeId = _store.getTypeId(internalRef.bufferId());
        if (typeId != _largeArrayTypeId) {
            size_t arraySize = getArraySize(typeId);
            _store.holdElem(ref, arraySize);
        } else {
            _store.holdElem(ref, 1, sizeof(EntryT) * get(ref).size());
        }
    }
}

namespace arraystore {

template <typename EntryT, typename RefT>
class CompactionContext : public ICompactionContext {
private:
    using ArrayStoreType = ArrayStore<EntryT, RefT>;
    DataStoreBase &_dataStore;
    ArrayStoreType &_store;
    uint32_t _bufferIdToCompact;

public:
    CompactionContext(DataStoreBase &dataStore,
                      ArrayStoreType &store,
                      uint32_t bufferIdToCompact)
        : _dataStore(dataStore),
          _store(store),
          _bufferIdToCompact(bufferIdToCompact)
    {}
    virtual ~CompactionContext() {
        _dataStore.holdBuffer(_bufferIdToCompact);
    }
    virtual void compact(vespalib::ArrayRef<EntryRef> refs) override {
        for (auto &ref : refs) {
            if (ref.valid()) {
                RefT internalRef(ref);
                if (internalRef.bufferId() == _bufferIdToCompact) {
                    EntryRef newRef = _store.add(_store.get(ref));
                    std::atomic_thread_fence(std::memory_order_release);
                    ref = newRef;
                }
            }
        }
    }
};

}

template <typename EntryT, typename RefT>
ICompactionContext::UP
ArrayStore<EntryT, RefT>::compactWorst()
{
    uint32_t bufferIdToCompact = _store.startCompactWorstBuffer();
    return std::make_unique<arraystore::CompactionContext<EntryT, RefT>>
            (_store, *this, bufferIdToCompact);
}

template <typename EntryT, typename RefT>
AddressSpace
ArrayStore<EntryT, RefT>::addressSpaceUsage() const
{
    uint32_t numPossibleBuffers = RefT::numBuffers();
    assert(_store.getNumActiveBuffers() <= numPossibleBuffers);
    return AddressSpace(_store.getNumActiveBuffers(), numPossibleBuffers);
}

template <typename EntryT, typename RefT>
const BufferState &
ArrayStore<EntryT, RefT>::bufferState(EntryRef ref) const
{
    RefT internalRef(ref);
    return _store.getBufferState(internalRef.bufferId());
}

}
}