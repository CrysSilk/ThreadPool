# ThreadPool 项目学习：知识点查漏补缺

> 本笔记用于补充阅读 ThreadPool 项目源码时容易遇到、但基础知识中容易遗漏的 C++ 知识点。
>
> 内容以当前项目学习所需要的知识为中心，重点覆盖：
>
> - C++ 多态与对象切片
> - 任务队列为什么使用智能指针
> - `enum` 与 `enum class`
> - 禁用拷贝构造与赋值
> - `const` 与 `#define`
> - Callable、Lambda、`std::function`、`std::bind`
> - `std::unique_ptr`、`std::move`、`std::make_unique`
> - `std::condition_variable` 的 `wait` 系列函数
> - C++17 `std::any` 与手写 `Any`
> - 虚析构函数与 `= default`

---

## 一、C++ 多态与对象切片

<details>
<summary>点击展开查看详情</summary>

### 1. 先看项目里的场景

`Task` 是一个抽象基类，用户通过继承它来定义具体任务：

```cpp
class Task
{
public:
    virtual void run() = 0;   // 纯虚函数
};

class MyTask : public Task
{
public:
    void run() override
    {
        std::cout << "MyTask run" << std::endl;
    }
};
```

然后：

```cpp
MyTask task;
Task* p = &task;
p->run();   // 输出 MyTask run
```

这里**能够发生多态**，因为 `p` 指向的实际对象仍然是 `MyTask`。

> 补充：`Task` 含有纯虚函数，是抽象类，**本来就不能创建 `Task` 类型的对象**，所以 `Task t = task;` 连编译都过不了。这也是线程池里只能用"指针 / 引用 / 智能指针"来持有任务的原因之一。

### 2. 为什么传值不行？

假设存在一个普通的基类（非抽象）：

```cpp
class Animal
{
public:
    virtual void speak() { std::cout << "Animal" << std::endl; }
};

class Dog : public Animal
{
public:
    void speak() override { std::cout << "Dog" << std::endl; }
};
```

如果：

```cpp
Dog dog;
Animal a = dog;   // 拷贝构造一个 Animal
a.speak();        // 输出 Animal，而不是 Dog
```

这里发生了：

```text
Dog 对象
   │
   │ 拷贝
   ↓
Animal 对象
```

`a` 已经不是 `Dog` 对象了，而是一个**独立的 `Animal` 对象**。这就是：

**对象切片（Object Slicing）**——派生类中属于 `Dog` 的那部分被"切掉"了。

> 补充：切片不只发生在 `Animal a = dog;`，下面这些场景同样会切片：
>
> - 函数按值传参：`void f(Animal a);`，调用 `f(dog);`
> - 容器按值存储：`std::vector<Animal> v; v.push_back(dog);`
>
> 所以任务队列不能写成 `std::queue<Task>`，而应该存放指针 / 智能指针。

### 3. 指针为什么可以？

```cpp
Dog dog;
Animal* p = &dog;
p->speak();
```

这里**没有创建新的 `Animal` 对象**，内存布局实际上是：

```text
        dog
   ┌───────────────┐
   │ Animal 部分   │
   │ Dog 部分      │
   └───────────────┘
         ▲
         │
       p 指针
```

`p` 的静态类型是 `Animal*`，但它实际指向的是 `Dog` 对象。所以调用 `p->speak()` 时，C++ 根据**虚函数机制**（虚表）找到 `Dog::speak()`，这就是多态。

### 4. 引用也是同样的道理

```cpp
Dog dog;
Animal& ref = dog;
ref.speak();   // 输出 Dog
```

`ref` 不是一个新的 `Animal` 对象，它只是 `dog` 的另一个名字：

```text
Dog dog
  ▲
  │
Animal& ref
```

所以仍然可以调用 `Dog::speak()`。

### 5. 小结

| 方式 | 是否创建新对象 | 是否发生多态 |
| --- | --- | --- |
| 按值（`Animal a = dog`） | 是（拷贝并切片） | 否 |
| 指针（`Animal* p = &dog`） | 否 | 是 |
| 引用（`Animal& r = dog`） | 否 | 是 |

**多态的两个条件：** ① 函数是虚函数；② 通过基类的指针或引用调用。

</details>

---

## 二、任务队列为什么使用智能指针

<details>
<summary>点击展开查看详情</summary>

### 1. 先看裸指针为什么会出现悬空

假设用户这样提交任务，而 `submitTask()` 是：

```cpp
void submitTask(Task* task)
{
    taskQueue.push(task);   // 队列里存的是裸指针
}
```

如果用户传入的对象是**局部对象或临时对象**，可以理解成：

```text
┌─────────────────┐
│ 局部 / 临时对象 │ ◄──── Task* task
└─────────────────┘            │
                               ▼
                      ┌─────────────────┐
                      │ TaskQueue       │
                      │ Task*           │
                      └─────────────────┘
```

例如：

```cpp
void foo(ThreadPool& pool)
{
    MyTask t;
    pool.submitTask(&t);   // 队列保存了 &t
}                          // foo 结束，t 被析构
```

> 说明：`pool.submitTask(MyTask())` 这种写法里的 `MyTask()` 是临时对象（右值），不能直接对它取地址得到裸指针；但无论是局部对象还是临时对象，**生命周期都不会因为你保存了一个裸指针而延长**，问题本质是一样的。

对象一旦析构：

```text
MyTask 对象
   ↓
析构
   ↓
内存不再代表有效对象

Task*
   ↓
仍然保存原来的地址
   ↓
悬空指针（Dangling Pointer）
```

之后工作线程执行：

```cpp
Task* task = taskQueue.front();
task->run();   // 访问了一个已经析构的对象 —— 未定义行为
```

这就是问题的核心：**任务的提交线程与执行线程不同，对象的生命周期必须由"队列"来保证，而不能依赖提交者。**

### 2. 智能指针如何解决

让队列保存智能指针，例如 `std::shared_ptr<Task>`：

```cpp
std::queue<std::shared_ptr<Task>> taskQueue;

void submitTask(std::shared_ptr<Task> task)
{
    taskQueue.push(task);   // 队列共同持有任务，引用计数 +1
}
```

- **延长生命周期**：只要任务还在队列中（或正在被线程执行），对象就不会被销毁，不会悬空。
- **自动释放**：任务执行完、最后一个智能指针销毁时，对象自动 `delete`，不需要手动管理，也不会泄漏。
- **保留多态**：智能指针本质上仍是"指针"，通过 `shared_ptr<Task>` 调用 `run()` 依然是多态调用，同时规避了第一节的对象切片问题。

### 3. 小结

| 队列元素类型 | 问题 |
| --- | --- |
| `Task`（按值） | 抽象类无法实例化；即使是普通基类也会对象切片 |
| `Task*`（裸指针） | 对象生命周期无人保证 → 悬空指针；谁负责 `delete` 也不明确 |
| `shared_ptr<Task>` | 生命周期随队列自动管理，多态保留 ✅ |

</details>

---

## 三、enum 与 enum class

<details>
<summary>点击展开查看详情</summary>

### Part 1：enum 和 enum class 的由来

- `enum`：C++98 就存在的**未限域枚举**（也称传统枚举），继承自 C 语言。
- `enum class`（也可写成 `enum struct`）：C++11 引入的**限域枚举**（scoped enum）。

C++98 继承自 C 语言的 `enum` 本质上是整数的别名集合，其设计存在三个核心缺陷：

- **作用域污染**：枚举值自动进入外层作用域，形成命名污染
- **类型不安全**：允许与整数类型自由转换，破坏类型系统完整性
- **前置声明障碍**：无法在头文件中进行前置声明，影响模块化设计

`enum class` 通过以下改进解决了上述问题：

- **作用域隔离**：枚举值必须通过作用域解析运算符 `::` 访问
- **类型安全约束**：禁止隐式转换到整数类型
- **前置声明支持**：底层类型默认是 `int`，编译器可预知类型大小

```cpp
// 典型作用域污染案例（同一作用域内的冲突）
enum Color { RED, GREEN, BLUE };
int RED = 255;   // 编译错误：RED 已被枚举值占用
```

```cpp
enum class Color { Red, Green, Blue };
int Red = 255;                // 合法：枚举值不会泄漏到外层作用域
Color c = Color::Red;         // 必须显式指定作用域
```

### Part 2：enum 和 enum class 的区别

#### 2.1 命名空间污染问题

`enum` 存在命名空间污染的问题。声明 `enum` 时，其中的枚举常量会**泄漏到外层作用域**，在该作用域内就不能再出现与枚举常量同名的标识符，否则会导致编译错误。

```cpp
// 传统枚举 enum 的命名冲突问题
enum Color {
    red,
    green,
    blue
};

enum Light {
    red,      // 编译错误，red 已在 Color 中定义，产生命名冲突
    yellow
};

// 即使在不同的头文件中定义，只要被同一源文件包含，也会出现冲突
// 例如 color.h 中定义了 enum Color{red, ...};
//      light.h 中定义了 enum Light{red, ...};
// 当某个源文件同时包含这两个头文件时，会出现 red 重复定义的错误

// 更危险的是，枚举常量可能与函数名、变量名等冲突
void red() {   // 编译错误，与 Color 中的 red 冲突
    // ...
}
```

而 `enum class` 很好地解决了这个问题。它的枚举常量被限定在自身的作用域内，必须通过 `::` 才能访问，因此不会与其他作用域中的标识符产生冲突。

```cpp
// enum class 避免命名冲突
enum class Color {
    red,
    green,
    blue
};

enum class TrafficLight {
    red,      // 正确，与 Color 中的 red 处于不同作用域，无冲突
    yellow,
    green
};

// 使用时必须通过作用域限定符
Color c = Color::red;
TrafficLight t = TrafficLight::red;

void red() {  // 正确，与枚举常量不冲突
    // ...
}
```

#### 2.2 类型安全问题

`enum` 不是类型安全的：它允许枚举常量**隐式转换为整数类型**，也允许**不同枚举类型的常量之间进行比较**，这可能导致一些隐藏的 bug。

```cpp
// enum 的隐式转换问题
enum Priority { low, medium, high };
enum Status   { success, failure };

// 枚举常量隐式转换为 int
int value = low;          // 正确，low 被隐式转换为 0

// 枚举常量与其他数值比较
if (medium == 1) {        // 正确，medium 隐式转换为 1 后比较
    // ...
}

// 危险：不相关的枚举类型之间也能比较（通常最多一个警告）
if (low == success) {     // 都提升为 int（0 == 0），逻辑上毫无意义却能通过编译
    // ...
}

// 危险：函数参数用 int 接收时，传错枚举也不会被检测
void process(int priority, int status) { /* ... */ }

int main() {
    Priority p = high;
    Status s = failure;
    process(s, p);        // 参数顺序错了，但都隐式转成 int，编译器不会报错
    return 0;
}
```

`enum class` 是类型安全的：它阻止枚举常量的隐式转换，不同枚举类型之间也不能比较，必须通过显式转换才能把枚举常量转换为其他类型。

```cpp
// enum class 的类型安全
enum class Priority { low, medium, high };
enum class Status   { success, failure };

// 不允许隐式转换为 int
int value = Priority::low;                    // 编译错误
// 必须显式转换
int value2 = static_cast<int>(Priority::low); // 正确

// 不允许与其他数值直接比较
if (Priority::medium == 1) { }                // 编译错误

// 不同枚举类型之间不能比较
if (Priority::low == Status::success) { }     // 编译错误

// 参数传递错误会被编译器检测
void process(Priority p, Status s) { /* ... */ }

int main() {
    Priority p = Priority::high;
    Status s = Status::failure;
    process(s, p);        // 编译错误，类型不匹配
    return 0;
}
```

#### 2.3 前置声明支持

传统的 `enum` 在 C++98 标准下**不支持前置声明**，必须在声明的同时进行定义。这是因为 `enum` 的底层类型由编译器根据枚举常量的值隐式确定，在没有定义的情况下，编译器无法知道其大小。

```cpp
// 传统 enum 不支持前置声明（C++98）
enum Status;              // 编译错误，必须同时定义

// 必须像这样声明并定义
enum Status {
    success,
    failure
};
```

这会带来一个问题：如果在头文件中定义了 `enum`，当枚举内容发生变化时，所有包含该头文件的源文件都需要重新编译，增加了编译时间。

而 `enum class` 支持前置声明，因为它的底层类型默认是 `int`，编译器知道其大小：

```cpp
// enum class 支持前置声明
enum class Status;        // 正确，前置声明

// 在其他地方进行定义
enum class Status {
    success,
    failure
};
```

此外，在 C++11 标准中，传统的 `enum` 也可以通过**显式指定底层类型**来支持前置声明：

```cpp
// C++11 中，显式指定底层类型的 enum 支持前置声明
enum Status : int;        // 正确，前置声明并指定底层类型为 int

// 定义
enum Status : int {
    success,
    failure
};
```

支持前置声明的好处是可以将**声明和定义分离**：头文件中只放前置声明，定义放在源文件中。当枚举内容发生变化时，只需要重新编译包含定义的源文件，其他包含头文件的源文件无需重新编译，减少了编译时间。

### 小结

| 对比项 | `enum` | `enum class` |
| --- | --- | --- |
| 引入标准 | C++98（继承自 C） | C++11 |
| 作用域 | 枚举值泄漏到外层作用域 | 枚举值限定在枚举类型内，需 `Color::Red` |
| 隐式转 `int` | 允许 | 禁止，需 `static_cast` |
| 不同枚举互相比较 | 可能通过编译 | 编译错误 |
| 前置声明 | C++98 不支持（C++11 需显式指定底层类型） | 支持（默认底层类型 `int`） |

</details>

---

## 四、禁用拷贝构造与赋值操作

<details>
<summary>点击展开查看详情</summary>

### 1. 为什么要禁用拷贝和赋值？

在以下场景中，通常需要禁用拷贝构造和赋值操作：

1. **资源管理类**：如封装文件句柄、网络连接或动态内存的类。拷贝会导致多个对象持有同一份资源，从而被多次释放，引发崩溃。
2. **单例模式**：为了确保只有一个实例，需要防止拷贝和赋值。
3. **语义上不应被复制的对象**：例如 `std::unique_ptr`、互斥锁 `std::mutex`、线程对象 `std::thread`。

> 在 ThreadPool 项目中：线程池对象内部持有线程、互斥锁、条件变量等资源，它们本身就不可拷贝，因此线程池自身也应该禁止拷贝和赋值。

### 2. 语法

**C++11 及之后（推荐）：使用 `= delete`**

```cpp
class NonCopyable {
public:
    NonCopyable() = default;                               // 默认构造函数
    ~NonCopyable() = default;                              // 默认析构函数

    NonCopyable(const NonCopyable&) = delete;              // 禁用拷贝构造
    NonCopyable& operator=(const NonCopyable&) = delete;   // 禁用赋值操作
};
```

**C++11 之前的做法（了解即可）：** 把拷贝构造和赋值运算符声明为 `private` 且不提供定义，使外部调用时报编译 / 链接错误。`= delete` 的优势是错误在编译期给出，报错信息更清晰。

### 3. 使用效果

```cpp
NonCopyable a;
NonCopyable b = a;   // 编译错误：使用了被删除的拷贝构造函数
NonCopyable c;
c = a;               // 编译错误：使用了被删除的赋值运算符
```

</details>

---

## 五、const 与 #define

<details>
<summary>点击展开查看详情</summary>

### 1. 为什么要用符号常量

符号常量可提高代码的可读性和可维护性。常量名指出了其含义，如果要修改它的值，只需修改一次定义，然后重新编译即可。

### 2. 两种方式

C 使用预处理器来创建常量的符号名称：

```c
#define MAX_LENGTH 100
```

预处理器在编译之前对源代码执行**文本置换**，即用 `100` 替换所有的 `MAX_LENGTH`。所以宏定义是一个"预处理阶段"的概念，在预编译阶段就被替换掉了，**不能被调试**。

而 C++ 则在变量声明中使用限定符 `const`：

```cpp
const int MAX_LENGTH = 100;
```

这样 `MAX_LENGTH` 将被视为一个只读的 `int` 变量。`const` 常量由编译器处理，参与类型检查，并且在符号表中可见，所以**可以被调试**。

### 3. const 的优越性

**① 声明显式指明了类型。** `#define` 只是简单的字符串替换，没有类型检查，所以必须在数字后加后缀来指出除 `char`、`int`、`double` 之外的类型，例如用 `100L` 表示 `long`，用 `3.14F` 表示 `float`。更重要的是，`const` 方法可以很方便地用于复合类型：

```cpp
const int base_vals[5] = {1000, 2000, 3500, 6000, 10000};
const string ans[3] = {"yes", "no", "maybe"};
```

**② `const` 标识符遵循变量的作用域规则**，因此可以创建作用域为全局、名称空间或数据块（函数 / 代码块）的常量。在特定函数中定义常量时，不必担心其定义会与程序其他地方使用的全局常量冲突。例如：

```cpp
#include <iostream>
#include <memory>
using namespace std;

#define n 5
const int dz = 12;

void DZ()
{
    int a = n;
    int dz = 34;                      // 局部变量，遮蔽（隐藏）全局的 dz
    cout << a << " " << dz << endl;   // 输出：5 34
    cout << addressof(dz) << " ";     // 局部 dz 的地址
}

int main() {
    DZ();
    cout << addressof(dz) << endl;    // 全局 const dz 的地址，与上面不同
    return 0;
}
```

首先预处理器会把 `n` 替换为 `5`，所以输出 `a` 为 5。而 `DZ()` 中定义的 `dz` 是局部变量，与 `const` 定义的全局变量无关，两者地址不同。

> 这也说明了宏的一个隐患：`#define n 5` 没有作用域，之后任何地方出现的标识符 `n`（包括别的函数里的局部变量名）都会被替换。

### 4. 存储方式的区别

- 宏定义是直接文本替换，**不会分配内存**，值被直接嵌入到使用它的代码中
- `const` 常量是有类型的变量，**通常需要内存分配**，存储于程序的只读数据段中（编译器也可能把它优化掉，直接当作字面量使用）

### 5. 结论

综上所述，在 C++ 中使用 `const` 定义常量，而不使用 `#define` 定义宏常量。（C++11 起，编译期常量也可以使用 `constexpr`。）

### 6. #define 仍然有用的地方

在控制何时编译头文件方面，`#define` 编译指令仍然很有帮助（头文件保护）：

```cpp
// blooper.h
#ifndef _BLOOPER_H_
#define _BLOOPER_H_
// code goes here
#endif
```

### 小结

| 对比项 | `#define` | `const` |
| --- | --- | --- |
| 处理阶段 | 预处理阶段文本替换 | 编译器处理，参与语义分析 |
| 类型检查 | 无 | 有 |
| 作用域 | 无（从定义处到文件末尾） | 遵循变量作用域规则 |
| 调试 | 不可调试（已被替换） | 可调试 |
| 内存 | 不分配 | 通常分配（可能被优化） |
| 复合类型（数组等） | 不适用 | 适用 |

</details>

---

## 六、Callable：什么是可调用对象

<details>
<summary>点击展开查看详情</summary>

### 1. 概念

简单来说，就是形如 `对象(...)` 这种形式能够被调用的东西。

**普通函数：**

```cpp
void hello()
{
    std::cout << "hello\n";
}

hello();
```

**函数指针：**

```cpp
void (*func)() = hello;

func();
```

**Lambda：**

```cpp
auto func = []() {
    std::cout << "hello\n";
};

func();
```

**函数对象（仿函数）：**

```cpp
struct Hello
{
    void operator()()
    {
        std::cout << "hello\n";
    }
};

Hello h;
h();
```

这些东西虽然**类型完全不同**，但它们都有一个共同点：都能写成 `xxx();` 的形式被调用。

而 `std::function` 的核心作用，就是：**把这些不同类型的可调用对象统一装起来。**

此外，`std::bind` 的返回值、成员函数指针（需配合对象调用）也属于可调用对象的范畴。

### 2. Lambda 补充

Lambda 可以直接在需要的地方写出一个临时的可调用对象，通常比 `std::bind` 更直观。基本形式：

```cpp
[捕获列表](参数列表) -> 返回值类型 { 函数体 }
```

常见捕获方式：

| 写法 | 含义 |
| --- | --- |
| `[]` | 不捕获任何外部变量 |
| `[x]` | 按值捕获 `x`（拷贝一份） |
| `[&x]` | 按引用捕获 `x` |
| `[=]` | 按值捕获所有用到的外部变量 |
| `[&]` | 按引用捕获所有用到的外部变量 |
| `[this]` | 捕获当前对象指针（在成员函数里使用成员时需要） |

> 注意：按引用捕获的 Lambda 如果被保存起来延后执行（如放进任务队列），被捕获的变量可能已经销毁，同样会产生悬空引用，这与第二节的问题本质相同。

### 3. 小结

```text
Lambda / 普通函数 / 成员函数 → 可以被 std::function 统一包装
std::bind                    → 可以把函数和参数提前绑定，生成一个新的可调用对象
Lambda                       → 可以直接写出一个临时的可调用对象，通常比 std::bind 更直观
```

</details>

---

## 七、std::function

<details>
<summary>点击展开查看详情</summary>

### 1. 作用与头文件

`std::function` 用来统一包装各种可调用对象，头文件：

```cpp
#include <functional>
```

最简单的例子：

```cpp
#include <iostream>
#include <functional>

void hello()
{
    std::cout << "hello\n";
}

int main()
{
    std::function<void()> func;

    func = hello;

    func();
}
```

这里的 `std::function<void()>` 可以理解成：**一个"返回值为 `void`、没有参数"的函数包装器**，对应 `void xxx();`。

### 2. 有参数的情况

```cpp
void add(int a, int b)
{
    std::cout << a + b << '\n';
}
```

那么：

```cpp
std::function<void(int, int)> func;

func = add;

func(10, 20);   // 输出 30
```

`std::function<void(int, int)>` 表示：

- 返回值：`void`
- 参数：`int, int`

可以记成：

```text
std::function<返回值(参数列表)>
```

例如 `std::function<int(int, int)>` 表示 `int xxx(int, int);`。

### 3. 包装不同种类的可调用对象

```cpp
std::function<void()> f1 = hello;                          // 普通函数
std::function<void()> f2 = []() { std::cout << "lambda\n"; };  // Lambda
std::function<void()> f3 = Hello();                        // 函数对象
```

它们类型各不相同，但都被统一成了 `std::function<void()>`，因此可以放进同一个容器，比如任务队列：

```cpp
std::queue<std::function<void()>> taskQueue;
```

> 补充：`std::function` 为了实现类型统一，内部使用了类型擦除，调用时有少量额外开销；对象为空时调用会抛出 `std::bad_function_call`，可以用 `if (func)` 判断是否持有可调用对象。

</details>

---

## 八、std::bind

<details>
<summary>点击展开查看详情</summary>

### 1. 作用

可以简单理解成：**把一个函数和它需要的部分参数提前绑定起来，生成一个新的可调用对象。**

例如：

```cpp
void add(int a, int b)
{
    std::cout << a + b << '\n';
}
```

正常调用是 `add(10, 20);`。现在我希望：**把 `a` 固定为 10，只留下 `b`。**

可以这样写：

```cpp
auto f = std::bind(add, 10, std::placeholders::_1);
```

现在：

```cpp
f(20);
```

实际上相当于 `add(10, 20);`。

### 2. 占位符

```text
std::placeholders::_n  =  调用时传入的第 n 个参数
```

例如：

```cpp
auto f = std::bind(add, std::placeholders::_2, std::placeholders::_1);
f(1, 2);   // 相当于 add(2, 1)，可以用占位符调换参数顺序
```

### 3. 绑定成员函数

成员函数需要对象才能调用，所以 `bind` 时要把对象（或对象指针）作为第二个参数传入：

```cpp
class Worker {
public:
    void work(int x) { std::cout << "work " << x << '\n'; }
};

Worker w;
auto f = std::bind(&Worker::work, &w, std::placeholders::_1);
f(5);   // 相当于 w.work(5)
```

在类内部绑定自己的成员函数时，对象传 `this`：

```cpp
std::bind(&ThreadPool::threadFunc, this);
```

### 4. 注意事项

- `std::bind` 默认**按值拷贝**绑定的参数；如果希望按引用绑定，需要使用 `std::ref(x)`。
- 同样的功能也可以用 Lambda 实现，现代 C++ 中通常更推荐 Lambda（更直观，也更易于编译器优化）：

```cpp
auto f = [](int b) { add(10, b); };   // 与 std::bind(add, 10, _1) 等价
```

### 5. std::function 与 std::bind 的分工

| 工具 | 作用 |
| --- | --- |
| `std::function` | 统一包装可调用对象 |
| `std::bind` | 提前绑定函数和参数，生成新的可调用对象 |

两者经常配合使用：用 `bind` 生成一个参数已就位的可调用对象，再交给 `std::function` 保存起来。

</details>

---

## 九、std::unique_ptr（含 std::move、std::make_unique）

<details>
<summary>点击展开查看详情</summary>

### 1. unique_ptr 用法

`std::unique_ptr` 持有对资源的**独占所有权**——同一时间只能有一个 `unique_ptr` 指向该资源。它**不可复制（no copy），但可以移动（movable）**。

核心特性：

- **自动释放**：当 `unique_ptr` 超出其作用域时，会自动调用 `delete` 释放内存，无需手动写 `delete`。
- **禁止拷贝**：`std::unique_ptr<Widget> p4 = p3;` 会编译报错。
- **所有权转移**：使用 `std::move` 将所有权从一个指针转移给另一个。
- **数组支持**：支持管理动态数组。

```cpp
std::unique_ptr<Widget> p3(new Widget());
// std::unique_ptr<Widget> p4 = p3;            // 编译错误：不能拷贝
std::unique_ptr<Widget> p4 = std::move(p3);    // 正确：所有权转移，p3 变为 nullptr

std::unique_ptr<int[]> arr(new int[10]);       // 管理动态数组，销毁时使用 delete[]
```

### 2. 为什么禁用左值拷贝、支持右值移动

`std::unique_ptr` 的核心设计目标是：**严格独占它所指向的动态内存（或资源）**。任何时候，只能有一个指针实例负责这块内存的生命周期。当这个指针实例被销毁（如离开作用域）时，它会自动调用 `delete` 释放内存。

**假设允许左值拷贝的后果：**

```cpp
{
    std::unique_ptr<int> ptr1(new int(42));
    std::unique_ptr<int> ptr2 = ptr1;   // 假设允许左值拷贝（编译会通过）
}
```

1. **双重释放（Double Free）**：`ptr1` 和 `ptr2` 指向同一块内存。当它们两个离开作用域被销毁时，都会尝试对该内存调用 `delete`。第二次 `delete` 会直接导致程序崩溃（Segment Fault / 内存损坏）。
2. **所有权模糊**：如果修改 `ptr1` 指向的值，`ptr2` 也会受到影响，这违背了"指针独立管理资源"的初衷，退化成了普通的裸指针，失去了智能指针防泄漏的安全保证。

### 3. 左值 vs 右值的语义差异

- **左值（Lvalue）**：持久的对象，有名字，有明确的内存地址（比如你定义的普通变量 `ptr1`）。你无法确定别人后续还会不会使用它，所以不能在用户不知道的情况下悄悄毁掉它。
- **右值（Rvalue）**：临时的、即将被销毁的对象，通常没有名字（比如函数的临时返回值 `std::unique_ptr<int>(new int(5))`）。既然它马上就要死了，把它内部的资源"偷"过来据为己有是完全安全的。

### 4. 右值移动构造的哲学

`unique_ptr` 提供右值移动构造，允许我们做"所有权转移"。它的底层逻辑极其简单且高效：**资源不复制，只复制指针地址，并将原指针清空。**

```cpp
// unique_ptr 内部的移动构造函数伪代码
unique_ptr(unique_ptr&& other) noexcept {
    this->ptr_ = other.ptr_;   // 1. 接管右值对象的内存地址
    other.ptr_ = nullptr;      // 2. 将原右值对象清空！使其失去所有权
}
```

### 5. std::move

- `std::move` 的本质是一个**强制类型转换**。
- 它把一个左值（`unique_ptr&`）强制转换为了右值引用（`unique_ptr&&`），从而让编译器选择"移动构造 / 移动赋值"。

> 补充：`std::move` 本身**不会移动任何东西**，真正转移资源的是后续被调用的移动构造函数 / 移动赋值运算符。被移动之后，原对象处于"有效但内容不再可依赖"的状态，对 `unique_ptr` 来说就是变成了空指针，不应再解引用它。

### 6. std::make_unique 用法

`std::make_unique` 是 **C++14** 引入的模板辅助函数，用来创建并返回一个 `std::unique_ptr`。

```cpp
auto p = std::make_unique<Widget>(arg1, arg2);   // 等价于 unique_ptr<Widget>(new Widget(arg1, arg2))
auto q = std::make_unique<int[]>(10);            // 数组版本
```

**为什么推荐使用 `make_unique`？**

1. **更安全（防止内存泄漏）**：如果手动写 `f(std::unique_ptr<T>(new T()), g())`，在某些复杂的函数参数求值顺序下，若 `new T()` 成功后 `g()` 抛出异常，可能导致内存泄漏。`std::make_unique` 将内存分配和构造封装为一步，更加安全。
   （注：C++17 起规定了不同函数参数的求值不会交错，这个问题在 C++17 之后已基本不存在，但在 C++11/14 中仍需注意。）
2. **代码更简洁**：不需要显式写 `new` 和 `delete`，配合 `auto` 关键字可以避免写出重复的类型名：

```cpp
// 繁琐
std::unique_ptr<MyVeryLongClassName> ptr(new MyVeryLongClassName(arg));
// 简洁
auto ptr = std::make_unique<MyVeryLongClassName>(arg);
```

### 7. 总结

- **禁用左值拷贝**：为了防止多个指针指向同一块内存，引发双重释放和所有权混乱。
- **提供右值移动**：允许临时对象或即将销毁的对象进行高效的所有权交接，只需复制指针并清空原指针，零开销且安全。
- **必须用 `std::move`**：将左值强转为右值，作为一种显式的代码契约，提醒程序员该左值在移动后已失效（变为空指针），防止误用。

</details>

---

## 十、std::condition_variable：wait / wait_for / wait_until

<details>
<summary>点击展开查看详情</summary>

`std::condition_variable` 提供了 **`wait`、`wait_for` 和 `wait_until`** 三个函数，用于实现线程间的同步与等待。它们都会**原子性地释放互斥锁并阻塞当前线程**，被唤醒后会重新获取锁；区别在于等待时间的限制和返回值。

| 函数名 | 等待时长类型 | 返回值 | 典型用途 |
| --- | --- | --- | --- |
| `wait` | 无限制（无限期等待） | `void` | 必须被 `notify_one()` / `notify_all()` 唤醒 |
| `wait_for` | 相对时间（如：等待 50 毫秒） | `std::cv_status`（`no_timeout` 或 `timeout`） | 在指定时间段内等待，超时则自动返回 |
| `wait_until` | 绝对时间点（如：等到 2026 年 10 月 10 日 12:30） | `std::cv_status`（`no_timeout` 或 `timeout`） | 等待至某个具体的时间点，超时则自动返回 |

### 补充

- 三个函数都需要传入 `std::unique_lock<std::mutex>`（调用前必须已加锁）。
- 它们都有**带谓词（条件）的重载版本**，例如 `cv.wait(lock, [] { return !queue.empty(); });`，等价于：

  ```cpp
  while (!pred()) {
      cv.wait(lock);
  }
  ```

  这样可以应对**虚假唤醒**（线程在没有被 notify 的情况下被唤醒），因此实际使用时推荐总是带条件判断。
- 带谓词的 `wait_for` / `wait_until` 返回的是 `bool`（谓词最终的结果），而不是 `cv_status`。

```cpp
std::unique_lock<std::mutex> lock(mtx);

// 无限等待，直到队列非空
cv.wait(lock, [&] { return !taskQueue.empty(); });

// 最多等 50 毫秒
if (cv.wait_for(lock, std::chrono::milliseconds(50)) == std::cv_status::timeout) {
    // 超时处理
}
```

</details>

---

## 十一、C++17 std::any 类型介绍

<details>
<summary>点击展开查看详情</summary>

C++17 引入的 `std::any` 是一个**类型安全的通用容器**，可以用来存储任意**可复制构造**（CopyConstructible）的单个类型的值。

```cpp
#include <any>

std::any a = 10;                    // 存 int
a = std::string("hello");           // 改存 string
std::cout << std::any_cast<std::string>(a) << '\n';
```

### 主要特点与优势

- **类型安全（Type-safe）**：与传统的 `void*` 不同，`std::any` 知道自己内部存储的具体类型，不会发生隐式的、不安全的类型转换（类型不匹配时 `any_cast` 会抛出 `std::bad_any_cast`）。
- **自动内存管理**：不需要手动 `new` 或 `delete`，内部会自动处理内存分配和释放（对于较小对象通常使用 SSO 小对象优化，较大对象则在堆上分配）。
- **可为空状态（Empty state）**：可以不存任何值，处于"空"状态（可用 `has_value()` 判断）。

### 使用注意事项与局限性

1. **性能开销**：由于涉及类型擦除（Type Erasure）和可能的堆内存分配，频繁地创建、拷贝或转换 `std::any` 会带来一定的性能损耗，应避免在高性能核心逻辑中滥用。
2. **无比较操作**：`std::any` 本身没有重载 `==` 等比较运算符，无法直接进行大小或相等性判断。
3. **空指针查询**：也可以使用指针形式的 `std::any_cast<T>(&a)`，转换失败时会返回 `nullptr` 而不是抛出异常，这在某些场景下更安全。

```cpp
if (int* p = std::any_cast<int>(&a)) {
    std::cout << *p << '\n';        // 类型匹配
} else {
    std::cout << "not an int\n";    // 类型不匹配，不抛异常
}
```

</details>

---

## 十二、利用 C++ 基础知识，怎样构建一个 Any 类型？

<details>
<summary>点击展开查看详情</summary>

### 1. 设计思路

要实现"一个类型可以接收任意其他类型的数据"，需要解决两个问题：

```text
任意的其它类型  ──?──►  template（模板，让构造函数可以接收任意类型）
让一个类型指向其它任意的类型  ──►  基类指针指向派生类对象
```

即：**基类类型 vs 派生类类型** —— `Any` 内部持有一个 `Base*`（智能指针），而真正存放数据的是派生类模板 `Derive<T>`，利用**多态**（见第一节）把不同类型的数据统一到同一个基类指针之下。

```text
Any ──► Base*   ◄── Derive<T> : public Base
                     └── T data_   （真正存放用户数据）
```

### 2. 实现代码

```cpp
// Any类型: 可以接收任意数据的类型
class Any
{
public:
    Any() = default;
    ~Any() = default;
    Any(const Any&) = delete;
    Any& operator=(const Any&) = delete;
    Any(Any&&) = default;
    Any& operator=(Any&&) = default;

    // 接收Any任意的其它数据类型
    template<typename T>
    Any(T data) : base_(std::make_unique<Derive<T>>(data))
    {}

    // 提取出Any存储的data数据类型
    template<typename T>
    T cast_()
    {
        // 如何从base_找到指向它的Derive对象
        // 基类指针 -> 派生类指针   RTTI
        Derive<T> *pd = dynamic_cast<Derive<T>*>(base_.get());
        if (pd == nullptr)
        {
            throw "type is unmatch!";
        }
        return pd->data_;
    }

private:
    // 基类类型
    class Base
    {
    public:
        virtual ~Base() = default;
    };

    // 派生类类型
    template<typename T>
    class Derive : public Base
    {
    public:
        Derive(T data) : data_(data)
        {}
        T data_;
    };

private:
    // 定义一个基类的指针
    std::unique_ptr<Base> base_;
};
```

### 3. 用法

```cpp
Any a = 42;
int x = a.cast_<int>();                     // 正确

try {
    std::string s = a.cast_<std::string>(); // 类型不匹配，抛出异常
} catch (const char* msg) {                 // 抛出的是字符串字面量，所以按 const char* 捕获
    std::cout << msg << '\n';               // type is unmatch!
}
```

### 4. 代码讲解

**① 整体结构：一个"壳"，三层嵌套**

```text
Any
 └── unique_ptr<Base> base_ ───► Derive<int>   （Any a = 42 时）
                                  ├── Base 部分（只有虚表指针）
                                  └── int data_ = 42
```

- `Any` 本身不是模板，所以任何地方都可以用同一个类型 `Any` 去定义变量、放进容器、作为函数返回值。
- 真正的数据放在模板类 `Derive<T>` 里，而 `Derive<T>` 继承自非模板的 `Base`，因此 `Any` 只需要持有一个 `Base` 指针就能指向**任意** `Derive<T>`——这就是第一节讲的"基类指针指向派生类对象"。
- `Base` 和 `Derive` 都放在 `private` 里，是 `Any` 的实现细节，外部用不到也看不到。

**② 模板构造函数：存数据**

```cpp
template<typename T>
Any(T data) : base_(std::make_unique<Derive<T>>(data)) {}
```

执行 `Any a = 42;` 时：

1. 编译器根据实参推导出 `T = int`；
2. 实例化出 `Derive<int>`，并用 `data` 构造出一个堆上的 `Derive<int>` 对象；
3. `make_unique` 返回 `unique_ptr<Derive<int>>`，它被隐式转换为 `unique_ptr<Base>` 并交给 `base_`（派生类指针到基类指针的转换）。

模板在**编译期**为用到的每种 `T` 各生成一份 `Derive<T>`，而"保存的是哪一种 `T`"这件事，在**运行期**由对象自己的虚表来记住。

> `Derive` 是在这个构造函数之后才定义的，但类内成员函数体和成员初始化列表是在整个类定义完之后才编译的，所以这里可以放心使用。

**③ cast_：取数据**

```cpp
Derive<T>* pd = dynamic_cast<Derive<T>*>(base_.get());
```

- `base_.get()` 拿到的是 `Base*`，调用者通过模板参数 `T` 告诉 `Any` "我认为里面存的是 `T`"。
- `dynamic_cast` 借助 **RTTI**（运行时类型信息）检查：`base_` 实际指向的对象，是不是 `Derive<T>`？
  - 是 → 返回有效的 `Derive<T>*`，通过 `pd->data_` 取出数据；
  - 不是 → 返回 `nullptr`，于是抛出异常。
- `dynamic_cast` 要求 `Base` 是**多态类型**（至少有一个虚函数），这里正是靠虚析构函数满足的。

**④ 为什么要写这几个特殊成员函数**

| 写法 | 原因 |
| --- | --- |
| `Any(const Any&) = delete;` `operator=(const Any&) = delete;` | 成员 `unique_ptr` 本来就不可拷贝，显式 `delete` 让意图清晰、报错信息直观 |
| `Any(Any&&) = default;` `operator=(Any&&) = default;` | `unique_ptr` 可以移动，所以 `Any` 可以移动，例如作为函数返回值传出去 |
| `Any() = default;` | 因为定义了模板构造函数，需要显式要求编译器生成无参构造，得到"空的 `Any`" |
| `~Any() = default;` | `unique_ptr` 会自动释放 `Derive<T>`，无需手写析构 |
| `virtual ~Base() = default;` | 通过 `Base*` 销毁 `Derive<T>` 时，必须调用到 `Derive<T>` 的析构（见第十三节） |

**⑤ 使用时需要注意的细节**

- **类型必须完全一致**：`Any a = 42;` 存的是 `int`，用 `cast_<long>()` 或 `cast_<double>()` 都会失败，因为 `Derive<long>` 和 `Derive<int>` 是两个不同的类型。
- **字符串字面量**：`Any a = "abc";` 推导出的 `T` 是 `const char*`，取出时也要写 `cast_<const char*>()`，写 `cast_<std::string>()` 会失败。
- **拷贝开销**：`data` 按值传入并拷贝进 `Derive`，对 `std::string` 这类较大的对象会有多次拷贝。可以改成 `std::make_unique<Derive<T>>(std::move(data))` 和 `data_(std::move(data))` 来减少拷贝。
- **异常类型**：`throw "type is unmatch!";` 抛出的是 `const char*`，捕获时要对应；更规范的做法是抛出 `std::runtime_error` 或 `std::bad_cast`，这样可以统一用 `std::exception` 捕获。
- `cast_` 返回的是 `data_` 的**拷贝**，修改返回值不会影响 `Any` 内部保存的数据。

### 5. 关键点回顾

- **模板构造函数**：解决"接收任意类型"。
- **基类指针指向派生类对象**：解决"用同一个类型保存不同类型"，依赖多态。
- **`dynamic_cast`**：取值时把基类指针安全地转回具体的 `Derive<T>*`，类型不匹配返回 `nullptr`。
- **`unique_ptr` 持有数据**：自动释放；因此 `Any` 不可拷贝，只可移动。
- **基类虚析构**：保证通过基类指针销毁时，派生类（及其保存的 `T`）能被正确析构。

</details>

---

## 十三、什么时候析构函数需要是虚函数？

<details>
<summary>点击展开查看详情</summary>

### 1. 一句话结论

通过**基类指针**（或引用）去释放（`delete`）**派生类对象**时。

### 2. 具体发生条件

**① 多态删除（最常见的情况）：**

- 你定义了一个基类指针，但它实际指向的是通过 `new` 创建的派生类对象。
- 当你对这个基类指针执行 `delete` 操作时。

**② 派生类有动态资源需要释放：**

- 如果派生类分配了内存、文件句柄或网络连接等需要析构时手动释放的资源。
- 如果基类析构函数**不是虚函数**，编译器在编译时只会静态绑定到基类的析构函数，导致只会调用基类的析构函数，派生类的析构函数不会被执行，从而引发派生类资源的内存泄漏。（严格来说，这种删除在标准中属于未定义行为。）
- 如果基类析构函数声明为 `virtual`，则会触发动态绑定，**先调用派生类的析构函数，再自动回溯调用基类的析构函数**，确保资源完全释放。

```cpp
class Base
{
public:
    virtual ~Base() { std::cout << "~Base\n"; }   // 去掉 virtual 试试
};

class Derive : public Base
{
public:
    ~Derive() { std::cout << "~Derive\n"; }
};

Base* p = new Derive();
delete p;
// 有 virtual：输出 ~Derive、~Base
// 无 virtual：只输出 ~Base（派生类部分没有被析构）
```

### 3. 不需要声明为虚析构函数的情况

- **类不作为基类**：该类不会被其他类继承。
- **不通过基类指针管理对象**：虽然有继承关系，但代码中所有的对象都是直接创建和销毁的（即对象类型明确，不通过 `Base* ptr = new Derived()` 的方式操作和删除）。

> 补充：如果对象由 `std::shared_ptr<Base>` 管理，并且是通过 `make_shared<Derived>()` / `shared_ptr<Base>(new Derived)` 创建的，删除器在创建时就已经记录了真实类型，即使基类析构函数不是虚函数也能正确析构；但 `unique_ptr<Base>` 就不行。为了避免这种差异带来的隐患，直接把基类析构函数写成虚函数更稳妥。

### 4. 实践建议

作为**接口类 / 抽象类**：如果一个类本来就是作为基类，且含有其他虚函数，通常**无条件建议把它的析构函数也声明为 `virtual`**，以防未来有人通过基类指针去删除派生类对象。

</details>

---

## 十四、C++11 的 = default（显式默认函数）

<details>
<summary>点击展开查看详情</summary>

### 1. 作用

`= default` 用于**特殊成员函数**（默认构造函数、析构函数、拷贝构造函数、拷贝赋值运算符、移动构造函数、移动赋值运算符等）。

- **作用**：显式要求编译器生成该函数的默认实现。
- **背景**：一旦用户定义了任何有参构造函数，编译器就不会再自动生成无参的默认构造函数。如果此时还想使用编译器默认的行为，以前需要手写空函数体，现在可以直接使用 `= default`。

```cpp
class Widget
{
public:
    Widget(int x) : x_(x) {}     // 定义了有参构造，编译器不再生成默认构造
    Widget() = default;          // 显式要求编译器生成默认构造
    virtual ~Widget() = default; // 虚析构函数也可以这样写（见第十三节）
private:
    int x_ = 0;
};
```

### 2. 优点

1. **代码更清晰**：省去手写空函数体的麻烦。
2. **性能更好**：由编译器生成的默认函数通常比用户自定义的空函数体执行效率更高（编译器可以做更多优化，例如保持类型为 trivial）。
3. **语义明确**：向阅读代码的人传达"这里使用的是标准、默认的类初始化 / 清理逻辑"的意图。

### 3. 与 = delete 的关系

| 写法 | 含义 |
| --- | --- |
| `= default` | 要求编译器生成默认实现 |
| `= delete` | 显式禁用该函数（见第四节） |

两者都是 C++11 引入的"显式控制特殊成员函数"的语法，常常成对出现在同一个类中（例如 `Any` 里既有 `= default` 也有 `= delete`）。

</details>
