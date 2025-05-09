// Copyright 2018, University of Freiburg,
// Chair of Algorithms and Data Structures.
// Author: Johannes Kalmbach (joka921) <johannes.kalmbach@gmail.com>

#ifndef QLEVER_SRC_UTIL_MMAPVECTOR_H
#define QLEVER_SRC_UTIL_MMAPVECTOR_H

#include <concepts>
#include <exception>
#include <sstream>
#include <string>

#include "util/Exception.h"
#include "util/ExceptionHandling.h"
#include "util/File.h"
#include "util/Forward.h"
#include "util/Iterators.h"
#include "util/ResetWhenMoved.h"

namespace ad_utility {
// _________________________________________________________________________
class UninitializedArrayException : public std::exception {
  const char* what() const noexcept override {
    return "Tried to access a DiskBasedArray which was closed or "
           "uninitialized\n";
  }
};

// _________________________________________________________________________
class InvalidFileException : public std::exception {
  const char* what() const noexcept override {
    return "Error reading meta data of  Mmap file: Maybe magic number is "
           "missing or there is a version mismatch\n";
  }
};

// _________________________________________________________________________
class TruncateException : public std::exception {
 public:
  // _______________________________________________________________________
  TruncateException(const std::string& file, size_t size, int err) {
    std::stringstream stream;
    stream << "truncating of file " << file << " to size " << size
           << "set errno to" << err << " terminating\n";
    _msg = std::move(stream).str();
  }

  // ______________________________________________________________________________________________________
  const char* what() const noexcept override { return _msg.c_str(); }

  std::string _msg;
};

// ________________________________________________________________
struct VecInfo {
  size_t _capacity;
  size_t _bytesize;
};

// 2 tags to differentiate between different versions of the
// setup / construction variants of Mmap Vector which only take a filename
class CreateTag {};
class ReuseTag {};

// Enum that specifies access patterns to this array
enum class AccessPattern { None, Random, Sequential };

// STL-like class which implements a dynamic array (similar to std::vector)
// whose contents are stored persistently in a file on memory and are accessed
// using memory mapping
template <class T>
class MmapVector {
 public:
  using iterator = ad_utility::IteratorForAccessOperator<
      MmapVector, AccessViaBracketOperator, IsConst::False>;
  using const_iterator = ad_utility::IteratorForAccessOperator<
      MmapVector, AccessViaBracketOperator, IsConst::True>;
  using value_type = T;

  // __________________________________________________________________
  size_t size() const { return _size; }

  // __________________________________________________________________
  size_t capacity() const { return _capacity; }

  // standard iterator functions, each in a const and non-const version
  // begin
  iterator begin() { return {this, 0}; }
  T* data() { return _ptr; }
  const_iterator begin() const { return {this, 0}; }
  const T* data() const { return _ptr; }
  // end
  iterator end() { return {this, _size}; }
  const_iterator end() const { return {this, _size}; }
  // cbegin and cend
  const_iterator cbegin() const { return {this, 0}; }
  const_iterator cend() const { return {this, _size}; }

  // Element access
  // without bounds checking
  T& operator[](size_t idx) {
    throwIfUninitialized();
    return _ptr[idx];
  }
  const T& operator[](size_t idx) const {
    throwIfUninitialized();
    return *(_ptr + idx);
  }

  // with bounds checking
  T& at(size_t idx) {
    throwIfUninitialized();
    if (idx >= _size) {
      throw std::out_of_range("call to MmapVector::at with idx >= size");
    }
    return *(_ptr + idx);
  }

  // _____________________________________________________________
  const T& at(size_t idx) const {
    throwIfUninitialized();
    if (idx >= _size) {
      throw std::out_of_range("call to MmapVector::at with idx >= size");
    }
    return *(_ptr + idx);
    AD_CONTRACT_CHECK(idx < _size);
  }

  // ____________________________________________________________________________
  T& back() { return *(_ptr + _size - 1); }
  const T& back() const { return *(_ptr + _size - 1); }

  // _____________________________________________________________
  std::string getFilename() const { return _filename; }

  // we will never have less than this capacity
  static constexpr size_t MinCapacity = 100;

  // Default constructor. This array is invalid until initialized by one of the
  // setup methods;
  MmapVector() = default;
  virtual ~MmapVector();

  // no copy construction/assignment since we exclusively handle our mapping
  MmapVector(const MmapVector<T>&) = delete;
  MmapVector& operator=(const MmapVector<T>&) = delete;

  // move construction and assignment
  MmapVector(MmapVector<T>&& other) noexcept;
  MmapVector& operator=(MmapVector<T>&& other) noexcept;

  CPP_template(class Arg, typename... Args)(requires CPP_NOT(
      std::derived_from<std::remove_cvref_t<Arg>, MmapVector<T>>))
      MmapVector(Arg&& arg, Args&&... args)
      : MmapVector<T>() {
    this->open(AD_FWD(arg), AD_FWD(args)...);
  }

  // create Array of given size  fill with default value
  // file contents will be overridden if existing!
  // allows read and write access
  void open(size_t size, const T& defaultValue, std::string filename,
            AccessPattern pattern = AccessPattern::None);

  // create uninitialized array of given size at path specified by filename
  // file will be created or overridden!
  // allows read and write access
  void open(size_t size, std::string filename,
            AccessPattern pattern = AccessPattern::None);

  // create from given Iterator range
  // It must be an iterator type whose value Type must be convertible to T
  // TODO<joka921>: use enable_if or constexpr if or concepts/ranges one they're
  // out
  template <class It>
  void open(It begin, It end, const std::string& filename,
            AccessPattern pattern = AccessPattern::None);

  // _____________________________________________________________________
  void open(std::string filename, CreateTag,
            AccessPattern pattern = AccessPattern::None) {
    open(0, std::move(filename), pattern);
  }

  // open previously created array.
  // File at path filename must be a valid file created previously by this class
  // else the behavior is undefined
  void open(std::string filename, ReuseTag,
            AccessPattern pattern = AccessPattern::None);

  // Close the vector, saves all buffered data to the mapped file and closes it.
  // Vector
  void close();

  // change size of array.
  // if new size < old size elements and the end will be deleted
  // if new size > old size new elements will be uninitialized
  // iterators are possibly invalidated
  void resize(size_t newSize);
  void clear() { resize(0); }

  // make sure that this vector has capacity for at least newCapacity elements
  // before having to allocate again. If n <= the current capacity,
  // this has no effect
  void reserve(size_t newCapacity) {
    if (newCapacity > _capacity) {
      adaptCapacity(newCapacity);
    }
  }

  // add element specified by arg el at the end of the array
  // possibly invalidates iterators
  void push_back(T&& el);
  void push_back(const T& el);

  // set a different access pattern if the use case  of  this  vector has
  // changed
  void setAccessPattern(AccessPattern p) {
    _pattern = p;
    advise(_pattern);
  }

 protected:
  // _________________________________________________________________________
  inline void throwIfUninitialized() const {
    if (_ptr == nullptr) {
      throw UninitializedArrayException();
    }
  }

  // truncate the underlying file to _bytesize and write meta data (_size,
  // _capacity, etc) for this
  // array to the end. new size of file will be _bytesize + sizeof(meta data)
  void writeMetaDataToEnd();

  // read meta data (_size, _capacity, _bytesize) etc. from the end of the
  // underlying file
  void readMetaDataFromEnd();

  // map the underlying file to memory in a read-only way.
  // Write accesses will be undefined behavior (mostly segfaults because of mmap
  // specifications, but we can enforce thread-safety etc.
  void mapForReading();

  // map the underlying file to memory in a read-write way.
  // reading and writing has to be synchronized
  void mapForWriting();

  // remap the file after a change of _bytesize
  // only supported on linux operating systems which support the mremap syscall.
  void remapLinux(size_t oldBytesize);

  // mmap can handle only multiples of the file system's page size
  // convert a size ( number of elements) to the smallest multiple of page size
  // that is >= than targetSize * sizeof(T)
  VecInfo convertArraySizeToFileSize(size_t targetSize) const;

  // make sure this vector has place for at least newCapacity elements before
  // having to "allocate" in the underlying file again.
  // Can also be used to shrink the file;
  // possibly invalidates iterators
  void adaptCapacity(size_t newCapacity);

  // wrapper to munmap, release the contents of the file.
  // invalidates iterators
  void unmap();

  // advise the kernel to use a certain access pattern
  void advise(AccessPattern pattern);

  ResetWhenMoved<T*, nullptr> _ptr;
  ResetWhenMoved<size_t, 0> _size;
  ResetWhenMoved<size_t, 0> _capacity;
  ResetWhenMoved<size_t, 0> _bytesize;
  std::string _filename = "";
  AccessPattern _pattern = AccessPattern::None;
  static constexpr float ResizeFactor = 1.5;
  static constexpr uint32_t MagicNumber = 7601577;
  static constexpr uint32_t Version = 0;
};

// MmapVector variation that only supports read-access to a previously created
// MmapVector-file.
template <class T>
class MmapVectorView : private MmapVector<T> {
 public:
  using value_type = T;
  using const_iterator = typename MmapVector<T>::const_iterator;
  using iterator = typename MmapVector<T>::iterator;
  // const access and iteration methods, directly map to the MmapVector-Variants
  const_iterator begin() const { return MmapVector<T>::begin(); }
  const T* data() const { return MmapVector<T>::data(); }
  const_iterator end() const { return MmapVector<T>::end(); }
  const_iterator cbegin() const { return MmapVector<T>::cbegin(); }
  const_iterator cend() const { return MmapVector<T>::cend(); }
  const T& operator[](size_t idx) const {
    return MmapVector<T>::operator[](idx);
  }
  const T& at(size_t idx) const { return MmapVector<T>::at(idx); }

  // ____________________________________________________
  size_t size() const { return MmapVector<T>::size(); }

  // default constructor, leaves an uninitialized vector that will throw until a
  // valid call to open()
  MmapVectorView() = default;

  // construct with any combination of arguments that is supported by the open()
  // member function
  CPP_template(typename Arg, typename... Args)(requires CPP_NOT(
      std::same_as<std::remove_cvref_t<Arg>,
                   MmapVectorView>)) explicit MmapVectorView(Arg&& arg,
                                                             Args&&... args) {
    open(AD_FWD(arg), AD_FWD(args)...);
  }

  // Move construction and assignment are allowed, but not copying
  MmapVectorView(MmapVectorView&& other) noexcept;
  MmapVectorView& operator=(MmapVectorView&& other) noexcept;

  // no copy construction/assignment. It would be possible to create a shared
  // mapping or to map the same file twice when copying. But the preferred
  // method should be to share one MmapVectorView by a (shared or non-owning)
  // pointer
  MmapVectorView(const MmapVectorView<T>&) = delete;
  MmapVectorView& operator=(const MmapVectorView<T>&) = delete;

  void open(std::string filename, AccessPattern pattern = AccessPattern::None) {
    this->unmap();
    this->_filename = std::move(filename);
    this->_pattern = pattern;
    this->readMetaDataFromEnd();
    this->mapForReading();
    this->advise(this->_pattern);
  }

  void open(const std::string& filename, ReuseTag,
            AccessPattern pattern = AccessPattern::None) {
    open(filename, pattern);
  }

  // explicitly close the vector to an uninitialized state and free the
  // associated resources
  void close();

  // destructor
  ~MmapVectorView() override { close(); }

  // _____________________________________________________________
  std::string getFilename() const { return this->_filename; }
};

// MmapVector that deletes the underlying file on destruction.
// This is the only difference to the ordinary MmapVector(which is persistent)
template <class T>
class MmapVectorTmp : public MmapVector<T> {
 public:
  void open(std::string filename) {
    MmapVector<T>::open(std::move(filename), CreateTag());
  }

  MmapVectorTmp<T>& operator=(MmapVectorTmp<T>&& rhs) noexcept {
    MmapVector<T>::operator=(std::move(rhs));
    return *this;
  }

  MmapVectorTmp& operator=(const MmapVectorTmp<T>&) = delete;

  MmapVectorTmp(MmapVectorTmp<T>&& rhs) noexcept
      : MmapVector<T>(std::move(rhs)) {}
  MmapVectorTmp(const MmapVectorTmp<T>& rhs) = delete;

  CPP_template(class Arg, typename... Args)(requires CPP_NOT(
      std::derived_from<std::remove_cvref_t<Arg>,
                        MmapVectorTmp>)) explicit MmapVectorTmp(Arg&& arg,
                                                                Args&&... args)
      : MmapVector<T>() {
    this->open(AD_FWD(arg), AD_FWD(args)...);
  }

  // If we still own a file, delete it after cleaning up
  // everything else
  ~MmapVectorTmp() override {
    // if the filename is not empty, we still own a file
    std::string oldFilename = this->_filename;
    std::string message = absl::StrCat(
        "Error while unmapping a file with name \"", oldFilename, "\"");
    ad_utility::terminateIfThrows([this]() { this->close(); }, message);
    if (!oldFilename.empty()) {
      ad_utility::deleteFile(oldFilename);
    }
  }
};

}  // namespace ad_utility
#include "./MmapVectorImpl.h"

#endif  // QLEVER_SRC_UTIL_MMAPVECTOR_H
