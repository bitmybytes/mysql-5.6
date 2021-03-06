/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

/*
 * This header defines FbsonDocument, FbsonKeyValue, and various value classes
 * which are derived from FbsonValue, and a forward iterator for container
 * values - essentially everything that is related to FBSON binary data
 * structures.
 *
 * Implementation notes:
 *
 * None of the classes in this header file can be instantiated directly (i.e.
 * you cannot create a FbsonKeyValue or FbsonValue object - all constructors
 * are declared non-public). We use the classes as wrappers on the packed FBSON
 * bytes (serialized), and cast the classes (types) to the underlying packed
 * byte array.
 *
 * For the same reason, we cannot define any FBSON value class to be virtual,
 * since we never call constructors, and will not instantiate vtbl and vptrs.
 *
 * Therefore, the classes are defined as packed structures (i.e. no data
 * alignment and padding), and the private member variables of the classes are
 * defined precisely in the same order as the FBSON spec. This ensures we
 * access the packed FBSON bytes correctly.
 *
 * The packed structures are highly optimized for in-place operations with low
 * overhead. The reads (and in-place writes) are performed directly on packed
 * bytes. There is no memory allocation at all at runtime.
 *
 * For updates/writes of values that will expand the original FBSON size, the
 * write will fail, and the caller needs to handle buffer increase.
 *
 * ** Iterator **
 * Both ObjectVal class and ArrayVal class have iterator type that you can use
 * to declare an iterator on a container object to go through the key-value
 * pairs or value list. The iterator has both non-const and const types.
 *
 * Note: iterators are forward direction only.
 *
 * ** Query **
 * Querying into containers is through the member functions find (for key/value
 * pairs) and get (for array elements), and is in streaming style. We don't
 * need to read/scan the whole FBSON packed bytes in order to return results.
 * Once the key/index is found, we will stop search.  You can use text to query
 * both objects and array (for array, text will be converted to integer index),
 * and use index to retrieve from array. Array index is 0-based.
 *
 * ** External dictionary **
 * During query processing, you can also pass a call-back function, so the
 * search will first try to check if the key string exists in the dictionary.
 * If so, search will be based on the id instead of the key string.
 *
 * @author Tian Xia <tianx@fb.com>
 */

#ifndef FBSON_FBSONDOCUMENT_H
#define FBSON_FBSONDOCUMENT_H

namespace fbson {

#pragma pack(push, 1)

#define FBSON_VER 1

// forward declaration
class FbsonValue;
class ObjectVal;

/*
 * FbsonDocument is the main object that accesses and queries FBSON packed
 * bytes. NOTE: FbsonDocument only allows object container as the top level
 * FBSON value. However, you can use the static method "createValue" to get any
 * FbsonValue object from the packed bytes.
 *
 * FbsonDocument object also dereferences to an object container value
 * (ObjectVal) once FBSON is loaded.
 *
 * ** Load **
 * FbsonDocument is usable after loading packed bytes (memory location) into
 * the object. We only need the header and first few bytes of the payload after
 * header to verify the FBSON.
 *
 * Note: creating an FbsonDocument (through createDocument) does not allocate
 * any memory. The document object is an efficient wrapper on the packed bytes
 * which is accessed directly.
 *
 * ** Query **
 * Query is through dereferencing into ObjectVal.
 */
class FbsonDocument {
 public:
  // create an FbsonDocument object from FBSON packed bytes
  static FbsonDocument *createDocument(const char *pb, uint32_t size);

  // create an FbsonValue from FBSON packed bytes
  static FbsonValue *createValue(const char *pb, uint32_t size);

  uint8_t version() { return header_.ver_; }

  FbsonValue *getValue() { return ((FbsonValue *)payload_); }

  ObjectVal *operator->() { return ((ObjectVal *)payload_); }

  const ObjectVal *operator->() const { return ((const ObjectVal *)payload_); }

 private:
  /*
   * FbsonHeader class defines FBSON header (internal to FbsonDocument).
   *
   * Currently it only contains version information (1-byte). We may expand the
   * header to include checksum of the FBSON binary for more security.
   */
  struct FbsonHeader {
    uint8_t ver_;
  } header_;

  char payload_[0];

  FbsonDocument();
};

/*
 * FbsonFwdIteratorT implements FBSON's iterator template.
 *
 * Note: it is an FORWARD iterator only due to the design of FBSON format.
 */
template <class Iter_Type, class Cont_Type>
class FbsonFwdIteratorT {
  typedef Iter_Type iterator;
  typedef typename std::iterator_traits<Iter_Type>::pointer pointer;
  typedef typename std::iterator_traits<Iter_Type>::reference reference;

 public:
  explicit FbsonFwdIteratorT(const iterator &i) : current_(i) {}

  // allow non-const to const iterator conversion (same container type)
  template <class Iter_Ty>
  FbsonFwdIteratorT(const FbsonFwdIteratorT<Iter_Ty, Cont_Type> &rhs)
      : current_(rhs.base()) {}

  bool operator==(const FbsonFwdIteratorT &rhs) const {
    return (current_ == rhs.current_);
  }

  bool operator!=(const FbsonFwdIteratorT &rhs) const {
    return !operator==(rhs);
  }

  bool operator<(const FbsonFwdIteratorT &rhs) const {
    return (current_ < rhs.current_);
  }

  bool operator>(const FbsonFwdIteratorT &rhs) const { return !operator<(rhs); }

  FbsonFwdIteratorT &operator++() {
    current_ = (iterator)(((char *)current_) + current_->numPackedBytes());
    return *this;
  }

  FbsonFwdIteratorT operator++(int) {
    auto tmp = *this;
    current_ = (iterator)(((char *)current_) + current_->numPackedBytes());
    return tmp;
  }

  explicit operator pointer() { return current_; }

  reference operator*() const { return *current_; }

  pointer operator->() const { return current_; }

  iterator base() const { return current_; }

 private:
  iterator current_;
};

typedef int (*hDictInsert)(const char *key, unsigned len);
typedef int (*hDictFind)(const char *key, unsigned len);

/*
 * FbsonType defines 10 primitive types and 2 container types, as described
 * below.
 *
 * primitive_value ::=
 *   0x00        //null value (0 byte)
 * | 0x01        //boolean true (0 byte)
 * | 0x02        //boolean false (0 byte)
 * | 0x03 int8   //char/int8 (1 byte)
 * | 0x04 int16  //int16 (2 bytes)
 * | 0x05 int32  //int32 (4 bytes)
 * | 0x06 int64  //int64 (8 bytes)
 * | 0x07 double //floating point (8 bytes)
 * | 0x08 string //variable length string
 * | 0x09 binary //variable length binary
 *
 * container ::=
 *   0x0A int32 key_value_list //object, int32 is the total bytes of the object
 * | 0x0B int32 value_list     //array, int32 is the total bytes of the array
 */
enum class FbsonType : char {
  T_Null = 0x00,
  T_True = 0x01,
  T_False = 0x02,
  T_Int8 = 0x03,
  T_Int16 = 0x04,
  T_Int32 = 0x05,
  T_Int64 = 0x06,
  T_Double = 0x07,
  T_String = 0x08,
  T_Binary = 0x09,
  T_Object = 0x0A,
  T_Array = 0x0B,
  NUM_TYPES,
};

typedef std::underlying_type<FbsonType>::type FbsonTypeUnder;

/*
 * FbsonKeyValue class defines FBSON key type, as described below.
 *
 * key ::=
 *   0x00 int8    //1-byte dictionary id
 * | int8 (byte*) //int8 (>0) is the size of the key string
 *
 * value ::= primitive_value | container
 *
 * FbsonKeyValue can be either an id mapping to the key string in an external
 * dictionary, or it is the original key string. Whether to read an id or a
 * string is decided by the first byte (size_).
 *
 * Note: a key object must be followed by a value object. Therefore, a key
 * object implicitly refers to a key-value pair, and you can get the value
 * object right after the key object. The function numPackedBytes hence
 * indicates the total size of the key-value pair, so that we will be able go
 * to next pair from the key.
 *
 * ** Dictionary size **
 * By default, the dictionary size is 255 (1-byte). Users can define
 * "USE_LARGE_DICT" to increase the dictionary size to 655535 (2-byte).
 */
class FbsonKeyValue {
 public:
#ifdef USE_LARGE_DICT
  static const int sMaxKeyId = 65535;
  typedef uint16_t keyid_type;
#else
  static const int sMaxKeyId = 255;
  typedef uint8_t keyid_type;
#endif // #ifdef USE_LARGE_DICT

  static const uint8_t sMaxKeyLen = 64;

  // size of the key. 0 indicates it is stored as id
  uint8_t klen() const { return size_; }

  // get the key string. Note the string may not be null terminated.
  const char *getKeyStr() const { return key_.str_; }

  keyid_type getKeyId() const { return key_.id_; }

  unsigned int keyPackedBytes() const {
    return size_ ? (sizeof(size_) + size_)
                 : (sizeof(size_) + sizeof(keyid_type));
  }

  FbsonValue *value() const {
    return (FbsonValue *)(((char *)this) + keyPackedBytes());
  }

  // size of the total packed bytes (key+value)
  unsigned int numPackedBytes() const;

 private:
  uint8_t size_;

  union key_ {
    keyid_type id_;
    char str_[1];
  } key_;

  FbsonKeyValue();
};

/*
 * FbsonValue is the base class of all FBSON types. It contains only one member
 * variable - type info, which can be retrieved by member functions is[Type]()
 * or type().
 */
class FbsonValue {
 public:
  static const uint32_t sMaxValueLen = 1 << 24; // 16M

  bool isNull() const { return (type_ == FbsonType::T_Null); }
  bool isTrue() const { return (type_ == FbsonType::T_True); }
  bool isFalse() const { return (type_ == FbsonType::T_False); }
  bool isInt8() const { return (type_ == FbsonType::T_Int8); }
  bool isInt16() const { return (type_ == FbsonType::T_Int16); }
  bool isInt32() const { return (type_ == FbsonType::T_Int32); }
  bool isInt64() const { return (type_ == FbsonType::T_Int64); }
  bool isDouble() const { return (type_ == FbsonType::T_Double); }
  bool isString() const { return (type_ == FbsonType::T_String); }
  bool isBinary() const { return (type_ == FbsonType::T_Binary); }
  bool isObject() const { return (type_ == FbsonType::T_Object); }
  bool isArray() const { return (type_ == FbsonType::T_Array); }

  FbsonType type() const { return type_; }

  // size of the total packed bytes
  unsigned int numPackedBytes() const;

 protected:
  FbsonType type_; // type info

  FbsonValue();
};

/*
 * NumerValT is the template class (derived from FbsonValue) of all number
 * types (integers and double).
 */
template <class T>
class NumberValT : public FbsonValue {
 public:
  T val() const { return num_; }

  unsigned int numPackedBytes() const { return sizeof(FbsonValue) + sizeof(T); }

  // catch all unknow specialization of the template class
  bool setVal(T val) { return false; }

 private:
  T num_;

  NumberValT();
};

typedef NumberValT<int8_t> Int8Val;

// override setVal for Int8Val
template <>
inline bool Int8Val::setVal(int8_t val) {
  if (!isInt8()) {
    return false;
  }

  num_ = val;
  return true;
}

typedef NumberValT<int16_t> Int16Val;

// override setVal for Int16Val
template <>
inline bool Int16Val::setVal(int16_t val) {
  if (!isInt16()) {
    return false;
  }

  num_ = val;
  return true;
}

typedef NumberValT<int32_t> Int32Val;

// override setVal for Int32Val
template <>
inline bool Int32Val::setVal(int32_t val) {
  if (!isInt32()) {
    return false;
  }

  num_ = val;
  return true;
}

typedef NumberValT<int64_t> Int64Val;

// override setVal for Int64Val
template <>
inline bool Int64Val::setVal(int64_t val) {
  if (!isInt64()) {
    return false;
  }

  num_ = val;
  return true;
}

typedef NumberValT<double> DoubleVal;

// override setVal for DoubleVal
template <>
inline bool DoubleVal::setVal(double val) {
  if (!isDouble()) {
    return false;
  }

  num_ = val;
  return true;
}

/*
 * BlobVal is the base class (derived from FbsonValue) for string and binary
 * types. The size_ indicates the total bytes of the payload_.
 */
class BlobVal : public FbsonValue {
 public:
  // size of the blob payload only
  unsigned int getBlobLen() const { return size_; }

  // return the blob as byte array
  const char *getBlob() const { return payload_; }

  // size of the total packed bytes
  unsigned int numPackedBytes() const {
    return sizeof(FbsonValue) + sizeof(size_) + size_;
  }

 protected:
  uint32_t size_;
  char payload_[0];

  // set new blob bytes
  bool internalSetVal(const char *blob, uint32_t size) {
    // if we cannot fit the new blob, fail the operation
    if (size > size_) {
      return false;
    }

    memcpy(payload_, blob, size);

    // Set the reset of the bytes to 0.  Note we cannot change the size_ of the
    // current payload, as all values are packed.
    memset(payload_ + size, 0, size_ - size);

    return true;
  }

  BlobVal();
};

/*
 * Binary type
 */
class BinaryVal : public BlobVal {
 public:
  bool setVal(const char *blob, uint32_t size) {
    if (!isBinary()) {
      return false;
    }

    return internalSetVal(blob, size);
  }

 private:
  BinaryVal();
};

/*
 * String type
 * Note: FBSON string may not be a c-string (NULL-terminated)
 */
class StringVal : public BlobVal {
 public:
  bool setVal(const char *str, uint32_t size) {
    if (!isString()) {
      return false;
    }

    return internalSetVal(str, size);
  }

 private:
  StringVal();
};

/*
 * ContainerVal is the base class (derived from FbsonValue) for object and
 * array types. The size_ indicates the total bytes of the payload_.
 */
class ContainerVal : public FbsonValue {
 public:
  // size of the container payload only
  unsigned int getContainerSize() const { return size_; }

  // size of the total packed bytes
  unsigned int numPackedBytes() const {
    return sizeof(FbsonValue) + sizeof(size_) + size_;
  }

 protected:
  uint32_t size_;
  char payload_[0];

  ContainerVal();
};

/*
 * Object type
 */
class ObjectVal : public ContainerVal {
 public:
  // find the FBSON value by a key string
  FbsonValue *find(const char *key, hDictFind handler = nullptr) const {
    int key_id = -1;
    unsigned int klen = strlen(key);
    if (handler && (key_id = handler(key, klen)) >= 0) {
      return find(key_id);
    }

    const char *pch = payload_;
    const char *fence = payload_ + size_;

    while (pch < fence) {
      FbsonKeyValue *pkey = (FbsonKeyValue *)(pch);
      if (klen == pkey->klen() && strncmp(key, pkey->getKeyStr(), klen) == 0) {
        return pkey->value();
      }
      pch += pkey->numPackedBytes();
    }

    assert(pch == fence);

    return nullptr;
  }

  // find the FBSON value by a key dictionary ID
  FbsonValue *find(int key_id) const {
    if (key_id < 0 || key_id > FbsonKeyValue::sMaxKeyId)
      return nullptr;

    const char *pch = payload_;
    const char *fence = payload_ + size_;

    while (pch < fence) {
      FbsonKeyValue *pkey = (FbsonKeyValue *)(pch);
      if (!pkey->klen() && key_id == pkey->getKeyId()) {
        return pkey->value();
      }
      pch += pkey->numPackedBytes();
    }

    assert(pch == fence);

    return nullptr;
  }

  typedef FbsonKeyValue value_type;
  typedef value_type *pointer;
  typedef const value_type *const_pointer;
  typedef FbsonFwdIteratorT<pointer, ObjectVal> iterator;
  typedef FbsonFwdIteratorT<const_pointer, ObjectVal> const_iterator;

  iterator begin() { return iterator((pointer)payload_); }

  const_iterator begin() const { return const_iterator((pointer)payload_); }

  iterator end() { return iterator((pointer)(payload_ + size_)); }

  const_iterator end() const {
    return const_iterator((pointer)(payload_ + size_));
  }

 private:
  ObjectVal();
};

/*
 * Array type
 */
class ArrayVal : public ContainerVal {
 public:
  // get the FBSON value at index
  FbsonValue *get(int idx) const {
    if (idx < 0)
      return nullptr;

    const char *pch = payload_;
    const char *fence = payload_ + size_;

    while (pch < fence && idx-- > 0)
      pch += ((FbsonValue *)pch)->numPackedBytes();

    if (idx == -1)
      return (FbsonValue *)pch;
    else {
      assert(pch == fence);
      return nullptr;
    }
  }

  // Get number of elements in array
  unsigned int numElem() const {
    const char *pch = payload_;
    const char *fence = payload_ + size_;

    unsigned int num = 0;
    while (pch < fence) {
      ++num;
      pch += ((FbsonValue *)pch)->numPackedBytes();
    }

    assert(pch == fence);

    return num;
  }

  typedef FbsonValue value_type;
  typedef value_type *pointer;
  typedef const value_type *const_pointer;
  typedef FbsonFwdIteratorT<pointer, ArrayVal> iterator;
  typedef FbsonFwdIteratorT<const_pointer, ArrayVal> const_iterator;

  iterator begin() { return iterator((pointer)payload_); }

  const_iterator begin() const { return const_iterator((pointer)payload_); }

  iterator end() { return iterator((pointer)(payload_ + size_)); }

  const_iterator end() const {
    return const_iterator((pointer)(payload_ + size_));
  }

 private:
  ArrayVal();
};

inline FbsonDocument *FbsonDocument::createDocument(const char *pb,
                                                    uint32_t size) {
  if (!pb || size < sizeof(FbsonHeader) + sizeof(FbsonValue)) {
    return nullptr;
  }

  FbsonDocument *doc = (FbsonDocument *)pb;
  if (doc->header_.ver_ != FBSON_VER) {
    return nullptr;
  }

  FbsonValue *val = (FbsonValue *)doc->payload_;
  if (!val->isObject() || size != sizeof(FbsonHeader) + val->numPackedBytes()) {
    return nullptr;
  }

  return doc;
}

inline FbsonValue *FbsonDocument::createValue(const char *pb, uint32_t size) {
  if (!pb || size < sizeof(FbsonHeader) + sizeof(FbsonValue)) {
    return nullptr;
  }

  FbsonDocument *doc = (FbsonDocument *)pb;
  if (doc->header_.ver_ != FBSON_VER) {
    return nullptr;
  }

  FbsonValue *val = (FbsonValue *)doc->payload_;
  if (size != sizeof(FbsonHeader) + val->numPackedBytes()) {
    return nullptr;
  }

  return val;
}

inline unsigned int FbsonKeyValue::numPackedBytes() const {
  unsigned int ks = keyPackedBytes();
  FbsonValue *val = (FbsonValue *)(((char *)this) + ks);
  return ks + val->numPackedBytes();
}

// Poor man's "virtual" function FbsonValue::numPackedBytes
inline unsigned int FbsonValue::numPackedBytes() const {
  switch (type_) {
  case FbsonType::T_Null:
  case FbsonType::T_True:
  case FbsonType::T_False: { return sizeof(type_); }

  case FbsonType::T_Int8: { return sizeof(type_) + sizeof(int8_t); }
  case FbsonType::T_Int16: { return sizeof(type_) + sizeof(int16_t); }
  case FbsonType::T_Int32: { return sizeof(type_) + sizeof(int32_t); }
  case FbsonType::T_Int64: { return sizeof(type_) + sizeof(int64_t); }
  case FbsonType::T_Double: { return sizeof(type_) + sizeof(double); }
  case FbsonType::T_String:
  case FbsonType::T_Binary: { return ((BlobVal *)(this))->numPackedBytes(); }

  case FbsonType::T_Object:
  case FbsonType::T_Array: {
    return ((ContainerVal *)(this))->numPackedBytes();
  }
  default:
    return 0;
  }
}

#pragma pack(pop)

} // namespace fbson

#endif // FBSON_FBSONDOCUMENT_H
