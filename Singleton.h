// 单例模式封装
// date 2024-4-14

#pragma once

#include <memory>

template <typename T, typename X, int N>
T &GetInstanceX()
{
    static T v; // C++11之后编译器保证多线程创建静态成员安全创建
    return v;
}

template <typename T, typename X, int N>
std::shared_ptr<T> GetInstancePtr()
{
    static std::shared_ptr<T> v(new T);
    return v;
}

// 单例模式封装类
// T 类型
// X 为了创造多个实例对应的Tag
// N 同一个Tag创造多个实例索引
template <typename T, typename X = void, int N = 0>
class Singleton
{
public:
    // 返回单例裸指针
    static T *GetInstance()
    {
        static T v;
        return &v;
    }
};

// 单例模式智能指针封装类
// T 类型
// X 为了创造多个实例对应的Tag
// N 同一个Tag创造多个实例索引
template <typename T, typename X = void, int N = 0>
class SingletonPtr
{
public:
    // 返回单例智能指针
    static std::shared_ptr<T> GetInstance()
    {
        static std::shared_ptr<T> v(new T);
        return v;
    }
};

