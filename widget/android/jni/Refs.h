#ifndef mozilla_jni_Refs_h__
#define mozilla_jni_Refs_h__

#include <jni.h>

#include "mozilla/Move.h"
#include "mozilla/jni/Utils.h"

#include "nsError.h" // for nsresult
#include "nsString.h"
#include "nsTArray.h"

namespace mozilla {
namespace jni {

class Accessor;
template<class T> class Constructor;
template<class T> class Field;
template<class T, typename R> class Method;

// Wrapped object reference (e.g. jobject, jclass, etc...)
template<class Cls> class Ref;
// Wrapped local reference that inherits from Ref.
template<class Cls> class LocalRef;
// Wrapped global reference that inherits from Ref.
template<class Cls> class GlobalRef;

// Type used for a reference parameter. Default is a wrapped object
// reference, but ParamImpl can be specialized to define custom behavior,
// e.g. a StringParam class that automatically converts nsAString& and
// nsACString& to a jstring.
template<class Cls> struct ParamImpl { typedef Ref<Cls> Type; };
template<class Cls> using Param = typename ParamImpl<Cls>::Type;

namespace detail {

template<class Cls> struct TypeAdapter;

} // namespace detail


// How exception during a JNI call should be treated.
enum class ExceptionMode
{
    // Abort on unhandled excepion (default).
    ABORT,
    // Ignore the exception and return to caller.
    IGNORE,
    // Catch any exception and return a nsresult.
    NSRESULT,
};


// Class to hold the native types of a method's arguments.
// For example, if a method has signature (ILjava/lang/String;)V,
// its arguments class would be jni::Args<int32_t, jni::String::Param>
template<typename...>
struct Args {};


// Base class for all JNI binding classes.
// Templated so that we have one sClassRef for each class.
template<class Cls>
class Class
{
    friend class Accessor;
    template<class T> friend class Constructor;
    template<class T> friend class Field;
    template<class T, typename R> friend class Method;

private:
    static jclass sClassRef; // global reference

protected:
    jobject mInstance; // local or global reference

    Class(jobject instance) : mInstance(instance) {}
};


// Binding for a plain jobject.
class Object : public Class<Object>
{
protected:
    Object(jobject instance) : Class(instance) {}

public:
    typedef jni::Ref<Object> Ref;
    typedef jni::LocalRef<Object>  LocalRef;
    typedef jni::GlobalRef<Object> GlobalRef;
    typedef const jni::Param<Object>& Param;

    static constexpr char name[] = "java/lang/Object";
};


// Binding for a built-in object reference other than jobject.
template<typename T>
class TypedObject : public Class<TypedObject<T>>
{
    typedef TypedObject<T> Self;

protected:
    TypedObject(jobject instance) : Class<TypedObject<T>>(instance) {}

public:
    typedef jni::Ref<Self> Ref;
    typedef jni::LocalRef<Self>  LocalRef;
    typedef jni::GlobalRef<Self> GlobalRef;
    typedef const jni::Param<Self>& Param;

    static const char name[];
};

// Define bindings for built-in types.
typedef TypedObject<jstring>    String;
typedef TypedObject<jclass>     ClassObject;
typedef TypedObject<jthrowable> Throwable;

typedef TypedObject<jbooleanArray> BooleanArray;
typedef TypedObject<jbyteArray>    ByteArray;
typedef TypedObject<jcharArray>    CharArray;
typedef TypedObject<jshortArray>   ShortArray;
typedef TypedObject<jintArray>     IntArray;
typedef TypedObject<jlongArray>    LongArray;
typedef TypedObject<jfloatArray>   FloatArray;
typedef TypedObject<jdoubleArray>  DoubleArray;
typedef TypedObject<jobjectArray>  ObjectArray;

template<> struct ParamImpl<String> { class Type; };


// Base class for Ref and its specializations.
template<class Cls, typename JNIType>
class RefBase : protected Cls
{
    typedef RefBase<Cls, JNIType> Self;
    typedef void (Self::*bool_type)() const;
    void non_null_reference() const {}

protected:
    RefBase(jobject instance) : Cls(instance) {}

public:
    // Construct a Ref form a raw JNI reference.
    static Ref<Cls> From(JNIType obj)
    {
        return Ref<Cls>(static_cast<jobject>(obj));
    }

    // Construct a Ref form a generic object reference.
    static Ref<Cls> From(const RefBase<Object, jobject>& obj)
    {
        return Ref<Cls>(obj.Get());
    }

    // Get the raw JNI reference.
    JNIType Get() const
    {
        return static_cast<JNIType>(Cls::mInstance);
    }

    bool operator==(const RefBase& other) const
    {
        // Treat two references of the same object as being the same.
        return Cls::mInstance == other.mInstance &&
                GetEnvForThread()->IsSameObject(
                        Cls::mInstance, other.mInstance) != JNI_FALSE;
    }

    bool operator!=(const RefBase& other) const
    {
        return !operator==(other);
    }

    bool operator==(decltype(nullptr)) const
    {
        return !Cls::mInstance;
    }

    bool operator!=(decltype(nullptr)) const
    {
        return !!Cls::mInstance;
    }

    Cls* operator->()
    {
        MOZ_ASSERT(Cls::mInstance);
        return this;
    }

    const Cls* operator->() const
    {
        MOZ_ASSERT(Cls::mInstance);
        return this;
    }

    // Any ref can be cast to an object ref.
    operator Ref<Object>() const;

    // Null checking (e.g. !!ref) using the safe-bool idiom.
    operator bool_type() const
    {
        return Cls::mInstance ? &Self::non_null_reference : nullptr;
    }

    // We don't allow implicit conversion to jobject because that can lead
    // to easy mistakes such as assigning a temporary LocalRef to a jobject,
    // and using the jobject after the LocalRef has been freed.

    // We don't allow explicit conversion, to make outside code use Ref::Get.
    // Using Ref::Get makes it very easy to see which code is using raw JNI
    // types to make future refactoring easier.

    // operator JNIType() const = delete;
};


// Wrapped object reference (e.g. jobject, jclass, etc...)
template<class Cls>
class Ref : public RefBase<Cls, jobject>
{
    template<class C, typename T> friend class RefBase;
    friend struct detail::TypeAdapter<Ref<Cls>>;

    typedef RefBase<Cls, jobject> Base;

protected:
    // Protected jobject constructor because outside code should be using
    // Ref::From. Using Ref::From makes it very easy to see which code is using
    // raw JNI types for future refactoring.
    Ref(jobject instance) : Base(instance) {}

    // Protected copy constructor so that there's no danger of assigning a
    // temporary LocalRef/GlobalRef to a Ref, and potentially use the Ref
    // after the source had been freed.
    Ref(const Ref& ref) : Base(ref.mInstance) {}

public:
    MOZ_IMPLICIT Ref(decltype(nullptr)) : Base(nullptr) {}
};


template<class Cls, typename JNIType>
RefBase<Cls, JNIType>::operator Ref<Object>() const
{
    return Ref<Object>(Cls::mInstance);
}


template<typename T>
class Ref<TypedObject<T>>
        : public RefBase<TypedObject<T>, T>
{
    friend class RefBase<TypedObject<T>, T>;
    friend struct detail::TypeAdapter<Ref<TypedObject<T>>>;

    typedef RefBase<TypedObject<T>, T> Base;

protected:
    Ref(jobject instance) : Base(instance) {}

    Ref(const Ref& ref) : Base(ref.mInstance) {}

public:
    MOZ_IMPLICIT Ref(decltype(nullptr)) : Base(nullptr) {}
};


namespace {

// See explanation in LocalRef.
template<class Cls> struct GenericObject { typedef Object Type; };
template<> struct GenericObject<Object> { typedef struct {} Type; };
template<class Cls> struct GenericLocalRef
{
    template<class C> struct Type : jni::Object {};
};
template<> struct GenericLocalRef<Object>
{
    template<class C> using Type = jni::LocalRef<C>;
};

} // namespace

template<class Cls>
class LocalRef : public Ref<Cls>
{
    template<class C> friend class LocalRef;

private:
    // In order to be able to convert LocalRef<Object> to LocalRef<Cls>, we
    // need constructors and copy assignment operators that take in a
    // LocalRef<Object> argument. However, if Cls *is* Object, we would have
    // duplicated constructors and operators with LocalRef<Object> arguments. To
    // avoid this conflict, we use GenericObject, which is defined as Object for
    // LocalRef<non-Object> and defined as a dummy class for LocalRef<Object>.
    typedef typename GenericObject<Cls>::Type GenericObject;

    // Similarly, GenericLocalRef is useed to convert LocalRef<Cls> to,
    // LocalRef<Object>. It's defined as LocalRef<C> for Cls == Object,
    // and defined as a dummy template class for Cls != Object.
    template<class C> using GenericLocalRef
            = typename GenericLocalRef<Cls>::template Type<C>;

    static jobject NewLocalRef(JNIEnv* env, jobject obj)
    {
        if (!obj) {
            return nullptr;
        }
        return env->NewLocalRef(obj);
    }

    JNIEnv* const mEnv;

    LocalRef(JNIEnv* env, jobject instance)
        : Ref<Cls>(instance)
        , mEnv(env)
    {}

    LocalRef& swap(LocalRef& other)
    {
        auto instance = other.mInstance;
        other.mInstance = Ref<Cls>::mInstance;
        Ref<Cls>::mInstance = instance;
        return *this;
    }

public:
    // Construct a LocalRef from a raw JNI local reference. Unlike Ref::From,
    // LocalRef::Adopt returns a LocalRef that will delete the local reference
    // when going out of scope.
    static LocalRef Adopt(jobject instance)
    {
        return LocalRef(GetEnvForThread(), instance);
    }

    static LocalRef Adopt(JNIEnv* env, jobject instance)
    {
        return LocalRef(env, instance);
    }

    // Copy constructor.
    LocalRef(const LocalRef<Cls>& ref)
        : Ref<Cls>(NewLocalRef(ref.mEnv, ref.mInstance))
        , mEnv(ref.mEnv)
    {}

    // Move constructor.
    LocalRef(LocalRef<Cls>&& ref)
        : Ref<Cls>(ref.mInstance)
        , mEnv(ref.mEnv)
    {
        ref.mInstance = nullptr;
    }

    explicit LocalRef(JNIEnv* env = GetEnvForThread())
        : Ref<Cls>(nullptr)
        , mEnv(env)
    {}

    // Construct a LocalRef from any Ref,
    // which means creating a new local reference.
    MOZ_IMPLICIT LocalRef(const Ref<Cls>& ref)
        : Ref<Cls>(nullptr)
        , mEnv(GetEnvForThread())
    {
        Ref<Cls>::mInstance = NewLocalRef(mEnv, ref.Get());
    }

    LocalRef(JNIEnv* env, const Ref<Cls>& ref)
        : Ref<Cls>(NewLocalRef(env, ref.Get()))
        , mEnv(env)
    {}

    // Move a LocalRef<Object> into a LocalRef<Cls> without
    // creating/deleting local references.
    MOZ_IMPLICIT LocalRef(LocalRef<GenericObject>&& ref)
        : Ref<Cls>(ref.mInstance)
        , mEnv(ref.mEnv)
    {
        ref.mInstance = nullptr;
    }

    template<class C>
    MOZ_IMPLICIT LocalRef(GenericLocalRef<C>&& ref)
        : Ref<Cls>(ref.mInstance)
        , mEnv(ref.mEnv)
    {
        ref.mInstance = nullptr;
    }

    // Implicitly converts nullptr to LocalRef.
    MOZ_IMPLICIT LocalRef(decltype(nullptr))
        : Ref<Cls>(nullptr)
        , mEnv(GetEnvForThread())
    {}

    ~LocalRef()
    {
        if (Ref<Cls>::mInstance) {
            mEnv->DeleteLocalRef(Ref<Cls>::mInstance);
            Ref<Cls>::mInstance = nullptr;
        }
    }

    // Get the JNIEnv* associated with this local reference.
    JNIEnv* Env() const
    {
        return mEnv;
    }

    // Get the raw JNI reference that can be used as a return value.
    // Returns the same JNI type (jobject, jstring, etc.) as the underlying Ref.
    auto Forget() -> decltype(Ref<Cls>(nullptr).Get())
    {
        const auto obj = Ref<Cls>::Get();
        Ref<Cls>::mInstance = nullptr;
        return obj;
    }

    LocalRef<Cls>& operator=(LocalRef<Cls> ref)
    {
        return swap(ref);
    }

    LocalRef<Cls>& operator=(const Ref<Cls>& ref)
    {
        LocalRef<Cls> newRef(mEnv, ref);
        return swap(newRef);
    }

    LocalRef<Cls>& operator=(LocalRef<GenericObject>&& ref)
    {
        LocalRef<Cls> newRef(mozilla::Move(ref));
        return swap(newRef);
    }

    template<class C>
    LocalRef<Cls>& operator=(GenericLocalRef<C>&& ref)
    {
        LocalRef<Cls> newRef(mozilla::Move(ref));
        return swap(newRef);
    }

    LocalRef<Cls>& operator=(decltype(nullptr))
    {
        LocalRef<Cls> newRef(mEnv, nullptr);
        return swap(newRef);
    }
};


template<class Cls>
class GlobalRef : public Ref<Cls>
{
private:
    static jobject NewGlobalRef(JNIEnv* env, jobject instance)
    {
        if (!instance) {
            return nullptr;
        }
        return env->NewGlobalRef(instance);
    }

    GlobalRef& swap(GlobalRef& other)
    {
        auto instance = other.mInstance;
        other.mInstance = Ref<Cls>::mInstance;
        Ref<Cls>::mInstance = instance;
        return *this;
    }

public:
    GlobalRef()
        : Ref<Cls>(nullptr)
    {}

    // Copy constructor
    GlobalRef(const GlobalRef& ref)
        : Ref<Cls>(NewGlobalRef(GetEnvForThread(), ref.mInstance))
    {}

    // Move constructor
    GlobalRef(GlobalRef&& ref)
        : Ref<Cls>(ref.mInstance)
    {
        ref.mInstance = nullptr;
    }

    MOZ_IMPLICIT GlobalRef(const Ref<Cls>& ref)
        : Ref<Cls>(NewGlobalRef(GetEnvForThread(), ref.Get()))
    {}

    GlobalRef(JNIEnv* env, const Ref<Cls>& ref)
        : Ref<Cls>(NewGlobalRef(env, ref.Get()))
    {}

    MOZ_IMPLICIT GlobalRef(const LocalRef<Cls>& ref)
        : Ref<Cls>(NewGlobalRef(ref.Env(), ref.Get()))
    {}

    // Implicitly converts nullptr to GlobalRef.
    MOZ_IMPLICIT GlobalRef(decltype(nullptr))
        : Ref<Cls>(nullptr)
    {}

    ~GlobalRef()
    {
        if (Ref<Cls>::mInstance) {
            Clear(GetEnvForThread());
        }
    }

    // Get the raw JNI reference that can be used as a return value.
    // Returns the same JNI type (jobject, jstring, etc.) as the underlying Ref.
    auto Forget() -> decltype(Ref<Cls>(nullptr).Get())
    {
        const auto obj = Ref<Cls>::Get();
        Ref<Cls>::mInstance = nullptr;
        return obj;
    }

    void Clear(JNIEnv* env)
    {
        if (Ref<Cls>::mInstance) {
            env->DeleteGlobalRef(Ref<Cls>::mInstance);
            Ref<Cls>::mInstance = nullptr;
        }
    }

    GlobalRef<Cls>& operator=(GlobalRef<Cls> ref)
    {
        return swap(ref);
    }

    GlobalRef<Cls>& operator=(const Ref<Cls>& ref)
    {
        GlobalRef<Cls> newRef(ref);
        return swap(newRef);
    }

    GlobalRef<Cls>& operator=(decltype(nullptr))
    {
        GlobalRef<Cls> newRef(nullptr);
        return swap(newRef);
    }
};


// Ref specialization for arrays.
template<class Type, typename JNIType, class ElementType>
class ArrayRefBase : public RefBase<Type, JNIType>
{
    typedef RefBase<Type, JNIType> Base;

protected:
    ArrayRefBase(jobject instance) : Base(instance) {}

    ArrayRefBase(const ArrayRefBase& ref) : Base(ref.mInstance) {}

public:
    size_t Length() const
    {
        MOZ_ASSERT(Base::mInstance);
        JNIEnv* const env = GetEnvForThread();
        const size_t ret = env->GetArrayLength(JNIType(Base::mInstance));
        MOZ_CATCH_JNI_EXCEPTION(env);
        return ret;
    }

    ElementType GetElement(size_t index) const
    {
        MOZ_ASSERT(Base::mInstance);
        JNIEnv* const env = GetEnvForThread();
        ElementType ret;
        (env->*detail::TypeAdapter<ElementType>::GetArray)(
                JNIType(Base::mInstance), jsize(index), 1, &ret);
        MOZ_CATCH_JNI_EXCEPTION(env);
        return ret;
    }

    nsTArray<ElementType> GetElements() const
    {
        MOZ_ASSERT(Base::mInstance);
        static_assert(sizeof(ElementType) ==
                sizeof(typename detail::TypeAdapter<ElementType>::JNIType),
                "Size of native type must match size of JNI type");

        JNIEnv* const env = GetEnvForThread();
        const jsize len = size_t(env->GetArrayLength(
                JNIType(Base::mInstance)));

        nsTArray<ElementType> array((size_t(len)));
        array.SetLength(size_t(len));
        (env->*detail::TypeAdapter<ElementType>::GetArray)(
                JNIType(Base::mInstance), 0, len, array.Elements());
        return array;
    }

    ElementType operator[](size_t index) const
    {
        return GetElement(index);
    }

    operator nsTArray<ElementType>() const
    {
        return GetElements();
    }
};

#define DEFINE_PRIMITIVE_ARRAY_REF(Type, JNIType, ElementType) \
    template<> \
    class Ref<Type> : public ArrayRefBase<Type, JNIType, ElementType> \
    { \
        friend class RefBase<Type, JNIType>; \
        friend class detail::TypeAdapter<Ref<Type>>; \
    \
        typedef ArrayRefBase<Type, JNIType, ElementType> Base; \
    \
    protected: \
        Ref(jobject instance) : Base(instance) {} \
    \
        Ref(const Ref& ref) : Base(ref.mInstance) {} \
    \
    public: \
        MOZ_IMPLICIT Ref(decltype(nullptr)) : Base(nullptr) {} \
    }

DEFINE_PRIMITIVE_ARRAY_REF(BooleanArray, jbooleanArray, bool);
DEFINE_PRIMITIVE_ARRAY_REF(ByteArray,    jbyteArray,    int8_t);
DEFINE_PRIMITIVE_ARRAY_REF(CharArray,    jcharArray,    char16_t);
DEFINE_PRIMITIVE_ARRAY_REF(ShortArray,   jshortArray,   int16_t);
DEFINE_PRIMITIVE_ARRAY_REF(IntArray,     jintArray,     int32_t);
DEFINE_PRIMITIVE_ARRAY_REF(LongArray,    jlongArray,    int64_t);
DEFINE_PRIMITIVE_ARRAY_REF(FloatArray,   jfloatArray,   float);
DEFINE_PRIMITIVE_ARRAY_REF(DoubleArray,  jdoubleArray,  double);

#undef DEFINE_PRIMITIVE_ARRAY_REF

// Ref specialization for jobjectArray.
template<>
class Ref<ObjectArray> : public RefBase<ObjectArray, jobjectArray>
{
    friend class RefBase<ObjectArray, jobjectArray>;
    friend class detail::TypeAdapter<Ref<ObjectArray>>;

    typedef RefBase<ObjectArray, jobjectArray> Base;

protected:
    Ref(jobject instance) : Base(instance) {}

    Ref(const Ref& ref) : Base(ref.mInstance) {}

public:
    MOZ_IMPLICIT Ref(decltype(nullptr)) : Base(nullptr) {}

    size_t Length() const
    {
        MOZ_ASSERT(Base::mInstance);
        JNIEnv* const env = GetEnvForThread();
        const size_t ret = env->GetArrayLength(jobjectArray(Base::mInstance));
        MOZ_CATCH_JNI_EXCEPTION(env);
        return ret;
    }

    Object::LocalRef GetElement(size_t index) const
    {
        MOZ_ASSERT(ObjectArray::mInstance);
        JNIEnv* const env = GetEnvForThread();
        auto ret = Object::LocalRef::Adopt(env, env->GetObjectArrayElement(
                jobjectArray(ObjectArray::mInstance), jsize(index)));
        MOZ_CATCH_JNI_EXCEPTION(env);
        return ret;
    }

    nsTArray<Object::LocalRef> GetElements() const
    {
        MOZ_ASSERT(ObjectArray::mInstance);
        JNIEnv* const env = GetEnvForThread();
        const jsize len = size_t(env->GetArrayLength(
                jarray(ObjectArray::mInstance)));

        nsTArray<Object::LocalRef> array((size_t(len)));
        for (jsize i = 0; i < len; i++) {
            array.AppendElement(Object::LocalRef::Adopt(
                    env, env->GetObjectArrayElement(
                            jobjectArray(ObjectArray::mInstance), i)));
            MOZ_CATCH_JNI_EXCEPTION(env);
        }
        return array;
    }

    Object::LocalRef operator[](size_t index) const
    {
        return GetElement(index);
    }

    operator nsTArray<Object::LocalRef>() const
    {
        return GetElements();
    }

    void SetElement(size_t index, Object::Param element) const
    {
        MOZ_ASSERT(ObjectArray::mInstance);
        JNIEnv* const env = GetEnvForThread();
        env->SetObjectArrayElement(jobjectArray(ObjectArray::mInstance),
                                   jsize(index), element.Get());
        MOZ_CATCH_JNI_EXCEPTION(env);
    }
};


// Ref specialization for jstring.
template<>
class Ref<String> : public RefBase<String, jstring>
{
    friend class RefBase<String, jstring>;
    friend struct detail::TypeAdapter<Ref<String>>;

    typedef RefBase<String, jstring> Base;

protected:
    Ref(jobject instance) : Base(instance) {}

    Ref(const Ref& ref) : Base(ref.mInstance) {}

public:
    MOZ_IMPLICIT Ref(decltype(nullptr)) : Base(nullptr) {}

    // Get the length of the jstring.
    size_t Length() const
    {
        JNIEnv* const env = GetEnvForThread();
        return env->GetStringLength(Get());
    }

    // Convert jstring to a nsString.
    operator nsString() const
    {
        MOZ_ASSERT(String::mInstance);

        JNIEnv* const env = GetEnvForThread();
        const jchar* const str = env->GetStringChars(Get(), nullptr);
        const jsize len = env->GetStringLength(Get());

        nsString result(reinterpret_cast<const char16_t*>(str), len);
        env->ReleaseStringChars(Get(), str);
        return result;
    }

    // Convert jstring to a nsCString.
    operator nsCString() const
    {
        return NS_ConvertUTF16toUTF8(operator nsString());
    }
};


// Define a custom parameter type for String,
// which accepts both String::Ref and nsAString/nsACString
class ParamImpl<String>::Type : public Ref<String>
{
private:
    // Not null if we should delete ref on destruction.
    JNIEnv* const mEnv;

    static jstring GetString(JNIEnv* env, const nsAString& str)
    {
        const jstring result = env->NewString(
                reinterpret_cast<const jchar*>(str.BeginReading()),
                str.Length());
        MOZ_CATCH_JNI_EXCEPTION(env);
        return result;
    }

public:
    MOZ_IMPLICIT Type(const String::Ref& ref)
        : Ref<String>(ref.Get())
        , mEnv(nullptr)
    {}

    MOZ_IMPLICIT Type(const nsAString& str, JNIEnv* env = GetEnvForThread())
        : Ref<String>(GetString(env, str))
        , mEnv(env)
    {}

    MOZ_IMPLICIT Type(const char16_t* str, JNIEnv* env = GetEnvForThread())
        : Ref<String>(GetString(env, nsDependentString(str)))
        , mEnv(env)
    {}

    MOZ_IMPLICIT Type(const nsACString& str, JNIEnv* env = GetEnvForThread())
        : Ref<String>(GetString(env, NS_ConvertUTF8toUTF16(str)))
        , mEnv(env)
    {}

    MOZ_IMPLICIT Type(const char* str, JNIEnv* env = GetEnvForThread())
        : Ref<String>(GetString(env, NS_ConvertUTF8toUTF16(str)))
        , mEnv(env)
    {}

    ~Type()
    {
        if (mEnv) {
            mEnv->DeleteLocalRef(Get());
        }
    }

    operator String::LocalRef() const
    {
        // We can't return our existing ref because the returned
        // LocalRef could be freed first, so we need a new local ref.
        return String::LocalRef(mEnv ? mEnv : GetEnvForThread(), *this);
    }
};


// Support conversion from LocalRef<T>* to LocalRef<Object>*:
//   LocalRef<Foo> foo;
//   Foo::GetFoo(&foo); // error because parameter type is LocalRef<Object>*.
//   Foo::GetFoo(ReturnTo(&foo)); // OK because ReturnTo converts the argument.
template<class Cls>
class ReturnToLocal
{
private:
    LocalRef<Cls>* const localRef;
    LocalRef<Object> objRef;

public:
    explicit ReturnToLocal(LocalRef<Cls>* ref) : localRef(ref) {}
    operator LocalRef<Object>*() { return &objRef; }

    ~ReturnToLocal()
    {
        if (objRef) {
            *localRef = mozilla::Move(objRef);
        }
    }
};

template<class Cls>
ReturnToLocal<Cls> ReturnTo(LocalRef<Cls>* ref)
{
    return ReturnToLocal<Cls>(ref);
}


// Support conversion from GlobalRef<T>* to LocalRef<Object/T>*:
//   GlobalRef<Foo> foo;
//   Foo::GetFoo(&foo); // error because parameter type is LocalRef<Foo>*.
//   Foo::GetFoo(ReturnTo(&foo)); // OK because ReturnTo converts the argument.
template<class Cls>
class ReturnToGlobal
{
private:
    GlobalRef<Cls>* const globalRef;
    LocalRef<Object> objRef;
    LocalRef<Cls> clsRef;

public:
    explicit ReturnToGlobal(GlobalRef<Cls>* ref) : globalRef(ref) {}
    operator LocalRef<Object>*() { return &objRef; }
    operator LocalRef<Cls>*() { return &clsRef; }

    ~ReturnToGlobal()
    {
        if (objRef) {
            *globalRef = (clsRef = mozilla::Move(objRef));
        } else if (clsRef) {
            *globalRef = clsRef;
        }
    }
};

template<class Cls>
ReturnToGlobal<Cls> ReturnTo(GlobalRef<Cls>* ref)
{
    return ReturnToGlobal<Cls>(ref);
}

} // namespace jni
} // namespace mozilla

#endif // mozilla_jni_Refs_h__
